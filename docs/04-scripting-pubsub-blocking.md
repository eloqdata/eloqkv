# 04 — Lua Scripting, Pub/Sub, and Blocking Commands

EloqKV layers three "stateful" Redis features on top of the Data Substrate engine. **Lua scripting** (EVAL/EVALSHA/SCRIPT) runs each script inside a single engine transaction: a pooled Lua 5.1 interpreter executes the script body, and every `redis.call()` re-enters the normal command-parsing path (`RedisServiceImpl::GenericCommand`) bound to that one `TransactionExecution`, so the transaction-scoped operations commit or abort atomically (default RepeatableRead + OCC, with automatic whole-script retry on OCC conflicts); direct commands such as `PUBLISH` run outside that transaction. **Pub/Sub** is a node-local, in-memory subscriber registry (`PubSubManager`) plus a best-effort cross-node fan-out: PUBLISH sends a `PublishTxRequest` that streams a `PublishRequest` CcMessage to every other node-group leader, where the engine calls back into EloqKV's registered `publish_func`. **Blocking commands** (BLPOP/BRPOP/BLMOVE/BLMPOP/BRPOPLPUSH) never block a thread: the engine parks the command's `ApplyCc` request on the key's lock object (`queue_block_cmds_`), a committing writer pops and re-executes it, and the transaction machine is re-enlisted every 10 ms to check the timeout — the client's bthread meanwhile just waits on the `TxRequest` result. Engine internals referenced here are documented in `data_substrate/docs/02-threading-model.md`, `03-concurrency-control.md`, and `04-transaction-execution.md`.

Siblings: `01-architecture-overview.md`, `02-command-processing.md`, `03-data-model.md`, `05-namespaces.md`, `06-vector-search.md`, `07-persistence-and-tools.md`.

---

## 1. Lua scripting

Files: `include/lua_interpreter.h`, `src/lua_interpreter.cpp`, `include/lua_output_handler.h`, EVAL/EVALSHA/SCRIPT plumbing in `src/redis_service.cpp` and `src/redis_handler.cpp`. The Lua VM itself is the vendored Redis fork under `lua/` (with `lua_enablereadonlytable`, cjson, struct, cmsgpack, bit).

### Interpreter instances and sandbox

- `LuaInterpreter` wraps one `lua_State` (`include/lua_interpreter.h:45`). Instances are pooled in a global `moodycamel::ConcurrentQueue<std::unique_ptr<LuaInterpreter>>` (`include/redis_service.h:597`); `GetLuaInterpreter()` dequeues or constructs one, `CleanAndReturnLuaInterpreter()` clears the stack and returns it (`src/redis_service.cpp:1354-1370`). The pool is global, not per-core; any brpc worker bthread may use any interpreter.
- `InitLua` (`src/lua_interpreter.cpp:188-294`) loads base/table/string/math/debug plus cjson, struct, cmsgpack, bit (each made read-only); installs `__redis__err__handler` (adds `source:line` to errors); installs the global-protection metatable (scripts cannot create or read undeclared globals); then nils out `print`, `loadfile`, `dofile` and sets `debug = nil`. There is no `os`/`io` library, so no wall-clock or filesystem access from scripts.
- The `redis` table exposes only `call`, `pcall`, `error_reply`, `status_reply`, `sha1hex` (`src/lua_interpreter.cpp:778-819`). Redis features like `redis.log`, `redis.setresp`, `redis.replicate_commands`, `redis.breakpoint` do not exist.
- Each interpreter owns a private `RedisConnectionContext` (`lua_conn_ctx_`, `include/lua_interpreter.h:92-98`); `SetConnectionContext` copies only `db_id` from the caller's connection, so `SELECT` inside a script cannot leak to the client connection (`src/lua_interpreter.cpp:391-394`).

### Script cache and SHA

