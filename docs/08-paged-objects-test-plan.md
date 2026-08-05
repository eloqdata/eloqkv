# Paged Hash — Comprehensive Test Plan

> Companion to [08-paged-objects.md](08-paged-objects.md) and
> [08-paged-objects-plan.md](08-paged-objects-plan.md). This is the release
> qualification plan for the v1 paged hash implementation. It is intentionally
> broader than the tests already present in `tests/unit_cc/`: existing tests are
> retained as regression tests, while this plan adds systematic command,
> lifecycle, concurrency, failure, durability, and deployment coverage.

## 1. Purpose and scope

The objective is not merely to execute every line. It is to establish that a
paged hash:

1. has the same logical and RESP-visible behavior as a Redis hash for every
   supported command and argument class;
2. remains correct in every representation and residency state;
3. remains live and internally consistent through page faults, eviction,
   memory pressure, concurrent access, transaction aborts, payload replacement,
   checkpointing, restart, replay, standby catch-up, and failover;
4. either recovers or fails deterministically on store and metadata corruption,
   never hanging, looping, silently losing acknowledged data, or installing a
   partial object; and
5. satisfies the design invariants in §16 of the design document on every
   supported EloqStore deployment.

The storage scope is **EloqStore only**. Run the integration matrix against the
`ELOQDSS_ELOQSTORE` configuration. Other data-store handlers are not release
gates for v1.

The object scope is the v1 paged hash and its TTL twin. Out-of-line large-value
runs, reverse conversion to monolithic, online bucket migration, and DUMP
reassembly remain deferred. Tests must nevertheless verify the current explicit
behavior at those boundaries: oversized command records are rejected, an
ineligible RESTORE remains monolithic, DUMP returns the documented unsupported
error for a paged object, and deployment tooling refuses unsupported topology or
configuration changes.

“All inputs” below means exhaustive protocol grammar arms plus exhaustive
equivalence and boundary classes. Exhausting every byte string is impossible;
binary-safe property tests and deterministic fuzzing cover the remaining input
space.

## 2. Test oracles

Every test must use at least one independent oracle. Checking that the server
did not crash is never sufficient.

### 2.1 Logical model

Maintain a reference `map<byte-string, byte-string>` plus TTL deadline for each
test key. Apply the same operation to the model and EloqKV, then compare:

- point reads and integer replies exactly;
- `HMGET` ordering and nil placement exactly;
- `HGETALL` as a field-to-value map;
- `HKEYS` and `HVALS` as multisets, because reply order is unspecified;
- `HLEN` with both the model cardinality and the independently decoded
  `HKEYS` reply length;
- `MEMORY USAGE` with the documented paged logical-size semantics;
- key existence, type, and TTL state after every lifecycle transition.

The model must update only after a successful server reply. On a timeout,
disconnect, or injected failure, first discover the committed state rather than
assuming whether the operation landed.

### 2.2 Differential Redis/Valkey oracle

Run command/protocol cases against the upstream Redis or Valkey version whose
protocol EloqKV claims to implement. Normalize only behavior Redis documents as
unordered or nondeterministic:

- sort `HKEYS`, compare `HVALS` as a multiset, and decode `HGETALL` into a map;
- for `HRANDFIELD`, compare cardinality, uniqueness/repetition rules,
  membership, field/value association, and error behavior rather than exact
  random choices;
- for `HSCAN`, apply Redis's documented weak-scan guarantees rather than
  expecting a snapshot or identical cursor values;
- compare TTLs with a bounded timing tolerance while requiring the same
  existence transition.

Outside those exceptions, compare the RESP type and value exactly: integer vs
bulk/status reply, nil bulk vs empty bulk vs empty array, array nesting, and the
documented error class/message. Run the same assertion under every RESP version
EloqKV advertises.

Any intentional EloqKV divergence must be named in the test and linked to the
design. A difference must not be silently normalized.

### 2.3 Structural oracle

Unit tests and Debug integration tests need a test-only paged-object inspector
that reports, without mutating the object:

- representation and TTL twin;
- page size, global depth, directory, page entry counts, field count, and
  logical bytes;
- live, resident, dirty, pinned, reserved, pending-delete, and free page ids;
- outstanding fetch ids and waiter counts;
- per-page last-modified timestamp and flushed state.

Tests should assert `CheckInvariants()` and zero leaked pins, reservations,
waiters, and fetches at every quiescent boundary. Using DUMP failure as the sole
“is paged” probe is acceptable for old regression scripts but not for the new
suite.

### 2.4 Durable-store oracle

Provide a test-only EloqStore row inspector or administrative helper that can:

- enumerate the metadata row and derived page rows for one logical key;
- decode the page-key envelope;
- report row status, commit timestamp, byte length, and TTL attribute;
- read one atomic batch's result after a forced checkpoint; and
- inject a missing, truncated, wrong-sized, wrong-page, or stale row.

The inspector is required to prove atomicity and row-shape claims directly. A
successful client read alone can be satisfied from cache and is not a durable
oracle.

## 3. Coverage dimensions

The full Cartesian product is too large, so coverage has two layers:

1. **Mandatory exhaustive axes:** every supported command, every input class,
   every object lifecycle state, both TTL twins, all four deployment lanes,
   and every named fault outcome.
2. **Combinatorial axes:** transaction shape, residency, concurrent actor,
   checkpoint timing, and memory state. Run all pairwise combinations and the
   high-risk triples explicitly listed in §8–§11. A nightly seeded state-machine
   test explores higher-order combinations.

Every generated case records its seed, operation trace, topology, WAL setting,
page size, conversion threshold, memory limit, and checkpoint interval.

### 3.1 Object lifecycle states

Use the following states consistently in test names and matrices:

| ID | State |
|---|---|
| `A` | key absent |
| `M` | monolithic hash below conversion threshold |
| `MC` | mutation that crosses the conversion threshold |
| `PD` | newly paged, resident, and dirty before its first checkpoint |
| `PC` | paged, fully resident, and clean after checkpoint |
| `PP` | paged and partially resident; requested set mixes resident and missing pages |
| `PM` | metadata-only paged object after full page shedding or store reload |
| `DW` | transaction owns a COW dirty paged payload while readers can still observe the committed payload |
| `PF` | paged object with a checkpoint in flight |
| `PX` | logically deleted/expired paged object awaiting durable delete fan-out |
| `R` | same key recreated after deletion, expiry, or overwrite; old page rows may still exist as orphans |

