# Architecture Overview

EloqKV is a Redis/Valkey-compatible distributed database. This repo implements the **Redis protocol layer**: a brpc-based RESP server (`RedisServiceImpl`) that parses commands into command objects, applies them to Redis-type data objects (`TxObject` subclasses), and drives everything through the transaction engine in the `data_substrate/` submodule (documented separately under `data_substrate/docs/`). The layer registers itself with the engine as `TableEngine::EloqKv`, supplying a `CatalogFactory` so the engine can create EloqKV's concrete keys, records, and cc maps without knowing anything about Redis.

## Component Map

| Area | Key files | Doc |
|---|---|---|
| Bootstrap / main | `src/redis_server.cpp` | this file |
| Service & dispatch | `src/redis_service.cpp` (~6.5k lines), `include/redis_service.h`, `redis_handler.*`, `redis_connection_context.*` | [02](02-command-processing.md) |
| Output / errors / stats | `redis_replier.*`, `output_handler.h`, `redis_errors.*`, `redis_stats.*` | [02](02-command-processing.md) |
| Commands | `include/redis_command.h` (~8k lines), `src/redis_command.cpp` (~21k lines) | [03](03-data-model.md) |
| Data objects | `redis_object.h`, `redis_{string,hash,list,set,zset}_object.*` | [03](03-data-model.md) |
| Engine plug-in | `eloqkv_key.*` (EloqKey = TxKey impl), `eloqkv_catalog_factory.*` | [03](03-data-model.md) |
| Lua scripting | `lua_interpreter.*`, `lua_output_handler.h`, vendored `lua/` | [04](04-scripting-pubsub-blocking.md) |
| Pub/Sub | `pub_sub_manager.*` | [04](04-scripting-pubsub-blocking.md) |
| Namespaces (multi-tenancy) | `include/namespace/`, `src/namespace/` | [05](05-namespaces.md) |
| Vector search | `include/vector/`, `src/vector/` | [06](06-vector-search.md) |
| RDB/AOF interop & tools | `redis_rdb_restore.*`, `src/tools/eloqkv2{rdb,aof}/`, `crcspeed/`, `fpconv/` | [07](07-persistence-and-tools.md) |
| Vendored Redis C utils | `src/redis/` (dict, sha1, siphash, zmalloc, commands…) | — |
| Engine | `data_substrate/` submodule | `data_substrate/docs/` |

## Process Bootstrap (`src/redis_server.cpp`)

