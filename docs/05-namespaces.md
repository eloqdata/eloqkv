# 05 — Namespace Isolation & Management

EloqKV namespaces (PR #479) are token-authenticated, multi-tenant key spaces layered on top of the
Data Substrate engine. A namespace is created by an administrator with `NAMESPACE ADD <name>`, which
returns a random 128-bit token; a client enters the namespace by sending that token as the password
in `AUTH`. Isolation is implemented purely by **key prefixing inside one shared engine table**
(`ns_data_0`), not by separate tables: every user key is transparently prepended with
`b255(ns_id) ':' b255(epoch) ':'` at `EloqKey` construction time, driven by a bthread-local
"current namespace" that is set per dispatched command from the connection context. Namespace
metadata lives in a dedicated engine table (`__ns_0`) and all metadata mutations are multi-key
engine transactions. `FLUSHDB`/`FLUSHALL` in a namespace is an O(1) epoch bump; a background GC
bthread later range-deletes the orphaned prefix through normal engine transactions. Related docs:
[02-command-processing.md](02-command-processing.md) for the dispatch pipeline,
[03-data-model.md](03-data-model.md) for `EloqKey`, and
`data_substrate/docs/04-transaction-execution.md` / `05-data-model-and-catalog.md` for the engine
transaction and table machinery used here.

## 1. Purpose & model

- A **namespace** is a named, isolated key space. The implicit namespace is `default`
  (`kDefaultNamespace`, `include/namespace/context.h:13`); it is the entire pre-existing keyspace
  and cannot be added, refreshed, or deleted (`src/redis_command.cpp:1606-1610,1638,1677-1681`).
- Names are arbitrary byte strings of 1–255 bytes (`src/redis_command.cpp:1612-1618`); the only
  reserved name is `default`. Names are *not* embedded in data keys — a monotonically increasing
  `uint64` **namespace id** is allocated from a `next_id` counter and used instead
  (`src/namespace/storage.cpp:211-243`). There is no explicit limit on namespace count.
- Relationship to the 16 Redis databases: the default namespace keeps the classic per-DB engine
  tables `data_table_0..15`. **All non-default namespaces share one engine table** `ns_data_0`, and
  `SELECT` is rejected inside a namespace with `RD_ERR_SELECT_FORBIDDEN_IN_NS`
  (`src/redis_command.cpp:1736-1743`) — a namespace has exactly one logical DB.
- Three prebuilt tables are registered at startup (`src/redis_service.cpp:236-281`):
  `data_table_<0..15>` (default namespace), `__ns_0` (namespace metadata,
  `RedisServiceImpl::NamespaceTableName()`), and `ns_data_0` (all namespace data,
  `RedisServiceImpl::NsDataTableName()`), all `TableEngine::EloqKv` primary tables.

## 2. Isolation mechanism: key prefixing, baked in at key construction

Verified: isolation is **prefix-based within a shared table**, not table-per-namespace.

- The data-key prefix is `encoded_ns_id ':' b255(epoch) ':'`
  (`NamespacePrefix::MakePrefix`, `include/namespace/prefix.h:16-25`). `b255e()` encodes integers
  base-255 while skipping the byte `0x3A` (`:`), so encoded ids/epochs can never contain the
  delimiter (`src/b255.cpp:11-28`) — parsing is unambiguous and prefixes are dense (≤9 bytes per
  component).
- A bthread-local variable holds the *current namespace prefix* (`GetCurrentNamespace()`,
  `src/namespace/context.cpp:32-50`; falls back to a thread-local for non-bthread callers). The
  RAII `NamespaceGuard` swaps it for a scope (`include/namespace/context.h:52-69`).
- `RedisServiceImpl::DispatchCommand` installs `NamespaceGuard ns_guard(ctx->ns_id)` before any
  command logic runs (`src/redis_service.cpp:5935-5956`), after re-validating the connection's
  cached namespace metadata (see §5).
- The scoping hook is the `EloqKey` constructor itself: `EloqKey(std::string_view)` routes through
  `CreateEloqStringFromNamespace`, which prepends the current prefix via `ComposeNamespaceKey`
  (`include/eloqkv_key.h:55-58`, `include/namespace/context.h:18-50`). For `default`/empty the key
  is unchanged. Since every command parser builds keys with this constructor (333 call sites in
  `src/redis_command.cpp` vs. 2 deliberate `EloqKey::Raw` uses), every command is scoped without
  per-command code. `EloqKey::Raw` (`include/eloqkv_key.h:72-80`) is the explicit bypass used only
  for already-composed or metadata keys (namespace module, GC, DBSIZE range bounds).
- Table routing: `RedisServiceImpl::RedisTableName(db_id)` returns `data_table_<db_id>` when the
  current prefix is empty/`default`, otherwise `ns_data_0` (`src/redis_service.cpp:5863-5870`).
- Cross-namespace access is prevented because clients never supply the prefix — it is derived from
  the token they authenticated with, and `:`-free b255 encoding means no crafted user key can
  collide with another tenant's prefix. Range operations (SCAN/KEYS/DBSIZE) are clamped to
  `[prefix, MakePrefixNext(prefix))` (`include/namespace/prefix.h:56-74`).

An important invariant: **the prefix is baked into the key bytes when the command is parsed**, so
queued `MULTI` commands, blocking commands, and engine-side retries keep their scope even if they
later execute on a different bthread.

## 3. Lifecycle: create / authenticate / use / drop

Management is exposed as a `NAMESPACE` command (handler registered at
`src/redis_service.cpp:1394-1396`; parsing at `src/redis_command.cpp:10600-10644`):
`NAMESPACE CURRENT | GET <name|*> | ADD <name> | REFRESH <name> | DEL <name>`.

Management preconditions (`NamespaceCommand::Execute`, `src/redis_command.cpp:1540-1572`): all
subcommands except `CURRENT` require (a) cluster mode disabled, (b) `requirepass` configured, and
(c) the caller authenticated with `requirepass` in the `default` namespace — i.e. only the admin
manages namespaces.

- **Create** (`ADD`): generates a random token (re-rolled while it equals `requirepass`,
  `src/redis_command.cpp:1619-1623`) and calls `NamespaceManager::Add` →
  `NamespaceStorage::Add` (`src/namespace/storage.cpp:150-326`). Inside **one engine transaction**
  (`RepeatableRead` + `Locking`, committed with `txservice::CommitTx`) it checks name and token
  uniqueness, allocates `next_id`, and writes five records to `__ns_0`:

  | key in `__ns_0`             | value                | purpose                          |
  |-----------------------------|----------------------|----------------------------------|
  | `n:<name>`                  | raw 16-byte token    | name → token                     |
  | `t:<raw token>`             | `b255(id)`           | token → encoded id               |
  | `i:<b255(id)>`              | name                 | id → name                        |
  | `e:<b255(id)>`              | epoch (decimal text) | current epoch, starts at `"1"`   |
  | `next_id`                   | next counter value   | id allocator                     |

  (prefix constants at `include/namespace/storage.h:13-20`). The reply is the token in base64url
  (22 chars). These ops run through `RedisServiceImpl::ExecuteNamespaceTxRequest`
  (`src/redis_service.cpp:1201-1217`), a thin typed dispatcher over the normal engine
  `TxRequest` path — so namespace metadata gets the engine's atomicity, replication, and
  persistence for free (see `data_substrate/docs/04-transaction-execution.md`).
- **Authenticate**: `AuthCommand::Execute` (`src/redis_command.cpp:1495-1525`) first interprets the
  password as a token (`NamespaceToken::FromBase64Url`); if
  `NamespaceManager::GetMetadataByToken` resolves it, the connection binds to that namespace
  (`ctx->ns`, `ctx->ns_id`, `ctx->ns_meta`). Otherwise the password is compared with `requirepass`
  for default-namespace access. `GetMetadataByToken` consults an RCU-cached
  `token → NamespaceMetadata` map and falls back to a `__ns_0` lookup
  (`src/namespace/manager.cpp:221-266`; RCU container in `include/rcu.h`).
- **Use**: every dispatched command recomputes `ctx->ns_id` from the live metadata epoch and
  installs the guard (§5).
- **Rotate** (`REFRESH`): issues a new token for the namespace inside one engine transaction,
  deleting the old `t:` record and keeping the id/epoch (`src/namespace/storage.cpp:328-567`);
  the manager cache entry is invalidated (`src/namespace/manager.cpp:153-165`).
- **Drop** (`DEL`): in one engine transaction, reads token/id/epoch, writes a GC record
  `g:<b255(id)>:<epoch-decimal>` = `"1"`, and deletes the `n:`, `t:`, `i:`, `e:` records
  (`src/namespace/storage.cpp:569-745`). Data keys in `ns_data_0` are *not* touched here — the
  commit makes them unreachable (token gone), and the GC record schedules asynchronous physical
  deletion. Because record + tombstones commit atomically, a crash either leaves the namespace
  intact or leaves a durable GC record.
- **Flush** (`FLUSHDB`/`FLUSHALL` inside a namespace — both take the same path,
  `src/redis_service.cpp:2907-2918`): bumps `e:<id>` to `epoch+1` and writes the GC record for the
  *old* epoch in the same transaction (`src/redis_service.cpp:2757-2821`), then updates the cached
  atomic epoch. All existing keys instantly become invisible (new prefix), making flush O(1) for
  the client; the old-epoch keys are GC'd later.

## 4. Garbage collection of dropped/flushed prefixes

`NamespaceGc` (`include/namespace/gc.h`, `src/namespace/gc.cpp`) is one background bthread started
after service init and joined on shutdown (`src/redis_service.cpp:762-763,782`;
`gc.cpp:18-33`). It must be stopped and joined before `DataSubstrate::Shutdown()` because an
in-progress GC iteration may be waiting for engine read, scan, or delete requests to complete.

- **Trigger/scan**: the daemon loops while `!server_->IsStopping()`, range-scanning `__ns_0` over
  `["g:", "g;")` for GC records (`ScanGCRecords`, `gc.cpp:107-218`) with a `ReadCommitted` +
  `OccRead` transaction that is aborted afterwards (read-only). If none exist it sleeps 30 s in
  100 ms slices so shutdown stays responsive (`gc.cpp:53-64`).
- **Deletion path**: for each record it reconstructs the dead data prefix
  `MakePrefix(b255_id, epoch)` (`gc.cpp:73-93`, safe to parse on `:` because b255 output never
  contains it) and calls `CleanPrefixKeys` (`gc.cpp:220-411`): repeatedly (1) scan
  `[prefix, prefixNext)` in `ns_data_0` with a read-only tx, (2) delete every found key with
  `DelCommand`s inside a single `RepeatableRead`+`Locking` engine transaction, (3) sleep and
  rescan, until a scan round finds zero keys. Deletion goes through the normal engine write path —
  no direct KV-store calls — so checkpointing propagates the deletes to the data store
  (see `data_substrate/docs/07-durability-and-recovery.md` / `09-store-handler.md`).
- **Throttling**: 5 ms between successful delete rounds, 50 ms after a failed one
  (`gc.cpp:409`), 100 ms backoff on scan/tx-creation errors, 10 ms between GC records
  (`gc.cpp:101`), and `IsStopping()` checks at every loop boundary.
- **Crash safety / idempotency**: the GC record is committed atomically with the epoch bump or
  metadata deletion, so it survives crashes and is re-discovered on restart; cleanup is pure
  range-delete and safe to repeat. The record itself is deleted (own small transaction,
  `DeleteGCRecord`, `gc.cpp:413-439`) only after a fully empty scan; malformed records are dropped
  defensively (`gc.cpp:74-90`).

## 5. Connection binding & command interactions

Per-connection state lives in `RedisConnectionContext`
(`include/redis_connection_context.h:122-124`): `ns` (name, default `"default"`), `ns_id` (the
composed key prefix; empty for default), and `ns_meta` (shared `NamespaceMetadata` with
`encoded_id` and an atomic `epoch`, `include/namespace/manager.h:17-23`).

- **Entry**: only via `AUTH <token>` (or `AUTH <user> <token>`; the username is ignored by
  `AuthCommand::Execute`). There is no `HELLO AUTH` path — EloqKV has no HELLO handler; `hello`,
  `auth`, `quit`, `reset`, plus the special-case `NAMESPACE CURRENT`, are the only commands allowed
  pre-auth (`AuthRequired`, `src/redis_service.cpp:5877-5915`). Namespace *management*
  (`NAMESPACE ADD`/`REFRESH`/`DEL`) is refused when `requirepass` is empty
  (`src/redis_command.cpp:1558-1562`), so namespace tokens can only be created on a
  password-protected server. Authentication itself is token-based; note that only the 2-argument
  `AUTH` form errors on an empty `requirepass` (`src/redis_command.cpp:10558-10567`) — the
  3-argument `AUTH <user> <token>` form does not.
- **Per-command re-validation** (`src/redis_service.cpp:5935-5956`): if `ctx->ns_meta` is set,
  dispatch re-fetches the live metadata by token and requires pointer equality with the cached
  object; on success it recomputes `ctx->ns_id` from the current epoch (so another connection's
  `FLUSHDB` takes effect immediately via the shared atomic), otherwise it resets the connection
  to the default namespace. Then the `NamespaceGuard` scopes everything the
  command does.
- **MULTI / Lua**: both run inside `DispatchCommand` under the guard
  (`src/redis_service.cpp:5999-6008`), and keys are prefixed at parse time, so queued transactions
  and `redis.call` from scripts are scoped correctly.
- **SCAN / KEYS / DBSIZE**: these manage prefixes explicitly. The scan path disables the ambient
  guard (`NamespaceGuard ns_guard("")`, `src/redis_service.cpp:5005`), composes
  `[ns_id ⊕ pattern-prefix, next)` bounds itself, pushes `ComposeNamespaceKey(ns, pattern)` down as
  the engine-side filter, and strips `ns_id` from returned keys before matching/replying
  (`src/redis_service.cpp:5002-5062,5237-5240,5329-5334`). `DBSIZE` counts a prefix-bounded scan
  with `EloqKey::Raw` bounds (`src/redis_command.cpp:2224-2270`).
- **SELECT** is forbidden inside a namespace (`src/redis_command.cpp:1736-1743`); `db_id` is
  ignored by table routing anyway for non-default namespaces.

## 6. Gotchas & invariants (verified unless marked inference)

1. **Tokens are bearer credentials stored in plaintext.** 16 random bytes (OpenSSL `RAND_bytes`,
   UUIDv4-formatted; falls back to `std::random_device` if OpenSSL fails,
   `src/namespace/token.cpp:11-30`) stored raw in `__ns_0` and listable by the admin via
   `NAMESPACE GET *`. A token equal to `requirepass` is rejected/re-rolled at every layer
   (`src/namespace/storage.cpp:69-72,156-159,334-337`; `src/redis_command.cpp:1620-1623`) so the
   admin password can never resolve as a namespace token.
2. **Cluster mode**: namespace management is refused when `FLAGS_cluster_mode` is set
   (`src/redis_command.cpp:1550-1556`). Inference: this is because the `NamespaceManager` RCU cache
   is per-process and has no cross-node invalidation.
3. **GC vs. in-flight writes**: a command parsed before a concurrent flush/drop carries the
   old-epoch prefix and may commit after the epoch bump; it is invisible to the namespace and is
   normally mopped up because GC rescans until empty. Inference: a write landing *after* GC's final
   empty scan (record already deleted) would leak an invisible orphan key in `ns_data_0`.
4. **Epoch-in-prefix makes flush O(1) but defers space reclamation** to the GC daemon's paced
   range-deletes; large namespaces are deleted in scan-sized batches, each batch one engine
   transaction (`src/namespace/gc.cpp:355-403`).
5. **`MemoryNamespaceStorage`** (`src/namespace/manager.cpp:13-133`) is an in-memory
   `INamespaceStorage` with no GC; no production call site constructs it (the service always wires
   `NamespaceStorage`, `src/redis_service.cpp:213`). Note it stores `b255prefix(id)` (with trailing
   `:`) as the id where the persistent storage stores bare `b255e(id)`
   (`manager.cpp:32` vs. `storage.cpp:243`) — harmless today but inconsistent if ever swapped in.
6. **Default namespace fast path is zero-cost**: `ComposeNamespaceKey` returns the key untouched
   for `default`/empty (`include/namespace/context.h:18-30`), so pre-namespace deployments see no
   key-format change and no overhead beyond the per-dispatch guard swap.
