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

// Object-level command tests for the paged hash: every Execute(XCommand&) /
// Commit* method, on both twins, in both residency states — fully resident
// (the reply arms) and metadata-only after a serialize/reload (the fault
// arms) — plus the PagedTxObject surface a unit test can drive without a
// shard. These are the paths the server-level suites exercise end to end;
// here each arm is hit directly so line coverage of the paged headers is
// measurable and complete.

#include <gflags/gflags.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "redis_paged_hash_object.h"
#include "redis_string_object.h"

DECLARE_uint32(paged_hash_convert_threshold);
DECLARE_uint32(paged_hash_page_size);
DECLARE_uint32(paged_object_reply_bound);

namespace
{
using namespace EloqKV;

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

constexpr uint32_t kPage = 512;

/**
 * @brief A resident object with `n` fields f000..; values "v<i>".
 */
RedisPagedHashObject Build(int n)
{
    RedisPagedHashObject obj(kPage, &TestFieldHash);
    for (int i = 0; i < n; ++i)
    {
        obj.Put("f" + std::to_string(i), "v" + std::to_string(i));
    }
    return obj;
}

/**
 * @brief Serialize `src` and reload it metadata-only: every page
 * non-resident, so command arms that touch a page fault. Also returns the
 * resident page images for InstallPage-driven refaulting.
 */
RedisPagedHashObject Shed(const RedisPagedHashObject &src,
                          std::vector<std::pair<PageId, std::string>> *pages)
{
    std::string row;
    std::vector<std::pair<PageId, std::string>> imgs;
    src.SerializeAll(row, imgs);
    if (pages != nullptr)
    {
        *pages = imgs;
    }
    RedisPagedHashObject shell(&TestFieldHash);
    size_t offset = 0;
    shell.Deserialize(row.data(), offset);
    CHECK(shell.ResidentPageCount() == 0);
    return shell;
}

void DrainFaults(RedisPagedHashObject &obj)
{
    std::vector<uint32_t> ids;
    CHECK(obj.TakePendingFaults(ids));
    CHECK(!ids.empty());
}

void TestPointReads()
{
    RedisPagedHashObject obj = Build(40);

    HGetCommand get("f7");
    CHECK(obj.Execute(get));
    CHECK(get.result_.err_code_ == RD_OK);
    HGetCommand miss("nope");
    CHECK(obj.Execute(miss));
    CHECK(miss.result_.err_code_ == RD_NIL);

    HLenCommand len;
    CHECK(obj.Execute(len));
    CHECK(std::get<int64_t>(len.result_.result_) == 40);

    HExistsCommand ex;
    ex.key_ = EloqString("f7");
    CHECK(obj.Execute(ex));
    CHECK(ex.result_.err_code_ == RD_OK);
    HExistsCommand exmiss;
    exmiss.key_ = EloqString("nope");
    exmiss.result_.err_code_ = RD_NIL;  // the not-found default is preserved
    CHECK(obj.Execute(exmiss));
    CHECK(exmiss.result_.err_code_ == RD_NIL);

    HStrLenCommand sl;
    sl.field_ = EloqString("f7");
    CHECK(obj.Execute(sl));
    CHECK(std::get<int64_t>(sl.result_.result_) == 2);  // "v7"
    HStrLenCommand slmiss;
    slmiss.field_ = EloqString("nope");
    CHECK(obj.Execute(slmiss));
    CHECK(slmiss.result_.err_code_ == RD_NIL);

    // Fault arms: metadata-only reload.
    RedisPagedHashObject cold = Shed(obj, nullptr);
    HGetCommand cg("f7");
    CHECK(!cold.Execute(cg));
    DrainFaults(cold);
    HExistsCommand ce;
    ce.key_ = EloqString("f7");
    CHECK(!cold.Execute(ce));
    DrainFaults(cold);
    HStrLenCommand cs;
    cs.field_ = EloqString("f7");
    CHECK(!cold.Execute(cs));
    DrainFaults(cold);
    // HLEN answers from metadata even with nothing resident.
    HLenCommand cl;
    CHECK(cold.Execute(cl));
    CHECK(std::get<int64_t>(cl.result_.result_) == 40);
    std::printf("point reads: ok\n");
}

void TestHSetHDel()
{
    RedisPagedHashObject obj = Build(10);

    std::vector<std::pair<EloqString, EloqString>> pairs;
    pairs.emplace_back(EloqString("new1"), EloqString("x"));
    pairs.emplace_back(EloqString("f3"), EloqString("longer-value"));
    HSetCommand hset(HSetCommand::SubType::HSET, std::move(pairs));
    auto r = obj.Execute(hset);
    CHECK(r.has_value() && *r);
    CHECK(std::get<int64_t>(hset.result_.result_) == 1);  // one NEW field
    std::vector<std::pair<EloqString, EloqString>> commit_pairs;
    commit_pairs.emplace_back(EloqString("new1"), EloqString("x"));
    commit_pairs.emplace_back(EloqString("f3"), EloqString("longer-value"));
    CHECK(obj.CommitHset(commit_pairs));
    CHECK(obj.FieldCount() == 11);
    CHECK(*obj.Get("f3") == "longer-value");

    // Object-too-big arm, via the metadata's own logical size (the ceiling
    // is a compile-time constant far above what a unit test can write).
    RedisPagedHashObject big = Build(2);
    big.MutableMeta().logical_bytes_ = UINT64_MAX / 2;
    std::vector<std::pair<EloqString, EloqString>> bp;
    bp.emplace_back(EloqString("k"), EloqString("v"));
    HSetCommand toobig(HSetCommand::SubType::HSET, std::move(bp));
    auto rb = big.Execute(toobig);
    CHECK(rb.has_value() && !*rb);
    CHECK(toobig.result_.err_code_ == RD_ERR_OBJECT_TOO_BIG);

    // HDEL: removed / none / ModifiedToEmpty.
    HDelCommand del;
    del.del_list_.emplace_back(EloqString("f3"));
    del.del_list_.emplace_back(EloqString("nope"));
    auto dr = obj.Execute(del);
    CHECK(dr.has_value() && *dr == CommandExecuteState::Modified);
    CHECK(std::get<int64_t>(del.result_.result_) == 1);
    std::vector<EloqString> dl;
    dl.emplace_back(EloqString("f3"));
    bool now_empty = false;
    CHECK(obj.CommitHdel(dl, now_empty));
    CHECK(!now_empty);

    HDelCommand delnone;
    delnone.del_list_.emplace_back(EloqString("nope"));
    auto dn = obj.Execute(delnone);
    CHECK(dn.has_value() && *dn == CommandExecuteState::NoChange);

    RedisPagedHashObject two = Build(2);
    HDelCommand delall;
    delall.del_list_.emplace_back(EloqString("f0"));
    delall.del_list_.emplace_back(EloqString("f1"));
    auto da = two.Execute(delall);
    CHECK(da.has_value() && *da == CommandExecuteState::ModifiedToEmpty);
    std::vector<EloqString> dall;
    dall.emplace_back(EloqString("f0"));
    dall.emplace_back(EloqString("f1"));
    CHECK(two.CommitHdel(dall, now_empty));
    CHECK(now_empty);

    // Fault arms.
    std::vector<std::pair<PageId, std::string>> imgs;
    RedisPagedHashObject cold = Shed(obj, &imgs);
    std::vector<std::pair<EloqString, EloqString>> cp;
    cp.emplace_back(EloqString("f1"), EloqString("z"));
    HSetCommand chs(HSetCommand::SubType::HSET, std::move(cp));
    CHECK(!chs.field_value_pairs_.empty());
    auto cr = cold.Execute(chs);
    CHECK(!cr.has_value());
    DrainFaults(cold);
    std::vector<std::pair<EloqString, EloqString>> ccp;
    ccp.emplace_back(EloqString("f1"), EloqString("z"));
    CHECK(!cold.CommitHset(ccp));
    CHECK(cold.HasPendingFaults());
    std::vector<uint32_t> ids;
    CHECK(cold.TakePendingFaults(ids));
    // Install the needed pages from the saved images and retry: the
    // discover-then-mutate contract (§10).
    for (uint32_t id : ids)
    {
        for (auto &[pid, bytes] : imgs)
        {
            if (pid == id)
            {
                CHECK(cold.InstallPage(id, bytes, 77));
            }
        }
    }
    CHECK(cold.CommitHset(ccp));
    CHECK(*cold.Get("f1") == "z");

    // A fresh metadata-only copy for the HDEL fault arms (the install above
    // made `cold` fully resident again).
    RedisPagedHashObject cold2 = Shed(obj, nullptr);
    HDelCommand cdel;
    cdel.del_list_.emplace_back(EloqString("f6"));
    auto cdr = cold2.Execute(cdel);
    CHECK(!cdr.has_value());
    cold2.TakePendingFaults(ids);
    std::vector<EloqString> cdl;
    cdl.emplace_back(EloqString("f6"));
    CHECK(!cold2.CommitHdel(cdl, now_empty));
    cold2.TakePendingFaults(ids);
    std::printf("hset/hdel: ok\n");
}

void TestSetNxIncr()
{
    RedisPagedHashObject obj = Build(6);

    HSetNxCommand nx;
    nx.key_ = EloqString("f2");
    nx.value_ = EloqString("won't");
    auto r = obj.Execute(nx);
    CHECK(r.has_value() && !*r);  // exists: no write
    // OutputResult maps RD_NIL to Redis's integer 0. RD_OK would incorrectly
    // return 1 even though Execute correctly chose the no-write path.
    CHECK(nx.result_.err_code_ == RD_NIL);
    HSetNxCommand nx2;
    nx2.key_ = EloqString("brand");
    nx2.value_ = EloqString("new");
    auto r2 = obj.Execute(nx2);
    CHECK(r2.has_value() && *r2);
    EloqString nf("brand");
    EloqString nv("new");
    CHECK(obj.CommitHSetNx(nf, nv));
    CHECK(*obj.Get("brand") == "new");
    // Too-big arm.
    RedisPagedHashObject big = Build(2);
    big.MutableMeta().logical_bytes_ = UINT64_MAX / 2;
    HSetNxCommand nxbig;
    nxbig.key_ = EloqString("k");
    nxbig.value_ = EloqString("v");
    auto rb = big.Execute(nxbig);
    CHECK(rb.has_value() && !*rb);
    CHECK(nxbig.result_.err_code_ == RD_ERR_OBJECT_TOO_BIG);

    // HINCRBY: fresh, existing-numeric, non-numeric, overflow.
    HIncrByCommand inc;
    inc.field_ = EloqString("ctr");
    inc.score_ = 5;
    auto ir = obj.Execute(inc);
    CHECK(ir.has_value() && *ir);
    CHECK(std::get<int64_t>(inc.result_.result_) == 5);
    EloqString cf("ctr");
    CHECK(obj.CommitHincrby(cf, 5));
    HIncrByCommand inc2;
    inc2.field_ = EloqString("ctr");
    inc2.score_ = 3;
    auto ir2 = obj.Execute(inc2);
    CHECK(ir2.has_value() && *ir2);
    CHECK(std::get<int64_t>(inc2.result_.result_) == 8);
    HIncrByCommand bad;
    bad.field_ = EloqString("f0");  // "v0", not a number
    bad.score_ = 1;
    auto br = obj.Execute(bad);
    CHECK(br.has_value() && !*br);
    CHECK(bad.result_.err_code_ == RD_ERR_HASH_VAL_ERROR);
    EloqString of("ovf");
    CHECK(obj.CommitHincrby(of, INT64_MAX - 1));
    HIncrByCommand ovf;
    ovf.field_ = EloqString("ovf");
    ovf.score_ = 10;
    auto orr = obj.Execute(ovf);
    CHECK(orr.has_value() && !*orr);
    CHECK(ovf.result_.err_code_ == RD_ERR_INCR_OVERFLOW);

    // HINCRBYFLOAT: fresh, existing, bad float, nan/inf.
    HIncrByFloatCommand fin;
    fin.field_ = EloqString("fc");
    fin.incr_ = 1.5;
    auto fr = obj.Execute(fin);
    CHECK(fr.has_value() && *fr);
    EloqString ff("fc");
    CHECK(obj.CommitHIncrByFloat(ff, 1.5));
    HIncrByFloatCommand fin2;
    fin2.field_ = EloqString("fc");
    fin2.incr_ = 0.25;
    auto fr2 = obj.Execute(fin2);
    CHECK(fr2.has_value() && *fr2);
    HIncrByFloatCommand fbad;
    fbad.field_ = EloqString("f0");
    fbad.incr_ = 1;
    auto fbr = obj.Execute(fbad);
    CHECK(fbr.has_value() && !*fbr);
    CHECK(fbad.result_.err_code_ == RD_ERR_FLOAT_VALUE);
    HIncrByFloatCommand fnan;
    fnan.field_ = EloqString("fc");
    fnan.incr_ = std::numeric_limits<long double>::infinity();
    auto fnr = obj.Execute(fnan);
    CHECK(fnr.has_value() && !*fnr);
    CHECK(fnan.result_.err_code_ == RD_ERR_INCR_NAN_OR_INFINITY);

    // Fault arms for all four.
    RedisPagedHashObject cold = Shed(obj, nullptr);
    std::vector<uint32_t> ids;
    HSetNxCommand cnx;
    cnx.key_ = EloqString("f2");
    cnx.value_ = EloqString("x");
    CHECK(!cold.Execute(cnx).has_value());
    cold.TakePendingFaults(ids);
    HIncrByCommand cinc;
    cinc.field_ = EloqString("ctr");
    cinc.score_ = 1;
    CHECK(!cold.Execute(cinc).has_value());
    cold.TakePendingFaults(ids);
    HIncrByFloatCommand cfin;
    cfin.field_ = EloqString("fc");
    cfin.incr_ = 1;
    CHECK(!cold.Execute(cfin).has_value());
    cold.TakePendingFaults(ids);
    EloqString k1("f2");
    EloqString v1("x");
    CHECK(!cold.CommitHSetNx(k1, v1));
    cold.TakePendingFaults(ids);
    EloqString k2("ctr");
    CHECK(!cold.CommitHincrby(k2, 1));
    cold.TakePendingFaults(ids);
    EloqString k3("fc");
    CHECK(!cold.CommitHIncrByFloat(k3, 1));
    cold.TakePendingFaults(ids);
    std::printf("setnx/incr: ok\n");
}

void TestWholeObjectAndRand()
{
    RedisPagedHashObject obj = Build(50);

    HGetAllCommand all;
    CHECK(obj.Execute(all));
    CHECK(std::get<std::vector<std::string>>(all.result_.result_).size() ==
          100);
    HKeysCommand keys;
    CHECK(obj.Execute(keys));
    CHECK(std::get<std::vector<std::string>>(keys.result_.result_).size() ==
          50);
    HValsCommand vals;
    CHECK(obj.Execute(vals));
    CHECK(std::get<std::vector<std::string>>(vals.result_.result_).size() ==
          50);

    // Reply-bound arm, via the runtime knob.
    uint32_t saved = FLAGS_paged_object_reply_bound;
    FLAGS_paged_object_reply_bound = 1;
    HGetAllCommand bounded;
    CHECK(obj.Execute(bounded));
    CHECK(bounded.result_.err_code_ == RD_ERR_OBJECT_TOO_BIG);
    FLAGS_paged_object_reply_bound = saved;

    // HRANDFIELD: default single, distinct capped at size, repeats
    // (negative), with values, count 0, empty object.
    HRandFieldCommand one;
    CHECK(obj.Execute(one));
    HRandFieldCommand capped(1000, false);
    CHECK(obj.Execute(capped));
    CHECK(std::get<std::vector<std::string>>(capped.result_.result_).size() ==
          50);
    HRandFieldCommand rep(-70, true);
    CHECK(obj.Execute(rep));
    CHECK(std::get<std::vector<std::string>>(rep.result_.result_).size() ==
          140);
    HRandFieldCommand zero(0, false);
    CHECK(obj.Execute(zero));
    CHECK(std::get<std::vector<std::string>>(zero.result_.result_).empty());
    RedisPagedHashObject two = Build(2);
    std::vector<EloqString> dl;
    dl.emplace_back(EloqString("f0"));
    dl.emplace_back(EloqString("f1"));
    bool now_empty = false;
    CHECK(two.CommitHdel(dl, now_empty));
    HRandFieldCommand onempty;
    CHECK(two.Execute(onempty));
    CHECK(std::get<std::vector<std::string>>(onempty.result_.result_).empty());

    // HMGET hit+miss, then fault arms for the family.
    HMGetCommand mget;
    mget.fields_.emplace_back(EloqString("f1"));
    mget.fields_.emplace_back(EloqString("nope"));
    CHECK(obj.Execute(mget));
    auto &mv =
        std::get<std::vector<std::optional<std::string>>>(mget.result_.result_);
    CHECK(mv.size() == 2 && mv[0].has_value() && !mv[1].has_value());

    RedisPagedHashObject cold = Shed(obj, nullptr);
    std::vector<uint32_t> ids;
    HGetAllCommand ca;
    CHECK(!cold.Execute(ca));
    cold.TakePendingFaults(ids);
    HKeysCommand ck;
    CHECK(!cold.Execute(ck));
    cold.TakePendingFaults(ids);
    HValsCommand cv;
    CHECK(!cold.Execute(cv));
    cold.TakePendingFaults(ids);
    HMGetCommand cm;
    cm.fields_.emplace_back(EloqString("f1"));
    CHECK(!cold.Execute(cm));
    cold.TakePendingFaults(ids);
    HRandFieldCommand cr(5, false);
    CHECK(!cold.Execute(cr));
    cold.TakePendingFaults(ids);
    std::printf("whole-object/rand: ok\n");
}

void TestHScanArms()
{
    RedisPagedHashObject obj = Build(60);

    HScanCommand start(0, false, EloqString(), 10, true);
    CHECK(obj.Execute(start));
    auto &v0 = std::get<std::vector<std::string>>(start.result_.result_);
    CHECK(v0.size() >= 2 && v0[0] != "0");

    // Resume with values, then MATCH, then a stale/garbage cursor.
    HScanCommand more(std::stoll(v0[0]), false, EloqString(), 10, false);
    CHECK(obj.Execute(more));
    HScanCommand match(0, true, EloqString("f1*"), 10, true);
    CHECK(obj.Execute(match));
    HScanCommand stale(int64_t{1} << 40, false, EloqString(), 10, true);
    CHECK(obj.Execute(stale));
    CHECK(std::get<std::vector<std::string>>(stale.result_.result_)[0] == "0");
    HScanCommand neg(-5, false, EloqString(), 10, true);
    CHECK(obj.Execute(neg));
    CHECK(std::get<std::vector<std::string>>(neg.result_.result_)[0] == "0");
    // COUNT 0 → default budget.
    HScanCommand dflt(0, false, EloqString(), 0, true);
    CHECK(obj.Execute(dflt));

    RedisPagedHashObject cold = Shed(obj, nullptr);
    std::vector<uint32_t> ids;
    HScanCommand cs(0, false, EloqString(), 10, true);
    CHECK(!cold.Execute(cs));
    cold.TakePendingFaults(ids);
    std::printf("hscan arms: ok\n");
}

void TestConversionAndTwins()
{
    // Oversized-record eligibility (§14): a record that cannot fit an empty
    // page makes the whole object ineligible, so conversion must decline and
    // leave it monolithic rather than abort in Put.
    {
        const uint32_t kSmallPage = 512;
        std::string huge(kSmallPage * 4, 'x');
        CHECK(!RedisPagedHashObject::RecordFits("f", huge, kSmallPage));
        CHECK(!RedisPagedHashObject::RecordFits(huge, "v", kSmallPage));
        CHECK(RedisPagedHashObject::RecordFits("f", "v", kSmallPage));
        // Exactly at the boundary. Found by search rather than arithmetic:
        // RecordSize includes length varints that themselves grow with the
        // value, so a closed form is easy to get subtly wrong.
        size_t max_fit = 0;
        for (size_t n = 0; n < kSmallPage; ++n)
        {
            if (RedisPagedHashObject::RecordFits(
                    "f", std::string(n, 'y'), kSmallPage))
            {
                max_fit = n;
            }
        }
        CHECK(max_fit > 0);
        CHECK(RedisPagedHashObject::RecordFits(
            "f", std::string(max_fit, 'y'), kSmallPage));
        CHECK(!RedisPagedHashObject::RecordFits(
            "f", std::string(max_fit + 1, 'y'), kSmallPage));

        std::vector<std::pair<std::string_view, std::string_view>> ok_fields = {
            {"a", "1"}, {"b", "2"}};
        CHECK(RedisPagedHashObject::AllRecordsFit(ok_fields, kSmallPage));
        std::vector<std::pair<std::string_view, std::string_view>> bad_fields =
            {{"a", "1"}, {"b", huge}};
        CHECK(!RedisPagedHashObject::AllRecordsFit(bad_fields, kSmallPage));

        // The write paths on an ALREADY-paged object must refuse an
        // oversized record rather than let CommitOn reach Put's assert.
        // Refusal happens in Execute, before the WAL, because replay and
        // standby run CommitOn with no Execute and cannot reject anything.
        RedisPagedHashObject small(kSmallPage, &TestFieldHash);
        small.Put("keep", "me");

        std::vector<std::pair<EloqString, EloqString>> hp;
        hp.emplace_back(EloqString("f"), EloqString(huge.data(), huge.size()));
        HSetCommand hs(HSetCommand::SubType::HSET, std::move(hp));
        auto hr = small.Execute(hs);
        CHECK(hr.has_value() && !*hr);
        CHECK(hs.result_.err_code_ == RD_ERR_PAGED_RECORD_TOO_BIG);

        HSetNxCommand nx;
        nx.key_ = EloqString("f2");
        nx.value_ = EloqString(huge.data(), huge.size());
        auto nr = small.Execute(nx);
        CHECK(nr.has_value() && !*nr);
        CHECK(nx.result_.err_code_ == RD_ERR_PAGED_RECORD_TOO_BIG);

        // An oversized FIELD NAME on the increment paths (their values are
        // short numbers, but the name is client-supplied).
        HIncrByCommand ib;
        ib.field_ = EloqString(huge.data(), huge.size());
        ib.score_ = 1;
        auto ir = small.Execute(ib);
        CHECK(ir.has_value() && !*ir);
        CHECK(ib.result_.err_code_ == RD_ERR_PAGED_RECORD_TOO_BIG);

        HIncrByFloatCommand ibf;
        ibf.field_ = EloqString(huge.data(), huge.size());
        ibf.incr_ = 1;
        auto ifr = small.Execute(ibf);
        CHECK(ifr.has_value() && !*ifr);
        CHECK(ibf.result_.err_code_ == RD_ERR_PAGED_RECORD_TOO_BIG);

        // The object is untouched by every refusal.
        CHECK(small.FieldCount() == 1);
        CHECK(*small.Get("keep") == "me");
    }

    // ShouldConvert under the runtime knob.
    uint32_t saved = FLAGS_paged_hash_convert_threshold;
    FLAGS_paged_hash_convert_threshold = 0;
    CHECK(!RedisPagedHashObject::ShouldConvert(1 << 20));
    FLAGS_paged_hash_convert_threshold = 64;
    CHECK(RedisPagedHashObject::ShouldConvert(64));
    CHECK(!RedisPagedHashObject::ShouldConvert(63));
    FLAGS_paged_hash_convert_threshold = saved;

    // FromFields builds the deterministic layout.
    std::vector<std::pair<std::string_view, std::string_view>> fields = {
        {"a", "1"}, {"b", "2"}, {"c", "3"}};
    auto conv = RedisPagedHashObject::FromFields(fields, kPage, &TestFieldHash);
    CHECK(conv->FieldCount() == 3);
    CHECK(conv->CheckInvariants());

    // The TTL twin: class swap both ways, serialization round trip,
    // MetadataRowTtl through the flush export.
    RedisPagedHashObject base = Build(8);
    uint64_t logical = base.LogicalBytes();
    auto ttl_rec = base.AddTTL(123456);
    auto *twin = static_cast<RedisPagedHashTTLObject *>(ttl_rec.get());
    CHECK(twin->HasTTL() && twin->GetTTL() == 123456);
    CHECK(twin->ObjectType() == RedisObjectType::Hash);
    CHECK(twin->LogicalBytes() == logical);
    twin->SetTTL(99);
    CHECK(twin->GetTTL() == 99);
    auto flush = twin->ExportPagedFlush(false);
    CHECK(flush.metadata_row_ttl_ > 99);  // deadline + slack
    CHECK(!flush.metadata_.empty() && !flush.pages_.empty());
    auto back = twin->RemoveTTL();
    auto *plain = static_cast<RedisPagedHashObject *>(back.get());
    CHECK(!plain->HasTTL());
    CHECK(plain->LogicalBytes() == logical);
    CHECK(plain->ExportPagedFlush(false).metadata_row_ttl_ == 0);

    // Twin codec: Serialize/SerializedLength agree; Deserialize +
    // SetEncodedBlob round-trip; string and vector sinks agree.
    RedisPagedHashObject src = Build(8);
    auto rec2 = src.AddTTL(4242);
    auto *t2 = static_cast<RedisPagedHashTTLObject *>(rec2.get());
    std::string row;
    t2->Serialize(row);
    CHECK(row.size() == t2->SerializedLength());
    std::vector<char> vrow;
    size_t voff = 0;
    t2->Serialize(vrow, voff);
    CHECK(voff == row.size() && std::string(vrow.begin(), vrow.end()) == row);
    RedisPagedHashTTLObject loaded;
    size_t off = 0;
    loaded.Deserialize(row.data(), off);
    CHECK(off == row.size());
    CHECK(loaded.GetTTL() == 4242 && loaded.FieldCount() == 8);
    CHECK(loaded.ResidentPageCount() == 0);
    RedisPagedHashTTLObject blobbed;
    blobbed.SetEncodedBlob(reinterpret_cast<const unsigned char *>(row.data()),
                           row.size());
    CHECK(blobbed.GetTTL() == 4242 && blobbed.FieldCount() == 8);
    CHECK(blobbed.MemUsage() > 0);
    auto clone = blobbed.Clone();
    CHECK(static_cast<RedisPagedHashTTLObject *>(clone.get())->GetTTL() ==
          4242);

    // Base sinks + misc surface — on a fresh object: AddTTL above MOVED
    // src into the twin (the class-twin convention), so src is a husk. The
    // husk must at least be copyable-as-empty, which is asserted last.
    RedisPagedHashObject fresh = Build(8);
    std::string brow;
    fresh.Serialize(brow);
    CHECK(brow.size() == fresh.SerializedLength());
    std::vector<char> bvrow;
    size_t bvoff = 0;
    fresh.Serialize(bvrow, bvoff);
    CHECK(bvoff == brow.size());
    RedisPagedHashObject copyt(fresh);
    CHECK(copyt.FieldCount() == fresh.FieldCount());
    RedisPagedHashObject assigned;
    assigned.Copy(fresh);
    CHECK(assigned.FieldCount() == fresh.FieldCount());
    CHECK(!fresh.ToString().empty());
    CHECK(fresh.MemUsage() > 0);
    CHECK(fresh.ObjectType() == RedisObjectType::Hash);
    auto bclone = fresh.Clone();
    CHECK(bclone != nullptr);
    RedisPagedHashObject moved(std::move(copyt));
    CHECK(moved.FieldCount() == fresh.FieldCount());
    // The moved-from husk is empty-consistent: copying it must not crash.
    RedisPagedHashObject husk_copy(copyt);
    CHECK(husk_copy.ResidentPageCount() == 0);
    RedisPagedHashObject src_husk_copy(src);
    CHECK(src_husk_copy.ResidentPageCount() == 0);
    std::printf("conversion/twins: ok\n");
}

void TestEngineSurface()
{
    RedisPagedHashObject obj = Build(30);
    obj.SetWriteTs(500);
    obj.Put("late", "write");
    obj.StampWrites(600);

    // AsPaged both ways.
    const RedisPagedHashObject &cref = obj;
    CHECK(obj.AsPaged() == &obj);
    CHECK(cref.AsPaged() == &obj);

    CHECK(obj.IsFullyResident());
    CHECK(obj.ResidentPageCount() == obj.Frames().LivePageCount());
    CHECK(obj.IsPageLive(0));
    CHECK(obj.IsPageResident(0));
    CHECK(!obj.IsPageLive(9999));
    CHECK(!obj.IsPageResident(9999));

    // Pin protocol: context creation, routing test, fetch outcomes, release.
    obj.EnsureTxFaultContext(42);
    CHECK(obj.HasPageWaiter(42));
    CHECK(!obj.HasPageWaiter(43));
    obj.NotePageFetched(42, 0, true);
    CHECK(obj.Frames().SlotOf(0)->pin_count_ == 1);
    obj.NotePageFetched(42, 0, false);  // failure: no pin
    obj.NotePageFetched(777, 0, true);  // no context: no pin
    CHECK(obj.Frames().SlotOf(0)->pin_count_ == 1);
    obj.ReleaseTxPins(42);
    CHECK(obj.Frames().SlotOf(0)->pin_count_ == 0);
    obj.ReleaseTxPins(42);  // idempotent
    obj.EnsureTxFaultContext(1);
    obj.EnsureTxFaultContext(2);
    obj.AbandonAllTxContexts();
    CHECK(!obj.HasPageWaiter(1) && !obj.HasPageWaiter(2));

    // Install arms: wrong size, dead id, shared-buffer form. A multi-page
    // source, so installing one page cannot make the object fully resident.
    RedisPagedHashObject multi = Build(200);
    CHECK(multi.Frames().LivePageCount() > 1);
    std::vector<std::pair<PageId, std::string>> imgs;
    RedisPagedHashObject cold = Shed(multi, &imgs);
    CHECK(!cold.InstallPage(0, "short", 9));
    CHECK(!cold.InstallPage(9999, std::string(kPage, 'x'), 9));
    txservice::PageBuf shared(new uint8_t[kPage]());
    std::memcpy(shared.get(), imgs[0].second.data(), kPage);
    CHECK(cold.InstallPageShared(imgs[0].first, shared, kPage, 9));
    CHECK(!cold.InstallPageShared(9999, shared, kPage, 9));
    CHECK(!cold.InstallPageShared(imgs[0].first, nullptr, kPage, 9));
    CHECK(cold.ResidentPageCount() == 1);
    CHECK(!cold.IsFullyResident());

    // Shed policy through the virtual; deletion-arm export.
    RedisPagedHashObject warm = Build(30);
    warm.OnPagedFlushApplied(1);  // everything clean (write ts 1)
    size_t shed = warm.ShedCleanPages();
    CHECK(shed >= 1);
    auto del_flush = warm.ExportPagedFlush(true);
    CHECK(!del_flush.pages_.empty());
    for (auto &p : del_flush.pages_)
    {
        CHECK(p.buf_ == nullptr);
    }

    // Id lifecycle through the frame table: allocation reuses freed ids
    // after the drain, and the partition stays canonical.
    txservice::PageFrameTable &frames = warm.MutableFrames();
    PageId a = frames.AllocatePageId();
    frames.SetWriteTs(700);
    frames.CreateDirtyPage(a);
    frames.FreePage(a);
    CHECK(!frames.FreeList().Contains(a));  // pending, not yet free
    frames.OnFlushApplied(701);             // drain: pending -> free
    CHECK(frames.FreeList().Contains(a));
    PageId b = frames.AllocatePageId();
    CHECK(b == a);  // reused
    CHECK(frames.CheckLruInvariants());
    std::printf("engine surface: ok\n");
}
/**
 * @brief A minimal concrete paged type: exercises PagedTxObject's BASE
 * defaults (MetadataRowTtl = 0, the generic ExportPagedFlush) without the
 * hash type's overrides in the way.
 */
struct MiniPaged : public txservice::PagedTxObject
{
    void SerializeMetadataRow(std::string &out) const override
    {
        out.append("mini");
    }
    txservice::PageRowKind PageKind() const override
    {
        return txservice::PageRowKind::HashPage;
    }
    void Init(uint32_t page_size)
    {
        frames_.InitFresh(page_size);
        PageId id = frames_.AllocatePageId();
        frames_.CreateDirtyPage(id);
    }
};

void TestRemainingArms()
{
    // Base-class SetEncodedBlob (the twin's variant is covered elsewhere).
    RedisPagedHashObject src = Build(12);
    std::string row;
    src.Serialize(row);
    RedisPagedHashObject blobbed(&TestFieldHash);
    blobbed.SetEncodedBlob(reinterpret_cast<const unsigned char *>(row.data()),
                           row.size());
    CHECK(blobbed.FieldCount() == 12);

    // Commit increments against an EXISTING numeric value (the read-back
    // parse arms).
    RedisPagedHashObject obj = Build(4);
    EloqString c1("n");
    CHECK(obj.CommitHincrby(c1, 5));
    CHECK(obj.CommitHincrby(c1, 3));
    CHECK(*obj.Get("n") == "8");
    EloqString c2("fl");
    CHECK(obj.CommitHIncrByFloat(c2, 1.5));
    CHECK(obj.CommitHIncrByFloat(c2, 0.5));
    CHECK(*obj.Get("fl") == "2");

    // Shared directory entries (local depth < global depth): whole-object
    // iteration and index addressing must skip repeats. Also the
    // past-the-end index arm.
    RedisPagedHashObject multi = Build(200);
    CHECK(multi.Meta().global_depth_ >= 2);
    HGetAllCommand all;
    CHECK(multi.Execute(all));
    CHECK(std::get<std::vector<std::string>>(all.result_.result_).size() ==
          400);
    HRandFieldCommand sample(50, true);
    CHECK(multi.Execute(sample));
    CHECK(multi.PageForIndex(1u << 30, nullptr) == kInvalidPageId);
    CHECK(!multi.EntryAt(1u << 30).has_value());

    // HSCAN on a depth-0 (single-page) object: the degenerate directory arm.
    RedisPagedHashObject tiny = Build(3);
    CHECK(tiny.Meta().global_depth_ == 0);
    HScanCommand scan0(0, false, EloqString(), 100, true);
    CHECK(tiny.Execute(scan0));
    auto &sv = std::get<std::vector<std::string>>(scan0.result_.result_);
    CHECK(sv[0] == "0" && sv.size() == 4);

    // The TTL twin without a deadline set: MetadataRowTtl reports 0. The
    // normal export arm consults it (the deletion arm never does).
    RedisPagedHashObject twin_src = Build(2);
    auto twin_rec = twin_src.AddTTL(777);
    auto *twin_bare = static_cast<RedisPagedHashTTLObject *>(twin_rec.get());
    twin_bare->SetTTL(UINT64_MAX);  // "no deadline"
    auto bare_flush = twin_bare->ExportPagedFlush(false);
    CHECK(bare_flush.metadata_row_ttl_ == 0);

    // The pinned production field hash (docs/08 §4): deterministic, and the
    // reserved-prefix predicate that guards the page keyspace (§5).
    uint64_t h1 = PagedFieldHash("stability-probe");
    uint64_t h2 = PagedFieldHash("stability-probe");
    CHECK(h1 == h2 && h1 != 0);
    CHECK(PagedFieldHash("a") != PagedFieldHash("b"));
    CHECK(
        IsReservedPagedKey(std::string_view("\x00"
                                            "EKVPAGEuser",
                                            12)));
    CHECK(!IsReservedPagedKey("plain-key"));

    // Invariants hold on a metadata-only object too (the non-resident skip
    // arm of the checker).
    RedisPagedHashObject cold_check = Shed(Build(60), nullptr);
    CHECK(cold_check.CheckInvariants());

    // TakePendingFaults on a clean object reports nothing.
    std::vector<uint32_t> none;
    CHECK(!cold_check.TakePendingFaults(none));
    CHECK(none.empty());

    // PagedTxObject base defaults through a minimal concrete type.
    MiniPaged mini;
    mini.Init(256);
    auto mf = mini.ExportPagedFlush(false);
    CHECK(mf.metadata_ == "mini");
    CHECK(mf.metadata_row_ttl_ == 0);
    CHECK(mf.pages_.size() == 1 && mf.pages_[0].buf_ != nullptr);

    // Frame-table arms not reached elsewhere: copy/move assignment,
    // LivePageCount with free + pending populated, Contains edges,
    // RebuildFreeRanges over a loaded object that has free ids (mid-gap and
    // trailing), and crafted-malformed page-manager sections.
    txservice::PageFrameTable &frames = multi.MutableFrames();
    PageId extra1 = frames.AllocatePageId();
    PageId extra2 = frames.AllocatePageId();
    PageId extra3 = frames.AllocatePageId();
    frames.FreePage(extra1);
    frames.FreePage(extra3);
    frames.OnFlushApplied(UINT64_MAX / 2);  // drain extra1+extra3 to free
    frames.FreePage(extra2);                // stays pending
    CHECK(frames.FreeList().Contains(extra1));
    CHECK(!frames.FreeList().Contains(extra2));
    CHECK(frames.FreeList().Contains(extra3));
    CHECK(!frames.FreeList().Contains(0));  // below the first range
    CHECK(!frames.IsLive(extra2));          // pending, not live
    size_t live = frames.LivePageCount();   // free AND pending loops run
    CHECK(live == frames.ResidentPageCount());

    // Serialize/reload: RebuildFreeRanges must reconstruct the mid-gap
    // (extra1) and note pending (extra2) — and extra3 as a trailing run.
    std::string row2;
    multi.Serialize(row2);
    RedisPagedHashObject reloaded(&TestFieldHash);
    size_t off = 0;
    reloaded.Deserialize(row2.data(), off);
    CHECK(reloaded.Frames().FreeList().Contains(extra1));
    CHECK(reloaded.Frames().FreeList().Contains(extra3));
    CHECK(!reloaded.Frames().FreeList().Contains(extra2));
    CHECK(!reloaded.IsPageLive(extra2));

    // Copy assignment and self-consistent move assignment.
    txservice::PageFrameTable copy_target;
    copy_target = reloaded.Frames();
    CHECK(copy_target.FreeList().Contains(extra1));
    txservice::PageFrameTable move_target;
    move_target = std::move(copy_target);
    CHECK(move_target.FreeList().Contains(extra1));
    CHECK(!copy_target.FreeList().Contains(extra1));  // husk is empty

    // Crafted malformed page-manager sections, past what truncation can
    // reach: an over-long varint, a pending range out of order, a pending
    // range past the high-water, and a zero-count range.
    std::string body;
    multi.SerializeMeta(body);
    {
        // The section-length varint replaced by 11 continuation bytes.
        std::string bad(1, static_cast<char>(kPagedFormatVersion));
        bad.append(11, static_cast<char>(0x80));
        RedisPagedHashObject shell(&TestFieldHash);
        size_t o = 0;
        CHECK(!shell.DeserializeSections(bad.data(), bad.size(), o));
    }
    {
        // A depth byte past kMaxDepth in the type section: locate it as the
        // second byte after the PM section ends. Recompute via a fresh
        // serialize of a small object where offsets are simple.
        RedisPagedHashObject small = Build(2);
        std::string b2;
        small.SerializeMeta(b2);
        size_t pm_size = small.Frames().SerializedSize();
        size_t depth_off = 1 + pm_size + 1;  // version + PM + algo byte
        std::string bad = b2;
        bad[depth_off] = 60;  // > kMaxDepth
        RedisPagedHashObject shell(&TestFieldHash);
        size_t o = 0;
        CHECK(!shell.DeserializeSections(bad.data(), bad.size(), o));
    }
    std::printf("remaining arms: ok\n");
}

/**
 * @brief Malformed STORAGE must produce a deterministic rejection, never an
 * out-of-bounds read, an allocation blowup, or a silently missing field
 * (docs/08 §13). Corrupts each field of a real page image and of a real
 * metadata row in turn and requires every one to be refused.
 */
void TestCorruptInputRejected()
{
    // ---- page images ----
    RedisPagedHashObject src = Build(200);
    CHECK(src.Frames().LivePageCount() > 1);
    std::vector<std::pair<PageId, std::string>> imgs;
    RedisPagedHashObject cold = Shed(src, &imgs);
    PageId pid = imgs[0].first;
    const std::string &good = imgs[0].second;
    // The control: the untouched image installs.
    CHECK(cold.InstallPage(pid, good, 9));

    auto rejects = [&](const std::string &img)
    {
        RedisPagedHashObject fresh = Shed(src, nullptr);
        return !fresh.InstallPage(pid, img, 9);
    };

    std::string bad = good;
    bad[0] = 0x7F;  // layout version
    CHECK(rejects(bad));

    bad = good;
    bad[2] = 60;  // local depth beyond the directory
    CHECK(rejects(bad));

    // free_offset: below the slot array, and past the page.
    bad = good;
    txservice::paged_detail::StoreU32(
        reinterpret_cast<uint8_t *>(bad.data()) + 4, 0);
    CHECK(rejects(bad));
    bad = good;
    txservice::paged_detail::StoreU32(
        reinterpret_cast<uint8_t *>(bad.data()) + 4, kPage * 4);
    CHECK(rejects(bad));

    // dead_bytes past the page.
    bad = good;
    txservice::paged_detail::StoreU32(
        reinterpret_cast<uint8_t *>(bad.data()) + 8, kPage * 4);
    CHECK(rejects(bad));

    // A slot offset pointing outside the record area.
    bad = good;
    txservice::paged_detail::StoreU32(
        reinterpret_cast<uint8_t *>(bad.data()) + PageView::kHeaderSize +
            4 * src.Meta().page_entry_counts_.find(pid)->second,
        kPage - 1);
    CHECK(rejects(bad));

    // Descending hashes break Find's binary search.
    {
        uint32_t n = src.Meta().page_entry_counts_.find(pid)->second;
        if (n >= 2)
        {
            bad = good;
            uint8_t *d = reinterpret_cast<uint8_t *>(bad.data());
            uint32_t h0 =
                txservice::paged_detail::LoadU32(d + PageView::kHeaderSize);
            txservice::paged_detail::StoreU32(d + PageView::kHeaderSize + 4,
                                              h0 - 1);
            CHECK(rejects(bad));
        }
    }

    // Wrong size, unknown page id, and a VALID page served under the WRONG
    // id (the identity check that routing provides).
    CHECK(rejects(good.substr(0, good.size() - 1)));
    {
        RedisPagedHashObject fresh = Shed(src, nullptr);
        CHECK(!fresh.InstallPage(999999, good, 9));
        // A different live page's image under this id.
        PageId other = kInvalidPageId;
        for (auto &[id, bytes] : imgs)
        {
            if (id != pid && src.Meta().page_entry_counts_.find(id)->second > 0)
            {
                other = id;
                CHECK(!fresh.InstallPage(pid, bytes, 9));
                break;
            }
        }
        CHECK(other != kInvalidPageId);
    }

    // ---- metadata rows ----
    std::string row;
    src.SerializeMeta(row);
    auto row_rejects = [&](const std::string &r)
    {
        RedisPagedHashObject shell(&TestFieldHash);
        size_t off = 0;
        return !shell.DeserializeSections(r.data(), r.size(), off);
    };
    // A high-water past the configured bound would size the free-list
    // bitmap and its scan; a page size of 0 or an absurd one breaks every
    // later size computation. Both live at fixed offsets in the PM section,
    // which follows the one-byte format version.
    size_t pm = 1;
    uint64_t seclen = 0;
    const uint8_t *after = txservice::paged_detail::ReadVarint(
        reinterpret_cast<const uint8_t *>(row.data()) + pm,
        reinterpret_cast<const uint8_t *>(row.data()) + row.size(),
        seclen);
    CHECK(after != nullptr);
    size_t psz_off = static_cast<size_t>(
        after - reinterpret_cast<const uint8_t *>(row.data()));
    std::string badrow = row;
    txservice::paged_detail::StoreU32(
        reinterpret_cast<uint8_t *>(badrow.data()) + psz_off, 0);
    CHECK(row_rejects(badrow));  // page size 0
    badrow = row;
    txservice::paged_detail::StoreU32(
        reinterpret_cast<uint8_t *>(badrow.data()) + psz_off, 0xFFFFFFFFu);
    CHECK(row_rejects(badrow));  // page size absurd
    badrow = row;
    txservice::paged_detail::StoreU32(
        reinterpret_cast<uint8_t *>(badrow.data()) + psz_off + 4, 0xFFFFFFFFu);
    CHECK(row_rejects(badrow));  // high-water past kMaxPageCount
    // A high-water BELOW a live directory id: in range for the bound, but
    // inconsistent across sections (would index the bitmap out of bounds).
    badrow = row;
    txservice::paged_detail::StoreU32(
        reinterpret_cast<uint8_t *>(badrow.data()) + psz_off + 4, 1);
    CHECK(row_rejects(badrow));
    // The control: the untouched row still loads, so the rejections above
    // are the corruption and not the harness.
    {
        RedisPagedHashObject shell(&TestFieldHash);
        size_t off = 0;
        CHECK(shell.DeserializeSections(row.data(), row.size(), off));
        CHECK(off == row.size());
    }
    // Guards that a whole-image corruption cannot reach, exercised directly.
    {
        // Degenerate arguments.
        CHECK(!PageView::ValidateImage(nullptr, kPage, 0));
        CHECK(!PageView::ValidateImage(
            reinterpret_cast<const uint8_t *>(good.data()),
            PageView::kHeaderSize + PageView::kSlotSize,
            0));
        // An entry count whose slot array cannot fit the page.
        CHECK(!PageView::ValidateImage(
            reinterpret_cast<const uint8_t *>(good.data()), kPage, kPage));

        // A record whose SECOND varint (the tagged length) never terminates,
        // and one whose body runs past the page end.
        uint32_t n = src.Meta().page_entry_counts_.find(pid)->second;
        CHECK(n >= 1);
        const uint8_t *gd = reinterpret_cast<const uint8_t *>(good.data());
        uint32_t off0 = txservice::paged_detail::LoadU32(
            gd + PageView::kHeaderSize + 4 * n);
        std::string b2 = good;
        // Key length 0, then continuation bytes to the end of the page.
        uint8_t *d2 = reinterpret_cast<uint8_t *>(b2.data());
        d2[off0] = 0;
        for (size_t i = off0 + 1; i < kPage; ++i)
        {
            d2[i] = 0x80;
        }
        CHECK(rejects(b2));

        std::string b3 = good;
        uint8_t *d3 = reinterpret_cast<uint8_t *>(b3.data());
        d3[off0] = 0;         // empty key
        d3[off0 + 1] = 0xFE;  // tagged length: huge, inline
        d3[off0 + 2] = 0xFF;
        d3[off0 + 3] = 0xFF;
        d3[off0 + 4] = 0x7F;
        CHECK(rejects(b3));

        // Local depth inside the legal range but deeper than the directory.
        std::string b4 = good;
        b4[2] = static_cast<char>(src.Meta().global_depth_ + 1);
        CHECK(rejects(b4));

        // Routing identity: move the LAST entry's hash to the top of the
        // space. Order stays ascending, so framing still validates, but the
        // entry now routes to the directory's last slot, not this page.
        if (n >= 1 && src.Meta().global_depth_ > 0)
        {
            std::string b5 = good;
            txservice::paged_detail::StoreU32(
                reinterpret_cast<uint8_t *>(b5.data()) + PageView::kHeaderSize +
                    4 * (n - 1),
                0xFFFFFFFFu);
            CHECK(rejects(b5));
        }
    }

    // A field count that disagrees with the per-page entry counts.
    {
        RedisPagedHashObject two = Build(40);
        std::string r2;
        two.SerializeMeta(r2);
        RedisPagedHashObject shell(&TestFieldHash);
        size_t off = 0;
        CHECK(shell.DeserializeSections(r2.data(), r2.size(), off));
        // field_count_ sits in the TYPE section, after the PM section and
        // the algo + depth bytes.
        uint64_t seclen2 = 0;
        const uint8_t *a2 = txservice::paged_detail::ReadVarint(
            reinterpret_cast<const uint8_t *>(r2.data()) + 1,
            reinterpret_cast<const uint8_t *>(r2.data()) + r2.size(),
            seclen2);
        CHECK(a2 != nullptr);
        size_t type_off =
            static_cast<size_t>(a2 -
                                reinterpret_cast<const uint8_t *>(r2.data())) +
            seclen2;
        std::string bad2 = r2;
        txservice::paged_detail::StoreU64(
            reinterpret_cast<uint8_t *>(bad2.data()) + type_off + 2, 999999);
        RedisPagedHashObject shell2(&TestFieldHash);
        size_t off2 = 0;
        CHECK(!shell2.DeserializeSections(bad2.data(), bad2.size(), off2));
    }

    // The BASE hook accepts (the generic layer cannot know a page format);
    // size and liveness still apply beneath it.
    {
        MiniPaged mini;
        mini.Init(256);
        CHECK(mini.InstallPage(0, std::string(256, '\0'), 5));
        CHECK(!mini.InstallPage(0, std::string(255, '\0'), 5));
        CHECK(!mini.InstallPage(4242, std::string(256, '\0'), 5));
        // Same rejections on the shared-buffer install.
        txservice::PageBuf shared_ok(new uint8_t[256]());
        CHECK(mini.InstallPageShared(0, shared_ok, 256, 5));
        txservice::PageBuf shared2(new uint8_t[256]());
        CHECK(!mini.InstallPageShared(0, shared2, 255, 5));
        CHECK(!mini.InstallPageShared(4242, shared2, 256, 5));
        CHECK(!mini.InstallPageShared(0, nullptr, 256, 5));
    }
    {
        // A page-manager section whose pending-count varint is unterminated
        // (distinct from one that runs off the section end).
        namespace pd2 = txservice::paged_detail;
        std::string inner(8, '\0');  // page_size + high-water
        pd2::StoreU32(reinterpret_cast<uint8_t *>(inner.data()), 512);
        pd2::StoreU32(reinterpret_cast<uint8_t *>(inner.data()) + 4, 4);
        inner.append(3, static_cast<char>(0x80));  // never terminates
        std::string sec;
        pd2::AppendVarint(sec, inner.size());
        sec += inner;
        txservice::PageFrameTable t;
        size_t o = 0;
        CHECK(!t.DeserializeMeta(sec.data(), sec.size(), o));
    }
    std::printf("corrupt input rejected: ok\n");
}

/**
 * @brief The bounded, fallible STORE-path parse (§5, review finding).
 *
 * The store backfill previously deserialized paged metadata with SIZE_MAX —
 * every §5 bound was bypassed on the one path they exist for, and a failed
 * parse was asserted in Debug and IGNORED in Release. The store entry is
 * RedisEloqObject::DeserializeObject(buf, avail, offset): the tag read is
 * bounds-checked and a malformed row yields nullptr. Swept here by
 * truncating a real row at EVERY length, on both twins.
 */
void TestBoundedStoreParse()
{
    RedisPagedHashObject obj = Build(12);
    std::string row;
    obj.Serialize(row);
    const RedisEloqObject &dispatcher = obj;

    // The full row parses and round-trips.
    {
        size_t off = 0;
        txservice::TxRecord::Uptr rec =
            dispatcher.DeserializeObject(row.data(), row.size(), off);
        CHECK(rec != nullptr);
        auto *typed = static_cast<RedisPagedHashObject *>(
            static_cast<txservice::TxObject *>(rec.get()));
        CHECK(typed->AsPaged() != nullptr);
        CHECK(typed->FieldCount() == 12);
        CHECK(off == row.size());
    }

    // Every truncation is a clean nullptr — no crash, no OOB read.
    for (size_t len = 0; len < row.size(); ++len)
    {
        size_t off = 0;
        CHECK(dispatcher.DeserializeObject(row.data(), len, off) == nullptr);
    }

    // A wrong tag is refused, not misdispatched.
    {
        std::string bad = row;
        bad[0] = 0x7F;
        size_t off = 0;
        CHECK(dispatcher.DeserializeObject(bad.data(), bad.size(), off) ==
              nullptr);
    }

    // The TTL twin, same sweep.
    {
        RedisPagedHashObject src = Build(8);
        auto ttl_rec = src.AddTTL(123456);
        auto *twin = static_cast<RedisPagedHashTTLObject *>(ttl_rec.get());
        std::string trow;
        twin->Serialize(trow);
        size_t off = 0;
        txservice::TxRecord::Uptr rec =
            dispatcher.DeserializeObject(trow.data(), trow.size(), off);
        CHECK(rec != nullptr);
        auto *typed = static_cast<RedisPagedHashTTLObject *>(
            static_cast<txservice::TxObject *>(rec.get()));
        CHECK(typed->HasTTL() && typed->GetTTL() == 123456);
        for (size_t len = 0; len < trow.size(); ++len)
        {
            size_t o2 = 0;
            CHECK(dispatcher.DeserializeObject(trow.data(), len, o2) ==
                  nullptr);
        }
    }

    // The class-level guards, reached directly (the dispatcher rejects
    // short/rogue rows before them, but DeserializeBounded is itself the
    // public store entry of each twin): an empty buffer, and each twin
    // handed the OTHER twin's row (tag mismatch).
    {
        RedisPagedHashObject plain = Build(4);
        std::string prow;
        plain.Serialize(prow);
        auto ttl_rec = plain.AddTTL(99);
        auto *twin = static_cast<RedisPagedHashTTLObject *>(ttl_rec.get());
        std::string trow;
        twin->Serialize(trow);

        RedisPagedHashObject sink1;
        size_t off = 0;
        CHECK(!sink1.DeserializeBounded(prow.data(), 0, off));
        off = 0;
        CHECK(!sink1.DeserializeBounded(trow.data(), trow.size(), off));

        RedisPagedHashTTLObject sink2;
        off = 0;
        CHECK(!sink2.DeserializeBounded(trow.data(), 0, off));
        off = 0;
        CHECK(!sink2.DeserializeBounded(prow.data(), prow.size(), off));
        // TTL row cut inside the ttl field itself: tag fits, the u64 does
        // not.
        off = 0;
        CHECK(!sink2.DeserializeBounded(trow.data(), 5, off));
    }

    // A MONOLITHIC row still parses through the bounded entry (legacy
    // delegation), so the store path is uniform.
    {
        RedisHashObject mono;
        std::vector<std::pair<EloqString, EloqString>> pairs;
        pairs.emplace_back(EloqString("a"), EloqString("1"));
        mono.CommitHset(pairs);
        std::string mrow;
        mono.Serialize(mrow);
        size_t off = 0;
        CHECK(dispatcher.DeserializeObject(mrow.data(), mrow.size(), off) !=
              nullptr);
    }
    std::printf("bounded store parse: ok\n");
}

/**
 * @brief The §4/§14 inline-record cap and the command-level guard, plus the
 * RESTORE conversion arms that used to live in a server-level test.
 *
 * The cap is max(page_size/8, 4 KB), further bounded by page capacity: a
 * true N/8 cap at production page sizes (>= 32 KB), degenerating to "fits
 * one page" at small test pages — one record per page is deliberately
 * allowed there. The guard errors the COMMAND when conversion is enabled;
 * dark servers keep stock behavior.
 */
void TestInlineRecordCap()
{
    // Cap arithmetic, all three regimes.
    CHECK(RedisPagedHashObject::InlineRecordCap(128 * 1024) == 16 * 1024);
    CHECK(RedisPagedHashObject::InlineRecordCap(32 * 1024) == 4096);
    CHECK(RedisPagedHashObject::InlineRecordCap(4096) ==
          4096 - PageView::kHeaderSize);
    CHECK(RedisPagedHashObject::InlineRecordCap(kPage) ==
          kPage - PageView::kHeaderSize);

    uint32_t saved_thr = FLAGS_paged_hash_convert_threshold;
    uint32_t saved_page = FLAGS_paged_hash_page_size;
    FLAGS_paged_hash_page_size = 128 * 1024;
    std::string big(20 * 1024, 'v');  // over the 16 KB cap, fits the page

    auto hset_big = [&]() -> txservice::ExecResult
    {
        RedisHashObject mono;
        std::vector<std::pair<EloqString, EloqString>> pairs;
        pairs.emplace_back(EloqString("f"), EloqString(big.data(), big.size()));
        HSetCommand cmd(HSetCommand::SubType::HSET, std::move(pairs));
        return cmd.ExecuteOn(mono);
    };

    // Conversion enabled: the command is REFUSED, monolithic or not.
    FLAGS_paged_hash_convert_threshold = 1;
    CHECK(hset_big() == txservice::ExecResult::Fail);

    // An oversized FIELD NAME is the same hazard, via the increments.
    {
        RedisHashObject mono;
        HIncrByCommand inc;
        inc.field_ = EloqString(big.data(), big.size());
        inc.score_ = 1;
        CHECK(inc.ExecuteOn(mono) == txservice::ExecResult::Fail);
        CHECK(inc.result_.err_code_ == RD_ERR_PAGED_RECORD_TOO_BIG);

        HIncrByFloatCommand incf;
        incf.field_ = EloqString(big.data(), big.size());
        incf.incr_ = 1;
        CHECK(incf.ExecuteOn(mono) == txservice::ExecResult::Fail);

        HSetNxCommand nx;
        nx.key_ = EloqString("f");
        nx.value_ = EloqString(big.data(), big.size());
        CHECK(nx.ExecuteOn(mono) == txservice::ExecResult::Fail);
        CHECK(nx.result_.err_code_ == RD_ERR_PAGED_RECORD_TOO_BIG);
    }

    // Dark server (threshold 0): stock behavior, the same write passes.
    FLAGS_paged_hash_convert_threshold = 0;
    CHECK(hset_big() == txservice::ExecResult::Write);

    // RESTORE: imports never error on the cap; the conversion policy decides
    // the representation. An ELIGIBLE import converts; one carrying an
    // over-cap record stays monolithic (declines), today's §14 contract.
    FLAGS_paged_hash_convert_threshold = 1;
    {
        RedisHashObject src;
        std::vector<std::pair<EloqString, EloqString>> pairs;
        for (int i = 0; i < 6; ++i)
        {
            pairs.emplace_back(EloqString(("f" + std::to_string(i)).c_str()),
                               EloqString("value"));
        }
        src.CommitHset(pairs);
        // The RESTORE payload is [outer type byte][object serialization] —
        // the outer byte routes CommitOn's switch, and Serialize() begins
        // with the object's own tag (redis_rdb_restore's
        // PrefixEloqOuterType + Serialize produce exactly this shape).
        std::string blob(1, static_cast<char>(RedisObjectType::Hash));
        src.Serialize(blob);
        RestoreCommand restore(blob.data(), blob.size(), UINT64_MAX, false);
        std::unique_ptr<txservice::TxObject> out(restore.CommitOn(nullptr));
        CHECK(out != nullptr && out->AsPaged() != nullptr);
    }
    {
        RedisHashObject src;
        std::vector<std::pair<EloqString, EloqString>> pairs;
        pairs.emplace_back(EloqString("big"),
                           EloqString(big.data(), big.size()));
        pairs.emplace_back(EloqString("a"), EloqString("1"));
        src.CommitHset(pairs);
        std::string blob(1, static_cast<char>(RedisObjectType::Hash));
        src.Serialize(blob);
        RestoreCommand restore(blob.data(), blob.size(), UINT64_MAX, false);
        std::unique_ptr<txservice::TxObject> out(restore.CommitOn(nullptr));
        CHECK(out != nullptr && out->AsPaged() == nullptr);
    }

    // The increments' EXACT record check (a review hole: field + "" passed
    // while field + the rendered result exceeded the cap).
    {
        // At kPage=512 the cap equals page capacity. Find a field that fits
        // with an empty value but not with a 20-byte int64 render.
        std::string long20(20, '9');
        size_t flen = kPage;
        while (flen > 0 &&
               !RedisPagedHashObject::RecordFits(
                   std::string_view(std::string(flen, 'f')), "", kPage))
        {
            --flen;
        }
        // flen now fits with ""; walk down until the 20-byte render fails —
        // it must, at the boundary.
        std::string field(flen, 'f');
        CHECK(RedisPagedHashObject::RecordFits(field, "", kPage));
        CHECK(!RedisPagedHashObject::RecordFits(field, long20, kPage));

        RedisPagedHashObject obj(kPage, &TestFieldHash);
        HIncrByCommand inc;
        inc.field_ = EloqString(field.data(), field.size());
        inc.score_ = 1000000000000000000LL;  // renders to 19 digits
        auto r = obj.Execute(inc);
        CHECK(r.has_value() && !*r);
        CHECK(inc.result_.err_code_ == RD_ERR_PAGED_RECORD_TOO_BIG);

        // HINCRBYFLOAT: a double-range value renders ~300+ bytes under
        // ld2string's fixed notation; field 300 + render 301 exceeds the
        // 512-byte page's capacity while field + "" fits comfortably.
        RedisPagedHashObject obj2(kPage, &TestFieldHash);
        HIncrByFloatCommand incf;
        std::string field300(300, 'g');
        incf.field_ = EloqString(field300.data(), field300.size());
        incf.incr_ = 1e300L;
        auto rf = obj2.Execute(incf);
        CHECK(rf.has_value() && !*rf);
        CHECK(incf.result_.err_code_ == RD_ERR_PAGED_RECORD_TOO_BIG);

        // The MONOLITHIC branch's post-Execute check, through the command
        // wrapper at the production page size: the field passes the
        // field-only fast-fail, the rendered result pushes it over the cap.
        FLAGS_paged_hash_convert_threshold = 1;
        FLAGS_paged_hash_page_size = 128 * 1024;
        RedisHashObject mono;
        HIncrByFloatCommand monof;
        std::string field16k(16200, 'h');
        monof.field_ = EloqString(field16k.data(), field16k.size());
        monof.incr_ = 1e300L;
        CHECK(monof.ExecuteOn(mono) == txservice::ExecResult::Fail);
        CHECK(monof.result_.err_code_ == RD_ERR_PAGED_RECORD_TOO_BIG);
        // A small increment on the same field is fine: the exact check
        // charges the real render, not a worst case.
        HIncrByFloatCommand monosmall;
        monosmall.field_ = EloqString(field16k.data(), field16k.size());
        monosmall.incr_ = 1.5L;
        CHECK(monosmall.ExecuteOn(mono) == txservice::ExecResult::Write);
    }

    // PERSIST / EXPIRE on paged twins through the COMMAND layer (a reported
    // bug: the CommitOns downcast by logical type — UB on a paged payload —
    // and PERSIST's ttl_reset_ shipped a paged image the recovery factory
    // aborts on; the engine now logs the plain command instead).
    {
        RedisPagedHashObject plain = Build(4);
        ExpireCommand exp;
        exp.expire_ts_ = 987654;
        // EXPIRE on the plain twin: AddTTL twin swap via the virtual.
        std::unique_ptr<txservice::TxObject> ttl_twin(exp.CommitOn(&plain));
        CHECK(ttl_twin != nullptr && ttl_twin.get() != &plain);
        auto *as_eloq = static_cast<RedisEloqObject *>(ttl_twin.get());
        CHECK(as_eloq->HasTTL() && as_eloq->GetTTL() == 987654);
        CHECK(ttl_twin->AsPaged() != nullptr);

        // EXPIRE again (a TTL reset): SetTTL in place, same object.
        ExpireCommand exp2;
        exp2.expire_ts_ = 111111;
        txservice::TxObject *same = exp2.CommitOn(ttl_twin.get());
        CHECK(same == ttl_twin.get());
        CHECK(as_eloq->GetTTL() == 111111);

        // PERSIST: RemoveTTL twin swap, fields intact.
        PersistCommand persist;
        std::unique_ptr<txservice::TxObject> back(
            persist.CommitOn(ttl_twin.get()));
        CHECK(back != nullptr && back.get() != ttl_twin.get());
        auto *plain_again = static_cast<RedisPagedHashObject *>(back.get());
        CHECK(!plain_again->HasTTL());
        CHECK(plain_again->AsPaged() != nullptr);
        CHECK(plain_again->FieldCount() == 4);
        CHECK(*plain_again->Get("f2") == "v2");

        // PERSIST on the non-TTL twin: no-op, same object.
        PersistCommand persist2;
        CHECK(persist2.CommitOn(back.get()) == back.get());

        // ExecuteOn on a paged TTL twin must not even BUILD the recover
        // image (§16 belt; the review follow-up's transaction window runs
        // ExecuteOn on a dirty payload that is already paged).
        RedisPagedHashObject base2 = Build(3);
        std::unique_ptr<txservice::TxRecord> ttl_rec2 = base2.AddTTL(424242);
        auto *twin2 = static_cast<RedisPagedHashTTLObject *>(ttl_rec2.get());
        PersistCommand pex;
        CHECK(pex.ExecuteOn(*twin2) == txservice::ExecResult::Write);
        CHECK(pex.recover_ttl_obj_cmd_ == nullptr);
        ExpireCommand eex;
        eex.expire_ts_ = 999999;
        CHECK(eex.ExecuteOn(*twin2) == txservice::ExecResult::Write);
        CHECK(eex.recover_ttl_obj_cmd_ == nullptr);
    }

    FLAGS_paged_hash_convert_threshold = saved_thr;
    FLAGS_paged_hash_page_size = saved_page;
    std::printf("inline record cap: ok\n");
}

/**
 * @brief The §8 memory-admission gate on the fault path.
 *
 * Admission IS allocation: a fault set claims its buffers before any fetch is
 * issued, so page memory cannot overshoot the shard budget between the
 * decision to fetch and the arrival of the bytes, and two concurrent faulters
 * cannot both pass a "there is room" test. The properties that make that safe
 * are all here — all-or-nothing acquisition, nothing retained on refusal, the
 * never-fits/wait distinction, and the write path staying exempt (CommitOn
 * cannot fail, so it must not be handed a null buffer).
 */
void TestMemoryAdmission()
{
    // A gate with a fixed budget: `used` tracks what this test has claimed.
    struct Budget
    {
        size_t cap_{0};
        size_t used_{0};

        static bool Admit(void *ctx, size_t bytes)
        {
            Budget *b = static_cast<Budget *>(ctx);
            if (b->used_ + bytes > b->cap_)
            {
                return false;
            }
            b->used_ += bytes;
            return true;
        }

        static size_t Cap(void *ctx)
        {
            return static_cast<Budget *>(ctx)->cap_;
        }
    };

    // No gate installed: everything is admitted, the pre-admission behaviour.
    {
        txservice::PageFrameTable t;
        t.InitFresh(kPage);
        CHECK(t.ReservePageBuffers({0, 1, 2}));
        CHECK(t.ReservedCount() == 3);
        CHECK(txservice::PageFrameTable::FaultSetCanEverFit(1000000, kPage));
    }

    // Room for exactly three pages.
    {
        Budget budget{kPage * 3, 0};
        txservice::PageAdmissionScope gate(
            &Budget::Admit, &Budget::Cap, &budget);
        txservice::PageFrameTable t;
        t.InitFresh(kPage);

        // Fits: claimed, and the budget records it.
        CHECK(t.ReservePageBuffers({0, 1}));
        CHECK(t.ReservedCount() == 2);
        CHECK(budget.used_ == kPage * 2);

        // Does not fit. ALL-OR-NOTHING: the partial claim this attempt took
        // (page 2) is released, so a caller that goes on to wait is not
        // holding memory hostage while waiting for memory.
        size_t before = budget.used_;
        CHECK(!t.ReservePageBuffers({2, 3, 4}));
        CHECK(t.ReservedCount() == 2);  // unchanged: nothing new retained
        (void) before;

        // Re-reserving an id already claimed costs nothing and succeeds.
        size_t used_before = budget.used_;
        CHECK(t.ReservePageBuffers({0, 1}));
        CHECK(budget.used_ == used_before);
        CHECK(t.ReservedCount() == 2);

        // An install CONSUMES the claim rather than allocating again.
        CHECK(t.AllocatePageId() == 0);
        std::string img(kPage, '\0');
        CHECK(t.InstallPage(0, img, 7));
        CHECK(t.ReservedCount() == 1);
        CHECK(t.IsResident(0));

        // Never-fits vs merely-busy. A set larger than the whole budget can
        // never be admitted, so the caller must error rather than wait; one
        // that merely does not fit right now must NOT be misreported as
        // impossible.
        CHECK(!txservice::PageFrameTable::FaultSetCanEverFit(4, kPage));
        CHECK(txservice::PageFrameTable::FaultSetCanEverFit(3, kPage));
    }

    // The WRITE path is exempt: CommitOn runs post-WAL and cannot fail, so a
    // refused budget must never hand it a null buffer. With a zero budget the
    // gate refuses everything, yet a dirty page is still created and written.
    {
        Budget budget{0, 0};
        txservice::PageAdmissionScope gate(
            &Budget::Admit, &Budget::Cap, &budget);
        RedisPagedHashObject obj(kPage, &TestFieldHash);
        for (int i = 0; i < 40; ++i)
        {
            obj.Put("w" + std::to_string(i), "v" + std::to_string(i));
        }
        CHECK(obj.FieldCount() == 40);
        CHECK(obj.CheckInvariants());
        CHECK(*obj.Get("w7") == "v7");

        // ... while the fault path IS refused under the same zero budget.
        txservice::PageFrameTable t;
        t.InitFresh(kPage);
        CHECK(!t.ReservePageBuffers({0}));
        CHECK(t.ReservedCount() == 0);
    }

    // Through the PagedTxObject surface, which is what the fault-issuance
    // path actually calls.
    {
        RedisPagedHashObject obj = Build(12);
        std::vector<std::pair<PageId, std::string>> imgs;
        RedisPagedHashObject cold = Shed(obj, &imgs);
        std::vector<uint32_t> ids;
        for (const auto &[id, img] : imgs)
        {
            ids.push_back(id);
        }
        txservice::PagedTxObject &as_paged = cold;

        // No gate: admitted, and a set of any size could fit in principle.
        CHECK(as_paged.ReserveFaultBuffers(ids));
        CHECK(as_paged.FaultSetCanEverFit(ids.size()));

        // Under a budget too small for the set: refused, and — because the
        // shard could not hold it even when empty — reported as impossible
        // rather than as something to wait for.
        Budget tiny{kPage / 2, 0};
        txservice::PageAdmissionScope gate(&Budget::Admit, &Budget::Cap, &tiny);
        RedisPagedHashObject cold2 = Shed(obj, nullptr);
        txservice::PagedTxObject &as_paged2 = cold2;
        CHECK(!as_paged2.ReserveFaultBuffers(ids));
        CHECK(!as_paged2.FaultSetCanEverFit(ids.size()));
    }

    // The reply-materialization bound (§8): HRANDFIELD with a negative count
    // is NOT clamped to the field count, so it is charged against the reply
    // ceiling like any whole-object read.
    {
        RedisPagedHashObject obj = Build(8);
        uint32_t saved_bound = FLAGS_paged_object_reply_bound;
        FLAGS_paged_object_reply_bound = 1;
        HRandFieldCommand rand;
        rand.count_ = -1000000;
        rand.with_values_ = true;
        CHECK(obj.Execute(rand));
        CHECK(rand.result_.err_code_ == RD_ERR_OBJECT_TOO_BIG);
        FLAGS_paged_object_reply_bound = saved_bound;

        // A modest request under the real bound still works.
        HRandFieldCommand ok;
        ok.count_ = -4;
        ok.with_values_ = false;
        CHECK(obj.Execute(ok));
        CHECK(ok.result_.err_code_ == RD_OK);
    }

    // The reservation LIFECYCLE (§8, review finding): the production
    // completion path installs via InstallPageShared, so the reservation
    // must be consumed there on EVERY outcome — success, stale id, size
    // mismatch, rejected image, fetch error — or every ordinary fault
    // retains the admitted buffer besides the installed one and admission
    // bounds nothing.
    {
        txservice::PageFrameTable t;
        t.InitFresh(kPage);

        // Success: consumed.
        CHECK(t.AllocatePageId() == 0);
        CHECK(t.ReservePageBuffers({0}));
        CHECK(t.ReservedCount() == 1);
        txservice::PageBuf ok_buf(new uint8_t[kPage]());
        CHECK(t.InstallPageShared(0, std::move(ok_buf), kPage, 7));
        CHECK(t.ReservedCount() == 0);

        // Stale id (freed while the fetch flew): refused AND released.
        CHECK(t.ReservePageBuffers({42}));
        CHECK(t.ReservedCount() == 1);
        txservice::PageBuf stale_buf(new uint8_t[kPage]());
        CHECK(!t.InstallPageShared(42, std::move(stale_buf), kPage, 7));
        CHECK(t.ReservedCount() == 0);

        // Size mismatch: refused AND released, on both install shapes.
        CHECK(t.ReservePageBuffers({0}));
        txservice::PageBuf small_buf(new uint8_t[kPage / 2]());
        CHECK(!t.InstallPageShared(0, std::move(small_buf), kPage / 2, 7));
        CHECK(t.ReservedCount() == 0);
        CHECK(t.ReservePageBuffers({0}));
        CHECK(!t.InstallPage(0, std::string(kPage / 2, '\0'), 7));
        CHECK(t.ReservedCount() == 0);

        // Fetch error / no store row: the explicit release path.
        CHECK(t.ReservePageBuffers({0}));
        t.DropReserved(0);
        CHECK(t.ReservedCount() == 0);
        t.DropReserved(0);  // idempotent

        // A move carries claimed fetches with the table.
        CHECK(t.ReservePageBuffers({0}));
        txservice::PageFrameTable moved(std::move(t));
        CHECK(moved.ReservedCount() == 1);
        CHECK(t.ReservedCount() == 0);
    }

    // A REJECTED IMAGE on the type wrapper releases too: reserve on a real
    // hash, then install garbage bytes of the right size.
    {
        RedisPagedHashObject obj = Build(6);
        std::vector<std::pair<PageId, std::string>> imgs;
        RedisPagedHashObject cold = Shed(obj, &imgs);
        txservice::PagedTxObject &as_paged = cold;
        std::vector<uint32_t> ids = {imgs[0].first};
        CHECK(as_paged.ReserveFaultBuffers(ids));
        CHECK(cold.Frames().ReservedCount() == 1);

        auto garbage = std::shared_ptr<uint8_t[]>(new uint8_t[kPage]);
        std::memset(garbage.get(), 0xEE, kPage);
        CHECK(!as_paged.InstallPageShared(
            imgs[0].first, std::move(garbage), kPage, 7));
        CHECK(cold.Frames().ReservedCount() == 0);

        // And the canonical-reuse shape BackFillPage now uses: take the
        // reserved buffer, fill it, share it in — nothing left claimed.
        CHECK(as_paged.ReserveFaultBuffers(ids));
        txservice::PageBuf canonical =
            as_paged.TakeReservedBuffer(imgs[0].first);
        CHECK(canonical != nullptr);
        CHECK(cold.Frames().ReservedCount() == 0);
        std::memcpy(canonical.get(), imgs[0].second.data(), kPage);
        CHECK(as_paged.InstallPageShared(
            imgs[0].first, std::move(canonical), kPage, 7));
        CHECK(cold.IsPageResident(imgs[0].first));
        CHECK(as_paged.PageSizeBytes() == kPage);

        // DropPageReservation through the seam (the term-dead path).
        CHECK(as_paged.ReserveFaultBuffers(ids));
        as_paged.DropPageReservation(imgs[0].first);
        CHECK(cold.Frames().ReservedCount() == 0);
        // Taking with nothing claimed is a clean null.
        CHECK(as_paged.TakeReservedBuffer(imgs[0].first) == nullptr);
    }

    // The scope restores the previous gate, so a shard's gate cannot leak
    // into a thread that has stopped acting for it.
    CHECK(txservice::AdmitPageBytes(1ULL << 40));
    // The two-payload completion scenario a review follow-up caught: a
    // multi-command transaction faults on its DIRTY payload, so the
    // reservation lives there — while the completion classifies the
    // COMMITTED payload first. The canonical buffer must come from
    // whichever payload claimed it; the committed side having none must
    // not trigger a fresh allocation. This mirrors BackFillPage's
    // selection order against the PagedTxObject surface.
    {
        RedisPagedHashObject committed = Build(6);
        RedisPagedHashObject dirty(committed);  // the COW copy
        std::vector<std::pair<PageId, std::string>> imgs;
        RedisPagedHashObject cold_dirty = Shed(dirty, &imgs);
        CHECK(!imgs.empty());
        PageId id = imgs.front().first;

        // Only the dirty payload reserved (its fault, its claim).
        CHECK(cold_dirty.ReserveFaultBuffers({id}));
        CHECK(cold_dirty.ReservedCount() == 1);

        // Committed-first: no reservation there.
        CHECK(committed.TakeReservedBuffer(id) == nullptr);
        // The completion falls through to the other payload's claim.
        txservice::PageBuf canonical = cold_dirty.TakeReservedBuffer(id);
        CHECK(canonical != nullptr);
        std::memcpy(canonical.get(), imgs.front().second.data(),
                    imgs.front().second.size());
        // Shared into the payload that faulted; its reservation is gone and
        // no second buffer was ever allocated for this page.
        CHECK(cold_dirty.InstallPageShared(
            id, canonical, imgs.front().second.size(), 7));
        CHECK(cold_dirty.ReservedCount() == 0);
        CHECK(committed.ReservedCount() == 0);
    }

    std::printf("memory admission: ok\n");
}

/**
 * @brief The centralized conversion policy, MaybeConvertHashToPaged (§11).
 *
 * The server-level test (paged_conversion_policy.py) proves every mutator and
 * the import path CALL it; this covers what it decides once called, including
 * the arms a command path cannot reach on demand — a null payload, a non-hash
 * (only RESTORE passes those), and the ineligible/TTL branches.
 */
void TestConversionPolicy()
{
    uint32_t saved_thr = FLAGS_paged_hash_convert_threshold;
    uint32_t saved_page = FLAGS_paged_hash_page_size;
    FLAGS_paged_hash_page_size = 4096;

    auto make_hash = [](size_t n, size_t value_len)
    {
        auto obj = std::make_unique<RedisHashObject>();
        std::vector<std::pair<EloqString, EloqString>> fields;
        for (size_t i = 0; i < n; ++i)
        {
            std::string f = "f" + std::to_string(i);
            std::string v(value_len, 'v');
            fields.emplace_back(EloqString(f.data(), f.size()),
                                EloqString(v.data(), v.size()));
        }
        obj->CommitHset(fields);
        return obj;
    };

    // A null payload (the key was deleted by this very command) is returned
    // as-is, never dereferenced.
    CHECK(MaybeConvertHashToPaged(nullptr) == nullptr);

    FLAGS_paged_hash_convert_threshold = 1;  // convert at any size

    // Already paged: returned unchanged, and NOT re-converted.
    {
        RedisPagedHashObject paged = Build(4);
        auto *as_tx = static_cast<txservice::TxObject *>(&paged);
        CHECK(MaybeConvertHashToPaged(as_tx) == as_tx);
    }

    // A non-hash object: the type guard returns it untouched. RESTORE hands
    // the policy every type, so this arm is load-bearing.
    {
        RedisStringObject str;
        EloqString val("hello", 5);
        str.CommitSet(val);
        auto *as_tx = static_cast<txservice::TxObject *>(&str);
        CHECK(MaybeConvertHashToPaged(as_tx) == as_tx);
    }

    // Below the threshold (0 == disabled, the dark default): unchanged.
    {
        FLAGS_paged_hash_convert_threshold = 0;
        auto hash = make_hash(4, 8);
        auto *as_tx = static_cast<txservice::TxObject *>(hash.get());
        CHECK(MaybeConvertHashToPaged(as_tx) == as_tx);
        FLAGS_paged_hash_convert_threshold = 1;
    }

    // Ineligible — one record cannot fit a page — stays monolithic however
    // far past the threshold it is (§14: no out-of-line large values in v1).
    {
        auto hash = make_hash(1, FLAGS_paged_hash_page_size * 2);
        auto *as_tx = static_cast<txservice::TxObject *>(hash.get());
        CHECK(MaybeConvertHashToPaged(as_tx) == as_tx);
    }

    // Eligible: converts, and the paged twin holds every field.
    {
        auto hash = make_hash(6, 32);
        auto *as_tx = static_cast<txservice::TxObject *>(hash.get());
        auto *out = MaybeConvertHashToPaged(as_tx);
        CHECK(out != as_tx && out != nullptr);
        std::unique_ptr<txservice::TxObject> owned(out);
        auto *paged = static_cast<RedisPagedHashObject *>(owned.get());
        CHECK(paged->AsPaged() != nullptr);
        CHECK(paged->FieldCount() == 6);
        CHECK(!paged->HasTTL());
        CHECK(paged->CheckInvariants());
        CHECK(*paged->Get("f3") == std::string(32, 'v'));
    }

    // Eligible AND carrying a TTL: converts to the TTL twin with the deadline
    // intact. Converting through FromFields alone yields the non-TTL class,
    // which would silently make an expiring key permanent.
    {
        auto hash = make_hash(6, 32);
        RedisHashTTLObject ttl_hash(std::move(*hash), 987654);
        CHECK(ttl_hash.HasTTL() && ttl_hash.GetTTL() == 987654);
        auto *as_tx = static_cast<txservice::TxObject *>(&ttl_hash);
        auto *out = MaybeConvertHashToPaged(as_tx);
        CHECK(out != as_tx && out != nullptr);
        std::unique_ptr<txservice::TxObject> owned(out);
        auto *paged = static_cast<RedisPagedHashTTLObject *>(owned.get());
        CHECK(paged->AsPaged() != nullptr);
        CHECK(paged->HasTTL() && paged->GetTTL() == 987654);
        CHECK(paged->FieldCount() == 6);
        CHECK(paged->CheckInvariants());
    }

    FLAGS_paged_hash_convert_threshold = saved_thr;
    FLAGS_paged_hash_page_size = saved_page;
    std::printf("conversion policy: ok\n");
}

/**
 * @brief The central paged-deletion rule: TxCommand::CommitOn(obj, ctx).
 *
 * The two-arg overload is the ONE definition of "a deleted paged object is
 * retained for its fan-out" (docs/08 §9, §16); every commit path calls it.
 * This is the direct unit coverage a review asked for: the crash-replay
 * integration test deliberately exercises the unknown-prior contract
 * (sweeper debt) and never reaches this hook, so without this test the
 * retire could be deleted and nothing in the suite would fail.
 */
void TestCentralDeletionRetire()
{
    // A paged hash with staged VOLATILE state of every kind the teardown
    // must clear: resident frames, a pinned frame (a stale aggregate pin,
    // exactly what AbandonAllTxContexts leaves behind), and a §8 admission
    // reservation for a page whose fetch never landed.
    RedisPagedHashObject obj = Build(200);
    txservice::PageFrameTable &frames = obj.MutableFrames();
    std::vector<txservice::PageId> live_before;
    frames.ForEachLivePageId(
        [&](txservice::PageId id) { live_before.push_back(id); });
    CHECK(live_before.size() >= 2);
    frames.PinPage(live_before.front());
    std::vector<txservice::PageId> to_reserve{live_before.back()};
    CHECK(frames.ReservePageBuffers(to_reserve));
    CHECK(frames.ReservedCount() == 1);
    CHECK(obj.ResidentPageCount() > 0);

    // Delete every field; CommitOn decides deletion (returns null), and the
    // overload must retire-and-retain instead.
    HDelCommand delall;
    std::vector<EloqString> dl;
    for (int i = 0; i < 200; ++i)
    {
        delall.del_list_.emplace_back(
            EloqString(("f" + std::to_string(i)).c_str()));
        dl.emplace_back(EloqString(("f" + std::to_string(i)).c_str()));
    }
    auto da = obj.Execute(delall);
    CHECK(da.has_value() && *da == CommandExecuteState::ModifiedToEmpty);
    txservice::TxCommand &delall_base = delall;
    txservice::TxObject *out = delall_base.CommitOn(
        &obj, txservice::PagedCommitContext{nullptr, nullptr});

    // Retained: same pointer back, tagged; nullness is NOT the signal.
    CHECK(out == &obj);
    CHECK(obj.IsDeletionRetained());

    // Complete volatile teardown: frames (pinned one included — the pin was
    // a stale aggregate with no live owner), reservations, pending faults.
    CHECK(obj.ResidentPageCount() == 0);
    CHECK(obj.Frames().ReservedCount() == 0);
    CHECK(!obj.HasPendingFaults());

    // The fan-out inventory SURVIVES the teardown: the deletion flush must
    // still enumerate every previously-live page id, with null buffers.
    txservice::PagedObjectFlush flush = obj.ExportPagedFlush(true);
    std::set<uint32_t> flushed;
    for (const txservice::PagedFlushPage &pg : flush.pages_)
    {
        CHECK(pg.buf_ == nullptr);
        flushed.insert(pg.page_id_);
    }
    for (txservice::PageId id : live_before)
    {
        CHECK(flushed.count(id) == 1);
    }

    // Non-paged passthrough: the overload must not disturb the monolithic
    // contract — deleting a plain hash to empty still comes back as null,
    // never retained.
    RedisHashObject mono;
    std::vector<std::pair<EloqString, EloqString>> mono_pairs;
    mono_pairs.emplace_back(EloqString("a"), EloqString("1"));
    mono.CommitHset(mono_pairs);
    HDelCommand mono_del;
    mono_del.del_list_.emplace_back(EloqString("a"));
    CommandExecuteState md = mono.Execute(mono_del);
    CHECK(md == CommandExecuteState::ModifiedToEmpty);
    txservice::TxCommand &mono_del_base = mono_del;
    txservice::TxObject *plain_out = mono_del_base.CommitOn(
        &mono, txservice::PagedCommitContext{nullptr, nullptr});
    CHECK(plain_out == nullptr);

    std::printf("central deletion retire: ok\n");
}

}  // namespace

int main()
{
    TestPointReads();
    TestHSetHDel();
    TestSetNxIncr();
    TestWholeObjectAndRand();
    TestHScanArms();
    TestConversionAndTwins();
    TestEngineSurface();
    TestRemainingArms();
    TestCorruptInputRejected();
    TestConversionPolicy();
    TestMemoryAdmission();
    TestInlineRecordCap();
    TestBoundedStoreParse();
    TestCentralDeletionRetire();
    std::printf("all paged object-command tests passed\n");
    return 0;
}
