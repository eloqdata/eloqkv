# Vector Search (EloqVec)

EloqKV ships an optional vector-search module ("EloqVec") that adds eight `ELOQVEC.*` commands for creating HNSW indexes, inserting/updating/deleting vectors with typed metadata, and running (optionally filtered) k-NN searches. The ANN structure itself is **not** a `TxObject` and lives outside the engine's cc maps: each node keeps a process-local in-memory [usearch](https://github.com/unum-cloud/usearch) index, while everything that must be durable — index metadata and a sharded delta log of every mutation — is stored as ordinary records in an engine-internal hash table (`__vector_index_meta_table`) via normal transactions, so it inherits the engine's WAL/checkpoint durability (`data_substrate/docs/07-durability-and-recovery.md`). Snapshots of the in-memory index are saved to a local file and optionally uploaded to object storage through an `rclone rcd` sidecar; recovery on any node is lazy: download/load the last snapshot, then replay the delta log.

Compile-time gated: `VECTOR_INDEX_ENABLED` (default `OFF`, `CMakeLists.txt:145`) — when on, usearch v2.21.0 and nlohmann/json are fetched at build time (`CMakeLists.txt:302-326`) and `src/vector/*.cpp` is compiled into the server (`CMakeLists.txt:383-394`).

## Key files

| File | Role |
|---|---|
| `include/vector/vector_handler.h`, `src/vector/vector_handler.cpp` | `VectorHandler` singleton: all index lifecycle + data ops, index cache, persistence |
| `include/vector/vector_index.h`, `hnsw_vector_index.h/.cpp` | abstract index API and the usearch-backed HNSW implementation |
| `include/vector/log_object.h`, `src/vector/log_object.cpp` | sharded delta log stored as engine records |
| `include/vector/vector_type.h`, `src/vector/vector_type.cpp` | `IndexConfig`, `VectorIndexMetadata`, `VectorRecordMetadata` (schema), `VectorId`, serialization |
| `include/vector/predicate.h`, `src/vector/predicate.cpp` | JSON filter → predicate tree, evaluated against per-vector metadata |
| `include/vector/cloud_manager.h`, `src/vector/cloud_manager.cpp` | object-storage upload/download via a spawned `rclone rcd` process |
| `include/vector/vector_util.h` | memcmp-comparable binary encoding of metadata values |
| `src/redis_command.cpp:20520-21356`, `src/redis_service.cpp:5539-5861` | command parsing and execution glue |

## 1. Command surface

Commands are registered in `src/redis_service.cpp:1896-1928` and parsed in `src/redis_command.cpp` (`RedisCommandType::ELOQVEC_*`, `src/redis_command.cpp:274-281`):

| Command | Syntax | Notes |
|---|---|---|
| `ELOQVEC.CREATE` | `index config_json [schema_json]` | `config_json` fields are **order-sensitive** (parsed with `nlohmann::ordered_json`): `dimension` (uint > 0), `metric` (`L2SQ`/`L2`, `IP`, `COSINE`), `algorithm` (`HNSW` only), `persist_strategy` (`EVERY_N` requires `threshold` > 0; `MANUAL` → threshold −1), then optional HNSW params `m`, `ef_construction`, `ef_search` (`src/redis_command.cpp:20520-20767`, allowed-key check `src/vector/hnsw_vector_index.cpp:138-161`). `schema_json` is an ordered object defining the metadata schema; each field maps to a type from `INT32 / INT64 / DOUBLE / BOOL / STRING`. |
| `ELOQVEC.ADD` | `index key vector ["metadata_json"]` | `key` is a uint64; `vector` is whitespace-separated floats; `metadata_json` is a **JSON array** of values in schema order, not an object (`VectorRecordMetadata::Encode`, `src/vector/vector_type.cpp:199-234`). |
| `ELOQVEC.BADD` | `index key_count key1 vec1 [meta1] key2 vec2 [meta2] …` | all-with-meta or all-without; max 10000 entries (`MAX_BATCH_ADD_SIZE`, `include/redis_command.h:7092`). |
| `ELOQVEC.UPDATE` | `index key vector ["metadata_json"]` | fails if `key` is not in the index. |
| `ELOQVEC.DELETE` | `index key` | fails if `key` is not in the index. |
| `ELOQVEC.SEARCH` | `index k vector ["filter_json"]` | replies array of `[id, distance]` pairs (`src/redis_command.cpp:21331-21355`); no hydration of vectors or any Redis keys. |
| `ELOQVEC.INFO` | `index` | dumps config/params/timestamps; `status` is hard-coded `"ready"` (`src/redis_command.cpp:20838-20839`). |
| `ELOQVEC.DROP` | `index` | removes metadata, log shards, cache entry, local + cloud snapshot files. |

