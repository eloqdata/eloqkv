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

// Structural unit tests for the paged hash (docs/08-paged-objects-plan.md):
// page format, metadata codec, splits/doubling, scan guarantees. Built by the
// project (the object under test reaches the full protocol layer through its
// vtable, so the binary links a static build of the eloqkv sources). The
// field hash is injected (splitmix64 here, adversarial variants below); the
// pinned production hash is exercised in-tree, not here.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "redis_paged_hash_object.h"
#include "tx_service/include/page_key_codec.h"

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

// Deterministic 64-bit mixer (stable across runs and platforms).
uint64_t SplitMix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

uint64_t TestFieldHash(std::string_view field)
{
    uint64_t h = 0xE10A0B5EULL;  // arbitrary fixed seed
    for (char c : field)
    {
        h = SplitMix64(h ^ static_cast<uint8_t>(c));
    }
    return h;
}

// PRNG for property tests.
struct Rng
{
    uint64_t state_;
    explicit Rng(uint64_t seed) : state_(seed)
    {
    }
    uint64_t Next()
    {
        state_ = SplitMix64(state_);
        return state_;
    }
    uint64_t Below(uint64_t n)
    {
        return Next() % n;
    }
};

void TestVarint()
{
    using namespace paged_detail;
    uint8_t buf[10];
    for (uint64_t v : {uint64_t{0},
                       uint64_t{1},
                       uint64_t{127},
                       uint64_t{128},
                       uint64_t{300},
                       uint64_t{16383},
                       uint64_t{16384},
                       uint64_t{UINT32_MAX},
                       (uint64_t{UINT32_MAX} << 1) | 1,
                       UINT64_MAX})
    {
        uint8_t *end = WriteVarint(buf, v);
        CHECK(static_cast<size_t>(end - buf) == VarintSize(v));
        uint64_t out = 0;
        const uint8_t *p = ReadVarint(buf, buf + sizeof(buf), out);
        CHECK(p == end);
        CHECK(out == v);
    }
    std::printf("varint: ok\n");
}

void TestPageKeyCodec()
{
    using namespace txservice;
    // Round trip, including binary object keys with embedded NUL and magic
    // bytes inside the key body.
    std::string object_key(
        "\x00\x01"
        "EKVPAGE"
        "\xFFuser:42",
        16);
    for (uint32_t page_id : {0u, 1u, 255u, 0xDEADBEEFu, UINT32_MAX})
    {
        for (PageRowKind kind :
             {PageRowKind::HashPage, PageRowKind::LargeValuePage})
        {
            std::string enc;
            EncodePageKey(enc, object_key, kind, page_id);
            CHECK(enc.size() == kPageKeyOverhead + object_key.size());
            CHECK(HasPageKeyMagic(enc));
            PageKeyParts parts;
            CHECK(DecodePageKey(enc, parts));
            CHECK(parts.object_key_ == object_key);
            CHECK(parts.kind_ == kind);
            CHECK(parts.page_id_ == page_id);
        }
    }
    // Big-endian ids sort page rows numerically.
    std::string a;
    std::string b;
    EncodePageKey(a, "k", PageRowKind::HashPage, 1);
    EncodePageKey(b, "k", PageRowKind::HashPage, 256);
    CHECK(a < b);

    // PREFIX SAFETY (docs/08 §5): object key "a" is a byte-prefix of
    // "a\0\0\0\0\0", and without the length field their page rows
    // interleave and a per-object prefix delete removes the other object's
    // pages. With it, one object's rows are contiguous and its prefix covers
    // them exactly.
    std::string key_a("a");
    std::string key_b("a\0\0\0\0\0", 6);
    std::string a0;
    std::string a1;
    std::string b0;
    EncodePageKey(a0, key_a, PageRowKind::HashPage, 0);
    EncodePageKey(a1, key_a, PageRowKind::HashPage, 1);
    EncodePageKey(b0, key_b, PageRowKind::HashPage, 0);
    // B's rows never land between A's.
    CHECK(!(a0 < b0 && b0 < a1));
    // A's prefix covers A's rows and none of B's.
    std::string prefix_a;
    EncodePageKeyPrefix(prefix_a, key_a);
    CHECK(a0.compare(0, prefix_a.size(), prefix_a) == 0);
    CHECK(a1.compare(0, prefix_a.size(), prefix_a) == 0);
    CHECK(b0.compare(0, prefix_a.size(), prefix_a) != 0);
    // And the full key is exactly prefix + suffix.
    CHECK(a0.size() == prefix_a.size() + kPageKeySuffixLen);
    // Non-page keys are rejected.
    PageKeyParts parts;
    CHECK(!DecodePageKey("user:42", parts));
    CHECK(!DecodePageKey(std::string("\x00"
                                     "EKVPAGE",
                                     8),
                         parts));  // too short
    // A bad kind byte fails decode.
    std::string bad;
    EncodePageKey(bad, "k", PageRowKind::HashPage, 7);
    bad[bad.size() - 5] = 0x7F;
    CHECK(!DecodePageKey(bad, parts));
    // A length field that disagrees with the actual size fails decode.
    std::string lied;
    EncodePageKey(lied, "kk", PageRowKind::HashPage, 7);
    lied[kPageKeyMagicLen + 3] = 9;  // low byte: claims key_len 9, actual 2
    CHECK(!DecodePageKey(lied, parts));
    std::printf("page key codec: ok\n");
}