Every command-level unit test must run against `PC`, `PP`, and `PM` where the
command is meaningful. Mutators must additionally run against `PD` and `DW`.
Lifecycle integration tests must cover the complete path:

```text
A -> M -> MC -> PD -> PC -> PP -> PM -> PC
                         -> PF -> PC
                         -> PX -> A -> M -> MC -> PD
                         -> overwrite with another type -> A/M -> paged again
```

The **delete-all-and-start-over** sequence is release-blocking, not an
incidental HDEL arm:

1. create and checkpoint a multi-page hash;
2. shed at least one page;
3. delete all fields using one HDEL, several HDELs, and a transaction;
4. assert the key is absent (`EXISTS=0`, `TYPE=none`, point reads nil);
5. checkpoint and restart;
6. recreate the same key first below, then above, the threshold;
7. verify old page rows are never referenced, new page writes are coherent,
   and repeated delete/recreate cycles reuse ids safely.

### 3.2 Layout and input configurations

Exercise at least:

- page sizes: the minimum testable size, 4 KiB, 32 KiB (cap transition), the
  128 KiB production default, and one large supported size;
- conversion thresholds: disabled, exactly one byte below the post-image,
  exactly equal, one byte above, and the production setting;
- fields/values of length 0, 1, varint boundaries (127/128, 16383/16384), slot
  and page-fit boundaries, inline-cap−1/equal/+1, maximum legal protocol size,
  and embedded `\0`, `\r\n`, high-bit, and invalid UTF-8 bytes;
- object keys that are empty where supported, binary, exactly at and one byte
  around the page-key-adjusted maximum, prefixed by the reserved page-row
  fingerprint, and admitted through single-key, multi-key, transaction,
  script, and RESTORE/import paths;
- hash distributions that put fields on the same page, adjacent pages, distant
  pages, and both children of a page immediately before and after a split;
- empty strings for both field and value, duplicate field arguments, duplicate
  values, and a large number of tiny records;
- objects with one page, many pages, a deep directory, heavy dead bytes before
  compaction, and many pending deletes.

Use test-only reduced object/reply limits for exact boundary tests so the PR
suite does not need hundreds of megabytes per case; retain at least one
production-limit qualification case to detect narrowing and integer overflow.

Use an injected deterministic hash function in unit tests to construct layout
boundaries exactly. Use the production hash in differential, integration, and
soak tests.

## 4. Unit-test plan

Unit tests should remain fast, deterministic, and independent of a running
EloqStore. Extend the four existing binaries where ownership is clear; add
focused engine harnesses rather than growing one monolithic test executable.

### 4.1 Page format, directory, and id lifecycle

Extend `paged_hash_core_test` with property/state-machine tests for:

- insert, update shorter/equal/longer, delete, compact, and reinsert around
  every free-space boundary;
- equal hash32 values with different binary fields, including a long collision
  run that must not be split by HSCAN pagination;
- split at every local depth, repeated directory doubling, maximum supported
  depth, and the 65535-entry count valve;
- deterministic layout from identical logical command streams, including
  delete/reinsert and replay from multiple checkpoint cut points;
- `field_count`, `logical_bytes`, per-page counts, routing identity, local
  depth, free ranges, pending deletes, and id high-water after every operation;
- freeing the last record in a page, freeing the last field in the object,
  checkpoint-draining the delete, and reallocating the same id;
- repeated empty/recreate cycles and replacement by a monolithic object;
- canonical serialization independent of hash-map iteration order;
- malformed metadata and page images: every truncation, length overflow,
  varint overflow, unknown version/kind, duplicate page id, out-of-range id,
  count mismatch, routing mismatch, wrong local depth, overlapping ranges,
  non-canonical pending deletes, valid page under the wrong id, and trailing
  garbage according to the chosen format policy.

Add libFuzzer/AFL targets for `PageView::ValidateImage`, metadata bounded
deserialization, and page-key decoding. The assertion is “never crash, hang,
allocate beyond configured bounds, or accept a state that fails invariants.”
Seed corpora with every valid format version and every handcrafted malformed
case above.

### 4.2 Frame table, COW, eviction, and admission

Extend `paged_eviction_test` and the frame-table portion of
`paged_object_command_test` to cover:

- LRU touch order for every read and write accessor;
- shedding 0, 1, 10%, and all eligible pages; convergence across repeated
  clean passes; dirty and pinned exclusions;
- COW with two and three payload owners, checkpoint-held buffers, and shedding
  one owner while others retain the buffer;
- reservation acquisition as an atomic set, duplicate ids, an already
  resident id, partial allocation failure, release on every completion result,
  move/destruction, and coalesced waiters;
- committed-only, dirty-only, both-payload, and no-reservation canonical buffer
  selection; assert allocation counts, not only `ReservedCount()`;
- two simultaneous admitted fault sets whose combined size reaches the exact
  budget; the next allocation must be refused before it occurs;
- `fault_set == capacity`, `capacity+1`, and arithmetic-overflow-sized counts;
- memory release after install rejection, page free, payload swap, abort, term
  change, and object destruction.

Use an allocation spy so issue-9 regressions fail if an unchecked canonical
buffer is allocated while either payload owns a matching reservation.

### 4.3 Command-semantic matrix

Add a table-driven `paged_hash_command_matrix_test` or expand
`paged_object_command_test` so each row runs against non-TTL and TTL twins and
against `PC`, `PP`, and `PM`. A missing-page first pass must have no side
effects; after pages are installed, the authoritative pass must produce the
same result as the logical model. Mutators also run through the CommitOn-only
replay/standby path.

