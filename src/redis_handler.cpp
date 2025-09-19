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
#include "redis_handler.h"

#include <brpc/redis.h>
#include <bthread/task_group.h>
#include <butil/strings/string_piece.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <list>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cc_protocol.h"
#include "eloq_key.h"
#include "eloq_string.h"
#include "error_messages.h"
#include "redis_command.h"
#include "redis_connection_context.h"
#include "redis_object.h"
#include "redis_replier.h"
#include "redis_service.h"
#include "sharder.h"
#include "tx_key.h"
#include "tx_request.h"
#include "tx_service.h"
#include "tx_util.h"

namespace bthread
{
extern BAIDU_THREAD_LOCAL TaskGroup *tls_task_group;
};

namespace EloqKV
{
brpc::RedisCommandHandlerResult PingCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "ping");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParsePingCommand(cmd_arg_list, &reply);
    if (success)
    {
        redis_impl_->ExecuteCommand(ctx, &cmd, &reply);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult AuthCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "auth");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseAuthCommand(cmd_arg_list, &reply);
    if (success)
    {
        redis_impl_->ExecuteCommand(ctx, &cmd, &reply);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult SelectCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "select");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseSelectCommand(cmd_arg_list, &reply);
    if (success)
    {
        redis_impl_->ExecuteCommand(ctx, &cmd, &reply);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ConfigCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "config");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseConfigCommand(cmd_arg_list, &reply);
    if (success)
    {
        redis_impl_->ExecuteCommand(ctx, &cmd, &reply);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult DBSizeCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "dbsize");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseDBSizeCommand(cmd_arg_list, &reply);
    if (success)
    {
        redis_impl_->ExecuteCommand(ctx, &cmd, &reply);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult SlowLogCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "slowlog");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseSlowLogCommand(cmd_arg_list, &reply);
    if (success)
    {
        redis_impl_->ExecuteCommand(ctx, &cmd, &reply);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ReadOnlyCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "readonly");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseReadOnlyCommand(cmd_arg_list, &reply);
    if (success)
    {
        redis_impl_->ExecuteCommand(ctx, &cmd, &reply);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ClusterCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "cluster");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseClusterCommand(cmd_arg_list, &reply);
    if (success)
    {
        redis_impl_->ExecuteCommand(ctx, cmd.get(), &reply);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult SentinelCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "sentinel");

    if (args.size() < 2)
    {
        output->SetError(
            "ERR wrong number of arguments for 'sentinel' command");
        return brpc::REDIS_CMD_HANDLED;
    }

    std::string sub = args[1].as_string();
    std::transform(sub.begin(),
                   sub.end(),
                   sub.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    auto ng_id_from_arg = [](const butil::StringPiece &sp, uint32_t &ng_id)
    {
        ng_id = 0;
        const char *b = sp.data();
        const char *e = sp.data() + sp.size();
        while (b < e && std::isspace(static_cast<unsigned char>(*b)))
            b++;
        if (b == e)
            return false;
        uint64_t val = 0;
        for (; b < e; ++b)
        {
            if (!std::isdigit(static_cast<unsigned char>(*b)))
                return false;
            val = val * 10 + static_cast<uint64_t>(*b - '0');
            if (val > UINT32_MAX)
                return false;
        }
        ng_id = static_cast<uint32_t>(val);
        return true;
    };

    // Gather cluster state reused by multiple subcommands
    std::unordered_map<uint32_t, std::vector<HostNetworkInfo>> replicas_info;
    redis_impl_->GetReplicaNodesStatus(replicas_info);

    auto all_ngs = txservice::Sharder::Instance().AllNodeGroups();

    auto find_master_info = [&](uint32_t ng_id, HostNetworkInfo &out) -> bool
    {
        auto it = replicas_info.find(ng_id);
        if (it == replicas_info.end())
            return false;
        uint32_t leader_node_id =
            txservice::Sharder::Instance().LeaderNodeId(ng_id);
        for (const auto &n : it->second)
        {
            if (n.node_id == leader_node_id)
            {
                out = n;
                return true;
            }
        }
        return false;
    };

    auto live_nodes_count = [&]() -> uint32_t
    {
        uint32_t cnt = 0;
        for (const auto &kv : replicas_info)
        {
            for (const auto &n : kv.second)
            {
                if (n.status == HostStatus::Online)
                    cnt++;
            }
        }
        return cnt;
    }();

    auto write_kv_array =
        [](brpc::RedisReply *entry,
           const std::vector<std::pair<std::string, std::string>> &kvs)
    {
        entry->SetArray(kvs.size() * 2);
        for (size_t i = 0; i < kvs.size(); ++i)
        {
            (*entry)[i * 2].SetString(kvs[i].first);
            (*entry)[i * 2 + 1].SetString(kvs[i].second);
        }
    };

    // command: sentinel masters
    if (sub == "masters")
    {
        size_t masters_cnt = all_ngs ? all_ngs->size() : 0;
        output->SetArray(masters_cnt);
        size_t idx = 0;
        for (auto ng_id : *all_ngs)
        {
            HostNetworkInfo master{};
            bool ok = find_master_info(ng_id, master);
            // Compose fields per Redis Sentinel spec
            uint32_t num_slaves = 0;
            if (replicas_info.count(ng_id))
            {
                for (const auto &n : replicas_info[ng_id])
                {
                    if (n.node_id != master.node_id &&
                        n.status == HostStatus::Online)
                    {
                        num_slaves++;
                    }
                }
            }
            std::vector<std::pair<std::string, std::string>> kvs;
            kvs.reserve(22);
            kvs.emplace_back("name", std::to_string(ng_id));
            kvs.emplace_back("ip", ok ? master.ip : "");
            kvs.emplace_back("port", ok ? std::to_string(master.port) : "0");
            kvs.emplace_back("runid",
                             ok ? master.host_id() : std::string(40, '0'));
            std::string flags = "master";
            if (!ok || master.status != HostStatus::Online)
                flags += ",disconnected";
            kvs.emplace_back("flags", flags);
            kvs.emplace_back("link-pending-commands", "0");
            kvs.emplace_back("link-refcount", "1");
            kvs.emplace_back("last-ping-sent", "0");
            kvs.emplace_back("last-ok-ping-reply", "0");
            kvs.emplace_back("last-ping-reply", "0");
            kvs.emplace_back("down-after-milliseconds", "0");
            kvs.emplace_back("info-refresh", "0");
            kvs.emplace_back("role-reported", "master");
            kvs.emplace_back("role-reported-time", "0");
            kvs.emplace_back("config-epoch", "0");
            kvs.emplace_back("num-slaves", std::to_string(num_slaves));
            uint32_t other_sentinels =
                live_nodes_count > 0 ? (live_nodes_count - 1) : 0;
            kvs.emplace_back("num-other-sentinels",
                             std::to_string(other_sentinels));
            kvs.emplace_back("quorum", "1");

            write_kv_array(&(*output)[idx], kvs);
            idx++;
        }
        return brpc::REDIS_CMD_HANDLED;
    }

    // command: sentinel master <master-name>
    if (sub == "master")
    {
        if (args.size() < 3)
        {
            output->SetError(
                "ERR wrong number of arguments for 'sentinel master' command");
            return brpc::REDIS_CMD_HANDLED;
        }
        uint32_t ng_id = 0;
        if (!ng_id_from_arg(args[2], ng_id))
        {
            output->SetError("ERR No such master with that name");
            return brpc::REDIS_CMD_HANDLED;
        }
        HostNetworkInfo master{};
        bool ok = find_master_info(ng_id, master);
        if (!ok)
        {
            output->SetError("ERR No such master with that name");
            return brpc::REDIS_CMD_HANDLED;
        }
        std::vector<std::pair<std::string, std::string>> kvs;
        kvs.reserve(22);
        kvs.emplace_back("name", std::to_string(ng_id));
        kvs.emplace_back("ip", ok ? master.ip : "");
        kvs.emplace_back("port", ok ? std::to_string(master.port) : "0");
        kvs.emplace_back("runid", ok ? master.host_id() : std::string(40, '0'));
        std::string flags = "master";
        if (master.status != HostStatus::Online)
            flags += ",disconnected";
        kvs.emplace_back("flags", flags);
        kvs.emplace_back("link-pending-commands", "0");
        kvs.emplace_back("link-refcount", "1");
        kvs.emplace_back("last-ping-sent", "0");
        kvs.emplace_back("last-ok-ping-reply", "0");
        kvs.emplace_back("last-ping-reply", "0");
        kvs.emplace_back("down-after-milliseconds", "0");
        kvs.emplace_back("info-refresh", "0");
        kvs.emplace_back("role-reported", "master");
        kvs.emplace_back("role-reported-time", "0");
        kvs.emplace_back("config-epoch", "0");
        uint32_t num_slaves = 0;
        if (replicas_info.count(ng_id))
        {
            for (const auto &n : replicas_info[ng_id])
            {
                if (ok && n.node_id != master.node_id &&
                    n.status == HostStatus::Online)
                {
                    num_slaves++;
                }
            }
        }
        kvs.emplace_back("num-slaves", std::to_string(num_slaves));
        uint32_t other_sentinels =
            live_nodes_count > 0 ? (live_nodes_count - 1) : 0;
        kvs.emplace_back("num-other-sentinels",
                         std::to_string(other_sentinels));
        kvs.emplace_back("quorum", "1");

        write_kv_array(output, kvs);
        return brpc::REDIS_CMD_HANDLED;
    }

    // command: sentinel replicas <master-name>
    if (sub == "replicas" || sub == "slaves")
    {
        if (args.size() < 3)
        {
            output->SetError(
                "ERR wrong number of arguments for 'sentinel replicas' "
                "command");
            return brpc::REDIS_CMD_HANDLED;
        }
        uint32_t ng_id = 0;
        if (!ng_id_from_arg(args[2], ng_id) || !replicas_info.count(ng_id))
        {
            output->SetError("ERR No such master with that name");
            return brpc::REDIS_CMD_HANDLED;
        }
        HostNetworkInfo master{};
        bool has_master = find_master_info(ng_id, master);

        // Build replicas array
        std::vector<HostNetworkInfo> reps;
        for (const auto &n : replicas_info[ng_id])
        {
            if (!has_master || n.node_id != master.node_id)
            {
                reps.push_back(n);
            }
        }
        output->SetArray(reps.size());
        for (size_t i = 0; i < reps.size(); ++i)
        {
            const auto &r = reps[i];
            std::vector<std::pair<std::string, std::string>> kvs;
            kvs.reserve(20);
            kvs.emplace_back("name", r.host_id());
            kvs.emplace_back("ip", r.ip);
            kvs.emplace_back("port", std::to_string(r.port));
            kvs.emplace_back("runid", r.host_id());
            std::string flags = "slave";
            if (r.status != HostStatus::Online)
                flags += ",disconnected";
            kvs.emplace_back("flags", flags);
            kvs.emplace_back("master-link-status",
                             r.status == HostStatus::Online ? "up" : "down");
            kvs.emplace_back("last-ping-sent", "0");
            kvs.emplace_back("last-ok-ping-reply", "0");
            kvs.emplace_back("last-ping-reply", "0");
            kvs.emplace_back("down-after-milliseconds", "0");
            kvs.emplace_back("info-refresh", "0");
            kvs.emplace_back("replica-priority", "100");
            kvs.emplace_back("replica-repl-offset", "0");
            kvs.emplace_back("link-pending-commands", "0");
            kvs.emplace_back("link-refcount", "1");
            write_kv_array(&(*output)[i], kvs);
        }
        return brpc::REDIS_CMD_HANDLED;
    }

    // command: sentinel sentinels <master-name>
    if (sub == "sentinels")
    {
        if (args.size() < 3)
        {
            output->SetError(
                "ERR wrong number of arguments for 'sentinel sentinels' "
                "command");
            return brpc::REDIS_CMD_HANDLED;
        }
        uint32_t ng_id = 0;
        if (!ng_id_from_arg(args[2], ng_id) || !replicas_info.count(ng_id))
        {
            output->SetError("ERR No such master with that name");
            return brpc::REDIS_CMD_HANDLED;
        }
        // Return all live nodes in the cluster
        // All of the nodes in the cluster can get status of all node groups, so
        // technically they are all sentinels monitoring all node groups.
        std::vector<HostNetworkInfo> sentinels;
        for (const auto &kv : replicas_info)
        {
            for (const auto &n : kv.second)
            {
                if (n.status == HostStatus::Online)
                    sentinels.push_back(n);
            }
        }
        output->SetArray(sentinels.size());
        for (size_t i = 0; i < sentinels.size(); ++i)
        {
            const auto &s = sentinels[i];
            std::vector<std::pair<std::string, std::string>> kvs;
            kvs.reserve(12);
            kvs.emplace_back("name", s.host_id());
            kvs.emplace_back("ip", s.ip);
            kvs.emplace_back("port", std::to_string(s.port));
            kvs.emplace_back("runid", s.host_id());
            kvs.emplace_back("flags", "sentinel");
            kvs.emplace_back("last-ping-sent", "0");
            kvs.emplace_back("last-ping-reply", "0");
            kvs.emplace_back("link-pending-commands", "0");
            kvs.emplace_back("link-refcount", "1");
            write_kv_array(&(*output)[i], kvs);
        }
        return brpc::REDIS_CMD_HANDLED;
    }

    // command: sentinel get-master-addr-by-name <master-name>
    if (sub == "get-master-addr-by-name")
    {
        if (args.size() < 3)
        {
            output->SetError(
                "ERR wrong number of arguments for 'sentinel "
                "get-master-addr-by-name' command");
            return brpc::REDIS_CMD_HANDLED;
        }
        uint32_t ng_id = 0;
        if (!ng_id_from_arg(args[2], ng_id))
        {
            output->SetError("ERR No such master with that name");
            return brpc::REDIS_CMD_HANDLED;
        }
        HostNetworkInfo master{};
        if (!find_master_info(ng_id, master))
        {
            output->SetError("ERR No such master with that name");
            return brpc::REDIS_CMD_HANDLED;
        }
        output->SetArray(2);
        (*output)[0].SetString(master.ip);
        (*output)[1].SetString(std::to_string(master.port));
        return brpc::REDIS_CMD_HANDLED;
    }

    output->SetError("ERR Unknown SENTINEL subcommand");
    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult FailoverCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "failover");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseFailoverCommand(cmd_arg_list, &reply);
    if (success)
    {
        redis_impl_->ExecuteCommand(ctx, cmd.get(), &reply);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ClientCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "client");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseClientCommand(cmd_arg_list, &reply);
    if (success)
    {
        redis_impl_->ExecuteCommand(ctx, cmd.get(), &reply);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult InfoCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "info");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseInfoCommand(cmd_arg_list, &reply);
    if (success)
    {
        redis_impl_->ExecuteCommand(ctx, &cmd, &reply);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult FlushDBCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "flushdb");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    bool success = ParseFlushDBCommand(cmd_arg_list, &reply);
    if (success)
    {
        if (ctx->txm != nullptr)
        {
            output->SetError("ERR FLUSHDB inside a transaction");
            return brpc::REDIS_CMD_HANDLED;
        }
        TransactionExecution *txm =
            redis_impl_->NewTxm(iso_level_, cc_protocol_);
        redis_impl_->ExecuteFlushDBCommand(ctx, txm, &reply, auto_commit_);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult CommandCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "command");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseCommandCommand(cmd_arg_list, &reply);
    if (success)
    {
        redis_impl_->ExecuteCommand(ctx, cmd.get(), &reply);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult FlushALLCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "flushall");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    bool success = ParseFlushALLCommand(cmd_arg_list, &reply);
    if (success)
    {
        if (ctx->txm != nullptr)
        {
            output->SetError("ERR FLUSHALL inside a transaction");
            return brpc::REDIS_CMD_HANDLED;
        }
        redis_impl_->ExecuteFlushALLCommand(
            ctx, &reply, auto_commit_, iso_level_, cc_protocol_);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult GetCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "get");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseGetCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult GetDelCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "getdel");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseGetDelCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult SetCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "set");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseSetCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }
    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult SetNXCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "setnx");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseSetCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult GetSetCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "getset");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseSetCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult StrLenCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "strlen");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseStrLenCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult PSetExCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "psetex");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseSetCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult SetExCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "setex");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseSetCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult GetBitCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "getbit");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseGetBitCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult GetRangeCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "getrange");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseGetRangeCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult SetBitCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "setbit");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseSetBitCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult SetRangeCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "setrange");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseSetRangeCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult BitCountCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "bitcount");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);
    auto [success, key, cmd] = ParseBitCountCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult AppendCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "append");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseAppendCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult BitFieldCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "bitfield");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseBitFieldCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult BitFieldRoCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "bitfield_ro");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseBitFieldRoCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult BitPosCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "bitpos");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseBitPosCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult BitOpCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "bitop");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseBitOpCommand(cmd_arg_list, &reply);

    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult SubStrCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "substr");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseSubStrCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult IncrByFloatCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "incrbyfloat");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseIncrByFloatCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult EchoCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "echo");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseEchoCommand(cmd_arg_list, &reply);
    if (success)
    {
        redis_impl_->ExecuteCommand(ctx, &cmd, &reply);
    }
    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult RPushHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "rpush");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseRPushCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }
    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult HSetHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "hset" || args[0] == "hmset");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseHSetCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult LPushHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "lpush");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseLPushCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult HGetHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "hget");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseHGetCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult LRangeHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "lrange");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseLRangeCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult LPopHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "lpop");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseLPopCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult RPopHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "rpop");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseRPopCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult LMPopHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "lmpop");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseLMPopCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }
    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult MultiHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "multi");
    if (ctx->txm != nullptr)
    {
        output->SetError("ERR MULTI called inside a transaction");
        return brpc::REDIS_CMD_HANDLED;
    }
    output->SetStatus("OK");
    return brpc::REDIS_CMD_CONTINUE;
}