- `CreateFunction` SHA1-hashes the body and defines a global Lua function `f_<sha1hex>` if not already present, so each interpreter instance also memoizes compiled bodies (`src/lua_interpreter.cpp:344-373`).
- The service-level cache is `std::unordered_map<std::string, std::string> scripts_` (sha → body) guarded by `std::shared_mutex script_mutex_` (`include/redis_service.h:600-601`). `SCRIPT LOAD` compiles + inserts (`src/redis_service.cpp:2419-2447`); EVAL also inserts (`src/redis_service.cpp:2588-2593`); `SCRIPT EXISTS`/`SCRIPT FLUSH` read/clear it (`src/redis_service.cpp:2393-2417`). The cache is **node-local and not replicated** — EVALSHA on a node that never saw the body returns `NOSCRIPT` (`src/redis_service.cpp:2468-2473`). EVALSHA resolves sha → body and falls into `EvalLua` (`src/redis_service.cpp:2449-2481`).

### Execution flow (`EvalLua`, `src/redis_service.cpp:2483-2646`)

1. `EvalHandler`/`EvalshaHandler` reject EVAL inside `BEGIN` or `MULTI` (`src/redis_handler.cpp:1964-1973`, `2049-2058`), then call `EvalLua` on the connection's bthread.
2. Parse `numkeys`, KEYS, ARGV; enter a `while (true)` retry loop (`src/redis_service.cpp:2549`).
3. Per attempt: `NewTxm(txn_isolation_level_, txn_protocol_)` — the *transaction-class* settings, default **RepeatableRead + OCC** via `--txn_isolation_level`/`--txn_protocol` (`src/redis_service.cpp:135-141`, `440-485`), distinct from the simple-command defaults (ReadCommitted/OccRead).
4. Acquire an interpreter, set `KEYS`/`ARGV` globals, and install the redis hook: a lambda capturing the txm that forwards to `GenericCommand(ctx, txm, args, reply)` (`src/redis_service.cpp:2565-2572`).
5. `CallFunction` runs `f_<sha>` under `lua_pcall` with the error handler, with `_G` temporarily read-only (`src/lua_interpreter.cpp:396-430`).
6. On Lua error: `AbortTx(txm)`; if the message indicates an OCC conflict (`OCC break repeatable read` / `write-write conflicts`) and `--retry_on_occ_error` (default true, `src/redis_service.cpp:143`), the **whole script re-runs** in a fresh txm (`src/redis_service.cpp:2596-2614`). Otherwise the error is returned.
7. On success: `CommitTx(txm)`; commit-time OCC conflicts also re-run the script (`src/redis_service.cpp:2616-2638`). Finally `LuaReplyToRedisReply` converts the script's return value.

So a script is exactly one engine transaction: every `redis.call` issues `ObjectCommandTxRequest`s with `auto_commit=false` against the shared txm (e.g. `src/redis_service.cpp:2657-2714`), and nothing is visible to other transactions until the final commit.

### The redis.call bridge and value conversion

- `redis.call`/`redis.pcall` land in `RedisGenericCommand` (`src/lua_interpreter.cpp:599-709`): arguments are popped from the Lua stack (numbers formatted with `%.17g`), the command name lower-cased, and the hook invoked with a `LuaOutputHandler`. `call` raises a Lua error on command failure (aborting the script); `pcall` returns the `{err=...}` table to the script. Recursive invocation via debug hooks is detected and refused (`src/lua_interpreter.cpp:609-621`).
- Command result → Lua (`LuaOutputHandler`, `src/lua_interpreter.cpp:67-173`): string→string, int→integer, nil→`false`, status→`{ok=...}`, error→`{err=...}`, arrays→tables (nested via `array_index_` stack).
- Lua return → RESP (`LuaReplyToRedisReply`, `src/lua_interpreter.cpp:435-545`): string→bulk, number→**integer (truncated)**, `false`→nil, `true`→`:1`, table→array up to the first nil (with `{err=}`/`{ok=}` recognized), other types→`ERR Unsupported return type`. These match Redis semantics.

### Limits / unsupported