| Command | Mandatory semantic and boundary classes |
|---|---|
| `HSET` / `HMSET` | absent key; insert; update same/shorter/longer value; mixed insert+update; duplicate fields in one command; empty field/value; binary data; fields on one/many pages; split caused by first/middle/last pair; cap−1/equal/+1; object-size−1/equal/+1; atomic refusal leaves every pair unchanged; exact integer vs `OK` reply |
| `HSETNX` | absent key/field; existing field; empty/binary value; cap boundaries; page resident/missing; concurrent creator; TTL preserved; reply 1/0 exactly |
| `HGET` | absent key, absent field, empty value, binary value, resident/missing page, wrong type, expired key; exact bulk/nil behavior |
| `HMGET` | zero/one/many fields as allowed by grammar; mixed present/missing; duplicate fields; caller order differs from page order; fields on same/different pages; partial fault set; wrong type; exact array order and nil placement |
| `HEXISTS` | absent key/field, present field with empty value, wrong type, expiry, resident/missing page |
| `HLEN` | absent, one, page-count boundaries, after updates, after partial/all deletes, during delete/recreate cycles, after restart; agree with model and HKEYS count |
| `HSTRLEN` | absent key/field, empty value, binary value with NUL, varint boundaries, wrong type, resident/missing page |
| `HDEL` | missing fields; duplicate arguments; one/many pages; mixture of present/missing; delete first/middle/last slot; free a page; delete every field; fields become empty before argument list ends; delete-all then recreate; exact removed count and key disappearance |
| `HINCRBY` | absent key/field; existing positive/negative/zero; INT64 min/max and ±1 overflow; 32-bit boundaries; leading sign/zeros; invalid bytes, whitespace, decimal/exponent, embedded NUL; rendered record cap; update without changing cardinality; exact error leaves value unchanged |
| `HINCRBYFLOAT` | absent key/field; positive/negative/zero; `-0`; exponent input; rounding-sensitive values; very large/small finite values; NaN/±Inf result rejection; whitespace, malformed, embedded NUL; canonical output formatting; rendered cap; failed operation leaves bytes unchanged |
| `HKEYS` | absent/empty/one/many; binary fields; all pages resident/partial/absent; reply bound below/equal/above estimate; compare field multiset |
| `HVALS` | duplicate values, empty/binary values, absent/one/many, partial residency, reply-bound edges; compare value multiset |
| `HGETALL` | absent/one/many, binary data, partial residency, reply-bound edges; decode pairs and compare map; never expose page rows |
| `HRANDFIELD` | no count; count 0, 1, cardinality−1/equal/+1, INT limits; negative count with repetition; `WITHVALUES`; absent key; wrong type; highly uneven per-page counts; reply-memory bound; membership, uniqueness, association, and non-flaky distribution sanity |
| `HSCAN` | cursor 0/completion/invalid/overflow; COUNT omitted, 1, small, huge, and invalid; MATCH none/some/all with glob metacharacters and binary-safe patterns where supported; `NOVALUES`; equal-hash runs; page split/doubling/delete/insert between calls; delete-all and same-key recreation mid-scan; bounded work and eventual completion |

For every command also exhaust its parser grammar: missing arguments, extra
arguments, odd HSET/HMSET pairs, invalid integer/float/cursor/count, repeated or
misordered options, case-insensitive option spelling, RESP bulk lengths, and
wrong-type keys. Parser cases belong in protocol/TCL tests; object-unit tests
cover semantic cases after parsing.

### 4.4 Key-level commands and TTL twin

The paged representation must also be tested through commands that operate on
the whole key:

- `EXISTS`, `TYPE`, `DEL`, supported multi-key delete/existence commands,
  `MEMORY USAGE`, `KEYS`, `SCAN`, and `SCAN TYPE`;
- every key-moving, copying, database-moving, or database-clearing command
  exposed by the command registry (`RENAME`, `RENAMENX`, `COPY`, `MOVE`,
  `FLUSHDB`, `FLUSHALL`, as applicable); an unsupported command must return its
  documented error and leave metadata/page rows untouched;
- every supported expiry form (`EXPIRE`, `PEXPIRE`, absolute variants and
  option flags), `TTL`, `PTTL`, and `PERSIST`;
- DUMP's explicit paged-object rejection and RESTORE's conversion/ineligibility
  policy;
- overwrite with string and RESTORE replacement, followed by recreation as a
  hash;
- transaction and script paths that invoke the same commands, where supported.

TTL cases cover conversion with an existing deadline, deadline extension and
shortening, removing TTL, expiry while fully/partially/non-resident, expiry
while a fetch or checkpoint is in flight, deleting the last field before
expiry, recreation before/after durable expiry, restart before/after the
deadline, and the v1 checkpoint-slack boundary. Use a controllable test clock
or configurable short slack rather than hour-long sleeps. Within slack `S`,
recovery must retain every WAL base the design promises. A deliberately stalled
checkpoint exceeding `S` tests and documents the accepted v1 limitation and,
more importantly, verifies that checkpoint-duration monitoring identifies the
violated deployment assumption instead of reporting the run as ordinary
durability coverage.

### 4.5 Production backfill and fetch-hub engine tests

The current `PageFetch-Test` covers mostly empty hub mechanics. Add an engine
unit fixture with a real `ObjectCcMap`, entry, committed payload, dirty payload,
fetch hub, and fake asynchronous store. It must exercise the actual
`BackFillPage`, `BackFill`, and `PageFetch::Execute` paths:

- one fetch/one waiter; multiple waiters coalesced on one page; one transaction
  waiting on multiple pages; partial issuance followed by store Retry;
- committed-only and dirty-only waiters, one transaction with contexts in
  both payloads, and multiple transactions split between them;
- reservation on committed, dirty, both, or neither; canonical allocation and
  cleanup assertions;
- success, transport error, missing row, empty Normal metadata row, truncated
  metadata, wrong-size page, structurally corrupt page, live-id mismatch,
  rejected install, stale id, and stale record timestamp;
- abort one waiter while others remain; payload swap/DEL/expiry; term change;
  orphan completion; entry/key-lock recycling;
- exact waiter wake count, request error, pin count, reservation count, hub
  emptiness, and installed bytes after completion.

This fixture is required even when an integration test exists: it makes the
critical completion outcomes deterministic and tests the production routing
rather than a hand-written approximation of it.

### 4.6 Flush and store-client units

Extend `paged_flush_roundtrip_test` and add DSS-client tests for:

- metadata plus 0/1/many dirty page puts and 0/1/many deletes;
- record-boundary batch cutting immediately before, exactly at, and after the
  configured batch limit; an oversized one-object batch remains indivisible;
- metadata written in every dirty cycle, including a one-page update;
- zero-copy export stability under concurrent COW writes;
- page timestamps below/equal/above callback commit timestamp;
- pending-delete prefix drain and a free/reallocate attempt before/after the
  durable delete;
- delete fan-out for fully resident, partially resident, and metadata-only
  objects;
- conversion batch, ordinary update batch, delete-all batch, and replacement
  batch;
- metadata TTL `deadline+S`, page TTL 0, TTL reset, and non-TTL conversion;
- whole-batch retry, timeout, duplicate completion, and callback replay;
- EloqStore atomicity: after every injected client/server failure, observe
  either the complete old batch or complete new batch, never a mixture.