brpc::RedisCommandHandlerResult BeginHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "begin");
    if (ctx->txm != nullptr || ctx->multi_transaction_handler != nullptr)
    {
        output->SetError("ERR BEGIN called in transaction");
        return brpc::REDIS_CMD_HANDLED;
    }
    ctx->txm = redis_impl_->NewTxm(redis_impl_->GetTxnIsolationLevel(),
                                   redis_impl_->GetTxnProtocol());
    output->SetStatus("OK");
    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult CommitHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "commit");
    if (ctx->txm == nullptr)
    {
        output->SetError("ERR COMMIT without BEGIN");
        return brpc::REDIS_CMD_HANDLED;
    }

    auto [success, err_code] = CommitTx(ctx->txm);
    ctx->txm = nullptr;
    if (!success)
    {
        output->SetError("ERR " + txservice::TxErrorMessage(err_code));
        return brpc::REDIS_CMD_HANDLED;
    }
    output->SetStatus("OK");
    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult RollbackHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "rollback");
    if (ctx->txm == nullptr)
    {
        output->SetError("ERR ROLLBACK without BEGIN");
        return brpc::REDIS_CMD_HANDLED;
    }
    AbortTx(ctx->txm);

    ctx->txm = nullptr;
    output->SetStatus("OK");
    return brpc::REDIS_CMD_HANDLED;
}

