# Paged Objects: Splitting Large Collections into Independently Managed Pages

> **Status: DESIGN — not implemented.** This doc specifies planned work. Statements about *current* behavior cite code and are verifiable today; statements about the *paged* design are proposals. When implementation lands, rewrite the proposal sections as descriptions and update `data_substrate/docs/` for the engine-side mechanisms (§13).

Today one Redis collection key is one `TxObject`, held whole in one `CcEntry`, serialized as one contiguous blob, and flushed as one KV row. That equivalence is the source of three scaling problems for large collections (§1). This design breaks it: a large collection becomes a small always-resident **metadata block** (kept in the `CcEntry` payload under the ordinary key) plus many fixed-size **pages** that are fetched, evicted, and flushed independently, each stored under a derived key. Concurrency control stays on the logical key. Both representations coexist; an object converts to paged form only when it grows past a threshold.

The design is presented **concretely for hash** (§4) but the format, key scheme, and engine hooks are deliberately **type-generic** (§3) so that set/list/zset are follow-on work rather than a rewrite. Siblings: [03-data-model.md](03-data-model.md) (current object/command model — read first), [02-command-processing.md](02-command-processing.md), [07-persistence-and-tools.md](07-persistence-and-tools.md). Engine background: `data_substrate/docs/03-concurrency-control.md`, `04-transaction-execution.md`, `07-durability-and-recovery.md`.

## 1. Problem

The current equivalence chain, with citations:

| Link | Where |
|---|---|
| hash key → one `absl::flat_hash_map<EloqString,EloqString>` | `include/redis_hash_object.h:235` |
| object → one contiguous serialized blob | `include/redis_hash_object.h:77-180` |
| `CcEntry` → one payload (`NonVersionedPayload::cur_payload_`) | `data_substrate/tx_service/include/cc/cc_entry.h` |
| one `CcEntry` → one `FlushRecord` → one KV row | `cc_entry.h:71` (`FlushRecord`), `cc_entry.h:1387` (`ExportForCkpt`), `include/store/data_store_handler.h:88` (`PutAll`) |
| cache miss → fetch + deserialize the **whole** record | `src/cc/cc_shard.cpp:2172` (`FetchRecord`), `include/cc/object_cc_map.h:2625` (`ObjectCcMap::BackFill`, the override object tables use) |
| eviction → whole `CcEntry`, only if clean and unlocked | `src/cc/cc_entry.cpp:104` (`IsFree`) |

Consequences:

1. **Cold-start latency.** A cache miss on a 1 GB hash reads and deserializes 1 GB before the command runs, even for `HGET k f`.
2. **Write amplification.** One `HSET` marks the whole object dirty; the next checkpoint rewrites the entire blob.
3. **Residency.** The object is all-or-nothing in memory. A hash with a small hot field set still costs its full size, and the shard cannot reclaim the cold remainder.

The existing `MAX_OBJECT_SIZE` = 256 MB cap (`src/redis_service.cpp:208`) exists *because* of the single-blob flush — the comment at `include/redis_hash_object.h:236-238` says so explicitly ("Limit the objects size so the persistent storage flush wouldn't fail"). Paging is what would eventually allow that cap to rise.

**Goals.** Point and bounded operations on a huge collection touch O(1) pages: they neither load nor flush nor retain the whole object. Cold pages are independently evictable. Write amplification is proportional to pages modified.

**Non-goals.** (a) Making full-scan commands (`HGETALL`/`HKEYS`/`HVALS`) work on objects larger than memory — like Redis, those materialize and may fail (deterministically, from the metadata-resident size bound — §3); `HSCAN` is the incremental path (§12). (b) MVCC/versioned pages — EloqKV object tables are non-versioned and stay so. (c) Changing the concurrency-control granularity: locking remains per logical key.

## 2. The Model

```text
   key "h"  ──►  CcEntry payload = METADATA BLOCK        (always resident while "h" is cached)
                 ├── format/version, TTL, logical count, logical bytes
                 ├── routing structure  (type-specific: directory / counts / separators)
                 └── per-page state: page id + buffer  (null buffer ⇒ not resident)
                                            │
                 pages (≈128 KB each) ──────┴──►  fetched on demand, evicted independently,
                                                  flushed individually under derived keys
```

- **Metadata block** is the `CcEntry` payload. It is what `FetchRecord`/`BackFill` load when the key is first touched, and it is small (§4 sizing).
- **Pages** are not `CcEntry`s and are not in any `CcMap`. They are owned by the metadata block via `shared_ptr`. Durably, each is its own KV row under a derived key (§5).
- **Concurrency control is unchanged**: the metadata `CcEntry`'s key lock governs the whole logical object.

**Central invariant — routing lives in the metadata, never in the pages.** No page may contain a pointer or reference to another page. Two independent reasons force this, and they reinforce each other:

1. **Eviction safety.** Pages are independently evictable, so any intra-page reference to another page could dangle.
2. **Fault-set predictability.** With routing entirely in the always-resident metadata, a command's page set follows from `(metadata, arguments)` rather than from chasing links through pages — which keeps the fault protocol cheap (§6) and lets it generalize to ordered types (§3). It is not universally one step: where the target is addressed by *data* rather than arguments — an out-of-line value's descriptor, a zset member's current score — discovery takes a further round (§6). What routing-in-metadata guarantees is that each round is a lookup, never a scan.

Page linkage is therefore by **page id / index recorded in the metadata**, never by in-page pointers.

## 3. Type-Generic Contract

The machinery is generic; each Redis type plugs in four things. Build hash first, but do not let hash assumptions leak into the generic half — the **on-disk format framing, page-key scheme, and engine hooks are the expensive-to-retrofit parts** (§5, §13).

**Generic (shared base class + engine):**

- page fetch, request parking, and resume (§6)
- per-page pin counters (§6)
- copy-on-write dirty payload (§7)
- partial eviction (§8)
- checkpoint fan-out and per-page clean-marking (§9)
- page-key encoding and co-location routing (§5)
- format/version framing and conversion protocol (§5, §11)

**Per-type plug-in:**

| Hook | Responsibility |
|---|---|
| routing metadata | the type's structure mapping logical position/identity → page id, plus its maintenance on insert/delete |
| page codec | serialize/deserialize one page's contents |
| fault-set computation | given a command and the metadata, the set of page ids the command will touch (read set ∪ write set) |
| split/merge policy | deterministic (§10). Splits are mandatory but allocate rather than fetch; merges are discretionary, so `CommitOn` merges only where the buddy is already resident and leaves the rest sparse (§6) |

Per-type routing designs (hash is specified in §4; the rest are sketches to validate the contract, not commitments):

| Type | Routing metadata | Fault set predictable from metadata? |
|---|---|---|
| hash | extendible-hash directory: entry → page id | **Yes** — `hash(field)` → directory entry → page |
| set | same as hash (member directory) | **Yes** |
| list | ordered page-id list + per-page element counts | **Yes** — prefix sum over counts gives the page for any index/range |
| zset | ordered page list + per-page `(score,member)` boundaries + counts, **plus a required paged member→score index** | **By rank/score: yes** (binary search the separators). **By member: via the index**, which is not optional — see below |

### Validation against the actual command set

Evaluated against the ~179 command registrations in EloqKV (`AddCommandHandler`, `src/redis_service.cpp`):

| Type | Verdict | One-shot fault set | Does not fit |
|---|---|---|---|
| **string** | **best fit** — routing is pure arithmetic, `page = offset / page_size`, needing no directory | `getrange`/`substr`, `setrange`, `append`, `getbit`/`setbit`, `bitcount`/`bitpos`, `bitfield`, `strlen` (metadata only) | `get`/`getset`/`getdel` are inherently whole-value |
| **list** | works | `lpush`/`rpush`(`x`), `lpop`/`rpop`/`lmpop` and blocking twins, `lindex`, `lrange`, `lset`, `ltrim`, `llen`, `lmove`/`rpoplpush` | `linsert BEFORE\|AFTER pivot`, `lrem`, `lpos` — search by *value*, so iterative |
| **set** | works, ≈ hash | `sadd`/`srem`/`sismember`/`smismember`/`smove`, `scard`, `sscan` | `srandmember`/`spop` (per-page counts); `sdiff`/`sinter`/`sunion` iterate one set wholly |
| **zset** | works, but **needs two directories** — most implementation work, so last | `zrange`/`zrevrange`, `zrangeby{score,lex}`, `zcount`, `zlexcount`, `zpopmin`/`zpopmax`/`zmpop`, `zremrangeby{rank,score,lex}`, `zcard` | `zadd`, `zscore`/`zmscore`, `zincrby`, `zrem`, `zrank`/`zrevrank` are member-addressed, so they route through the index |

**String is the strongest candidate, ahead of hash.** `append` to a 100 MB value dirties one page, and `set`/`setex`/`psetex` declare `IgnoreOldValue()`, so overwriting a huge value never fetches the old one at all. `incr`/`decr` act on numbers and are single-page by construction.

**Zset needs a structural decision.** The obstacle is a **data dependency**, not merely a missing index. Pages are ordered by score, so routing is *score → page*; but `ZADD key score member` must also remove the member's *existing* entry, whose page is identified by its **current** score — data the command does not carry. Score ordering cannot locate a member, so without an index the command must fault every page just to discover whether the member exists, on the most common zset write. (`GT`/`LT`/`NX`/`XX` sharpen this: the command's outcome itself depends on the old score.) Contrast `HSET h f v`, where `hash(f)` yields the page from an *argument* in one step, with no intervening data lookup — which is precisely why hash is one-shot.

Its member-addressed group therefore includes `zadd`, the hot write path, so the two access paths of today's dual representation (`std::set<ZNode>` by (score, field) **plus** `flat_hash_map` member→score) both have to survive paging. Putting a member index in the metadata is O(members) — roughly 100 MB of metadata for a 10 M-member zset — which destroys the sizing property of §4; omitting it makes every member op scan all pages. The workable option is a **paged member→score index as a second sub-structure**, with the metadata holding two directories — the index lives in *pages*, so the always-resident part stays at §4 sizing.

Its cost, though, is larger than it first appears, and **paging is what creates it**. Today's duality is nearly free because the member string is stored once and referenced twice: `z_hash_map_`'s keys are `string_view`s into the `ZNode` strings owned by `z_ordered_set_` ([03-data-model.md](03-data-model.md) §3), exactly as Redis shares one sds between its dict and skiplist. **Pages are self-contained byte buffers and cannot hold pointers into one another**, so each structure must carry its own copy — paging converts a shared reference into a duplicated string. For 10 M members with ~20 B names, ordered pages cost ~360 MB and a full-member index another ~360 MB: **2×**. Storing `hash64(member) → score` instead gives ~1.55×, at the price of verifying collisions against the ordered page — free for `zadd`/`zrem`/`zincrby`, which visit it anyway, but it makes `zscore` two rounds. The index cannot instead map `member → page_id`, which would be smaller, because page ids change on split and every split would rewrite ~900 scattered index entries; scores are stable under split, so `→ score` is the only split-friendly payload. On top of storage, `zadd`/`zrem`/`zincrby`/`zrank` each need a **two-round fault**.

**Why the index is not optional.** Without it there is no page to name: routing is *score → page*, and the score is exactly what a member lookup is trying to discover, so the only correct fault target becomes every page — ~11 K of them for 10 M members, roughly 1.4 GB of I/O per `ZADD` cold and an O(N) scan hot, against O(1) today. Faulting on demand performs that I/O faithfully rather than avoiding it; the coroutine addresses *how* to do I/O, not *how much*. The lookup also cannot be skipped (the reply counts *newly added* members, and `GT`/`LT`/`NX`/`XX` decide from the old score) nor deferred by inserting the new entry and cleaning up later, since the set would transiently hold the member twice and `ZRANGE` would return it twice.

**With the index, nothing is scanned.** `ZADD key 500 alice` touches exactly three pages: the index page, located by `hash("alice")` from the argument alone; then, once the index yields the old score, the ordered pages for the old and new scores. Two fault rounds, three pages, no search — and the two-round shape is handled by the ordinary Yield protocol, which is what it exists for.

**Today's code already splits along exactly this seam.** `ZAddCommand::ExecuteOn` (`src/redis_command.cpp:12723`) calls `RedisZsetObject::Execute(ZAddCommand&) const` (`src/redis_zset_object.cpp:414`), whose `ZAddIncr`/`ZAddLT`/`ZAddGT`/`ZAddNX`/`ZAddXX` filters consult **`z_hash_map_` alone** — the member→score index — and never touch the ordered set. `CommitZAdd` (`:494`) then does the structural work: re-filter, `z_hash_map_.find`, `z_ordered_set_.find(ZNode(member, old_score))`, erase, insert, re-emplace (`:537-576`) — note `:552` already constructs the ordered-set search key *from* the index result, which is precisely the index → old score → ordered page chain a paged zset needs.

Because `CommitOn` must not fault (§6), the paged `Execute(ZAddCommand&)` override carries both rounds: it faults the index pages for its own filtering, and then — beyond anything its own work requires — the ordered pages for each surviving element's old and new scores, so `CommitZAdd` finds them resident. The two-round shape is what the Yield protocol exists for; the override simply iterates until its set is closed.

Nothing here is a new burden on the command's logic: `CommitZAdd` is *already* independent of `ExecuteOn` by design, re-deriving its filtering from object state because standby apply and WAL recovery call it with no preceding `ExecuteOn` (the #509 invariant, documented at `:501-512`). That independence is also what lets the replay path work with no pre-fault step at all: `CommitZAdd` re-derives its own page set and may simply fault for it (§10).

**Assessment: zset is the most work to implement, not a different category.** A write touches ~3 pages across two sub-structures in 2 rounds, against 1 page in 1 round for hash — a constant factor, not a structural obstacle. Storage lands at ~1.55× raw on disk (~360 MB ordered + ~200 MB hashed index for 10 M members), which *improves* on today's monolithic zset: a `std::set<ZNode>` at ~72 B/node plus a `flat_hash_map<string_view,double>` at ~34 B/entry is roughly **2.9× raw and entirely resident**. The two structures share one object, one commit and one atomic checkpoint batch, so they cannot diverge, and every command routes. Zset should come **last** because it carries roughly double the implementation surface — two directories, two page codecs — not because it is expensive or risky. String, hash, set and list are simply easier.

**Per-page element counts are needed by three of the four types** — list for *every* positional operation, hash for `hrandfield`, set for `srandmember`/`spop` — which is why they are carried in the metadata rather than left in page headers (§4). Treating them as redundant with the header holds for hash alone, not for the general design.

**Multi-key commands are safe.** `sinterstore`, `zunionstore`, `lmove`, `smove`, `bitop` and `sort` fault several objects, and cannot deadlock doing so: a faulting command waits only on store I/O, which always completes — never on another transaction — whether it holds no locks (the speculative path) or retains one (§6). They do stack pins across objects, which is bounded memory pressure rather than a correctness issue.

Commands that are inherently whole-object regardless of paging: `get`/`getset`/`getdel`, `smembers`, `hgetall`/`hkeys`/`hvals`, `sort`, and `dump` — which must additionally reassemble a paged object into monolithic Redis-RDB form. Their oversize failure mode is deterministic and page-free: `logical_bytes_` lives in the always-resident metadata, so a whole-object command whose reply would exceed the configured bound errors out **before faulting a single page** — the same short-circuit shape as TTL expiry (§6) — rather than exhausting shard memory mid-materialization.

One category remains inherently unpredictable and falls back to iterative faulting (§6): **list value-search** — `LREM`, `LPOS`, `LINSERT BEFORE <value>` — where the target is found by scanning rather than routing. This is a correctness-preserving fallback, not a special case; the same Yield protocol handles it, just with more passes. Zset member-addressed commands are *not* in this category, because the member index resolves them in one lookup.

**Metadata maintenance note (ordered types).** Store **per-page counts, not cumulative offsets.** Cumulative offsets make a middle insert rewrite every downstream separator (O(pages) metadata churn per insert); per-page counts with an on-the-fly prefix sum keep an insert local to its page plus one count bump.

## 4. Hash: Concrete Design

**Routing: extendible hashing on `hash(field)`.** The metadata holds a directory of `2^global_depth` **entries**; each entry names a page id, and each page records its own local depth in its header. Lookup is `entry = hash(field) >> (bits - global_depth)`. Overflow splits the page and, when local depth would exceed global depth, doubles the directory. **Many directory entries may name the same page** — a page with `local_depth < global_depth` is shared by `2^(global_depth - local_depth)` entries, which is what makes a split rewrite only the affected entries rather than the whole directory.

> **Terminology.** Two different things are easy to conflate, so this doc keeps them apart: a **directory entry** (`dir_[i]`) holds a *page id* and answers "which page holds this field"; an in-page **slot** holds a `(hash32, offset)` pair and answers "where in this page are that field's bytes". Directory entries never hold data — key/value bytes live only in a page's data region. A lookup traverses both levels: directory entry → page, then in-page slot → bytes.

Extendible hashing is chosen over alternatives for three reasons: routing is a pure function of `(directory, field)`, so a field-addressed command's page follows from its arguments in one lookup (§2); splits are **deterministic** given the directory and page contents, which the WAL replay contract requires (§10); and directory doubling is exactly the structure Redis's own `SCAN` cursor is designed to survive, giving `HSCAN` a correct incremental cursor (§12).