void TestPageViewBasics()
{
    constexpr uint32_t kPage = 4096;
    std::vector<uint8_t> buf(kPage);
    PageView pv(buf.data(), kPage, 0);
    pv.Init(0);
    CHECK(pv.LayoutVersion() == PageView::kPageLayoutVersion);
    CHECK(pv.EntryCount() == 0);
    CHECK(pv.Find(1, "a") == PageView::kNpos);

    // Insert out of hash order; slots must stay ascending.
    CHECK(pv.Write(30, "c", "vc") == PageView::WriteResult::Inserted);
    CHECK(pv.Write(10, "a", "va") == PageView::WriteResult::Inserted);
    CHECK(pv.Write(20, "b", "vb") == PageView::WriteResult::Inserted);
    CHECK(pv.EntryCount() == 3);
    CHECK(pv.Hash32At(0) == 10 && pv.Hash32At(1) == 20 && pv.Hash32At(2) == 30);
    CHECK(pv.ValueAt(pv.Find(10, "a")) == "va");
    CHECK(pv.ValueAt(pv.Find(20, "b")) == "vb");
    CHECK(pv.ValueAt(pv.Find(30, "c")) == "vc");

    // Equal-hash run: same hash32, different keys.
    CHECK(pv.Write(20, "b2", "vb2") == PageView::WriteResult::Inserted);
    CHECK(pv.ValueAt(pv.Find(20, "b")) == "vb");
    CHECK(pv.ValueAt(pv.Find(20, "b2")) == "vb2");
    CHECK(pv.Find(20, "b3") == PageView::kNpos);

    // Update: shrink and grow; dead bytes accumulate.
    size_t old_len = 0;
    CHECK(pv.Write(10, "a", "V", &old_len) == PageView::WriteResult::Updated);
    CHECK(old_len == 2);
    CHECK(pv.ValueAt(pv.Find(10, "a")) == "V");
    CHECK(pv.DeadBytes() > 0);
    CHECK(pv.Write(10, "a", "longer-value") == PageView::WriteResult::Updated);
    CHECK(pv.ValueAt(pv.Find(10, "a")) == "longer-value");

    // Erase from the middle of the equal run.
    pv.EraseAt(pv.Find(20, "b"));
    CHECK(pv.EntryCount() == 3);
    CHECK(pv.Find(20, "b") == PageView::kNpos);
    CHECK(pv.ValueAt(pv.Find(20, "b2")) == "vb2");
    CHECK(pv.ValueAt(pv.Find(30, "c")) == "vc");

    // Compact preserves content and zeroes dead bytes.
    uint32_t dead = pv.DeadBytes();
    CHECK(dead > 0);
    pv.Compact();
    CHECK(pv.DeadBytes() == 0);
    CHECK(pv.ValueAt(pv.Find(10, "a")) == "longer-value");
    CHECK(pv.ValueAt(pv.Find(20, "b2")) == "vb2");
    CHECK(pv.ValueAt(pv.Find(30, "c")) == "vc");
    std::printf("page view basics: ok\n");
}

void TestPageViewFillAndSplit()
{
    constexpr uint32_t kPage = 1024;
    std::vector<uint8_t> low_buf(kPage);
    PageView low(low_buf.data(), kPage, 0);
    low.Init(0);
    // Fill until NeedSplit; hash = key index spread over both halves of the
    // top bit.
    std::vector<std::pair<uint32_t, std::string>> entries;
    uint32_t i = 0;
    for (;; ++i)
    {
        uint32_t h = (i % 2 == 0) ? (0x10000000u + i) : (0x90000000u + i);
        std::string key = "key" + std::to_string(i);
        PageView::WriteResult r = low.Write(h, key, "value");
        if (r == PageView::WriteResult::NeedSplit)
        {
            break;
        }
        CHECK(r == PageView::WriteResult::Inserted);
        entries.emplace_back(h, key);
    }
    CHECK(entries.size() > 10);

    std::vector<uint8_t> high_buf(kPage);
    PageView high(high_buf.data(), kPage, 0);
    high.Init(0);
    uint32_t moved = low.SplitInto(high, 0);  // split on the top bit
    CHECK(moved > 0);
    CHECK(low.LocalDepth() == 1 && high.LocalDepth() == 1);
    CHECK(low.EntryCount() + high.EntryCount() == entries.size());
    for (const auto &[h, key] : entries)
    {
        PageView &owner = (h & 0x80000000u) ? high : low;
        PageView &other = (h & 0x80000000u) ? low : high;
        size_t idx = owner.Find(h, key);
        CHECK(idx != PageView::kNpos);
        CHECK(owner.ValueAt(idx) == "value");
        CHECK(other.Find(h, key) == PageView::kNpos);
    }
    std::printf("page view fill/split: ok (%zu entries, %u moved)\n",
                entries.size(),
                moved);
}

