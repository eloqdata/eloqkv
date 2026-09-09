# 02 — Command Processing & the Service Layer

The service layer translates Redis RESP requests into Data Substrate work. It
owns protocol dispatch, per-connection state, transaction scoping, reply
rendering, and the policy that decides whether remote data is forwarded or
reported to the client. Command and object semantics live in
[03-data-model.md](03-data-model.md); engine transaction internals live in
`data_substrate/docs/`.

## Request flow

```text
RESP request on a brpc bthread
  -> RedisServiceImpl::DispatchCommand
  -> authentication and namespace scope
  -> command-specific parser and handler
  -> DirectCommand, ObjectCommandTxRequest, or MultiObjectCommandTxRequest
  -> TransactionExecution and owner CcShard
  -> command result through OutputHandler
  -> RESP2 reply
```

brpc owns the socket parsing context and invokes the service inline while it
consumes a connection's commands. `DispatchCommand` refreshes the connection's
namespace binding, installs the namespace guard, enforces authentication and
shutdown state, then routes either to the active `MULTI` handler or to the
registered command handler. Parsers validate arguments before any engine work
is submitted.

Transactional handlers construct an `EloqKey` and a `RedisCommand`, acquire or
reuse a `TransactionExecution`, and wrap the command in an engine request.
Single-key commands use `ObjectCommandTxRequest`; commands that coordinate
several keys or stages use `MultiObjectCommandTxRequest`. Direct commands run
in the service layer when no transactional record access is required. Results
are rendered through `OutputHandler`, which lets the same command feed either
the RESP2 replier or Lua's result bridge.

## Connection ownership and lifecycle

Each accepted socket has one `RedisConnectionContext`, owned through brpc's
connection-context lifecycle. It holds the selected database, authentication
and namespace binding, transaction state, scan cursors, client metadata, and
Pub/Sub subscriptions. Long-lived state is connection-scoped:

- a queued `MULTI` transaction is owned by `MultiTransactionHandler`;
- an interactive `BEGIN` transaction is referenced by `ctx->txm`;
- subscription membership is mirrored in the connection and the process-wide
  Pub/Sub registry.

Disconnect cleanup removes subscriptions and aborts an unfinished `BEGIN`
transaction. The `MULTI` handler likewise aborts any transaction it still owns.
This prevents a connection lifetime from leaking an engine transaction or a
subscriber pointer.

The selected Redis database maps to a prebuilt `data_table_<n>` table. A
non-default namespace instead routes to the shared namespace data table; the
namespace prefix is already embedded in parsed keys. See
[05-namespaces.md](05-namespaces.md).

## Transaction shapes

| Shape | Service-layer behavior | Transaction boundary |
|---|---|---|
| Ordinary command | Allocate a txm and submit one object or multi-object request with auto-commit | One command |
| Multi-stage command | Run stages on one txm; intermediate stages do not auto-commit | Whole command |
| `MULTI` / `EXEC` | Parse and queue commands, execute them sequentially on one txm at `EXEC`, then commit or abort | Whole queue |
| `WATCH` | Read watched keys on the transaction later used by `EXEC`; commit validation decides whether `EXEC` succeeds | `WATCH` through `EXEC`/discard |
| `BEGIN` / `COMMIT` / `ROLLBACK` | Store one txm in the connection; commands execute immediately without auto-commit until the session ends | Interactive session |
| Lua | Re-enter command dispatch with one script-owned txm | Whole script; see [04](04-scripting-pubsub-blocking.md) |

Simple commands and multi-command transactions have separately configurable
isolation/protocol settings. The service may retry an un-watched `MULTI` or Lua
execution after retryable OCC conflicts; retry replays the whole transaction,
not an individual failed operation. Once `CommitTx` or `AbortTx` returns, the
txm may have been recycled and must no longer be referenced.

`MULTI` differs from the interactive transaction interface: queued commands do
not execute until `EXEC`, whereas commands inside `BEGIN` execute and reply
immediately against the open transaction. Multi-stage commands keep
auto-commit disabled until their final stage so all participating keys remain
in one atomic engine transaction.

## Threading contract

With external transaction processors enabled, `NewTxm` binds the transaction
to the current brpc task group. The custom brpc parser keeps command processing
on that group, which preserves the one-to-one relationship between a worker,
its `TxProcessor`, and its `CcShard`.

Waiting must preserve that ownership model. Requests either drive the txm while
waiting or use the configured yield/resume callbacks to park and resume the
bound bthread. Code shared with shard-side `CcRequest::Execute()` must not use a
bthread mutex/condition-variable dependency that can block the shard's worker;
the engine's atomic polling or bound-task resume patterns are the supported
cross-context mechanisms. The complete rule is in
`data_substrate/docs/02-threading-model.md`.

## Distribution and replies

`EloqKey` uses Redis-compatible hash slots. For data owned by another node
group, a request is either forwarded by the engine or translated into a Redis
`MOVED` reply according to the redirect policy. Transactions and multi-object
operations request internal forwarding because returning a redirect after
partial execution would break their atomic boundary. Replica write failures
are translated to the Redis `READONLY` form where applicable.

The public output contract is RESP2. Protocol/validation errors originate in
the command parser or Redis error table; engine failures are mapped at the
request boundary, with explicit translations for routing and replica state.
Lua uses a different `OutputHandler` implementation but consumes the same
command result model.

## Cross-module invariants

- A command is parsed under the namespace guard, so its physical keys retain
  their tenant scope throughout queueing, forwarding, and retries.
- A transaction-spanning command must reuse one txm and must not auto-commit an
  intermediate request.
- A txm is invalid after commit or abort returns.
- Work that runs on a `CcShard` must respect the bthread/TxProcessor ownership
  contract; shard execution must never synchronously depend on a parked bthread
  running on the same worker.

## Source map

| Claim | Repository source |
|---|---|
| RESP dispatch, authentication, namespace guard, handler lookup, and error mapping | `src/redis_service.cpp`, `include/redis_service.h` |
| Command parsing and handler-to-request translation | `src/redis_handler.cpp`, `include/redis_handler.h`, `src/redis_command.cpp`, `include/redis_command.h` |
| Per-connection state and disconnect cleanup | `include/redis_connection_context.h`, `src/redis_connection_context.cpp` |
| `MULTI`, `WATCH`, and interactive transaction lifecycles | `src/redis_handler.cpp`, `src/redis_service.cpp`, `tests/unit/eloq/multi.tcl`, `tests/unit/eloq/multi2.tcl` |
| Object and multi-object request contracts | `data_substrate/tx_service/include/tx_request.h`, `data_substrate/tx_service/src/tx_execution.cpp` |
| Bound-task transaction acquisition and wait behavior | `src/redis_service.cpp`, `data_substrate/tx_service/include/tx_request.h`, `data_substrate/docs/02-threading-model.md` |
| Redis hash slots and client-visible redirects | `include/eloqkv_key.h`, `src/redis_service.cpp`, `tests/unit/eloq/cluster_cmds.tcl` |
| RESP2/Lua result abstraction | `include/output_handler.h`, `include/redis_replier.h`, `src/redis_replier.cpp`, `include/lua_output_handler.h` |