`main()` wires the protocol layer into the engine in four ordered steps (mirroring `DataSubstrate`'s lifecycle, see `data_substrate/docs/01-architecture-overview.md`):

1. **`DataSubstrate::Instance().Init(config_file)`** — after `ConvertEloqkvFlagsToTxFlags()` translates eloqkv-named flags (`ip`/`port`/`ip_port_list`/standby/voter lists) into the engine's `tx_*` flags. gflags override ini values throughout (`CheckCommandLineFlagIsDefault` pattern).
2. **`RedisServiceImpl::Init(server)`** (`src/redis_service.cpp:219`) — builds the prebuilt table list and calls `DataSubstrate::RegisterEngine(TableEngine::EloqKv, &catalog_factory, nullptr, prebuilt_tables, engine_metrics, eloqkv_publish_func)`:
   - one engine table per Redis database: `data_table_0` … `data_table_<databases-1>` (`databases` from config, default 16), each `TableType::Primary` + `TableEngine::EloqKv`;
   - two namespace system tables: `__ns_0` (namespace registry) and `ns_data_0` (shared namespace data) — see [05-namespaces.md](05-namespaces.md);
   - per-command duration/total metrics and read/write aggregates;
   - `eloqkv_publish_func` so the engine can deliver cross-node PUBLISH messages back into this layer ([04](04-scripting-pubsub-blocking.md)).
3. **`DataSubstrate::Instance().Start()`** — boots log service, storage handler, and the tx service (TxProcessors, Sharder, checkpointer…).
4. **`RedisServiceImpl::Start(server)`** (`src/redis_service.cpp:635`) then brpc server start — the service object is installed as `brpc::ServerOptions::redis_service` (the brpc Redis protocol entry point; brpc owns and deletes it), `num_threads` is pinned to the engine's `bthread_concurrency` (= `core_number`), builtin brpc services are disabled, and TLS is configured when enabled. The server listens on `eloqkv_port` (default 6379).

Shutdown is the reverse: stop accepting, `RedisServiceImpl::Stop()`, `DataSubstrate::Shutdown()`.

## How a command becomes engine work (one paragraph; details in [02](02-command-processing.md)/[03](03-data-model.md))

brpc parses RESP on a bthread and calls into `RedisServiceImpl`; the command name is looked up in the dispatch table; a command object is constructed (parse/validate args); the service obtains a `TransactionExecution` from the engine (`NewTxm`, pinned to the current core's shard) and submits an `ObjectCommandTxRequest` / `MultiObjectCommandTxRequest`; the engine routes the command to the owner shard (local or remote) where it executes against the in-memory `TxObject`; results flow back through the request's `TxResult`, and the bthread renders the RESP reply via the replier. Single commands auto-commit; `MULTI/EXEC` and the session-style `BEGIN/COMMIT/ROLLBACK` map onto one engine transaction.

Because connection bthreads both *submit* transactions and *drive* the engine (brpc workers double as tx processors — `data_substrate/docs/02-threading-model.md`), this layer must follow the engine's threading contract: never block a bthread on a synchronization primitive shared with shard-side `Execute()` code; use the yield/resume functors on `TxRequest` or atomic + `bthread_usleep` patterns.

## Configuration Surface (layer-specific)

Defined in `src/redis_server.cpp` / `src/redis_service.cpp`; engine flags are listed in `data_substrate/docs/01-architecture-overview.md`.

| Flag | Default | Meaning |
|---|---|---|
| `config` | "" | ini file path (same file is handed to the engine) |
| `eloqkv_port` | 6379 | RESP listen port |
| `ip` / `port` / `ip_port_list` / `standby_ip_port_list` / `voter_ip_port_list` | — | translated to engine `tx_*` flags at startup |
| `databases` (ini `[local]`) | 16 | number of Redis databases = number of `data_table_<n>` tables |
| `requirepass` (ini) | "" | AUTH password |
| `cluster_mode` | — | strict Redis Cluster compatibility behavior |
| `txn_isolation_level` / `protocol` / `isolation_level` | — | engine isolation/cc-protocol selection for txs ([02](02-command-processing.md)) |
| `retry_on_occ_error` | — | auto-retry policy for OCC conflicts |
| `enable_tls` / `tls_cert_file` / `tls_key_file` | off | TLS on the RESP port |
| `slow_log_threshold` / `slow_log_max_length` | — | SLOWLOG |
| `enable_redis_stats`, `enable_cmd_sort` | — | INFO/stats behavior |
| `cc_notify` | — | notify-based (vs polling) wakeup between layer and engine |
| `vector_index_worker_num` | — | vector index worker threads ([06](06-vector-search.md)) |
| `maxclients`, `enable_io_uring`, … | — | engine flags commonly set from eloqkv configs |

## Build shapes

- Default: `eloqkv` executable (`src/redis_server.cpp` + the `RESELOQ` library).
- `BUILD_ELOQKV_AS_LIBRARY=ON`: builds `eloqkv_lib` for converged multi-engine binaries (the engine's `EnableEngine`/`WaitForEnabledEnginesRegistered` flow exists for this).
- Offline tools `eloqkv_to_rdb` / `eloqkv_to_aof` ([07](07-persistence-and-tools.md)); storage backend selected by `WITH_DATA_STORE` (see repo `CLAUDE.md`).