## 5. Protocol and differential integration tests

Run the upstream hash TCL suite with conversion forced early, but do not treat
that as exhaustive paged coverage. Add a dedicated differential driver that:

1. sends identical binary-safe RESP commands to reference Redis/Valkey and
   EloqKV;
2. maintains the independent model;
3. forces and proves each target residency state between commands; and
4. compares replies and final state using the normalization rules in §2.

Organize traces into:

- one command per fresh key;
- long sequences on one key;
- delete-all and recreate loops;
- transactions that delete the last field and then read or recreate the same
  key before EXEC/COMMIT, plus abort after the last-field deletion;
- multiple keys sharing a shard and keys on different shards;
- pipelined commands with complete reply decoding;
- `MULTI/EXEC`, explicit transaction APIs, abort/disconnect, and WATCH if
  supported;
- Lua/script invocation of hash commands if scripting is part of the supported
  contract;
- RESP2 and every supported RESP3 reply shape. If RESP3 is not supported for a
  command, assert the documented behavior rather than skipping it silently.

Property traces should contain 100–10,000 operations selected from all hash and
key-lifecycle commands. After every operation, sample point reads; every 25–100
operations, compare the complete object; at the end, checkpoint, restart, and
compare again. Persist every failing seed and minimize its trace.

## 6. EloqStore deployment matrix

Every generally applicable integration scenario runs in four lanes:

| Lane | Topology | WAL | Durability expectation |
|---|---|---|---|
| `S0` | single node | off | exact state after a confirmed checkpoint/graceful stop; a SIGKILL may lose acknowledged post-checkpoint writes, but must recover a coherent checkpoint |
| `S1` | single node with external log service | on | every acknowledged write before SIGKILL survives WAL replay; the test must prove replay actually ran |
| `P0` | primary + backup | off | replication and graceful failover are coherent while nodes live; after loss of both nodes, require the last confirmed checkpoint, not uncheckpointed acknowledgements |
| `P1` | primary + backup with external log service | on | replica catch-up, graceful failover, restart, and WAL recovery preserve every acknowledged write covered by the log durability contract |

`standby_cluster.sh --wal on|off` is the starting point for `P0/P1`. Add a
matching single-node harness that starts the same standalone log service for
`S1`; setting `enable_wal=on` without proving the log service replays records is
not a valid WAL lane. Before any paged assertion, each WAL lane runs a small
monolithic control and requires a replay marker plus recovery of an
acknowledged post-checkpoint write.

The two-node deployment cannot automatically elect after abrupt leader loss in
the current one-voter setup. Therefore:

- test graceful `FAILOVER TO` in `P0/P1`;
- test leader crash, restart, and catch-up without promotion;
- if production promises automatic failover, add a separate quorum-capable
  three-voter lane rather than misclassifying a known no-quorum result as a
  paged failure.

The runner must generate fresh EloqStore cloud prefixes and local directories
per lane and seed, verify identical paging configuration on all replicas, and
archive the exact generated configuration. It must also refuse online scaling
or bucket migration in v1 rather than accidentally exercising an unsupported
configuration as though it were a valid lane.

## 7. Baseline integration scenarios in every lane

Run these without fault injection first:

1. **Command conformance:** the §4.3 matrix at small and multi-page sizes.
2. **Conversion:** each growth path crosses the threshold; TTL survives;
   RESTORE eligible/ineligible arms; representation agrees across replicas.
3. **Lifecycle:** create, update, shrink, delete all, checkpoint, restart,
   recreate, overwrite with another type, and recreate again.
4. **Residency:** fully resident, partial shed, metadata-only reload, mixed
   resident/missing multi-page command, repeated fault/evict cycles.
5. **Checkpoint:** incremental dirty pages, no-op/read-only interval, large
   dirty set, delete fan-out, concurrent write during flush.
6. **Restart:** graceful restart before/after checkpoint and repeated restart
   loops.
7. **TTL:** expire/reset/persist in each residency state and across restart.
8. **Isolation:** KEYS/SCAN/export tools never expose derived page rows.
9. **Large replies:** HKEYS/HVALS/HGETALL/HRANDFIELD at reply-bound edges.
10. **Long churn:** fixed logical cardinality with continuous update/delete/add
    to stress dead bytes, splits, pending deletes, and id reuse.

For `P0/P1`, compare leader and backup logical contents and representation at
each phase, then repeat the comparison after graceful failover and after both
nodes restart from durable state.

## 8. Concurrency matrix

Use deterministic barriers around page fetch issuance/completion and commit.
Each scenario runs with actors targeting (a) the same field/page, (b) different
fields on the same page, and (c) different pages. Repeat with one and multiple
missing pages.

### 8.1 Required actor combinations

| Actors | Required assertions |
|---|---|
| two point readers | same-page fetch coalesces; both receive correct bytes; one abort/disconnect does not strand the other |
| two multi-page readers | shared pages coalesce, distinct pages proceed independently, each reply is complete and ordered correctly |
| one reader + one writer | reader observes an allowed committed image, never a partially mutated dirty image; writer lands once; no lost wake/pin |
| **two readers + one writer, all faulting** | cover same page and overlapping page sets; committed and dirty payloads both participate; no unadmitted buffer, stale install, deadlock, or starvation |
| two writers | lock ordering and COW preserve both serializable outcomes; same-field HSET and HINCRBY have no lost update |
| one transaction + outside reader/writer | transaction faults first on committed then dirty payload; outsiders observe only committed versions; abort and commit clean all contexts |
| scanner + writer/deleter | HSCAN obeys weak-scan guarantees through split, doubling, delete, and insertion; cursor always progresses |
| writer + checkpoint + reader | exported buffer stays immutable; reader sees pre- or post-commit state; callback cannot clean a newer page |
| reader/writer + expiry/DEL/type overwrite | in-flight fetch is orphaned, waiter reruns against the successor, and stale bytes never install |

Run actor starts in every meaningful order and place barriers at: before fault
registration, after reservation, after store submission, after store response
but before backfill, after committed install but before dirty install, before
waiter wake, before CommitOn, and before lock release.

Record operation invocation/response intervals and check point-operation traces
for linearizability. HSCAN is checked against its documented non-snapshot
guarantees separately.

## 9. Page-fetch and data-store failure matrix

For each fetch shape—point, multi-page, coalesced, dirty-payload, replay drain,
and standby drain—inject:

