# 02 — Command Processing & the Service Layer

EloqKV's service layer is a brpc-based RESP server (`RedisServiceImpl`, a subclass of the custom
brpc fork's `brpc::RedisService`) that turns Redis commands into Data Substrate transactions.
Every command is parsed on the brpc worker bthread that read it from the socket, looked up in a
flat name→handler table, parsed into a command object (`RedisCommand` / `RedisMultiObjectCommand` /
`DirectCommand` / `CustomCommand`), wrapped in a `TxRequest`, and executed through a
`TransactionExecution` (txm) obtained from the engine's `TxService` — one auto-committed txm per
simple command, or a long-lived txm for MULTI/EXEC, Lua scripts, and the EloqKV-specific
interactive `BEGIN`/`COMMIT`/`ROLLBACK` sessions. Replies are rendered through an `OutputHandler`
abstraction (RESP2 via `RedisReplier`, or Lua tables via `LuaOutputHandler`). This doc covers the
service layer only; engine internals are in `data_substrate/docs/` (esp. `02-threading-model.md`,
`04-transaction-execution.md`). Command/object semantics are in `docs/03-data-model.md`.

## File map

| Area | Files |
|---|---|
| Service impl, dispatch, tx execution | `include/redis_service.h`, `src/redis_service.cpp` |
| Per-command handlers (the command table) | `include/redis_handler.h`, `src/redis_handler.cpp` |
| Per-connection state | `include/redis_connection_context.h`, `src/redis_connection_context.cpp` |
| Command objects + parsers | `include/redis_command.h`, `src/redis_command.cpp` (see docs/03) |
| Process entry point | `src/redis_server.cpp` |
| Reply rendering | `include/output_handler.h`, `include/redis_replier.h`, `src/redis_replier.cpp`, `include/lua_output_handler.h` |
| Errors / stats | `include/redis_errors.h`, `include/redis_stats.h`, `src/redis_stats.cpp` |

## 1. Process bootstrap

`main()` (`src/redis_server.cpp:425`) runs a fixed sequence:

1. Parse gflags + INI config (`--config`). User-facing flags (`ip`, `port`, `ip_port_list`,
   `standby_ip_port_list`, `voter_ip_port_list`) are translated to engine flags (`tx_ip`,
   `tx_port`, `tx_ip_port_list`, ...) with **tx ports = redis port + 10000**
   (`src/redis_server.cpp:152-423`; reverse mapping `TxPortToRedisPort`,
   `include/redis_service.h:185`).
2. `DataSubstrate::Instance().Init(config_file)` then `EnableEngine(TableEngine::EloqKv)`.
3. `RedisServiceImpl::Init()` (`src/redis_service.cpp:219`) — *before* the engine starts:
   registers the EloqKv engine with `DataSubstrate::RegisterEngine` along with prebuilt tables
   (`data_table_0..N-1` for `databases` logical DBs, default 16, plus the two namespace tables
   `__ns_0` and `ns_data_0`; `src/redis_service.cpp:232-281`), the catalog factory, per-command
   metric definitions, and the pub/sub publish callback. It also parses isolation/protocol flags
   (§3), TLS settings (`enable_tls`, `tls_cert_file`, `tls_key_file`; TLS forces io_uring off,
   `src/redis_service.cpp:528-531`), and builds the command table via `AddHandlers()`.
4. `DataSubstrate::Instance().Start()` — engine up (TxService, store handler, log service).
5. `RedisServiceImpl::Start()` (`src/redis_service.cpp:635`) — second phase: grabs
   `TxService`/`DataStoreHandler` pointers, sizes per-core slow-log structures, computes the
   listen address, starts the metrics collector thread and namespace GC daemon. In `bootstrap`
   mode it exits the process right here after table creation (`src/redis_service.cpp:704-721`).
6. brpc `Server::Start()` with `server_options.redis_service = redis_service_impl` (ownership
   transfers to the server) and optional SSL options; then `RunUntilAskedToQuit()`
   (`src/redis_server.cpp:502-548`).

## 2. Request path end-to-end