// The §4 tagged-length encoding: the record length varint's low bit is the
// inline/out-of-line indicator. Inline and large records must coexist in one
// page, survive updates in both directions, splits, and compaction.
void TestLargeRecordEncoding()
{
    constexpr uint32_t kPage = 4096;
    std::vector<uint8_t> buf(kPage);
    PageView pv(buf.data(), kPage, 0);
    pv.Init(0);

    // A large descriptor whose total_length exceeds u32 once shifted.
    constexpr uint64_t kTotal = uint64_t{300} * 1024 * 1024;
    CHECK(pv.Write(10, "inline", "small-value") ==
          PageView::WriteResult::Inserted);
    CHECK(pv.WriteLargeRef(20, "big", 777, kTotal) ==
          PageView::WriteResult::Inserted);
    CHECK(!pv.IsLargeAt(pv.Find(10, "inline")));
    size_t big_idx = pv.Find(20, "big");
    CHECK(pv.IsLargeAt(big_idx));
    CHECK(pv.LargeFirstPageIdAt(big_idx) == 777);
    CHECK(pv.LargeTotalLengthAt(big_idx) == kTotal);

    // Update large -> large (run replaced), reporting old total_length.
    uint64_t old_len = 0;
    CHECK(pv.WriteLargeRef(20, "big", 900, kTotal + 5, &old_len) ==
          PageView::WriteResult::Updated);
    CHECK(old_len == kTotal);
    big_idx = pv.Find(20, "big");
    CHECK(pv.LargeFirstPageIdAt(big_idx) == 900);

    // Update large -> inline and inline -> large.
    CHECK(pv.Write(20, "big", "now-inline", &old_len) ==
          PageView::WriteResult::Updated);
    CHECK(old_len == kTotal + 5);
    CHECK(!pv.IsLargeAt(pv.Find(20, "big")));
    CHECK(pv.ValueAt(pv.Find(20, "big")) == "now-inline");
    CHECK(pv.WriteLargeRef(10, "inline", 5, 1u << 20, &old_len) ==
          PageView::WriteResult::Updated);
    CHECK(old_len == 11);  // "small-value"
    CHECK(pv.IsLargeAt(pv.Find(10, "inline")));

    // Split moves raw bytes: descriptors survive unchanged. Hash 10 and 20
    // both have the top bit clear; add a large record in the high half.
    CHECK(pv.WriteLargeRef(0x90000000u, "high-big", 42, 12345) ==
          PageView::WriteResult::Inserted);
    std::vector<uint8_t> high_buf(kPage);
    PageView high(high_buf.data(), kPage, 0);
    high.Init(0);
    pv.SplitInto(high, 0);
    size_t idx = high.Find(0x90000000u, "high-big");
    CHECK(idx != PageView::kNpos);
    CHECK(high.IsLargeAt(idx));
    CHECK(high.LargeFirstPageIdAt(idx) == 42);
    CHECK(high.LargeTotalLengthAt(idx) == 12345);
    CHECK(pv.IsLargeAt(pv.Find(10, "inline")));

    // Compaction preserves both kinds.
    pv.Compact();
    CHECK(pv.LargeTotalLengthAt(pv.Find(10, "inline")) == 1u << 20);
    CHECK(pv.ValueAt(pv.Find(20, "big")) == "now-inline");
    std::printf("large record encoding: ok\n");
}

void TestFreeRanges()
{
    FreeRanges fr;
    // Coalescing left, right, both.
    fr.Insert({10, 5});
    fr.Insert({20, 5});
    CHECK(fr.Ranges().size() == 2);
    fr.Insert({15, 5});  // bridges both
    CHECK(fr.Ranges().size() == 1);
    CHECK(fr.Ranges()[0].first_ == 10 && fr.Ranges()[0].count_ == 15);
    CHECK(fr.CheckCanonical());
    // Prefix allocation shrinks, never splits.
    CHECK(fr.Allocate(4) == 10);
    CHECK(fr.Ranges().size() == 1);
    CHECK(fr.Ranges()[0].first_ == 14 && fr.Ranges()[0].count_ == 11);
    // Allocation larger than any range fails over to the caller.
    CHECK(fr.Allocate(12) == kInvalidPageId);
    CHECK(fr.Allocate(11) == 14);
    CHECK(fr.Ranges().empty());

    // Property test against a reference set.
    Rng rng(42);
    FreeRanges set;
    std::vector<bool> free_ids(2048, false);
    for (int step = 0; step < 4000; ++step)
    {
        if (rng.Below(2) == 0)
        {
            // Free a random small run of currently-unfree ids.
            uint32_t start = static_cast<uint32_t>(rng.Below(2000));
            uint32_t len = 1 + static_cast<uint32_t>(rng.Below(8));
            bool ok = true;
            for (uint32_t k = start; k < start + len; ++k)
            {
                if (free_ids[k])
                {
                    ok = false;
                    break;
                }
            }
            if (ok)
            {
                set.Insert({start, len});
                for (uint32_t k = start; k < start + len; ++k)
                {
                    free_ids[k] = true;
                }
            }
        }
        else
        {
            uint32_t len = 1 + static_cast<uint32_t>(rng.Below(6));
            PageId got = set.Allocate(len);
            if (got != kInvalidPageId)
            {
                for (uint32_t k = got; k < got + len; ++k)
                {
                    CHECK(free_ids[k]);
                    free_ids[k] = false;
                }
            }
        }
        CHECK(set.CheckCanonical());
    }
    // The canonical set and the reference bitmap agree.
    size_t bitmap_free = 0;
    for (bool b : free_ids)
    {
        bitmap_free += b ? 1 : 0;
    }
    size_t set_free = 0;
    for (const auto &r : set.Ranges())
    {
        set_free += r.count_;
    }
    CHECK(bitmap_free == set_free);
    std::printf("free ranges: ok\n");
}

void TestPendingDeletes()
{
    PendingDeletes pd;
    pd.Append({100, 3}, 50);
    pd.Append({200, 1}, 50);
    pd.Append({300, 2}, 75);
    CHECK(pd.Entries().size() == 3);  // equal ts never coalesces ranges
    auto drained = pd.DrainUpTo(50);
    CHECK(drained.size() == 2);
    CHECK(drained[0].range_.first_ == 100 && drained[1].range_.first_ == 200);
    CHECK(pd.Entries().size() == 1);
    CHECK(pd.DrainUpTo(60).empty());
    drained = pd.DrainUpTo(75);
    CHECK(drained.size() == 1 && drained[0].range_.first_ == 300);
    CHECK(pd.Entries().empty());
    std::printf("pending deletes: ok\n");
}