MultiTransactionHandler::~MultiTransactionHandler()
{
    if (txm_ != nullptr)
    {
        txservice::AbortTx(txm_);
    }
}

brpc::RedisCommandHandlerResult MultiTransactionHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool flush_batched)
{
    if (args[0] == "multi")
    {
        output->SetError("ERR MULTI calls can not be nested");
        return brpc::REDIS_CMD_CONTINUE;
    }
    if (args[0] == "begin")
    {
        output->SetError("ERR BEGIN called in multi transaction");
        return brpc::REDIS_CMD_CONTINUE;
    }
    if (args[0] == "commit")
    {
        output->SetError("ERR COMMIT called in multi transaction");
        return brpc::REDIS_CMD_CONTINUE;
    }
    if (args[0] == "rollback")
    {
        output->SetError("ERR ROLLBACK called in multi transaction");
        return brpc::REDIS_CMD_CONTINUE;
    }
    if (args[0] == "watch")
    {
        if (txn_begun_)
        {
            output->SetError("ERR WATCH in MULTI not supported");
            return brpc::REDIS_CMD_CONTINUE;
        }
        else
        {
            bool success = WatchKeys(ctx, args, output);
            if (!success)
            {
                LOG(WARNING) << "Watch keys fails";
            }
            return brpc::REDIS_CMD_HANDLED;
        }
    }
    else if (args[0] == "unwatch")
    {
        bool success = Unwatch(output);
        if (success)
        {
            output->SetStatus("OK");
        }
        if (txn_begun_)
        {
            return brpc::REDIS_CMD_CONTINUE;
        }
        else
        {
            return brpc::REDIS_CMD_HANDLED;
        }
    }
    else if (args[0] == "discard")
    {
        bool success = Discard(output);
        if (success)
        {
            output->SetStatus("OK");
        }
        return brpc::REDIS_CMD_HANDLED;
    }
    if (args[0] != "exec")
    {
        RedisReplier reply(output);

        std::vector<std::string> raw_cmd_arg;
        raw_cmd_arg.reserve(args.size());
        for (const auto &sp : args)
        {
            raw_cmd_arg.emplace_back(sp.as_string());
        }

        auto [success, tx_req] =
            ParseMultiCommand(redis_impl_, ctx, raw_cmd_arg, &reply, txm_);
        if (!success)
        {
            // The error output must be set.
            error_ = RD_ERR_EXEC_ABORT_FOR_PREV_ERROR;
            return brpc::REDIS_CMD_CONTINUE;
        }
        else
        {
            raw_cmd_args_.emplace_back(std::move(raw_cmd_arg));
            cmd_reqs_.emplace_back(std::move(tx_req));
            output->SetStatus("QUEUED");
            return brpc::REDIS_CMD_CONTINUE;
        }
    }

    if (error_ != RD_OK)
    {
        // Case watched key changed, "error_" was set as RD_NIL and return nil
        // to client too; Case error occurs at previous command parse stage,
        // "error_" was set as RD_ERR_EXEC_ABORT_FOR_PREV_ERROR and return the
        // error to client.
        LOG(INFO) << "error occurs before EXEC, return nil or error";
        if (txm_ != nullptr)
        {
            AbortTxAndOutputError(nullptr);
        }

        if (error_ == RD_NIL)
        {
            output->SetNullString();
        }
        else
        {
            output->SetError(redis_get_error_messages(error_));
        }
    }
    else
    {
        TxErrorCode tx_err_code =
            redis_impl_->MultiExec(cmd_reqs_, raw_cmd_args_, output, txm_, ctx);
        while ((tx_err_code == TxErrorCode::OCC_BREAK_REPEATABLE_READ ||
                tx_err_code == TxErrorCode::WRITE_WRITE_CONFLICT) &&
               redis_impl_->retry_on_occ_error_ && tx_retrieable_)
        {
            txm_ = nullptr;
            output->Reset();
            // reconstruct cmd_reqs_ with raw_cmd_args_
            cmd_reqs_.clear();
            for (const auto &raw_cmd_arg : raw_cmd_args_)
            {
                auto [success, tx_req] = ParseMultiCommand(
                    redis_impl_, ctx, raw_cmd_arg, nullptr, txm_);
                assert(success);
                cmd_reqs_.emplace_back(std::move(tx_req));
            }

            tx_err_code = redis_impl_->MultiExec(
                cmd_reqs_, raw_cmd_args_, output, txm_, ctx);
        }
    }
    txm_ = nullptr;

    return brpc::REDIS_CMD_HANDLED;
}