```
client TCP/RESP
   │
   ▼  brpc worker bthread (EloqData brpc fork)
ParseRedisMessage / ConsumeCommand              brpc fork: src/brpc/policy/redis_protocol.cpp
   │   - RESP parse into args[]; args[0] lowercased by the parser
   │   - pins the task to its TaskGroup (SetBoundGroup) when
   │     brpc_worker_as_ext_processor is on
   ▼
RedisServiceImpl::DispatchCommand               src/redis_service.cpp:5924
   │   - refresh namespace binding (ctx->ns_meta/ns_id), NamespaceGuard
   │   - AuthRequired() → "NOAUTH" (5877); shutdown check
   │   - in MULTI? → MultiTransactionHandler::Run (queue)
   │   - else FindCommandHandler(args[0]) → handler->Run(...)
   ▼
<X>CommandHandler::Run                          src/redis_handler.cpp (e.g. GET at 747)
   │   - Parse<X>Command(args) → (EloqKey, <X>Command) or error reply
   │   - txm = ctx->txm (BEGIN session)  or  NewTxm(iso_level_, cc_protocol_)
   ▼
RedisServiceImpl::ExecuteCommand                src/redis_service.cpp:2657 (single-key)
   │   - key-size guard; builds ObjectCommandTxRequest
   │     (MultiObjectCommandTxRequest for multi-key, 2716)
   │   - optionally attaches yield/resume functors (§5)
   ▼
ExecuteTxRequest / ExecuteMultiObjTxRequest     src/redis_service.cpp:4471 / 4519
   │   - SendTxRequestAndWaitResult → txm->Execute(req); req->Wait()
   │   - error mapping: MOVED / READONLY / engine error text (1220-1331)
   ▼
engine: TransactionExecution → CcRequest        see data_substrate/docs/03, 04
   ▼
cmd->OutputResult(output)                       command object renders its result
   ▼
RedisReplier → brpc::RedisReply → RESP2 bytes back on the socket
```

Commands are executed **inline in the socket-parse path** — there is no per-command bthread
spawn; pipelined commands in one read buffer are consumed in a loop and their replies batched
into a single socket write (brpc fork `redis_protocol.cpp`, `ConsumeCommand` loop).

Command objects own argument validation: each `Parse<X>Command` in `src/redis_command.cpp`
writes protocol errors (wrong arity, bad integer, ...) straight to the `OutputHandler` and
returns `success=false`, in which case no txm work happens. Limits enforced at this layer:
key ≤ 32 MB (2 KB for the EloqStore backend) and object ≤ 256 MB
(`src/redis_service.cpp:193-199`, checked at 2665 and in write-command parsers).

## 3. Connection state machine

One `RedisConnectionContext` per socket, created by `NewConnectionContext`
(`src/redis_service.cpp:5917`) and owned by brpc's per-socket parsing context. Key fields
(`include/redis_connection_context.h:61-170`):

| Field | Meaning |
|---|---|
| `db_id` | logical DB selected by `SELECT` (0..`databases`-1); maps to table `data_table_<n>` via `RedisTableName` (`src/redis_service.cpp:5863`). `SELECT` is rejected inside a non-default namespace (`RD_ERR_SELECT_FORBIDDEN_IN_NS`, `src/redis_command.cpp:1736`) |
| `authenticated` | set by `AUTH`; password compared to `requirepass`, **or** interpreted as a namespace token, which also binds `ns`/`ns_meta`/`ns_id` (`src/redis_command.cpp:1495-1516`; see docs/05) |
| `in_multi_transaction` + `multi_transaction_handler` | MULTI queueing state machine (§4) |
| `txm` | non-null while a `BEGIN` session transaction is open |
| `subscribed_channels` / `subscribed_patterns` | pub/sub state (docs/04); destructor unsubscribes all |
| `scan_cursors`, `bucket_scan_cursors` | server-side SCAN cursor caches (LRU of 100 / per-db bucket save points) |
| `connection_name`, `lib_name`, `lib_ver` | `CLIENT SETNAME/SETINFO` |
| `output`/`arena` | connection-owned `RedisReply` used for out-of-band pushes (pub/sub `FlushOutput`, `src/redis_connection_context.cpp:90`) |

Auth gating: when `requirepass` is set, every command except `auth`, `hello`, `quit`, `reset`
and `NAMESPACE CURRENT` returns `NOAUTH` until authenticated (`src/redis_service.cpp:5895-5914`).
Note there is **no HELLO handler registered**, so `HELLO` (and thus RESP3 negotiation) returns
"unknown command" despite being auth-exempt — the server speaks RESP2 only (§7).

Disconnect: the context destructor aborts a dangling `BEGIN` txm with `AbortTx` and bumps the
closed-connection stat (`src/redis_connection_context.cpp:37-54`). A pending MULTI handler's
destructor likewise aborts its txm (`src/redis_handler.cpp:1680`).

