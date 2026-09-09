# Architecture overview

EloqKV is the Redis/Valkey-compatible API layer of a distributed transactional database. This repository owns RESP-facing service behavior and Redis data semantics. The `data_substrate/` submodule owns transaction execution, concurrency control, distribution, WAL and persistent storage.

## System context and responsibilities

| Component | Responsibility | Boundary |
|---|---|---|
| Server bootstrap | Translate EloqKV configuration, register the API engine, order service startup and shutdown | `DataSubstrate` lifecycle and the brpc server |
| Redis service | Bind connection state to parsed commands, transactions and RESP replies | brpc Redis protocol on one side; `TxRequest` on the other |
| Redis data model | Supply concrete keys, records, objects and commands without exposing Redis semantics to the engine | `CatalogFactory`, `TxKey`, `TxObject` and `TxCommand` contracts |
| Feature subsystems | Implement scripting, Pub/Sub, blocking operations, namespace isolation and vector indexes | Redis commands plus focused engine requests |
| Compatibility tools | Translate between EloqKV objects and Redis DUMP, RDB or AOF formats | Stable Redis formats and the configured data-store reader |
| Data Substrate | Execute and commit transactions, shard ownership, persist WAL/checkpoints and recover state | Public headers and lifecycle APIs under `data_substrate/` |

Vendored Redis utilities, Lua, `crcspeed` and `fpconv` support those components but are not EloqKV-owned architecture modules.

## Lifecycle

`main()` loads configuration, initializes the Data Substrate, and lets `RedisServiceImpl` register the EloqKV catalog and prebuilt tables. The Data Substrate then starts its log, storage and transaction services. EloqKV installs its Redis service into brpc and begins accepting connections only after those dependencies are ready.

An optional administrative Redis listener dispatches through a non-owning proxy to the same `RedisServiceImpl`; it does not create a second service state. That listener is stopped before the primary brpc server releases the shared service.

Shutdown stops request admission and EloqKV-owned background work before tearing down the Data Substrate. This ordering keeps callbacks and background tasks from reaching engine state after its lifetime ends.

## Request and data flow

1. brpc parses a RESP request and dispatches it with its connection context to `RedisServiceImpl`.
2. EloqKV parses command arguments, selects the Redis database or namespace, and creates Redis-specific command/key objects.
3. The service submits an object, multi-object or other focused `TxRequest` to a `TransactionExecution` owned by the Data Substrate.
4. The engine routes the request to the owning shard, applies the `TxCommand` to the `TxObject`, and commits according to the selected transaction mode.
5. EloqKV converts the result into a RESP reply. Explicit transactions retain connection-scoped state; standalone commands complete within one request.

The detailed service state machine is in [02](02-command-processing.md), the object/catalog contract in [03](03-data-model.md), and engine execution in [`data_substrate/docs/`](../data_substrate/docs/README.md).

## Cross-cutting invariants

- EloqKV defines Redis semantics, while the Data Substrate owns transaction and storage correctness. The integration must go through the engine's public key, object, command, catalog and request contracts.
- Request handlers run on brpc bthreads, while shard work may run on a TxProcessor or a brpc worker main stack. Cross-context waits must preserve the engine threading contract documented in `data_substrate/docs/02-threading-model.md`.
- Connection-scoped state owns authentication, selected database/namespace, subscription mode and explicit transaction state. It must be cleaned up when the connection closes.
- Namespace isolation is established before command keys are constructed; bypassing that boundary would alias tenant data.
- Persisted or wire-visible Redis encodings are compatibility contracts. In-memory container choices and local optimizations are not architecture unless they change one of those contracts.

## Source map

| Claim | Repository source |
|---|---|
| Process initialization, engine registration and shutdown ordering | `src/redis_server.cpp`, `src/redis_service.cpp` |
| RESP service and connection-scoped state | `include/redis_service.h`, `src/redis_service.cpp`, `include/redis_connection_context.h`, `src/redis_connection_context.cpp` |
| Redis key/object/command integration contracts | `include/eloqkv_key.h`, `include/redis_object.h`, `include/redis_command.h`, `include/eloqkv_catalog_factory.h` |
| Namespace, vector, scripting and Pub/Sub subsystem boundaries | `include/namespace/`, `src/namespace/`, `include/vector/`, `src/vector/`, `include/lua_interpreter.h`, `src/lua_interpreter.cpp`, `include/pub_sub_manager.h`, `src/pub_sub_manager.cpp` |
| Redis format interoperability and offline exporters | `include/redis_rdb_restore.h`, `src/redis_rdb_restore.cpp`, `src/tools/` |
| Transaction-engine lifecycle and request boundary | `data_substrate/core/include/data_substrate.h`, `data_substrate/tx_service/include/tx_request.h`, `data_substrate/tx_service/include/tx_execution.h` |