- delayed success;
- immediate and delayed transport error;
- EloqStore `Retry` before any request, after partial issuance, and on a retry;
- missing live page row;
- status Normal with empty metadata row;
- truncated/unknown/malformed metadata;
- wrong-sized page row (0, page−1, page+1, huge bounded response);
- structurally invalid page of the right size;
- a valid page image belonging to another page id or object;
- stale/older row timestamp and duplicate completion;
- node-group term change before and after the store response;
- transaction abort, client disconnect, DEL, expiry, non-paged overwrite, and
  new paged incarnation while the fetch is in flight.

Selected mandatory combinations, beyond pairwise coverage:

1. page fault → partial success → store error on another page;
2. page fault → store Retry → memory refusal on re-entry → successful reclaim;
3. two readers + writer coalesced on one fetch → one reader aborts → store
   error;
4. dirty-payload fault with reservation only on dirty → committed install
   first → writer aborts after wake;
5. missing/corrupt page while a concurrent DEL or expiry makes the id no
   longer live—classify benign vs corruption according to the owning payload;
6. page fault during standby/replay drain → term change or error → no serving
   stale state and no infinite redrive;
7. corrupt fetch, disarm injection, clean refetch succeeds without restart.

Every case has a bounded timeout and proves that its injector/barrier fired.
After completion assert the exact client error/success, server health, key
usability, unchanged state on error, and zero residual pins, reservations,
waiters, and fetches.

## 10. Memory-pressure matrix

Test real small shard budgets as well as deterministic admission injection.
Cover:

- enough memory, exact-fit memory, one-byte/page over budget, and a fault set
  that can never fit an empty shard;
- no free memory followed by clean-page reclamation and successful retry;
- no reclaimable memory because all pages are dirty or pinned;
- multiple concurrent fault sets admitted/refused atomically;
- the two-reader/one-writer case with all three reserving/faulting;
- memory becoming full after fetch submission—the preallocated reservation
  must prevent completion overshoot;
- error, abort, swap, term change, and timeout after reservation;
- write-side commit park with one and several queued writers, abort/disconnect,
  other-key progress, same-key lock-free reads, and eventual resumption;
- replay/standby drain's intentionally unchecked allocation under the stated
  deployment axiom;
- whole-object reply estimates at below/equal/above limit, including many tiny
  fields and negative-count HRANDFIELD;
- repeated pressure/clean/fault cycles until memory and resident-page counts
  converge, with no starvation.

Sample allocator/shard usage before admission, at peak, after install, and after
quiescence. The pass condition includes the configured peak bound, not only a
successful reply.

## 11. Checkpoint, crash, replay, and failover matrix

Add deterministic crash points around one paged-object flush:

1. before export;
2. after export holds page buffers;
3. after DSS expansion but before EloqStore batch submission;
4. while the request is in flight;
5. after EloqStore commits but before the callback;
6. during/after the callback;
7. before and after WAL/checkpoint truncation advances.

Run each point for:

- initial conversion;
- one-page update and multi-page update;
- split/directory doubling;
- delete/free with pending page deletes;
- delete-all key removal;
- overwrite by a non-paged value and recreation;
- TTL set/reset/expiry;
- a concurrent write that COWs an exported page.

Expected outcome is always one coherent logical version. For no-WAL lanes it
may be the old checkpoint; for WAL lanes every acknowledged logged command must
be replayed. Never accept metadata from one version with page rows from another.

Replay and standby tests must prove the buffered drain actually page-faulted,
made progress, and finished before the node served traffic. Test one, many, and
thousands of buffered commands; repeated faults on one page; commands spanning
many pages; error and retry; and the progress-based promotion deadline.

Primary/backup tests add:

- backup offline during writes, then restart/catch-up while writes continue;
- backup restart from metadata-only state followed by CommitOn-only apply;
- graceful failover before checkpoint, during a fault, and after checkpoint;
- writes on the promoted node, promoted checkpoint, both-node restart, and
  full content comparison;
- old leader rejoining after promotion without overwriting newer paged state;
- repeated role swaps.

## 12. Store corruption and compatibility

Run corruption tests against metadata and page rows separately. For every
malformed input assert bounded CPU/memory, deterministic error, untouched cache
entry, server health, and successful clean refetch after repair.

Compatibility lanes cover:

- pre-feature monolithic rows read and updated by the new binary;
- paged rows read and updated with conversion disabled;
- toggling conversion off stops new conversion but never disables paged reads;
- metadata format version rejection/forward-compatibility policy;
- `RedisObjectType` tag stability;
- exporter behavior: page rows are filtered/reassembled according to each
  tool's supported contract;
- deployment-generated configs enforce identical conversion threshold and
  page size across replicas. Unsupported skew is rejected by deployment
  validation; it is not a normal runtime test lane.
- invalid or inconsistent startup settings: page size below/above bounds,
  conversion threshold above the object limit, shard memory smaller than one
  page/fault set, invalid reply bound, and WAL/topology combinations missing
  their required services. Each must either be rejected clearly at startup or
  follow an explicitly documented fallback.

## 13. Long-running, randomized, and resource tests

### 13.1 Stateful fuzzing

Run deterministic model-based traces with weighted operations:

- 40% point writes/deletes/increments;
- 30% point/multi-point reads;
- 10% scans/enumerations/random-field reads;
- 10% TTL/key lifecycle/overwrite/recreate;
- 10% checkpoint, shed, restart, failover, abort, and injected faults.

Bias generation toward boundary sizes and the current page's free-space edge.
Run one-key hot traces and many-key traces. At random safe points compare the
complete model, checkpoint/restart, and continue the same trace.

### 13.2 Soak workloads

Nightly/weekly soaks include:

- one hot hash continuously updated for hours;
- many paged hashes competing for a small memory budget;
- fixed-cardinality churn that creates dead bytes and pending deletes;
- repeated full deletion and recreation of the same keys;
- continuous backup restart/catch-up with foreground writes;
- periodic graceful failover in `P0/P1`;
- checkpoint interval much shorter than write cadence and much longer than it;
- object dirty delta near and above the data-sync scan-heap threshold;
- repeated store Retry/error bursts and memory pressure.

Assert bounded memory trend after warm-up, checkpoint progress, no permanently
dirty/pinned/reserved pages, no growing hub/orphan population, bounded operation
latency after faults stop, and exact final logical/durable state.

### 13.3 Sanitizers and diagnostics