// Mirror-map property test: RedisPagedHashObject vs std::unordered_map under a
// random put/update/del workload with a page size small enough to force many
// splits and directory doublings.
void TestCoreMirror()
{
    constexpr uint32_t kPage = 512;
    RedisPagedHashObject core(kPage, &TestFieldHash);
    std::unordered_map<std::string, std::string> mirror;
    Rng rng(7);
    uint64_t logical = 0;
    for (int step = 0; step < 30000; ++step)
    {
        std::string field = "f" + std::to_string(rng.Below(4000));
        uint64_t op = rng.Below(10);
        if (op < 7)
        {
            std::string value = "v" + std::to_string(rng.Next() % 100000) +
                                std::string(rng.Below(40), 'x');
            bool inserted = core.Put(field, value);
            auto [it, is_new] = mirror.try_emplace(field, value);
            if (!is_new)
            {
                CHECK(!inserted);
                logical += value.size();
                logical -= it->second.size();
                it->second = value;
            }
            else
            {
                CHECK(inserted);
                logical += field.size() + value.size();
            }
        }
        else
        {
            bool deleted = core.Del(field);
            auto it = mirror.find(field);
            if (it != mirror.end())
            {
                CHECK(deleted);
                logical -= field.size() + it->second.size();
                mirror.erase(it);
            }
            else
            {
                CHECK(!deleted);
            }
        }
    }
    CHECK(core.FieldCount() == mirror.size());
    CHECK(core.LogicalBytes() == logical);
    for (const auto &[k, v] : mirror)
    {
        auto got = core.Get(k);
        CHECK(got.has_value());
        CHECK(*got == v);
    }
    CHECK(!core.Get("not-a-field").has_value());
    CHECK(core.CheckInvariants());
    CHECK(core.Meta().global_depth_ > 0);  // splits actually happened
    std::printf("core mirror: ok (fields=%llu, depth=%u, pages~%zu)\n",
                static_cast<unsigned long long>(core.FieldCount()),
                core.Meta().global_depth_,
                core.Meta().page_entry_counts_.size());
}

/**
 * The long mirror test above is good at finding aggregate drift, but checking
 * only its final state misses bugs that are later repaired by another
 * mutation.  This state machine validates every cut point and repeatedly
 * crosses the most delicate lifecycle boundary: the last field is deleted,
 * the empty metadata is serialized, and the same object is populated again.
 * It also reloads through metadata-only form during active workloads, then
 * installs the durable page images exactly as the engine's fault path does.
 */
void TestLifecycleStateMachine()
{
    constexpr uint32_t kPage = 256;
    Rng rng(0xE10C1FECULL);

    auto logical_bytes = [](const auto &model)
    {
        uint64_t n = 0;
        for (const auto &[field, value] : model)
        {
            n += field.size() + value.size();
        }
        return n;
    };

    auto verify = [&](const RedisPagedHashObject &obj, const auto &model)
    {
        CHECK(obj.CheckInvariants());
        CHECK(obj.FieldCount() == model.size());
        CHECK(obj.LogicalBytes() == logical_bytes(model));
        for (const auto &[field, value] : model)
        {
            std::optional<std::string_view> got = obj.Get(field);
            CHECK(got.has_value());
            CHECK(*got == value);
        }
        CHECK(!obj.Get("definitely-absent\0field").has_value());
    };

    auto reload = [&](const RedisPagedHashObject &src)
    {
        std::string metadata;
        std::string metadata_row;
        std::vector<std::pair<PageId, std::string>> pages;
        src.SerializeAll(metadata_row, pages);
        src.SerializeMeta(metadata);

        RedisPagedHashObject dst(&TestFieldHash);
        size_t offset = 0;
        CHECK(dst.DeserializeSections(metadata.data(), metadata.size(), offset));
        CHECK(offset == metadata.size());
        CHECK(dst.ResidentPageCount() == 0);
        for (const auto &[id, image] : pages)
        {
            CHECK(dst.InstallPage(id, image, 100));
        }
        CHECK(dst.IsFullyResident());
        return dst;
    };

    RedisPagedHashObject obj(kPage, &TestFieldHash);
    std::unordered_map<std::string, std::string> model;
    for (int generation = 0; generation < 24; ++generation)
    {
        // Include protocol-interesting byte strings in every generation.
        std::vector<std::string> special_fields = {
            "", std::string("nul\0field", 9), std::string("\xFF\x80", 2)};
        for (size_t i = 0; i < special_fields.size(); ++i)
        {
            std::string value = i == 0 ? "" : std::string(i * 7, '\0');
            bool inserted = obj.Put(special_fields[i], value);
            auto [it, fresh] = model.insert_or_assign(special_fields[i], value);
            (void) it;
            CHECK(inserted == fresh);
        }

        for (int step = 0; step < 600; ++step)
        {
            std::string field = "g" + std::to_string(generation) + ":f" +
                                std::to_string(rng.Below(180));
            if (rng.Below(5) == 0)
            {
                bool removed = obj.Del(field);
                CHECK(removed == (model.erase(field) != 0));
            }
            else
            {
                size_t len = rng.Below(72);
                std::string value(len, static_cast<char>(rng.Next() & 0xFF));
                if (len > 2)
                {
                    value[len / 2] = '\0';
                }
                bool inserted = obj.Put(field, value);
                auto [it, fresh] = model.insert_or_assign(field, value);
                (void) it;
                CHECK(inserted == fresh);
            }

            if (step % 19 == 0)
            {
                verify(obj, model);
            }
            if (step != 0 && step % 113 == 0)
            {
                obj.Copy(reload(obj));
                verify(obj, model);
            }
        }

        // Delete every field in a changing order. The object must remain a
        // valid empty core even though the key-level engine will normally
        // discard this payload after ModifiedToEmpty.
        std::vector<std::string> fields;
        fields.reserve(model.size());
        for (const auto &[field, value] : model)
        {
            (void) value;
            fields.push_back(field);
        }
        for (size_t i = fields.size(); i > 1; --i)
        {
            std::swap(fields[i - 1], fields[rng.Below(i)]);
        }
        for (const std::string &field : fields)
        {
            CHECK(obj.Del(field));
            CHECK(model.erase(field) == 1);
        }
        verify(obj, model);
        CHECK(obj.FieldCount() == 0);
        CHECK(obj.LogicalBytes() == 0);

        // Persist and reload the empty state before starting the same key's
        // next incarnation. This catches stale directory/count/page-id state
        // surviving a last-field deletion.
        obj.Copy(reload(obj));
        verify(obj, model);
        std::string first = "restart:" + std::to_string(generation);
        CHECK(obj.Put(first, "first"));
        model.emplace(first, "first");
        verify(obj, model);
        CHECK(obj.Del(first));
        model.clear();
    }

    verify(obj, model);
    std::printf("lifecycle state machine: ok (24 empty/recreate cycles)\n");
}

