/**
 *    Copyright (C) 2025 EloqData Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under either of the following two licenses:
 *    1. GNU Affero General Public License, version 3, as published by the Free
 *    Software Foundation.
 *    2. GNU General Public License as published by the Free Software
 *    Foundation; version 2 of the License.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License or GNU General Public License for more
 *    details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    and GNU General Public License V2 along with this program.  If not, see
 *    <http://www.gnu.org/licenses/>.
 *
 */

// Phase 2 flush/fetch round-trip tests (docs/08-paged-objects-plan.md Phase
// 2): checkpoint -> evict -> fault back -> identical bytes, driven against a
// fake store that stands in for the DSS row space. These exercise the
// *protocol-layer* half of the paged plumbing — the dirty-set projection, the
// guarded post-flush callback, the page-id lifecycle, and the TTL-attribute
// placement — without needing a CcShard.
//
// Standalone build:
//   g++ -std=c++20 -fsanitize=address,undefined -I include -I data_substrate
//       tests/unit_cc/paged_flush_roundtrip_test.cpp -o paged_flush_roundtrip

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "redis_paged_hash_object.h"
#include "tx_service/include/page_key_codec.h"

namespace
{
using namespace EloqKV;
using txservice::PageRowKind;

// The protocol-layer include chain brings in glog, whose CHECK this test
// shadows deliberately (abort-with-location, no logging dependency).
#undef CHECK
#define CHECK(cond)                                                            \
    do                                                                         \
    {                                                                          \
        if (!(cond))                                                           \
        {                                                                      \
            std::fprintf(stderr,                                               \
                         "CHECK failed at %s:%d: %s\n",                        \
                         __FILE__,                                             \
                         __LINE__,                                             \
                         #cond);                                               \
            std::abort();                                                      \
        }                                                                      \
    } while (0)

uint64_t SplitMix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

uint64_t TestFieldHash(std::string_view field)
{
    uint64_t h = 0xE10A0B5EULL;
    for (char c : field)
    {
        h = SplitMix64(h ^ static_cast<uint8_t>(c));
    }
    return h;
}

/**
 * @brief A stand-in for the store's row space: encoded key -> (value, ts,
 * ttl). Deliberately a plain sorted map so the tests can assert on the exact
 * row set, key encoding, and per-row TTL attributes the DSS would receive.
 */
struct FakeStore
{
    struct Row
    {
        std::string value_;
        uint64_t commit_ts_{0};
        uint64_t ttl_{0};
    };
    std::map<std::string, Row> rows_;

    void Put(const std::string &key,
             std::string value,
             uint64_t ts,
             uint64_t ttl)
    {
        Row &row = rows_[key];
        row.value_ = std::move(value);
        row.commit_ts_ = ts;
        row.ttl_ = ttl;
    }

    void Delete(const std::string &key)
    {
        rows_.erase(key);
    }

    const Row *Get(const std::string &key) const
    {
        auto it = rows_.find(key);
        return it == rows_.end() ? nullptr : &it->second;
    }

    size_t PageRowCount(std::string_view object_key) const
    {
        size_t n = 0;
        for (const auto &[key, row] : rows_)
        {
            txservice::PageKeyParts parts;
            if (txservice::DecodePageKey(key, parts) &&
                parts.object_key_ == object_key)
            {
                ++n;
            }
        }
        return n;
    }
};

/**
 * @brief One checkpoint cycle for one paged object, mirroring the engine
 * flush path (docs/08 §9): project the dirty set, write metadata + page rows
 * under derived keys, then run the guarded post-flush callback with the
 * commit ts that was actually written.
 *
 * @param mutate_mid_flush Runs between the export and the callback — the
 * window the §4 ts guard exists to close: the export runs on the shard core,
 * the flush on a worker, and the callback is enqueued back, so the core can
 * accept a write to an already-exported page in between.
 */
struct FlushStats
{
    size_t page_puts_{0};
    size_t page_deletes_{0};
    uint64_t metadata_row_ttl_{0};
};

FlushStats FlushCycle(RedisPagedHashObject &core,
                      const std::string &object_key,
                      FakeStore &store,
                      uint64_t commit_ts,
                      uint64_t metadata_row_ttl = 0,
                      const std::function<void()> &mutate_mid_flush = {})
{
    FlushStats stats;
    stats.metadata_row_ttl_ = metadata_row_ttl;

    // --- Export: metadata row + every dirty page + every pending delete ---
    std::string metadata;
    core.SerializeMeta(metadata);
    std::vector<std::pair<PageId, PageBuf>> dirty;
    core.Frames().ForEachDirtyPage([&](PageId id, const PageBuf &buf)
                                   { dirty.emplace_back(id, buf); });
    std::vector<PageId> deletes;
    core.Frames().ForEachPendingDeleteId([&](PageId id)
                                         { deletes.push_back(id); });

    if (mutate_mid_flush)
    {
        mutate_mid_flush();
    }

    // --- Batch shape: what EloqStore requires of one write batch ---
    // BatchWriteTask::SetBatch asserts the batch's keys are UNIQUE and
    // ORDERED. The DSS adapter sorts before dispatch
    // (eloq_store_data_store.cpp), so emission order is free — but sorting
    // cannot fix DUPLICATES, and a duplicate key would assert inside the
    // store. Assert here that one object's emitted row set is
    // duplicate-free: metadata row plus every dirty page plus every pending
    // delete, all distinct. (Dirty and pending-delete sets are disjoint by
    // construction — FreePage erases the frame — and this guards that.)
    {
        std::set<std::string> batch_keys;
        CHECK(batch_keys.insert(object_key).second);
        for (const auto &[id, buf] : dirty)
        {
            std::string k;
            txservice::EncodePageKey(k, object_key, PageRowKind::HashPage, id);
            CHECK(batch_keys.insert(k).second);
        }
        for (PageId id : deletes)
        {
            std::string k;
            txservice::EncodePageKey(k, object_key, PageRowKind::HashPage, id);
            CHECK(batch_keys.insert(k).second);
        }
        // And the sorted batch is strictly increasing, which is exactly the
        // store's precondition once the adapter has sorted it.
        std::string prev;
        bool first = true;
        for (const std::string &k : batch_keys)
        {
            CHECK(first || prev < k);
            prev = k;
            first = false;
        }
    }

    // --- Apply: one atomic batch, in the handler's order ---
    store.Put(object_key, metadata, commit_ts, metadata_row_ttl);
    for (const auto &[id, buf] : dirty)
    {
        std::string key;
        txservice::EncodePageKey(key, object_key, PageRowKind::HashPage, id);
        store.Put(key,
                  std::string(reinterpret_cast<const char *>(buf.get()),
                              core.Frames().PageSize()),
                  commit_ts,
                  /*ttl=*/0);  // page rows never carry a TTL attribute (§9)
        ++stats.page_puts_;
    }
    for (PageId id : deletes)
    {
        std::string key;
        txservice::EncodePageKey(key, object_key, PageRowKind::HashPage, id);
        store.Delete(key);
        ++stats.page_deletes_;
    }

    // --- Post-flush callback, with the flushed commit ts ---
    core.OnPagedFlushApplied(commit_ts);
    return stats;
}

/**
 * @brief Faults page `id` back from the fake store, as BackFillPage would.
 */
bool FaultPage(RedisPagedHashObject &core,
               const std::string &object_key,
               const FakeStore &store,
               PageId id)
{
    std::string key;
    txservice::EncodePageKey(key, object_key, PageRowKind::HashPage, id);
    const FakeStore::Row *row = store.Get(key);
    if (row == nullptr)
    {
        return false;
    }
    return core.InstallPage(id, row->value_, row->commit_ts_);
}

// A paged object that survives a checkpoint, full eviction, and a reload must
// read back byte-identically — the property every later recovery claim rests
// on (§16).
void TestFlushEvictFaultRoundTrip()
{
    const std::string kKey = "user:42";
    FakeStore store;
    RedisPagedHashObject core(256, &TestFieldHash);
    core.SetWriteTs(10);

    std::map<std::string, std::string> expected;
    for (int i = 0; i < 400; ++i)
    {
        std::string f = "field" + std::to_string(i);
        std::string v = "value" + std::to_string(i * 7);
        core.Put(f, v);
        expected[f] = v;
    }
    CHECK(core.CheckInvariants());

    std::string meta_before;
    std::vector<std::pair<PageId, std::string>> pages_before;
    core.SerializeAll(meta_before, pages_before);

    FlushStats stats = FlushCycle(core, kKey, store, /*commit_ts=*/10);
    CHECK(stats.page_puts_ == pages_before.size());
    CHECK(stats.page_deletes_ == 0);
    // Every page is clean now, so a second cycle exports no page at all —
    // dirtiness is the flushed_ bit, and the guard passed for all of them.
    FlushStats again = FlushCycle(core, kKey, store, /*commit_ts=*/11);
    CHECK(again.page_puts_ == 0);

    // Evict every page: clean and unpinned, so all sheddable (§8).
    std::vector<PageId> resident;
    for (const auto &[id, unused] : pages_before)
    {
        resident.push_back(id);
    }
    for (PageId id : resident)
    {
        CHECK(core.MutableFrames().ShedPage(id));
    }
    CHECK(core.ResidentPageCount() == 0);

    // Reload from the metadata row alone: no page I/O at load time (§5).
    const FakeStore::Row *meta_row = store.Get(kKey);
    CHECK(meta_row != nullptr);
    RedisPagedHashObject reloaded(&TestFieldHash);
    size_t offset = 0;
    CHECK(reloaded.DeserializeSections(
        meta_row->value_.data(), meta_row->value_.size(), offset));
    CHECK(reloaded.ResidentPageCount() == 0);

    // Fault pages on demand and read every field back.
    for (const auto &[field, value] : expected)
    {
        uint64_t h = 0;
        PageId pid = reloaded.RouteField(field, &h);
        if (!reloaded.Frames().IsResident(pid))
        {
            CHECK(FaultPage(reloaded, kKey, store, pid));
        }
        std::optional<std::string_view> got = reloaded.Get(field);
        CHECK(got.has_value());
        CHECK(*got == value);
    }
    CHECK(reloaded.FieldCount() == expected.size());
    CHECK(reloaded.CheckInvariants());

    // Byte-identical layout after the round trip.
    std::string meta_after;
    std::vector<std::pair<PageId, std::string>> pages_after;
    reloaded.SerializeAll(meta_after, pages_after);
    CHECK(meta_after == meta_before);
    CHECK(pages_after == pages_before);

    // A faulted page is clean: it came from the store, so a flush now
    // exports nothing.
    FlushStats after_fault = FlushCycle(reloaded, kKey, store, 12);
    CHECK(after_fault.page_puts_ == 0);
    std::printf("flush/evict/fault round trip: ok (%zu pages)\n",
                pages_before.size());
}

// The §4 guard: a page rewritten between export and callback carries a newer
// last_modified_ts_, fails `last_modified_ts_ <= flushed_commit_ts`, and stays
// dirty — so its newer content cannot be silently marked durable.
void TestMidFlushWriteStaysDirty()
{
    const std::string kKey = "h";
    FakeStore store;
    RedisPagedHashObject core(256, &TestFieldHash);
    core.SetWriteTs(10);
    for (int i = 0; i < 50; ++i)
    {
        core.Put("f" + std::to_string(i), "v");
    }

    // Flush at ts 50, but at ts 75 (mid-flush) rewrite one field.
    std::string victim = "f7";
    uint64_t h = 0;
    PageId victim_page = core.RouteField(victim, &h);
    FlushCycle(core,
               kKey,
               store,
               /*commit_ts=*/50,
               /*metadata_row_ttl=*/0,
               [&]
               {
                   // The write only marks the page dirty; the commit ts
                   // arrives at PostWriteCc, which StampWrites models.
                   core.Put(victim, "REWRITTEN");
                   core.StampWrites(75);
               });

    const PageSlot *slot = core.Frames().SlotOf(victim_page);
    CHECK(slot != nullptr);
    CHECK(slot->last_modified_ts_ == 75);
    // Guard failed (75 > 50): still dirty, hence not evictable and re-exported
    // next round.
    CHECK(!slot->flushed_);
    CHECK(!core.MutableFrames().ShedPage(victim_page));

    // The next cycle picks it up and the new content lands.
    FlushStats next = FlushCycle(core, kKey, store, /*commit_ts=*/75);
    CHECK(next.page_puts_ >= 1);
    CHECK(core.Frames().SlotOf(victim_page)->flushed_);
    std::printf("mid-flush write stays dirty: ok\n");
}

// A freed page's row must be deleted before its id can be reused, and the
// drain is scoped to what was actually written (§4/§9): a range freed after
// the export keeps its row until the next cycle.
void TestPendingDeleteDrainScoping()
{
    const std::string kKey = "h";
    FakeStore store;
    RedisPagedHashObject core(256, &TestFieldHash);
    core.SetWriteTs(10);
    for (int i = 0; i < 200; ++i)
    {
        core.Put("f" + std::to_string(i), "v");
    }
    FlushCycle(core, kKey, store, 10);
    size_t rows_before = store.PageRowCount(kKey);
    CHECK(rows_before > 1);

    // Free a page at ts 20 and flush at 20: its row goes away and the id
    // becomes reusable.
    PageId freed = core.Meta().dir_.back();
    core.SetWriteTs(20);
    core.MutableFrames().FreePage(freed);
    CHECK(core.Frames().PendingDeleteEntries().size() == 1);
    FlushStats s = FlushCycle(core, kKey, store, 20);
    CHECK(s.page_deletes_ == 1);
    CHECK(store.PageRowCount(kKey) == rows_before - 1);
    CHECK(core.Frames().PendingDeleteEntries().empty());

    // Now free at ts 75 but flush at 50 — the range was not in that batch,
    // so draining it would recycle an id whose Delete never went out.
    PageId freed_late = core.Meta().dir_.front();
    core.SetWriteTs(75);
    core.MutableFrames().FreePage(freed_late);
    core.OnPagedFlushApplied(50);
    CHECK(core.Frames().PendingDeleteEntries().size() == 1);
    // The later callback drains it.
    core.OnPagedFlushApplied(75);
    CHECK(core.Frames().PendingDeleteEntries().empty());
    std::printf("pending-delete drain scoping: ok\n");
}

// The whole-object delete fan-out names every live page id and needs no page
// resident (§9) — the reason a deleted paged object keeps its metadata block
// until the deletion flushes.
void TestDeletionFanOut()
{
    const std::string kKey = "h";
    FakeStore store;
    RedisPagedHashObject core(256, &TestFieldHash);
    core.SetWriteTs(10);
    for (int i = 0; i < 300; ++i)
    {
        core.Put("f" + std::to_string(i), "value-value-value");
    }
    FlushCycle(core, kKey, store, 10);
    size_t page_rows = store.PageRowCount(kKey);
    CHECK(page_rows > 1);

    // Shed everything: the fan-out must still enumerate every id.
    std::vector<PageId> ids;
    core.Frames().ForEachLivePageId([&](PageId id) { ids.push_back(id); });
    for (PageId id : ids)
    {
        core.MutableFrames().ShedPage(id);
    }
    CHECK(core.ResidentPageCount() == 0);

    std::vector<PageId> fan_out;
    core.Frames().ForEachLivePageId([&](PageId id) { fan_out.push_back(id); });
    CHECK(fan_out.size() == page_rows);

    // Applying the fan-out empties the object's key space in the store.
    store.Delete(kKey);
    for (PageId id : fan_out)
    {
        std::string key;
        txservice::EncodePageKey(key, kKey, PageRowKind::HashPage, id);
        store.Delete(key);
    }
    CHECK(store.PageRowCount(kKey) == 0);
    CHECK(store.Get(kKey) == nullptr);
    std::printf("deletion fan-out: ok (%zu page rows)\n", page_rows);
}

// Row-level assertions on what the store actually receives (§5/§9): page keys
// carry the reserved prefix and the object key, sort numerically by page id,
// and only the metadata row gets a TTL attribute.
void TestRowShapeAndTtlPlacement()
{
    const std::string kKey = "obj";
    FakeStore store;
    RedisPagedHashObject core(256, &TestFieldHash);
    core.SetWriteTs(10);
    for (int i = 0; i < 120; ++i)
    {
        core.Put("k" + std::to_string(i), "v");
    }
    const uint64_t kMetaTtl = 1234567;
    FlushCycle(core, kKey, store, 10, kMetaTtl);

    const FakeStore::Row *meta = store.Get(kKey);
    CHECK(meta != nullptr);
    CHECK(meta->ttl_ == kMetaTtl);

    std::vector<uint32_t> seen_ids;
    for (const auto &[key, row] : store.rows_)
    {
        txservice::PageKeyParts parts;
        if (!txservice::DecodePageKey(key, parts))
        {
            CHECK(key == kKey);  // the metadata row is the only non-page row
            continue;
        }
        CHECK(parts.object_key_ == kKey);
        CHECK(parts.kind_ == PageRowKind::HashPage);
        CHECK(row.value_.size() == core.Frames().PageSize());
        // Page rows never carry a store-TTL attribute (§9).
        CHECK(row.ttl_ == 0);
        seen_ids.push_back(parts.page_id_);
    }
    CHECK(!seen_ids.empty());
    // Big-endian ids: map order (byte order) is page-id order.
    for (size_t i = 1; i < seen_ids.size(); ++i)
    {
        CHECK(seen_ids[i - 1] < seen_ids[i]);
    }
    std::printf("row shape and TTL placement: ok (%zu page rows)\n",
                seen_ids.size());
}

// Only the pages a cycle dirtied are rewritten — the write-amplification
// property paging exists for (§1 goals).
void TestIncrementalDirtySet()
{
    const std::string kKey = "h";
    FakeStore store;
    RedisPagedHashObject core(256, &TestFieldHash);
    core.SetWriteTs(10);
    for (int i = 0; i < 500; ++i)
    {
        core.Put("f" + std::to_string(i), "v");
    }
    FlushStats first = FlushCycle(core, kKey, store, 10);
    CHECK(first.page_puts_ > 5);

    // One field update dirties exactly one page.
    core.SetWriteTs(20);
    core.Put("f13", "updated");
    FlushStats second = FlushCycle(core, kKey, store, 20);
    CHECK(second.page_puts_ == 1);

    // Two fields on (probably) different pages.
    core.SetWriteTs(30);
    core.Put("f1", "x");
    core.Put("f400", "y");
    FlushStats third = FlushCycle(core, kKey, store, 30);
    CHECK(third.page_puts_ >= 1);
    CHECK(third.page_puts_ <= 2);
    std::printf("incremental dirty set: ok (%zu, %zu, %zu pages)\n",
                first.page_puts_,
                second.page_puts_,
                third.page_puts_);
}

// A crash between the page flush and the metadata flush must not corrupt: the
// metadata row is the replay watermark, so an older metadata row plus newer
// page rows still reads consistently — every stored page reflects all commands
// up to the stored metadata's commit ts (§10), and the un-replayed remainder
// comes from the WAL.
void TestCrashBetweenPageAndMetadataFlush()
{
    const std::string kKey = "h";
    FakeStore store;
    RedisPagedHashObject core(256, &TestFieldHash);
    core.SetWriteTs(10);
    for (int i = 0; i < 200; ++i)
    {
        core.Put("f" + std::to_string(i), "v");
    }
    FlushCycle(core, kKey, store, 10);

    // Simulate the torn write the design forbids: page rows written, metadata
    // row NOT updated. One atomic batch makes this impossible; the test
    // pins down *why* it must be atomic.
    //
    // A same-length replacement is deliberate — it is §4's "tempting
    // optimization" case (field count and byte length unchanged, à la
    // HINCRBY 5 -> 6), which dirties exactly one page and changes no
    // structure. A longer value could split, which would move directory
    // entries too and make this a different (structural) scenario.
    core.SetWriteTs(20);
    CHECK(!core.Put("f5", "w"));  // update, not insert
    std::vector<std::pair<PageId, PageBuf>> dirty;
    core.Frames().ForEachDirtyPage([&](PageId id, const PageBuf &buf)
                                   { dirty.emplace_back(id, buf); });
    CHECK(dirty.size() == 1);
    for (const auto &[id, buf] : dirty)
    {
        std::string key;
        txservice::EncodePageKey(key, kKey, PageRowKind::HashPage, id);
        store.Put(key,
                  std::string(reinterpret_cast<const char *>(buf.get()),
                              core.Frames().PageSize()),
                  20,
                  0);
    }

    // Reload against the OLD metadata row.
    const FakeStore::Row *meta_row = store.Get(kKey);
    RedisPagedHashObject reloaded(&TestFieldHash);
    size_t offset = 0;
    CHECK(reloaded.DeserializeSections(
        meta_row->value_.data(), meta_row->value_.size(), offset));
    CHECK(meta_row->commit_ts_ == 10);

    // The hazard, made concrete: the page row now carries a NEWER commit ts
    // than the metadata row that is supposed to dominate it. Replay gated on
    // the metadata's ts (10) would re-apply the command the page already
    // reflects — which for a non-idempotent command corrupts. This is exactly
    // why §4 forbids skipping the metadata rewrite on a value-only update and
    // why §9 writes both in one batch.
    uint64_t h = 0;
    PageId pid = reloaded.RouteField("f5", &h);
    std::string page_key;
    txservice::EncodePageKey(page_key, kKey, PageRowKind::HashPage, pid);
    CHECK(store.Get(page_key)->commit_ts_ == 20);
    CHECK(meta_row->commit_ts_ == 10);

    // Structure survives regardless: the directory still routes and the page
    // parses, so the failure mode is a stale watermark, never a torn layout.
    CHECK(FaultPage(reloaded, kKey, store, pid));
    CHECK(*reloaded.Get("f5") == "w");
    CHECK(reloaded.CheckInvariants());
    std::printf("crash between page and metadata flush: ok\n");
}
// Conversion determinism, stated as §10 actually requires it: the SAME
// command order must produce byte-identical layouts, because replay applies a
// key's commands in log order and the un-replayed on-disk pages must match.
//
// It is deliberately NOT order-independence. Extendible hashing allocates page
// ids in the order splits occur, so a different insertion order legitimately
// yields a different id assignment for the same logical content — which is
// fine, since no replay path ever reorders one key's commands. Asserting the
// stronger property would fail on correct code.
void TestConversionDeterminism()
{
    std::vector<std::pair<std::string, std::string>> owned;
    for (int i = 0; i < 400; ++i)
    {
        owned.emplace_back("field:" + std::to_string(i),
                           "value:" + std::to_string(i * 7));
    }

    auto build = [&](const std::vector<size_t> &order)
    {
        auto core = std::make_unique<RedisPagedHashObject>(512, &TestFieldHash);
        core->SetWriteTs(50);
        for (size_t idx : order)
        {
            core->Put(owned[idx].first, owned[idx].second);
        }
        return core;
    };

    std::vector<size_t> forward;
    for (size_t i = 0; i < owned.size(); ++i)
    {
        forward.push_back(i);
    }

    // Same order, twice: byte-identical metadata AND pages. This is the
    // property replay depends on.
    std::string meta_a, meta_b;
    std::vector<std::pair<PageId, std::string>> pages_a, pages_b;
    build(forward)->SerializeAll(meta_a, pages_a);
    build(forward)->SerializeAll(meta_b, pages_b);
    CHECK(meta_a == meta_b);
    CHECK(pages_a == pages_b);
    CHECK(pages_a.size() > 1);  // actually split; not a degenerate one-pager

    // A different order may lay pages out differently, but must preserve
    // every field's value and the logical size.
    std::vector<size_t> reverse(forward.rbegin(), forward.rend());
    auto fwd = build(forward);
    auto rev = build(reverse);
    CHECK(fwd->FieldCount() == rev->FieldCount());
    CHECK(fwd->LogicalBytes() == rev->LogicalBytes());
    for (const auto &[field, value] : owned)
    {
        auto a = fwd->Get(field);
        auto b = rev->Get(field);
        CHECK(a.has_value() && b.has_value());
        CHECK(*a == value);
        CHECK(*b == value);
    }
    CHECK(fwd->CheckInvariants());
    CHECK(rev->CheckInvariants());

    // Every page a conversion produces is dirty, so the first checkpoint
    // writes the metadata row and all pages as one indivisible record (§9).
    size_t dirty = 0;
    fwd->Frames().ForEachDirtyPage([&](PageId, const PageBuf &) { ++dirty; });
    CHECK(dirty == pages_a.size());

    std::printf("conversion determinism: ok (%zu pages, all dirty)\n",
                pages_a.size());
}
}  // namespace

int main()
{
    TestFlushEvictFaultRoundTrip();
    TestMidFlushWriteStaysDirty();
    TestPendingDeleteDrainScoping();
    TestDeletionFanOut();
    TestRowShapeAndTtlPlacement();
    TestIncrementalDirtySet();
    TestCrashBetweenPageAndMetadataFlush();
    TestConversionDeterminism();
    std::printf("all paged flush round-trip tests passed\n");
    return 0;
}