Run ordinary unit and protocol conformance tests in both Debug and Release (or
RelWithDebInfo): correctness must not depend on assertions, and invariant
self-check failures must have their documented both-build behavior. Fault
injection cases may remain Debug-only, but their non-injected success/error
paths still run in Release. Run unit and deterministic integration subsets
under ASAN and UBSAN. Run TSAN only if the bthread/toolchain combination is
supported; otherwise use the engine's race instrumentation and document the
limitation. On failure archive:

- server and host-manager logs;
- generated configs and start commands;
- fault/barrier history;
- seed and minimized operation trace;
- process exit status/core/backtrace;
- allocator and paged-object inspector snapshots;
- raw EloqStore rows for affected keys.

## 14. Harness requirements and anti-vacuity rules

Consolidate the repeated Python RESP/process helpers into
`tests/unit_cc/paged_testlib.py` with a complete binary-safe RESP2 parser and
supported RESP3 parsing. A single `recv()` or reply-header-only comparison is
not sufficient for array/bulk assertions.

Replace timing-only races with named barriers where practical. Each injector
or barrier exposes a monotonic hit counter. A test fails as **vacuous** if it
cannot prove:

- the object was paged;
- the intended pages were clean and actually shed;
- a page fetch rather than a whole-record fetch occurred;
- the intended waiter/transaction shape existed;
- the error, Retry, memory refusal, crash point, or term change fired;
- replay, standby drain, checkpoint, or failover path actually ran.

Use polling on observable state instead of fixed sleeps for checkpoint
completion, replication convergence, role transition, and memory cleanup.
Every asynchronous test has separate deadlines for reaching the barrier and
for completing after release.

Each scenario uses unique keys and a fresh store namespace or performs a
verified cleanup. Tests must disarm all injectors in `finally` blocks and prove
the server is responsive before handing the deployment to the next case.

## 15. Existing tests: retain and expand

| Existing test | Role in the broader plan |
|---|---|
| `paged_hash_core_test` | retain structural/codec tests; add state-machine, boundary, collision-run, delete/recreate, and fuzz corpus cases |
| `paged_eviction_test` | retain LRU/COW tests; add multi-owner, allocation-count, exact-budget, and cleanup cases |
| `paged_flush_roundtrip_test` | retain fake-store round trips; add batch boundaries, failure idempotency, delete-all/recreate, and atomicity fixtures |
| `paged_object_command_test` | retain Execute/Commit regression arms; expand into the complete §4.3 command/state table |
| upstream `hash.tcl` and `scan.tcl` | keep as Redis compatibility baselines; run in all applicable deployment lanes and add missing paged-specific lifecycle cases |
| `paged_fault_matrix.py` | retain its ten regressions; replace the hand-selected matrix with §9 pairwise generation plus required triples |
| `swap_with_inflight_fetch.py`, `abort_with_parked_command.py` | retain; fold into barrier-driven concurrency/fetch suite |
| `paged_hscan.py`, `paged_hlen_consistency.py` | retain; add differential, lifecycle, restart, TTL, and concurrency variants |
| `paged_corrupt_page.py`, `paged_corrupt_record.py` | retain; add wrong-page, wrong-size, stale timestamp, memory-bound, and every deployment lane |
| `paged_commit_memory_park.py` | retain; add real-pressure, several waiters, abort, term-change, and fairness cases |
| `paged_conversion_policy.py`, `standby_conversion_paths.py` | retain; add exact threshold/page-size boundaries and repeated role/restart transitions |
| `replay_paged_restart.py`, `replay_serving_window.py` | make the external-log single-node WAL harness reliable; promote from conditional diagnostics to mandatory `S1` gates |
| `standby_paged_apply.py`, `standby_failover_paged.py` | run with WAL off/on; add repeated restart, concurrent workload, fault, and role-swap cases |
| `paged_dark_feature.py`, `paged_key_security.py`, `paged_scan_leak.py`, `paged_oversized_record.py` | retain as focused compatibility/security regressions and run in the applicable four-lane matrix |

## 16. Suite organization

The implemented entry points are:

```text
tests/unit_cc/
  paged_hash_core_test.cpp
  paged_object_command_test.cpp
  paged_flush_roundtrip_test.cpp
  paged_eviction_test.cpp
  paged_codec_fuzz_test.cpp
  paged_backfill_engine_test.cpp
  paged_testlib.py
  paged_protocol_differential.py
  paged_lifecycle_matrix.py
  paged_concurrency_matrix.py
  paged_store_failure_matrix.py
  paged_persistence_matrix.py
  paged_production_config.py
  paged_ttl_matrix.py
  paged_soak.py
  paged_test_matrix.sh

data_substrate/tx_service/tests/
  PageFetch-Test.cpp
  PagedStoreBatch-Test.cpp
  LockGrantability-Test.cpp
```

The command/model coverage is deliberately expanded in the existing core and
object-command binaries rather than split into two more executables. The
production DSS batch fixture belongs to `data_substrate`, beside the batching
code it calls. `paged_codec_fuzz_test` is both a deterministic corpus/mutation
runner and the shared `LLVMFuzzerTestOneInput` entry point; configure
`-DPAGED_BUILD_FUZZERS=ON` with Clang to build the continuous libFuzzer target.

The differential runner always uses an independent Python logical model. Its
second protocol endpoint may be upstream Redis/Valkey; the checked-in matrix
starts a conversion-disabled EloqKV endpoint when no external reference binary
is installed, which additionally compares paged and monolithic
representations. A release qualification environment should supply the claimed
upstream Redis/Valkey version as the reference endpoint at least once.

`paged_test_matrix.sh` owns deployment lifecycle and accepts at least:

```text
--lane S0|S1|P0|P1|all
--tier smoke|pr|nightly|release
--seed N
--repeat N
--keep-artifacts-on-failure
```

It prints a machine-readable result per scenario: pass, fail, or inconclusive.
Release gates permit no inconclusive result; an unobserved race or replay window
must be fixed in the harness, not counted as passing.

`smoke` runs the deterministic unit/corpus suite and the baseline of every
selected lane. `pr` adds corruption, failure, concurrency, lifecycle, TTL,
memory, replay, and failover cases. `nightly` adds upstream TCL, differential,
persistence/crash coherence, in-flight swap/abort, soak, dark-restart, and the
specialized TTL-conversion replay deployment. `release` increases corpus,
differential, and soak budgets, repeats the lane matrix at least twice, and
turns every inconclusive result into a failure. The authoritative record is
`bld-paged-matrix/results.tsv`; documentation must never substitute for a
fresh qualification record.

