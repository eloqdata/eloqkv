# 07 — Redis-Format Interop and Offline Export

EloqKV's durability comes from Data Substrate WAL, recovery, checkpointing, and
the configured KV store. Redis RDB/AOF support in this repository is an
interoperability boundary, not the server's native durability mechanism.

The layer provides two forms of interop:

- online `DUMP`/`RESTORE` conversion for one Redis key; and
- offline export from a checkpointed RocksDB-backed store to an RDB file or
  RESP command streams.

Whole-server Redis `SAVE`, `BGSAVE`, and AOF rewrite are not part of this
architecture. Backup consistency is established by the underlying store or its
snapshot mechanism before an offline exporter reads it.

## Online DUMP and RESTORE

`DUMP` runs as a read-only object command. It converts an EloqKV string, list,
set, hash, or sorted set into a Redis RDB object payload and appends a
version-10 Redis DUMP footer with CRC64. The emitted payload uses
ordinary Redis encodings rather than preserving EloqKV's in-memory
representation. TTL is not embedded in the object payload; `RESTORE` receives
expiration separately.

`RESTORE` verifies the footer before decoding. It accepts Redis payload
versions 4 and 10 and retains version-zero decoding for legacy EloqKV-native
payloads. Redis encodings for the five supported logical types are normalized
into EloqKV's object serialization; unsupported object families are rejected.
The decoder bounds expanded strings and
collection sizes and validates compact-container structure before constructing
an object.

After conversion, `RESTORE` is an ordinary transactional overwrite/existence-
gated `RedisCommand`. `REPLACE` controls whether an existing key may be
overwritten, and the supplied relative or absolute TTL selects the TTL-bearing
object representation at commit. Database and namespace routing are inherited
from normal command dispatch.

The compatibility contract is logical data, not physical encoding identity:
compact Redis containers decode to EloqKV objects, and a later `DUMP` may emit
a different valid Redis representation of the same value.

## Offline exporters

Exporter targets depend on the configured storage build:

| Build | Exporters | Input |
|---|---|---|
| `WITH_DATA_STORE=ROCKSDB` | `eloqkv_to_rdb`, `eloqkv_to_aof` | Local embedded RocksDB database |
| `WITH_DATA_STORE=ELOQDSS_ROCKSDB_CLOUD_S3` | `eloqkv_to_rdb` | Named RocksDB-Cloud snapshot for each DSS shard |
| Other store builds | None | — |

Both exporters read the checkpoint/store representation directly; they do not
replay the WAL. A committed write that has not reached the selected checkpoint
or cloud snapshot is therefore outside the export. The local embedded-RocksDB
path opens the database itself, while the cloud RDB path opens the named
snapshot branches and scans the DSS composite key/value format.

The embedded-RocksDB readers enumerate the fixed `data_table_0` through
`data_table_15` set, while the cloud reader accepts DSS table names with the
`eloqkv_data_table_<n>` prefix. Neither path exports the namespace metadata
table or the shared `ns_data_0` tenant table, so offline export is not a
complete namespace backup.

### RDB output

`eloqkv_to_rdb` writes a Redis RDB file containing database selection,
expiration, key, and one of the five supported logical value types. The local
and cloud readers decode their different KV-store record envelopes into the
same `RedisEloqObject` model before RDB encoding. The file is terminated with a
Redis-compatible CRC64.

The cloud reader uses the provided snapshot name for each shard, so its input
view is fixed by the store's snapshot contract. It filters DSS records to
EloqKV data-table names and reconstructs the Redis database number from the
table name.

### AOF output

`eloqkv_to_aof` is available only for the local embedded-RocksDB build. It
converts each logical object into Redis commands: scalar values become one
write, collections become element writes, and TTL becomes a following expiry
command. Worker outputs are independent RESP command streams and each emits
its own database selections where needed.

AOF export preserves logical values and expiration but is not size-equivalent
to the store: collection values expand into command-per-element streams.

## Cross-module invariants

- RDB/AOF interop is not EloqKV's recovery path; native recovery belongs to
  Data Substrate.
- `DUMP`/`RESTORE` preserve supported logical Redis values, not compact encoding
  identity.
- RESTORE treats payload bytes as untrusted and validates version, checksum,
  container structure, and decode bounds before commit.
- Offline exports contain only state visible in their selected checkpoint or
  snapshot, never newer WAL-only commits.
- Exporter build availability and input record format must match the configured
  storage backend.
- Offline exporters omit non-default namespace data and metadata.

## Source map

| Claim | Repository source |
|---|---|
| DUMP/RESTORE transaction semantics, footer verification, TTL, and `REPLACE` | `include/redis_command.h`, `src/redis_command.cpp`, `tests/unit/eloq/dump.tcl` |
| Redis object-payload decoding, encoding, and validation | `include/redis_rdb_restore.h`, `src/redis_rdb_restore.cpp` |
| Exporter build matrix | `CMakeLists.txt` |
| Local/cloud RDB inputs, record decoding, data-table filtering, and RDB output | `src/tools/eloqkv2rdb/eloqkv2rdb.cpp` |
| Embedded-RocksDB AOF conversion and worker streams | `src/tools/eloqkv2aof/eloqkv2aof.cpp` |
| Native WAL/checkpoint/store durability boundary | `data_substrate/docs/07-durability-and-recovery.md`, `data_substrate/docs/09-store-handler.md`, `data_substrate/tx_service/include/checkpointer.h` |
