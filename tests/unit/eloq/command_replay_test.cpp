#define CATCH_CONFIG_MAIN

#include <catch2/catch_all.hpp>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "redis_command.h"
#include "redis_list_object.h"
#include "redis_object.h"
#include "redis_set_object.h"
#include "redis_zset_object.h"

// These tests exercise the standby-apply / WAL-recovery replay path, which is
// commit-only: a command image is Deserialize'd and CommitOn is invoked WITHOUT
// a preceding ExecuteOn. For a remote-owned key the replayed image is the
// PRE-ExecuteOn one, so any element filtering (ZADD NX/XX/GT/LT) or index
// normalization (LTRIM) that ExecuteOn performs is absent at commit time. The
// commit methods must therefore be deterministic from the current object state.
// See eloqdata/eloqkv#509.

namespace
{
using EloqKV::CommandExecuteState;
using EloqKV::EloqString;
using EloqKV::LTrimCommand;
using EloqKV::RedisHashSetObject;
using EloqKV::RedisListObject;
using EloqKV::RedisZsetObject;
using EloqKV::SPopCommand;
using EloqKV::ZAddCommand;
using EloqKV::ZParams;

EloqString Str(std::string_view s)
{
    return EloqString(s.data(), s.size());
}

// Deserialize a serialized command image on the replay path. Serialize() writes
// a leading command-type byte that RedisTableSchema::CreateTxCommand consumes
// to pick the concrete command type before delegating to Deserialize, so strip
// it here exactly as the real replay dispatcher does.
template <typename CmdT>
void ReplayDeserialize(CmdT &cmd, const std::string &image)
{
    cmd.Deserialize({image.data() + 1, image.size() - 1});
}

RedisZsetObject MakeZset(
    const std::vector<std::pair<double, std::string>> &members)
{
    RedisZsetObject obj;
    std::vector<std::pair<double, EloqString>> vec;
    for (const auto &[score, member] : members)
    {
        vec.emplace_back(score, Str(member));
    }
    std::variant<std::monostate,
                 std::pair<double, EloqString>,
                 std::vector<std::pair<double, EloqString>>>
        elements = std::move(vec);
    ZParams params;  // no flags: plain add
    obj.CommitZAdd(elements, params, false);
    return obj;
}

// Canonical, order-stable textual dump so REQUIRE prints both sides on
// mismatch.
std::string ZsetDump(const RedisZsetObject &obj)
{
    std::map<std::string, double> sorted;
    for (const auto &[field, score] : obj.Elements())
    {
        sorted.emplace(std::string(field), score);
    }
    std::string out;
    for (const auto &[field, score] : sorted)
    {
        out += field;
        out += '=';
        out += std::to_string(score);
        out += ';';
    }
    return out;
}

ZParams MakeParams(uint8_t flags)
{
    ZParams params;
    params.flags = flags;
    return params;
}

ZAddCommand MakeZAddVector(
    const std::vector<std::pair<double, std::string>> &members, uint8_t flags)
{
    std::vector<std::pair<double, EloqString>> vec;
    for (const auto &[score, member] : members)
    {
        vec.emplace_back(score, Str(member));
    }
    ZParams params = MakeParams(flags);
    return ZAddCommand(
        std::move(vec), params, ZAddCommand::ElementType::vector);
}

// PRE-image replay: primary runs ExecuteOn + CommitOn; replica replays the
// command's pre-ExecuteOn image via Deserialize + CommitOn only.
std::string RunZsetPreimageReplica(const RedisZsetObject &base,
                                   ZAddCommand &cmd)
{
    std::string preimage;
    cmd.Serialize(preimage);

    RedisZsetObject replica(base);
    ZAddCommand replay_cmd;
    ReplayDeserialize(replay_cmd, preimage);
    replay_cmd.CommitOn(&replica);
    return ZsetDump(replica);
}

std::string RunZsetPrimary(const RedisZsetObject &base, ZAddCommand &cmd)
{
    RedisZsetObject primary(base);
    cmd.ExecuteOn(primary);
    cmd.CommitOn(&primary);
    return ZsetDump(primary);
}

RedisListObject MakeList(const std::vector<std::string> &members)
{
    RedisListObject obj;
    std::vector<EloqString> vec;
    for (const auto &member : members)
    {
        vec.emplace_back(Str(member));
    }
    obj.CommitRPush(vec);
    return obj;
}

std::string ListDump(const RedisListObject &obj)
{
    std::string out;
    for (const auto &element : obj.Elements())
    {
        out += element.StringView();
        out += ',';
    }
    return out;
}

RedisHashSetObject MakeSet(const std::vector<std::string> &members)
{
    RedisHashSetObject obj;
    std::vector<EloqString> vec;
    for (const auto &member : members)
    {
        vec.emplace_back(Str(member));
    }
    obj.CommitSAdd(vec, false);
    return obj;
}

std::string SetDump(const RedisHashSetObject &obj)
{
    std::map<std::string, int> sorted;
    for (const auto &element : obj.Elements())
    {
        sorted.emplace(std::string(element.StringView()), 0);
    }
    std::string out;
    for (const auto &[member, _] : sorted)
    {
        out += member;
        out += ',';
    }
    return out;
}
}  // namespace