Subscribe mode: subscriptions are tracked, but `DispatchCommand` performs **no
"subscribe-mode only" command restriction** the way vanilla Redis does — regular commands still
execute on a subscribed connection (verified absence in `src/redis_service.cpp:5924-6191`).

## 4. Transaction semantics

Two independent (isolation, protocol) pairs are configured at startup
(`src/redis_service.cpp:129-141, 391-487`):

| Mode | Flags (defaults) | Used by |
|---|---|---|
| Simple commands | `--isolation_level=ReadCommitted`, `--protocol=OccRead` → statics `RedisCommandHandler::iso_level_`/`cc_protocol_` (`include/redis_handler.h:45-50, 92-96`) | every auto-commit command |
| Transactions | `--txn_isolation_level=RepeatableRead`, `--txn_protocol=OCC` → `txn_isolation_level_`/`txn_protocol_` | MULTI/EXEC, WATCH, Lua scripts, BEGIN sessions |

Accepted values: `ReadCommitted`/`RepeatableRead` and `OCC`/`OccRead`/`Locking`.

**Auto-commit (default).** Handler calls `NewTxm()` and `ExecuteCommand(..., auto_commit=true)`;
the engine commits/aborts the tx as part of processing the `ObjectCommandTxRequest` itself
(`auto_commit_` flag on the request). Multi-step commands (`MultiObjectCommandTxRequest`, e.g.
LMPOP/SMOVE/blocking pops) suppress auto-commit on middle steps and the service layer issues the
final `CommitTx`/`AbortTx` based on `IsPassed()` (`src/redis_service.cpp:4538-4613`).

**MULTI/EXEC.** `MultiHandler::Run` returns `REDIS_CMD_CONTINUE` (`src/redis_handler.cpp:1603`),
which makes `DispatchCommand` create a `MultiTransactionHandler` and route every subsequent
command to it (`src/redis_service.cpp:5999-6086`). Queued commands are parsed eagerly by
`ParseMultiCommand` (`src/redis_command.cpp:8351`) into a
`std::variant<DirectRequest, ObjectCommandTxRequest, MultiObjectCommandTxRequest,
CustomCommandRequest>`; parse failure marks the tx poisoned
(`RD_ERR_EXEC_ABORT_FOR_PREV_ERROR`) and EXEC aborts. On `EXEC`, `MultiExec`
(`src/redis_service.cpp:2055`) lazily creates **one** txm (`txn_isolation_level_`,
`txn_protocol_`), optionally key-sorts the requests to reduce deadlocks
(`--enable_cmd_sort`, off by default; 2109), executes them sequentially, then `CommitTx`.
Divergence from vanilla Redis: any runtime error **aborts the whole transaction and returns
nil** rather than executing remaining commands (2159-2352). `DISCARD` aborts and drops the
queue. Commands not whitelisted in `ParseMultiCommand` get
"Unsupported command in MULTI" (`src/redis_command.cpp:10499`).

**WATCH.** Handled before MULTI begins: `watch`/`unwatch` outside a queue create the
`MultiTransactionHandler` early (`src/redis_service.cpp:6014-6041`). `WatchKeys`
(`src/redis_handler.cpp:1849`) creates the txm immediately and reads the keys under it with a
`MultiObjectCommandTxRequest` so RepeatableRead+OCC validation detects modification at commit —
a changed watched key surfaces as an OCC failure and EXEC replies nil. `WATCH` inside MULTI or
inside a BEGIN session is an error. Watching disables OCC retry (`tx_retrieable_ = false`).

**Retry on OCC conflict.** With `--retry_on_occ_error` (default true), EXEC and Lua scripts are
transparently re-parsed and re-executed when commit fails with
`OCC_BREAK_REPEATABLE_READ`/`WRITE_WRITE_CONFLICT` (`src/redis_handler.cpp:1809-1827`,
`src/redis_service.cpp:2601-2633`) — unless keys were WATCHed.

