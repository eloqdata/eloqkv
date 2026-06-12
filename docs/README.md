# EloqKV Technical Documentation

Module-by-module design documentation for the EloqKV Redis API layer, written so that an engineer (or an AI model) can understand the design before reading the code. Every doc cites the source files it was derived from — when docs and code disagree, the code wins; please fix the doc in the same change.

This repo is the **protocol layer only**. The transaction/storage engine lives in the `data_substrate/` submodule and has its own doc set at `data_substrate/docs/` — engine topics (concurrency control, transactions, distribution, durability, storage, log service) are documented there and only cross-referenced here.

## Reading order

| Doc | Module | Start here if you're touching… |
|---|---|---|
| [01-architecture-overview.md](01-architecture-overview.md) | Process bootstrap, brpc server, engine registration, config | anything (read first) |
| [02-command-processing.md](02-command-processing.md) | RESP dispatch, connection state, MULTI/EXEC & BEGIN/COMMIT, txm-on-bthread contract, cluster/redirects, repliers | `redis_service.*`, `redis_connection_context.*`, command dispatch |
| [03-data-model.md](03-data-model.md) | EloqKey, the five type objects (string/hash/list/set/zset), TxCommand families, TTL, catalog factory | `redis_*_object.*`, `redis_command.*`, `eloqkv_key.*`, `eloqkv_catalog_factory.*` |
| [04-scripting-pubsub-blocking.md](04-scripting-pubsub-blocking.md) | Lua/EVAL, pub/sub, blocking commands (BLPOP family) | `lua_interpreter.*`, `pub_sub_manager.*` |
| [05-namespaces.md](05-namespaces.md) | Namespace isolation & management, tokens, namespace GC | `include/namespace/`, `src/namespace/` |
| [06-vector-search.md](06-vector-search.md) | Vector indexes (HNSW), vector commands, index durability | `include/vector/`, `src/vector/` |
| [07-persistence-and-tools.md](07-persistence-and-tools.md) | DUMP/RESTORE RDB interop, eloqkv2rdb / eloqkv2aof exporters | `redis_rdb_restore.*`, `src/tools/` |

Engine-side reading: `data_substrate/docs/README.md` (index), especially `02-threading-model.md` (bthread/TxProcessor contract — required reading before touching anything concurrent) and `05-data-model-and-catalog.md` (the TxObject/TxCommand model EloqKV plugs into).

## Maintenance rules

- **Code changes that alter behavior described here must update the corresponding doc in the same PR.** The per-doc "Key files" lists tell you which doc owns which source files.
- Keep docs grounded: cite file paths, prefer invariants and flows over API listings, delete statements you can no longer verify.
- New module → new numbered doc + a row in the table above + a pointer from the repo `CLAUDE.md`.