## 17. Execution tiers

### Per change / pull request

- four current C++ binaries plus the new command, model, backfill, and store
  units;
- bounded fuzz corpus replay;
- single-node `S0` protocol/lifecycle smoke;
- targeted integration tests selected from files touched;
- feature line coverage must remain 100%; track meaningful branch coverage for
  command and completion outcomes.

### Nightly

- all four deployment lanes;
- full command differential matrix;
- pairwise concurrency/failure/memory matrix;
- checkpoint crash points with a rotating subset of object operations;
- multiple deterministic state-machine seeds;
- ASAN/UBSAN unit suites and selected integration cases.

### Weekly and release qualification

- all crash points × all mutation classes × `S0/S1/P0/P1`;
- long soaks, repeated failover/restart, and high-churn workloads;
- larger seed set and fuzzing time budget;
- production page size/threshold/memory settings in addition to accelerated
  test settings;
- full raw-store atomicity and compatibility inspection;
- zero inconclusive tests and no unexplained warnings, assertions, FATALs,
  leaked state, or durability gaps.

## 18. Exit criteria

The feature is ready for v1 only when:

1. every supported hash command and parser grammar arm has a named test and
   matches the Redis/Valkey oracle or a documented EloqKV divergence;
2. every command passes in full, partial, and metadata-only residency, and
   mutators pass through Execute+Commit and CommitOn-only paths;
3. the complete delete-all/checkpoint/restart/recreate lifecycle passes in all
   four deployment lanes;
4. every named fetch, corruption, memory, transaction, checkpoint, replay, and
   failover outcome terminates with exact expected state and no leaked runtime
   bookkeeping;
5. `S1` and `P1` prove recovery of acknowledged post-checkpoint writes through
   actual WAL replay, while no-WAL lanes recover a coherent checkpoint;
6. EloqStore inspection never observes a torn metadata/page batch and derived
   rows never leak through Redis key enumeration;
7. required concurrency traces are linearizable, HSCAN satisfies its weaker
   documented guarantees, and no stress/soak run finds a hang or permanent
   progress loss;
8. all race/fault tests prove their preconditions and release qualification has
   no inconclusive result; and
9. all failures are reproducible from archived seeds/traces and leave enough
   evidence to distinguish an EloqKV defect, engine defect, EloqStore defect,
   and harness defect.

## 19. Current implementation and qualification record (2026-08-03)

This section records what has actually been implemented and run. It does not
relax the exit criteria above. In particular, a passing bounded substitute is
not recorded as completion of an hours-long soak or an exhaustive crash-point
Cartesian product.

### 19.1 Harness and coverage completed

The entry points in §16 are implemented. The release runner now also:

- runs a dedicated S0 production-configuration scenario with 128 KiB pages, a
  4 MiB representative low-single-digit-MiB conversion threshold, and a
  384 MiB node budget;
- records the threshold, page size, and memory budget in `current.env`; and
- archives configs, start commands, stdout, and node/log-service logs
  immediately when a scenario fails or is inconclusive, before a later lane or
  repetition can overwrite them.

`paged_production_config.py` proves monolithic state below the threshold,
conversion above it, TTL preservation, exact multi-MiB whole-object replies and
`MEMORY USAGE`, checkpoint/shed/refault, update/delete/restart, delete-all
durability, and same-key recreation without stale page or TTL inheritance.

Feature line coverage was measured with `tests/unit_cc/paged_coverage.sh bld`:
1,899 of 1,899 feature lines were covered (100%). This is line coverage, not a
claim that every branch/interleaving named in this plan is implemented.

### 19.2 Release-matrix result

The following command was run against EloqStore:

```bash
tests/unit_cc/paged_test_matrix.sh --lane all --tier release --seed 57612
```

The authoritative record is `bld-paged-matrix/results.tsv`. It contains 188
scenario results: 187 pass and one fail, with no inconclusive result.

| Scope | Pass | Fail | Material coverage |
|---|---:|---:|---|
| unit | 9 | 0 | core/command/flush/eviction, 500k codec mutations, production backfill, fetch hub, lock grantability, DSS batch expansion |
| S0 | 44 | 0 | two complete release cycles, including 10k differentials and 50k soaks |
| S1 | 53 | 0 | S0 coverage plus WAL replay, serving-window proof, corrupt-record handling, commit-memory park, and TTL conversion/PERSIST replay |
| P0 | 40 | 0 | two primary/backup cycles without WAL, standby apply/conversion/TTL and graceful failover |
| P1 | 41 | 1 | two primary/backup cycles with WAL; the first graceful-failover recovery read timed out |

Every 50,000-operation soak completed. Across the two repetitions, page-shed
counts were 253--255 per lane. S0/P0 p99 latency was at most 12 ms; S1/P1 p99
was 490--575 ms. The largest observed operation latency was 1.023 s.

The sole release failure was `P1 graceful-failover` in the first repetition.
After full restart, `HLEN` returned the expected 700, but the following cold
`HKEYS` did not finish within its 60-second socket deadline. The cluster served
the subsequent P1 TTL and 50k-soak scenarios, and the identical failover path
passed in the second release repetition. The test now reports separate HLEN and
HKEYS timing rather than an unclassified traceback. Three additional fresh
P1/EloqStore repetitions passed, covering both possible leader roles; recovered
`HKEYS` times were 19 ms, 9 ms, and 12 ms, and all 700 fields survived. Per-run
logs are retained under `bld-paged-matrix/failover-reruns/`.

Thus no corruption or permanent progress loss was reproduced, but the original
outlier is unexplained. Because §16 makes every release failure blocking, the
release matrix is **not green**.

### 19.3 Additional seeds and production-sized configuration

- Codec corpus/mutation runs passed for seeds 57613 and 3735928559 at 100,000
  mutations each, in addition to the release seed's 500,000 mutations.
- An additional 10,000-operation binary-safe S0 model/differential trace passed
  with seed 57613 after proving conversion and a metadata-only transition.
- The production-configuration S0 scenario passed with 4,800 fields and
  4,972,800 field/value bytes: it crossed from a 3,108,000-byte monolithic
  image to the paged representation, checkpointed and refaulted complete
  replies, recovered a 4,519-field dirty post-image with its TTL, durably
  deleted every field, then recreated the same key without stale state.

No Redis/Valkey server binary is installed in this qualification environment.
The differential endpoint was therefore conversion-disabled EloqKV plus the
independent Python model, while the upstream Redis hash TCL suite ran and
passed in every deployment lane. A release environment containing the claimed
Redis/Valkey version is still required for the external-server differential.