- Commands not in `GenericCommand`'s switch fail with `Unknown Redis command called from script` (`src/redis_service.cpp:4362-4364`). Notably absent: SUBSCRIBE/PSUBSCRIBE, blocking commands (BLPOP etc.), MULTI/EXEC, EVAL (no nesting). PUBLISH **is** supported inside scripts (`src/redis_service.cpp:3112-3120`).
- No script kill / max-execution-time mechanism was found (no `SCRIPT KILL`, no `lua_sethook` budget); a long-running script occupies its bthread until it finishes.
- Determinism is not required for replication correctness: scripts execute once on the receiving node and only their *effects* (individual `TxCommand`s) are WAL-logged / forwarded to standbys; the script text never replicates. (Inference from the architecture; the per-command logging path is the same as for ordinary writes.)
- Keys are not validated for slot-locality: each `redis.call` runs with `always_redirect=true` (`include/redis_service.h:317-323`), so a script may touch keys on remote node groups; the engine fetches/locks them remotely rather than returning CROSSSLOT errors.

---

## 2. Pub/Sub

Files: `include/pub_sub_manager.h`, `src/pub_sub_manager.cpp`, glue in `src/redis_service.cpp`, engine path in `data_substrate/tx_service`.

### Data structures

A single process-global `PubSubManager eloqkv_pub_sub_mgr` (`src/redis_service.cpp:184`) holds, under one `bthread::Mutex pub_sub_mu_`:

- `pub_sub_channels_`: `flat_hash_map<std::string, flat_hash_set<RedisConnectionContext*>>` — exact channels (`include/pub_sub_manager.h:69-72`).
- `pattern_subs_`: same shape for glob patterns (`include/pub_sub_manager.h:75-77`).

Each connection mirrors its own membership in `subscribed_channels` / `subscribed_patterns` sets (`include/redis_connection_context.h:158-159`) so `SUBSCRIBE` replies can report the count and teardown can be quick. Channel/pattern entries are erased when their subscriber set empties.

### Subscribe / unsubscribe

`SubscribeHandler` etc. (`src/redis_handler.cpp:4818+`) call straight into the manager (`src/redis_service.cpp:6426-6449`). Confirmations (`subscribe`/`unsubscribe`/`psubscribe`/`punsubscribe`, channel, count) are 3-element RESP2 arrays built in the connection's reusable `output` reply and written **directly to the socket** by `RedisConnectionContext::FlushOutput()` → `socket->Write` (`src/pub_sub_manager.cpp:39-45`, `src/redis_connection_context.cpp:90-105`), bypassing the normal brpc reply pipeline. There is no RESP3 push-frame support; subscribers receive classic RESP2 arrays.

### PUBLISH flow

`PublishCommand` is a `DirectCommand` — it does not touch any key/cc-map (`include/redis_command.h:1292-1305`, `src/redis_command.cpp:3668-3677`). `RedisServiceImpl::Publish` (`src/redis_service.cpp:6451-6462`):

1. **Cross-node**: create a throwaway txm, send `PublishTxRequest{chan, msg}` (`data_substrate/tx_service/include/tx_request.h:1166-1179`). `TransactionExecution::ProcessTxRequest(PublishTxRequest&)` iterates **all node groups** and calls `cc_handler_->PublishMessage(ng_id, ...)`, then finishes the request immediately — fire-and-forget (`data_substrate/tx_service/src/tx_execution.cpp:1074-1083`).
2. `LocalCcHandler::PublishMessage` skips the local node and sends a `CcMessage::PublishRequest` over the cc stream to each **node-group leader** (`data_substrate/tx_service/src/cc/local_cc_handler.cpp:1819-1830`, `remote/remote_cc_handler.cpp:1096-1112`).
3. On the receiving node, `CcStreamReceiver` dispatches the message to `LocalCcShards::PublishMessage` (`remote/cc_stream_receiver.cpp:1896-1905`), which spawns a background bthread that invokes the engine-registered `publish_func_` (`src/cc/local_cc_shards.cpp:1250-1268`, `include/cc/local_cc_shards.h:1838-1846`).
4. `publish_func` is EloqKV's `eloqkv_publish_func = [](chan, msg){ eloqkv_pub_sub_mgr.Publish(chan, msg); }`, wired in at startup via `DataSubstrate::RegisterEngine(..., eloqkv_publish_func)` (`src/redis_service.cpp:185-187`, `315-320`; plumbed through `core/src/data_substrate.cpp:410` → `core/src/tx_service_init.cpp:319` → `LocalCcShards`).
5. **Local**: the publisher then runs `eloqkv_pub_sub_mgr.Publish` itself (`src/pub_sub_manager.cpp:395-449`): exact-channel matches get `["message", chan, msg]`; every pattern is tested with `stringmatchlen` (Redis glob, `src/redis_string_match.cpp`) and matches get `["pmessage", pattern, chan, msg]`. Each delivery is serialized and written to the subscriber's socket inline, under `pub_sub_mu_`, so a slow subscriber stalls unrelated Pub/Sub traffic.

