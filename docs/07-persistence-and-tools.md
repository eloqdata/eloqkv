# 07 — Redis-Format Persistence Interop & Offline Tools

**Summary.** EloqKV does not persist data through RDB snapshots or AOF rewrite the way Redis does — native durability is the engine's replicated WAL plus background checkpoints into the kv store (see `data_substrate/docs/07-durability-and-recovery.md`). What this layer provides instead is *interop* with the Redis on-disk/wire formats for migration and tooling: (1) online `DUMP`/`RESTORE` commands that serialize/deserialize single values in Redis RDB payload format (`src/redis_rdb_restore.cpp`, `src/redis_command.cpp`), so `redis-cli --migrate`-style key copying works in both directions; and (2) offline exporters `eloqkv_to_rdb` and `eloqkv_to_aof` (`src/tools/`) that read the checkpointed kv store directly — either an embedded RocksDB directory or, since PR #485, a rocksdb-cloud snapshot in S3/GCS — and emit a Redis-loadable `dump.rdb` or AOF command stream. `SAVE`/`BGSAVE`/`BGREWRITEAOF` are not implemented at all; they return `ERR unknown command`.

Related docs: [03-data-model.md](03-data-model.md) (the `RedisEloqObject` types and their native `Serialize()` format these tools convert from/to), [05-namespaces.md](05-namespaces.md), engine side `data_substrate/docs/07-durability-and-recovery.md` and `09-store-handler.md`.

## 1. Positioning

| Mechanism | Online? | Purpose | Source of truth |
|---|---|---|---|
| Engine WAL + checkpoint | yes | actual durability/recovery | `data_substrate` (TxLog + Checkpointer) |
| `DUMP` / `RESTORE` | yes | per-key migration to/from real Redis | `src/redis_command.cpp:15944`, `src/redis_rdb_restore.cpp` |
| `eloqkv_to_rdb` | no (offline) | full-store export to a Redis `dump.rdb` | `src/tools/eloqkv2rdb/eloqkv2rdb.cpp` |
| `eloqkv_to_aof` | no (offline) | full-store export to RESP command files | `src/tools/eloqkv2aof/eloqkv2aof.cpp` |

There is no RDB *loader* for whole files: imports into EloqKV go through `RESTORE` (or replaying an AOF stream of normal commands).

Vendored libs used here: `crcspeed/` is the CRC-64 (Jones polynomial, same as Redis) implementation used for DUMP payload and RDB file checksums (`CMakeLists.txt:403`); `fpconv/` is a Grisu2 double-to-shortest-string converter used by `d2string()` for score formatting (`include/redis_string_num.h:256`).

## 2. DUMP / RESTORE

### 2.1 DUMP (EloqKV → Redis payload)

`DumpCommand::ExecuteOn` (`src/redis_command.cpp:15944`) converts the live object via `ConvertEloqObjectToRedisDumpPayload` (`src/redis_rdb_restore.cpp:1424`), then appends the standard 10-byte Redis DUMP footer: 2-byte little-endian RDB version `10` (`redis_dump_version_`, `include/redis_command.h:7287`) + 8-byte CRC-64 over everything before it (`src/redis_command.cpp:15958`). Encodings emitted are deliberately plain (no listpack/ziplist/intset, no LZF):

| EloqKV type | RDB type byte emitted |
|---|---|
| String | 0 (`RDB_TYPE_STRING`) |
| List | 1 (`RDB_TYPE_LIST`, plain length-prefixed elements) |
| Set | 2 (`RDB_TYPE_SET`) |
| Hash | 4 (`RDB_TYPE_HASH`) |
| Zset | 5 (`RDB_TYPE_ZSET_2`, binary little-endian double scores) |

Any other object type makes the conversion fail and DUMP returns a syntax error (`src/redis_rdb_restore.cpp:1486`, `src/redis_command.cpp:15949-15952`). Footer version 10 corresponds to Redis 7.0; real Redis only accepts payload versions ≤ its own RDB version, so (inference) Redis ≤ 6.x will reject EloqKV DUMP output while 7.x accepts it.

### 2.2 RESTORE — version & checksum gate

`ParseRestoreCommand` (`src/redis_command.cpp:20005`) verifies the payload *at parse time* via `RestoreCommand::VerifyDumpPayload` (`src/redis_command.cpp:16176`):