**BEGIN/COMMIT/ROLLBACK (interactive sessions, EloqKV-specific).** `BeginHandler` stores a fresh
txm in `ctx->txm` (`src/redis_handler.cpp:1619`). While set, every ordinary handler detects
`in_tx = ctx->txm != nullptr` and runs its command on that txm with `auto_commit=false`,
`always_redirect=true`, and `cmd.SetVolatile()` (results may be buffered on the cc entry until
commit; e.g. `src/redis_handler.cpp:761-771`). Commands execute and reply immediately —
unlike MULTI there is no queueing. `COMMIT`/`ROLLBACK` call `CommitTx`/`AbortTx` and clear
`ctx->txm` (`src/redis_handler.cpp:1637, 1661`); commit failure is reported as
`ERR <engine message>`. BEGIN inside MULTI or vice versa is rejected.

**Lua (EVAL/EVALSHA).** One txm per script (`txn_isolation_level_`/`txn_protocol_`); each
`redis.call` goes through `GenericCommand` (`src/redis_service.cpp:3024`), a big switch that
re-parses and executes the command on the script's txm with `auto_commit=false`; the script
commits at the end (`EvalLua`, `src/redis_service.cpp:2483`). Details in docs/04.

## 5. txm acquisition and the bthread threading contract

`NewTxm` (`src/redis_service.cpp:2021`) binds the new `TransactionExecution` to the **current
bthread's task group**: with `EXT_TX_PROC_ENABLED` it calls
`tx_service_->NewTx(bthread::tls_task_group->group_id_)`, so the txm lives on the TxProcessor/
CcShard paired 1:1 with this brpc worker (see `data_substrate/docs/02-threading-model.md`).
The brpc fork pins the parsing task to its group for the duration of command processing
(`SetBoundGroup` in `redis_protocol.cpp`), keeping shard-local accesses thread-safe.

Waiting for results uses two schemes (`data_substrate/tx_service/include/tx_request.h:94-150`):

- **cc_notify path** — `ExecuteCommand` attaches `yield_func = bthread_block()` and
  `resume_func = resume_group->resume_bound_task(resume_tid)` to the `ObjectCommandTxRequest`
  when `FLAGS_cc_notify && (!auto_commit || skip_wal_) && ctx->txm == nullptr`
  (`src/redis_service.cpp:2677-2702`). The bthread drives the txm itself
  (`ForceExternalForwardOnce` → `txm->ExternalForward()`), parks via `bthread_block`, and the
  finishing CC request resumes it **on the original task group** — the bthread never migrates,
  so it cannot land on a "wrong" group. This covers Lua-driven commands and auto-commit
  commands when WAL is disabled.
- **default path** — no functors; `TxRequest::Wait()` loops `txm_->ExternalForward()` +
  `tx_result_.Wait()` until the result is set. Used for auto-commit commands with WAL on, for
  queued MULTI requests, and for BEGIN-session commands (excluded from cc_notify by
  `ctx->txm == nullptr`).

`SendTxRequest` (`src/redis_service.cpp:1333`) maps a rejected `txm->Execute()` (committed/
aborted txm) to `TX_REQUEST_TO_COMMITTED_ABORTED_TX`.

## 6. Cluster behavior: redirect vs MOVED

Key → slot uses Redis-compatible CRC16 with `{hash-tag}` support
(`include/eloqkv_key.h:168-221`); slot = `Hash() & 0x3fff` (16384 slots). Slots map onto the
engine's 1024 range buckets as `slot_id = bucket_id + i*1024, i∈[0,16)`
(`GetNodeSlotsInfo`, `src/redis_service.cpp:960-970`), i.e. **bucket = slot mod 1024**; bucket
ownership comes from `LocalCcShards::GetAllBucketOwners` and node-group leadership from
`Sharder` (cf. `data_substrate/docs/06`, `08`).

Whether a command whose key lives on another node group is forwarded internally or bounced back
is decided in the engine (`data_substrate/tx_service/src/cc/local_cc_handler.cpp:1740`):

- `txservice_auto_redirect_redis_cmd` — set from the DataSubstrate flag `--auto_redirect`
  (default **false**, `data_substrate/core/src/tx_service_init.cpp:52`). When true, every remote
  command is transparently shipped via the remote CC handler.
- `always_redirect_` per request — handlers pass `in_tx` for simple commands (so plain
  auto-commit commands are *not* force-redirected), while MULTI-queued, Lua, and multi-object
  requests pass `true` (a transaction can't be bounced mid-flight without breaking atomicity;
  `src/redis_handler.cpp:770-771`, `src/redis_command.cpp:8497`).