The integer PUBLISH returns counts **local subscribers only** (comment at `src/redis_service.cpp:6459-6461`).

### Lifecycle and guarantees

- `~RedisConnectionContext` calls `UnsubscribeAll(this)` if any subscriptions remain (`src/redis_connection_context.cpp:37-53`), removing the raw pointer from both maps. The destructor carries a `Fixme(zkl): risk of data race for subscribed_channels` comment — teardown vs. concurrent publish is a known thin spot.
- Delivery is **at-most-once, best-effort**: no persistence, no acks (the `PublishTxRequest` completes before remote delivery), per-message background bthreads, and remote fan-out targets only node-group leaders. A failed `socket->Write` just logs a warning (`src/redis_connection_context.cpp:99-103`). Subscribers connected to a node that is not currently a node-group leader will not see remote publishes (mechanism-implied; verified that sends go to `Sharder::Instance().LeaderNodeId(ng)` only).
- Namespaces/db ids are **not** part of the channel key: `Publish(chan, msg)` carries the raw channel name, so pub/sub is global across SELECT databases and namespaces (no `db_id`/`ns_id` qualification anywhere in `PubSubManager`).

---

## 3. Blocking commands (BLPOP / BRPOP / BLMOVE / BLMPOP / BRPOPLPUSH)

Files: handlers `src/redis_handler.cpp:4008-4146`, command objects `include/redis_command.h:2746-2976` + `src/redis_command.cpp:5067-5575`, engine machinery in `data_substrate/tx_service` (see `data_substrate/docs/03-concurrency-control.md` §5 and `04-transaction-execution.md`, which notes the `tx_progress_block_` / 10 ms re-enlist design).

### Command model

All five commands compile to a `RedisMultiObjectCommand` (`BLMPopCommand` or `BLMoveCommand`) — a multi-*step* state machine where each step is a vector of (key, `TxCommand`) pairs sent as one `MultiObjectCommandTxRequest`. The shared child command is `BlockLPopCommand`, whose behavior is selected by `txservice::BlockOperation` (`data_substrate/tx_service/include/tx_command.h:61-71`): `PopNoBlock` (try-pop), `BlockLock` (wait until non-empty, then hold the lock), `PopElement` (actual pop after a successful BlockLock), `Discard` (cancel a parked request), `NoBlock` (non-blocking variant for MULTI). Its `ExecuteOn` maps these to `ExecResult::{Read,Write,Delete,Block,Unlock}` (`src/redis_command.cpp:5067-5122`). Timeouts are absolute microsecond deadlines: `ts_expired_ = ClockTs() + timeout`; timeout 0 becomes one year (`src/redis_command.cpp:19847-19850`); `IsExpired()` compares against `LocalCcShards::ClockTs()` and only at the blocking step (`include/redis_command.h:2877-2885`).

### End-to-end flow (BLPOP on an empty list)