// ------------------------- RED: ZADD flag filtering -------------------------

TEST_CASE("zadd_gt_single_filtered_replay")
{
    RedisZsetObject base = MakeZset({{10.0, "m"}});
    ZAddCommand cmd = MakeZAddVector({{5.0, "m"}}, ZADD_IN_GT);
    std::string replica = RunZsetPreimageReplica(base, cmd);
    std::string primary = RunZsetPrimary(base, cmd);
    // GT: 5 < 10, so the update is dropped; the key keeps score 10.
    REQUIRE(replica == primary);
}

TEST_CASE("zadd_gt_multi_partial_replay")
{
    RedisZsetObject base = MakeZset({{10.0, "a"}, {1.0, "b"}});
    ZAddCommand cmd = MakeZAddVector({{5.0, "a"}, {7.0, "b"}}, ZADD_IN_GT);
    std::string replica = RunZsetPreimageReplica(base, cmd);
    std::string primary = RunZsetPrimary(base, cmd);
    // a dropped (5 < 10), b applied (7 > 1) -> {a:10, b:7}.
    REQUIRE(replica == primary);
}

TEST_CASE("zadd_nx_existing_member_replay")
{
    RedisZsetObject base = MakeZset({{10.0, "m"}});
    ZAddCommand cmd = MakeZAddVector({{5.0, "m"}}, ZADD_IN_NX);
    std::string replica = RunZsetPreimageReplica(base, cmd);
    std::string primary = RunZsetPrimary(base, cmd);
    // NX: member already exists -> update dropped, keeps 10.
    REQUIRE(replica == primary);
}

TEST_CASE("zadd_xx_missing_member_replay")
{
    RedisZsetObject base = MakeZset({{10.0, "m"}});
    ZAddCommand cmd = MakeZAddVector({{5.0, "n"}}, ZADD_IN_XX);
    std::string replica = RunZsetPreimageReplica(base, cmd);
    std::string primary = RunZsetPrimary(base, cmd);
    // XX: member does not exist -> insert dropped, "n" must not appear.
    REQUIRE(replica == primary);
}

TEST_CASE("zadd_lt_filtered_replay")
{
    RedisZsetObject base = MakeZset({{10.0, "m"}});
    ZAddCommand cmd = MakeZAddVector({{15.0, "m"}}, ZADD_IN_LT);
    std::string replica = RunZsetPreimageReplica(base, cmd);
    std::string primary = RunZsetPrimary(base, cmd);
    // LT: 15 > 10 -> update dropped, keeps 10.
    REQUIRE(replica == primary);
}