Filter JSON (Mongo-style, `src/vector/predicate.cpp:101-347`): leaf operators `$eq $ne $gt $gte $lt $lte $in`, combinators `$and $or $not`, implicit AND for multiple fields/ops at one level. Fields must exist in the schema except the pseudo-field `id` (compared as Int64 against `VectorId::id_`, `predicate.cpp:212-215, 440-443`). All comparisons are `memcmp` over the order-preserving binary encoding produced at write time (`include/vector/vector_util.h:105-195` — sign-flipped big-endian ints, MySQL-style sortable doubles).

Vector IDs are bare uint64s chosen by the client; there is **no linkage to Redis keys, databases, or namespaces** — the module is a self-contained store keyed by `(index_name, id)`, global to the cluster (not db- or namespace-scoped, cf. [05-namespaces.md](05-namespaces.md)).

## 2. Architecture

Three kinds of state per index named `N`:

1. **Metadata record** — key `vector_index:N:metadata` in the engine-internal table `__vector_index_meta_table` (`TableType::Primary`, `TableEngine::InternalHash`, hash-partitioned; `include/vector/vector_type.h:36-42`). Serialized `VectorIndexMetadata`: name, `IndexConfig`, schema, persist threshold, current snapshot `file_path`, created/last-persist timestamps (`src/vector/vector_type.cpp:393-459`).
2. **Sharded delta log** — 1024 shards (`VECTOR_INDEX_LOG_SHARD_COUNT`, `src/vector/vector_handler.cpp:50`). Each shard is a metadata record `log:meta:vector_index:N:shard_<i>` plus one record per entry `log:item:vector_index:N:shard_<i>:<seq>` in the *same* internal table (`src/vector/log_object.cpp:180-220`). A log item is `{INSERT|UPDATE|DELETE, serialized VectorId (id + encoded metadata), serialized float vector, ts, seq}`. Shard chosen by FNV-1a of the decimal id (`log_object.cpp:193-214`).
3. **In-memory usearch index + snapshot file** — `HNSWVectorIndex` wraps `index_dense_gt<VectorId>`; the usearch key type is the whole `VectorId`, so the encoded metadata rides inside the ANN structure itself, while hashing/equality use only `id_` (`src/vector/hnsw_vector_index.cpp:707-763`). Snapshot file: `<data_path>/N-<ts>.index`, with sentinel timestamp `0000000000000000` meaning "never persisted" (`vector_type.cpp:359-381`, `vector_type.h:44`).

(1) and (2) are replicated/durable engine state shared by the whole cluster; (3) is **per-process**, held in `VectorHandler::vec_indexes_` (name → `{shared_ptr<VectorIndex>, shared_ptr<VectorIndexMetadata>}`) under a `std::shared_mutex` (`include/vector/vector_handler.h:277-281`).

### Threading model

- A dedicated `TxWorkerPool` named `"vindex"` (plain `std::thread`s, default 1 thread, `--vector_index_worker_num`, `src/redis_service.cpp:109-112, 495-506`) executes every vector command: the brpc bthread submits a closure and blocks on a `bthread::ConditionVariable` until it finishes (`src/redis_service.cpp:5550-5575` and siblings). So vector work never runs on TxProcessor threads or brpc workers (cf. `data_substrate/docs/02-threading-model.md`); the worker threads drive their own `TransactionExecution` via `NewTxInit`/`Execute`/`Wait`.
- The worker's `thread_id` is forwarded to usearch as the search-context slot (`vector_handler.cpp:448`, `hnsw_vector_index.cpp:313-337`).
- Inside `HNSWVectorIndex`, add/remove/update/get/search take only a **shared** lock — concurrent mutation safety is delegated to usearch's internal synchronization; the exclusive lock is reserved for `initialize`, `save`, and parameter changes, so a snapshot save blocks all index ops (`hnsw_vector_index.cpp:35, 93, 295, 380, …`).
- Async persistence runs on the same pool via `SubmitWork` (`vector_handler.cpp:552-554`).

