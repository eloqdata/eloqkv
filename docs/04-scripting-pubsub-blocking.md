# 04 — Lua Scripting, Pub/Sub, and Blocking Commands

Lua scripting, Pub/Sub, and blocking list commands add stateful behavior around
the ordinary command pipeline. They share the service and engine boundaries,
but their state has different ownership and durability.

## Lua scripting

`LuaInterpreter` wraps the vendored Lua runtime and is reused through a
service-owned interpreter pool. An interpreter has a private
`RedisConnectionContext`; the caller's selected database is copied into it, so
script-local connection changes do not mutate the client's context. The
runtime exposes the supported Redis bridge and selected libraries while
removing filesystem and operating-system access and protecting the global
environment.

Script source is cached by SHA in `RedisServiceImpl`. The cache and compiled
interpreter functions are process-local, not replicated cluster state, so an
`EVALSHA` request must reach a node that has learned the script body.

Each `EVAL`/`EVALSHA` attempt creates one transaction using the transaction
isolation/protocol configuration. `redis.call` and `redis.pcall` re-enter
`RedisServiceImpl::GenericCommand` with that same txm and auto-commit disabled.
On success the service commits once; on an error it aborts. Retryable OCC
failures rerun the complete script in a fresh transaction, so script code and
direct side effects must tolerate whole-script retry.

The Lua/Redis bridge translates command results through `LuaOutputHandler` and
converts the final Lua return value back into RESP2. Only commands implemented
by `GenericCommand` are available from scripts. Nested scripting and
transaction-control commands are outside this model. Direct commands such as
`PUBLISH` do not become transactional merely because they are invoked by a
script.

## Pub/Sub

`PubSubManager` is one process-local registry of exact-channel and pattern
subscriptions. It holds raw connection-context pointers under its mutex, while
each connection mirrors its memberships so unsubscribe replies and disconnect
cleanup can remove them. Subscription acknowledgements and published messages
are serialized as RESP2 arrays and written directly to the socket through the
connection context.

Publishing has two paths:

1. The local manager matches the channel and patterns and writes to local
   subscribers.
2. A `PublishTxRequest` asks Data Substrate to forward the message to other
   node-group leaders. The engine invokes the publish callback registered by
   EloqKV, which feeds each receiving process's local manager.

Pub/Sub state and messages are not persisted or transactionally acknowledged.
Delivery is best-effort and at-most-once, and the integer returned by
`PUBLISH` counts local deliveries. Channel identity is not qualified by Redis
database or EloqKV namespace, so channels are process/cluster-wide names rather
than tenant data keys.

## Blocking commands

Blocking list commands are represented as multi-stage transactional commands;
they do not block a `TxProcessor` thread. Their durable mutation is separated
from waiting:

1. The command first tries the candidate keys without blocking.
2. If no value is available, an `ApplyCc` request is parked on each key's
   non-blocking lock queue and the shard continues other work.
3. A writer that makes a key eligible transfers one parked request to the
   write lock and re-enqueues it for shard execution.
4. The multi-object command selects the winning key, discards the other parked
   requests, and performs the actual pop (and destination push for move
   commands) before committing.
5. Timeout or failure sends discard operations that remove parked requests and
   complete the client request without a data mutation.

The client bthread waits on the transaction request while yielding scheduler
capacity. The transaction state machine remains heap-owned and is periodically
re-enlisted so wakeups and deadlines make progress. Shard workers only enqueue,
park, transfer, or discard requests; they never sleep waiting for a list value.

The wait phase is read-only and does not enter the WAL. Only the post-wakeup
pop/push phase takes the write lock and becomes durable, preventing another
consumer from stealing the selected element between wakeup and mutation.
Inside `MULTI`, blocking forms execute as non-blocking variants so the queued
transaction cannot suspend indefinitely; blocking commands are not exposed
through the Lua bridge.

## Cross-module invariants

- All transactional calls made by one script attempt share one txm; an OCC
  retry restarts the whole script.
- Pub/Sub lifetime is tied to connection cleanup, but delivery is neither
  durable nor part of a Redis data transaction.
- Pub/Sub channel names are not scoped by database or namespace.
- A blocking wait never occupies a shard thread and never writes the WAL.
- The writer-to-waiter handoff grants the winning transaction the write lock
  before the pop stage is executed.
- Timeout cleanup must discard every still-parked child request.

## Source map

| Claim | Repository source |
|---|---|
| Lua runtime, sandbox, Redis bridge, and value conversion | `include/lua_interpreter.h`, `src/lua_interpreter.cpp`, `include/lua_output_handler.h` |
| Interpreter pool, script cache, transaction/retry loop, and `GenericCommand` bridge | `include/redis_service.h`, `src/redis_service.cpp`, `src/redis_handler.cpp` |
| Script behavior exercised through the Redis interface | `tests/unit/eloq/scripting.tcl` |
| Process-local subscriptions, direct socket output, and disconnect cleanup | `include/pub_sub_manager.h`, `src/pub_sub_manager.cpp`, `include/redis_connection_context.h`, `src/redis_connection_context.cpp` |
| Cross-node publish request and callback integration | `src/redis_service.cpp`, `data_substrate/tx_service/include/tx_request.h`, `data_substrate/tx_service/src/tx_execution.cpp`, `data_substrate/tx_service/src/cc/local_cc_shards.cpp` |
| Pub/Sub observable behavior | `tests/unit/eloq/pubsub.tcl` |
| Blocking command stages and timeout/discard commands | `include/redis_command.h`, `src/redis_command.cpp`, `src/redis_service.cpp` |
| Parked-request queues, writer handoff, and transaction re-enlistment | `data_substrate/tx_service/include/cc/object_cc_map.h`, `data_substrate/tx_service/src/cc/non_blocking_lock.cpp`, `data_substrate/tx_service/src/tx_operation.cpp`, `data_substrate/tx_service/include/tx_service.h` |
| Blocking command behavior | `tests/unit/eloq/list.tcl` |
