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

// Phase 5 unit tests: partial eviction of paged objects (docs/08 §8).
// Standalone, like the other paged tests:
//   g++ -std=c++20 -fsanitize=address,undefined -I include -I data_substrate
//       tests/unit_cc/paged_eviction_test.cpp -o paged_eviction_test
//
// What these pin down, in the order §8 states the policy:
//   - the internal LRU is maintained by every page access and stays
//     structurally consistent across split / free / install / copy / load;
//   - victims come from the COLD end;
//   - dirty and pinned pages are never shed;
//   - the batch is 10 % of EVICTABLE pages, never fewer than one;
//   - repeated visits converge (no livelock) and leave routable metadata;
//   - reclaim accounting stays honest when another reference keeps a shed
//     page's bytes alive.
//
// Note on method: every page read goes through the core's View(), which is the
// LRU touch point -- so ForEachEntry(), which views every page, would scramble
// the order. These tests therefore locate a field on a given page with
// RouteField() and shape the LRU deliberately with Get().

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "redis_paged_hash_object.h"

namespace
{
using namespace EloqKV;

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

constexpr uint32_t kPageSize = 512;

std::string FieldName(size_t i)
{
    return "field" + std::to_string(i);
}

/**
 * @brief Builds a core with `field_cnt` fields, then runs the §9 checkpoint
 * sequence so every page is clean and therefore an eviction candidate.
 */
RedisPagedHashObject BuildCleanCore(size_t field_cnt, uint64_t flush_ts = 100)
{
    RedisPagedHashObject core(kPageSize, TestFieldHash);
    core.SetWriteTs(10);
    for (size_t i = 0; i < field_cnt; ++i)
    {
        core.Put(FieldName(i), "value" + std::to_string(i));
    }
    core.StampWrites(flush_ts);
    core.OnPagedFlushApplied(flush_ts);
    CHECK(core.CheckInvariants());
    return core;
}

/**
 * @brief One field routing to each currently-live page, so a test can touch or
 * dirty a specific page without ForEachEntry's side effect on the LRU.
 */
std::vector<std::pair<PageId, std::string>> FieldPerPage(
    const RedisPagedHashObject &core, size_t field_cnt)
{
    std::vector<std::pair<PageId, std::string>> out;
    std::set<PageId> seen;
    for (size_t i = 0; i < field_cnt; ++i)
    {
        std::string f = FieldName(i);
        PageId pid = core.RouteField(f, nullptr);
        if (seen.insert(pid).second)
        {
            out.emplace_back(pid, f);
        }
    }
    return out;
}

size_t DirtyPageCount(const RedisPagedHashObject &core)
{
    size_t n = 0;
    core.Frames().ForEachDirtyPage([&](PageId, const PageBuf &) { ++n; });
    return n;
}

void TestLruMaintainedByAccess()
{
    constexpr size_t kFields = 200;
    RedisPagedHashObject core = BuildCleanCore(kFields);
    CHECK(core.ResidentPageCount() > 3);

    std::vector<PageId> order = core.Frames().LruColdToHot();
    CHECK(order.size() == core.ResidentPageCount());
    std::set<PageId> unique(order.begin(), order.end());
    CHECK(unique.size() == order.size());  // each resident page linked once

    // Reading a field moves its page to the hot end.
    PageId cold = order.front();
    std::string probe;
    for (const auto &[pid, field] : FieldPerPage(core, kFields))
    {
        if (pid == cold)
        {
            probe = field;
        }
    }
    CHECK(!probe.empty());
    CHECK(core.Get(probe).has_value());

    std::vector<PageId> after = core.Frames().LruColdToHot();
    CHECK(after.size() == order.size());
    CHECK(after.back() == cold);   // now the hottest
    CHECK(after.front() != cold);  // no longer the coldest
    CHECK(core.CheckInvariants());
    std::printf("lru maintained by access: ok (%zu pages)\n", after.size());
}

void TestShedTakesColdestFirst()
{
    RedisPagedHashObject core = BuildCleanCore(300);
    std::vector<PageId> order = core.Frames().LruColdToHot();
    CHECK(order.size() >= 10);

    CHECK(core.MutableFrames().ShedColdPages(3) == 3);
    for (size_t i = 0; i < 3; ++i)
    {
        CHECK(!core.Frames().IsResident(order[i]));
    }
    for (size_t i = 3; i < order.size(); ++i)
    {
        CHECK(core.Frames().IsResident(order[i]));
    }
    // What remains is the old list minus its cold prefix, order preserved.
    std::vector<PageId> rest = core.Frames().LruColdToHot();
    CHECK(rest.size() == order.size() - 3);
    for (size_t i = 0; i < rest.size(); ++i)
    {
        CHECK(rest[i] == order[i + 3]);
    }
    CHECK(core.CheckInvariants());
    std::printf("shed takes coldest first: ok\n");
}

void TestDirtyAndPinnedNeverShed()
{
    constexpr size_t kFields = 300;
    RedisPagedHashObject core = BuildCleanCore(kFields);
    std::vector<std::pair<PageId, std::string>> per_page =
        FieldPerPage(core, kFields);
    CHECK(per_page.size() >= 8);

    // Dirty two pages by writing to them. That also makes them hot, so touch
    // every other page afterwards to push the dirty pair back to the cold end
    // -- the position where the shed walk will actually consider them.
    core.SetWriteTs(200);
    PageId dirty_a = per_page[0].first, dirty_b = per_page[1].first;
    core.Put(per_page[0].second, "rewritten-a");
    core.Put(per_page[1].second, "rewritten-b");
    CHECK(DirtyPageCount(core) == 2);
    for (size_t i = 2; i < per_page.size(); ++i)
    {
        CHECK(core.Get(per_page[i].second).has_value());
    }

    std::vector<PageId> order = core.Frames().LruColdToHot();
    CHECK(order[0] == dirty_a || order[0] == dirty_b);
    CHECK(order[1] == dirty_a || order[1] == dirty_b);

    // Pin the two next-coldest. PinPage is not an access, so it does not move
    // them.
    PageId pinned_a = order[2], pinned_b = order[3];
    core.MutableFrames().PinPage(pinned_a);
    core.MutableFrames().PinPage(pinned_b);

    CHECK(core.Frames().EvictablePageCount() == core.ResidentPageCount() - 4);

    // Asking for four victims must walk past all four protected pages.
    CHECK(core.MutableFrames().ShedColdPages(4) == 4);
    CHECK(core.Frames().IsResident(dirty_a));
    CHECK(core.Frames().IsResident(dirty_b));
    CHECK(core.Frames().IsResident(pinned_a));
    CHECK(core.Frames().IsResident(pinned_b));
    CHECK(core.CheckInvariants());

    // Drain the rest: with only the protected four left, the object yields
    // nothing and the pass moves on -- as it does for a dirty whole entry
    // today.
    while (core.ShedCleanPages() > 0)
    {
    }
    CHECK(core.ResidentPageCount() == 4);
    CHECK(core.Frames().EvictablePageCount() == 0);
    CHECK(core.ShedCleanPages() == 0);

    // Unpinning restores candidacy; being dirty still does not.
    core.MutableFrames().UnpinPage(pinned_a);
    core.MutableFrames().UnpinPage(pinned_b);
    CHECK(core.Frames().EvictablePageCount() == 2);
    CHECK(core.ShedCleanPages() > 0);
    while (core.ShedCleanPages() > 0)
    {
    }
    CHECK(core.ResidentPageCount() == 2);  // the dirty pair survives
    CHECK(DirtyPageCount(core) == 2);
    CHECK(core.CheckInvariants());
    std::printf("dirty and pinned never shed: ok\n");
}

void TestTenPercentPolicyAndFloor()
{
    // The batch is 10 % of EVICTABLE pages, not of resident pages: an object
    // whose pages are mostly pinned must not have its small clean remainder
    // over-harvested, and one with nothing evictable must yield 0 rather than
    // the floor of 1.
    constexpr size_t kFields = 4000;
    RedisPagedHashObject core = BuildCleanCore(kFields);
    size_t resident = core.ResidentPageCount();
    CHECK(resident >= 40);  // enough that 10 % is well above the floor

    size_t evictable = core.Frames().EvictablePageCount();
    CHECK(evictable == resident);
    CHECK(core.ShedCleanPages() == evictable / 10);

    // Pin all but 25 pages; 10 % of 25 is 2 (floor division), not 10 % of
    // resident.
    std::vector<PageId> order = core.Frames().LruColdToHot();
    CHECK(order.size() > 25);
    for (size_t i = 25; i < order.size(); ++i)
    {
        core.MutableFrames().PinPage(order[i]);
    }
    CHECK(core.Frames().EvictablePageCount() == 25);
    CHECK(core.ShedCleanPages() == 2);
    CHECK(core.Frames().EvictablePageCount() == 23);

    // The floor of one keeps small objects making progress where 10 % rounds
    // to zero.
    RedisPagedHashObject small = BuildCleanCore(4);
    CHECK(small.ResidentPageCount() >= 1);
    CHECK(small.Frames().EvictablePageCount() / 10 == 0);
    CHECK(small.ShedCleanPages() == 1);

    // Nothing evictable => 0, never the floor.
    RedisPagedHashObject all_dirty(kPageSize, TestFieldHash);
    all_dirty.SetWriteTs(10);
    for (size_t i = 0; i < 100; ++i)
    {
        all_dirty.Put(FieldName(i), "v");
    }
    CHECK(all_dirty.Frames().EvictablePageCount() == 0);
    CHECK(all_dirty.ShedCleanPages() == 0);
    CHECK(all_dirty.ResidentPageCount() > 0);
    std::printf("10%% policy and floor: ok\n");
}

void TestConvergenceNoLivelock()
{
    // §8's forward-progress claim: repeated visits drive a fully clean,
    // unpinned object to metadata-only in a bounded number of passes. Geometric
    // decay plus the floor of one is what rules out the shard sweeping forever
    // and reclaiming nothing.
    RedisPagedHashObject core = BuildCleanCore(4000);
    size_t start = core.ResidentPageCount();
    CHECK(start > 20);

    size_t passes = 0;
    while (core.ResidentPageCount() > 0)
    {
        size_t before = core.ResidentPageCount();
        size_t shed = core.ShedCleanPages();
        CHECK(shed > 0);  // never stalls while something is evictable
        CHECK(core.ResidentPageCount() == before - shed);
        CHECK(core.CheckInvariants());
        ++passes;
        CHECK(passes <= start);  // bounded, not merely terminating
    }

    // Terminal state: metadata only, and still routable -- the directory and
    // per-page counts survive, which is what lets the pages be faulted back.
    CHECK(core.ResidentPageCount() == 0);
    CHECK(core.Frames().LruColdToHot().empty());
    CHECK(core.Frames().LivePageCount() > 0);
    CHECK(core.Meta().dir_.size() == (size_t{1} << core.Meta().global_depth_));
    CHECK(core.MetaSerializedSize() > 0);
    CHECK(core.CheckInvariants());
    std::printf("convergence, no livelock: ok (%zu pages in %zu passes)\n",
                start,
                passes);
}

void TestShedIsHonestWhenBytesStayAlive()
{
    // §8's accounting caveat: dropping a page whose buffer another reference
    // keeps alive -- a flush worker holding the exported page -- must not be
    // credited as memory reclaimed. Residency and reclaimed bytes are different
    // quantities, which is why the shard measures its heap directly instead of
    // crediting sheds.
    RedisPagedHashObject core = BuildCleanCore(200);
    PageId victim = core.Frames().LruColdToHot().front();

    const PageSlot *slot = core.Frames().SlotOf(victim);
    CHECK(slot != nullptr && slot->buf_ != nullptr);
    PageBuf held = slot->buf_;  // as ExportPagedFlush shares it, by reference
    CHECK(held.use_count() == 2);

    size_t resident_before = core.ResidentPageCount();
    CHECK(core.MutableFrames().ShedPage(victim));
    CHECK(core.ResidentPageCount() == resident_before - 1);
    CHECK(!core.Frames().IsResident(victim));
    CHECK(core.Frames().SlotOf(victim) == nullptr);
    // The bytes are still alive, held by the other reference: a shard that
    // credited this shed as reclaimed would spin believing it progressed.
    CHECK(held != nullptr);
    CHECK(held.use_count() == 1);
    CHECK(core.CheckInvariants());
    std::printf("shed accounting stays honest: ok\n");
}

void TestLruSurvivesStructuralChange()
{
    // Splits, frees, re-installs, copies and metadata-only loads all mutate the
    // resident set; each must leave the LRU naming exactly that set. A stale id
    // here would make the shed walk dereference a freed page.
    constexpr size_t kFields = 500;
    RedisPagedHashObject core(kPageSize, TestFieldHash);
    core.SetWriteTs(10);
    for (size_t i = 0; i < kFields; ++i)
    {
        core.Put(FieldName(i), "v" + std::to_string(i));
        CHECK(core.CheckInvariants());
    }
    CHECK(core.Meta().global_depth_ >= 1);

    for (size_t i = 0; i < kFields; i += 2)
    {
        core.Del(FieldName(i));
        CHECK(core.CheckInvariants());
    }

    core.StampWrites(50);
    core.OnPagedFlushApplied(50);
    std::vector<PageId> src_order = core.Frames().LruColdToHot();

    // Deep copy: links are ids into the copy's own map, and the eviction order
    // is reproduced rather than reset.
    RedisPagedHashObject copy(core);
    CHECK(copy.CheckInvariants());
    CHECK(copy.Frames().LruColdToHot() == src_order);
    // Independent: shedding from the copy leaves the original untouched.
    CHECK(copy.ShedCleanPages() > 0);
    CHECK(copy.CheckInvariants());
    CHECK(core.Frames().LruColdToHot() == src_order);
    CHECK(core.CheckInvariants());

    // Metadata-only load: empty LRU, and shedding is a no-op.
    std::string meta;
    core.SerializeMeta(meta);
    RedisPagedHashObject loaded(TestFieldHash);
    size_t off = 0;
    CHECK(loaded.DeserializeSections(meta.data(), meta.size(), off));
    CHECK(loaded.ResidentPageCount() == 0);
    CHECK(loaded.Frames().LruColdToHot().empty());
    CHECK(loaded.ShedCleanPages() == 0);
    CHECK(loaded.CheckInvariants());

    // Installed pages enter at the hot end, so install order is cold-to-hot.
    std::vector<PageId> live;
    core.Frames().ForEachLivePageId([&](PageId id) { live.push_back(id); });
    CHECK(live.size() >= 2);
    for (size_t i = 0; i < 2; ++i)
    {
        const PageSlot *s = core.Frames().SlotOf(live[i]);
        CHECK(s != nullptr && s->buf_ != nullptr);
        CHECK(loaded.InstallPage(
            live[i],
            std::string_view(reinterpret_cast<const char *>(s->buf_.get()),
                             kPageSize),
            50));
        CHECK(loaded.CheckInvariants());
    }
    std::vector<PageId> loaded_order = loaded.Frames().LruColdToHot();
    CHECK(loaded_order.size() == 2);
    CHECK(loaded_order.front() == live[0]);
    CHECK(loaded_order.back() == live[1]);

    // An installed page is clean, so it is immediately a candidate.
    CHECK(loaded.Frames().EvictablePageCount() == 2);
    CHECK(loaded.ShedCleanPages() == 1);
    CHECK(!loaded.Frames().IsResident(live[0]));  // the colder of the two
    CHECK(loaded.CheckInvariants());
    std::printf("lru survives structural change: ok\n");
}
/**
 * @brief §7 copy-on-write. A clone SHARES page buffers with its source; the
 * first write on either side copies. This is what makes a write transaction
 * affordable — the dirty object is a clone of the committed one, and deep
 * copying would duplicate the whole resident page set per write.
 */
void TestCopyOnWrite()
{
    constexpr size_t kFields = 300;
    RedisPagedHashObject src = BuildCleanCore(kFields);
    std::vector<std::pair<PageId, std::string>> per_page =
        FieldPerPage(src, kFields);
    CHECK(per_page.size() >= 4);

    // The clone shares every resident buffer: same address, use_count 2.
    RedisPagedHashObject clone(src);
    CHECK(clone.ResidentPageCount() == src.ResidentPageCount());
    for (const auto &[pid, field] : per_page)
    {
        const PageSlot *a = src.Frames().SlotOf(pid);
        const PageSlot *b = clone.Frames().SlotOf(pid);
        CHECK(a != nullptr && b != nullptr);
        CHECK(a->buf_.get() == b->buf_.get());  // shared, not copied
        CHECK(a->buf_.use_count() == 2);
    }

    // Writing through the clone copies only the page it touches...
    PageId touched = per_page[0].first;
    std::string touched_field = per_page[0].second;
    PageId untouched = per_page[1].first;
    clone.SetWriteTs(200);
    clone.Put(touched_field, "clone-only-value");

    CHECK(src.Frames().SlotOf(touched)->buf_.get() !=
          clone.Frames().SlotOf(touched)->buf_.get());
    CHECK(src.Frames().SlotOf(touched)->buf_.use_count() == 1);
    CHECK(clone.Frames().SlotOf(touched)->buf_.use_count() == 1);
    // ...and leaves every other page shared.
    CHECK(src.Frames().SlotOf(untouched)->buf_.get() ==
          clone.Frames().SlotOf(untouched)->buf_.get());
    CHECK(src.Frames().SlotOf(untouched)->buf_.use_count() == 2);

    // The source is untouched by the clone's write — the property the whole
    // mechanism exists for.
    std::optional<std::string_view> from_src = src.Get(touched_field);
    CHECK(from_src.has_value());
    CHECK(*from_src != "clone-only-value");
    std::optional<std::string_view> from_clone = clone.Get(touched_field);
    CHECK(from_clone.has_value() && *from_clone == "clone-only-value");

    CHECK(src.CheckInvariants());
    CHECK(clone.CheckInvariants());
    std::printf("copy-on-write: ok (shared, split on write)\n");
}

/**
 * @brief §9's zero-copy flush claim: the export shares page buffers, so a
 * write concurrent with a flush must COPY rather than mutate the bytes the
 * flush worker is about to write out. Before copy-on-write existed this
 * comment was aspirational — the write went straight into the exported buffer.
 */
void TestFlushExportIsStableUnderWrite()
{
    constexpr size_t kFields = 300;
    RedisPagedHashObject core = BuildCleanCore(kFields);
    std::vector<std::pair<PageId, std::string>> per_page =
        FieldPerPage(core, kFields);
    CHECK(!per_page.empty());

    // Dirty one page so it appears in the export set, then export by
    // reference exactly as the checkpoint does.
    core.SetWriteTs(200);
    PageId target = per_page[0].first;
    core.Put(per_page[0].second, "before-flush");

    std::vector<std::pair<PageId, PageBuf>> exported;
    core.Frames().ForEachDirtyPage([&](PageId id, const PageBuf &buf)
                                   { exported.emplace_back(id, buf); });
    CHECK(!exported.empty());

    // Snapshot the exported bytes of the page we are about to overwrite.
    PageBuf held;
    for (auto &[id, buf] : exported)
    {
        if (id == target)
        {
            held = buf;
        }
    }
    CHECK(held != nullptr);
    CHECK(held.use_count() >= 2);  // core + "flush worker"
    std::vector<uint8_t> snapshot(held.get(), held.get() + kPageSize);

    // Now write to that page while the export is still held.
    core.SetWriteTs(300);
    core.Put(per_page[0].second, "written-during-flush");

    // The flush worker's bytes are unchanged, and the core moved to its own
    // buffer.
    CHECK(std::memcmp(held.get(), snapshot.data(), kPageSize) == 0);
    CHECK(core.Frames().SlotOf(target)->buf_.get() != held.get());
    CHECK(core.Get(per_page[0].second).value() == "written-during-flush");
    CHECK(core.CheckInvariants());
    std::printf("flush export stable under concurrent write: ok\n");
}

/**
 * @brief Shedding a page that a clone still shares must not disturb the clone,
 * and must not be credited as reclaimed memory (§8's accounting caveat).
 */
void TestShedWithSharedBuffers()
{
    RedisPagedHashObject src = BuildCleanCore(300);
    RedisPagedHashObject clone(src);

    PageId victim = src.Frames().LruColdToHot().front();
    const uint8_t *addr = src.Frames().SlotOf(victim)->buf_.get();
    CHECK(clone.Frames().SlotOf(victim) != nullptr);
    CHECK(clone.Frames().SlotOf(victim)->buf_.get() == addr);

    CHECK(src.MutableFrames().ShedPage(victim));
    CHECK(!src.Frames().IsResident(victim));
    // The clone still has it, at the same address: the bytes were not freed,
    // so nothing was actually reclaimed.
    CHECK(clone.Frames().IsResident(victim));
    CHECK(clone.Frames().SlotOf(victim)->buf_.get() == addr);
    CHECK(clone.Frames().SlotOf(victim)->buf_.use_count() == 1);
    CHECK(src.CheckInvariants());
    CHECK(clone.CheckInvariants());
    std::printf("shed with shared buffers: ok\n");
}
}  // namespace

int main()
{
    TestLruMaintainedByAccess();
    TestShedTakesColdestFirst();
    TestDirtyAndPinnedNeverShed();
    TestTenPercentPolicyAndFloor();
    TestConvergenceNoLivelock();
    TestShedIsHonestWhenBytesStayAlive();
    TestLruSurvivesStructuralChange();
    TestCopyOnWrite();
    TestFlushExportIsStableUnderWrite();
    TestShedWithSharedBuffers();
    std::printf("all paged eviction tests passed\n");
    return 0;
}