// Layout determinism (§10 / §16): the same trace applied twice yields
// byte-identical metadata and pages.
void TestLayoutDeterminism()
{
    constexpr uint32_t kPage = 512;
    auto run_trace = [](std::string &meta_out,
                        std::vector<std::pair<PageId, std::string>> &pages)
    {
        RedisPagedHashObject core(kPage, &TestFieldHash);
        Rng rng(99);
        for (int step = 0; step < 8000; ++step)
        {
            std::string field = "field" + std::to_string(rng.Below(1500));
            if (rng.Below(5) == 0)
            {
                core.Del(field);
            }
            else
            {
                core.Put(field, "value" + std::to_string(rng.Next() % 1000));
            }
        }
        core.SerializeAll(meta_out, pages);
    };
    std::string meta_a;
    std::string meta_b;
    std::vector<std::pair<PageId, std::string>> pages_a;
    std::vector<std::pair<PageId, std::string>> pages_b;
    run_trace(meta_a, pages_a);
    run_trace(meta_b, pages_b);
    CHECK(meta_a == meta_b);
    CHECK(pages_a.size() == pages_b.size());
    for (size_t i = 0; i < pages_a.size(); ++i)
    {
        CHECK(pages_a[i].first == pages_b[i].first);
        CHECK(pages_a[i].second == pages_b[i].second);
    }
    std::printf("layout determinism: ok (%zu pages)\n", pages_a.size());
}

// Metadata codec: full-row round trip is identity, and the reloaded free
// list matches the §4 derived definition. The row is sectioned (§5):
// [version][page-manager section][type section]; the PM section is exercised
// through the page manager's own id lifecycle, the type section through
// large runs.
void TestMetadataCodec()
{
    constexpr uint32_t kPage = 512;
    RedisPagedHashObject core(kPage, &TestFieldHash);
    Rng rng(123);
    for (int i = 0; i < 5000; ++i)
    {
        core.Put("k" + std::to_string(rng.Below(1200)),
                 "v" + std::to_string(rng.Next() % 977));
    }
    // Exercise the persisted large-run arm (type section) and the pending-
    // delete arm (page-manager section) directly.
    PagedHashMetadata &meta = core.MutableMeta();
    txservice::PageFrameTable &frames = core.MutableFrames();
    PageId run_a = frames.AllocatePageId();
    PageId run_b = frames.AllocatePageId();
    LargeRun run;
    run.page_ids_ = {run_a, run_b};
    run.total_length_ = 200000;
    meta.large_runs_.push_back(run);
    // Three freed ids awaiting their store-row Delete (PM section).
    PageId pend = frames.AllocatePageId();
    PageId pend2 = frames.AllocatePageId();
    PageId pend3 = frames.AllocatePageId();
    frames.FreePage(pend);
    frames.FreePage(pend2);
    frames.FreePage(pend3);
    std::string bytes_a;
    core.SerializeMeta(bytes_a);
    RedisPagedHashObject reloaded(&TestFieldHash);
    size_t offset = 0;
    CHECK(reloaded.DeserializeSections(bytes_a.data(), bytes_a.size(), offset));
    CHECK(offset == bytes_a.size());
    std::string bytes_b;
    reloaded.SerializeMeta(bytes_b);
    CHECK(bytes_a == bytes_b);
    // Size accounting must stay exact with large runs present.
    CHECK(bytes_a.size() == core.MetaSerializedSize());
    CHECK(reloaded.FieldCount() == core.FieldCount());
    CHECK(reloaded.LogicalBytes() == core.LogicalBytes());
    CHECK(reloaded.Meta().dir_ == core.Meta().dir_);
    CHECK(reloaded.Frames().PageSize() == kPage);
    // Reloaded pending deletes carry ts 0 (drainable by the next flush, §5),
    // and the id partition holds: pending ids are not live, run ids are.
    CHECK(reloaded.Frames().PendingDeleteEntries().size() == 3);
    CHECK(reloaded.Frames().PendingDeleteEntries()[0].freed_ts_ == 0);
    CHECK(!reloaded.IsPageLive(pend));
    CHECK(reloaded.IsPageLive(run_a));
    CHECK(reloaded.IsPageLive(run_b));
    std::printf("metadata codec: ok (%zu bytes)\n", bytes_a.size());
}