bool MultiTransactionHandler::Begin()
{
    // TODO(zkl): init txm here
    txn_begun_ = true;
    return txn_begun_;
}

bool MultiTransactionHandler::AbortTxAndOutputError(brpc::RedisReply *output)
{
    assert(txm_ != nullptr);
    txservice::AbortTx(txm_);
    txm_ = nullptr;
    return true;
}

bool MultiTransactionHandler::WatchKeys(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output)
{
    if (args.size() < 2)
    {
        output->SetError("ERR wrong number of arguments for 'watch' command");
        return false;
    }

    std::vector<std::string> raw_cmd_arg;
    raw_cmd_arg.reserve(args.size());
    for (const auto &sp : args)
    {
        raw_cmd_arg.emplace_back(sp.as_string());
    }

    RedisReplier reply(output);

    auto [success, watch_cmd] = ParseWatchCommand(raw_cmd_arg, &reply);
    if (!success)
    {
        return false;
    }

    if (txm_ == nullptr)
    {
        // To be consistent with Redis, Watch keys should use RepeatableRead and
        // OCC or OccRead.
        txm_ = redis_impl_->NewTxm(redis_impl_->txn_isolation_level_,
                                   redis_impl_->txn_protocol_);
    }

    MultiObjectCommandTxRequest tx_req(redis_impl_->RedisTableName(ctx->db_id),
                                       &watch_cmd,
                                       false,
                                       true,
                                       txm_,
                                       true);

    success =
        redis_impl_->ExecuteMultiObjTxRequest(txm_, &tx_req, nullptr, &reply);
    if (!success)
    {
        AbortTx(txm_);

        // Watched key change between to same key WATCH, set error_ to nil.
        error_ = RD_NIL;
        txm_ = nullptr;
    }
    else
    {
        tx_retrieable_ = false;
        output->SetStatus("OK");
    }

    return success;
}