Explicitly rejected: a B-tree or any structure requiring an internal-node read to locate the next leaf (breaks §2's predictability), and any layout whose page assignment depends on `absl::flat_hash_map` iteration order (breaks §10's determinism — the same instability that makes today's `HSCAN` cursor unimplementable, `src/redis_hash_object.cpp:582`).

**Field hash: MurmurHash3 with a pinned seed — never `absl::Hash`.** `EloqKey::Hash()` is 16-bit CRC16 for cluster slot routing (`include/eloqkv_key.h:170-221`) and is unusable for directory depth. The field hash must be a fixed 64-bit function whose output is stable **across processes and across releases**, because it determines both page assignment (replay determinism, §10) and on-disk placement. `absl::Hash` is disqualified outright — it is process-randomized and carries no cross-version stability guarantee, so using it would corrupt data on restart. Use `MurmurHash3_x64_128` from `butil/third_party/murmurhash3`, already a dependency and already used with a fixed seed for stable hashing in the engine (`data_substrate/tx_service/include/sharder.h:212`). Keep 64 bits, pin the seed, and record algorithm + seed under the metadata format version.

Routing uses the **top** `global_depth` bits; the in-page slot array is sorted by the **top 32 bits** of the same hash, so routing order and intra-page order agree — which is what makes splits contiguous (below).

**Page size: fixed-length, configurable, default 128 KB.** All pages are the same size, deliberately. Fixed length buys three things:

- **Future zero-copy I/O (§14).** A run of same-size page buffers maps directly onto an `iovec` array — one vectored `io_uring` submission for a huge value — and onto pre-registered fixed buffers (`IORING_REGISTER_BUFFERS` + `READ_FIXED`/`WRITE_FIXED`). Variable-size pages would need a registration pool and accounting per size class.
- **No allocator at all in v1.** Page buffers come from the **shard's existing mimalloc heap** via aligned allocation — no pool, no free lists, no size classes (a config change to `page_size` just means differently sized blocks, which mimalloc absorbs). This also solves memory accounting for free: shard pressure for collections is already tracked through per-shard heap usage rather than per-object `MemUsage`, so page faults and releases are counted automatically by the existing triggers — and the metric is structurally immune to the §8 crediting hazard, since a `shared_ptr` reset that does not actually free does not move heap usage. Cross-thread frees (the flush worker dropping the last reference) are native mimalloc behavior. A true *pre-registered* buffer pool is inherent to the §14 zero-copy path and deferred there with it.
- **`O_DIRECT` alignment.** 128 KB is a multiple of 4 KB. Page buffers **must be aligned from the outset** (the heap's aligned-allocation API) — retrofitting alignment is far more disruptive than requiring it now.

The default is not arbitrary. Because the metadata row is rewritten in every dirty cycle (below), the worst-case per-cycle cost of a single-field update is `page_size + metadata_size`. The metadata is the directory (~6 B/page) plus the `uint16` per-page entry counts (2 B/page): `metadata_size ≈ (D / page_size) × ~8 B` for object size `D`. Minimizing `p + 8D/p` gives `p* ≈ sqrt(8·D)`:

| Object size | Cost-optimal page size |
|---|---|
| 100 MB | ~29 KB |
| 1 GB | ~93 KB |
| 10 GB | ~293 KB |

The curve is flat near its minimum, so the default is not sensitive: at 1 GB, 128 KB costs ~194 KB per cycle against the optimum's ~186 KB, about **4 %**, and 128 KB is exactly optimal around 2.1 GB. The non-obvious consequence: **smaller pages are not simply better** — shrinking the page grows the directory, and past the optimum the per-cycle metadata rewrite dominates the flush. That is a worst-case (one dirty page per cycle) bound; when a cycle dirties many pages the metadata amortizes and larger pages win. Page size is a config knob, independent of the conversion threshold (§11).

**The page size must be recorded per object in the metadata row, not read from config at load time.** Otherwise changing the config would silently misinterpret every existing paged object's pages. The config governs only objects converted *after* the change; an existing object keeps its original page size for life, unless some future rewrite deliberately re-pages it.

**In-page layout: a sorted slotted page, mutated in place.** The decisive property is that **the on-disk page bytes *are* the in-memory page** — there is no separate deserialized per-page form.

```text
[header    : version, flags, local_depth, free_offset, dead_bytes]
[hash32[]  : entry_count × u32  — top 32 bits of the field hash, ASCENDING]  ┐ the
[offset[]  : entry_count × u32  — parallel array, points into the data region] ┘ slot array
... free space ...
[data      : varint klen | key | varint (vlen<<1 | large) | value-or-descriptor ]
             (grows from the page end; the length varint's LOW BIT is the
              inline/out-of-line indicator — see the large-value encoding below)
```

Lookup is a binary search over the dense `hash32` array — about 10 probes over ~3.6 KB for a full page, which stays in L1/L2 — then a key-byte comparison within the (almost always single-entry) run of equal hashes. The structure-of-arrays split matters: the search touches only hashes, never offsets or data.

| Property | Consequence |
|---|---|
| bytes are the working form | a page fault costs **no deserialization**; a future read-into-buffer store call yields zero-copy |
| flush is a `memcpy` | fewer bytes flushed **and** no re-serialization CPU |
| COW is a `memcpy` | §7's page copy is trivially correct, versus deep-copying a map of `EloqString`s |
| sorted by routing bits | splits are a contiguous partition (below) |
| stable intra-page order | supplies the intra-page half of the `HSCAN` cursor (§12) |
| no per-entry objects | no `EloqString` control blocks, no hash-map load-factor slack |

Two bounded costs: in-page free-space management (write the new value at `free_offset`, repoint the slot, add the old bytes to `dead_bytes`, compact when `dead_bytes` crosses a threshold), and values being `string_view`s into the page — already governed by §6's no-reference-across-a-yield rule, but now load-bearing rather than incidental.

Precedents, for reviewers: slot-array-plus-data-from-the-end is the classic slotted page (PostgreSQL heap pages, SQLite cell-pointer arrays); sorted-slots-then-binary-search is InnoDB's page directory; sorted-entries-plus-offset-array is RocksDB's SST block with restart points. Extendible hashing is Fagin et al. (1979), with CCEH (FAST 2019) as the cacheline-conscious modern variant.

**Splits are a contiguous partition.** Because slots are sorted by the top hash bits and routing uses those same bits, splitting a page on bit `local_depth + 1` divides the slot arrays at a **single point**: entries with a 0 bit form the low run, entries with a 1 bit the high run. The split is two `memcpy`s of contiguous slot and data ranges, requires no rehashing, and is deterministic given `(directory, page contents)` — satisfying §10's replay requirement structurally rather than by convention.

**Oversized values live out-of-line in a run of pages.** A field whose value does not fit a page does **not** store its bytes in the hash page. Its in-page slot payload becomes a small descriptor `{is_large, first_page_id, total_length}`, and the value occupies `ceil(total_length / page_size)` pages whose **full id list lives in the metadata**, located by its first id. The descriptor's encoding steals the **low bit of the record's length varint** — `(length << 1) | is_large` — so an inline entry pays at most one byte and usually zero; a large record's length field carries `total_length` and its body is the 4-byte `first_page_id`. An explicit bit rather than inferring "could not have fit inline" keeps the spill *policy* free to change (e.g. a future sub-page spill) without a format-version bump. Consequences:

- The run is recorded in the metadata, never as a chain through pages, so §2's no-inter-page-references invariant is untouched and the fault set stays metadata-computable.
- Hash pages stay uniform — they only ever hold small entries — so binary search and split logic never special-case a giant entry, and the descriptor stays a few bytes rather than an inline id list.
- Ids are allocated **best-effort contiguous** for sequential store keys and read locality, but contiguity is *not* required. Requiring it would force reallocating the whole run on any size change, turning id consumption from "net growth" into "per write" — which is what makes a `u32` id space comfortable rather than marginal.
- Same out-of-line strategy as PostgreSQL's TOAST.

Because Redis `HSET` replaces a field's entire value, a large-value update rewrites its whole run: same size reuses the run's own ids, a shrink tombstones the tail, a grow appends. No partial-run dirty tracking is needed for hash semantics.

**Sizing.** With the 128 KB default, ~0.7 load factor and ~100 B average field+value (≈900 entries/page):

| Hash size | Fields | Pages | Directory | Entry counts | Metadata total | / data |
|---|---|---|---|---|---|---|
| 1 GB | ~10 M | ~8–11 K | ~64 KB | ~22 KB | ~86 KB | ~0.008 % |
| 10 GB | ~100 M | ~80–110 K | ~512 KB | ~220 KB | ~732 KB | ~0.007 % |

Metadata stays under 0.01 % of the data — still some four orders of magnitude cheaper than re-flushing the object. (Plus `large_runs_`, proportional to out-of-line values rather than field count: ~3 KB per 100 MB value.)

**What the metadata persists vs. keeps volatile.** This split matters for write amplification: a single-field change must rewrite one page, not the directory.

| Persisted (in the metadata row, by §5 section) | Volatile (in-memory only, never serialized) |
|---|---|
| envelope: type tag, TTL, format version | per-page buffer (`PageBuf`, null ⇒ not resident) |
| page-manager section: page size, page-id high-water, pending-delete ranges (§9) | per-page last-modified ts, flushed bit, pin count; per-txn fault contexts (`tx_contexts_`) — the in-flight fetch *requests* live on the entry (§7) |
| type section: hash algorithm + seed, global depth, directory entries (page ids), per-page entry counts | per-page LRU/clock position |
| type section: logical field count, logical bytes, large-value runs (id lists) | derived free list, resident-bytes and dirty-page counters |

Per-page `local_depth`, `free_offset` and `dead_bytes` live **only** in the page header — every consumer of those already holds the page. `entry_count` lives **only in the metadata**, and is not repeated in the header: index-based selection must prefix-sum over counts to decide *which* page to fault, so a header copy would be unreachable exactly when it is needed, while duplicating a mutable fact invites the two copies to drift (a missed increment would run the binary search off the end of the slot array).

Note this costs nothing in self-description, because a page was never self-describing: `PageView` already takes `page_size` as a parameter, since §4 records page size per object in the metadata. Interpreting a page always required the metadata; the count simply joins what the metadata supplies.

**Consequence — and a correction to the tempting optimization.** It looks attractive to rewrite the metadata row only when the *structure* changes, letting a pure value update touch a single page. **That is wrong.** The stored metadata's `commit_ts` is the object's durable replay watermark (§10), so the metadata row must be flushed in **every** cycle in which the object is dirty, atomically with that cycle's dirty pages. Otherwise a value-only update — `HINCRBY` taking `5` → `6`, with field count and byte length unchanged — would advance a page's stored timestamp past the metadata's, and replay from the metadata watermark would re-apply a non-idempotent command the page already reflects. The cost is accepted: a sub-MB metadata row per dirty cycle is still 100–1000× lighter than rewriting the object, which is the entire point of paging.

### Data structures

**Ownership: two layers, split by what is type-specific.** Page management — frames, pins, faults, eviction, flush mechanics — is the correctness-critical machinery, and none of it depends on what a page *contains*. It is therefore a single shared layer, and everything above it is ONE class per type — a new paged type (list, set, zset; hash is the first) implements only its layout and routing, never the protocol:

| Layer | Lives in | Contains | Shared? |
|---|---|---|---|
| **page frames + protocol** | engine (`data_substrate`): `PageFrameTable`, `PagedTxObject` | resident-page slots (buf, pins, dirty, LRU), per-txn pin contexts, pending faults, shed/eviction, install, dirty iteration, the engine-facing virtuals | by every paged type of every API layer |
| **the type** | API layer: `RedisPagedHashObject` | the persisted metadata struct and its codec (wet cement, §5), routing (directory), splits/doubling, scan order, commands (`Execute`/`Commit*`), conversion, TTL twin, serialization tags | per type |

There is deliberately no intermediate "type core" class between the two. It would hold metadata + routing + layout operations with exactly one consumer, making the object a forwarding shell around it — a pure indirection layer of the kind this decomposition exists to remove. Its would-be contents are either vocabulary types or object members.

Vocabulary types (`PageId`, `PageIdRange`, `FreeRanges`, `PendingDeletes`, `PageBuf`, `PageSlot`, `LargeRun` — and, type-side, `PageView`, which *is* the page format) are plain structs/values, not a layer. The engine-defined ones let both layers speak the same language. The page manager owns the whole runtime id lifecycle (`free_ranges_`, `pending_delete_`, the `next_page_id_` high-water) and serializes its persisted part as the metadata row's own page-manager section (§5), with a codec written once in the engine.

```cpp
// ===== engine side (data_substrate), shared by every paged type =====

/**
 * All VOLATILE per-page state and the whole pin/fault protocol, under one
 * owner. In particular BOTH views of the pin fact live here: the per-page
 * aggregate (PageSlot::pin_count_ — what eviction consults) and the per-txn
 * decomposition (tx_contexts_[txn].pinned_ — what release consults, because
 * PostWriteCc's only handle across the Execute → WAL → CommitOn gap is the
 * tx number). State that must never desynchronize has one owner.
 *
 * PAYLOAD-scoped on purpose: the table is a member of the type object, so at
 * a §7 swap the whole thing dies with its block — pins counted this block's
 * slots and must not survive it. The ENTRY-scoped wake records (FetchHub)
 * deliberately do NOT move here.
 */
class PageFrameTable
{
  public:
    // residency
    bool IsResident(PageId id) const;
    size_t ResidentPageCount() const;
    size_t ResidentBytes() const;
    bool Install(PageId id, PageBuf buf, size_t len, uint64_t row_commit_ts);
    // pins — both views, updated together
    void PinPage(PageId id);                    // aggregate
    void UnpinPage(PageId id);
    void NotePageFetched(uint64_t txn, PageId id, bool success);  // both views
    void ReleaseTxPins(uint64_t txn);           // decomposition -> aggregate
    void EnsureTxFaultContext(uint64_t txn);
    bool HasPageWaiter(uint64_t txn) const;
    void AbandonAllTxContexts();                // §7 swap: contexts + pins die
    // faults
    void RecordPendingFault(PageId id) const;
    bool HasPendingFaults() const;
    bool TakePendingFaults(std::vector<PageId> &out) const;
    // page-id lifecycle (§4 "the two id lists"). The type asks for ids at a
    // split and returns them at a merge/shrink; everything else is internal.
    PageId AllocatePageIds(uint32_t count);     // free_ranges_ first, then the
                                                // next_page_id_ high-water
    void FreePages(PageIdRange range, uint64_t freed_ts);  // -> pending_delete_
    template <typename Fn> void ForEachPendingDeleteId(Fn &&fn) const;
    uint32_t PageSize() const;                  // per object, set at conversion
    // the metadata row's PAGE-MANAGER SECTION (§5): page size, id high-water,
    // pending deletes. This codec is the engine's — every paged type reuses
    // it verbatim; the type's codec writes only its own section. After both
    // sections load, the type hands over its live set to rebuild the derived
    // free list.
    void SerializeMeta(std::string &out) const;
    bool DeserializeMeta(const char *buf, size_t len, size_t &offset);
    template <typename Fn> void RebuildFreeRanges(Fn &&for_each_live_id);
    // dirtiness + flush
    void MarkDirty(PageId id);                  // called by the type on mutation
    void StampWrites(uint64_t commit_ts);
    template <typename Fn> void ForEachDirtyPage(Fn &&fn) const;
    void OnFlushApplied(uint64_t flushed_commit_ts);   // mark flushed pages
                                                       // clean AND drain
                                                       // pending deletes into
                                                       // free_ranges_ — fully
                                                       // generic
    // eviction (§8)
    size_t EvictablePageCount() const;
    size_t ShedColdPages(size_t max_pages);
    size_t ShedByPolicy();                      // the 10%-of-evictable rule

  private:
    absl::flat_hash_map<PageId, PageSlot> frames_;     // RESIDENT pages only
    absl::flat_hash_map<uint64_t, TxPageContext> tx_contexts_;
    mutable std::vector<PageId> pending_faults_;
    FreeRanges free_ranges_;                    // DERIVED at load (§4)
    PendingDeletes pending_delete_;
    PageId next_page_id_{0};
    uint32_t page_size_{0};                     // frames are page_size_-sized
    mutable PageId lru_head_, lru_tail_;
    uint64_t resident_bytes_{0};
};

/**
 * PagedTxObject: the engine's SEAM, and the shared machinery behind it.
 * Every engine touchpoint reaches a paged payload only through this class,
 * via TxObject::AsPaged(): the replay/standby drain (TakePendingFaults,
 * EnsureTxFaultContext), fetch completion (InstallPage, NotePageFetched),
 * the §7 swap rule (AbandonAllTxContexts), the shard clean pass
 * (ShedCleanPages), the checkpoint (ExportPagedFlush, OnPagedFlushApplied),
 * and commit/abort (ReleaseTxPins, StampWrites).
 *
 * It is deliberately NOT a pure interface with a separate implementation
 * mixin. The whole point of the shared layer is that the pin/fault/shed
 * protocol has exactly ONE implementation — a contract that invites a second
 * one invites a second pin-accounting bug farm — so the seam and the
 * machinery are the same class: it CONTAINS the page manager and implements
 * the engine-facing virtuals over it, once. Only the type hooks are pure. A
 * type that ever genuinely needed different behavior could still override
 * (the virtuals stay virtual); none should.
 *
 * Holding state in this base is safe because the other base is stateless:
 * RedisEloqObject is TxObject plus method defaults, carrying no data members,
 * and PagedTxObject does not derive from TxObject — so the multiple
 * inheritance has no diamond and the object's state is exactly frames_ plus
 * the type's own members.
 */
class PagedTxObject
{
  protected:
    PageFrameTable frames_;                     // the page manager, contained
    // type hooks — all a new paged type implements besides its layout: the
    // "header" that interprets pages (its metadata) and the row it persists.
    // Note there is NO live-id hook: liveness is derived generically from the
    // id partition (live = allocated − free − pending delete), which holds
    // because every id passes through AllocatePageId/FreePage. The type
    // enumerates its live set exactly once, at load, to rebuild the derived
    // free list.
    virtual void SerializeMetadataRow(std::string &out) const = 0;
    virtual txservice::PageRowKind PageKind() const = 0;   // HashPage today
    virtual uint64_t MetadataRowTtl() const;               // 0 unless TTL twin
  public:
    // IsFullyResident, TakePendingFaults, InstallPage(Shared), IsPageLive,
    // IsPageResident, StampWrites, AbandonAllTxContexts, ReleaseTxPins,
    // HasPendingFaults, HasPageWaiter, NotePageFetched, EnsureTxFaultContext,
    // ShedCleanPages, ResidentPageCount, ExportPagedFlush, OnPagedFlushApplied
    // — implemented here, once, over frames_ and the hooks above.
};
```

What stays per-type is exactly what a second type genuinely does differently: how entries route to pages (the extendible directory is a *hash* choice; a paged list would use positional segments), what a page's bytes mean (`PageView`'s sorted slotted layout), when pages split, what order a scan enumerates (`ScanStep`'s ascending-hash order is a property of *this* directory, §12), and the metadata row's body codec. The metadata row mirrors the same split (§5): a **page-manager section** (page size, id high-water, pending deletes) serialized by the engine's own codec, then the **type section** (`PagedHashMetadata`) serialized by the type's — each section written by the code that owns its state. At load the page-manager section seeds the frame table directly, and the derived free list is rebuilt from the type-enumerated live set.

```cpp
using PageId = uint32_t;                 // one id space, allocated from 0 upward
constexpr PageId kInvalidPageId = std::numeric_limits<PageId>::max();

struct PageIdRange                       // bulk frees are near-contiguous, so
{                                        // ranges keep both lists tiny
    PageId first_;
    uint32_t count_;
};

// free_ranges_ is a CANONICAL INTERVAL SET: sorted by first_, pairwise disjoint, and
// non-adjacent (first_ + count_ < next.first_, strictly) so touching ranges are always
// coalesced. Allocation takes only a PREFIX of a range, so it shrinks or removes but
// never splits; coalescing is therefore needed on exactly one operation, insert.

struct PendingDelete
{
    PageIdRange range_;
    uint64_t freed_ts_;                  // recycled by the post-flush callback once
};                                       // freed_ts_ <= the flushed commit_ts
// pending_delete_ is APPEND-ONLY in non-decreasing freed_ts_ order, and its ranges are
// disjoint by construction. Entries with different freed_ts_ must NEVER be coalesced.

// Refcounted, page_size_-sized, ALIGNED buffer from the shard's mimalloc heap
// (aligned alloc on the shard core at back-fill; the deleter is a plain mi_free,
// safe from any thread — the flush worker may drop the last reference). No page
// pool exists in v1 (§4, fixed-length rationale). use_count() drives COW (§7) —
// this is the only thing the refcount is used for.
using PageBuf = std::shared_ptr<std::byte[]>;

// Non-owning typed view over a page buffer: the slotted layout above. Pages are
// mutated through this; there is no deserialized per-page form.
class PageView
{
  public:
    // page_size_ and entry_count both come from the metadata (§4); a page is not
    // self-describing and never was.
    PageView(std::byte *data, uint32_t page_size, uint16_t entry_count);

    uint8_t LocalDepth() const;
    uint32_t FreeOffset() const;
    uint32_t DeadBytes() const;
    const uint32_t *Hashes() const;      // hash32[], ascending
    const uint32_t *Offsets() const;     // parallel to Hashes()
    // Find / Insert / Erase / Compact / SplitInto ...

  private:
    std::byte *data_;
    uint32_t page_size_;
};

struct LargeRun                          // one out-of-line value
{
    std::vector<PageId> page_ids_;       // best-effort contiguous, not required
    uint64_t total_length_;              // bounds the valid bytes of the last page
};

// ===== persisted: the metadata row's TYPE SECTION (§5). The row is
// [envelope][page-manager section][this] — the page-manager section (page
// size, id high-water, pending deletes) serializes from PageFrameTable by
// the engine's own codec =====
struct PagedHashMetadata
{
    uint8_t hash_algo_id_;                     // algorithm + pinned seed selector
    uint8_t global_depth_;
    uint64_t field_count_;                     // logical; answers HLEN
    uint64_t logical_bytes_;                   // logical; MEMORY USAGE, thresholds
    std::vector<PageId> dir_;                  // 2^global_depth_ directory entries
    absl::flat_hash_map<PageId, uint16_t> page_entry_counts_;  // exact; see below
    std::vector<LargeRun> large_runs_;         // live out-of-line values
};

// ===== volatile: never serialized =====
struct PageSlot
{
    PageBuf buf_;                        // null => not resident. (Distinct from the null in
                                         // PagedObjectFlush::pages_, which means "delete".)
    uint32_t pin_count_{0};              // deliberately NOT use_count(); see §7
    uint64_t last_modified_ts_{0};       // commit_ts of the last content change; on load,
                                         // the store row's commit_ts. 0 == unknown, per the
                                         // engine convention — never a "clean" sentinel
    bool flushed_{false};                // current content is durable; dirty <=> !flushed_
    PageId lru_prev_{kInvalidPageId};    // the object's own replacement clock (§8)
    PageId lru_next_{kInvalidPageId};
};

// One outstanding page fetch — the request ITSELF, not a wrapper around one.
// PageFetch derives from FetchRecordCc so the completion (the request's own
// overridden Execute(), run on the shard core once the store handler enqueues it
// back) can read its own orphaned_ flag directly; a wrapper would leave the
// completion holding a FetchRecordCc* with no safe way to reach the enclosing
// object. The derived override also keeps the whole-record completion path
// byte-for-byte untouched. The hub is reached through the inherited cce_ pointer
// (cce_ → cc_lock_and_extra_ → fetch_hub_); the ADDRESS MUST STAY STABLE for the
// I/O duration — the store handler holds a pointer — hence the owning unique_ptr.
// Page fetches bypass the shard's fetch_record_reqs_ map entirely (§13).
//
// waiter_txns_ holds TX NUMBERS, never request pointers. A completion resolves
// each txn through the current payload's tx_contexts_; a txn with no context
// (aborted, finished) resolves to nothing. Because the entries are values, a
// stale waiter can never dangle — no deregistration protocol exists on any
// teardown path: an aborting txn just erases its context.
struct PageFetch : public FetchRecordCc
{
    PageId page_id_;                                // + the caller-supplied routing hash (§13)
    absl::InlinedVector<TxNumber, 4> waiter_txns_;  // on the replay/standby/migration
                                                    // paths this holds kDrainTxn (§10)
    bool orphaned_{false};                          // set at the swap splice; carries the
};                                                  // incarnation boundary (§7)

// The single home for every outstanding page fetch of one entry, AND the wake
// records. Hangs off KeyGapLockAndExtraData as std::unique_ptr<FetchHub>
// fetch_hub_ — 8 bytes when absent, allocated on the first page fetch (which
// also takes the entry pin the structure exists for), asserted empty at
// Reset() (§7). Operations, all on the shard core: issue-or-join (live_ is
// both coalescing index and owner); swap splice (live_ → orphans_, orphaned_
// = true; nothing leaves the hub); completion (orphaned_ ? discard +
// erase-by-address from orphans_ : back-fill + resolve waiter txns — decrement
// that txn's TxWake::awaited_, record the pin via the CURRENT payload's frame
// table, and at zero either enqueue parked_req_ or, for the drain sentinel
// whose parked_req_ is null, re-drive TryCommitBufferedCommands — then erase
// from live_ and ReleasePin); Empty() feeds KeyGapLockAndExtraData::IsEmpty(),
// the recycle gate.
//
// The wake records LIVE HERE, on the entry, not in the payload. They cannot
// live in TxPageContext: DEL (or any §7 swap) drops the committed payload,
// and a parked-request pointer stored there would be destroyed with it — the
// fetch completion would have nothing to wake and the client would wait
// forever. Hence the scoping rule: I/O REQUESTS and WHO IS WAITING are
// ENTRY-scoped (they must outlive payload replacement); page STATE
// (residency, pins) is PAYLOAD-scoped and dies with its block.
struct FetchHub
{
    struct TxWake
    {
        CcRequestBase *parked_req_{nullptr};  // null for kDrainTxn (wake =
                                              // re-drive the drain) and for
                                              // lock-queue-parked commands
        uint32_t awaited_{0};                 // outstanding fetches; wake at 0.
                                              // Single wake for an N-page fault
                                              // set — waking per completion
                                              // would make HGETALL quadratic
        bool errored_{false};                 // a fetch failed; the woken
                                              // command errors out (§4)
    };
    absl::flat_hash_map<TxNumber, TxWake> tx_wakes_;
    absl::flat_hash_map<PageId, std::unique_ptr<PageFetch>> live_;   // ≤ 1 per page id
    std::vector<std::unique_ptr<PageFetch>> orphans_;  // vector, not map: one id may
                                                       // recur across chained swaps;
                                                       // completions erase by address
    bool Empty() const
    {
        return live_.empty() && orphans_.empty();
    }
};

// Per-faulting-transaction PIN context, in the payload's PageFrameTable —
// nothing is added to ApplyCc. This is the carrier §6's "pins across the WAL
// gap" requires: an ApplyCc is recycled at SetFinished(), but a write txn's
// pins must survive until CommitOn inside PostWriteCc — a different request,
// which has only the tx number. Keying by txn gives every phase access:
//   - fetch success: NotePageFetched pins the arrived page here (named by the
//     completing fetch itself) and the hub decrements TxWake::awaited_
//   - commit/abort: PostWriteCc erases the txn's context, releasing its pins
//   - a MULTI txn's pins accumulate here across its commands, released once
//   - the buffered-command drain (§10) participates as the RESERVED tx number
//     kDrainTxn = UINT64_MAX (never issued to real txns); its pins release
//     when the head buffered command applies, when the buffer empties, and at
//     term change
// Pins are this context's ONLY member, deliberately: the wake half (parked
// request, awaited count, error flag) is entry-scoped and lives in TxWake
// above — duplicating any of it here would be a second copy of state that
// must not desynchronize.
struct TxPageContext
{
    absl::InlinedVector<PageId, 8> pinned_;     // pin_count_ contributions held by this txn:
};                                              // the per-txn decomposition of the per-page
                                                // aggregate, so release knows what to decrement

// ===== the hash type: ONE class — layout, metadata, commands =====
class RedisPagedHashObject : public RedisEloqObject, public PagedTxObject
{
  public:
    size_t SerializedLength() const override;  // the METADATA ROW size, not logical
    void Serialize(std::string &str) const override;
    void Deserialize(const char *buf, size_t &offset) override;   // metadata only
    size_t MemUsage() const override;          // metadata + ResidentBytes()

    uint64_t LogicalBytes() const;             // the whole-object logical size

    // routing + layout + scan (all hash-specific): DirIndex/RouteField,
    // Get/Put/Del, PageView access, splits/doubling, ScanStep (§12), the
    // metadata-row codec (wet cement §5); commands (Execute/Commit* pairs,
    // §6); conversion (§11); TTL twin hook. The engine-facing virtuals are
    // NOT here — PagedTxObject implements them once over its contained
    // frames_; this class implements only the type hooks (metadata-row
    // serialize, page kind, live-id enumeration).

  private:
    PagedHashMetadata meta_;                   // the "header" that interprets
                                               // pages; frames_ is inherited
    FieldHashFn hash_fn_;
    // NOTE: no fetch collection lives in this class at all. Outstanding
    // fetches + wake records are ENTRY-scoped (FetchHub, §7); frame state and
    // pin contexts are PAYLOAD-scoped, inside the inherited frames_, and die
    // with this block at a §7 swap. A swap therefore never moves requests
    // across structures: it splices live → orphan (discard-on-complete)
    // within the hub and abandons the frame table's contexts.
};
```

Notes on the non-obvious choices:

- **The free list is derived, not persisted.** Live ids are the distinct values of `dir_` plus the ids in `large_runs_`; free ids are `[0, next_page_id_)` **minus the live set minus the pending deletes**. `Deserialize` already reads the whole directory, so rebuilding `free_ranges_` is a cheap scan that costs zero on-disk bytes and zero write bandwidth per dirty cycle — the codec seeds the page manager's id state and hands it the type-enumerated live set (`RebuildFreeRanges`). The pending-delete list *must* be persisted: a page whose `Delete` has not flushed is unreachable from the live set, so on reload an orphaned store row would be indistinguishable from a never-written id and would leak.
- **Per-page entry counts are in the metadata; the rest of the per-page state is not.** `local_depth`, `free_offset` and `dead_bytes` stay page-header-only, because any operation needing them holds the page already — routing uses `dir_` alone, and splitting faults the page anyway. `entry_count` is different: index-based selection needs a prefix sum over counts *to decide which page to fault*, so reading it from page headers is circular. Three of the four types need it — list for every positional operation (`LINDEX`, `LRANGE`, `LSET`, `LTRIM`), hash for `hrandfield`, set for `srandmember`/`spop` — so it is persisted and exact. Exactness is not optional: list positional indexing returns the wrong element from an approximate prefix sum.

    It is `uint16`, with a **forced split at 65535 entries** as the overflow valve. At the default page size the valve is unreachable rather than merely unlikely: the minimum entry costs 10 bytes — 4 (hash32) + 4 (offset) + 2 (varint `klen` and `vlen`, for the empty field name and empty value Redis permits) — so a 128 KB page tops out at 13,107 entries, five times under the limit. Only pages above ~640 KB can reach it, which is a real configuration since §4's optimum approaches 1 MB for very large objects. The valve is safe on both counts that matter: a count trigger is a pure function of page contents, so the split condition `byte_full || count == 65535` stays deterministic for replay (§10); and the split partitions on the next hash bit, which MurmurHash3 divides ~50/50, so one split clears the condition. Its one cost is that a count-triggered split fires on a page that is *not* byte-full — at 1 MB with 10-byte entries the page is only ~64 % full, leaving two pages at ~32 % — so it trades a little page-space waste, in that narrow regime, for 2 bytes per page of metadata everywhere. Beyond that, `dir_` enumerates the hash pages so nothing extra is needed for `DEL` fan-out, and only `large_runs_` earns its place, since large-value ids appear solely in in-page descriptors `DEL` must not have to fault pages to find.
- **A record larger than one page is REFUSED, not stored, until large runs land.** `Put` requires that one field+value fit an empty page; splitting cannot rescue a record that does not fit alone, so an oversized record would abort (Debug) or split the directory unboundedly (Release). Two guards keep it unreachable, both keyed on `RecordFits`: **conversion** consults `AllRecordsFit` and declines — an object with one oversized field stays monolithic however far past the threshold it grows, behaving exactly as a hash does today — and the **paged write paths** (`HSET`, `HSETNX`, and the increments, whose *field names* are client-supplied) reject with `RD_ERR_PAGED_RECORD_TOO_BIG`. The write-path check lives in `Execute`, before the WAL, and that placement is required: `CommitOn` on the replay and standby paths runs with no `ExecuteOn` (#509) and so has no way to reject anything, meaning any command that reaches the log must already be storable. Eligibility recovers on its own — delete the oversized field and the next write converts normally. Removing this restriction is the out-of-line large-value work (§14).
- **`SerializedLength()` and `LogicalBytes()` are different numbers.** The engine sizes the flush buffer from `SerializedLength()`, which for a paged object is the *metadata row*; `logical_bytes_` is the whole-object logical size used by `MEMORY USAGE`, the conversion threshold, and `MAX_OBJECT_SIZE`. Today's monolithic objects conflate both under `serialized_length_`, so this is a live rename hazard.
- **`MemUsage()` is finally overridden.** The monolithic hash returns the `TxRecord` default of 0 (`include/redis_hash_object.h:72` maintains `SerializedLength()` instead); the paged object must report metadata + `resident_bytes_`.
- **The entry's `FetchHub` owns the outstanding page fetches, and `pages_` holds only resident pages.** Page fetches bypass the shard's `fetch_record_reqs_` map (§13), which for whole records is both the coalescing index *and* the owner keeping each `FetchRecordCc` alive; for pages the `FetchHub` on `cc_lock_and_extra_` supplies both (§7). Presence in its live map *is* the "being fetched" flag. The `unique_ptr` is not incidental: the store handler holds a pointer to the request for the duration of the I/O, so the request's address must not move when the map rehashes — the same reason the engine's own `fetch_record_reqs_` is a node-based `std::unordered_map` rather than a flat one. Each completion erases its entry and resolves every `waiter_txns_` entry in two halves, one per owner: the hub decrements that txn's `TxWake::awaited_` and re-enqueues its parked request only at zero, while the payload's frame table records the pin (`NotePageFetched` — on success only). The wake-at-empty rule is what keeps a request awaiting N pages woken once, not N times (§6). Wake state lives on the entry so it survives payload replacement; pin state lives in the payload because the pins it feeds must outlive the `ApplyCc` yet die with the block whose slots they counted (see `TxWake` and `TxPageContext` above).

    Eviction **erases** a page's slot rather than merely nulling `buf_`, so `pages_.size()` is the resident page count. Nothing is lost: a clean page's `last_modified_ts_` is recoverable from the store row's `commit_ts` on re-fetch, with `flushed_ = true`, and a dirty page cannot be evicted at all.
- **Concurrent faulting is allowed, with per-page waiter txn lists plus per-txn wake records.** A command needing an in-flight page appends its txn to that page's `waiter_txns_` rather than issuing a duplicate read. The list says only *whom* to consider; *when* is that txn's `TxWake::awaited_` reaching zero — a command awaiting N pages appears in N lists but is re-enqueued exactly once (§6).
- **Leftover fetches from an aborted command are harmless, and need no cleanup.** Reads already handed to the store cannot be cancelled, so the fetch completes and back-fills its page — a net win, since the page becomes resident. The aborting txn erases its `tx_contexts_` entry and walks away; its txn number left behind in `waiter_txns_` lists resolves to nothing at completion time. Other waiters on the same page are woken as normal.
- **The object does not know its own key.** Page keys are derived from the hset key (§5), but a `TxObject` has no handle on it — the `CcEntry` holds it separately. The key must be threaded in from the request context wherever pages are fetched or flushed. Deliberately not cached in the object: a cached copy would duplicate state and go stale.
- **TTL follows the existing class-twin convention**, so `ttl_` belongs in a `RedisPagedHashTTLObject` twin rather than in `PagedHashMetadata` — matching `RedisHashTTLObject` today, including reporting the base `ObjectType()` ([03-data-model.md](03-data-model.md) §3). The deadline is serialized into the metadata row (`[ttl?]` in §5) exactly as monolithic TTL twins serialize `ttl_`, and must stay there: the fetch path returns no store-row TTL attribute (`FetchRecordCc` carries only `rec_str_`/`rec_ts_`/`rec_status_`), so the payload is the only channel that survives a reload. The store-row TTL *attribute* is a separate, enforcement-only annotation governed by the store-TTL contract (§9).
- **No separate metadata dirty flag** — the metadata is the `CcEntry` payload, so the entry's existing dirty machinery covers it.

### Page-id lifecycle

Every id is in exactly one of four allocation states. Only the Free and Pending-delete rows are invariants; for a Live page, whether a store row exists depends simply on whether a checkpoint has flushed it yet, and nothing in the design needs to know:

| State | Definition | Store row |
|---|---|---|
| Unallocated | `id >= next_page_id_` | none |
| Live | named by `dir_` or by a `large_runs_` entry | may or may not exist |
| Pending delete | listed in `pending_delete_` | **may exist, and must be deleted** |
| Free | `id < next_page_id_`, not live, not pending delete | **none** |

Transitions:

- **Allocate** — Free → Live, taking a **prefix** of a free range large enough for the whole request (best-effort contiguity for large runs); if none fits, Unallocated → Live by bumping `next_page_id_`. Taking only prefixes means allocation never splits a range.
- **Free** — Live → Pending delete, recording `freed_ts_`. Drop its buffer: its *contents* must never be flushed, only its row deleted.
- **Pending delete → Free** — drained by the post-flush callback, for ranges whose `freed_ts_` is at or below the `commit_ts` that was actually flushed (§9).

**Freeing always tombstones, even for a page that was never flushed.** A per-page "has a store row" latch would let such a page skip `pending_delete_` and return straight to the free list, saving a no-op `Delete`. That optimization is deliberately **not** taken. Page frees are uncommon — merging is opportunistic (§6), so an emptied page usually just stays in the directory — leaving the latch to fire mostly for large-value runs shrinking within a single checkpoint cycle; even a 100 MB value rewritten ten times per cycle would save roughly 7 K key-only `Delete`s, about 360 KB against the 1 GB the value itself wrote. Against that, the failure modes are asymmetric: a pessimistic error costs one no-op `Delete`, while an optimistic error — believing no row exists when one does — **leaks an orphan row permanently and silently**, with nothing left referencing it to drive cleanup. Deleting a key that does not exist is a harmless no-op tombstone, so the simpler rule is also the safer one.

**There is no stored checkpoint watermark, by design.** EloqKV object tables are hash-partitioned and non-versioned, so `CcEntry::entry_info_` resolves to plain `EntryInfo` (`cc_entry.h:409`) — where **`CkptTs()` asserts false**: there is no per-entry checkpoint timestamp. `SetCkptTs(ts)` stores no timestamp either; it sets only bit `0x10` ("latest version flushed"), and only when `commit_ts <= ts`. Whole-entry dirtiness for non-versioned records is that bit being unset, not a `CommitTs() > CkptTs()` comparison.

A paged object does not reintroduce one. Both decisions that would consume such a watermark — setting a page's `flushed_` bit, and draining `pending_delete_` — are made **inside the post-flush callback, which already receives the flushed `commit_ts` as a parameter**. Nothing outside the callback needs it: export selection and eviction only ask "is this page dirty?" (`!flushed_`), and `dirty_page_count_` is maintained incrementally. Storing a watermark would be caching a derived value that can only drift out of sync.

**The existing `SetCkptTs` guard already covers the whole-entry race.** Because `if (curr_commit_ts <= ts)` withholds the flushed bit when the entry was re-modified during the flush, and because *any* page write advances the object's `commit_ts`, a paged object with even one dirty page keeps the bit clear — so it is not `IsPersistent()`, so `IsFree()` is false, so it cannot be evicted and will be re-exported next round. §9's "`IsPersistent` means all-pages-clean" therefore falls out of the existing mechanism rather than needing to be built.

**The `freed_ts_` scoping is load-bearing, and it belongs on the drain — not on the export.** A checkpoint exports *every* pending-delete range it can see (§9: export is all-or-nothing per object, and the object's `commit_ts` already dominates every `freed_ts_`, so no filtering is possible or needed at that point). But pages freed *between* the export and the post-flush callback were never in that batch. Draining the list wholesale would recycle them while their store rows still exist — breaking `Free ⇒ no store row` and leaving orphan rows nothing will ever reclaim. Comparing each range's `freed_ts_` against the `commit_ts` the callback was handed scopes the drain to exactly what was written; anything freed later persists to the next cycle. The two page-level rules end up symmetric, and both are evaluated with the flushed `commit_ts` in hand rather than against stored state:

| Question | Test | Evaluated |
|---|---|---|
| is this page dirty? | `!flushed_` | anywhere |
| may this page be marked flushed? | `last_modified_ts_ <= the flushed commit_ts` | in the callback |
| is this freed range recyclable? | `freed_ts_ <= the flushed commit_ts` | in the callback |

**A page awaiting deletion is never reallocated.** Ids are cheap — allocation tracks net growth, not write volume — so deferring reuse by one checkpoint cycle costs nothing and buys the invariant that *a free id never has a store row*. That is what makes it structurally impossible for one flush batch to carry both a `Put` and a `Delete` for the same page key, with no cancellation logic on the allocation path.

**The two id lists are different data structures, despite sharing `PageIdRange`.** Implementing them alike is the obvious way to get this wrong.

`pending_delete_` needs **no overlap handling at all**: a page enters it only by being freed, and because it cannot be reallocated while pending, it cannot be freed twice — so entries are disjoint by construction. It is also naturally ordered, since commits on one object serialize on its write lock and therefore produce non-decreasing `freed_ts_`; frees append, and the drain is a **prefix pop** of entries with `freed_ts_ <= the flushed commit_ts`. Entries with different `freed_ts_` must **never** be coalesced: a merged range would have to pick one ts, and picking the smaller would recycle ids whose `Delete` was never written — precisely the leak the scoping exists to prevent. Nothing is lost by refusing to merge, because a large-value shrink frees a contiguous tail inside one transaction and so already arrives as a single range with a single ts.

`free_ranges_`, by contrast, must be maintained as a **canonical interval set** — sorted by `first_`, disjoint, and non-adjacent, so touching ranges are always coalesced. Merging is not cosmetic: without it, repeated free/allocate cycles fragment the set into singletons and the best-effort contiguous allocation that large-value runs depend on for store locality stops finding runs. Because allocation takes only prefixes, a range never splits, so coalescing is required on exactly one operation — insert, checking the left and right neighbours. In practice the set stays small — opportunistic merging (§6) frees pages only occasionally, and large-value shrinkage frees them in contiguous runs — so a short sorted vector with binary-search insert is the right shape, with `std::map<PageId, uint32_t>` as the fallback if a background compaction pass (§14) ever makes frees frequent enough to fragment it.

**Dirtiness is a per-page flushed bit, guarded by a faithful per-page timestamp.** `last_modified_ts_` always records when the page's content actually changed — the committing transaction's `commit_ts` on a write, and the store row's `commit_ts` on load. It is **never** overloaded as a clean/dirty sentinel: `0` means *unknown* throughout this system, matching how a fresh `CcEntry` starts at ts 0 with `RecordStatus::Unknown` (`cc_entry.h:585-587`). Dirtiness is the separate `flushed_` bit: `dirty ⇔ !flushed_`.

The bit is set only under a guard, and that guard is the per-page transcription of what `EntryInfo` already does for a whole entry:

```cpp
// EntryInfo::SetCkptTs(ts)              // per page, in the post-flush callback
if (curr_commit_ts <= ts)                if (slot.last_modified_ts_ <= flushed_commit_ts)
    commit_ts_and_status_ |= 0x10;           slot.flushed_ = true;
```

This is what makes clearing safe, and why a bare `bool dirty_` with no timestamp does not work. A boolean alone has no correct moment to change: clearing it at **export** time loses data if the flush later fails, marking pages clean whose contents never reached the store; clearing it in the **post-flush callback** loses data too, because the export runs on the shard core, the flush on a worker thread, and the callback is enqueued back — so in that window the core can accept a write to an already-exported page. The guard closes the window: a page rewritten mid-flush carries a newer `last_modified_ts_`, fails the comparison, and stays dirty.

Note the callback needs only **one** timestamp, not a per-page record of what was exported: since the object's `commit_ts` dominates every page's `last_modified_ts_` (§9), comparing each page against the flushed `commit_ts` is exactly right. Two alternatives work but are subtler: comparing the flushed `PageBuf` against the slot's current buffer (sound only because §7's COW guarantees a write during a flush yields a different buffer, and it needs the callback to carry buffers rather than ids), and a double-buffered dirty set snapshotted at export and restored on failure (smaller, but three code paths instead of one comparison).

## 5. On-Disk Format and Page Keys — the Wet Cement

Everything in this section describes bytes that end up in the data store. Once a cluster has written paged objects, none of it can be changed by editing code alone — a revision means **converting data already on disk**, so these choices should be settled (not merely sketched) before implementation begins. Everything else in this doc is in-memory and freely refactorable: `PageSlot`'s fields, the in-flight fetch bookkeeping, the `free_ranges_` representation, the eviction policy, the Yield protocol mechanics.

The **format version byte** below is what makes the rest survivable, and is the one item that cannot be omitted: with it a later binary can read v1 and write v2; without it the only upgrade path is dump-and-reload. The per-object recording of `page_size_` (§4) is the same idea applied to a config knob — because each object carries its own page size, changing the default is not a migration.

**Object type tags.** `RedisObjectType` values are on-disk format and must never be renumbered (`include/redis_object.h:44-45`). Paged hash needs new **appended** values (paged and paged-with-TTL), dispatched in `RedisEloqObject::DeserializeObject` (`src/redis_object.cpp:33`). A separate **format-version byte** inside the metadata body allows the paged layout to evolve without burning type tags.

**Metadata row** (value under the ordinary user key). The row's structure mirrors the §4 ownership split — a **page-manager section** followed by a **type section** — so the codec that writes each section is the code that owns its state:

```text
[type tag][ttl?][format version]                  — envelope, written by the type twin
[page-manager section, length-prefixed]           — engine codec, one per build
    [page size]
    [page-id high-water]
    [pending deletes  : ranges {first page id, count}]
[type section]                                    — per-type codec (hash below)
    [hash algorithm + seed id]
    [global depth][field count][logical bytes]
    [directory        : 2^global_depth × page id]
    [entry counts     : per live page → uint16, exact; split forced at 65535]
    [large-value runs : per run → id count, page id list, total length]
```

The sectioning earns its bytes three ways. The page-manager section's codec is written **once, in the engine** (`PageFrameTable::SerializeMeta`/`DeserializeMeta`) and reused verbatim by every paged type of every API layer — no per-type reimplementation of the id-lifecycle encoding, and no accessor round-trip between the type's codec and the page manager's state. The **length prefix** makes the section skippable and self-delimiting, so a type-unaware reader (the §9 sweeper, export tooling) can parse id state — or step over it to a section it does understand — without any type codec. And the single **format-version byte** in the envelope covers both sections, so the layout can evolve without burning type tags (the section codecs take the version as input).

The type section is `PagedHashMetadata` (§4). Deserializing the row yields a **metadata-only object with every page non-resident** — no page I/O at load time: the page-manager section seeds the frame table's id state directly, the type section fills `meta_`, and the derived free list is then rebuilt from the type-enumerated live set. The volatile per-page state (resident buffer, last-modified ts, flushed bit, pin count, LRU position), the per-txn fault contexts, and the free list are never serialized (§4); the in-flight fetch requests live on the entry, not in the object at all (§7).

Note the split of per-page state: `local_depth`, `free_offset` and `dead_bytes` live in the page header only, since every consumer of those already holds the page; `entry_count` lives only here in the metadata, since a prefix sum over counts is what decides which page to fault (§4). Neither is duplicated. `dir_` already enumerates the hash pages, so no further per-page table is needed.

**Everything read back from the store is validated before it is trusted.** The store is not a trusted input: a truncated row, a torn write, a stale row served under the wrong key, or a metadata row whose two sections disagree must all produce a deterministic corrupted-object error, never an out-of-bounds read, an allocation blowup, an assertion abort, or a silently missing field. Three layers, each owning what it can actually check:

| Layer | Checks |
|---|---|
| `PageFrameTable` (generic) | byte count equals the object's page size; the id is live; and on a metadata row, page size within `[kMinPageSize, kMaxPageSize]` and a multiple of 8, id high-water within `kMaxPageCount`, pending ranges ordered, disjoint, non-empty and below the high-water |
| `PageView::ValidateImage` (page format) | layout version; the slot array fits below `free_offset`; `free_offset`/`dead_bytes` inside the page; every slot offset in the record area; both varints of every record parse inside the page and its body fits; slot hashes non-decreasing (what `Find`'s binary search and the ordered scan assume) |
| the type's `ValidatePageImage` (semantics) | the entry count THIS object's metadata records for the page; local depth no deeper than the directory; and **routing identity** — every entry must route to this page id under this directory |

Routing identity is what makes a page's *name* checkable even though a page carries no id of its own: a structurally valid page belonging to a different id, served under this one, has entries that route elsewhere and is rejected. That is the reason no page-identity field is needed in the header.

Bounds are as important as structure, because the parser acts on what it reads before it can check it: an unbounded `next_page_id` sizes the free-list bitmap and its scan, and an unbounded large-run count sizes a `reserve`. Both are bounded before use, so a corrupt row costs a rejection rather than an OOM. Cross-section consistency is checked once both halves are in: every live page id the type section names must be below the page-manager section's high-water, and the per-page entry counts must sum to the object's field count.

**Page rows** (values under derived keys) come in two kinds, both exactly `page_size` bytes:

- **hash pages** — the slotted layout of §4. Not self-describing: interpreting one needs `page_size` and `entry_count` from the metadata (§4), which is always resident before any page is fetched;
- **large-value data pages** — raw value bytes with no header, so they can serve directly as vectored-I/O targets later (§14). The owning field's descriptor carries `total_length`, which also bounds the valid bytes in the run's final page. Integrity for these relies on the store row's own framing rather than a page header.

**The page key is a composite `TxKey` whose hash comes solely from the object key.** This is the mechanism that makes co-location structural rather than a rule someone must remember to apply.

A new `TxKey` implementation represents `<object key, page id>`: its serialized bytes carry both components, so it names a distinct store row, but its `Hash()` returns the **object key's** hash and nothing else. Because a single key object serves every storage path, the read side and the write side cannot disagree about placement — there is no override to forget on one of them.

The page id therefore decides *which row*, never *which shard*:

| Role | Routed by |
|---|---|
| online command dispatch | the Redis key alone — `Sharder::MapKeyHashToHashPartitionId(key->Hash())`. No page id exists at this level. |
| store row identity | the composite `<object key, page id>` — distinct bytes, but the object key's hash |

Where it appears, and where it must not:

- **Fetch.** A command evaluating inside the object discovers a missing page, builds the composite key, wraps it in a `TxKey`, and passes it to `FetchRecordCc`. Partition selection then lands on the object key's partition automatically. The fetch should pin the **metadata** `CcEntry` so the object cannot be evicted while its page loads.
- **Checkpoint.** `ExportForCkpt` puts the same composite `TxKey` into the scan batch vector, so `PutAll` computes the same partition for the write that the fetch used for the read.
- **Nowhere else — with one path that must actively exclude them.** Composite keys never enter a `CcMap`, are never seen by command dispatch, and take part in no concurrency control; pages are not `CcEntry`s. The exception that needs code rather than construction is the **online store scan**: because page rows share the object table's keyspace, a scan of the store *does* see them, and `BackfillForScanNextBatch` must discard them before pattern/type processing (below). Keeping them out of the CcMap is not sufficient by itself.

Three separate requirements collapse into this one invariant:

- the shard owning `hset_key` must be able to fault *its own* object's pages locally;
- checkpoint must be able to put the metadata row and its page rows in one atomic store batch (§9);
- bucket migration must move a logical object's rows together rather than splitting it across node groups.

**Page key byte encoding.** Page rows share the object table's keyspace, separated from user keys by a reserved 8-byte magic, and the object key is carried **length-prefixed** so the object component is prefix-free:

```text
\x00 E K V P A G E   <key_len:4, BE>   <object key bytes>   <kind:1>   <page_id:4, BE>
└──── 8-byte magic ────┘  └─ u32 ─┘                          └─ page id, u32 ─┘
```

- **The length prefix is what makes locality claims true.** It is NOT optional framing. Without it the object component is not prefix-free: object key `a` is a byte-prefix of object key `a\x00\x00\x00\x00\x00`, so `page(a, 0)` is a prefix of `page(a\x00…, 0)` and the latter sorts *between* `page(a, 0)` and `page(a, 1)`. Two consequences, both fatal: one object's page rows are no longer contiguous, and a per-object prefix delete over `\x00EKVPAGE<key>` deletes another object's pages. The fixed-width length field separates keys of different lengths at the length bytes and makes equal-length keys the same width, so `[magic][len][key]` identifies exactly one object. `EncodePageKeyPrefix` produces exactly that byte range.
- **Metadata rows are NOT in this keyspace, and need not be adjacent.** The metadata row lives under the plain object key; only page rows carry the magic. An earlier framing wanted metadata and pages contiguous — that is not required and is not done. Recovery loads the metadata row by the object key and faults pages by their derived keys; nothing scans "the object's rows" as one contiguous store range that must include the metadata.
- **Why eight magic bytes and not one.** Redis keys are binary-safe, so a workload using binary keys has a **1/256** chance of starting with any single reserved byte. Eight bytes drops that to 2⁻⁶⁴, and the cost is nil: page rows occur once per `page_size` of data.
- **Leading `\x00`** marks page rows as obviously non-textual; the ASCII body makes them self-identifying in a hexdump.
- **`key_len` is 4 bytes** so it spans every key the largest-key backend admits (RocksDB's 32 MB), which is what lets the public key limit reserve the codec's overhead without shrinking the usable limit on any backend (below).
- **`kind`** distinguishes hash pages from large-value data pages; **big-endian page id** so one object's pages sort in page order.
- The embedded object key already carries its namespace prefix (`include/eloqkv_key.h:49-70`), so page keys inherit namespace isolation with no extra work.

**Key-length budget: reserve the codec overhead, do not overflow the store.** A page key adds `kPageKeyOverhead` = 8 (magic) + 4 (len) + 1 (kind) + 4 (page id) = **17 bytes** to its object key. Conversion is key-blind — the object never sees its own key (§4) — so the only place "every derivable page key fits the store" can be enforced is the public key-length limit: `MAX_KEY_SIZE = store key ceiling − kPageKeyOverhead`. EloqStore rejects store keys above half its 4 KB data page (2048 bytes), so its limit is 2048 − 17 = 2031; other backends keep their historical ceiling (RocksDB 32 MB), the 4-byte length field spanning it so the 17-byte reservation is negligible. Without this reservation a maximal user key produces a page key past the store's ceiling — a Debug assert, undefined in Release.

**Page rows must be filtered out of every ONLINE STORE SCAN, not merely kept out of the CcMap.** Page rows live in the object table's keyspace, so any scan that reads the store sees them — and they are not Redis keys: a page row's first value byte is a page-layout version, not an object type tag. Leaving them in leaks internal binary keys to `KEYS`/`SCAN` (clients can feed them back into other commands), makes `SCAN TYPE` misclassify (page layout version 1 has the same byte value as the serialized List tag, so `SCAN TYPE list` returns hash pages), and wastes scan work and reply memory. `BackfillForScanNextBatch` (`template_cc_map.h`) therefore discards any key carrying the magic **before** pattern and type processing, in both its local and remote branches.

The filter sits at the point of CONSUMPTION, not where rows are collected (`FetchBucketDataCc::AddDataItem`), and that placement is required: the scan's pagination cursor is the **last raw row of the batch**, so the fetched deque must keep page rows for the cursor to advance past them. Filtering at ingress would leave a batch of all-page-rows empty, stalling the cursor — an unadvanceable scan rather than a leak. A batch that is entirely page rows correctly yields an empty scan cache and re-fetches from beyond them.

(A cleaner end state is a separate internal table for page rows, so no online scanner can see them at all. That is a larger change — a second table in the catalog, its own flush routing — and is deferred; the filter is what makes the current keyspace sharing safe.)

**Reserve the magic prefix by CENTRAL enforcement, not per command.** Reject any user key beginning with the magic, and enforce it — together with the length limit — over **every physical key of every command**, not one overload. `RedisServiceImpl::CheckKeyAdmissible` is the single gate; it runs in the single-key path, the multi-object path (MSET/MGET/DEL/EXISTS/…), the queued MULTI/EXEC path (`MultiExec`), the ZScan path, and WATCH. A gate attached only to the single-key overload is a security hole: `MSET <exact derived page key> <value>` would overwrite a live page row, and in a multi-partition deployment an adversary can search for a raw key that routes to the same partition as a target object's pages. Centralizing the check makes the collision rate exactly zero on every path.

This encoding also makes `DEL`/expiry fan-out expressible as a **prefix delete** over the per-object prefix `\x00EKVPAGE<key_len><object key>` — well-defined precisely because that prefix is now prefix-free — needing no page-id enumeration; see §15 for whether EloqStore exposes one.

**Batch ordering is the store adapter's job, not the emission order's.** Because page rows live under the magic prefix and the metadata row under the plain object key, one object's flush emits keys that are *not* in store order (the `\x00`-leading page keys sort below a printable object key). EloqStore requires each write batch to be **sorted and unique** (`BatchWriteTask::SetBatch` asserts strictly increasing keys), and that requirement is met one layer down: `EloqStoreDataStore::BatchWriteRecords` checks `is_sorted` and sorts the batch — carrying each row's key, value, ts, op and ttl together — before dispatch. So the flush path may emit in whatever order is natural (metadata first, then pages), and no ordering constraint propagates up into `ExportPagedFlush` or the checkpointer.

What sorting cannot repair is **duplicates**, so the emitted set must be duplicate-free by construction, and is: a freed page is erased from the frame table (`FreePage`), so the dirty set and the pending-delete set are disjoint; live-id enumeration excludes both free and pending ids; and the metadata key can never collide with a page key, since page keys carry the reserved magic that user keys are refused. `paged_flush_roundtrip_test` asserts this shape on every flush cycle it drives.

Note this is why the composite `TxKey` above matters: with one key type serving fetch and flush alike, the read and write paths cannot disagree about placement, so there is no per-path override anyone can forget to apply.

## 6. Command Execution: the Yield Protocol

**Current sequence** (verified in `data_substrate/tx_service/include/cc/object_cc_map.h`, `ObjectCcMap::Execute(ApplyCc&)`, lines 111-1354):

1. Lock mode is fixed upfront from the command: `cc_op = req.IsReadOnly() ? Read : ReadForWrite` (`:281`).
2. Whole-record fetch if `PayloadStatus() == Unknown`, under a **pin** only, then park with `BlockOnFetch` and return without executing (`:508`). Re-entry releases the fetch pin and skips locate (`:342-348`). The engine's ordering is *fetch happens before lock acquisition* — stated as such at `:310-315`, though only to justify an assert; the rationale is not recorded (see below).
3. `AcquireCceKeyLock` (`:542`); a write command gets **WriteIntent** (asserted `:626`).
4. Dirty payload built only if a pending command from an earlier op in this txn exists (`CreateDirtyPayloadFromPendingCommand`, `:632`, gated on `DirtyPayloadStatus() == Uncreated`).
5. Write commands **upgrade to WriteLock** (`:778-803`, asserted `:867`) — still before execution.
6. `ExecuteOn` runs synchronously on the committed or dirty payload (read-only path `:751`; write paths later in the function).
7. Lock release: if `apply_and_commit_` (the skip-WAL one-shot) the lock is released inside the same `ApplyCc` (`:755-765`, `:807-813`); otherwise it is handed to the txm via `obj_result.lock_acquired_` and held across `WriteToLog` until the transaction finishes.

**The change.** `ExecuteOn` gains a `Yield` outcome, and the object **declares** the pages it is missing rather than fetching them itself:

> **The object declares, the apply path fetches.** The command does not issue its own fetches — that would require `ExecuteOn` to take extra context (shard, request), changing `TxCommand::ExecuteOn`'s signature for *every* command in the codebase, monolithic ones included. Instead the paged object records its missing page ids while computing the fault set and exposes them through `PagedTxObject::TakePendingFaults`; `ApplyCc` drains that set and issues the fetches. Nothing about `ExecuteOn`'s signature changes, so no monolithic command is touched — and issuance lands where the key and the shard actually are, which this section already required, since a `TxObject` has no handle on its own key (§4). The recorded set is deduplicated (a command touching many fields on one page faults it once) and drained before the request parks.

The sequence:

1. `ExecuteOn` computes its fault set (§3) and, if any page is missing, **records the missing ids on the object and returns `Yield`**, having executed nothing and produced no reply. `ApplyCc` then drains that set (`TakePendingFaults`), issues one `FetchPage` per id — registering this transaction as a waiter on each — and parks. A store-busy `Retry` from any fetch unwinds the parking pin and re-enqueues the request rather than parking it, matching how the whole-record path handles the same condition; fetches already issued complete harmlessly and leave their pages resident.
2. `ApplyCc` sets `block_type_` to a new **`BlockOnPageFault`** value — appended to `ApplyBlockType` at `cc_request.h:7028`, deliberately *not* reusing `BlockOnFetch` — and returns without completing. On the speculative path no lock is released, because none was taken; on the contended-path re-run and in a multi-command transaction the fault happens with the lock held, and it stays held (below).
3. Fetch completion back-fills the page into the metadata block and re-enqueues the `ApplyCc` through its `TxPageContext` (§4).
4. Re-entry restores the cce from `req.CcePtr()` and skips locate, like today's `BlockOnFetch` skeleton, then branches on lock state: lock already held → re-run `ExecuteOn` under it; no lock → speculative re-run plus the deferred-acquisition protocol below. `BlockOnFetch` itself is untouched: its resume contract — fetch precedes acquisition, so a resume implies payload-known, no lock held, release the fetch pin, proceed to `AcquireCceKeyLock` (`object_cc_map.h:310-315`, `:342-349`) — is violated by page faults some of the time (a held lock, deferred acquisition, `tx_contexts_`-owned pins), so overloading it would thread paged conditionals through the monolithic resume path and weaken its assert. A separate state keeps that path and its assert byte-for-byte intact, scoped forever to whole-record fetches.
5. Repeat until a pass completes with no fault. For predictable types this is **two passes** (fault set fetched in one batch); for the content-search fallback (§3) it is iterative.

**Several `ApplyCc`s may fault the same object concurrently.** This follows the engine's existing shape: `FetchRecordCc` already registers multiple requesters on one fetch and re-enqueues them all on completion. So the entry's `FetchHub` maps `PageId → PageFetch` (§7), and a command needing a page either issues the fetch or registers its txn in that page's `waiter_txns_`. The park uses the new `BlockOnPageFault` state — see step 2 above for why `BlockOnFetch` is not reused.

**A request awaiting several pages must be woken once, not once per page.** `FetchRecordCc`'s existing multi-requester pattern assumes each requester awaits exactly *one* record, so waking every waiter on every completion is correct there. Page fetches break that assumption: a command with an N-page fault set appears in N `waiter_txns_` lists, and waking on each completion re-enqueues it N times — redundant runs at best (quadratic for a wide fault set like `HGETALL`, whose every re-run rescans the set), a request enqueued while already queued at worst. So the object's per-txn `TxPageContext` (§4) keeps an **awaited count**, decremented per completion, and the parked request is re-enqueued only at zero — a count, not a set, because the completing fetch itself names its page; nothing ever asks which pages a txn still awaits. Abort needs no deregistration: the txn erases its context, and its number left in waiter lists resolves to nothing.

Honoring the one-fetch-per-request assumption instead would mean serializing — fetch, wake, re-run, fetch again — turning an N-page fault set into N sequential store round-trips and destroying the fetch-the-whole-set-and-yield-once property.

There is no correctness obstacle. A speculatively faulting command holds **no locks** (acquisition is deferred), so it blocks no one; a writer that takes the lock and splits pages, or a commit that swaps the payload outright (§7), merely causes the resumed run to recompute — which the Yield protocol already handles.

> **Rejected: a single-fault-owner rule.** Admitting only one faulting `ApplyCc` per object suggests itself on two grounds — avoiding per-page waiter lists and per-request counters, and bounding pinned working sets. Both are thin. Pins are per-page **refcounts on shared pages**, so two commands on one object mostly overlap and the second merely increments a count rather than duplicating memory; where they touch disjoint ranges the peak does rise, but the rule bounds nothing *across* objects, so global pressure still needs the shard's existing `EnqueueWaitListIfMemoryFull` path either way. Nor can commands livelock evicting each other, since a pinned page cannot be evicted and each command therefore accumulates pins monotonically; the worst case is a wasted re-fetch. Meanwhile the ownership machinery — an owner field, a waiter queue, a check-before-locking rule, and a requirement to release ownership before waiting on a lock — costs more than the waiter lists it saves, and introduces a **deadlock** (owner holding no lock, waiter parked while holding one, owner then needing that lock) that would have to be designed around. A rule whose first implementation question surfaces a deadlock is not simplifying anything.

**In a multi-command transaction, page faults necessarily occur under a retained lock — and that is fine.** Deferred acquisition keeps a *single-command* (auto-commit) fault lock-free, which is the overwhelmingly common Redis case. But if an earlier command of the same transaction already took and retained a lock on the key, a later command's fault happens with that lock held; releasing it is not an option, since that is the very 2PL violation the design avoids.

This is a situation paging newly introduces. Its whole-record analogue cannot arise: holding a lock leaves `cc_lock_and_extra_ != nullptr`, so `IsFree()` is false and the entry is never evictable — a second command can never find the *record* missing. Pages are different, because partial eviction is deliberately not governed by the lock (§8): the lock pins the metadata block, not its pages. So a clean unpinned page evicted between two commands of one transaction will be faulted with the transaction's lock held.

It costs nothing. The lock is held until commit regardless, so a fetch inside that window adds no blocking that 2PL was not already imposing, and it cannot deadlock — the holder waits on store I/O, never on another transaction.

**Why unwinding is safe — and the one park that is not an unwind.** The txm learns nothing until `hd_res->SetFinished()`. A *lock-free* yield (the speculative path) unwinds the entire `ApplyCc`, leaving exactly the state of an ordinary cache miss: the object not fully resident, no lock held. This holds for the WAL-enabled path too, not only the one-shot path. The *lock-held* park (fifth table row below, and multi-command transactions) is different in kind — **a request parked on I/O while holding a conflict lock is a state the engine has never had**, since `BlockOnFetch` parks always precede acquisition. Notably, this creates **no new lock-release obligation**: locks are recorded against the *transaction*, so a fetch failure on a lock-held park merely errors the request — the txm aborts and its abort path releases every lock the tx holds, exactly as for any post-acquisition error today (a remote `ApplyCc` RPC failure is the existing precedent). The fetch-error handler must *not* release the lock itself. Term change likewise rides existing machinery: ng-term recovery clears the locks wholesale, and the parked request is errored through its `TxPageContext`. What is genuinely new is only the **diagnostic shape** — a granted lock whose owner is neither running nor in any lock queue but waiting on store I/O. No deadlock is possible (the owner waits only on I/O, which completes; there is no tx→tx edge), but tooling that assumes "granted ⇒ running or lock-queued" must be checked.

**The lock-free resume re-runs the protocol-routing check, not the protocol's middle.** While parked without a lock, the payload may have been swapped (§7) or replaced outright by a *monolithic* object — a `SET` overwrite can commit, since the parked request holds nothing, and it must stay allowed: forbidding it would add a new blocking edge (reads blocking writes for the duration of store I/O), and the engine already runs this race for whole records, where a fetch parks under a pin only and `BackFill`'s version compare discards bytes the entry has moved past. So the resume routes by representation, as if freshly entering: payload still paged → the paged protocol (taking the fully-resident fast path if it now applies); payload now monolithic or deleted → today's sequence, re-running against the new object and thereby serializing after the write. **Every resume re-runs this check, lock-held ones included**: only a holder at `WriteIntent` or above actually freezes the payload — `ReadIntent` does not block a writer's commit — and one representation test is cheaper than exempting particular lock modes. Fetch lifetime across any replacement is §7's orphan-list rule.

> **Implemented shape (2026-07): a pre-acquisition probe, not a reordering.** The correctness property this section needs is precisely *a faulting command holds no lock it must release on error*. That is obtained without moving the acquisition at all: immediately **before** the existing acquire, a scoped probe runs `ExecuteOn`; if it yields, the request parks holding nothing, and if it completes the result is discarded and control falls through to today's untouched acquire-then-execute sequence, which re-runs it under the lock. The probe is scoped to an existing, paged, not-fully-resident, non-expired payload — excluding monolithic and non-existent objects (cannot fault), fully resident ones (cannot yield, via an O(1) `IsFullyResident()` count check), and expired ones (whose expiry the normal path must evaluate before any page loads).
>
> This keeps the monolithic path byte-identical and needs none of the restructuring below, at the cost of executing a completing command twice — CPU only, since a non-yielding run is by definition fully resident. The **grantability test** (`NonBlockingLock::WouldAcquireLock`, built and pinned by `LockGrantability-Test`) is what removes that second execution when the lock is uncontended, and remains the next increment; the acquisition-outcome table below describes that end state.

**Defer lock acquisition until after `ExecuteOn` returns non-`Yield`.** Rather than acquiring a lock and releasing it on yield, the lock is not taken at all until the command is known to complete. `NonBlockingLock` gains a **test** — "would this request be granted?" — that inspects grantability *without* enqueuing the requester. `ApplyCc` then runs:

| Test says | `ExecuteOn` result | Action |
|---|---|---|
| grantable | no fault | acquire (cannot fail, see below) and finish — one execution |
| grantable | `Yield` | acquire nothing; park on the fetch |
| would block | `Yield` | acquire nothing; park on the fetch |
| would block | no fault | acquire — blocking now, properly queued — then **re-run `ExecuteOn`** under the lock |
| (lock acquired after queueing) | re-run returns `Yield` | **keep the lock; park on the fetch** (`BlockOnPageFault`), resume re-runs under the held lock |

The fifth row is not an anomaly: the directory can shift while the request waits in the lock queue (the holder splits pages), so the authoritative re-run may fault even though the speculative run's set was fully fetched and pinned. Faulting under the held lock is exactly the multi-command-transaction case above — the holder waits only on store I/O, which completes, so no deadlock is possible — and the resume re-runs `ExecuteOn` under the lock until a pass completes fault-free, resetting `result_` each time.

**This is a correctness-and-simplicity requirement, not a concurrency optimization.** The decisive case is a **fetch that fails**. If the lock is acquired before `ExecuteOn` and the page fetch then errors, that lock must be released by the error path — and it is a lock the txm does not yet know about, since the txm learns nothing until `hd_res->SetFinished()`. So the fault-error handler would have to release exactly the lock this command took, while *not* touching a lock an earlier command of the same transaction established: a lock-provenance distinction inside an error path, which is precisely the kind of logic that is easy to get subtly wrong.

Deferred acquisition removes the distinction entirely, because after it a fault can only ever happen in one of two states, and **neither requires the error path to release anything**:

| Fault happens with | On I/O error |
|---|---|
| no lock at all (single-command, the common Redis case) | nothing to release |
| a lock the *transaction* already holds and has recorded (2nd+ command of a MULTI/Lua txn) | the txm's abort releases it, as for any post-acquisition failure |

That is what makes "never release a lock on I/O error" a safe blanket rule rather than a special case. The concurrency gain — not blocking other transactions across store I/O — is real but secondary.

The mechanical payoff is that **a lock that is never acquired is never released.** This removes the whole class of restore logic: no snapshot of pre-acquisition state, no hazard of dropping a lock an earlier command in the same MULTI/Lua transaction established, and no need for a `WriteLock → WriteIntent` downgrade on this path (`CcMap::DowngradeCceKeyWriteLock` exists at `cc_map.h:432`, but deferred acquisition never has a lock to walk back). Deadlock detection never observes a transient acquire/release either.

It is also consistent with how a whole-record fetch already behaves — though note the engine states the *ordering*, not its rationale. `object_cc_map.h:310-315` records that "FetchRecord (if need to) happens before lock acquisition" only to justify an assert, and `data_substrate/docs/` does not discuss it. What the code shows is a mechanism choice: the fetch calls `GetOrCreateKeyLock`, which creates the lock structure and **pins** without taking a conflict lock, and the resume path releases exactly that pin (`:346-347`). Since `IsFree()` is false whenever `cc_lock_and_extra_ != nullptr`, a pin already prevents eviction; a conflict lock would add blocking the fetch does not need. So the ordering plausibly falls out of using the minimal sufficient mechanism, with blocking-avoidance as a consequence.

Two candidate rationales are ruled out by reading `AcquireCceKeyLock` (`src/cc/cc_map.cpp:40-110`): it consults `commit_ts` **only under Snapshot isolation**, and EloqKV runs RepeatableRead; and the object path passes `cce_payload_status` as a hardcoded `RecordStatus::Normal` ("we add lock regardless of whether the record is deleted"). Locking therefore does not require the record to be resident. **Confirmed with the engine owner (2026-07): the ordering carries no correctness requirement — fetch-before-lock exists to reduce lock contention.** Deferring acquisition past `ExecuteOn` extends the same contention-reduction rationale one step further and is safe to build on.

Two properties make it correct:

- **Test-then-acquire is atomic on the uncontended path.** Nothing interleaves between the two on a single shard core — no suspension point separates them — so if the test says grantable and `ExecuteOn` did not yield, the acquisition cannot fail and no concurrent commit can have intervened. The single speculative execution is therefore also the authoritative one. This atomicity is the load-bearing assumption of the whole scheme.
- **On the contended path the speculative run is only a fault-set probe.** Its results must be discarded and `ExecuteOn` re-run after the lock is held. Skipping the re-run would return a reply computed before the wait: run speculatively against value `V`, block, watch the holder commit `V'`, acquire, and answer with `V` — a stale read at the command's own serialization point.

Two practical requirements follow:

- **The test must mirror the real acquisition exactly**, including blocking-queue fairness. If the queue withholds a read grant while a writer waits, the test must report "would block" in that case too; any divergence breaks the atomicity argument above.
- **Scope: the protocol selector is representation alone** — a paged payload runs the paged protocol; monolithic and non-existent objects keep today's acquire-then-execute sequence untouched. Within the paged branch, a **fully resident** object (an O(1) test: resident-page count equals the live-page count, both maintained as counters) takes today's sequence as a *fast path* — not because the protocol would fail on it, but because a non-yielding `ExecuteOn` gains nothing from speculation while still paying the grantability test; the shortcut is reliable since a non-yielding `ExecuteOn` runs to completion on the shard core with no eviction interleaved. Residency is an optimization inside the paged branch, never a routing criterion. The reason is not caution alone: a monolithic object can never page-fault, and its whole-record fetch already completes before lock acquisition via the existing `BlockOnFetch` path, so deferral buys it nothing while exposing it to a restructured protocol whose correctness rests on the grantability test mirroring real acquisition exactly, on the contended path re-running and resetting `result_`, and on speculative runs never mutating the `CcEntry`. Confined this way, a mistake in any of those reaches only paged objects, and it rides the same rolling-upgrade gate as the paged format (§11). Gating on this also disposes of a hazard: the write path materializes a dirty payload for a missing object at `:991-1002`, mutating the `CcEntry` (`SetDirtyPayload`, `SetDirtyPayloadStatus`, `SetPendingCmd`). Doing that speculatively and then discovering the lock would block would leave uncommitted state behind for a transaction that holds nothing. Since a non-existent object has no pages, it can never fault, so it never speculates, and the hazard cannot arise.

    Stated generally: **a speculative run must not mutate the `CcEntry`.** It reads to discover which pages it needs; every state change belongs to the authoritative run under the lock.

**Discarding a speculative result means clearing it, not overwriting it.** Several result types accumulate rather than assign — `HGETALL`, `HMGET` and `HKEYS` append into vectors — so an authoritative re-run without an explicit reset would double the reply instead of replacing it. Each command must reset its `result_` at the top of an authoritative run, and this needs the same per-command audit that #509 applied to replay determinism.

**Neither step between locking and `ExecuteOn` moves — the lock moves past both.** Today `CreateDirtyPayloadFromPendingCommand` and TTL-expiry processing sit together at `object_cc_map.h:614-633`, after acquisition and before `ExecuteOn`. Deferring acquisition changes only where the lock lands:

| | today | deferred acquisition |
|---|---|---|
| 1 | fetch metadata | fetch metadata |
| 2 | **acquire lock** | TTL check, dirty payload |
| 3 | TTL check, dirty payload | `ExecuteOn` (computes and faults its page set) |
| 4 | `ExecuteOn` | **acquire lock** |

Both steps keep their position relative to `ExecuteOn`, and each is fine there for its own reason:

- **Dirty payload creation does not move at all — it never needed an acquisition.** `CreateDirtyPayloadFromPendingCommand` (`:632`) is gated on `DirtyPayloadStatus() == Uncreated`, meaning "no dirty payload, has pending cmd" (`tx_record.h:86-89`), and `Uncreated` is set at exactly one site: `:1138`, past the write-lock upgrade under `assert(lock_acquired_ == WriteLock)` (`:867`), and only when `!apply_and_commit_` (`:1103`) — the case in which the lock is **retained** by the transaction. So a second command of the same transaction arrives with the WriteLock *already held*; `AcquireCceKeyLock(ReadForWrite)` at `:542` returns `WriteLock` rather than `WriteIntent`, which is why the guard `if (acquired_lock != WriteLock)` at `:778` skips the upgrade block entirely for it. The upgrade happens in the **first** command, before its own `ExecuteOn`.

    Under deferred acquisition nothing about this changes: the condition can be tested without acquiring anything, since `lk->HasWriteLock() && lk->WriteLockTx() == txn` is a pure read of lock state — exactly what the read branch already does at `:620-623`. (A fresh write command holds only `WriteIntent` at `:632`, as the assert at `:625-626` allows, but its status is `NonExistent`, so it does not take this branch.)
- **TTL-expiry evaluation needs no lock, and must stay ahead of any page fault.** Expiry is a pure comparison of wall-clock time against the stored TTL, so no lock is involved; and because the TTL lives in the metadata block (§4) rather than in any page, evaluating it faults nothing. Its existing position — before `ExecuteOn`, and therefore before the page faulting that happens inside `ExecuteOn` — is exactly right and should be preserved: an expired key then **short-circuits** onto the "key does not exist" path with **no page ever loaded**, which for an expired large object is the difference between loading nothing and loading its working set. The expiry *action* is a write and takes the lock at the normal (now later) acquisition point — `need_write_lock` already folds in `ttl_expired_ || ttl_reset_` (`:780-783`) — and the verdict must be recomputed on an authoritative re-run, since wall-clock time advances while waiting for the lock.

**The grantability test must reason over the whole lock lattice, not just "is it free".** `LockType` is ordered — `NoLock < ReadIntent < ReadLock < WriteIntent < WriteLock` (`cc_protocol.h:74-81`) — and `LockTypeUtil::DeduceLockType` (`:93-143`) maps `ReadForWrite → WriteIntent`, `Write → WriteLock`, and `Read` to `ReadIntent` under EloqKV's `RepeatableRead` + `OCC` configuration (`include/redis_service.h:633,635`). The test must therefore answer for the *specific* mode being requested, and for an **upgrade** from a mode this transaction already holds, exactly as the real acquisition would.

**To be explicit about which outcome acquires what** — "does not proceed" is a *completed* execution, not a suspension:

| `ExecuteOn` outcome | Command | Acquire |
|---|---|---|
| `Yield` — a page is missing, execution incomplete | any | **nothing new** — a lock already held is retained; park on the fetch |
| completes | read-only | `ReadIntent` |
| completes, command proceeds (will modify) | write | `WriteLock` |
| completes, command does not proceed (ran fully, nothing to change — e.g. a hash write against a missing key) | write | `WriteIntent` |

The read row is EloqKV's configuration specifically: `LockTypeUtil::DeduceLockType(Read, …)` returns `ReadIntent` under `RepeatableRead` + `OCC` (`include/redis_service.h:633,635`), `ReadLock` under the `Locking` protocol, and `NoLock` under `ReadCommitted` or `Snapshot` — in which last case the deferral is a no-op, since there is nothing to acquire. The proceeds/does-not-proceed split is a **write-path** distinction only: `need_write_lock` (`:780-783`) sits after `assert(acquired_lock >= WriteIntent)` under "This is a write command", and read commands have no equivalent branch, finishing at `:770-771`. Orthogonally, an `apply_and_commit_` one-shot releases whatever it acquired before returning (`:755-765`, `:807-813`).

Finally, the required mode is knowable **before** `ExecuteOn` runs, not only after: `need_write_lock` (`:780-783`) is derived from object existence, the command's *static* `ProceedOnExistentObject()` / `ProceedOnNonExistentObject()` declarations, and the TTL flags — all metadata-resident, so determining it faults no page. The final mode can therefore be acquired in a single step once execution completes, collapsing today's `WriteIntent` → upgrade-to-`WriteLock` two-step while still leaving behind the lasting `WriteIntent` that a non-proceeding write command is supposed to hold (`:804-825`). The tradeoff is that today's early `WriteIntent` doubles as *early write-write conflict detection*: deferring it lets two writers both speculate before one queues, wasting the loser's speculative run. Cheap under low contention, and left as a separate question since the intent phase may carry semantics beyond this path.

**Restart must be side-effect-free.** `ExecuteOn` may not build a reply or mutate command result state on a pass that yields; the reply is constructed only on the pass where every needed page is resident. In a multi-command transaction a dirty payload may already exist at entry (`:632`), so the restart must be idempotent with respect to it.

**Pinning is a liveness requirement.** A page fetched in pass 1 must be **pinned until the command completes**, because the fetch itself grows resident memory and can trigger the eviction pass that would drop it — without pinning, the retry re-faults the same page forever. The pin covers the command's **whole gathered set**, not only fetched pages: a needed page that is already resident at discovery time is pinned then, or it becomes the page eviction takes during the async gap while the rest of the set is in flight. With pinning, each pass leaves at least one more page resident, so a K-page command converges in ≤K passes. Pin counts live in the metadata block, and *which txn holds which pins* lives there too (`tx_contexts_`, §4) — not on the request — so a write transaction's pins survive to `CommitOn` inside `PostWriteCc`, which releases them by tx number; abort and term-change teardown release the same way.

**`CommitOn` does not fault on the primary (replay may — §10). The paged `Execute` override makes its write set resident, explicitly.**

The rule is deliberately NOT stated as an emergent invariant. It could be — it follows from five conditions: same routing and arguments in both phases, a directory frozen by the WriteLock, pins surviving the WAL gap, splits allocating rather than faulting, and merges staying deferred — but any one of those could be broken silently by a later change, and "no one ever adds eager page merging" is a landmine rather than an invariant.

The obligation is therefore **explicit code instead of an emergent property**. Each paged type overrides its per-command `Execute(XCommand&)`, and the override is responsible for making resident everything the matching `CommitOn` will touch — computing that set from the same arguments and, where necessary, from what an earlier fetch revealed. `CommitOn` then **asserts residency**, so a violation fails loudly in test rather than silently in production. If eager merging is added later, the override fetches the buddy page and the assert catches the author who forgets.

**Dispatch prerequisite.** The per-type `Execute(XCommand&)` overloads are non-virtual today — `include/redis_zset_object.h` contains no `virtual` at all (its 18 `override`s are `TxObject`'s) — and `ZAddCommand::ExecuteOn` reaches the object through a **static** downcast, `static_cast<const RedisZsetObject &>(object)`. A paged class must therefore be reachable somehow. Two options, both mechanical: add a `virtual` representation query on `TxObject` and branch to the right cast at each command site (~114 sites, but it keeps paged and monolithic as independent implementations of one interface rather than forcing an inheritance relationship that would also drag in unused containers); or virtualize the overload sets and derive. Runtime cost either way is one branch or one indirect call per command.

**"As far as needed" means the command's data accesses — structural changes cost nothing extra.** For ZADD the override deliberately over-fetches relative to its own work: the filtering consults index pages alone, but the override also brings in `ordered_page(old)` and `ordered_page(new)` for each surviving element. Beyond that, none of the structural operations force a fetch:

- **splits** allocate rather than fetch, so no anticipation is required — and they are the only *mandatory* structural change, since a full page must split to accept an insert;
- **large values** need none either: replacing or deleting an out-of-line value frees its run from `large_runs_` in the metadata and allocates a new one, with no page read;
- **merges are discretionary**, which is what makes them harmless. A sparse page is valid indefinitely — nothing in extendible hashing requires merging — so `CommitOn` merges, at the end of the command, only those pages whose buddy **happens to be resident**, and leaves the rest sparse. Correctness is unaffected because merging only reclaims space. Doing it once at the end also means the decision sees the command's final state rather than intermediate ones, and a multi-element `HDEL` does not re-evaluate the same page repeatedly. Space that opportunistic merging skips can be reclaimed by a background pass, which is outside any command's critical path and free to fault.

**Why keep `CommitOn` fault-free at all**, given that holding a lock across store I/O is ordinary database behaviour: because **commit must not be able to stall or fail**. A store error during `Execute` merely aborts the command. Inside `PostWriteCc` the transaction is **already durable in the WAL** — it can neither be aborted nor applied — so a faulting `CommitOn` would need the command to stay in the buffered list and be retried (`BufferedTxnCmdList` / `TryCommitBufferedCommands`, `cc_entry.h:1751`), pulling replay machinery onto the normal commit path and forcing every read to drain buffers first. Keeping the mutation phase synchronous avoids that entirely.

Three conditions therefore remain load-bearing, alongside the explicit override: **pins across the WAL gap are correctness, not optimization** (pages fetched by `Execute` must survive to `CommitOn`); splits allocate rather than fault; and the directory stays frozen between the phases under the write lock.

**Where the fault set is not computable in one step.** The "compute the whole set, yield once" property holds for field-addressed commands but not universally. Where it fails, the `Execute` override simply iterates until its set is closed — extra rounds, not a correctness problem:

- **Out-of-line values need a two-level fault.** Reading a field whose value is large requires faulting `page(f)` first to read its `{is_large, first_page_id, total_length}` descriptor, and only then the run itself.
- **Zset member-addressed commands likewise** — the index page first, then the ordered pages the old score identifies (§3).

By contrast, index-based selection (`HRANDFIELD`, `SRANDMEMBER`/`SPOP`, list positional ops) resolves in **one** step, because the per-page entry counts it prefix-sums over are in the metadata (§4). Leaving them in page headers is what would have made it circular — the counts are precisely what decides which pages to fault.

Pinning also spans the **write path across the log gap**, and here it is a **correctness requirement**: the pages the `Execute` override made resident on `CommitOn`'s behalf must survive `WriteToLog`, or eviction takes them and `CommitOn`'s residency assert fires (below).

*Footnote on a neighbouring path:* the existing `ExecResult::Block` handling releases the lock unconditionally (`object_cc_map.h:1160-1164`). Deferred acquisition removes that hazard from the page-fault path but not from this one: if a multi-command transaction could ever reach `Block` on a key it already holds, the release would drop a lock an earlier command established. Redis semantics (blocking commands do not block inside MULTI — BLPOP degenerates to LPOP) suggest it is unreachable, consistent with the `assert(!req.apply_and_commit_)` there; worth confirming rather than assuming. A second interaction, for the later paged list types: a blocking command that faulted pages and then returns `Block` must **erase its `TxPageContext` when it parks on the condition** — §6 pins bridge I/O gaps, and a condition park is unbounded (a client can block for minutes), so carrying pins into it would make pages unevictable for the duration; the condition wakeup re-runs and re-faults.

**No reference into page or directory memory may survive a yield.** While a command is parked, eviction (which is not governed by the key lock) can drop page buffers, concurrent writers can split pages and rewrite directory entries, and a commit swap can replace the whole metadata block (§7). `EloqString` can be a `string_view` aliasing page bytes, so results must be **copied out** (or re-looked-up after resume), never held as views across a yield. This is a per-command coding discipline, not something the lock provides.

## 7. Dirty Payload and Copy-on-Write

There are two dirty-payload creation sites, with different purposes:

| Site | Purpose | Condition |
|---|---|---|
| `CreateDirtyPayloadFromPendingCommand` (`object_cc_map.h:632`) | materialize the transaction's **own prior** uncommitted effect so this command can read it | 2nd+ command of a txn on the same object (`DirtyPayloadStatus() == Uncreated`) |
| `CreateDirtyPayloadFromCommand` (`:991-1002`) | fabricate an **empty** object, because the key does not exist and there is nothing to execute against | `object_not_exist`, independent of command ordering |

So a first write command against an **existing** object creates no dirty payload at all — it executes on the committed payload (`:1071`, guarded by `assert(cce->IsNullPendingCmd())`) and buffers itself as a pending command, with the mutation applied later at commit.

The COW cost below therefore lands only where the directory is worth copying: a **multi-command transaction on an existing object**. The other case creates a brand-new object whose directory is empty, so there is nothing to copy — and it is paid once per transaction regardless of how many writes follow.

**Scheme.** The dirty payload is a **new metadata block whose per-page entries reference the committed object's page buffers** via `shared_ptr`. On writing a page: if its refcount is 1, mutate in place; otherwise **copy the page** and install the copy in the dirty metadata. Abort drops the dirty metadata (copies free themselves); commit swaps the metadata block, releasing the superseded pages.

Write memory cost is therefore proportional to **pages modified**, not object size.

**Two rules make this correct:**

1. **The COW refcount and the pin count are different mechanisms and must not be conflated.** `use_count()` counts the **logical versions** referencing a buffer (committed metadata, an uncommitted dirty metadata, an in-flight flush) and answers *may I mutate in place?*. `pin_count_` counts the **in-flight commands** needing it resident and answers *may I evict?*. They have different lifetimes — per-version versus per-command.

    Conflating them breaks the fast path on the *common* case, not a corner: HSET pins the page it is about to write (§6, liveness across the log gap), so if the pin were a `shared_ptr` then `use_count()` would be 2 at commit time, the COW test would copy, and the mutation would land in the copy while the request's pin still referenced the orphaned original. Still correct, but every HSET to a resident page would pay a full page copy — copy-always instead of copy-on-write. Nor can it be patched with `use_count() <= 1 + my_pins`: there is no robust way to tell which references are your own pins versus the dirty payload or the flush worker.
2. **`refcount == 1` is a check-then-act, so references may only be acquired on the owning shard core.** `ExportForCkpt` runs on the shard core, so a reference it takes for the flush worker is safe: the worker merely *holds* it. This yields flush-safety for free — a page handed to the flush worker has refcount ≥ 2, so a concurrent write copies instead of mutating a page being serialized on another thread, which is stronger than a "being checkpointed" flag.

### Volatile state across the commit swap

Committing a multi-command transaction installs the dirty metadata block as the payload, superseding the block that parked commands and in-flight fetches refer to. (The single-command path never swaps — a first write on an existing object commits its pending command on the committed payload in place, `object_cc_map.h:1063-1138` — so this applies to MULTI/Lua transactions and to `CommitOn`s that return a different object.) The rules:

- **One swap rule for every successor — paged, monolithic, or deleted.** All outstanding `PageFetch`es already live in the entry's `FetchHub` (below), so the swap moves nothing across structures: it **splices the hub's live map into its orphan vector**, flagging each request **discard-on-complete** — a local move; request addresses stay stable for the store handler. Every `tx_contexts_` entry is **erased** — pins die with the block, below — with each *fetch-parked* request (`parked_req_` non-null) eagerly re-enqueued; lock-queue-parked commands need no wake, since the lock grant is their wake, and if buffered commands remain the drain is re-driven once. The new payload, whatever its type, starts with empty page machinery and an empty live map. Uniformity is the point: chained overwrites (paged → string → paged → …) during one slow fetch just splice into the same orphan vector each time. A completion checks its request's orphan flag on the shard core: orphaned → discard bytes, erase from the vector; live → the normal §4 path. **The flag is load-bearing even though fetches never move**: it carries the incarnation boundary — a fetch issued for one incarnation's page id must never install into a successor's, since after `DEL` + recreate + re-conversion ids restart from 0 and the same id names an unrelated row, so install decisions cannot be made from id-liveness in the current directory. Eager wake cannot double-enqueue, because orphaned completions consult no contexts and fresh fetches are fresh hub entries. Its one cost is a possible duplicate read — a woken command re-faults a page id whose orphaned fetch is still flying — harmless and rare; lazy waiting would have bought nothing, since superseded-block results were always discarded anyway (the directory may have changed), so waiting only delayed the re-run.
- **Pins do not survive the swap.** Erasing the contexts releases every `pinned_` list together with the block whose slots carried the matching `pin_count_`s — the two levels (per-txn decomposition, per-page aggregate) die together, so they cannot desynchronize. The new block's COW-created slots copy `buf_`, `flushed_` and `last_modified_ts_` but zero `pin_count_` and the LRU links. A woken command re-runs from scratch against the current payload — recompute the fault set, re-fault, re-pin; at most one wasted fetch round, §6's ordinary restart safety. No per-request tagging is needed: pins are object state keyed by txn, so nothing on any request can go stale. A command parked in the lock *blocking queue* (the §6 contended path) is covered identically — its context is erased like any other, no wake is needed since the lock grant is its wake, and it re-runs from scratch.
- **The `FetchHub`, concretely.** `cc_lock_and_extra_` is a `KeyGapLockAndExtraData` (`non_blocking_lock.h:403`) — the pooled per-entry structure already holding the key lock, pending command, dirty payload, buffered command list, forward entry, and the entry `pin_count_`. It gains one lazily allocated member, `std::unique_ptr<FetchHub> fetch_hub_`, holding the **live page-fetch map** (`PageId → unique_ptr<PageFetch>`), the **orphan vector**, and the **per-txn wake records** (`tx_wakes_`, §4) — 8 bytes on every lock object, allocated only while fetches are outstanding, and guaranteed available when needed since a page fetch requires the pin the structure exists for. Integration rules: `IsEmpty()` (`:466-485`) additionally requires the hub empty or absent, or `RecycleKeyLock` could free requests the store handler still points into — every page fetch also holds an *entry* pin from issue to completion, so a non-empty hub already implies `pin_count_ > 0` and the explicit clause is defense in depth; `Reset()` asserts it empty (recycle precedes reuse); `ClearTx()` leaves it alone (fetches are entry state, not per-tx state). Implementation nit: the class's existing `pin_count_` (entry pins) and the new `PageSlot::pin_count_` (page pins) share a name across granularities — rename one. **A natural follow-up, deliberately out of v1's scope**: `CcShard::fetch_record_reqs_` — the shard-global `unordered_map<LruEntry*, FetchRecordCc>` owning and coalescing *whole-record* fetches — has exactly two use sites (`cc_shard.cpp:2187` try_emplace, `:2398` erase) and is never iterated globally, so it too could move into the hub, replacing a global hash lookup with a pointer hop and deleting `LruEntry*`-keyed global state. Deferred because it refactors the monolithic hot path this design otherwise leaves byte-for-byte untouched; it should land as its own change.
- **No dangling waiters, by construction.** `waiter_txns_` holds tx numbers, not pointers; an aborted or finished txn simply has no `tx_contexts_` entry, so a completion that resolves it finds nothing and moves on. There is no deregistration protocol to run on any teardown path.
- **Deletion follows the same rule.** The deleted metadata block still survives until the deletion flushes — that is a §9 *flush* requirement (the fan-out needs the page list), not a fetch-ownership one. Its fetches orphan to the entry list and its waiters wake like any other swap; a woken command re-runs the §6 routing check, sees `Deleted`, and completes with "not exists" immediately instead of waiting on pages that no longer matter.
- **`flushed_` needs no reconciliation at the swap**, provided the COW write stamps every copied slot `flushed_ = false` with the transaction's commit ts. An unmodified page's copied state is either still true (content unchanged, still durable — correct) or stale-false because a checkpoint completed mid-transaction, which costs one redundant re-export. Staleness can only err in the dirty direction; the §9 ts guard keeps the clean direction sound.

## 8. Partial Eviction

Current eviction is strictly whole-`CcEntry`: `CcShard::Clean()` (`src/cc/cc_shard.cpp:1681`) walks the LRU page list and frees entries where `IsFree()` — no locks **and** persistent (`src/cc/cc_entry.cpp:104`). A dirty entry is never evicted under memory pressure; the shard instead triggers a checkpoint to make entries clean.

**Change.** When the clean pass reaches a paged object's metadata entry, it invokes the object's **shed-clean-pages** hook rather than freeing the entry outright.

Eviction of a page is simply **resetting the metadata's `shared_ptr`** — safety is automatic; any other holder keeps its own reference alive. Per-page dirty state is tracked in the metadata (§4), and the existing invariant carries over per page: **a dirty page must be checkpointed before it can be evicted.**

### Policy

Per visit, shed **10 % of the object's evictable pages, and never fewer than one**; when no pages remain resident, the metadata-only entry becomes an ordinary whole-entry eviction candidate.

- **Victim selection** walks the object's internal LRU/clock (`lru_prev_`/`lru_next_` on `PageSlot`, §4) from the cold end, skipping pages that are dirty (`!flushed_`) or **pinned** — pins protect the working set of in-flight commands (§6), so an unpinned filter is as necessary as an unclean one. The §10 drain's pages are protected the same way, through the reserved `kDrainTxn` context's pins — a non-empty buffered list does *not* block shedding of the object's other pages.
- **The 10 % batch amortizes the visit.** A paged object is a single `CcEntry`, so one LRU sweep reaches it once; shedding a single page from a 10 000-page object would make reclaim proportional to sweeps rather than to memory pressure. Ten percent gives geometric decay across repeated sweeps while leaving a hot object mostly intact — and because a recently touched object moves to the LRU head, the policy naturally concentrates on cold ones. The floor of one page guarantees forward progress for small objects, where 10 % rounds to zero.
- **Metadata is shed last, and deliberately so.** It is ~0.01 % of the object's bytes (§4) yet required for *every* access, including the routing needed to fetch pages back. Freeing it to reclaim a rounding error of memory, then paying a store read to rebuild it on the next touch, is a bad trade — so pages go first and the metadata survives until the object is entirely cold.
- **When nothing is evictable** — every page dirty or pinned — the object yields nothing and the pass moves on, exactly as it does for a dirty whole entry today; the shard then relies on checkpoint to make pages clean (`ShardCleanCc` → `NotifyCkpt`).
- **Terminal state.** With all pages gone the entry holds only metadata, and the existing `IsFree()` gate applies unchanged: clean and unlocked, so it is freed by the normal whole-entry path.

The 10 % figure is a tunable, not a derived constant. Under severe pressure an escalation is available — evict a wholly `IsFree()` paged object outright, metadata and any remaining pages together — mirroring the checkpoint escalation `ShardCleanCc` already performs when it can free nothing.

**Implemented shape (Phase 5).** Three details differ from the sketch above, and one item is not implemented:

- **The internal LRU is intrusive by page id**, not by pointer: `PageSlot::lru_prev_`/`lru_next_` hold `PageId`s so they survive a rehash of the resident-page map, with `lru_head_`/`lru_tail_` on the core. The links and the two ends are `mutable`, because every page access funnels through the core's `View()`, which is `const` (ExecuteOn never mutates the object, per #509) and is therefore the natural — and exact — touch point. Nothing serialized or replayed depends on them, and `CheckInvariants()` verifies the list names precisely the resident set, once each, in both directions.
- **The hook is offered by the clean guard, not by the clean pass.** `CcMap::ShedPagesForEviction` defaults to declining, `ObjectCcMap` overrides it, and only `CcPageCleanGuardWithoutKickoutCc` — the regular memory-pressure clean — calls it. An explicit kickout (`KickoutCcEntryCc`, drop table, bucket migration) wants the entry gone and bypasses partial eviction entirely.
- **There is no escalation tier, and no pressure test in the hook.** Every visit sheds. Reaching a paged object in the LRU sweep is itself the signal: entries are chained by recency, so being visited means the object is not frequently accessed, and it is big, so taking 10 % is worthwhile. If that relieves the pressure the sweep stops arriving; if the same object is revisited, that says both that it is still cold and that pressure is high enough to keep triggering cleans, so it keeps shrinking until no page is resident and the ordinary whole-entry path reclaims the metadata. Any additional pressure gate inside the hook (a heap watermark, an unproductive-sweep counter) is at best redundant with the sweep's own trigger condition and at worst contradicts it, disabling shedding exactly when it is needed.
- **The fault-path throttle below IS implemented** (§8 "Memory admission"). Parking on the shard's memory wait list requires a request carrying no per-attempt state, which the refusal path now guarantees by recycling the entry's key lock and clearing `CcePtr` before parking — an earlier attempt to clear that state without recycling the lock produced a use-after-free, and retaining it produced an assert in the clean pass, so both halves are load-bearing. `ApplyBlockType::BlockOnMemory` exists alongside `BlockOnFetch`/`BlockOnPageFault` for the case where a retry still carries an entry pointer, so the resume path never mistakes an admission retry for a fetch resume.

**Allocation under memory pressure.** Page allocations consult the same budget everything else does — actual heap usage (§4) — and split by path along the line §6 already drew. **Commit-path allocations never block or fail**: splits, COW copies, and conversion run inside commit, which cannot stall (§6), so they may overshoot the budget by their bounded amounts (a page or two; low-MB for conversion) and trigger reclaim asynchronously — the existing `ShardCleanCc`/`NotifyCkpt` flow, which now also reaches this section's page shedding. **The fault path is the throttle**: before issuing its fault set, a command on an over-budget shard routes through the existing `EnqueueWaitListIfMemoryFull` wait — safe on the speculative path, which holds no locks, and safe on the lock-held re-run for the same reason faulting under a held lock is safe (§6): it waits on reclaim, which checkpoint progresses independently of any key lock, never on another transaction. **Back-fill completions allocate without blocking** — their bytes are already fetched, and the pre-issuance gate bounds the outstanding volume.

**Accounting caveat — resolved by heap-based accounting.** The reclaim accounting must not credit a dropped page as freed while another reference (flush worker, dirty payload) keeps it alive — otherwise the shard-full loop can spin believing it made progress. Because shard memory pressure is measured as the per-shard mimalloc heap's actual usage (§4) rather than by manual crediting, this cannot happen: a `shared_ptr` reset that does not actually free does not move the metric. The same applies to the 10 % target — measure it against heap usage actually reclaimed, which the heap reports directly. Note also that `RedisHashObject` does not override `MemUsage()` today (it returns the `TxRecord` default of 0; `include/redis_hash_object.h:72` maintains `SerializedLength()` instead), so the paged object needs a real **resident-bytes** counter distinct from **logical bytes** (§4): only the former shrinks under partial eviction, while the latter answers `HLEN`-adjacent sizing, `MEMORY USAGE`, and the conversion threshold.

### Memory admission (§8)

Nothing above bounds what the fault path allocates: page buffers come from plain `new[]`, a whole-object read faults its entire fault set however large, concurrent commands' fault sets aggregate without limit, and one object's between-checkpoint dirty set can exceed the flush machinery's own heap (2.5 % of shard memory), which wedges the key (see the plan's findings). The controller below closes all four, reusing the engine's existing allocation-failure machinery rather than inventing a parallel one.

**Status.** The read/fault path, the reply bounds, the allocator plumbing, and the write-side park are IMPLEMENTED. The park lives at the top of `PostWriteCc` — the durable-commit path every WAL'd write takes — where nothing is yet mutated, re-entry is idempotent, and the write lock is held for the duration; `force_paged_commit_park` (Debug) drives it deterministically in `tests/unit_cc/paged_commit_memory_park.py`. Two paths remain ungated BY DECISION, allocating via `AllocPageUnchecked` under the axiom: the **replay/standby drain** (parking recovery on a transient over-budget window risks failing the promotion deadline — a dead node — which is strictly worse than an axiom-bounded overshoot while applying the replicated stream), and the **`apply_and_commit` in-place commit** (no-WAL configs; the commit runs inside the same `Execute` as `ExecuteOn`, past forward-entry side effects, so a park there is not restart-safe — it would need the request re-run to not duplicate standby forwards).

**Admission = allocation. Allocate every needed page buffer BEFORE issuing any fetch; the heap is the only ledger.** The §6 speculative pass is side-effect-free toward the object — nothing pinned, nothing fetched — so fault-set computation is the one place a command can be turned away and simply re-run later. At that point the command try-allocates its missing pages' buffers from the shard's budget-checked heap. No shadow accounting (`in_flight_fault_bytes_`-style counters) exists to drift out of sync: concurrent commands cannot jointly overshoot because the memory is physically claimed, and a leak is visible in heap accounting rather than silently shrinking an admission number. The gate itself is `PageAdmission` (a thread-local hook installed for exactly the window in which a thread acts as a shard, alongside the existing heap override), answered by `CcShardHeap::CanAllocate`; `PageFrameTable::ReservePageBuffers` does the claiming and `InstallPage` consumes the claim instead of allocating. `AllocPage` returns null when refused, and `AllocPageUnchecked` names — and documents — every path that must not be refusable. Acquisition is **all-or-nothing**: a command that gets 7 of 10 buffers and fails the 8th frees all 7 before parking — parked requests holding memory while waiting for memory is the textbook deadlock — and re-acquires from scratch on wake. Cleaning is expensive — it walks the LRU and can cascade into a checkpoint request — so it is NOT tuned to the refused allocation's size. A triggered campaign runs to a TARGET, freeing ~10 % of the shard budget (or everything reclaimable, if less), which bounds how *often* cleaning runs rather than how much each pass does; stopping the moment the heap dips below its threshold leaves zero headroom and makes the next allocation clean again. Parking means the shard's memory wait list, and it obliges the refusal path to drop EVERY trace of the attempt first: the entry's key-lock structure is recycled (an empty one left behind trips the clean pass's own IsFree assert) and the raw entry pointer is cleared (retaining it while asking for eviction can leave it naming a freed entry). Liveness and termination then come from that list's existing machinery rather than from a retry spin. Between admission and install the claimed buffer lives in the payload's frame table (`reserved_`), deduped per page id (a command joining an in-flight fetch claims nothing new). Its lifecycle is closed on EVERY completion outcome — this is load-bearing, found broken in review: the claimed buffer becomes the CANONICAL fetched buffer (`BackFillPage` takes it, fills it from the fetch result, and shares it into every applicable payload), `InstallPageShared`/`InstallPage` consume any remaining claim for their id on every outcome including refusals, rejected images drop it, the fetch-error branch drops it on both payloads, and a completion under a dead term — which never reaches the install path — releases it through `CcMap::ReleasePageReservation`. The fetch result string itself is still one transient copy; landing the store read directly in the claimed buffer is the §14 zero-copy step.

**Refused: park, and drop every trace of the attempt first.** The request goes on the shard's existing `cc_wait_list_for_memory_` — the same list a failed `FindEmplace` uses — and `DequeueWaitListAfterMemoryFree` re-enqueues it to re-run `ExecuteOn` from scratch, which §6's restart safety already guarantees is correct. That list admits only requests carrying no per-attempt state, so the refusal path must first **recycle the entry's key lock** (the speculative probe created it; an empty lock left behind trips the clean pass's own `IsFree` assert, and in Release makes the entry permanently unevictable) and **clear the raw entry pointer** (retaining it while asking for eviction can leave it naming a freed entry). Parking must happen at this boundary ONLY — a mid-flight request with fetches issued and pins held would corrupt per-attempt state — which is why the check is at admission, not inside `AllocPage`.

**A refusal must also START a reclamation campaign, on its own state.** Admission refuses at `allocated + requested > limit`; the heap reports full at `allocated >= limit`. Those predicates differ, and in the gap between them the cleaner would find nothing to do, wake the waiter, and be refused again — a spin, not a park. So a refusal sets an explicit campaign request (a boolean; the campaign's SIZE is always the fixed target below, never the refused allocation), and the campaign, once started, runs on its own in-flight state rather than on `Full()` — the first thing a successful sweep does is make `Full()` false, so continuing on that predicate would abandon the campaign short of its target and leak its accounting into the next one.

**A cleaning campaign frees a BATCH, not what the caller asked for.** Cleaning walks the LRU and can cascade into a checkpoint request, so what must be bounded is how OFTEN it runs. A campaign therefore targets ~10 % of the shard budget free (or everything reclaimable, if less); stopping the moment the heap dips below its threshold leaves zero headroom and makes the next allocation clean again.

**When a campaign frees nothing, WHY decides what happens — and every branch must leave the waiters woken or aborted.** The clean guard's skip test already evaluates both clauses of `IsFree`, so attributing each refusal costs nothing: **pin-blocked** (a live key-lock/pin) or **dirty-blocked** (not yet persistent), pins winning when both hold, since a checkpoint alone cannot reclaim a pinned entry. Then: freed-something-but-short-of-target → retry the waiters, they may fit now; dirty-dominated → request the checkpoint and wake (a pending checkpoint means this is **not** a deadlock — those pages become reclaimable, so failing the waiters would fail commands that seconds of patience would serve); pin-dominated with nothing dirty → the hold-and-wait case, so abort the requests that opted into OOM and **wake the rest to retry until the holders finish and drop their pins**. Waiting the holders out is deliberate: they are ordinary in-flight commands that terminate on their own, whereas identifying and killing the specific pin-holding transaction is a far larger mechanism for the same outcome. The wake is unconditional because `AbortRequestsAfterMemoryFree` fails only requests whose `AbortIfOom()` is set — which ordinary Redis object commands never set — so a branch that only aborts leaves them parked with nothing left to re-trigger the cleaner: a hang, not a preemption.

**A fault set that can NEVER fit is an error, not a park.** If `missing_pages × page_size` alone exceeds the admissible ceiling (derived from the shard budget, not from config), parking would stall forever — the reply is a deterministic too-big error. This closes the 200 MB `HGETALL` on the 64 MB shard whatever the reply-bound flag says: the reply bound is config, the admission ceiling is the machine.

**The write/apply path allocates where it runs: in `CommitOn`, which may park on memory but can never fail. (IMPLEMENTED at `PostWriteCc`; the drain and the no-WAL in-place commit are ungated by decision — see Status above.)** An `Execute`-side reservation is unworkable twice over: a write's true allocation need is unknowable there (COW copies depend on commit-time refcounts; splits allocate pages the pre-image does not have; `RESTORE` and conversion materialize whole objects), and the replay/standby apply path has no `Execute` at all (#509) yet faults and allocates today. So `CommitOn` try-allocates on demand, and an allocation failure is handled exactly like a missing page in the drain's discover-then-mutate shape: the discovery phase parks the apply "blocked on memory" BEFORE any mutation, yielding the shard core — mandatory, since the clean pass runs on that same core and a spin would deadlock the cleaner — and is re-driven when memory frees. An allocation that surfaces mid-mutation (a split's new page) retries with the same yield. This amends §16's commit invariant honestly: commit cannot *fail*; it may *stall* on memory.

**The park retains the write lock; `CommitOn` fully evaluates before the lock is released — at both call sites, `PostWriteCc` and `ApplyCc`.** Past the WAL, the lock is the only thing between *durable* and *applied*: released early, a queued writer could commit against the pre-image and fork a version chain no replay or standby would reproduce. This is §6's existing yield rule ("a yield never releases a lock; resume under it") applied at the commit site. It cannot deadlock through the lock: memory reclaim never needs THIS entry — the clean pass sheds other, clean pages, and a locked dirty entry is skipped by eviction regardless — so the park waits on nothing that waits on the lock, and termination stays the axiom's job. Visibility is unchanged: lock-free ReadCommitted readers see the committed pre-image for the duration of the park, exactly as they do in the existing gap between `WriteToLog` and the apply; lock-based access queues behind the write lock, as it must.

**The axiom that makes the stall terminate.** After a full clean pass, the shard budget accommodates the largest object's dirty set plus one command's working allocations. Under it, every memory park is eventually woken; a deployment that violates it stalls — visibly, without corruption — and that is the accepted behavior. No per-object dirty caps, no early-flush throttles, no further machinery guards the axiom: engineering an unbounded mechanism chain for a config that should not exist is the endless loop this section refuses to enter. The axiom also binds the ENGINE's own machinery: the flush path must be able to carry any object the budget itself can hold — the data-sync scan heap at 2.5 % of shard memory currently cannot (the recorded key-wedge finding) — and sizing it to the axiom is a `data_substrate` fix, not something write admission compensates for. Conversion is bounded by the threshold (§11). The remaining write-side hole is the flush unit: per-object dirty bytes are already metadata-resident, so admission also refuses (parks) writes to an object whose un-flushed dirty set has reached a cap derived from the data-sync scan heap, and kicks an early flush of that object — keeping the §9 atomic export always smaller than the machinery that must carry it. The alternative — sizing the scan heap to the largest admissible dirty set — is the engine owners' call.

**Reply estimation and COUNT (implemented for paged).** `LogicalBytes()` under-counts materialization, so the whole-object gate now charges `EstimatedReplyBytes(elements) = logical_bytes + elements × 48` — the 48 covering RESP framing plus each element's `std::string` control block and allocator rounding — with `elements` counting what the variant actually emits (two per field for `HGETALL`, one for `HKEYS`/`HVALS`). Both inputs are metadata-resident, so the check is O(1) and runs before a single page is faulted. `HRANDFIELD` with a NEGATIVE count is not clamped to the field count (it means "with repeats"), so it is charged against the same ceiling: `HRANDFIELD k -100000000` on a 200-field hash is now a deterministic too-big error instead of an attempt to build a hundred-million-element reply. The identical unbounded negative count exists on the MONOLITHIC path and predates paging; bounding it there is an open item.

## 9. Checkpoint and Flush

> **Three timestamps to keep apart.** `ckpt_ts` is computed **once per checkpoint round**, node-group wide (`Checkpointer::GetNewCheckpointTs()` → per-shard `CcShard::ActiveTxMinTs`, which mins over lock-holding transactions' **write-lock timestamps** − 1 — read-only and meta-table transactions contribute nothing, and with no qualifying transaction it falls back to the clock). **For EloqKV it does not gate export at all**: the export condition is `if (!VersionedRecord || commit_ts <= to_ts)` (`cc_entry.h:1539`), and object tables instantiate with `VersionedRecord = false`, so the timestamp arm is never evaluated. Eligibility is pure dirtiness — `NeedCkpt()` is `!IsPersistent() && (Normal || Deleted)` (`cc_entry.h:699`), consistent with there being no per-entry ckpt ts to compare against (§4). `ckpt_ts` still bounds the round on the MVCC/versioned paths, which EloqKV does not take. `entry->CommitTs()` is **per object** — the commit ts of the transaction that last modified it. `PageSlot::last_modified_ts_` is **per page** — the `commit_ts` of that page's last content change, or the store row's `commit_ts` for a freshly loaded page (`0` means *unknown*, never "clean"; dirtiness is the separate `flushed_` bit). The version actually written by a flush is the object's `CommitTs()` at export time; it is passed to the post-flush callback and used there, never stored.
>
> Note that `entry->CkptTs()` is *not* a fourth option: for EloqKV's non-versioned hash-partitioned object tables `entry_info_` is plain `EntryInfo`, whose `CkptTs()` asserts false and whose `SetCkptTs(ts)` stores no timestamp — it sets only the `0x10` "latest version flushed" bit, and only when `commit_ts <= ts` (`cc_entry.h:409-448`). Hence the per-object watermark (§4).
>
> Worked example. `h` was last modified at ts 50, so `CommitTs() = 50`; it is dirty, so the round exports it (no ts comparison — see above), writing rows tagged `commit_ts = 50`. While that flush is in flight a transaction frees a page and commits at ts 75, so `CommitTs()` becomes 75 and `pending_delete_` gains a range with `freed_ts_ = 75` that is **not** in the batch. The callback is handed the flushed `commit_ts` 50: draining ranges with `freed_ts_ <= 50` leaves that range alone (`75 <= 50` is false) — correct. Draining against `ckpt_ts` would recycle it (`75 <= 100`) even though its `Delete` was never written, leaking the row permanently. Likewise, a page rewritten at 75 carries `last_modified_ts_ = 75`, fails the guard `75 <= 50`, and keeps `flushed_ == false`. And `SetCkptTs(50)` withholds the `0x10` bit because `75 > 50`, so `h` stays dirty, is not evictable, and is re-exported next round — when the `Delete` goes out.

Currently `ExportForCkpt` (`cc_entry.h:1387`) projects exactly one `FlushRecord` (`cc_entry.h:71`) per entry, `PutAll` (`include/store/data_store_handler.h:88`) writes it as one row, and `UpdateCceCkptTsCc` (`src/cc/cc_req_misc.cpp:1263`) marks the entry clean afterwards. The `FlushTaskEntry` structure already carries multiple vectors (the MVCC archive path fans one entry into many rows), so multi-row-per-entry is not unprecedented — but no path currently emits derived-key rows into the base table.

**Change.**

1. **`ExportForCkpt` projects one indivisible tuple per object.** A paged object exports the metadata row + **every** page dirty at export time + **every** pending-delete range, as a *single* `FlushRecord` that the store handler expands into rows at write time. There is no per-page timestamp filtering, and none is needed — see the invariant below.

    `FlushRecord::payload_` gains a third variant alternative alongside `std::shared_ptr<TxRecord>` and `BlobTxRecord`:

    ```cpp
    struct PagedObjectFlush
    {
        std::string metadata_;                            // metadata row value (copied; small)
        // Dirty pages BY REFERENCE, sorted by page id. A null PageBuf means "delete that
        // page row" — the same convention SetNonVersionedPayload(nullptr) already uses.
        // (Note the opposite sense in PageSlot::buf_, where null means "not resident".)
        std::vector<std::pair<PageId, PageBuf>> pages_;
    };
    ```

    `flush_key_` holds the plain object key and the handler derives each page key from it plus the page id (§5); `commit_ts_` and `cce_` are object-wide already and apply unchanged. Puts and deletes share one vector, so the handler is a single loop — null → `Delete`, non-null → `Put` — as is the post-flush callback: non-null sets `flushed_`, null moves the id from `pending_delete_` to the free list. Keeping the vector **sorted by page id** costs nothing and lets the handler coalesce runs of consecutive nulls into a range delete if the backend offers one (§15).

    Three properties follow from packing the object into one record rather than N:

    - **All-or-nothing is enforced by the type, not by convention.** With N separate records nothing prevents `FlushDataImpl`'s batching from splitting an object across batches; a single record cannot be split.
    - **The post-flush callback needs no page ids at all** (implementation finding, superseding the original "it gets its ids free from `pages_`"). Marking clean by iterating the exported ids looks necessary — `cce_` identifies the object but not its pages — but the ts guard below is *equivalent* to membership in the exported set, so the callback simply walks the object's own resident pages. Every exported page passes the guard (it was dirty at export, and the object's `commit_ts` dominates every page's `last_modified_ts_`); every page not exported either fails it (dirtied after the export, so its ts is newer) or is already clean, where marking is a no-op. Beyond simpler plumbing — the callback signature is just the flushed `commit_ts`, and nothing must keep the record's page vector alive through `UpdateCceCkptTsCc` — this is what makes the callback **safe across a §7 payload swap**: an id-driven callback would stamp those ids on the *successor* block, where the same ids may name pages that were never written, whereas COW-copied slots carry the new transaction's ts and the guard correctly leaves them dirty. No payload-identity check is needed anywhere in the flush path.
    - **Page content is never copied.** Only `metadata_` is materialized; page bytes travel as `shared_ptr` references, which is what keeps the checkpoint scan heap flat (below).

    **Invariant: the object's `commit_ts` dominates all of its parts.** At export time the metadata entry's `commit_ts` is ≥ every dirty page's `last_modified_ts_` and ≥ every pending delete's `freed_ts_`. This holds automatically, because every page write and every page free happens inside a transaction that commits on the object and so advances its `commit_ts`. Note the invariant stands on its own: it is a statement about the object's parts relative to its own `commit_ts`, and does **not** rest on any entry-level timestamp gate — EloqKV's export has none (see the note above), selecting purely on dirtiness. What the post-flush callback compares against is the `commit_ts` actually written by that flush, which the domination property makes sufficient.

    **A partial export would be a correctness bug, not merely inefficient.** §10's durable watermark asserts that every stored page reflects all commands with `commit_ts` ≤ the stored metadata's `commit_ts`. Exporting the metadata at version C while leaving one of its dirty pages at an older version makes that claim false, and replay would then skip commands the page never received. Hence: export the whole dirty set, or export nothing.

    **"Or nothing" is a usable escape.** All-or-nothing means all, or none *this cycle*. If a batch genuinely cannot be formed — a backend limit, or transient pressure — **skip the object and leave it dirty** for the next round. Deferring an export is always safe; only splitting one is forbidden. The object simply stays non-persistent, so it is not evictable and its pages remain (§8).

    **Export by reference, not by copy — this is what keeps memory bounded.** `FlushRecord::SetNonVersionedPayload` today calls `ptr->Serialize(value_)`, materializing a full byte copy into a `BlobTxRecord`. For a paged object that copy is pointless: the page **is** the on-disk form (§4), so the export holds a `shared_ptr` to the page buffer instead. DSS needs nothing extra for this — for object tables `record_parts` already carries the bare blob with ts and TTL in their own arrays, so a page row is one `string_view` aimed at the buffer (§13).

    §7's COW rule makes this safe with no additional mechanism: a page the flush worker holds has `use_count() >= 2`, so a concurrent write copies rather than mutating the buffer mid-serialization.

    What remains is small. The `pages_` vector costs O(dirty pages) × ~16 B for the id-plus-pointer pairs, and holding the page buffers alive adds **nothing**, since a dirty page cannot be evicted anyway. The binding constraint was therefore never the checkpoint scan heap but the volume of dirty data allowed to accumulate before a checkpoint, which point 5 bounds with machinery that already exists.
2. **Per-page clean-marking is a guarded flushed bit.** Dirtiness is `!flushed_`, and the callback sets `flushed_` **only if `last_modified_ts_ <= the flushed commit_ts`** — the per-page transcription of `EntryInfo::SetCkptTs`'s own guard — so a page rewritten during the flush keeps a newer ts, fails the guard, and stays dirty. No per-page "being checkpointed" bit is needed (today's `SetBeingCkpt` is one bit for the whole entry). The callback's other job is to **drain `pending_delete_` into the free list — but only ranges whose `freed_ts_` is at or below that same `commit_ts`**, since pages freed after the export were never in the batch (§4). Nothing is stored as a watermark; the flushed `commit_ts` is a callback parameter, and one value serves both guards.
3. **`IsPersistent` already means all-pages-clean — no new work.** The entry may be treated as persistent, which gates eviction *and* WAL truncation, only once the metadata, every dirty page and every pending page-delete are durable; otherwise truncation could drop log records still needed to recover an unflushed page. This holds for free: any page write advances the object's `commit_ts`, and `EntryInfo::SetCkptTs(ts)` withholds the `0x10` flushed bit whenever `commit_ts > ts`, so an object with any dirty page is never `IsPersistent()` (§4).
4. **One object's rows go in one atomic store batch.** This rests on two halves, in different layers:

    - **The store applies a batch all-or-nothing.** Confirmed for EloqStore: `BatchWriteRecords` commits the whole batch or none of it.
    - **The client must not split an object across batches.** `PreparePartitionBatches` cuts batches on accumulated size, so with one record expanding to `1 + N` rows (§13) that check has to fall on record boundaries. Server atomicity is no help if the client cuts between page 400 and 401 — each half would then commit atomically and *independently*, producing exactly the torn object this rule forbids.

    Together they keep recovery per-object and single-versioned, which is why per-page version-gated replay is **not** needed. Note the second half is the fragile one: it lives in our code, and a later change to the batching heuristic could break it silently.
5. **Bounded dirty set — via the existing shard-wide trigger, with no per-object machinery in v1.** What has to be bounded is how much dirty data accumulates before a checkpoint runs, and `CheckAndTriggerCkptByDirtyMemory` → `NotifyCkpt` (`src/cc/cc_shard.cpp:500`) already does exactly that when estimated dirty bytes cross `dirty_memory_threshold_bytes_`. A per-object targeted flush would be new production machinery — the engine has only a test-only single-entry flush today — and the reference-based export below removes the reason to build it. `dirty_page_count_` (§4) stays useful for observability and as a future hook, but nothing in v1 depends on it.

**Whole-object deletion needs no separate machinery.** `DEL`/`UNLINK` and TTL expiry follow today's path: the entry is marked `RecordStatus::Deleted`, the deletion is flushed like any other dirty state, and only then is the object evicted. Point 1's single-record projection is what makes it work for a paged object — one `PagedObjectFlush` carries the metadata-row delete *and* every page-row delete, so they land together. Two properties make this cheap and page-free: TTL and the page-id list both live in the resident metadata (§4), so neither the expiry check nor the delete fan-out ever faults a page; and page deletes carry keys only, no values, so even a six-figure page count stays a small atomic batch.

The one new requirement relative to a monolithic object: **a logically deleted paged object must keep its page-id list until the deletion is flushed.** A monolithic delete needs only the key, so freeing the payload on delete is harmless today; a paged delete needs metadata *content* to know which page rows to remove. The metadata block therefore survives until the flush completes — the existing dirty-entry-cannot-be-evicted gate already keeps the entry resident (§8) — and its in-flight fetches at the moment of deletion follow §7's swap rule (orphaned, waiters woken to a "not exists" answer).

**Replacement is not deletion, and emits no page deletes.** A paged object can be superseded by a *different* object under the same key: `SET` over a paged hash, `RESTORE ... REPLACE`, a store-destination overwrite (`StoreListCommand`), the blind-overwrite path where `IgnoreOldValue()` skips the fetch entirely (`redis_command.h:1461`, `:6616`, `:7428` — the only three commands that declare it), or a `DEL`+recreate coalesced within one checkpoint cycle. In every such case the old paged object is simply **discarded in memory**: no tombstone is kept, no page deletes are emitted, and the next checkpoint overwrites the key's row with the new object. The old page rows become orphans in the store, reclaimed later by the sweeper (below). This is a leak until swept, never corruption: page rows are reachable only through a live directory, and if a later incarnation of the key converts to paged again and reuses colliding ids, each reused row is overwritten in the same atomic batch as the metadata that references it. The contrast with `DEL`/expiry above is deliberate — a pure delete has the metadata resident and its key-only fan-out is nearly free, while replacement is exactly the case where the old metadata may be absent (blind overwrite under cache miss), so leaning on the sweeper unifies all replacement paths under one rule.

### TTL and store-side reclamation

**Principle: the data store may forget only what the WAL has already forgotten.** The key's store row and the un-truncated WAL suffix are two halves of one recovery input — the row summarizes all truncated history; replay applies the suffix on top. Store-side TTL enforcement that runs on the **wall clock** can destroy the row while the suffix still needs it: a TTL extension commits (WAL only), the store row still carries the old deadline, the old deadline passes, compaction drops the row — and replay of the extension has no base. The object is lost early, with no crash misbehavior anywhere; the flaw is the store unilaterally discarding state the log still references.

**Contract (target design).** Two changes, both store-side, documented here for the data-store owners:

1. The tx service passes its checkpoint/truncation watermark to the store (piggybacked on `BatchWriteRecords` or a periodic control call; for a partition written by several shards, the min).
2. The store's TTL machinery — the compaction filter **and** the read-path expiry filter — treats that watermark, not the wall clock, as "now".

Soundness: a row whose stored deadline precedes the watermark provably has no pending extension in the log — an extension committed at `c` while the object was alive has `c <` the then-current deadline, which (nothing newer having been flushed) is the stored deadline, so `c <` watermark means the record was already checkpointed into the row. A crashed node's watermark freezes, so the base survives *unbounded* downtime. Client-visible expiry is untouched: the engine's lazy check against the payload deadline stays on the wall clock; the store merely holds bytes longer. (Applied to monolithic rows this contract would also make the `RecoverObjectCommand` full-object logging unnecessary — see §15.)

**Interim (v1): slack.** Until the contract lands, the metadata row's store-TTL attribute is written as `logical deadline + S` (config, e.g. one hour), with the wall clock unchanged. Its correctness relies on the operational assumption that a checkpoint containing any TTL extension or removal finishes within `S`; normal checkpoint cadence is far shorter than one hour, so violating that assumption would indicate a serious checkpointing failure. This closes the window for any recovery within `S` of the old deadline; a node that stays down longer than `S` past it can still lose the object. That limitation is accepted and documented for v1. In practice the compromise is relatively safe because huge objects rarely carry refreshed TTLs and checkpoint failure lasting an entire slack interval should itself be exceptional.

Concretely, the annotation rides the existing per-row TTL parameter of the store put API. The export path already reads the object's TTL into the flush record (`obj->HasTTL()/GetTTL()`, `object_cc_map.h:640-652`), and the DSS client passes a per-row `records_ttl` where `0` means "no ttl" (`data_store_service_client.cpp:998`). For a `PagedObjectFlush` the handler therefore sets `records_ttl = ttl_ + S` on the **metadata row** and `0` on **every page row** — the TTL twin's `ttl_` is authoritative, the attribute is derived at flush time, and no row-kind ever gets the other's value.

**Page rows carry no TTL attribute — under both the interim and the target scheme.** The temptation is symmetry with the metadata row, but the soundness argument above covers only rows that are *rewritten on extension*. A TTL reset dirties the metadata alone; clean page rows keep whatever attribute their last flush stamped, so on a long-lived object whose TTL is refreshed, cold pages' attributes go arbitrarily stale and the store compacts pages of a live object — data loss with no crash, and the watermark clock does not save them (it keeps advancing while the node is up). Making page attributes track the deadline would require re-flushing every page on every TTL reset (O(object), re-faulting non-resident pages) or a store-side "touch TTL by prefix" API. Instead pages have no attribute at all, and their reclamation belongs to the sweeper: a page row is garbage exactly when its metadata row no longer vouches for it.

**The TTL transition is a class swap, like the monolithic hash.** `EXPIRE`/`PERSIST` do not set a flag on the paged object; they exchange it for its twin through `AddTTL`/`RemoveTTL` (§3 of [03-data-model.md](03-data-model.md)), which the paged classes **must** override — `TxRecord`'s defaults assert false (`tx_record.h:148`), so a missing override aborts the process on the first `EXPIRE` against a paged key. Both overrides move the core across, so the directory, the resident pages, their dirty bits and the per-txn page state transfer without a copy and without invalidating a page fetch in flight. The deadline itself lives in the twin's metadata payload (§4), and only the metadata row carries a store-TTL attribute; page rows never do.

**TTL resets on paged objects log their plain command image.** The `RecoverObjectCommand` substitution (`object_cc_map.h:1085`) must be bypassed for paged objects: its image would be the §4 metadata-only serialization, and logging that as an `IsOverwrite` record prunes preceding page-writes from the WAL — *write field, reset TTL, crash* would silently lose the write (§10). Plain-command replay is sound here because its base cannot vanish: pages have no attribute to lapse, and the metadata row is protected by the slack (interim) or the watermark contract (target).

**The sweeper.** A background store-side pass, and the single backstop for every orphan source in this design — replacement discards, expired objects' pages, failover leftovers:

- Scan the `\x00EKVPAGE` keyspace (page rows are self-identifying by the §5 signature); peel the object key from each page key.
- A page row is an orphan iff the object key's row is **absent**, **expired**, **not paged metadata** (type tag), or paged but the page id is **outside `dir_` ∪ `large_runs_`**.
- **Horizon guard (required for correctness):** never delete a page row younger than a horizon exceeding the longest possible in-flight checkpoint — the row's store `commit_ts` supplies the age. Without it the sweeper can race a concurrent batch re-creating the same key: read "metadata absent", then delete a page the in-flight batch just wrote.
- The sweeper is needed for space, not correctness, so it may ship after v1; until it does, orphans accumulate at the rate of paged-object replacements and expiries, which the workload argument bounds in practice.

**Residual, accepted:** a single committing transaction cannot be checkpointed mid-flight, so the true floor on atomic-batch size is the largest single transaction against one object (a huge `HSET`/`HMSET`, or a MULTI/Lua touching very many fields), not the object size. Bounded in practice by request and transaction size limits; the day-2 backstop is a cap on single-transaction dirty pages, not per-page versioned recovery.

## 10. Recovery, WAL, and Replay Determinism

The WAL carries **logical commands**, not pages ([03-data-model.md](03-data-model.md) §4). Recovery loads the metadata row, faults pages on demand, and replays post-checkpoint commands on top.

**The durable watermark is the metadata row's `commit_ts`, and it dominates every stored page.** Because a checkpoint writes the metadata row atomically with that cycle's dirty pages (§9) and the metadata row is flushed in *every* dirty cycle (§4), the stored metadata's `commit_ts` carries a strong property: **every stored page already reflects all commands with `commit_ts` ≤ that value.** Replay is therefore gated **per object, not per page** — a command whose `commit_ts` is not greater than the key's stored `commit_ts` has already been applied to every page and is dropped. This is what makes §9's claim (no per-page version-gated replay) actually true, and it is why §4 must not skip the metadata rewrite on value-only updates.

**Deterministic routing is a correctness requirement.** If a logged `HSET` triggered a page split, replay must reproduce the *identical* split, or the in-memory layout diverges from the un-replayed on-disk pages and the hash is silently corrupted. Determinism rests on exactly two facts, and needs nothing more: routing is a pure function of `hash(field)` and the directory, and replay applies a key's commands in log order. Together these make every page's contents identical at every step on primary and replica alike — so **any trigger derived from those contents reproduces exactly**, whether that is `byte_full`, `count == 65535` (§4), or something added later.

The rule is therefore about what must be *excluded*, not which inputs are permitted: nothing may depend on state outside that closure — memory pressure, wall-clock time, allocation addresses, or the iteration order of an unordered container. Never "append to whichever page has room", and never anything derived from `absl::flat_hash_map` ordering.

This joins the existing replay-determinism constraints on self-mutating commands ([03-data-model.md](03-data-model.md) §9) and is the paged-object analogue of them.

### On the replay path, `CommitOn` may simply fault

**Standby apply and recovery replay have no `ExecuteOn`** — they replay a command image via `Deserialize` + `CommitOn` only ([03-data-model.md](03-data-model.md) §9) — so the paged `Execute` override that makes the write set resident on the primary (§6) never runs. This path needs **no pre-fault step of its own**, because the reason `CommitOn` must not fault does not apply here.

That reason is specific to the primary: inside `PostWriteCc` the transaction is **already durable in the WAL**, so it can neither abort nor apply, and a stall there would drag the buffered-command retry path onto every normal commit (§6). In replay the command is *already in that buffer* — `CommitOn` is invoked from `TryCommitBufferedCommands` (`cc_entry.h:1751`) — so faulting costs nothing new. It parks, the page loads, the buffer retries: the mechanism it needs is the one it is already running inside.

What makes this work with a single implementation is `CommitOn`'s **discover-then-mutate** shape (§6): check residency, yield having changed nothing, then mutate in one non-suspending pass. On the primary the discovery is a no-op because the override pre-fetched; on replay it genuinely faults. Same code, different outcome — and no separate helper, no second per-command implementation to drift out of sync.

One mechanical detail survives: `CommitOn` returns `TxObject *`, so it needs some way to report "not ready yet" — a status, or a separate entry point exposing just the discovery half. That is one implementation invoked from two places, not a second concept.

**Four request types apply changes outside `ApplyCc`, and they are not alike.**

| Request | What it applies | Page-fault handling |
|---|---|---|
| `ReplayLogCc` | commands, via the buffer | covered by the drain mechanism below |
| `KeyObjectStandbyForwardCc` | commands — but has a **direct-apply fast path** | needs a fallback; see below |
| `UploadTxCommandsCc` | un-checkpointed command backlog shipped at migration/failover — emplaces a `TxnCmd` into the buffered list under the shipping txn's WriteLock and finishes immediately (`object_cc_map.h:1770`) | purely the drain mechanism below; the request itself never waits |
| `UploadBatchCc` | *records* (`req.EntryTuple()`), not commands | not a `CommitOn` fault at all; see below |

**The standby's fast path bypasses the buffer.** `Execute(KeyObjectStandbyForwardCc&)` checks `!cce->HasBufferedCommandList()` and, when true, calls `tx_cmd->CommitOn(obj_ptr)` **directly** (`object_cc_map.h:~2010`), building a `TxnCmd` for `EmplaceAndCommitBufferedTxnCommand` only otherwise. So on that path nothing is parked and a fault has nowhere to wait. The fix needs no new machinery, because the fallback is the sibling branch: **when `CommitOn` reports "not ready" on the fast path, divert the command into the buffered list**, where the page-fetch completion drain collects it. The fast path degrades into the slow one.

**Only a *fetched* paged object can fault on a standby — a *built* one never can.** Conversion lives in `CommitOn` (§11) precisely so a standby converges on the same representation, so a standby that applied every command itself really does hold a paged object; but it created every page, so all of them are resident and the fault this section is about cannot occur. The state that faults arises when the standby has to read the object back — it restarted, a forwarded command found no entry, `FetchRecord` ran — because a stored paged object is metadata only, its pages living under separate keys, and it therefore arrives with **zero** resident pages. Every subsequent `CommitOn` on it faults until its working set is paged in.

A corollary for §8: **a standby can never shed pages.** `EvictablePageCount()` counts only pages whose `flushed_` is set, `flushed_` is set by the checkpointer, and a standby does not checkpoint. Partial eviction is therefore a primary-only benefit; on a standby, paged objects are bounded only by whole-entry eviction. Not a v1 blocker, but it is why `shed_all_pages` is inert on a standby and cannot be used to construct this state in a test.

**`UploadBatchCc` is a different question, not a `CommitOn` fault.** It installs records during bucket migration. A checkpoint always precedes migration, so the store holds the complete object at some watermark W; the only objects the upload must carry are those updated *after* that checkpoint, and for a paged object the entry ships **the metadata block plus its dirty pages** — one self-contained envelope per object (metadata + `[page id, bytes]` list), for the same reason `PagedObjectFlush` is one indivisible record: metadata and its dirty pages install together or not at all. This is exactly sufficient by the §10 watermark argument: any page *not* shipped is clean, hence unchanged since W, and its store row — which does not move, since migration transfers ownership while §5's key-hash partitioning stays fixed — is already correct and faultable by the target. The destination installs the metadata as the entry payload at the shipped commit_ts and the shipped pages as resident **dirty** pages (`flushed_ = false`, `last_modified_ts_` = the shipped ts), so its next checkpoint flushes precisely what the source had not, and the `SetCkptTs` guard keeps the entry non-persistent until then.

**For the buffered paths, nothing parks on the fetch, because the command is already parked.** In normal execution an `ApplyCc` registers its txn in `PageFetch::waiter_txns_` and is re-enqueued through its `TxPageContext`; here there is no such requester — the command sits in `txn_cmd_list_`, and what is missing is only a *notification* to retry the drain. This is also why tx-number keying of `tx_contexts_` (§4) never has to represent these paths: none of them ever waits on a page fetch.

**Drain-issued fetches are ordinary `FetchHub` entries, and the drain is an ordinary — if reserved — waiter.** The drain's `CommitOn` creates or joins a `PageFetch` exactly as a command would, registering the reserved drain txn `kDrainTxn = UINT64_MAX` (§4) in `waiter_txns_`. One map, one issuance path, and one completion path serve everything: resolve each waiter txn through `tx_contexts_`, pin the arrived page into that context, decrement its `awaited_count_`, and at zero either enqueue `parked_req_` (a command) or — for the sentinel context, whose `parked_req_` is null — re-drive `TryCommitBufferedCommands`. No *request* ever parks on these paths; the sentinel context is bookkeeping, not a parked requester.

**The sentinel context is also the drain's liveness guard, with per-page precision.** §6's argument applies to the drain verbatim: the head buffered command faults P, halts; P arrives (the completion→drain re-drive is synchronous on the shard core, so nothing evicts within one step); the re-run faults Q — and in the async gap before Q arrives, an unpinned P could be shed and the drain would re-fault it forever under memory pressure. Pinning through the sentinel context protects exactly the pages the head command has gathered — a needed page already resident is pinned at discovery, a fetched one at back-fill — while the rest of a large object stays evictable. That precision matters: on a busy standby, buffered commands are present much of the time, so an object-wide "don't shed while buffered" rule would freeze shedding for hot paged objects wholesale. The sentinel's pins release when the head command applies (its gathered set is consumed synchronously), and unconditionally when the buffer empties or a term change clears it; at a §7 swap the sentinel is erased like any context — pins cleared — and the drain is re-driven once, re-faulting against the new payload.

**The whole-record path already does exactly this, so the page path mirrors it.** EloqKV object tables use `ObjectCcMap` (`object_cc_map.h:55-56`), which **overrides** `BackFill` (`:2625`) — note the base `TemplateCcMap::BackFill` does *not* drain, so the override is the one that matters here. After installing the fetched version it checks `HasBufferedCommandList()`, discards commands older than that version, and calls `TryCommitBufferedCommands` (`:2679-2703`), then updates status and either recycles the key lock if the buffer emptied or handles the leftover cases (`:2711+`).

A second, complementary mechanism sits above it: if commands remain after the drain — because they chain from a version newer than the one fetched — `FetchRecordCc::Execute` reopens the fetch for a later version (`src/cc/cc_req_misc.cpp:929-933`), clearing `requesters_` and keeping a null placeholder so the coalescing count stays non-zero. One mechanism applies what the fetched version enables; the other goes and gets a later version.

The paged analogue needs only the first. **A page back-fill re-attempts `TryCommitBufferedCommands` through the reserved `kDrainTxn` context** — the drain registered itself on every fetch it issued (§4), and the completion re-drives when that context's awaited count reaches zero, so a multi-page buffered `CommitOn` triggers one re-drive rather than one per page. The completion still never needs to know "who asked" beyond resolving waiter txns; on the replay paths the only waiter *is* the sentinel. No reopen analogue is required: a page arrives at the object's current version, so re-fetching the same page would loop without progress; draining is what advances.

**Reopen must be suppressed when commands are blocked on a page, not on a version.** This is a correctness requirement, not a tuning detail. For a monolithic object, leftover buffered commands can only mean a version gap, which is why `HasBufferedCommandList()` alone is a sufficient reopen trigger today. For a paged object they can equally mean `CommitOn` stalled on a missing page — and then the metadata row just fetched is *already current*, so the reopen re-reads the same row, the drain stalls at the same command, and it reopens again: an unbounded loop against the store. It also puts a record fetch in flight concurrently with the page fetch that is already outstanding on the same cce, re-pinning and re-registering in `fetch_record_reqs_`.

**The fix is to sharpen the condition, not to special-case paging.** `obj_version_` is ascending in the buffer — the discard loop at `:2687-2695` breaks on `obj_version_ >= commit_ts`, which is only valid on a sorted list — and commands chain, A at version V producing V′ for B. So the head alone decides:

| head's `obj_version_` | meaning | reopen? |
|---|---|---|
| `== cce->CommitTs()` | applicable yet unapplied → **page-blocked** | no; the outstanding page fetch will unblock it |
| `> cce->CommitTs()` | no local command can produce that version → **genuine hole** | yes; only the store can supply it |

```cpp
should_reopen = error_code_ == 0 && !only_fetch_archives_
             && cce_->HasBufferedCommandList()
             && front().obj_version_ > cce_->CommitTs();
```

**Testing *any* remaining command instead of the head would over-fire and spin.** In the ordinary chained case A is page-blocked at `CommitTs()` while B waits at V′ > `CommitTs()`; a scan finds B and reopens, even though B's version will come from A applying locally rather than from the store — and with the page still in flight the next `BackFill` reaches the same state and reopens again, hot-looping for the duration of the fetch. If the head is not applicable, nothing behind it can be either.

This needs no new plumbing: no reason has to propagate out of `CommitOn`, which still only reports "not ready yet" so the drain knows to stop. And it tightens existing behaviour rather than adding a paged special case — "buffer non-empty" is already a loose proxy for "the store holds a version we lack."

The loop converges because the drain applies what it can and halts at the first command it cannot complete — page arrives, drain advances, halts at the next missing page, that fetch is issued, repeat. Per-key ordering is inherent, since the buffer is version-ordered and the drain stops at a gap. As on the whole-record path, the **metadata entry stays pinned** for the fetch duration so it cannot be evicted mid-flight.

**Write-set derivability is entailed, not assumed.** If a command image could not determine its own mutation, replay could not reconstruct the object at all and would already be broken for reasons unrelated to paging; the write set then follows from the mutation by routing (§4). Termination follows too, since each round makes at least one more page resident and the page count is finite. So this is worth a sanity check per command during implementation, but it is not an open risk and cannot force an image widening.

**Discovery may take several rounds, and that is fine here.** `ZADD key score member` is the clear case: the page holding the member's existing entry is addressed by its **current** score, which the image does not carry, so the member index must be read before the ordered pages can be located (§3). On this path that costs nothing structural — each round faults, the drain retries, and it converges.

**v1 keeps it simple: one command at a time.** A command needing N pages issues N concurrent single-page fetches; there is no batching of the page union across pending commands. That is a later optimization if replay throughput ever demands it.

## 11. Conversion

Conversion is **one-way and early**. An object converts monolithic → paged when it crosses a size threshold; it does not convert back on shrink (a collapse path is deferred, §14).

**Threshold.** Convert in the **low-single-digit MB** range, not at the first page overflow. Converting at ~2 pages pays the paging overhead (directory, per-page bookkeeping, fault protocol, expanded checkpoint record) across a large population of medium hashes for almost no benefit; the window where conversion is still cheap *and* paging pays is wide. This is a tuning knob, chosen deliberately.

**Why early conversion matters beyond cost:** it guarantees conversion always operates on a small object, so the conversion flush always fits one atomic batch (§9) — the "convert a 10 GB hash" case never exists.

**Trigger: inside `CommitOn`, by returning the paged twin.** The command's `CommitOn` applies its mutation to the monolithic object as today, then checks the post-image logical size against the threshold and, on crossing, builds the paged representation and returns it — the same object-swap channel `AddTTL`/`RemoveTTL` already use ([03-data-model.md](03-data-model.md) §3). Locating the trigger in `CommitOn` is what reaches every apply path: standby apply and WAL replay run `Deserialize` + `CommitOn` only (#509), so an `ExecuteOn`-side trigger would never fire there and representations would diverge by construction. The converting `CommitOn` never faults — its input is the fully resident monolithic object — and its output is a metadata block whose pages are all dirty. Cost is O(object) synchronous on the shard core, bounded by the low-MB threshold above.

**The policy belongs to the object, not to a command.** "A hash is paged iff its post-image crossed the threshold" is a statement about hashes, so the check is one shared function — `MaybeConvertHashToPaged` (`include/redis_paged_hash_object.h`) — and *every* path that can grow or import a hash calls it on its post-image: `HSET`, `HSETNX`, `HINCRBY`, `HINCRBYFLOAT`, and both `RESTORE` outcomes (`src/redis_command.cpp`). Attaching it to a subset instead makes representation a function of *how* a hash was built rather than of what it is — two hashes of identical content in different formats — and leaves everything grown by the uncovered mutators permanently monolithic, which is the scaling problem paging exists to remove. The helper is total and self-guarding: it returns its input unchanged for a null object, an already-paged object, or a non-hash (`RESTORE` hands it every type), so call sites need no type test of their own.

Three properties are the helper's alone to preserve, and are why the check is not open-coded per command:

- **Eligibility.** An object holding a record too large for one page cannot be paged at all (§4 has no out-of-line storage in v1), so `AllRecordsFit` gates the conversion and such a hash stays monolithic — behaving exactly as a hash does today, however far past the threshold it grows. It converts on the first post-image that no longer contains an oversized record.
- **TTL.** TTL is carried by the object's *class*, not a field (§5), so the conversion must land on the twin matching the source: converting through `FromFields` alone yields the non-TTL class and the deadline is dropped, turning an expiring key permanent. The helper re-applies the source's TTL through `AddTTL` before returning.
- **Page size.** The new object's layout parameters come from config exactly once, at the moment of conversion, and are recorded in its metadata; every later access reads them from the object (§4).

**Determinism: per-stream, from logical inputs — and a DEPLOYMENT INVARIANT: every replica of a keyspace runs the same `paged_hash_convert_threshold` and `paged_hash_page_size`.** The trigger must be a function of the post-image's *logical* size (`logical_bytes_`) and the threshold — never memory pressure, residency, or allocator state (the same closure rule as §10's splits). Given identical config, primary, standby, and replay convert at the same command, since commands apply in fixed per-key order, and every record any node admits satisfies every node's §4 inline cap. The identical-config requirement is load-bearing, not stylistic (a review found the earlier "skew is safe" claim unsound): command admissibility is checked at `Execute` on the ACCEPTING node under ITS config, while standby apply and replay run `CommitOn` only (#509) and can reject nothing — so a dark node (threshold 0, stock record sizes) paired with a converted replica would feed the replica records its pages cannot physically hold, which no amount of splitting can place. Until out-of-line large values exist (§14), "threshold skew is harmless" and "dark servers keep unrestricted record sizes" cannot both be true; v1 keeps the second and forbids the first. `Put` enforces the invariant's backstop: a record that cannot fit an empty page dies with `LOG(FATAL)` in every build — the same loud-death policy replay corruption follows — rather than a Debug-only assert and a Release directory-doubling loop. What layout divergence remains LEGAL under identical config (a crash replayed with the same settings, restart ordering) stays safe for the original reasons: replication ships logical commands, never pages; a standby reload adopts the flushed layout wholesale; a promoted standby's all-dirty export rewrites the object coherently (§9). And as before, once converted, every layout parameter — page size, hash algorithm and seed — comes from the object's metadata, never from config (§4).

**Crash safety needs no separate protocol.** No staged conversion flush (page rows first, then a single-row overwrite of the key as the commit point) is needed: §9's flush already writes the metadata row and every page as one atomic `PagedObjectFlush` batch, so the store transitions monolithic → paged in a single commit with no orphan window. Crash before that checkpoint → the monolithic row stands and replay re-runs the conversion deterministically from the WAL; crash after → the object is paged and the watermark has advanced. (A replayed stream under a *changed* threshold may convert at a different command than the original run — harmless, since the original run's paged state was never flushed, or the base would already be paged.)

**Read compatibility.** Objects written before this feature remain monolithic on disk and must still load and operate; `DeserializeObject` dispatches on the type tag (§5), so both formats coexist indefinitely.

### Gating

Paged rows are unreadable by pre-feature binaries, so the feature needs a gate. Two dimensions, and they call for different mechanisms:

| Dimension | Mechanism | Reversible? |
|---|---|---|
| does this backend support paged rows? | the paged flush is implemented in the **DSS client** (both `ELOQDSS_*` backends); non-DSS handlers do not implement it, so the conversion knob must stay off there | n/a |
| may this cluster **read** paged rows? | none once shipped; the binary must always be able to | **no** — data exists |
| should new objects **convert** to paged? | **runtime**, cluster-aware, default off | **yes** |

**A build macro alone cannot do this job**, whatever else it is used for. During a rolling upgrade some nodes run the new binary and some do not, and a paged row written by an upgraded node is unreadable by a peer that has not restarted. Only a runtime, cluster-aware check expresses "every node can read this format now"; a build flag decides what one binary *can* do, never what its peers can.

Note also that only *one* of the three rows above is switchable. Once a single object has been converted, no binary serving that keyspace may lose the ability to read it — so the read path is permanent from the moment the feature ships, and "turning paging off" can only ever mean "stop creating new paged objects."

**That switch probably already exists**: the conversion threshold above is a config value, and setting it to disabled stops conversion with no separate boolean. One knob, and one needed regardless.

**There is no compile-time gate.** The runtime threshold is the feature's only switch, and it gates creation alone: `RedisEloqObject::DeserializeObject` compiles the `PagedHash` and `TTLPagedHash` cases unconditionally in every build. The same asymmetry the table above states rules out ever guarding the read side instead — it looks like the more cautious choice and is in fact a data-loss bug: any path that can still write a paged row while the read path is compiled out produces a store the binary cannot read back, an abort in `DeserializeObject` on the next restart. The dark default is exercised, not assumed: the CI-facing rehearsal (`tests/unit_cc/paged_dark_feature.py`) restarts a server with the feature dark on a store containing paged rows and asserts they stay readable and writable while new hashes stay monolithic.

## 12. HSCAN

`HSCAN` currently ignores its cursor and returns the entire hash in one reply, because `absl::flat_hash_map` has no stable iteration order — the `TODO(lzx): Support Cursor and Count` at `src/redis_hash_object.cpp:582` says exactly this. A stable page directory removes the obstacle.

**Cursor = the `hash32` lower bound of the next entry to emit — nothing else.** The scan enumerates the object in **ascending hash order**, and that single value is the whole cursor: 0 starts the scan, 0 returned means complete (Redis's own convention). This works because the directory routes by the **top** `global_depth` bits of the hash (§4), so a directory entry *is* a hash-prefix range and sequential entry order *is* ascending hash order. A split or a directory doubling refines ranges without reordering them, so a hash bound names the same frontier at every depth — the guarantee that every element present for the whole scan is returned at least once holds with no cursor transformation at all. On resume, the walk re-derives the directory entry as `bound >> (32 − depth)` at the *current* depth, scans that page from the first slot with `hash32 ≥ bound`, and on exhausting a page jumps the bound past every consecutive entry sharing it (so a page with `local_depth < global_depth` is emitted once, not once per pointing entry).

**Redis's reverse-binary-increment cursor is NOT correct for this directory, and the trap is worth recording** because it is the obvious thing to reach for. Reverse-binary is the solution to Redis's *low-bit* bucket indexing, where growth appends a **high** bit to the index and bucket `j`'s children are `j` and `j + 2^d` — the reversed-order threshold set is closed under that child relation. Here growth appends a **low** bit (entry `j`'s children are `2j` and `2j+1`), for which that closure fails: a reverse-binary walk resumed from a stale index after a doubling both revisits and **skips** directory entries (a depth-4 → 12 doubling leaves it covering only 3328 of 4096). The skip is also easy to miss in a test: a skipped entry loses data only once its page is no longer shared with a visited sibling, so page sharing masks the hole until enough splits make entries exclusive — which is why `TestScanDoublingMidScan` forces post-doubling splits before asserting.

The intra-page cursor must be the **hash value, never a slot offset**. The slot array is dense, sorted, and mutated in place (§4): a delete shifts every later slot down, so a positional cursor resumes past a stable element that has moved behind it — violating the one guarantee SCAN makes (duplicates are permitted; skips are not). A value cursor over the sorted array cannot skip, because an element's hash never changes. Two rules complete it: resume at the first slot with `hash32 ≥ cursor`, and **never split a run of equal hashes across replies** — emit the run to its end even past `COUNT`, which is advisory; runs are almost always singletons. The run rule is also what keeps the resume value unambiguous: the slots are sorted, so a mid-scan cursor is strictly greater than the last emitted hash and therefore never 0 — without it, a stop inside a run of genuine 0-hash entries would return cursor 0 ("complete") and skip the run's tail. `COUNT` bounds **work, not results** (again Redis's semantics): entries scanned count against the budget whether or not `MATCH` emits them, which is what keeps a non-matching pattern from walking — and faulting — the entire object in one call.

Because `HSCAN` faults a bounded number of pages per call, emits, and moves on, it is also the **memory-safe way to traverse an object larger than the shard budget** — the incremental counterpart to the materialize-or-fail `HGETALL` family (§1 non-goals).

## 13. Engine Mechanisms (`data_substrate`)

Protocol-layer work is in `include/redis_hash_object.h` / `src/redis_hash_object.cpp` (paged representation behind a common interface so command code is representation-agnostic — note the per-type `Execute(XCommand&)` / `Commit*` overloads are non-virtual today and reached by a static downcast, so paged objects need either a representation query on `TxObject` plus a branch per command site, or virtualized overload sets, §6), the hash commands in `src/redis_command.cpp` (fault-set computation + restart-safe `ExecuteOn`), and **reserved-prefix key validation** in `src/redis_service.cpp` — reject keys beginning with `\x00EKVPAGE` next to the existing `MAX_KEY_SIZE` check, and on the `RESTORE` / RDB-import paths in `src/redis_rdb_restore.cpp`, which admit keys that never passed through command parsing (§5).

The **type-generic layer is engine code**: `PageFrameTable` lives in `tx_service/include/` beside `paged_tx_object.h`, and `PagedTxObject` itself carries the shared implementation of its engine-facing virtuals (§4 "Ownership: two layers") — it is the seam *and* the machinery, not a pure interface with a separate mixin, because the protocol is meant to have exactly one implementation. Nothing in either is Redis-specific — frames, pins, faults, shed, install, dirty iteration — so a paged type in any API layer (EloqDoc, EloqSQL) implements only the type hooks and its own layout. Engine-side:

| Area | File | Mechanism |
|---|---|---|
| command contract | `include/tx_command.h` | `ExecuteOn` gains a `Yield` outcome. **Its signature does not change** — the object declares its missing pages and the apply path fetches them (§6 refinement) — so no monolithic command is touched. Only paged objects ever return `Yield`. (EloqKV is in any case the only consumer of the object-command path; EloqDoc and EloqSQL do not implement `TxCommand`/`ExecuteOn`.) |
| apply path | `include/cc/object_cc_map.h` | defer lock acquisition until `ExecuteOn` returns non-`Yield`, **on the paged, not-fully-resident path only** — monolithic objects keep today's sequence; handle `Yield` by setting the new **`BlockOnPageFault`** state and parking — no lock held on the speculative path, lock retained on the contended-path re-run and in multi-command transactions; the resume branches on lock state and re-runs under the lock where held, resetting `result_` first. `BlockOnFetch` and its fetch-before-lock assert (`:310-315`) stay untouched, scoped to whole-record fetches. The two steps at `:614-633` keep their position relative to `ExecuteOn` — only the lock moves past them — so TTL-expiry still precedes execution and an expired key short-circuits without loading a page (§6) |
| page fetch | `src/cc/cc_shard.cpp`, `src/cc/cc_req_misc.cpp`, `include/cc/template_cc_map.h` | subclass `FetchRecordCc` as `PageFetch` (§4) — routing hash, page id, waiter txn list, orphan flag, and its **own `Execute()` completion override**, so the shared whole-record completion gains no branches — and make `DeserializeCurrentPayload` virtual so a paged object installs the bytes as page P. `BackFill` needs a page mode that **skips** `SetCommitTsPayloadStatus` (the entry is already `Normal`) and never turns a missing page row into `RecordStatus::Deleted` — a missing row for a **live** page id is corruption (§4's lifecycle guarantees every live id a checkpoint flushed has a row): the fetch completes with a corruption error, waiting contexts error, the `ApplyCc` fails, the transaction aborts, and the client receives a corrupted-object error. Page fetches **bypass `fetch_record_reqs_` entirely**: that map is keyed by `LruEntry*` and exists to coalesce whole-record fetches, whereas page fetches are per `(object, page id)`. The entry's `FetchHub` on `cc_lock_and_extra_` owns both the in-flight page set and the lifetime of the outstanding requests (§7) — the request's address must remain stable while the I/O is outstanding, which is why it holds `unique_ptr`s and why the engine's own map is node-based. On completion, resolve each `waiter_txns_` entry through the object's `tx_contexts_` and re-enqueue a parked request only when its `awaited_count_` reaches zero — **`ApplyCc` gains no fields**: awaited pages, pins, and the parked-request pointer are all object-resident per-txn state, which is also what lets `PostWriteCc` release a write txn's pins at `CommitOn` by tx number (§4, §6). N single-page fetches give the two-pass property without a new batch request; a batched `FetchRecordsCc` is a later round-trip optimization. Must tolerate partial issuance (`FetchRecord` returns `Retry` when the store is busy) and **unpin on abort/term change** |
| eviction | `src/cc/cc_shard.cpp`, `include/cc/template_cc_map.h` | clean pass invokes the object's shed-clean-pages hook, which sheds 10 % of evictable pages per visit (minimum one) by the object's internal LRU, skipping dirty and pinned pages; a metadata-only object then falls to the existing `IsFree()` whole-entry path (§8) |
| checkpoint | `include/cc/cc_entry.h`, `src/cc/local_cc_shards.cpp`, `src/cc/cc_req_misc.cpp` | add a third `FlushRecord::payload_` alternative, `PagedObjectFlush` — metadata blob plus one id-sorted vector of dirty pages **by reference**, in which a null `PageBuf` denotes a delete — so `ExportForCkpt` emits one indivisible record per object and `PutAll` expands it into rows keyed via §5's composite key, landing them in the object key's partition. The post-flush callback reads its page ids straight from `pages_`, sets `flushed_` on those passing the ts guard, and drains `pending_delete_` (§9). Note `IsPersistent` = all-pages-clean needs *no* change — the existing `SetCkptTs` guard delivers it (§4) |
| flush payload | `include/cc/cc_entry.h`, `store_handler/*` | add the `PagedObjectFlush` variant (§9) and teach the handlers to expand one record into many rows — see "Store handler changes" below. No per-object targeted checkpoint is needed in v1: the existing shard-wide `CheckAndTriggerCkptByDirtyMemory` suffices |
| apply paths outside `ApplyCc` | `include/cc/object_cc_map.h`, `src/cc/cc_req_misc.cpp`, `include/standby.h` | four requests apply changes here — `ReplayLogCc`, `KeyObjectStandbyForwardCc`, `UploadTxCommandsCc` and `UploadBatchCc` (§10). No pre-fault step is needed: `CommitOn` may fault and the buffered-command retry handles it. But the standby's **direct-apply fast path** (`:~2010`) bypasses the buffer, so a not-ready `CommitOn` there must divert the command into the buffered list; and `UploadBatchCc` installs *records* — a checkpoint always precedes migration, and a paged object updated since ships as one envelope of metadata + dirty pages, installed at the target as a metadata payload with those pages resident-and-dirty (§10). Three further things needed: a way for `CommitOn` to report "not ready yet" (it returns `TxObject *` today); a page-fetch completion that re-drives `TryCommitBufferedCommands` through the reserved `kDrainTxn` context at awaited-count zero (§10), mirroring the drain `ObjectCcMap::BackFill` already performs for whole records at `object_cc_map.h:2679-2703` (the override at `:2625`, not the non-draining base in `template_cc_map.h`); and **sharpening `FetchRecordCc`'s reopen trigger** (`cc_req_misc.cpp:929-933`) from "buffer non-empty" to `front().obj_version_ > cce_->CommitTs()`, so it fires only on a genuine version hole — a head command blocked on a *page* would otherwise reopen a record fetch that changes nothing, spinning against the store and racing the outstanding page fetch on the same cce. No request parks on the fetch: the command is already in `txn_cmd_list_`, and the reopen path shows a waiter-less fetch is a supported shape (§10) |
| store keyspace | `store_handler/*` | derived-key rows partitioned by the hset key (§5); export tools and any full-store scan must reassemble metadata + pages |
| locking | `include/cc/non_blocking_lock.h` | **add a grantability test** — "would this request be granted?" — that reports the answer *without* enqueuing the requester, so `ApplyCc` can defer acquisition until `ExecuteOn` returns non-`Yield` (§6). Also extend `KeyGapLockAndExtraData` with the lazily allocated `FetchHub` — live page-fetch map plus orphan vector (§7): `IsEmpty()` must additionally require it empty before recycle, `Reset()` asserts it empty, `ClearTx()` leaves it alone. Called only on the paged, not-fully-resident path; the monolithic path never reaches it. It must mirror the real acquisition exactly: the requested `LockType` across the full ordered lattice, upgrades from a mode the transaction already holds, and blocking-queue fairness. No downgrade primitive is needed — a lock that is never acquired is never released |

### Store handler changes

**v1 targets EloqStore** — the `ELOQDSS_ELOQSTORE` default, and the main data store going forward. The implementation lives in the DSS client plus the EloqStore server side, so it serves both DSS-backed configurations (`ELOQDSS_ELOQSTORE` in production, `ELOQDSS_ROCKSDB` in dev builds). The non-DSS handlers — `rocksdb_handler`, `dynamo_handler`, `bigtable_handler` — do not implement the paged flush; enabling the conversion threshold on one of them is unsupported (the notes below are kept because they show what a second backend path would cost).

The assumption to break is **one `FlushRecord` → one row**; a `PagedObjectFlush` produces `1 + N`.

**DSS (`data_store_service_client.cpp`) accommodates this well.** `PrepareObjectData` (`:5854`) pushes exactly one entry into each of five parallel per-row arrays — `key_parts`, `record_parts`, `records_ts`, `records_ttl`, `op_types`. Expanding to `1 + N` rows means pushing that many entries into each; the arrays are already per-row, so nothing structural changes. Two consequences worth knowing:

- **`BatchWriteRecords`'s assert is not in the way.** `record_parts.size() % parts_cnt_per_record == 0` (`:5318-5319`) holds trivially because object tables set `parts_cnt_per_record = 1` (`:393-397`) — the general 5-part path is for non-object tables.
- **Zero-copy comes for free here.** For object tables `record_parts` carries the bare blob (`rec->EncodedBlobData()`), with ts and TTL travelling in their own arrays — so a page row is one `string_view` aimed straight at the page buffer, no header concatenation. Lifetime is safe because those views are non-owning and the `shared_ptr` in `PagedObjectFlush` keeps the page alive until the flush completes, exactly as `BlobTxRecord::value_` does today.

**RocksDB (`rocksdb_handler.cpp`), if it is ever added, needs the loop change but not scatter-gather.** `SerializeFlushRecord` (`:383`) concatenates `[deleted][commit_ts][blob]` into a buffer for `write_batch.Put` (`:563-706`). That copy can stay: it is per-row and transient, bounded by one page, and RocksDB copies into its own memtable and WAL regardless. The by-reference requirement of §9 is about `data_sync_vec_`, which persists for the whole flush — not about a reused per-row write buffer. `SliceParts` remains available if profiling later says otherwise.

**Key derivation needs one shared encoder.** Both handlers must build `\x00EKVPAGE<object key><kind><page_id BE>` (§5) from the record's object key plus each page id, and the **fetch path must build the identical key**. Put that encoder in one place in `data_substrate` and call it from all three, rather than teaching each backend the format.

**Batch boundaries must fall between records, never inside one.** `PreparePartitionBatches` accumulates `write_batch_size` and cuts a batch when it grows too large; with one record yielding many rows, that check has to happen at record granularity or the all-or-nothing guarantee (§9) is lost at the last hop. A single object exceeding the batch limit is written oversized rather than split.

**`FlushRecord::partition_id_` already does the right thing.** It is per-record, so with one record per object every derived row inherits the partition the export chose — the §5 co-location invariant lands for free. Had the export emitted N records, each would have needed the object key's partition set correctly and independently.

**The EloqStore server side must expand too.** The client sends `1 + N` rows per object in one `BatchWriteRecords`; the service has to apply them as a unit, since §9's all-or-nothing guarantee only holds if the final write is atomic. **Confirmed with the EloqStore owner (2026-07): `BatchWriteRecords` is all-or-nothing, and batch size has no hard upper bound — an oversized object's batch is written, never rejected or silently chunked.**

**Other backends stay unsupported, deliberately.** `dynamo_handler` and `bigtable_handler` implement the same interface but are not part of v1; the `WITH_DATA_STORE` gate should make a paged build on them a configure-time error rather than a runtime surprise.

Finally, page rows live in the persistence keyspace even though they are absent from the `CcMap`, so consumers that read the store directly must handle them: the RDB/AOF exporters (`src/tools/eloqkv2rdb`, `src/tools/eloqkv2aof`, see [07-persistence-and-tools.md](07-persistence-and-tools.md)) and any logical (as opposed to physical-snapshot) backup. Physical store snapshots are transparent. Bucket migration rests on two facts, both now settled: a checkpoint always precedes migration (so the store holds the base at a watermark), and post-checkpoint updates ship as metadata + dirty pages per object (§10); §5 co-location guarantees the target can fault everything else, since store rows do not move when ownership does.

## 14. Deferred / Out of Scope

- set / list / zset paged representations (§3 sketches the routing; the generic half is built to accept them)
- **`DUMP`/`RESTORE` and the RDB/AOF exporters on paged objects.** All four read the object as one serialized blob, which a paged object is not — its fields live in separate page rows needing reassembly. Until that lands, `DUMP` on a paged object returns `RD_ERR_PAGED_UNSUPPORTED_CMD` ("this command is not yet supported on a paged object") rather than crashing, and the exporters skip page rows and abort on a paged metadata row (§13). This is dark by default: a hash is paged only past the conversion threshold, so in a default deployment `DUMP` works on every (monolithic) hash. The reassembly is the same two-pass metadata+pages walk the bucket-migration envelope already does
- resume cursors inside a single command, so a large ordered range can stream and release pages as it passes them; without this a big range holds reply + all its pages at once (acceptable per §1 non-goals)
- read-ahead / sibling prefetch for forward scans
- a **background** merge/compaction pass, able to fault and reclaim the space that opportunistic in-command merging (§6) declines to chase, plus paged → monolithic collapse. Opportunistic merging itself is not deferred and needs no special handling, since it is discretionary; note that the §4 sizing remarks (a usually-empty free list, a rarely-fired tombstone path) assume little merging occurs, and should be revisited if a background pass makes page frees common
- raising `MAX_OBJECT_SIZE` (§1) — possible once flush is no longer single-blob, but a separate decision
- hierarchical or paged directory for pathological page counts (§4 sizing says this is far off)
- **out-of-line large values** (`large_runs_`, §4): the page-level descriptor encoding is implemented and pinned, but allocation and rewrite are not, so a field+value larger than one page cannot be stored in the paged representation. This is contained rather than latent: conversion declines such an object (it stays monolithic and fully functional) and the paged write paths refuse the record with a specific error, so neither path can reach `Put`'s precondition. What the work buys is removing that restriction — a 200 KB value in a hash is legal at the 256 MB object limit, and today such a hash simply never pages
- **zero-copy page I/O**: a `DataStoreHandler` read-into-caller-buffer path, plus `io_uring` registered buffers and one vectored submission per large-value run. Fixed-size pages, aligned page buffers, and the bytes-are-the-working-form layout (§4) are chosen specifically so this lands as an addition rather than a rewrite
- `RENAME`/`RENAMENX`/`COPY` are unimplemented today (no `AddCommandHandler` registration; the names appear only in the metrics table at `include/redis_service.h:665`, and the vendored handlers are commented out at `include/redis/server.h:3549-3551`). If added later, note that hset-key-derived page keys (§5) make them O(pages) store rewrites rather than Redis's O(1) — acceptable, but it is a consequence of the key scheme worth recording. Cheap rename would require an immutable object-id indirection, trading away co-location.

## 15. Open Questions

1. ~~**Per-command audit**~~ **DONE for the hash-only v1 command set; re-opens per future type.** For each command: does the paged `Execute(XCommand&)` override make resident everything the matching `CommitOn` touches, so the residency assert never fires on the primary (§6)? The hash overrides are exercised end to end by the TCL hash suite under `--paged_hash_convert_threshold=1` (every hash is paged), the fault matrix (yield/shed/stall/error/abort interleavings), and the replay/standby harnesses; the audit question below remains the checklist for `ZADD`, list value-search, and the other types when they arrive.

    That is the whole question. Write-set derivability on the replay path is *not* a second property to verify — it is entailed by replay working at all (§10) — and the replay path needs no pre-fault step, since `CommitOn` may simply fault there and the buffered-command retry it already runs inside will handle it.

    Verification reuses #509's method (`ae73a05`): per-command reasoning plus differential replay coverage. The relationship is close — #509 asked whether `CommitOn` reproduces the right *value* from the image, this asks whether the override covers the right *page set* — so the same command list and harness apply.

    Two to check early: `ZADD`, which needs two fault rounds because the old score is data rather than argument, so the override must iterate rather than compute its set in one step (§3); and list value-search (`LREM`, `LINSERT BEFORE`, `LPOS`), where the write set is discovered by scanning rather than predicted. Scope is ~16 commands for a hash-only v1, against ~114 across all types.
2. ~~**Store-side LSM behavior**~~ **DROPPED.** EloqStore is the main data store going forward, so a RocksDB compaction characterization buys nothing; EloqStore-side behavior is its own project's concern and gets measured there if page-row volume ever surfaces as a cost.
3. **Observability semantics** (status: `MEMORY USAGE` answers with the logical size; `OBJECT ENCODING`/`DEBUG OBJECT` do not exist as commands in EloqKV at all, so there is nothing to extend until they do; metrics await the plumbing decision) for paged objects: `OBJECT ENCODING`, `MEMORY USAGE` (resident vs logical), `DEBUG OBJECT`, plus new per-object metrics (page count, resident count, fault rate) — and per-state park counters, which the distinct `BlockOnPageFault` state makes separable from whole-record `BlockOnFetch` parks and lock blocks (§6).
4. **Does EloqStore expose a prefix or range delete?** The §5 encoding gives every page of one object the shared prefix `\x00EKVPAGE<object key>`, so if that range can be dropped in one call, `DEL`/expiry fan-out needs no page-id enumeration at all and `large_runs_` stops being required for that purpose — a worthwhile simplification of §9. It would also make the sweeper's reclaim step (§9) a single call per orphaned object.
5. ~~**Should the sweeper subsume `pending_delete_` entirely?**~~ **RESOLVED: no — `pending_delete_` stays.** The two mechanisms are sized for different frequencies, and collapsing them would put a rare-path tool on a common path. In-life frees (merges, large-value shrinks) happen during ordinary operation, so they delete their rows in the flush that observes them and recycle the ids promptly — which is what keeps `free ⇒ no store row` (§4) true and makes id reuse safe without cancellation logic. The sweeper is the **last resort**, for the genuinely rare orphan sources: a paged object overwritten by a non-paged one, and failover leftovers. Overwriting a large paged object with a different type is uncommon in practice, so sweep can run infrequently. Its value is that it makes the reasoning easy and v1 sound — every orphan has *some* eventual reclaimer — not that it replaces prompt reclamation.
6. **Should the watermark TTL contract (§9) retire the monolithic recover protocol?** The checkpoint-clock rule is not paged-specific: applied to all rows it makes `RecoverObjectCommand` full-object logging unnecessary for monolithic objects too, shrinking the WAL and deleting a protocol (including the #509 remote-owner recover fields). Bigger, separately-gated change — confirm nothing else depends on store-side wall-clock expiry.

## 16. Invariants (to hold and to test)

- **Routing lives in the metadata; no page references another page.** Forced independently by eviction safety and fault-set predictability (§2).
- **Page rows partition on the hset key.** Fetch locality, single-batch checkpoint atomicity, and migration correctness all reduce to this (§5). Structural rather than a discipline: one composite `TxKey` type serves both the fetch and the flush, and its `Hash()` returns the object key's hash, so the two paths cannot disagree about placement.
- **The `\x00EKVPAGE` prefix is reserved and enforced in BOTH directions, not merely documented.** Inbound: user keys beginning with it are rejected by `CheckKeyAdmissible`, called over every physical key on every command path — single-key, multi-object, queued MULTI/EXEC, WATCH, ZScan — so a collision is impossible rather than improbable. Outbound: page rows are filtered out of every online store scan (`BackfillForScanNextBatch`, both branches) before pattern or type processing, so an internal key can never reach a client. A gate on only one path, or in only one direction, is a hole: `MSET <derived page key>` would overwrite a live page row, and `SCAN TYPE list` would return hash pages (page layout version 1 collides with the List type tag). Test: `tests/unit_cc/paged_key_security.py`, `tests/unit_cc/paged_scan_leak.py` (§5).
- **Page assignment and splitting are deterministic functions of `(directory, field)`** — never iteration-order- or occupancy-opportunistic (§10). Sorted-by-routing-bits slots make a split a contiguous partition, so this holds structurally rather than by convention (§4).
- **The field hash must be byte-stable across processes and releases** — `MurmurHash3_x64_128` with a pinned seed; **never `absl::Hash`**, which is process-randomized and would corrupt data on restart (§4).
- **All pages are exactly `page_size` bytes and their buffers are aligned** — required by the future zero-copy path; alignment comes from the heap's aligned-allocation API, since no page pool exists in v1 (§4, §14).
- **The on-disk page bytes are the in-memory page.** No per-page deserialization, so a fault costs no decode, and flush and COW are both `memcpy` (§4).
- **A large value lives out-of-line in a page run whose id list is recorded in the metadata** — never a chain threaded through pages, and contiguity is best-effort locality only, never a correctness requirement (§4). **The out-of-line threshold is a FRACTION of the page — `record > page_size / k`, k ≥ 8 — never "does not fit a page".** The fraction is what bounds inline bucket capacity away from 1, and with it the directory at Θ(N^(1+1/k)); fits-a-page semantics admit near-page-size inline records, whose capacity-1 splitting drives the directory to the birthday bound — Θ(N²) entries, measured as tens of MB of directory for thousands of records. **Large-value runs use their own smaller page size: `P_L = max(N / k, 4 KB)` for normal page size N** — 16 KB at the 128 KB default. A uniform page size would waste up to 1−1/k of every page in the band just above the threshold; P_L = N/k makes every out-of-line value (which is > N/k by the threshold rule) fill at least one whole chunk, so utilization is > 50 % worst case and → 100 % for big values. The 4 KB floor exists because below it per-chunk overhead dominates — each chunk is a store row, a frame slot, and a fetch unit; the floor only bites for N < 32 KB, where the >50 % guarantee softens, an accepted corner since sub-32 KB normal pages are themselves degenerate. Two consequences: the fixed-buffer invariant becomes per-KIND uniformity (all hash pages are `page_size` bytes; all large-value buffers are P_L bytes — the page-key `kind` byte already distinguishes the rows), and since a large-value chunk is opaque bytes — unlike a hash page, whose fixed size is load-bearing for in-page addressing — the LAST chunk's store row may be written at its exact remaining length, eliminating the residual on-disk waste; the in-memory buffer still comes from the P_L pool.
- **Page ids are allocated on net growth, never per write.** An overwrite reuses its run's own ids (§4).
- **Nothing from the store is trusted before it is validated — and the validation is enforced on the store's REAL paths, not only in the helpers.** The checks: page images pass the page-format checks (`PageView::ValidateImage`) plus the type's semantic cross-checks (metadata entry count, local depth, routing identity) before install; metadata rows are bounds-checked at parse (page size, id high-water, run count) and cross-checked between sections (live ids below the high-water, entry counts summing to the field count). The enforcement (a review round found the original wiring bypassed it): the backfill parses the metadata row length-bounded and FALLIBLY (`RedisEloqObject::DeserializeObject(buf, avail, offset)` → `DeserializeBounded`) BEFORE the entry's commit ts or status are touched, so a corrupt row leaves the entry untouched and surfaces to every requester as a deterministic store error — never a retry loop, never a Release-mode ignored assert; the fetch completion classifies per payload — a benign discard (superseded payload, id freed mid-flight) wakes its waiters successfully, while corruption on a LIVE page (missing row, wrong-size row, rejected image) errors them — and allocates the page buffer only after the row's size is validated. Unbounded parsing survives only where the input is self-written: the WAL/replay images. Tests: `TestCorruptInputRejected` (every field corrupted in turn, untouched control), `TestBoundedStoreParse` (full truncation sweep through the store entry, both twins), `tests/unit_cc/paged_corrupt_page.py` (both live-page corruption classes → specific error, clean recovery after disarm) (§5, §13).
- **Identical `paged_hash_convert_threshold` and `paged_hash_page_size` on every replica of a keyspace** — a v1 deployment invariant, not a preference. Admission runs at `Execute` under the accepting node's config; apply paths cannot reject (#509); so config skew lets one node admit records another node's pages cannot hold. `Put`'s both-builds `LOG(FATAL)` is the backstop that turns a violated invariant into a loud death instead of a silent memory blowup (§11, §14).
- **Every record in a paged object is within the inline cap: `max(page_size/8, 4 KB)`, bounded by page capacity.** Guaranteed on entry, not assumed, and enforced at the COMMAND when conversion is enabled: `HSET`/`HSETNX`/`HINCRBY`/`HINCRBYFLOAT` refuse an over-cap field/value with a specific error in `ExecuteOn` — pre-WAL, so `CommitOn`, which cannot reject anything on the replay and standby paths, never meets a record it cannot place. Dark servers (threshold 0) keep stock behavior. `RESTORE` imports never error on the cap; the conversion policy declines instead and the import stays monolithic (§11). The `page_size/8` term keeps inline bucket capacity ≥ 8 at production page sizes, which is what keeps the extendible directory Θ(N^(1+1/8)) rather than the capacity-1 birthday bound Θ(N²); the 4 KB floor is a testing affordance — small-page harnesses may run one record per page, accepting the degenerate directory for convenience, and the capacity guarantee is claimed only for pages ≥ 32 KB. Records above the cap belong in the out-of-line large-value runs (§14, deferred). Test: `tests/unit_cc/paged_oversized_record.py`, plus the cap, guard, and RESTORE arms in `paged_object_command_test` (`TestInlineRecordCap`).
- **A free page id never has a store row — within one object incarnation.** Every freed page goes to `pending_delete_` and becomes free only once its `Delete` is durable; it is never reallocated in between. The derived free list is therefore `[0, next_page_id_)` minus live minus `pending_delete_`. This is what makes a `Put` and a `Delete` for the same page key in one batch structurally impossible (§4). Rows left by a *previous* incarnation of the key (replacement orphans, §9) sit outside the invariant: they are never referenced, are overwritten in the same atomic batch as the metadata if a later incarnation reuses their ids, and are reclaimed by the sweeper.
- **`free_ranges_` is a canonical interval set** (sorted, disjoint, non-adjacent); allocation takes only prefixes so ranges never split, and inserts coalesce with both neighbours. **`pending_delete_` is append-only in `freed_ts_` order**, disjoint by construction, drained as a prefix — and entries with different `freed_ts_` are never coalesced, since a merged range would have to pick one ts and the smaller choice recycles ids whose `Delete` was never written (§4).
- **A checkpoint export of a paged object is all-or-nothing**: the metadata row plus *every* dirty page plus *every* pending delete. Partial export would falsify the §10 durable watermark, which asserts that every stored page reflects all commands up to the stored metadata's `commit_ts`. This is enforced by the type — one `PagedObjectFlush` record, which the batching layer cannot split — rather than by convention (§9).
- **The object's `commit_ts` dominates all of its parts** — at export time it is ≥ every dirty page's `last_modified_ts_` and every pending delete's `freed_ts_`, because every page write and free commits as a transaction on the object. This is what makes per-page filtering at export unnecessary (§9).
- **No checkpoint watermark is ever stored, and the post-flush callback needs no page-id list either.** A page is dirty iff `!flushed_`. Both decisions that would need a watermark — setting a page's `flushed_` bit, and recycling a freed range — happen inside the post-flush callback, which receives the flushed `commit_ts` as its *only* input and applies one guard to both: set `flushed_` only if `last_modified_ts_ <= commit_ts`, recycle a range only if `freed_ts_ <= commit_ts`. Both guards exist because pages freed or written *between* the export and the callback were never in that batch. EloqKV's non-versioned `EntryInfo` deliberately has **no** `CkptTs()` — it asserts false, and `SetCkptTs` sets only the `0x10` flushed bit — and a paged object does not reintroduce one, only transcribing the same guard per page (§4, §9).
- **`last_modified_ts_` is a faithful timestamp, never a sentinel.** It records the `commit_ts` of the page's last content change, and a freshly loaded page takes the store row's `commit_ts`. `0` means *unknown*, consistent with the rest of the system; clean/dirty lives in `flushed_` (§4).
- **A yielding `ExecuteOn` pass has no side effects**; the reply is built only on the pass where all pages are resident (§6).
- **A command's gathered page set stays pinned until the command completes** — liveness, not just safety; without it the retry livelocks. The pin covers the *whole* gathered set: a needed page already resident at discovery is pinned then, not only pages that arrive by fetch — either can otherwise be shed during the async gap (§6). The buffered-command drain holds pins identically, under the reserved `kDrainTxn = UINT64_MAX` context (`parked_req_` null ⇒ wake = re-drive the drain), released when the head command applies, the buffer empties, or a term change clears it (§10).
- **Concurrent faulting on one object is permitted.** Speculative faulters hold no locks and block no one; a writer's split or a payload swap merely forces the resumed run to recompute (§6, §7). Per-page `waiter_txns_` lists dedupe the reads, and the per-txn awaited count gives single wake (§6).
- **No reference into page or directory memory survives a yield** — copy out or re-look-up; beware `EloqString` views (§6).
- **`CommitOn` does not fault on the primary, and asserts residency there.** The paged `Execute` override is explicitly responsible for making the matching `CommitOn`'s write set resident — code, not an emergent invariant — so a violation fails loudly in test. On the replay path the rule does not apply: `CommitOn` may fault, because it runs inside the buffered-command retry already (§6, §10). One `CommitOn` implementation serves both, via its discover-then-mutate shape.
- **Pins across the WAL gap are correctness, not optimization.** Pages fetched by `Execute` must survive `WriteToLog` to reach `CommitOn`; without the pin, eviction can take them and the assert fires (§6).
- **Every memory-admission refusal ends with the waiter woken or aborted — never merely parked.** The three defects this rule exists for were all liveness holes, not correctness ones: a branch that only called `AbortRequestsAfterMemoryFree` freed nobody (it fails only `AbortIfOom()` requests, which ordinary Redis commands never set), a campaign abandoned mid-flight left waiters with nothing to re-trigger the cleaner, and a "fail after N fruitless campaigns" rule stopped the retry cycle by hanging the reader. Reclamation is best-effort and asynchronous, so the only safe default is to wake and let the command retry; a spin is recoverable, a hang is not (§8).
- **Commit can never fail; under memory admission it may stall — holding the write lock for the duration.** Inside `PostWriteCc` the transaction is already durable in the WAL and can neither abort nor be un-applied, so no `CommitOn` outcome may be an error. The mutation phase stays synchronous on the fast path; the §8 admission design lets the apply park "blocked on memory" at the discover-then-mutate boundary (and retry-with-yield mid-mutation), terminating under the §8 axiom. The lock is released only after `CommitOn` completes, at both call sites (`PostWriteCc`, `ApplyCc`) — releasing it early would let a queued writer commit against the pre-image of a durable transaction (§6, §8).
- **The standby's direct-apply fast path must divert to the buffer on a not-ready `CommitOn`.** It bypasses `txn_cmd_list_` when the buffer is empty, so nothing is parked there and a fault would have nowhere to wait (§10).
- **Per-key log order is inherent, not enforced.** `txn_cmd_list_` is version-ordered and the drain halts at a gap, so a page-blocked command cannot be overtaken; the standby's direct-apply fast path preserves this by diverting to the buffer once one is present (§10).
- **`FetchRecordCc` reopens on `front().obj_version_ > cce->CommitTs()`, not on "buffer non-empty".** A head command applicable at the current version is page-blocked, and the store holds nothing that would help; reopening there re-reads an already-current row, stalls at the same command and loops, while racing the page fetch outstanding on that cce. Testing any command rather than the head over-fires the same way, since a chained successor legitimately awaits a version that local application will produce (§10).
- **Lock acquisition is deferred only for paged, not-fully-resident objects**, leaving the monolithic path untouched; where it applies, a yield never *releases* a lock: the speculative path yields holding nothing, while the contended-path re-run and multi-command transactions yield with the lock retained and resume under it (`BlockOnPageFault`; `BlockOnFetch` and its fetch-before-lock assert stay whole-record-only). On the contended path the speculative run is only a fault-set probe and `ExecuteOn` is re-run under the lock, or the reply would reflect state from before the wait; the re-run must **reset** `result_`, since several reply types accumulate rather than assign (§6).
- **An expired key never loads a page.** TTL lives in the metadata block, so expiry is evaluated before the fault set is computed and short-circuits the command onto the "does not exist" path (§6).
- **COW refcount ≠ pin count**, and refcount may only be acquired on the owning shard core (§7).
- **Page state is payload-scoped; fetch requests are entry-scoped.** The awaited count, pins, and parked-waiter identity are object-resident, keyed by tx number (`tx_contexts_`, §4), and die with their block; every outstanding page fetch lives in the entry's `FetchHub` (§7), which survives payload swaps. Fetch waiter lists hold tx numbers, so stale entries resolve to nothing and no teardown deregistration exists. At *any* payload swap — paged, monolithic, or deleted successor alike — the hub splices its live map into its orphan vector, flagged discard-on-complete (the flag carries the incarnation boundary: a fetch for one incarnation's page id must never install into a successor's); the block's contexts are erased (pins die with the block), fetch-parked requests are eagerly re-enqueued, and every resume re-runs the §6 routing check (§7).
- **A dirty page must be flushed before it can be evicted**; `IsPersistent` means metadata + all pages + all pending deletes are durable — which the existing `SetCkptTs` guard already guarantees, since any page write advances the object's `commit_ts` (§8, §9).
- **One object's checkpoint is one atomic store batch.** Two halves: EloqStore commits a batch all-or-nothing (confirmed), *and* the DSS client must cut batches on record boundaries so an object is never split across two — each half would otherwise commit atomically and independently, which is the torn object the rule exists to prevent (§9, §13).
- **The stored metadata's `commit_ts` is the object's durable replay watermark and dominates every stored page's timestamp.** This requires flushing the metadata row in every dirty cycle, atomically with that cycle's dirty pages — it is what allows replay to be gated per object rather than per page (§4, §9, §10).
- **A logically deleted paged object keeps its page-id list until the deletion is flushed** Enforced CENTRALLY, at the one place deletion is decided: `TxCommand::CommitOn(obj, PagedCommitContext)` — when a command's `CommitOn` returns null for a paged object, the overload retires the block (fetches orphaned, parked readers woken, pins dropped, every buffer released) and returns it RETAINED and tagged `deletion_retained_`, so every commit path — normal execution, the replay/standby drain, backup, bucket migration — inherits the rule from one definition instead of re-implementing it (per-path copies produced a round of defects: the live paths were fixed one by one while the replay drain still installed the null). The tag, not payload nullness, is the deletion signal (`CcEntry::DrainedPayloadStatus`); the post-flush callback releases the block once the deletion is durable. One case is out of the rule's reach BY DESIGN: a deletion replayed onto a NON-RESIDENT prior (crash replay of a DEL is an overwrite command, applied without fetching the old metadata) has no inventory in memory — its page rows become sweeper debt, the same accepted category as replacement (§9); forcing a record fetch per replayed deletion was judged not worth the mechanism. What is retained is the ID LIST, not the data. A deletion IS a payload swap in every §7 sense, and runs the swap rule first: in-flight fetches orphaned (a successor incarnation restarts page ids at 0, so an old fetch must never install into it), parked readers woken, every pin dropped — after which the deletion releases EVERY page buffer immediately; the deletion export walks ids and emits null buffers, never touching page bytes. Holding a multi-megabyte resident set until the next checkpoint to carry a list of integers would be its own leak.
- **The data store may forget only what the WAL has already forgotten.** Store-side TTL enforcement must never destroy a row that an un-truncated WAL record needs as its replay base. Target: the store's TTL clock is the checkpoint/truncation watermark, passed in by the tx service. Interim: the metadata row's store-TTL attribute is written with slack `S`, and a recovery later than `S` past the old deadline is the accepted, documented loss window (§9).
- **Page rows carry no store-TTL attribute.** Their attributes cannot track TTL resets (only the metadata row is rewritten), so annotation would let the store compact pages of a live object. Page-row reclamation belongs exclusively to the sweeper, guarded by the age horizon (§9).
- **TTL resets on paged objects log the plain command, never the `RecoverObjectCommand` substitution** — a metadata-only image logged as an overwrite prunes preceding page-writes from the WAL and silently loses them on replay (§9, §10).
- **Conversion is triggered inside `CommitOn`** — apply the mutation, check the post-image's *logical* size against the threshold, return the paged twin — so it fires identically on the primary, standby apply, and WAL replay, which never run `ExecuteOn`. It commits durably as one atomic §9 batch; there is no staged protocol and no orphan window. Cross-node config skew is FORBIDDEN — identical `paged_hash_convert_threshold` and `paged_hash_page_size` on every replica is the §11 deployment invariant, because admission runs on the accepting node's config and the apply paths cannot reject (#509). The self-healing properties (command-replicated content, store reloads adopting the flushed layout, a promoted standby's all-dirty re-export) cover only the layout divergence that remains legal under identical config (§11).
- **Conversion is a property of the object, so every path that produces a hash post-image applies the same policy** — `HSET`, `HSETNX`, `HINCRBY`, `HINCRBYFLOAT`, `RESTORE` — through the one shared `MaybeConvertHashToPaged`, which is also the single place the two things a per-command copy would drop are kept: an ineligible object (an oversized record) stays monolithic, and a TTL survives the class change. Covering a subset makes representation depend on how a hash was built rather than on what it is. Test: `tests/unit_cc/paged_conversion_policy.py` (§11).
- **`RedisObjectType` values are append-only**; the metadata's format-version byte carries layout evolution (§5).

Testing emphasis: replay determinism (kill mid-sequence, replay, assert identical layout); crash injection between page flush and metadata flush; and the interleaving of yield × partial eviction × concurrent fault × term change.

## 17. Testing

Tests were built alongside each implementation phase (the plan's per-phase gates) and are consolidated here. Three tiers:

**Unit (four binaries, `tests/unit_cc/*.cpp`, built by the project, run standalone).** `paged_hash_core_test` — page format, both metadata codecs (sectioned row, malformed sweeps, crafted page-manager sections), splits/doubling incl. the 65535-count valve, layout determinism, the SCAN guarantees (mid-scan delete, mid-scan doubling with forced splits), deep-copy independence, the core-vs-mirror property test. `paged_eviction_test` — §8: LRU order, dirty/pinned exclusion, the 10 % policy, convergence, COW, shed with shared buffers. `paged_flush_roundtrip_test` — §9: flush/evict/fault round trips, the guarded post-flush callback, pending-delete drain scoping, deletion fan-out, row shape and TTL placement, crash between page and metadata flush, conversion determinism. `paged_object_command_test` — every `Execute(XCommand&)`/`Commit*` on both twins in both residency states (reply arms resident, fault arms on a metadata-only reload), the class-twin swap, the `PagedTxObject` surface incl. a minimal non-hash type for the base defaults, the pin protocol, install/id-lifecycle arms, the pinned production hash, and the conversion policy's decision arms (null, already-paged, non-hash, below threshold, ineligible, eligible, eligible-with-TTL).

**Integration (server-level, `tests/unit_cc/*.py` + the TCL hash suite).** Single node: the fault matrix (yield × shed × stall × store-error × abort × concurrency), payload swap with a fetch in flight, abort with a parked command, HLEN/HKEYS consistency, the HSCAN protocol contract, **key admission** (`paged_key_security.py`: the reserved prefix refused on every command path, DUMP-on-paged refusing cleanly), **scan isolation** (`paged_scan_leak.py`: no page row reaches KEYS/SCAN/SCAN TYPE after a checkpoint — verified against a deliberately unfiltered build, which leaks 32 rows), **oversized records** (`paged_oversized_record.py`: conversion declines and the paged writes refuse, rather than aborting on `Put`'s precondition), **the write-side memory park** (`paged_commit_memory_park.py`: a paged commit stalls rather than fails while memory is refused, lock held, reads live, nothing dropped), **conversion policy** (`paged_conversion_policy.py`: every mutator and the `RESTORE` import converting, an ineligible hash declining, and a TTL surviving the class change), **store-input validation** (`TestCorruptInputRejected`: every field of a page image and of a metadata row corrupted in turn, each rejected, with an untouched control), replay/restart (`drain_stalled=True` asserted — the test refuses to pass vacuously), the serving-window probe, and the §11 dark-restart rehearsal. Two nodes (`standby_cluster.sh`: MinIO + a standalone log service + a leader/standby pair): standby paged apply (asserts `drain_page_stall` fired on the restarted replica), **conversion agreement** (`standby_conversion_paths.py`: each of the four mutators grows its own key on the leader and the replica must match it in field count *and* representation — the apply path runs `CommitOn` only, so a policy that fired on one side and not the other would leave the same content in two formats — plus TTL survival on both sides), and graceful failover with full-restart coherence. The `hash`, `scan`, `string`, `incr`, `multi` and `expire` TCL suites run with `--paged_hash_convert_threshold=1`, so every stock behavior executes against the paged representation. `dump` and `multi2` run against a dark server instead: `threshold=1` pages even a three-field hash, and `DUMP` on a paged object is a refusal while reassembly is deferred (§14).

**Coverage criterion.** The feature's own code — `page_frame_table.h`, `paged_tx_object.h`, `page_key_codec.h`, `redis_paged_defs.h`, `redis_paged_hash_core.h`, `redis_paged_hash_object.h` — holds **100 % line coverage** under the four unit binaries, measured by `tests/unit_cc/paged_coverage.sh` (gcovr, `--fail-under-line 100`). Exactly two exclusion categories exist, each marked inline with its reason: failure arms of invariant self-checks (no public mutator can violate the invariant, which passing tests prove), and closing-brace destructor code that guaranteed copy elision makes unreachable. Feature code threaded through pre-existing engine files (the replay drain, the fetch hub, the swap rule, deferred promotion) is exercised by the integration tier and mapped invariant-by-invariant in the plan's §16 checklist.

## 18. Key Files

| File | Role in this design |
|---|---|
| `include/redis_hash_object.h`, `src/redis_hash_object.cpp` | current monolithic hash (`hash_map_` at `:235`, `serialized_length_` at `:238`, serialization `:77-180`, `HSCAN` cursor TODO at `src/redis_hash_object.cpp:582`); gains the paged representation |
| `include/redis_object.h`, `src/redis_object.cpp` | `RedisObjectType` on-disk tags (`:44-60`) and `DeserializeObject` dispatch (`src/redis_object.cpp:33`) — new paged tags land here |
| `src/redis_command.cpp` | hash command `ExecuteOn`/`CommitOn`; fault-set computation and restart-safe execution |
| `src/redis_service.cpp` | `MAX_OBJECT_SIZE` (`:208`), conversion threshold config |
| `data_substrate/tx_service/include/tx_command.h` | `ExecuteOn` contract; `ExecResult` gains `Yield` |
| `data_substrate/tx_service/include/cc/object_cc_map.h` | `ApplyCc` execution: lock sequence (`:281`, `:542`, `:626`, `:778-803`, `:867`), dirty payload (`:632`), fetch park/resume (`:342-348`, `:508`), block release (`:755-765`, `:1160-1164`) |
| `data_substrate/tx_service/include/cc/cc_entry.h` | `FlushRecord` (`:71`) — gains a `PagedObjectFlush` variant alternative; `ExportForCkpt` (`:1387`) — emits one such record per paged object |
| `data_substrate/tx_service/src/cc/cc_entry.cpp` | `IsFree` (`:104`) and the clean/dirty predicates that become per-page |
| `data_substrate/tx_service/src/cc/cc_shard.cpp` | `FetchRecord` (`:2172`), `Clean` (`:1681`) — page fetch and partial eviction hooks |
| `data_substrate/tx_service/include/cc/template_cc_map.h` | `BackFill` (`:10028`) — whole-record materialization, the model for page back-fill |
| `data_substrate/tx_service/src/cc/cc_req_misc.cpp` | `UpdateCceCkptTsCc::Execute` (`:1263`) — per-page clean-marking |
| `data_substrate/tx_service/include/cc/cc_request.h` | `ApplyBlockType` (`:7028`) — gains `BlockOnPageFault` (§6) |
| `data_substrate/tx_service/include/store/data_store_handler.h` | `PutAll` (`:88`) — multi-row-per-object flush |
| `src/tools/eloqkv2rdb`, `src/tools/eloqkv2aof` | store-keyspace readers that must reassemble paged objects |