TEST_CASE("zadd_intra_command_duplicate_replay")
{
    RedisZsetObject base = MakeZset({{10.0, "m"}});
    // Sequential GT semantics: (12,m) is filtered against the pending (15,m).
    ZAddCommand cmd = MakeZAddVector({{15.0, "m"}, {12.0, "m"}}, ZADD_IN_GT);
    std::string replica = RunZsetPreimageReplica(base, cmd);
    std::string primary = RunZsetPrimary(base, cmd);
    // Primary keeps 15; a naive replay would end at 12.
    REQUIRE(replica == primary);
}

// ---------------------------- RED: LTRIM ranges -----------------------------

TEST_CASE("ltrim_full_range_replay")
{
    RedisListObject base = MakeList({"a", "b", "c", "d", "e"});
    LTrimCommand cmd(0, -1);
    std::string preimage;
    cmd.Serialize(preimage);

    RedisListObject primary(base);
    cmd.ExecuteOn(primary);
    cmd.CommitOn(&primary);

    RedisListObject replica(base);
    LTrimCommand replay_cmd;
    ReplayDeserialize(replay_cmd, preimage);
    replay_cmd.CommitOn(&replica);

    // (0,-1) keeps every element; a naive replay wipes the list.
    REQUIRE(ListDump(replica) == ListDump(primary));
}

TEST_CASE("ltrim_negative_end_replay")
{
    RedisListObject base = MakeList({"a", "b", "c", "d", "e"});
    LTrimCommand cmd(1, -2);
    std::string preimage;
    cmd.Serialize(preimage);

    RedisListObject primary(base);
    cmd.ExecuteOn(primary);
    cmd.CommitOn(&primary);

    RedisListObject replica(base);
    LTrimCommand replay_cmd;
    ReplayDeserialize(replay_cmd, preimage);
    replay_cmd.CommitOn(&replica);

    // Primary keeps [b,c,d]; a naive replay does begin()-1 iterator arithmetic.
    REQUIRE(ListDump(replica) == ListDump(primary));
}

TEST_CASE("ltrim_empty_range_start_overflow_replay")
{
    RedisListObject base = MakeList({"a", "b", "c", "d", "e"});
    LTrimCommand cmd(10, -2);  // start past end-of-list
    std::string preimage;
    cmd.Serialize(preimage);

    RedisListObject replica(base);
    LTrimCommand replay_cmd;
    ReplayDeserialize(replay_cmd, preimage);
    auto *result = replay_cmd.CommitOn(&replica);

    // Empty range -> object is deleted (nullptr) and left empty.
    REQUIRE(result == nullptr);
    REQUIRE(replica.Empty());
}

TEST_CASE("ltrim_empty_forward_range_replay")
{
    RedisListObject base = MakeList({"a", "b", "c", "d", "e"});
    LTrimCommand cmd(3, 1);  // start > end
    std::string preimage;
    cmd.Serialize(preimage);

    RedisListObject replica(base);
    LTrimCommand replay_cmd;
    ReplayDeserialize(replay_cmd, preimage);
    auto *result = replay_cmd.CommitOn(&replica);

    REQUIRE(result == nullptr);
    REQUIRE(replica.Empty());
}

TEST_CASE("ltrim_local_modified_to_empty_commit")
{
    // Pre-existing local UB, independent of replay: Execute normalizes (10,-2)
    // in place to (5,3) and returns ModifiedToEmpty, but the engine still calls
    // CommitOn(5,3) unconditionally.
    RedisListObject list = MakeList({"a", "b", "c", "d", "e"});
    LTrimCommand cmd(10, -2);
    CommandExecuteState state = list.Execute(cmd);
    REQUIRE(state == CommandExecuteState::ModifiedToEmpty);

    auto *result = cmd.CommitOn(&list);
    REQUIRE(result == nullptr);
    REQUIRE(list.Empty());
}

// ------------------------------- GREEN guards -------------------------------