bool MultiTransactionHandler::Unwatch(brpc::RedisReply *output)
{
    if (!txn_begun_ && txm_ != nullptr)
    {
        return AbortTxAndOutputError(output);
    }
    return true;
}

bool MultiTransactionHandler::Discard(brpc::RedisReply *output)
{
    if (!txn_begun_)
    {
        output->SetError("ERR DISCARD without MULTI");
        return false;
    }

    if (txm_ != nullptr)
    {
        return AbortTxAndOutputError(output);
    }

    return true;
}

brpc::RedisCommandHandlerResult DiscardHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "discard");
    if (args.size() > 1)
    {
        output->SetError("ERR wrong number of arguments for 'discard' command");
    }
    else
    {
        output->SetError("ERR DISCARD without MULTI");
    }
    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult EvalHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "eval");
    if (args.size() < 3)
    {
        output->SetError("ERR wrong number of arguments for 'eval' command");
        return brpc::REDIS_CMD_HANDLED;
    }
    if (ctx->txm)
    {
        output->SetError("ERR 'EVAL' command not allowed in BEGIN");
        return brpc::REDIS_CMD_HANDLED;
    }
    if (ctx->multi_transaction_handler)
    {
        output->SetError("ERR 'EVAL' command not allowed in MULTI");
        return brpc::REDIS_CMD_HANDLED;
    }
    redis_impl_->EvalLua(ctx, args, output);

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ScriptHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(args[0] == "script");
    if (args.size() == 1)
    {
        output->SetError("ERR wrong number of arguments for 'script' command");
        return brpc::REDIS_CMD_HANDLED;
    }

    if (strcasecmp(args[1].data(), "flush") == 0)
    {
        if (args.size() > 2)
        {
            std::string msg =
                "ERR unknown command 'redis', with args beginning with:";
            for (size_t i = 2; i < args.size(); ++i)
            {
                msg.append(" '");
                msg.append(args[i].as_string());
                msg.append("'");
            }
            return brpc::REDIS_CMD_HANDLED;
        }
        redis_impl_->ScriptFlush();
        output->SetStatus("OK");
        return brpc::REDIS_CMD_HANDLED;
    }
    if (strcasecmp(args[1].data(), "exists") == 0)
    {
        if (args.size() == 2)
        {
            output->SetError(
                "ERR wrong number of arguments for 'script|exists' command");
            return brpc::REDIS_CMD_HANDLED;
        }
        redis_impl_->ScriptExists(args, output);
        return brpc::REDIS_CMD_HANDLED;
    }
    if (strcasecmp(args[1].data(), "load") == 0)
    {
        if (args.size() == 2)
        {
            output->SetError(
                "ERR wrong number of arguments for 'script|load' command");
            return brpc::REDIS_CMD_HANDLED;
        }
        redis_impl_->ScriptLoad(args, output);
        return brpc::REDIS_CMD_HANDLED;
    }

    output->SetError("ERR unknown subcommand '" + args[1].as_string() + "'.");
    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult EvalshaHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(args[0] == "evalsha");
    if (args.size() <= 2)
    {
        output->SetError("ERR wrong number of arguments for 'evalsha' command");
        return brpc::REDIS_CMD_HANDLED;
    }
    if (ctx->txm)
    {
        output->SetError("ERR 'EVALSHA' command not allowed in BEGIN");
        return brpc::REDIS_CMD_HANDLED;
    }
    if (ctx->multi_transaction_handler)
    {
        output->SetError("ERR 'EVALSHA' command not allowed in MULTI");
        return brpc::REDIS_CMD_HANDLED;
    }

    redis_impl_->Evalsha(ctx, args, output);

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult IncrHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "incr");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseIncrCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult DecrHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "decr");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseDecrCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult IncrByHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "incrby");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseIncrByCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult DecrByHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "decrby");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseDecrByCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult TypeHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/
)
{
    assert(!args.empty() && args[0] == "type");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseTypeCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult DelHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "del");
    if (args.size() < 2)
    {
        output->SetError("ERR wrong number of arguments for 'del' command");
        return brpc::REDIS_CMD_HANDLED;
    }

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseDelCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ZAddHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "zadd");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseZAddCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ZRangeHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "zrange");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseZRangeCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ZRangeStoreHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "zrangestore");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseZRangeStoreCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ZRemHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "zrem");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseZRemCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ZScoreHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "zscore");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseZScoreCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ZMScoreHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "zmscore");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseZMScoreCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ZMPopHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "zmpop");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseZMPopCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ZCountHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "zcount");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseZCountCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ZCardHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "zcard");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseZCardCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ZRangeByScoreHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "zrangebyscore");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseZRangeByScoreCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ZRevRangeHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "zrevrange");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseZRevRangeCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ZRevRangeByLexHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "zrevrangebylex");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseZRevRangeByLexCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ZRevRangeByScoreHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "zrevrangebyscore");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] =
        ParseZRevRangeByScoreCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ZLexCountHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "zlexcount");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseZLexCountCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ZPopMinHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(!args.empty() && args[0] == "zpopmin");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseZPopMinCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ZPopMaxHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "zpopmax");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseZPopMaxCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ZRangeByLexHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "zrangebylex");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseZRangeByLexCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ZRangeByRankHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "zrangebyrank");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseZRangeByRankCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ZRemRangeHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty());

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseZRemRangeCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ZScanHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "zscan");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseZScanCommand(ctx, cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        redis_impl_->ExecuteCommand(ctx,
                                    txm,
                                    redis_impl_->RedisTableName(ctx->db_id),
                                    key,
                                    &cmd,
                                    &reply,
                                    in_tx ? false : auto_commit_);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ZUnionHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "zunion");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseZUnionCommand(cmd_arg_list, &reply);

    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ZUnionStoreHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "zunionstore");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseZUnionStoreCommand(cmd_arg_list, &reply);

    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ZInterHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "zinter");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseZInterCommand(cmd_arg_list, &reply);

    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ZInterCardHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "zintercard");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseZInterCardCommand(cmd_arg_list, &reply);

    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ZInterStoreHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "zinterstore");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseZInterStoreCommand(cmd_arg_list, &reply);

    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ZRandMemberHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "zrandmember");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseZRandMemberCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ZRankHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "zrank");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseZRankCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ZRevRankHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "zrevrank");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseZRevRankCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ZDiffHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "zdiff");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseZDiffCommand(cmd_arg_list, &reply);

    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ZDiffStoreHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "zdiffstore");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseZDiffStoreCommand(cmd_arg_list, &reply);

    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ZIncrByHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "zincrby");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseZIncrByCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult MSetHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "mset");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseMSetCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult MSetNxHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "msetnx");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseMSetNxCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult MGetHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "mget");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseMGetCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult HDelHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "hdel");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseHDelCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult HExistsHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "hexists");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseHExistsCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult HIncrbyHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "hincrby");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseHIncrByCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult HIncrByFloatHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "hincrbyfloat");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseHIncrByFloatCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult HMGetHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "hmget");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseHMGetCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult HKeysHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "hkeys");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseHKeysCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult HValsHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "hvals");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseHValsCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult HRandFieldHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "hrandfield");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseHRandFieldCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult HScanHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "hscan");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseHScanCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult HSetNxHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "hsetnx");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseHSetNxCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult HGetAllHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "hgetall");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseHGetAllCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult HLenHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "hlen");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseHLenCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult HStrLenHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool)
{
    assert(!args.empty() && args[0] == "hstrlen");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseHStrLenCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult LLenHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "llen");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseLLenCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult LTrimHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "ltrim");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseLTrimCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult LIndexHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "lindex");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseLIndexCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult LInsertHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "linsert");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseLInsertCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult LPosHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "lpos");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseLPosCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult LSetHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "lset");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseLSetCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult LMoveHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "lmove");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseLMoveCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult RPopLPushHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "rpoplpush");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseRPopLPushCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult LRemHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "lrem");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseLRemCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult LPushXHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "lpushx");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseLPushXCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult RPushXHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "rpushx");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseRPushXCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult BLMoveHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "blmove");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, mcmd] = ParseBLMoveCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            mcmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, &mcmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult BLMPopHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "blmpop");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, mcmd] = ParseBLMPopCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            mcmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, &mcmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult BLPopHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "blpop");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, mcmd] = ParseBLPopCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            mcmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, &mcmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult BRPopHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "brpop");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, mcmd] = ParseBRPopCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            mcmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, &mcmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult BRPopLPushHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "brpoplpush");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, mcmd] = ParseBRPopLPushCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            mcmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, &mcmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult SAddHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "sadd");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseSAddCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult SMembersHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "smembers");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseSMembersCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult SRemHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "srem");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseSRemCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult SCardHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "scard");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseSCardCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult SDiffHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "sdiff");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseSDiffCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult SDiffStoreHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "sdiffstore");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseSDiffStoreCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult SInterHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "sinter");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseSInterCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult SInterStoreHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "sinterstore");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseSInterStoreCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult SInterCardHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "sintercard");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseSInterCardCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult SIsMemberHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "sismember");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseSIsMemberCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult SMIsMemberHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "smismember");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseSMIsMemberCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult SMoveHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "smove");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseSMoveCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult SUnionHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "sunion");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [succeess, cmd] = ParseSUnionCommand(cmd_arg_list, &reply);
    if (succeess)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult SUnionStoreHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "sunionstore");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseSUnionStoreCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult SRandMemberHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "srandmember");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseSRandMemberCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult SPopHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "spop");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseSPopCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult SScanHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "sscan");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseSScanCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ExistsHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "exists");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseExistsCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult SortHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "sort" || args[0] == "sort_ro");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseSortCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        redis_impl_->ExecuteCommand(
            ctx, txm, &cmd, &reply, in_tx ? false : auto_commit_);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ScanHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "scan");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);
    auto [success, cmd] = ParseScanCommand(ctx, cmd_arg_list, &reply);

    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        const TableName *table = redis_impl_->RedisTableName(ctx->db_id);
        redis_impl_->ExecuteCommand(
            ctx, txm, table, &cmd, &reply, in_tx ? false : auto_commit_);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult KeysHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "keys");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseKeysCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        auto *table = redis_impl_->RedisTableName(ctx->db_id);
        redis_impl_->ExecuteCommand(
            ctx, txm, table, &cmd, &reply, in_tx ? false : auto_commit_);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult DumpHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/
)
{
    assert(args[0] == "dump");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseDumpCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult RestoreHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/
)
{
    assert(args[0] == "restore");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseRestoreCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult SubscribeHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/
)
{
    assert(args[0] == "subscribe");
    if (args.size() == 1)
    {
        output->SetError(
            "ERR wrong number of arguments for 'subscribe' command");
        return brpc::RedisCommandHandlerResult::REDIS_CMD_HANDLED;
    }

    std::vector<std::string_view> chans;
    chans.reserve(args.size() - 1);
    for (size_t i = 1; i < args.size(); i++)
    {
        chans.emplace_back(args[i].data(), args[i].size());
    }

    redis_impl_->Subscribe(chans, ctx);
    return brpc::RedisCommandHandlerResult::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult UnsubscribeHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/
)
{
    assert(args[0] == "unsubscribe");
    std::vector<std::string_view> chans;
    for (size_t i = 1; i < args.size(); i++)
    {
        chans.emplace_back(args[i].data(), args[i].size());
    }

    redis_impl_->Unsubscribe(chans, ctx);
    return brpc::RedisCommandHandlerResult::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult PSubscribeHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/
)
{
    assert(args[0] == "psubscribe");
    if (args.size() == 1)
    {
        output->SetError(
            "ERR wrong number of arguments for 'psubscribe' command");
        return brpc::RedisCommandHandlerResult::REDIS_CMD_HANDLED;
    }

    std::vector<std::string_view> patterns;
    patterns.reserve(args.size() - 1);
    for (size_t i = 1; i < args.size(); i++)
    {
        patterns.emplace_back(args[i].data(), args[i].size());
    }

    redis_impl_->PSubscribe(patterns, ctx);
    return brpc::RedisCommandHandlerResult::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult PUnsubscribeHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "punsubscribe");
    std::vector<std::string_view> patterns;
    for (size_t i = 1; i < args.size(); i++)
    {
        patterns.emplace_back(args[i].data(), args[i].size());
    }

    redis_impl_->PUnsubscribe(patterns, ctx);
    return brpc::RedisCommandHandlerResult::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult PublishHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/
)
{
    assert(args[0] == "publish");
    if (args.size() != 3)
    {
        output->SetError("ERR wrong number of arguments for 'publish' command");
        return brpc::RedisCommandHandlerResult::REDIS_CMD_HANDLED;
    }
    std::string_view chan(args[1].data(), args[1].size());
    std::string_view message(args[2].data(), args[2].size());

    DLOG(INFO) << "**** client: " << ctx << " publish into channel: " << chan
               << " message: " << message;

    int received = redis_impl_->Publish(chan, message);
    output->SetInteger(received);
    return brpc::RedisCommandHandlerResult::REDIS_CMD_HANDLED;
}
#ifdef WITH_FAULT_INJECT
brpc::RedisCommandHandlerResult FaultInjectHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "fault_inject");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseFaultInjectCommand(cmd_arg_list, &reply);

    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        redis_impl_->ExecuteCommand(
            ctx, txm, &cmd, &reply, in_tx ? false : auto_commit_);
    }

    return brpc::REDIS_CMD_HANDLED;
}
#endif