If neither applies, the request fails with `DATA_NOT_ON_LOCAL_NODE` (or
`WRITE_REQUEST_ON_SLAVE_NODE`) and `SendTxRequestAndWaitResult` converts it to a client-visible
`MOVED <slot> <ip>:<port>` using the first key's slot and the current slot map
(`src/redis_service.cpp:1256-1318`, `GenerateMovedErrorMessage` at 1175). In single-node-group
non-cluster deployments a write on a replica instead returns
`READONLY You can't write against a read only replica.` (1256-1269). There is no ASK/ASKING
migration protocol (no occurrences in the service layer).

`CLUSTER INFO/NODES/SLOTS/KEYSLOT` are the only supported subcommands (`ParseClusterCommand`,
`src/redis_command.cpp:10823`; anything else gets "ERR unknown subcommand"), all
`DirectCommand`s (`include/redis_command.h:915-987`)
built from `RedisClusterNodes`/`RedisClusterSlots` (`src/redis_service.cpp:993, 1101`), which
fan out `FetchNodeInfo` RPCs to classify each replica `Online`/`Loading`/`Failed`
(`GetReplicaNodesStatus`, 807-905; `HostStatus`, `include/redis_service.h:76`). Node IDs are the
numeric node id left-padded to the 40-char format clients expect (`host_id()`,
`include/redis_service.h:115`). `--cluster_mode` (default false) makes a single node group
present itself with the cluster protocol.

## 7. Command table organization

`AddHandlers()` (`src/redis_service.cpp:1372-2019`) instantiates ~175 handler objects (owned by
`hd_vec_`) and registers them by lowercase name in `command_map_` via `AddCommandHandler`
(6347; duplicate registration fails). Lookup is exact-match (`FindCommandHandler`, 6361) — safe
because the brpc parser lowercases `args[0]`. Aliases share one handler (`hmset`→`HSetHandler`,
`unlink`→`DelHandler`, `sort_ro`→`SortHandler`). Unknown names get
``ERR unknown command `x` `` (`src/redis_service.cpp:6049-6057`).

Handler families, by how their command executes:

| Family | Examples | Path |
|---|---|---|
| `DirectCommand` — no tx | PING, ECHO, INFO, CLUSTER, CLIENT, CONFIG, TIME, SLOWLOG | `ExecuteCommand(ctx, DirectCommand*, output)` (`src/redis_service.cpp:2648`) |
| Single-key `RedisCommand` | GET/SET/INCR/HSET/ZADD/EXPIRE... | `ObjectCommandTxRequest` (§2) |
| `RedisMultiObjectCommand` | MSET/MGET, SINTERSTORE, LMPOP, BLPOP/BLMOVE... | `MultiObjectCommandTxRequest` with multi-step loop (`src/redis_service.cpp:4519`) |
| `CustomCommand` / bespoke overloads | SORT, SCAN/KEYS, HSCAN/SSCAN/ZSCAN, DUMP/RESTORE, FLUSHDB/FLUSHALL | dedicated `ExecuteCommand` overloads (e.g. 4707 SORT, 4987 SCAN); FLUSH uses `UpsertTableTxRequest` truncates (2757, 2907) |
| Control-flow handlers | MULTI/BEGIN/COMMIT/ROLLBACK/DISCARD, EVAL/EVALSHA/SCRIPT, SUBSCRIBE family | manipulate ctx state / Lua / pub-sub directly |
| Conditional builds | `eloqvec.*` (`VECTOR_INDEX_ENABLED`), `compact` (RocksDB-cloud), `fault_inject` | registered only when compiled in |

Adding a new command = write the command object + `Parse<X>Command` (redis_command.h/.cpp),
write a `<X>Handler::Run` (redis_handler.h/.cpp), register it in `AddHandlers()`, add the name
to `command_types` (`src/redis_command.cpp:107`, used for metrics/slowlog classification) and,
if it should work inside MULTI/Lua, to `ParseMultiCommand` and `GenericCommand` — these are
**three separate dispatch tables** that must be kept in sync. Optionally add it to
`cmd_access_types_` for read/write aggregated metrics (`include/redis_service.h:647`).

## 8. Output handling and error taxonomy

`OutputHandler` (`include/output_handler.h`) is the visitor every command renders through:
`OnString/OnInt/OnArrayStart/OnArrayEnd/OnNil/OnStatus/OnError/OnBool/OnFormatError`. Two
implementations:

- `RedisReplier` (`src/redis_replier.cpp`) maps onto `brpc::RedisReply` (RESP2 wire types);
  a stack of `(array, next-index)` pairs supports nested arrays (e.g. EXEC results containing
  LRANGE arrays). `OnBool` → integer, `OnNil` → null bulk string. **RESP2 only** — stated in
  `include/output_handler.h:30` and no RESP3/HELLO support exists.