TEST_CASE("zadd_gt_postimage_idempotent")
{
    RedisZsetObject base = MakeZset({{10.0, "a"}, {1.0, "b"}});
    ZAddCommand cmd = MakeZAddVector({{5.0, "a"}, {7.0, "b"}}, ZADD_IN_GT);

    RedisZsetObject primary(base);
    cmd.ExecuteOn(primary);
    // Post-ExecuteOn image (already filtered), captured before CommitOn moves
    // the surviving fields out of the command.
    std::string postimage;
    cmd.Serialize(postimage);
    cmd.CommitOn(&primary);

    RedisZsetObject replica(base);
    ZAddCommand replay_cmd;
    ReplayDeserialize(replay_cmd, postimage);
    replay_cmd.CommitOn(&replica);

    REQUIRE(ZsetDump(replica) == ZsetDump(primary));
}

TEST_CASE("zadd_nx_postimage_idempotent")
{
    RedisZsetObject base = MakeZset({{10.0, "m"}});
    ZAddCommand cmd = MakeZAddVector({{5.0, "m"}, {3.0, "n"}}, ZADD_IN_NX);

    RedisZsetObject primary(base);
    cmd.ExecuteOn(primary);
    std::string postimage;
    cmd.Serialize(postimage);
    cmd.CommitOn(&primary);

    RedisZsetObject replica(base);
    ZAddCommand replay_cmd;
    ReplayDeserialize(replay_cmd, postimage);
    replay_cmd.CommitOn(&replica);

    REQUIRE(ZsetDump(replica) == ZsetDump(primary));
}

TEST_CASE("ltrim_full_range_postimage_idempotent")
{
    RedisListObject base = MakeList({"a", "b", "c", "d", "e"});
    LTrimCommand cmd(0, -1);

    RedisListObject primary(base);
    cmd.ExecuteOn(primary);  // normalizes to (0,4)
    std::string postimage;
    cmd.Serialize(postimage);
    cmd.CommitOn(&primary);

    RedisListObject replica(base);
    LTrimCommand replay_cmd;
    ReplayDeserialize(replay_cmd, postimage);
    replay_cmd.CommitOn(&replica);

    REQUIRE(ListDump(replica) == ListDump(primary));
}

TEST_CASE("zadd_incr_pair_replay")
{
    RedisZsetObject base = MakeZset({{10.0, "m"}});
    ZParams params = MakeParams(ZADD_IN_INCR);
    ZAddCommand cmd(std::pair<double, EloqString>{5.0, Str("m")},
                    params,
                    ZAddCommand::ElementType::pair);

    std::string preimage;
    cmd.Serialize(preimage);

    RedisZsetObject primary(base);
    cmd.ExecuteOn(primary);
    cmd.CommitOn(&primary);

    RedisZsetObject replica(base);
    ZAddCommand replay_cmd;
    ReplayDeserialize(replay_cmd, preimage);
    replay_cmd.CommitOn(&replica);

    // INCR recomputes 10+5=15 at commit; must be neither double-added nor
    // double-filtered.
    REQUIRE(ZsetDump(replica) == ZsetDump(primary));
    REQUIRE(ZsetDump(primary) == "m=15.000000;");
}

TEST_CASE("spop_postimage_replay")
{
    RedisHashSetObject base = MakeSet({"a", "b", "c"});
    SPopCommand cmd(2);

    RedisHashSetObject primary(base);
    cmd.ExecuteOn(primary);
    // Post-image records the popped members for idempotent replay.
    std::string postimage;
    cmd.Serialize(postimage);
    cmd.CommitOn(&primary);

    RedisHashSetObject replica(base);
    SPopCommand replay_cmd;
    ReplayDeserialize(replay_cmd, postimage);
    replay_cmd.CommitOn(&replica);

    REQUIRE(SetDump(replica) == SetDump(primary));
    REQUIRE(primary.Elements().size() == 1);
}

TEST_CASE("zadd_plain_no_flags_replay")
{
    RedisZsetObject base = MakeZset({{10.0, "m"}});
    ZAddCommand cmd = MakeZAddVector({{5.0, "m"}}, 0);
    std::string replica = RunZsetPreimageReplica(base, cmd);
    std::string primary = RunZsetPrimary(base, cmd);
    // No flags: nothing to filter, plain set to 5 on both sides.
    REQUIRE(replica == primary);
    REQUIRE(primary == "m=5.000000;");
}