- footer version `0` → legacy **EloqKV-native** payload (the object's own `Serialize()` image, produced by DUMP before Redis compatibility was added; `include/redis_command.h:7284-7286`). Checksum verified with `crc64speed_big` (initialized at server start, `src/redis_service.cpp:369`).
- footer version `4` or `10` → **Redis RDB** payload; checksum verified with `crc64` (byte-swapped on big-endian hosts).
- anything else (including version 9 from Redis 5/6 and version 11 from Redis 7.2+) → rejected: "DUMP payload version or checksum are wrong".

Redis-format payloads are then converted into the native object image by `ConvertRedisDumpPayloadToEloqPayload` (`src/redis_rdb_restore.cpp:1417`). Supported RDB object encodings:

| RDB type (byte) | Decodes to | Notes |
|---|---|---|
| STRING (0) | String | int8/16/32 and LZF-compressed strings handled (`Reader::ReadString`, `src/redis_rdb_restore.cpp:245`) |
| LIST (1), LIST_ZIPLIST (10), QUICKLIST (14), QUICKLIST2 (18) | List | quicklist2 plain + packed (listpack) node containers (`:1355-1396`) |
| SET (2), SET_INTSET (11), SET_LISTPACK (20) | Set | |
| ZSET (3, string scores incl. nan/±inf), ZSET_2 (5, binary doubles), ZSET_ZIPLIST (12), ZSET_LISTPACK (17) | Zset | |
| HASH (4), HASH_ZIPLIST (13), HASH_LISTPACK (16) | Hash | |
| Streams, modules, hash-with-field-TTL (21+), or any other type byte | **rejected** (`:1411-1412`) | |

Decode hardening: LZF output capped at 64 MiB, collection counts capped at 2^20 entries and sanity-checked against remaining bytes (`kMaxLzfDecodedLength`/`kMaxCollectionEntries`, `src/redis_rdb_restore.cpp:71-72`, `ValidateCount :140`); ziplist/listpack blobs must self-consistently terminate (back-length, 0xFF terminator, total-bytes header checked).

### 2.3 RESTORE — semantics

- `REPLACE`, `ABSTTL` supported; `IDLETIME`/`FREQ` parsed and validated but ignored (`uint64_t idle_time_sec_{0}; // unsupport`, `include/redis_command.h:7374-7375`).
- TTL argument is milliseconds; without `ABSTTL` it is added to the current clock (`src/redis_command.cpp:20083-20087`). `RestoreCommand::CommitOn` builds the object from the native image and, if a TTL was given, swaps it for the TTL-variant object via `AddTTL` (`src/redis_command.cpp:16008-16098`).
- Key-exists handling is done through the TxCommand protocol: `ProceedOnNonExistentObject()=true`, `ProceedOnExistentObject()=replace_` (`include/redis_command.h:7336-7344`); the default result is `RD_ERR_BUSY_KEY_EXIST` ("BUSYKEY") which stands when the object exists and `REPLACE` was not given (`include/redis_command.h:7383`).
- Invalid payload + no `REPLACE`: the command is still routed (so BUSYKEY can win, matching Redis error precedence) carrying `payload_valid_=false`; with `REPLACE` it errors immediately (`src/redis_command.cpp:20096-20133`).
- DUMP/RESTORE resolve tables through the connection's selected DB and namespace like any other command, so they work inside custom namespaces (inference from normal dispatch; see [05-namespaces.md](05-namespaces.md)).

## 3. eloqkv_to_rdb (offline RDB exporter)

One source file, two very different builds, selected by `WITH_DATA_STORE` (`CMakeLists.txt:473-482`, install rules `:519-528`):

| `WITH_DATA_STORE` | Tool built | Reads |
|---|---|---|
| `ROCKSDB` | `eloqkv_to_rdb`, `eloqkv_to_aof` | local embedded RocksDB dir (`--rocksdb_path`) |
| `ELOQDSS_ROCKSDB_CLOUD_S3` / `_GCS` | `eloqkv_to_rdb` only | rocksdb-cloud snapshot in S3/GCS (PR #485, commit 794c6ff) |
| anything else (incl. default `ELOQDSS_ELOQSTORE`) | none | — |

Both paths only see **checkpointed** data: writes that are committed in the WAL but not yet flushed by the engine checkpointer are *not* in the kv store and will be missing from the export.

### 3.1 Legacy local-RocksDB path (`Rocksdb2RDB`, `eloqkv2rdb.cpp:2068`)

- Opens the RocksDB directory read-write (`rocksdb::DB::Open`), so it requires the server to be stopped (RocksDB LOCK file) — run it on a copy otherwise.
- Discovers tables via the hardcoded catalog keys `data_table_{0..15}_catalog` in the default column family, reading the `kv_cf_name` wide column to find each DB's column family (`:2128-2167`). Exactly 16 databases are assumed (`const int databases = 16`, `:2129`); a store created with a different `databases` config crashes on `CHECK(status.ok())`.
- Pipeline: single reader thread batches keys (`--round_batch_size`) into a pool sized `thread_count * pre_read_ratio`; `ParseWorker` threads deserialize the stored value (layout `[deleted i8][version i64][obj_type i8][payload]`, `:734-797`), drop deleted/TTL-expired entries, and append RDB-encoded bytes to pooled buffers; a single `WriteWorker` drains buffers to the file and folds them into the running CRC (`:572-625`). `SELECT db` opcodes are written between per-DB phases (`:2210-2217`).

### 3.2 RocksDB-Cloud path (`RocksdbCloud2RDB`, `eloqkv2rdb.cpp:1690`)

- **Point-in-time and safe against a live cluster**: it opens each shard's bucket with `cookie_on_open = <snapshot name>` and an empty `new_cookie_on_open` (`:1912-1913`), i.e. it mounts a named CLOUDMANIFEST branch created by the DSS backup API (`CreateSnapshotForBackup` rolls a branch named `{backup_name}-{shard_id}-{backup_ts}`, `data_substrate/store_handler/eloq_data_store_service/rocksdb_cloud_data_store.cpp:855-871`), with `disable_cloud_file_deletion=true` and no manifest roll (`:1862-1864`). `--snapshot_name` takes one comma-separated cookie per shard and must match `--shard_num` (`:2401-2408`).
- Shard object paths follow the DSS layout `<object_path>/ds_{shard_id}` (`BuildShardObjectPath`, `:912`); shards are processed sequentially, each appending to the same output file.
- Within a shard, `--thread_count` > 1 splits the keyspace into ranges weighted by live-SST sizes (`BuildShardRangeBoundaries`, `:1469`) and scans them in parallel (`ScanShardRange`, `:1556`). Each flushed buffer re-emits its own `SELECT db` prefix, so out-of-order buffer interleaving from multiple scan threads still yields a semantically correct RDB (`:1648-1654`, `acquire_buffer :1593`).
- Key/value formats differ from the legacy path: keys are DSS-composite `{kv_table_name}/{partition_id}/{key}` (`ParseDssKey`, `:1349`); values are `[version_ts u64 (MSB = has_ttl)][ttl u64?][obj_type i8 + payload]` (`DeserializeDssValue`, `:1325`). Only tables whose kv name starts with `eloqkv_data_table_` are exported, with the DB index parsed from the suffix — FLUSHDB-renamed tables like `eloqkv_data_table_0_2026_...` still map to DB 0 (`ExtractDbNumberFromCatalogKey`, `:1432`).
- S3 specifics: optional static credentials (`--aws_access_key_id/--aws_secret_key`, otherwise instance credentials), MinIO-style endpoints via `--rocksdb_cloud_s3_endpoint_url` with path-style addressing (`:1816-1840`), tunable SST cache (`--rocksdb_cloud_sst_file_cache_size`), local scratch dir `--db_path` (default `/tmp/eloqkv_rdb_export`). A progress line per shard prints keys/bytes/rates (`ShardProgressPrinter`, `:1057`).

### 3.3 Output RDB structure (both paths)

Header `REDIS0006` (`RedisRdbUtil::ParseHeader`, `:213`) — RDB file version 6, old enough that only base types are legal — then per key: optional `0xFD` seconds-resolution expiry, type byte 0–4, length-prefixed key, plain (non-compact) value encoding; trailer `0xFF` + little-endian CRC-64 of the whole file (`:2049-2059`, `:2283-2294`). Integer-looking strings are stored with int8/16/32 special encodings (`OutputString`, `:134`); LZF compression is stubbed out (`:113-115`, `:162-174`). Zset scores are written as `std::to_string(score)` strings under RDB type 3, with single-byte nan/±inf markers (`:335-353`).

## 4. eloqkv_to_aof (offline AOF exporter)

`src/tools/eloqkv2aof/eloqkv2aof.cpp` — built only for `WITH_DATA_STORE=ROCKSDB`. Same legacy local-RocksDB discovery (16 hardcoded `data_table_N_catalog` entries, `:485-524`) and the same reader/parser pool pipeline, but each `ParseWorker` writes its **own** output file `<output_file_dir>/<thread_idx>.aof` (`:531-533`), so the result is N independent RESP command streams, each self-contained (each tracks `last_db_idx_` and emits its own `SELECT`, `:301-305`). Object → command mapping (`RedisReplyUtil::ParseEloqKV`, `:98-200`):

| Type | Commands emitted |
|---|---|
| String | one `SET key value` |
| List | one `RPUSH key elem` **per element** |
| Hash | one `HSET key field value` per pair |
| Set | one `SADD key member` per member |
| Zset | one `ZADD key score member` per member (score via `d2string`/fpconv, shortest round-trip) |
| any TTL | trailing `EXPIREAT key <ttl_ms/1000>` |

One-command-per-element makes output large: `test_result.md` records 13 GB of RocksDB exploding to a 188 GB AOF. Deleted and already-expired records are skipped, like the RDB tool.

## 5. SAVE / BGSAVE / BGREWRITEAOF / LASTSAVE

Not implemented and not stubbed: none of them appear in the `command_types` map (`src/redis_command.cpp:103+`) or the handler registry, so they fall through to `ERR unknown command` (`src/redis_service.cpp:6049-6057`). The only references are commented-out vendored Redis headers (`include/redis/server.h`). Durability is always-on engine WAL + checkpoint; backups are taken on the kv-store side (DSS snapshot/backup, `data_substrate/docs/07-durability-and-recovery.md`), not via Redis commands.

## 6. Gotchas & invariants

- **Exports are checkpoint-lagged.** Both offline tools read the kv store; un-checkpointed committed writes are absent. The cloud path is at least a consistent point-in-time snapshot (manifest branch); the legacy local path is only consistent because the server must be stopped.
- **Integer-string narrowing bug in the RDB exporter.** `RedisRdbUtil::OutputString` does `int32_t n = std::stol(s)` after a `^-?\d+$` regex match (`eloqkv2rdb.cpp:139`). Values that fit `long` but not `int32` (10–19 digit numbers, e.g. epoch-millis or phone numbers stored as strings) silently wrap and are written as a *wrong* int32 — and the third range check is always true for an `int32_t`. ≥ 20-digit numeric strings throw `std::out_of_range`, which is uncaught in worker threads (terminate). The AOF exporter is unaffected (raw bulk strings).
- **TTL precision loss on export.** RDB export writes second-resolution `0xFD` expiries from millisecond TTLs (`OutputTTL`, `eloqkv2rdb.cpp:230-238`); AOF export likewise emits `EXPIREAT` in seconds (`eloqkv2aof.cpp:195-198`). Sub-second TTL is truncated.
- **Encoding round-trips lose representation, not data.** RESTORE flattens every compact encoding (ziplist/listpack/intset/quicklist) into the regular EloqKV deque/flat_hash_map objects; DUMP never re-creates compact encodings. Values survive; memory layout and `OBJECT ENCODING` fidelity do not.
- **RESTORE version gate is exact-match.** Only footer versions 0/4/10 pass (`src/redis_command.cpp:16192,16219`); DUMP payloads from Redis 7.2+ (version 11) or 5.x/6.x (version 9) are rejected even when the inner encoding would be decodable.
- **Namespaces are invisible to the exporters.** Tables are named `eloqkv_` + logical name (`GenKvTableName`, `include/redis_service.h:574`); custom-namespace data lives in `eloqkv_ns_data_0`/`eloqkv___ns_0`, which fail the `eloqkv_data_table_` prefix filter (`eloqkv2rdb.cpp:1432-1467`) — only the default namespace's 16 DBs are exported. Online DUMP/RESTORE, by contrast, are namespace-aware.
- **Size caps on RESTORE.** Collections > 2^20 entries or LZF strings > 64 MiB inside a Redis-format payload are rejected (`src/redis_rdb_restore.cpp:71-72`) even though such keys can exist in Redis; legacy EloqKV-format (version 0) payloads bypass these decode caps entirely.
- The RDB exporters compute the file CRC with `crc64speed` after calling `crc64speed_init()` (`eloqkv2rdb.cpp:2335`); the server initializes only the big-endian table variant for legacy DUMP verification (`crc64speed_init_big()`, `src/redis_service.cpp:369`) — three CRC entry points (`crc64`, `crc64speed`, `crc64speed_big`) all implement the same Jones CRC-64.

## 7. Key files

| File | Role |
|---|---|
| `src/redis_rdb_restore.cpp`, `include/redis_rdb_restore.h` | RDB payload codec (RESTORE decode, DUMP encode) |
| `src/redis_command.cpp:15944-16249, 20005-20140` | DUMP/RESTORE command logic, footer verify, parse |
| `src/tools/eloqkv2rdb/eloqkv2rdb.cpp` | offline RDB exporter (local + rocksdb-cloud) |
| `src/tools/eloqkv2aof/eloqkv2aof.cpp` | offline AOF exporter (local RocksDB only) |
| `crcspeed/`, `fpconv/` | vendored CRC-64 / double-formatting libs |
| `CMakeLists.txt:473-528` | tool build/install wiring per `WITH_DATA_STORE` |