The handler singleton is created in `RedisServiceImpl::Start` once the tx service exists, with `data_path` from the engine core config and an optional `CloudConfig` (`vector_cloud_endpoint`/`vector_cloud_base_path` flags or `[store]` ini keys, `src/vector/vector_type.cpp:31-34, 465-475`; wiring `src/redis_service.cpp:743-760`). If a cloud endpoint is configured but `rclone` can't be started/connected, server startup fails.

## 3. Write path (ADD; UPDATE/DELETE/BADD analogous)

`VectorHandler::Add` (`src/vector/vector_handler.cpp:454-563`), all inside **one engine transaction** (`NewTxInit(RepeatableRead, OCC)`):

1. Read `vector_index:N:metadata` (plain read) — missing → `INDEX_NOT_EXIST`.
2. `GetOrCreateIndex`: return cached entry, or lazily build it (§5).
3. Encode metadata JSON against the schema (array, exact arity) → binary blob inside `VectorId`.
4. `LogObject::append_log_sharded`: reads the shard's `log:meta:…` **for write** (write intent; `is_for_write=true`, `log_object.cpp:424-437`, ctor `data_substrate/tx_service/include/tx_request.h:217-230`), assigns the next sequence id, upserts one `log:item:…` record per entry, updates shard meta. The write intent on the shard meta serializes all writers of that shard (`log_object.cpp:463-465`).
5. Mutate the in-memory usearch index (**before commit**).
6. `CommitTx`. On commit failure, compensate the in-memory index (remove the just-added id; UPDATE restores the previous vector; DELETE re-adds it) (`vector_handler.cpp:529-535, 651-657, 760-766`).
7. If `persist_threshold != −1` and `log_count_of_this_shard × 1024 ≥ threshold` and no persist is already pending for this index, enqueue `PersistIndex(name)` on the worker pool (`vector_handler.cpp:543-556`).

`BatchAdd` groups entries by shard id (sorted `std::map` traversal to keep shard-lock acquisition ordered and deadlock-free, `vector_handler.cpp:866-880`) and appends per shard within the same transaction.

## 4. Query path (SEARCH)

`VectorHandler::Search` (`vector_handler.cpp:382-452`): meta read → `GetOrCreateIndex` → if `filter_json` present, `PredicateExpression::Parse` against the schema (parse failure → `METADATA_OP_FAILED`, including filters on schema-less indexes) → `HNSWVectorIndex::search`. With a filter, usearch's `filtered_search` invokes the predicate callback **during graph traversal** (pre-filtering, not post-filtering): the callback decodes field offsets from the metadata blob embedded in the candidate's `VectorId` and evaluates the tree by memcmp (`vector_handler.cpp:436-444`, `hnsw_vector_index.cpp:312-330`, `predicate.cpp:378-471`). Results are ids + distances only; the `exact` brute-force flag exists in the API but the handler always passes `false` (`vector_handler.cpp:448-449`). The wrapping transaction does no data reads beyond the meta record (plus log replay if the index was lazily initialized) and is committed at the end.

## 5. Durability, recovery, persistence

**Durability of every mutation** comes from step 4 of the write path: log items are ordinary records in an engine table, so they hit the engine WAL at commit and checkpoint to the kv store like any write (`data_substrate/docs/07-…`, `10-log-service.md`). The usearch structure is only a serving cache of (snapshot ⊕ log).

**Lazy rebuild** — `CreateAndInitializeIndex` (`vector_handler.cpp:1016-1089`), triggered the first time any node touches an index after restart/failover/drop-from-cache:

1. Deserialize metadata from the record just read.
2. If `file_path` does not contain the initial-timestamp sentinel (i.e., a snapshot exists): with a cloud manager, delete any local copy and download `file_path` minus the `data_path` prefix from object storage; without one, require the local file (`vector_handler.cpp:1043-1063`) — so in cloud-less multi-node setups, a node that didn't write the snapshot cannot rebuild (`INDEX_INIT_FAILED`).
3. `initialize` = create usearch index from config + load snapshot file if present (`hnsw_vector_index.cpp:32-89`).
4. Replay the full sharded log via `ApplyLogItems` (scan all 1024 shards, batch consecutive INSERTs, apply UPDATE/DELETE as barriers) (`vector_handler.cpp:1260-1375`).

