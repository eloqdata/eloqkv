# 05 — Namespace Isolation and Lifecycle

EloqKV namespaces provide token-authenticated key-space isolation without
creating a table per tenant. Namespace metadata is transactional engine data;
tenant values share one table and are isolated by a physical key prefix.

## Data model

The implicit `default` namespace keeps the ordinary per-database
`data_table_<n>` layout. Every non-default namespace uses the shared
`ns_data_0` table and has one logical database; `SELECT` is rejected after a
client enters such a namespace.

Namespace metadata lives in the prebuilt `__ns_0` table. It records the
relationships among namespace name, bearer token, numeric id, and current
epoch, plus durable garbage-collection markers. Metadata changes use normal
Data Substrate transactions, so creation, token rotation, deletion, and epoch
changes have the engine's atomicity and durability.

A tenant data prefix is derived from the encoded namespace id and epoch:

```text
encoded_namespace_id ":" encoded_epoch ":" user_key
```

The base-255 encoding excludes the delimiter, making the two components
unambiguous. Prefixes have a computable exclusive upper bound, which lets scan
and garbage-collection operations stay inside one namespace range.

## Request scoping

`AUTH` first attempts to resolve the supplied password as a namespace token.
On success, the connection stores the namespace metadata object and its current
prefix. The administrative password continues to authenticate the default
namespace.

Before each command, `DispatchCommand` resolves the token against the live
metadata cache. A valid binding refreshes the prefix from the current epoch; a
deleted or rotated binding is cleared. `NamespaceGuard` then installs that
prefix in bthread-local context for the duration of dispatch.

Normal `EloqKey` construction prepends the current prefix. This makes tenant
scope part of the physical key at parse time, so queued `MULTI` commands,
blocking requests, remote forwarding, and transaction retries retain their
original scope. Internal paths that already hold physical or metadata keys use
`EloqKey::Raw` explicitly.

`RedisTableName` selects `ns_data_0` whenever a non-default prefix is active.
SCAN, KEYS, and DBSIZE additionally use prefix-bounded ranges and remove the
physical prefix before returning user-visible keys.

## Namespace lifecycle

Namespace management is available only to an authenticated default-namespace
administrator when a server password is configured, and it is disabled in
cluster mode.

- **Create:** allocate an id, generate a token, initialize the epoch, and write
  all lookup directions in one transaction.
- **Authenticate/use:** resolve the token through the manager's RCU cache,
  falling back to transactional metadata reads, then bind the connection.
- **Refresh:** replace the bearer token while retaining namespace id and epoch;
  invalidate the cached old binding.
- **Delete:** atomically remove the live metadata and write a GC marker for the
  current physical prefix. Data becomes unreachable at commit without waiting
  for physical deletion.
- **Flush:** atomically increment the epoch and write a GC marker for the prior
  prefix. Both `FLUSHDB` and `FLUSHALL` take this path inside a non-default
  namespace, making the logical flush independent of namespace size.

The id keeps data keys independent of a renameable/display name, while the
epoch makes logical invalidation an atomic metadata operation. Space
reclamation is intentionally asynchronous.

## Garbage collection

One `NamespaceGc` bthread is started with the Redis service and stopped before
the engine shuts down. It scans `__ns_0` for durable GC markers, derives the
dead prefix, and repeatedly scans and deletes matching keys from `ns_data_0`
through normal engine transactions. It removes the marker only after a scan
finds the prefix empty.

The marker is committed in the same transaction that deletes metadata or
advances the epoch. A crash therefore leaves either the old namespace visible
or a durable cleanup obligation. Range deletion is idempotent, and restart
rediscovers unfinished work. Using engine transactions for cleanup ensures the
deletions reach WAL/checkpoint/store paths like ordinary writes.

## Security and consistency boundaries

- Namespace tokens are bearer credentials; possession is sufficient to enter
  the namespace. Administrative operations require the configured server
  password in the default namespace.
- Isolation depends on applying `NamespaceGuard` before any user-key
  construction and on using raw-key construction only for already-scoped
  internal data.
- A flush or delete changes reachability at metadata commit. Old physical keys
  may remain until GC completes but cannot be addressed through the new epoch
  or a removed token.
- Per-command metadata refresh makes an epoch change visible to existing
  connections without rebinding them manually.
- The GC service must stop before Data Substrate because it may have outstanding
  engine requests.

## Source map

| Claim | Repository source |
|---|---|
| Namespace tables and per-command binding refresh | `src/redis_service.cpp`, `include/redis_service.h` |
| Prefix format, delimiter-safe encoding, and request-scoped guard | `include/namespace/prefix.h`, `include/namespace/context.h`, `src/namespace/context.cpp`, `src/b255.cpp` |
| Automatic key prefixing and raw-key escape hatch | `include/eloqkv_key.h` |
| Token authentication and namespace command policy | `src/redis_command.cpp`, `include/redis_command.h`, `src/namespace/token.cpp` |
| Metadata cache ownership and invalidation | `include/namespace/manager.h`, `src/namespace/manager.cpp`, `include/rcu.h` |
| Transactional metadata create/refresh/delete operations and GC markers | `include/namespace/storage.h`, `src/namespace/storage.cpp` |
| Epoch-based namespace flush | `src/redis_service.cpp` |
| Prefix-bounded SCAN/KEYS/DBSIZE behavior | `src/redis_service.cpp`, `src/redis_command.cpp`, `include/namespace/prefix.h` |
| GC lifecycle, repeated range cleanup, and marker deletion | `include/namespace/gc.h`, `src/namespace/gc.cpp` |