1. **Dispatch.** `BLPopHandler::Run` parses, creates/joins a txm, and calls the multi-object `ExecuteCommand` (`src/redis_handler.cpp:4064-4090`) → `ExecuteMultiObjTxRequest` (`src/redis_service.cpp:4519-4620`), which counts the client in `blocked_clients` stats and drives the step loop: `SendTxRequestAndWaitResult` → `HandleMiddleResult()` → `IncrSteps()` → repeat.
2. **Client wait.** The connection bthread parks in `tx_req->Wait()` on a `bthread::Mutex`/`ConditionVariable` (or yield/resume under `cc_notify`) (`data_substrate/tx_service/include/tx_request.h:112-150`, `tx_req_result.h:75-76`). A bthread is a coroutine; the brpc worker thread stays free.
3. **Step 0..n-1 (try-pop).** Each key is probed with `PopNoBlock`; a non-empty list returns elements immediately (`ExecResult::Write/Delete`) and the command skips to completion. An empty list returns `ExecResult::Unlock` — the shard releases the lock and finishes the request with a "nil" result (`data_substrate/tx_service/include/cc/object_cc_map.h:1168-1186`).
4. **Enter blocking step.** `HandleMiddleResult` flips all child commands to `BlockLock` (`src/redis_command.cpp:5541-5552`) and the next step sends them again to every key's owner shard.
5. **Park on the cc entry.** `ApplyCc` executes `BlockLock` on the object; if still empty, `ExecuteOn` returns `ExecResult::Block` and the shard pushes the *request itself* onto the key lock's blocked-command queue: `cce->PushBlockCmdRequest(&req)` → `NonBlockingLock::queue_block_cmds_`, then releases the just-acquired lock (`object_cc_map.h:1149-1166`, `cc_entry.h:360-366`, `non_blocking_lock.h:261-264`). The shard thread moves on — nothing blocks.
6. **Wake on mutation.** When any transaction that write-locked the key commits/clears (e.g. an LPUSH), `ReleaseWriteLock`/`ClearTx` first try `PopBlockCmdRequest(ccs, object)`: it scans the queue, asks each parked command `AblePopBlockRequest(object)` (list non-empty? `src/redis_command.cpp:5161-5173`), and for the first match upgrades that tx to the WriteLock and re-enqueues the parked `ApplyCc` onto the shard queue (`data_substrate/tx_service/src/cc/non_blocking_lock.cpp:385-391`, `570-580`, `675-705`). Re-execution now sees elements → `ExecResult::Read` → the handler-result completes, decrementing `atm_block_cnt_` in `MultiObjectCommandOp` (`tx_operation.cpp:5660-5677`).
7. **Re-enlist & forward.** Blocking txms are tracked in the TxProcessor's `tx_progress_block_` map (populated by `EnlistWaitingTx` when the top operation `IsBlockCommand()`, `tx_service.h:816-841`, via `StartTiming`, `tx_execution.cpp:700-711`). The processor loop's `CheckWaitingTxs()` re-forwards these txms every **10 ms** (`check_progress_block_period = 10000` µs vs 2 s for ordinary txs, `tx_service.h:719-749`). `MultiObjectCommandOp::Forward` at the blocking step returns early while `!IsExpired() && atm_block_cnt_ > 0` (`tx_operation.cpp:5785-5794`); once unblocked it calls `ForwardResult()` (picks the winning key, marks the rest for discard) and the service loop advances to the `PopElement` step — the real pop, this time written to the WAL — then (for BLMOVE) the push to the destination key, then auto-commit (`src/redis_service.cpp:4599-4613`).
8. **Timeout.** When the 10 ms re-enlist finds `IsExpired()`, `ForwardResult()` substitutes `BlockDiscardCommand` for every still-parked child; Forward sends these `Discard` commands to the shards (`tx_operation.cpp:5796-5864`). On the shard, `Discard` aborts the parked request out of `queue_block_cmds_`/`blocking_queue_` with `TASK_EXPIRED` (`object_cc_map.h:405-433`, `non_blocking_lock.cpp:707-735`); `TASK_EXPIRED` is deliberately not treated as an error in the post-lambdas (`tx_operation.cpp:5667-5672`). The command then reports nil to the client (`src/redis_command.cpp:5497-5500`). If a discard races and finds nothing, the txn is remembered in the shard's `active_blocking_txs_` so a late wake can still be cleaned up (`object_cc_map.h:420-422`, `cc_shard.h:1455-1459`).