**Snapshotting** — `PersistIndex` (`vector_handler.cpp:1091-1258`), its own transaction:

1. Read metadata **for write** (write intent on the meta record).
2. `truncate_all_sharded_logs`: per shard, read shard meta for write (blocking all concurrent writers of the index for the duration), delete every item record, reset counters (`log_object.cpp:669-793, 1079-1118`).
3. Save usearch index to `<data_path>/N-<new_ts>.index` (exclusive lock → searches/writes on this node stall).
4. Update metadata (`file_path`, `last_persist_ts`) via upsert; upload the new file to `<endpoint>:<base_path>/N-<new_ts>.index` through the rclone REST API (`cloud_manager.cpp:273-295`); then `CommitTx`. Any failure aborts the transaction (log truncation and meta update roll back; the in-memory index is untouched).
5. Post-commit: delete the old snapshot locally and in the cloud (best effort).

The trigger dedupe set `pending_persist_indexes_` guarantees at most one queued persist per index (`vector_handler.cpp:546-551`).

**Cloud sidecar** — `CloudManager` spawns `rclone rcd --rc-no-auth --rc-addr=127.0.0.1:15572` (logs to `/tmp/vector_cloud_service.log`) and drives it with libcurl JSON POSTs (`operations/copyfile`, `deletefile`, `mkdir`); the bucket (first path segment of `base_path`) is created at startup (`cloud_manager.cpp:119-271`). The remote object name is the snapshot path with the local `data_path` prefix stripped.

## 6. Consistency model

- **Per-command, not session-transactional.** Each `ELOQVEC.*` op opens and commits its own internal transaction; the commands do not participate in `MULTI/EXEC`, `BEGIN/COMMIT`, or Lua scripts (the handlers bypass the txm-per-connection machinery of [02-command-processing.md](02-command-processing.md) entirely).
- **Durable state is transactional; the served index is eventually consistent with it.** Log append + meta ops commit atomically, but the in-memory usearch mutation happens *before* commit with post-hoc compensation. A concurrent search on the same node can transiently observe an entry whose transaction later aborts (and briefly miss a deleted-then-restored one). There is no read-your-write guarantee across nodes: another node's cached index instance only learns about writes it didn't apply locally when it rebuilds from snapshot + log.
- **Single-writer-per-shard.** The write intent on each shard's log meta record serializes writers of that shard cluster-wide; persistence takes intents on all 1024 shards, momentarily quiescing all writes to the index.
- Every data/search op re-reads the metadata record first, so a dropped index is detected immediately on any node even if its cache entry survives.

## 7. Gotchas / verified invariants

- **Persist trigger is a coarse estimate**: per-shard count × 1024 assumes uniform shard fill (`vector_handler.cpp:545`).
- **`PersistIndex(force)` is dead**: the `force` parameter is never read in the body (`vector_handler.cpp:1091`).
- **Metadata for ADD is a positional JSON array** matching schema order — an object is rejected (`vector_type.cpp:215-219`). Filters, in contrast, are objects keyed by field name.
- **Float metadata fields are strict**: a `DOUBLE` field rejects integer JSON literals (`is_number_float()` check, `vector_util.h:150-156`); `Int32` overflow is rejected.
- **Per-entry engine round-trips.** Log append/truncate issue one `UpsertTxRequest` per item (TODO batch, `log_object.cpp:482`); DROP/persist of a 1024-shard log touches every shard meta plus every live item in one transaction.
- **usearch `update` = remove + add** with the remove error deliberately ignored (`hnsw_vector_index.cpp:557-565`), but `VectorHandler::Update/Delete` first `get()` the id and fail with the index untouched if it doesn't exist (`vector_handler.cpp:639-646, 748-755`).
- **`max_elements` is fixed at 1,000,000** (`IndexConfig` default, `vector_type.h:282`) — it is not exposed in `ELOQVEC.CREATE`'s parser, and usearch capacity is reserved up-front at that size.
- Test expectations (filter semantics, persist/reload round-trips, concurrency of add/search) live in `src/vector/tests/VectorHandler-Test.cpp`, `VectorCache_HNSW-Test.cpp`, `LogObject-Test.cpp`; the tests register `__vector_index_meta_table` as a prebuilt engine table and run with `skip_wal`/`skip_kv` (`VectorHandler-Test.cpp:75-120`).
