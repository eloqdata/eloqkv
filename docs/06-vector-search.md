# 06 — Vector Search (EloqVec)

EloqVec is an optional, compile-time-gated vector-search subsystem. It exposes
HNSW index lifecycle, mutation, and filtered nearest-neighbor search commands.
The serving index is process-local memory; durable metadata and mutation logs
are Data Substrate records; periodic index snapshots are files that can also be
copied to object storage.

Vector indexes are global to the EloqKV deployment. Their names and vector ids
are not qualified by Redis database or namespace, and vector records are not
linked to ordinary Redis keys.

## State and ownership

Each index has three state layers:

| State | Owner | Role |
|---|---|---|
| Index metadata | Internal engine table `__vector_index_meta_table` | Configuration, metadata schema, persistence policy, and authoritative snapshot reference |
| Sharded mutation log | Records in the same internal engine table | Durable ordered INSERT/UPDATE/DELETE deltas used after the referenced snapshot |
| HNSW index and snapshot file | `VectorHandler` process cache and local filesystem; optional object-store copy | Search-serving state and a compact rebuild base |

Metadata and log records use normal engine transactions and therefore inherit
WAL, replication, checkpoint, and recovery behavior. The HNSW instance is held
in `VectorHandler`'s process-local cache. A snapshot file is not independently
authoritative: the committed metadata record selects the snapshot that may be
loaded.

Vector ids include the encoded typed metadata used for predicate evaluation.
The schema fixes field order and types when metadata is encoded. Search filters
are parsed into a predicate tree and evaluated during the underlying filtered
HNSW traversal; results contain vector ids and distances rather than Redis key
payloads.

## Execution model

Redis handlers submit vector work to a dedicated `TxWorkerPool` backed by
native worker threads. The calling bthread waits for completion without running
HNSW work on a `TxProcessor` or brpc worker. Worker threads create and drive the
transactions used by `VectorHandler`.

Each vector command owns an internal transaction. Vector commands do not join
the connection's `MULTI`/`EXEC` or `BEGIN` transaction and are not exposed
through the Lua command bridge. HNSW operations use the index implementation's
lock and usearch concurrency support; initialization and snapshot save require
exclusive access to the local instance.

## Write and query flows

A mutation reads the current metadata, obtains or lazily builds the local
index, validates/encodes vector metadata, and appends a delta to one log shard
inside its transaction. It then mutates the local HNSW instance and commits.
If commit fails, the handler compensates the local in-memory mutation using the
pre-operation value where necessary.

The log shard's metadata record is read for write while appending, which
serializes writers assigned to that shard. Batch insertion groups entries by
log shard and acquires shard work in a stable order.

A search reads index metadata, obtains or rebuilds the local index, parses an
optional predicate against the configured schema, and executes HNSW search.
The query transaction protects the metadata/rebuild work; the search itself is
served from the process-local index.

## Snapshot and rebuild lifecycle

An index is rebuilt lazily when a process first accesses it and no cached
instance exists:

1. Deserialize the committed index metadata.
2. Load the referenced local snapshot, or download it through `CloudManager`
   when object storage is configured.
3. Initialize the HNSW instance from its configuration and snapshot.
4. Scan and replay the sharded durable log.
5. Publish the initialized instance in the process cache.

Snapshot persistence runs as a separate transaction. It acquires write intent
on the index metadata and every log shard, truncates the captured log entries,
saves the local HNSW instance, updates the snapshot reference, optionally
uploads the file, and commits the metadata/log changes. Holding the log write
intents prevents a concurrent writer from slipping between the captured index
state and log truncation. An older snapshot is deleted only after the new
metadata commit; file cleanup is best-effort because filesystem/object-store
effects are outside the engine transaction.

Automatic persistence deduplicates queued work per index. Manual and automatic
requests use the same snapshot transaction and committed metadata boundary.

## Consistency boundaries

- Durable metadata and log changes are transactional; the HNSW cache and files
  are not engine records.
- A local HNSW mutation occurs before transaction commit and is compensated on
  failure. Concurrent local search can therefore observe a transient mutation
  that later rolls back.
- Each process has its own cached index and does not continuously tail deltas
  applied by other processes. A rebuild converges from the committed snapshot
  and log, but cross-node searches are not a single coherent read view.
- Every operation rereads metadata, so a committed index drop is detected even
  if a stale cache entry still exists locally.
- Snapshot metadata, not file presence, determines which snapshot participates
  in recovery.

## Source map

| Claim | Repository source |
|---|---|
| Optional build and Redis command integration | `CMakeLists.txt`, `src/redis_service.cpp`, `src/redis_command.cpp` |
| Vector subsystem ownership, transaction flows, cache, snapshot, and rebuild lifecycle | `include/vector/vector_handler.h`, `src/vector/vector_handler.cpp` |
| Internal metadata table, index configuration, vector ids, and typed record metadata | `include/vector/vector_type.h`, `src/vector/vector_type.cpp` |
| Sharded durable mutation log | `include/vector/log_object.h`, `src/vector/log_object.cpp`, `src/vector/tests/LogObject-Test.cpp` |
| HNSW concurrency, save/load, mutation, and filtered search boundary | `include/vector/vector_index.h`, `include/vector/hnsw_vector_index.h`, `src/vector/hnsw_vector_index.cpp` |
| Predicate parsing and evaluation | `include/vector/predicate.h`, `src/vector/predicate.cpp` |
| Optional object-store file transfer | `include/vector/cloud_manager.h`, `src/vector/cloud_manager.cpp` |
| Index lifecycle, recovery, filtering, and concurrency behavior | `src/vector/tests/VectorHandler-Test.cpp`, `src/vector/tests/VectorCache_HNSW-Test.cpp` |