### Why nothing blocks

Three parking lots, all non-blocking: (a) the *client bthread* parks on the TxRequest result (coroutine yield); (b) the *txm* is a heap state machine that simply isn't forwarded while waiting (re-polled at 10 ms); (c) the *cc request* is parked on the key's `NonBlockingLock`. Shard threads (TxProcessors) never wait on anything — consistent with the threading rules in `data_substrate/docs/02-threading-model.md`.

### Interaction with MULTI / Lua / failover

- Inside MULTI, the same commands are parsed with `multi=true` → `BlockOperation::NoBlock` and `ts_expired_ = 0`, i.e. they degrade to non-blocking try-pops (`src/redis_command.cpp:19719-19724`, `19877-19880`; dispatched from `ParseMultiCommand`, `src/redis_command.cpp:10073-10121`). Inside Lua they are simply unsupported (Section 1).
- Remote keys: a parked remote `ApplyCc` acks with the cce's term; on timeout the txm probes the remote node's liveness via `BlockCcReqCheck` (`tx_operation.cpp:5890-5904`). If the owner fails over, the request errors out rather than hanging (and the engine recovers orphaned blocking txs via the `active_blocking_txs_` map and `RemoveExpiredActiveBlockingTxs`, `cc_shard.cpp:2649-2680`).

### SCAN

Cursor iteration does **not** use this machinery: SCAN cursors are cached per connection (`BucketScanCursor` / `scan_cursors` in `include/redis_connection_context.h:48-59`, `135-151`) and each SCAN call is an ordinary bounded request.

---

## 4. Gotchas and invariants (verified)

1. **Script atomicity is transaction-scoped; retries are whole-script.** A script aborted by an OCC conflict re-executes from the top (`src/redis_service.cpp:2596-2614`), and transactional effects roll back. `PUBLISH` is a `DirectCommand` that runs outside the transaction, so it is not part of that atomic unit.
2. **Script cache is per node.** `scripts_` is a plain member of `RedisServiceImpl`; EVALSHA against a different node than the SCRIPT LOAD target yields NOSCRIPT. Also, `SCRIPT FLUSH` clears only the sha→body map; compiled `f_<sha>` globals linger in pooled interpreters (harmless, since EVALSHA checks `scripts_` first).
3. **EVAL is barred from MULTI/BEGIN** (`src/redis_handler.cpp:1964-1973`) — no nested transactions.
4. **Pub/sub is at-most-once and node-local in bookkeeping.** No durability, no cross-node subscriber registry; remote fan-out goes only to node-group leaders, fire-and-forget. PUBLISH's return value counts local deliveries only.
5. **Pub/sub writes bypass the reply pipeline** and share the connection's single `output`/arena; teardown vs. publish has an acknowledged race (`src/redis_connection_context.cpp:38` Fixme).
6. **Blocking pops are two-phase on purpose**: the `BlockLock` wait step is read-only (not logged); only the post-wake `PopElement` step takes the write lock and hits the WAL (`src/redis_command.cpp:5353-5355` comments, `object_cc_map.h` Block branch). The wake handoff upgrades the parked tx to WriteLock *before* re-execution, so no competing pop can steal the element (`non_blocking_lock.cpp:695-700`).
7. **Timeout granularity is ~10 ms** (the `CheckWaitingTxs` cadence), and a "0 = forever" timeout is actually one year (`src/redis_command.cpp:19847-19850`).
8. **Blocked clients are observable**: `INFO clients` reports `blocked_clients` maintained around `ExecuteMultiObjTxRequest` (`src/redis_service.cpp:4528-4535`, `4615-4618`).