- `LuaOutputHandler` (`include/lua_output_handler.h`) pushes the same events onto a Lua stack
  for `redis.call` results.

Parse/validation errors use the table in `include/redis_errors.h` (`RD_ERR_*` codes indexing
`redis_error_messages`; e.g. WRONGTYPE, cursor, EXECABORT, key/object-too-big, cluster
shutting down). Engine errors surface as `TxErrorMessage(err_code)` text via
`SendTxRequestAndWaitResult` → `error->OnError(...)` (`src/redis_service.cpp:1320-1327`), plus
the special MOVED/READONLY translations of §6.

## 9. Stats, INFO, slow log, metrics

- `RedisStats` (`src/redis_stats.cpp`) exposes brpc bvars when `--enable_redis_stats` (default
  on): connections received/rejected/closed, blocked clients, read/write/multi-object command
  counters. Incremented in the connection ctor/dtor and in `ExecuteTxRequest`/
  `ExecuteMultiObjTxRequest` (4486-4496, 4528-4536). `INFO` is a `DirectCommand`
  (`include/redis_command.h:811`) assembled from these counters plus service fields captured at
  Init (OS info, exe path, memory; `src/redis_service.cpp:587-630`).
- Prometheus-style metrics (eloq_metrics): per-command duration histograms and totals plus
  read/write aggregates, registered with the engine at Init (289-313) and collected in
  `DispatchCommand`/`MultiExec` on sampled rounds (`CheckAndUpdateRedisCmdRound`, 6405). A
  dedicated collector thread reports connection count and slow-log length every second
  (`CollectConnectionsMetrics`, 6381).
- Slow log is **per task group** (vector indexed by `group_id_`, guarded by per-group
  `bthread::Mutex`; `src/redis_service.cpp:201-209`) with `--slow_log_threshold` (µs) and
  `--slow_log_max_length`; entries truncate args (32 args / 128 bytes each). `SLOWLOG GET`
  merges all groups. `CONFIG SET slowlog-*` adjusts at runtime (4683).

## 10. Gotchas / verified invariants

- **MULTI error semantics differ from Redis**: first runtime error aborts the whole EXEC and
  returns nil; vanilla Redis executes the remaining queued commands (`src/redis_service.cpp:2159-2352`).
- **Three dispatch tables** (handler map, `ParseMultiCommand`, `GenericCommand`) must agree; a
  command registered only in the handler map silently becomes "Unsupported command in MULTI"
  and unusable from Lua.
- `FindCommandHandler` relies on the brpc fork lowercasing `args[0]`; don't register
  mixed-case names.
- `RedisTableName` consults the thread-local `current_namespace` set by `NamespaceGuard` in
  `DispatchCommand` (`src/redis_service.cpp:5863-5870, 5956`) — calling engine paths without the
  guard silently targets the wrong table family (docs/05).
- `config_` is guarded by an **atomic spin flag, not a mutex**, because the bthread may resume
  on a different task group mid-update and a bthread mutex must unlock on its locking thread
  (`include/redis_service.h:605-610`). Same class of constraint as the engine's bthread-mutex
  deadlock rules (`data_substrate/CLAUDE.md`, threading gotcha).
- A txm obtained from `NewTxm` is **invalid after `CommitTx`/`AbortTx`** (recycled); EvalLua
  nulls its pointer explicitly (`src/redis_service.cpp:2634-2637`). `CommitHandler` likewise
  clears `ctx->txm` before inspecting the result.
- Auto-commit failures of `TX_INIT_FAIL` require a manual `AbortTx` because the request never
  reached the engine (`src/redis_service.cpp:1235-1255`).
- `GET`-family commands inside BEGIN sessions must be marked `SetVolatile()` so buffered
  uncommitted writes on the cc entry are handled correctly (`src/redis_handler.cpp:764-769`).
- The brpc server owns and deletes the `RedisServiceImpl`
  (`src/redis_server.cpp:505-506`); `Stop()` only halts collectors/GC — engine shutdown is
  `DataSubstrate::Shutdown()` in `main`.
- `--auto_redirect` defaults to **false**: a multi-node-group deployment without it returns
  MOVED for simple commands (clients must be cluster-aware) while transactions still forward
  internally because they set `always_redirect_`.