brpc::RedisCommandHandlerResult MemoryHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "memory");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);
    assert(cmd_arg_list.size() > 1);

    if (stringcomp(cmd_arg_list[1], "usage", 1))
    {
        auto [success, key, cmd] =
            ParseMemoryUsageCommand(cmd_arg_list, &reply);

        if (success)
        {
            bool in_tx = ctx->txm != nullptr;
            TransactionExecution *txm =
                in_tx ? ctx->txm
                      : redis_impl_->NewTxm(iso_level_, cc_protocol_);
            if (in_tx)
            {
                // If not committed in place, a write command might be buffered
                // on the cce so we need to mark it as volatile.
                cmd.SetVolatile();
            }
            redis_impl_->ExecuteCommand(ctx,
                                        txm,
                                        key,
                                        &cmd,
                                        &reply,
                                        in_tx ? false : auto_commit_,
                                        in_tx);
        }
    }
    else
    {
        std::string err_msg = "ERR unknown subcommand '";
        err_msg.append(cmd_arg_list[1]).append("'");
        reply.OnError(err_msg);
    }
    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ExpireCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "expire");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] =
        ParseExpireCommand(cmd_arg_list, &reply, false, false);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }
    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult PExpireCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "pexpire");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] =
        ParseExpireCommand(cmd_arg_list, &reply, true, false);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }
    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ExpireAtCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "expireat");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] =
        ParseExpireCommand(cmd_arg_list, &reply, false, true);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }
    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult PExpireAtCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "pexpireat");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] =
        ParseExpireCommand(cmd_arg_list, &reply, true, true);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }
    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult TTLCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "ttl");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] =
        ParseTTLCommand(cmd_arg_list, &reply, false, false, false);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }
    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult PTTLCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "pttl");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] =
        ParseTTLCommand(cmd_arg_list, &reply, true, false, false);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }
    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult ExpireTimeCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "expiretime");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] =
        ParseTTLCommand(cmd_arg_list, &reply, false, true, false);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }
    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult PExpireTimeCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "pexpiretime");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] =
        ParseTTLCommand(cmd_arg_list, &reply, false, false, true);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }
    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult PersistCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "persist");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParsePersistCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }
    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult GetExCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "getex");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, key, cmd] = ParseGetExCommand(cmd_arg_list, &reply);
    if (success)
    {
        bool in_tx = ctx->txm != nullptr;
        TransactionExecution *txm =
            in_tx ? ctx->txm : redis_impl_->NewTxm(iso_level_, cc_protocol_);
        if (in_tx)
        {
            // If not committed in place, a write command might be buffered on
            // the cce so we need to mark it as volatile.
            cmd.SetVolatile();
        }
        redis_impl_->ExecuteCommand(
            ctx, txm, key, &cmd, &reply, in_tx ? false : auto_commit_, in_tx);
    }
    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult TimeCommandHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "time");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);

    auto [success, cmd] = ParseTimeCommand(cmd_arg_list, &reply);
    if (success)
    {
        redis_impl_->ExecuteCommand(ctx, &cmd, &reply);
    }
    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult CreateVecIndexHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "eloqvec.create" || args[0] == "ELOQVEC.CREATE");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);
    auto [success, cmd] = ParseCreateVecIndexCommand(cmd_arg_list, &reply);

    if (success)
    {
        if (ctx->txm != nullptr)
        {
            reply.OnError("ERR vector command is not supported in transaction");
            return brpc::REDIS_CMD_HANDLED;
        }

        redis_impl_->ExecuteCommand(ctx, &cmd, &reply);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult InfoVecIndexHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "eloqvec.info" || args[0] == "ELOQVEC.INFO");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);
    auto [success, cmd] = ParseInfoVecIndexCommand(cmd_arg_list, &reply);

    if (success)
    {
        if (ctx->txm != nullptr)
        {
            reply.OnError("ERR vector command is not supported in transaction");
            return brpc::REDIS_CMD_HANDLED;
        }
        // txm should not be bound to core since it is used in vector index
        // worker pool
        TransactionExecution *txm = redis_impl_->GetTxService()->NewTx();
        txm->InitTx(iso_level_, cc_protocol_);
        redis_impl_->ExecuteCommand(ctx, txm, &cmd, &reply, true);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult DropVecIndexHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "eloqvec.drop" || args[0] == "ELOQVEC.DROP");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);
    auto [success, cmd] = ParseDropVecIndexCommand(cmd_arg_list, &reply);

    if (success)
    {
        if (ctx->txm != nullptr)
        {
            reply.OnError("ERR vector command is not supported in transaction");
            return brpc::REDIS_CMD_HANDLED;
        }
        redis_impl_->ExecuteCommand(ctx, &cmd, &reply);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult AddVecIndexHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "eloqvec.add" || args[0] == "ELOQVEC.ADD");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);
    auto [success, cmd] = ParseAddVecIndexCommand(cmd_arg_list, &reply);

    if (success)
    {
        if (ctx->txm != nullptr)
        {
            reply.OnError("ERR vector command is not supported in transaction");
            return brpc::REDIS_CMD_HANDLED;
        }
        TransactionExecution *txm = redis_impl_->GetTxService()->NewTx();
        txm->InitTx(iso_level_, cc_protocol_);
        redis_impl_->ExecuteCommand(ctx, txm, &cmd, &reply, true);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult UpdateVecIndexHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "eloqvec.update" || args[0] == "ELOQVEC.UPDATE");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);
    auto [success, cmd] = ParseUpdateVecIndexCommand(cmd_arg_list, &reply);

    if (success)
    {
        if (ctx->txm != nullptr)
        {
            reply.OnError("ERR vector command is not supported in transaction");
            return brpc::REDIS_CMD_HANDLED;
        }
        // txm should not be bound to core since it is used in vector index
        // worker pool
        TransactionExecution *txm = redis_impl_->GetTxService()->NewTx();
        txm->InitTx(iso_level_, cc_protocol_);
        redis_impl_->ExecuteCommand(ctx, txm, &cmd, &reply, true);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult DeleteVecIndexHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "eloqvec.delete" || args[0] == "ELOQVEC.DELETE");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);
    auto [success, cmd] = ParseDeleteVecIndexCommand(cmd_arg_list, &reply);

    if (success)
    {
        if (ctx->txm != nullptr)
        {
            reply.OnError("ERR vector command is not supported in transaction");
            return brpc::REDIS_CMD_HANDLED;
        }
        // txm should not be bound to core since it is used in vector index
        // worker pool
        TransactionExecution *txm = redis_impl_->GetTxService()->NewTx();
        txm->InitTx(iso_level_, cc_protocol_);
        redis_impl_->ExecuteCommand(ctx, txm, &cmd, &reply, true);
    }

    return brpc::REDIS_CMD_HANDLED;
}

brpc::RedisCommandHandlerResult SearchVecIndexHandler::Run(
    RedisConnectionContext *ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool /*flush_batched*/)
{
    assert(args[0] == "eloqvec.search" || args[0] == "ELOQVEC.SEARCH");

    RedisReplier reply(output);
    std::vector<std::string_view> cmd_arg_list = Transform(args);
    auto [success, cmd] = ParseSearchVecIndexCommand(cmd_arg_list, &reply);

    if (success)
    {
        if (ctx->txm != nullptr)
        {
            reply.OnError("ERR vector command is not supported in transaction");
            return brpc::REDIS_CMD_HANDLED;
        }
        // txm should not be bound to core since it is used in vector index
        // worker pool
        TransactionExecution *txm = redis_impl_->GetTxService()->NewTx();
        txm->InitTx(iso_level_, cc_protocol_);
        redis_impl_->ExecuteCommand(ctx, txm, &cmd, &reply, true);
    }

    return brpc::REDIS_CMD_HANDLED;
}

}  // namespace EloqKV