// Truncated or corrupt metadata must fail deserialization, never crash.
void TestMetadataCodecMalformed()
{
    RedisPagedHashObject core(512, &TestFieldHash);
    core.Put("a", "b");
    std::string bytes;
    core.Meta().Serialize(bytes);
    for (size_t cut = 0; cut < bytes.size(); ++cut)
    {
        PagedHashMetadata m;
        size_t offset = 0;
        CHECK(!m.Deserialize(bytes.data(), cut, offset));
    }
    // Bad format version in the row body (the version byte leads the
    // sectioned body, ahead of the page-manager section).
    std::string body;
    core.SerializeMeta(body);
    for (size_t cut = 0; cut < body.size(); ++cut)
    {
        RedisPagedHashObject shell(&TestFieldHash);
        size_t off = 0;
        CHECK(!shell.DeserializeSections(body.data(), cut, off));
    }
    std::string bad = body;
    bad[0] = 99;
    RedisPagedHashObject shell(&TestFieldHash);
    size_t offset = 0;
    CHECK(!shell.DeserializeSections(bad.data(), bad.size(), offset));

    // The same truncation sweep over a TYPE section that carries a large
    // run, so the run-parsing arms see every cut.
    RedisPagedHashObject runner(512, &TestFieldHash);
    runner.Put("a", "b");
    LargeRun lr;
    lr.page_ids_ = {runner.MutableFrames().AllocatePageId(),
                    runner.MutableFrames().AllocatePageId()};
    lr.total_length_ = 4096;
    runner.MutableMeta().large_runs_.push_back(lr);
    std::string rbytes;
    runner.Meta().Serialize(rbytes);
    for (size_t cut = 0; cut < rbytes.size(); ++cut)
    {
        PagedHashMetadata m2;
        size_t o2 = 0;
        CHECK(!m2.Deserialize(rbytes.data(), cut, o2));
    }
    {
        PagedHashMetadata m2;
        size_t o2 = 0;
        CHECK(m2.Deserialize(rbytes.data(), rbytes.size(), o2));
        CHECK(m2.large_runs_.size() == 1);
    }

    // Crafted page-manager sections: pending entries that truncation cannot
    // produce — zero count, out of order, past the high-water.
    namespace pd = txservice::paged_detail;
    auto craft =
        [](uint32_t next_id, std::vector<std::pair<uint32_t, uint32_t>> entries)
    {
        std::string body;
        pd::AppendVarint(body, entries.size());
        std::string pending;
        uint8_t u32[4];
        for (auto &[first, count] : entries)
        {
            pd::StoreU32(u32, first);
            pending.append(reinterpret_cast<char *>(u32), 4);
            pd::AppendVarint(pending, count);
        }
        std::string inner;
        pd::StoreU32(u32, 512);  // page_size
        inner.append(reinterpret_cast<char *>(u32), 4);
        pd::StoreU32(u32, next_id);
        inner.append(reinterpret_cast<char *>(u32), 4);
        inner += body + pending;
        std::string out;
        pd::AppendVarint(out, inner.size());
        return out + inner;
    };
    auto reject = [&](const std::string &section)
    {
        txservice::PageFrameTable t;
        size_t o = 0;
        CHECK(!t.DeserializeMeta(section.data(), section.size(), o));
    };
    reject(craft(10, {{2, 0}}));          // zero-count range
    reject(craft(10, {{5, 2}, {3, 1}}));  // out of order
    reject(craft(10, {{8, 5}}));          // past the high-water
    {
        // And the well-formed control: parses and reports the entry.
        txservice::PageFrameTable t;
        std::string good = craft(10, {{2, 3}});
        size_t o = 0;
        CHECK(t.DeserializeMeta(good.data(), good.size(), o));
        CHECK(o == good.size());
        CHECK(t.PendingDeleteEntries().size() == 1);
    }
    {
        // A large run whose declared id_count exceeds the bytes that remain:
        // must be refused before resize(), not allocated.
        RedisPagedHashObject runner2(512, &TestFieldHash);
        runner2.Put("a", "b");
        LargeRun lr2;
        lr2.page_ids_ = {runner2.MutableFrames().AllocatePageId()};
        lr2.total_length_ = 8;
        runner2.MutableMeta().large_runs_.push_back(lr2);
        std::string rb;
        runner2.Meta().Serialize(rb);
        // Find the run section: it follows the counts, and the run count is
        // the last u32 before it. Corrupt the id_count of the first run by
        // scanning for the encoded run count of 1 near the end.
        CHECK(rb.size() > 16);
        // id_count is the u32 right after the run-count u32 at the tail:
        // [... run_count=1][id_count=1][page id][total_length]
        size_t idc = rb.size() - 4 - 4 - 8;
        std::string bad_run = rb;
        pd::StoreU32(reinterpret_cast<uint8_t *>(bad_run.data()) + idc,
                     0xFFFFFFFFu);
        PagedHashMetadata m3;
        size_t o3 = 0;
        CHECK(!m3.Deserialize(bad_run.data(), bad_run.size(), o3));
    }
    {
        // A section whose declared body is shorter than the fixed fields.
        std::string tiny;
        pd::AppendVarint(tiny, 2);
        tiny.append(2, '\0');
        txservice::PageFrameTable t;
        size_t o = 0;
        CHECK(!t.DeserializeMeta(tiny.data(), tiny.size(), o));
    }
    {
        // A pending-count varint that runs off the section's end.
        std::string body(8, '\0');  // page_size + next_page_id
        body.append(2, '\x80');     // unterminated varint to bend
        std::string sec;
        pd::AppendVarint(sec, body.size());
        sec += body;
        txservice::PageFrameTable t;
        size_t o = 0;
        CHECK(!t.DeserializeMeta(sec.data(), sec.size(), o));
    }
    {
        // The pending-delete list as a standalone vocabulary type: append,
        // ts-scoped prefix drain, containment.
        txservice::PendingDeletes pds;
        pds.Append({4, 2}, 10);
        pds.Append({9, 1}, 20);
        CHECK(pds.Contains(5) && !pds.Contains(6));
        auto drained = pds.DrainUpTo(15);
        CHECK(drained.size() == 1 && drained[0].range_.first_ == 4);
        CHECK(pds.Entries().size() == 1);
    }
    std::printf("metadata codec malformed: ok\n");
}
// SerializedSize() sizes the engine's flush buffer, so it must agree with
// Serialize() to the byte across every shape the directory can take.
void TestMetaSerializedSizeExact()
{
    Rng rng(0x512E);
    for (int trial = 0; trial < 40; ++trial)
    {
        RedisPagedHashObject core(256, &TestFieldHash);
        size_t n = rng.Below(300);
        for (size_t i = 0; i < n; ++i)
        {
            std::string k = "k" + std::to_string(rng.Below(500));
            core.Put(k, std::string(rng.Below(20) + 1, 'v'));
        }
        std::string bytes;
        core.SerializeMeta(bytes);
        CHECK(bytes.size() == core.MetaSerializedSize());
    }
    std::printf("metadata SerializedSize exact: ok\n");
}