### 19.4 Sanitizers

All nine deterministic unit binaries passed under ASAN and UBSAN, including a
100,000-mutation codec run in each configuration. ASAN initially found a
use-after-free in the new `PagedStoreBatch-Test` fixture: zero-copy
`string_view`s outlived the fixture's `FlushRecord` owners. The fixture now
retains the owners for the complete prepared-batch lifetime, matching the
production ownership contract, and the ASAN rerun passed.

The ASAN unit lane used `detect_leaks=0` because LeakSanitizer cannot operate
under the execution wrapper's ptrace environment; address checking remained
enabled with `halt_on_error=1`. This means leak checking was not performed.

A fully UBSAN-instrumented EloqStore S0 binary was built with GCC 15. The hash
fault/model, concurrency, coalesced store-error/disconnect, complete
delete/checkpoint/restart/recreate lifecycle, TTL/expiry/PERSIST/restart, and
memory-admission refusal/reclaim subsets all passed with
`halt_on_error=1`. No UBSAN diagnostic was emitted.

Supported ASAN EloqStore integration could not be configured: EloqStore
requires `libboost_context-asan`, which is not installed. A diagnostic build
that instrumented only the outer binary is not a valid substitute—txservice's
CMake resets its flags, so that build mixed ASAN allocation interception with
non-ASAN mimalloc accounting and crashed in `DataSyncForHashPartition`. That
result is classified as an unsupported mixed-instrumentation artifact, not a
product defect or a passing ASAN integration lane.

TSAN was not run because there is no configured/supported bthread TSAN lane in
this workspace.

### 19.5 Coverage still required before this plan's exit criteria are met

The following are not completed by the current suite and remain explicit
qualification work:

1. run selected EloqStore integrations under the supported ASAN build after
   providing `libboost_context-asan`, including leak checking in an environment
   where LeakSanitizer is supported;
2. execute the new deterministic flush hooks against every named mutation in
   P0/P1 as well as S0/S1; the current run covers a multi-page update at all
   seven applicable S0 boundaries and all nine concrete S1 hooks, but not the
   §11 mutation/topology Cartesian product;
3. complete the hours-long/many-object pressure soaks, continuous backup
   restart/catch-up, repeated role swaps, and periodic failover workloads from
   §13.2; the completed 50k-operation soaks are bounded release regressions,
   not those duration/resource tests; and
4. add the remaining deterministic engine combinations named in §§4.5, 8--10,
   especially partial store issuance + Retry, term changes at every completion
   boundary, and exact allocator/reservation snapshots. Transport error, term
   change, and committed/dirty dual-payload completion with the reservation on
   either payload are now covered, but the full pairwise set is not.

Until these items and the recorded failures are resolved, §18's feature
release-ready exit criteria are not satisfied.

### 19.6 Follow-up qualification and new durable-store finding

The exact P1 release lane was rerun twice with seed 57612. All 52 recorded
scenarios passed (10 unit and 42 P1), including both `graceful-failover`
executions at the former failure point, both 50,000-operation soaks, and both
memory-pressure scenarios. Post-full-restart `HKEYS` took 7 ms in each
failover execution. Both 50,000-operation soaks completed; the second took
2,160.7 s and reported p99 616 ms, maximum 1.681 s, and 254 non-vacuous page
sheds.
Together with the previously recorded executions, the unexplained timeout now
stands at one occurrence in thirteen known runs and was not reproduced in the
two exact release repetitions. The shell process returned 2 only after every
scenario had been recorded pass because this runner file was edited while that
long-running bash process was still reading it; the current file passes
`bash -n`. That post-test harness artifact is not a scenario failure.

The §2.4 durable-store mechanism now exists:

- `eloqstore_paged_rows.py` discovers the active physical table, computes the
  Redis hash partition, decodes derived page-key envelopes, and reports stored
  timestamps, byte lengths, digests, and TTL attributes. It uses exact-key
  `ScanNext`, because the current EloqStore `Read` adapter omits `expire_ts_`
  from its otherwise-present response TTL field.
- `eloqstore_paged_corrupt.py` performs guarded, backed-up physical mutations
  and atomic metadata+page repair. `paged_durable_corruption.py` passed in S0
  and S1 for a missing page, truncated page, page-size mismatch, and a valid
  page image belonging to another page id. Every cold read failed
  deterministically in 5--13 ms, the server remained live, and repair plus
  restart restored the exact logical image and timestamp dominance.
- The external binary-safe differential passed 10,000 deterministic
  operations against upstream Redis 7.0.15 after proving paged conversion and
  a metadata-only target state.
- Deterministic PANIC hooks now bracket export, DSS expansion, store in-flight,
  store commit, callback before/after, and WAL/checkpoint watermark before/after.
  Every hook must leave a durable hit marker. S0 passed its seven applicable
  hooks (old before submission/in-flight, new after store commit); S1 passed
  all nine and recovered the acknowledged new image at every point. Normal
  checkpoints complete asynchronously in `DataSyncTask::SetFinish`, so the
  watermark hooks cover that production path as well as the synchronous
  no-task path.

The durable lifecycle oracle found one reproducible implementation failure
(since FIXED). After a paged hash was checkpointed, both deleting its final
fields with one `HDEL` and deleting it with `DEL` removed the metadata row
but left every derived page row live in EloqStore (`HDEL-to-empty left
metadata=False pages=19; DEL left metadata=False pages=15`). The cause was
that both deletion routes dropped the paged payload — and with it the page-id
inventory §9's fan-out needs — before the checkpoint ran. The fix is the
central paged-deletion rule (`TxCommand::CommitOn(obj, PagedCommitContext)`,
design doc §16): a deleted paged block is retired (fetches orphaned, parked
readers woken, its full volatile state — frames, admission reservations,
pending faults, contexts, LRU — torn down) and RETAINED, tagged, until the
deletion flush emits the metadata-row delete plus every page-row delete; the
post-flush callback then releases it. `durable-store-rows` now PASSES on
both deletion routes and continues through same-key recreation with no
stale-row inheritance. The remaining, deliberate exception is a deletion
replayed from the WAL onto a NON-RESIDENT prior (crash replay of a DEL is an
overwrite command, applied without fetching the old metadata): its page rows
are accepted sweeper debt, the same §9 category as replacement, asserted as
such by `paged_replay_delete_rows.py` in the S1 lane.
