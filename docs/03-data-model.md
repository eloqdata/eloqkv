# 03 — Data Model and Engine Plug-in

EloqKV exposes Redis data through the Data Substrate object-command model. The
integration consists of a physical key type (`EloqKey`), five Redis object
families, transactional command objects, and a catalog factory that creates the
engine's object concurrency-control maps and scanners.

## Tables and keys

The default namespace maps each configured Redis database to one prebuilt
primary table named `data_table_<n>`. `SELECT` changes the table selected by the
connection; no DDL is issued for Redis databases. Namespace metadata and
non-default namespace data use separate prebuilt tables described in
[05-namespaces.md](05-namespaces.md).

`EloqKey` owns the physical key bytes used by the object table. Normal key
construction applies the active namespace prefix, while `EloqKey::Raw` is the
explicit path for bytes that are already physical. Hashing follows Redis CRC16
hash-tag semantics, so keys sharing a `{tag}` are assigned together. The
engine/message encoding of a key is distinct from the raw key bytes used by the
KV-store boundary; both formats are compatibility-sensitive.

## Redis objects

Every stored value is a `RedisEloqObject`, with concrete implementations for
string, list, hash, set, and sorted set. Each family also has a TTL-bearing
variant. The TTL variant reports the same logical Redis type as its non-TTL
counterpart, so type checks and commands do not need a second semantic type.

The first byte of the serialized object identifies the concrete object class;
the remaining payload contains that object's logical value and, for TTL
variants, its expiration. `RedisObjectType` values are therefore a durable
format contract and must not be renumbered. `DeserializeObject` uses that tag
to recreate the concrete `TxRecord` when the engine loads a record.

Objects maintain the serialized-size information needed to reject a mutation
whose post-image exceeds EloqKV's object limit. Empty collections are not kept
as stored objects: a successful command that produces an empty value returns a
deletion result to the engine.

## Command execution contract

Transactional Redis commands implement `txservice::TxCommand` and carry both
the operation's parameters and its result. Their lifecycle separates decision
from mutation:

1. The service parses arguments, constructs the physical key and command, and
   submits them in a transaction request.
2. On the owner shard, `ExecuteOn` reads the current object, checks types and
   preconditions, and records the result without mutating the object.
3. If the transaction commits, `CommitOn` applies the chosen mutation. It may
   replace the concrete object, change TTL representation, or return `nullptr`
   to delete the key.
4. `OutputResult` renders the stored result through the service's output
   abstraction.

This split lets concurrency control validate and buffer commands before their
effects become visible. Read-only, overwrite, existence-gated, delete, and TTL
properties are communicated through the generic `TxCommand` predicates rather
than through Redis-specific engine logic.

Mutating commands serialize to a command image whose leading
`RedisCommandType` is used by WAL recovery, standby application, remote
execution, and migration. These numeric values are explicitly assigned and
append-only. Replay consumers apply the deserialized command's commit phase;
therefore any choice made during owner-side execution that affects the
mutation must be represented in the image delivered to replay consumers. The
replay tests exercise this determinism independently of client reply behavior.

## Multi-key commands

`RedisMultiObjectCommand` groups keys and child commands into one or more
stages. Keys within a stage are submitted together; the command interprets the
stage results before advancing. This supports single-stage multi-key reads and
writes, read/compute/write operations, moves, and blocking operations without
embedding those algorithms in the service dispatcher.

All stages share one `TransactionExecution`. Intermediate requests suppress
auto-commit, and only the final outcome commits or aborts the transaction. The
engine can coordinate keys across buckets and node groups; EloqKV does not
impose a service-layer `CROSSSLOT` restriction on this command model.

## TTL lifecycle

Expiration is stored as an absolute epoch-millisecond timestamp in the
TTL-bearing object. TTL-setting commands either update that representation or
replace the object with its TTL/non-TTL counterpart.

Expiration is enforced lazily by the engine's object map. Reads treat an
expired record as absent. A mutation against an expired record first retires
the old value and then applies the command to a fresh logical object, keeping
delete-and-recreate ordering transactional. Physical reclamation is left to
the engine's cache/store cleanup paths.

TTL-only mutations require special replay handling: replay cannot reconstruct
the value by changing only a timestamp when the prior object is unavailable.
Such a command supplies a `RecoverObjectCommand` containing the complete
post-operation value needed for independent WAL or standby application. TTL,
expired-state, and recovery-image information must survive remote-owner
execution as well as the local path.

## Catalog boundary

`RedisCatalogFactory` creates `RedisTableSchema`, the primary
`ObjectCcMap<EloqKey, RedisEloqObject>`, hash-partition scanners, range-map
plumbing, keys, and empty records for `TableEngine::EloqKv` tables.
`RedisTableSchema::CreateTxCommand` is the decoder for serialized command
images.

Redis tables have no secondary-index schema, auto-increment value, or table
statistics in this plug-in. SCAN/KEYS use the primary hash-table scanner;
secondary and ordered-range scanners are not provided.

## Cross-module invariants

- `RedisObjectType` and `RedisCommandType` numbers are persisted/wire-visible;
  existing values are never renumbered.
- `ExecuteOn` decides and records a result; `CommitOn` performs the mutation.
- A command image must contain every owner-side choice needed for deterministic
  commit-only replay.
- Normal `EloqKey` construction applies the active namespace. Code handling an
  already-composed key must use the raw factory explicitly.
- Multi-stage operations retain one transaction until their final stage.
- TTL-only recovery remains independent of the previous KV-store image.

## Source map

| Claim | Repository source |
|---|---|
| Prebuilt Redis database tables and catalog registration | `src/redis_service.cpp`, `include/redis_service.h` |
| Physical key ownership, namespace application, hashing, and serialization | `include/eloqkv_key.h`, `src/eloqkv_key.cpp`, `include/namespace/context.h` |
| Object type tags and concrete record reconstruction | `include/redis_object.h`, `src/redis_object.cpp` |
| String/list/hash/set/zset objects, TTL variants, and serialized-size accounting | `include/redis_string_object.h`, `include/redis_list_object.h`, `include/redis_hash_object.h`, `include/redis_set_object.h`, `include/redis_zset_object.h`, `src/redis_string_object.cpp`, `src/redis_list_object.cpp`, `src/redis_hash_object.cpp`, `src/redis_set_object.cpp`, `src/redis_zset_object.cpp`, `tests/unit/eloq/object_serialize_deserialize_test.cpp` |
| Command execution, commit, result, and multi-object contracts | `include/redis_command.h`, `src/redis_command.cpp`, `src/redis_service.cpp` |
| Stable command-image numbering and replay decoding | `include/redis_object.h`, `include/redis_command.h`, `src/eloqkv_catalog_factory.cpp`, `tests/unit/eloq/command_replay_test.cpp` |
| Expiration and full-object TTL recovery | `include/redis_command.h`, `src/redis_command.cpp`, `data_substrate/tx_service/include/cc/object_cc_map.h`, `tests/unit/eloq/expire.tcl` |
| EloqKV table schema, object map, and primary scanner | `include/eloqkv_catalog_factory.h`, `src/eloqkv_catalog_factory.cpp` |