// A deserialized core is metadata-only: directory and counts intact, no page
// resident, free list rebuilt (docs/08 §5).
void TestMetadataOnlyLoad()
{
    RedisPagedHashObject core(256, &TestFieldHash);
    for (int i = 0; i < 200; ++i)
    {
        core.Put("field" + std::to_string(i), "value" + std::to_string(i));
    }
    std::string bytes;
    core.SerializeMeta(bytes);

    RedisPagedHashObject loaded(&TestFieldHash);
    size_t offset = 0;
    CHECK(loaded.DeserializeSections(bytes.data(), bytes.size(), offset));
    CHECK(offset == bytes.size());
    CHECK(loaded.ResidentPageCount() == 0);
    CHECK(loaded.ResidentBytes() == 0);
    CHECK(loaded.FieldCount() == core.FieldCount());
    CHECK(loaded.LogicalBytes() == core.LogicalBytes());
    CHECK(loaded.Meta().dir_ == core.Meta().dir_);
    CHECK(loaded.Frames().PageSize() == core.Frames().PageSize());
    for (PageId id : loaded.Meta().dir_)
    {
        CHECK(!loaded.Frames().IsResident(id));
    }
    // Re-serializing the loaded metadata reproduces the bytes exactly.
    std::string again;
    loaded.SerializeMeta(again);
    CHECK(again == bytes);
    // The unbounded path (TxRecord::Deserialize supplies no length) must
    // parse identically: SIZE_MAX is a sentinel, not an end pointer.
    RedisPagedHashObject unbounded(&TestFieldHash);
    size_t uoff = 0;
    CHECK(unbounded.DeserializeSections(bytes.data(), SIZE_MAX, uoff));
    CHECK(uoff == bytes.size());
    CHECK(unbounded.FieldCount() == core.FieldCount());
    std::printf("metadata-only load: ok\n");
}

// Clone must not alias pages: mutating the copy leaves the original intact.
void TestDeepCopyIndependence()
{
    RedisPagedHashObject core(256, &TestFieldHash);
    for (int i = 0; i < 150; ++i)
    {
        core.Put("k" + std::to_string(i), "v" + std::to_string(i));
    }
    std::string before_meta;
    std::vector<std::pair<PageId, std::string>> before_pages;
    core.SerializeAll(before_meta, before_pages);

    RedisPagedHashObject copy(core);
    for (int i = 0; i < 150; ++i)
    {
        copy.Put("k" + std::to_string(i), "MUTATED" + std::to_string(i));
    }
    copy.Put("brand-new", "x");

    std::string after_meta;
    std::vector<std::pair<PageId, std::string>> after_pages;
    core.SerializeAll(after_meta, after_pages);
    CHECK(after_meta == before_meta);
    CHECK(after_pages == before_pages);
    CHECK(core.CheckInvariants());
    CHECK(copy.CheckInvariants());
    CHECK(copy.FieldCount() == core.FieldCount() + 1);
    std::printf("deep copy independence: ok\n");
}
// The one guarantee SCAN makes (docs/08 §12): every element present for the
// whole scan is returned at least once. Duplicates are allowed; a SKIP is a
// correctness bug. This drives the cursor primitives directly — reverse-binary
// bucket order plus a last-returned hash32 — including across a delete that
// shifts slots down, which is exactly what a positional cursor would skip on.
// The §4 forced-split valve: a page reaching 65535 entries reports NeedSplit
// even when bytes remain. Only reachable on pages above ~640 KB, so it is
// exercised directly on a PageView over a 1 MB buffer, with ascending hashes
// so every insert appends.
void TestCountValveSplit()
{
    constexpr uint32_t kBig = 1u << 20;
    std::vector<uint8_t> buf(kBig);
    PageView pv(buf.data(), kBig, 0);
    pv.Init(0);
    for (uint32_t i = 0; i < kMaxEntriesPerPage; ++i)
    {
        char k[8];
        std::snprintf(k, sizeof(k), "%06x", i);
        CHECK(pv.Write(i + 1, std::string_view(k, 6), "") ==
              PageView::WriteResult::Inserted);
    }
    CHECK(pv.EntryCount() == kMaxEntriesPerPage);
    CHECK(pv.Write(kMaxEntriesPerPage + 1, "over", "") ==
          PageView::WriteResult::NeedSplit);
    std::printf("count-valve split: ok\n");
}

void TestScanGuarantee()
{
    RedisPagedHashObject core(512, &TestFieldHash);
    std::vector<std::string> stable;
    for (int i = 0; i < 300; ++i)
    {
        std::string f = "f" + std::to_string(i);
        core.Put(f, "v" + std::to_string(i));
        stable.push_back(f);
    }

    auto full_scan = [&](bool delete_midway)
    {
        std::set<std::string> seen;
        uint32_t cursor = 0;
        int steps = 0;
        bool deleted = false;
        while (true)
        {
            uint32_t next = 0;
            bool ok = core.ScanStep(cursor,
                                    4,
                                    &next,
                                    [&](std::string_view f, std::string_view)
                                    { seen.insert(std::string(f)); });
            CHECK(ok);  // everything resident in this test
            if (delete_midway && !deleted && ++steps == 3)
            {
                // Remove a field that has already been emitted, shifting the
                // slots after it down by one.
                core.Del("f0");
                deleted = true;
            }
            if (next == 0)
            {
                break;
            }
            // The cursor is an ascending hash bound; going backwards would be
            // a revisit loop.
            CHECK(next > cursor);
            cursor = next;
        }
        return seen;
    };

    std::set<std::string> plain = full_scan(false);
    for (const std::string &f : stable)
    {
        CHECK(plain.count(f) == 1);
    }

    // Rebuild and scan again with a concurrent delete: every field that
    // survives the whole scan must still be returned.
    RedisPagedHashObject core2(512, &TestFieldHash);
    for (int i = 0; i < 300; ++i)
    {
        core2.Put("f" + std::to_string(i), "v" + std::to_string(i));
    }
    core.Copy(core2);
    std::set<std::string> during = full_scan(true);
    for (const std::string &f : stable)
    {
        if (f == "f0")
        {
            continue;  // deleted mid-scan; either outcome is legal
        }
        CHECK(during.count(f) == 1);
    }

    std::printf("scan guarantee: ok (%zu fields)\n", plain.size());
}

// The same guarantee across a DIRECTORY DOUBLING mid-scan -- the case the
// cursor design exists for (docs/08 §12). Every field present before the scan
// started and never deleted must be returned at least once, no matter how many
// splits and doublings the concurrent inserts cause.
void TestScanDoublingMidScan()
{
    RedisPagedHashObject core(512, &TestFieldHash);
    std::vector<std::string> stable;
    for (int i = 0; i < 200; ++i)
    {
        std::string f = "f" + std::to_string(i);
        core.Put(f, "v" + std::to_string(i));
        stable.push_back(f);
    }
    uint8_t depth_before = core.Meta().global_depth_;
    CHECK(depth_before > 0);

    std::set<std::string> seen;
    uint32_t cursor = 0;
    int steps = 0;
    bool doubled = false;
    while (true)
    {
        uint32_t next = 0;
        bool ok = core.ScanStep(cursor,
                                4,
                                &next,
                                [&](std::string_view f, std::string_view)
                                { seen.insert(std::string(f)); });
        CHECK(ok);
        // The doubling lands mid-scan, once the cursor is well inside the key
        // space. The extra inserts keep splitting pages until most directory
        // entries are exclusive: a stale-cursor skip only loses data once the
        // skipped range's page is no longer shared with a visited one, which
        // is exactly how the reverse-binary walk this replaced passed its own
        // test while covering only 3328 of 4096 entries.
        if (!doubled && ++steps == 8)
        {
            int g = 0;
            while (core.Meta().global_depth_ == depth_before)
            {
                std::string f = "g" + std::to_string(g++);
                core.Put(f, std::string(64, 'g'));
                CHECK(g < 100000);
            }
            for (int extra = 0; extra < 3000; ++extra)
            {
                core.Put("h" + std::to_string(extra), std::string(64, 'h'));
            }
            doubled = true;
        }
        if (next == 0)
        {
            break;
        }
        CHECK(next > cursor);
        cursor = next;
    }
    CHECK(doubled);
    std::printf("  depth %u -> %u across the scan\n",
                unsigned(depth_before),
                unsigned(core.Meta().global_depth_));

    size_t missing = 0;
    for (const std::string &f : stable)
    {
        if (seen.count(f) == 0)
        {
            if (++missing <= 5)
            {
                std::printf("  SKIPPED: %s\n", f.c_str());
            }
        }
    }
    if (missing != 0)
    {
        std::printf("  %zu of %zu stable fields skipped across the doubling\n",
                    missing,
                    stable.size());
    }
    CHECK(missing == 0);
    std::printf("scan across doubling: ok (%zu stable fields)\n",
                stable.size());
}
}  // namespace

int main()
{
    TestVarint();
    TestPageKeyCodec();
    TestPageViewBasics();
    TestPageViewFillAndSplit();
    TestLargeRecordEncoding();
    TestFreeRanges();
    TestPendingDeletes();
    TestCoreMirror();
    TestLifecycleStateMachine();
    TestLayoutDeterminism();
    TestMetadataCodec();
    TestMetadataCodecMalformed();
    TestMetaSerializedSizeExact();
    TestMetadataOnlyLoad();
    TestDeepCopyIndependence();
    TestCountValveSplit();
    TestScanGuarantee();
    TestScanDoublingMidScan();
    std::printf("all paged-hash core tests passed\n");
    return 0;
}
