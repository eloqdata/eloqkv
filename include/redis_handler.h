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
#pragma once

#include <brpc/redis_reply.h>
#include <butil/strings/string_piece.h>

#include <string>
#include <variant>
#include <vector>

#include "eloq_key.h"
#include "redis_command.h"
#include "tx_request.h"

namespace EloqKV
{
class RedisServiceImpl;
class RedisConnectionContext;

class RedisCommandHandler
{
public:
    virtual ~RedisCommandHandler() = default;

    static void Initialize(txservice::IsolationLevel iso_level,
                           txservice::CcProtocol cc_protocol)
    {
        iso_level_ = iso_level;
        cc_protocol_ = cc_protocol;
    }

    // Once Server receives commands, it will first find the corresponding
    // handlers and call them sequentially(one by one) according to the order
    // that requests arrive, just like what redis-server does. `args' is the
    // array of request command. For example, "set somekey somevalue"
    // corresponds to args[0]=="set", args[1]=="somekey" and
    // args[2]=="somevalue". `output', which should be filled by user, is the
    // content that sent to client side. Read brpc/src/redis_reply.h for more
    // usage. `flush_batched' indicates whether the user should flush all the
    // results of batched commands. If user want to do some batch processing,
    // user should buffer the commands and return REDIS_CMD_BATCHED. Once
    // `flush_batched' is true, run all the commands, set `output' to be an
    // array in which every element is the result of batched commands and return
    // REDIS_CMD_HANDLED.
    //
    // The return value should be REDIS_CMD_HANDLED for normal cases. If you
    // want to implement transaction, return REDIS_CMD_CONTINUE once server
    // receives an start marker and brpc will call MultiTransactionHandler() to
    // new a transaction handler that all the following commands are sent to
    // this tranction handler until it returns REDIS_CMD_HANDLED. Read the
    // comment below.
    virtual brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool flush_batched) = 0;

public:
    static std::vector<std::string_view> Transform(
        const std::vector<butil::StringPiece> &args)
    {
        std::vector<std::string_view> cmd_arg_list;
        cmd_arg_list.reserve(args.size());
        for (const butil::StringPiece &arg : args)
        {
            cmd_arg_list.emplace_back(arg.data(), arg.size());
        }
        return cmd_arg_list;
    }

public:
    inline static txservice::IsolationLevel iso_level_ =
        txservice::IsolationLevel::ReadCommitted;
    inline static txservice::CcProtocol cc_protocol_ =
        txservice::CcProtocol::OccRead;
    static constexpr bool auto_commit_{true};
};

class PingCommandHandler : public RedisCommandHandler
{
public:
    explicit PingCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class AuthCommandHandler : public RedisCommandHandler
{
public:
    explicit AuthCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class SelectCommandHandler : public RedisCommandHandler
{
public:
    explicit SelectCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ConfigCommandHandler : public RedisCommandHandler
{
public:
    explicit ConfigCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class DBSizeCommandHandler : public RedisCommandHandler
{
public:
    explicit DBSizeCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class SlowLogCommandHandler : public RedisCommandHandler
{
public:
    explicit SlowLogCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ReadOnlyCommandHandler : public RedisCommandHandler
{
public:
    explicit ReadOnlyCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class InfoCommandHandler : public RedisCommandHandler
{
public:
    explicit InfoCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class CommandCommandHandler : public RedisCommandHandler
{
public:
    explicit CommandCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ClusterCommandHandler : public RedisCommandHandler
{
public:
    explicit ClusterCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class SentinelCommandHandler : public RedisCommandHandler
{
public:
    explicit SentinelCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class FailoverCommandHandler : public RedisCommandHandler
{
public:
    explicit FailoverCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ClientCommandHandler : public RedisCommandHandler
{
public:
    explicit ClientCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class GetCommandHandler : public RedisCommandHandler
{
public:
    explicit GetCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class GetDelCommandHandler : public RedisCommandHandler
{
public:
    explicit GetDelCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class SetCommandHandler : public RedisCommandHandler
{
public:
    explicit SetCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class SetNXCommandHandler : public RedisCommandHandler
{
public:
    explicit SetNXCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class GetSetCommandHandler : public RedisCommandHandler
{
public:
    explicit GetSetCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class StrLenCommandHandler : public RedisCommandHandler
{
public:
    explicit StrLenCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class PSetExCommandHandler : public RedisCommandHandler
{
public:
    explicit PSetExCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class SetExCommandHandler : public RedisCommandHandler
{
public:
    explicit SetExCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class GetBitCommandHandler : public RedisCommandHandler
{
public:
    explicit GetBitCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class GetRangeCommandHandler : public RedisCommandHandler
{
public:
    explicit GetRangeCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class SetBitCommandHandler : public RedisCommandHandler
{
public:
    explicit SetBitCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class SetRangeCommandHandler : public RedisCommandHandler
{
public:
    explicit SetRangeCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class BitCountCommandHandler : public RedisCommandHandler
{
public:
    explicit BitCountCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class AppendCommandHandler : public RedisCommandHandler
{
public:
    explicit AppendCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class BitFieldCommandHandler : public RedisCommandHandler
{
public:
    explicit BitFieldCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class BitFieldRoCommandHandler : public RedisCommandHandler
{
public:
    explicit BitFieldRoCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class BitPosCommandHandler : public RedisCommandHandler
{
public:
    explicit BitPosCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class BitOpCommandHandler : public RedisCommandHandler
{
public:
    explicit BitOpCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

struct SubStrCommandHandler : public RedisCommandHandler
{
public:
    explicit SubStrCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class IncrByFloatCommandHandler : public RedisCommandHandler
{
public:
    explicit IncrByFloatCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class EchoCommandHandler : public RedisCommandHandler
{
public:
    explicit EchoCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class RPushHandler : public RedisCommandHandler
{
public:
    explicit RPushHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class LPushHandler : public RedisCommandHandler
{
public:
    explicit LPushHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class LPopHandler : public RedisCommandHandler
{
public:
    explicit LPopHandler(RedisServiceImpl *redis_impl) : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class HSetHandler : public RedisCommandHandler
{
public:
    explicit HSetHandler(RedisServiceImpl *redis_impl) : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class HGetHandler : public RedisCommandHandler
{
public:
    explicit HGetHandler(RedisServiceImpl *redis_impl) : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class RPopHandler : public RedisCommandHandler
{
public:
    explicit RPopHandler(RedisServiceImpl *redis_impl) : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class LMPopHandler : public RedisCommandHandler
{
public:
    explicit LMPopHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class LRangeHandler : public RedisCommandHandler
{
public:
    explicit LRangeHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class MultiTransactionHandler
{
public:
    explicit MultiTransactionHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    explicit MultiTransactionHandler(const RedisServiceImpl *redis_impl)
        : redis_impl_(const_cast<RedisServiceImpl *>(redis_impl))
    {
    }

    ~MultiTransactionHandler();

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool flush_batched);

    size_t PendingRequestNum() const
    {
        return cmd_reqs_.size();
    }

    bool Begin();

private:
    bool AbortTxAndOutputError(brpc::RedisReply *output);

    bool WatchKeys(RedisConnectionContext *ctx,
                   const std::vector<butil::StringPiece> &args,
                   brpc::RedisReply *output);

    bool Unwatch(brpc::RedisReply *output);

    bool Discard(brpc::RedisReply *output);

    std::vector<std::vector<std::string>> raw_cmd_args_;
    std::vector<std::variant<DirectRequest,
                             txservice::ObjectCommandTxRequest,
                             txservice::MultiObjectCommandTxRequest,
                             CustomCommandRequest>>
        cmd_reqs_;
    RedisServiceImpl *redis_impl_;
    txservice::TransactionExecution *txm_{};
    bool txn_begun_{};
    int error_{RD_OK};
    // Whether the transaction is retrieable on occ break repeatable read error.
    // This is set to false if the transaction has watched any keys.
    bool tx_retrieable_{true};
};

class BeginHandler : public RedisCommandHandler
{
public:
    explicit BeginHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool flush_batched) override;

private:
    RedisServiceImpl *redis_impl_;
};

class CommitHandler : public RedisCommandHandler
{
public:
    explicit CommitHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class RollbackHandler : public RedisCommandHandler
{
public:
    explicit RollbackHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class MultiHandler : public RedisCommandHandler
{
public:
    explicit MultiHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class DiscardHandler : public RedisCommandHandler
{
public:
    explicit DiscardHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class EvalHandler : public RedisCommandHandler
{
public:
    explicit EvalHandler(RedisServiceImpl *redis_impl) : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ScriptHandler : public RedisCommandHandler
{
public:
    explicit ScriptHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class EvalshaHandler : public RedisCommandHandler
{
public:
    explicit EvalshaHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class IncrHandler : public RedisCommandHandler
{
public:
    explicit IncrHandler(RedisServiceImpl *redis_impl) : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class DecrHandler : public RedisCommandHandler
{
public:
    explicit DecrHandler(RedisServiceImpl *redis_impl) : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class IncrByHandler : public RedisCommandHandler
{
public:
    explicit IncrByHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class DecrByHandler : public RedisCommandHandler
{
public:
    explicit DecrByHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class TypeHandler : public RedisCommandHandler
{
public:
    explicit TypeHandler(RedisServiceImpl *redis_impl) : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class DelHandler : public RedisCommandHandler
{
public:
    explicit DelHandler(RedisServiceImpl *redis_impl) : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ZAddHandler : public RedisCommandHandler
{
public:
    explicit ZAddHandler(RedisServiceImpl *redis_impl) : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ZRangeHandler : public RedisCommandHandler
{
public:
    explicit ZRangeHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ZRangeStoreHandler : public RedisCommandHandler
{
public:
    explicit ZRangeStoreHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ZCountHandler : public RedisCommandHandler
{
public:
    explicit ZCountHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    ~ZCountHandler() override = default;

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ZCardHandler : public RedisCommandHandler
{
public:
    explicit ZCardHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    ~ZCardHandler() override = default;

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;  //
};

class ZRemHandler : public RedisCommandHandler
{
public:
    explicit ZRemHandler(RedisServiceImpl *redis_impl) : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ZScoreHandler : public RedisCommandHandler
{
public:
    explicit ZScoreHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ZMScoreHandler : public RedisCommandHandler
{
public:
    explicit ZMScoreHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ZMPopHandler : public RedisCommandHandler
{
public:
    explicit ZMPopHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ZLexCountHandler : public RedisCommandHandler
{
public:
    explicit ZLexCountHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ZPopMinHandler : public RedisCommandHandler
{
public:
    explicit ZPopMinHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ZPopMaxHandler : public RedisCommandHandler
{
public:
    explicit ZPopMaxHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ZRangeByScoreHandler : public RedisCommandHandler
{
public:
    explicit ZRangeByScoreHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ZRangeByLexHandler : public RedisCommandHandler
{
public:
    explicit ZRangeByLexHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ZRevRangeHandler : public RedisCommandHandler
{
public:
    explicit ZRevRangeHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ZRevRangeByScoreHandler : public RedisCommandHandler
{
public:
    explicit ZRevRangeByScoreHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ZRevRangeByLexHandler : public RedisCommandHandler
{
public:
    explicit ZRevRangeByLexHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ZRangeByRankHandler : public RedisCommandHandler
{
public:
    explicit ZRangeByRankHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ZRemRangeHandler : public RedisCommandHandler
{
public:
    explicit ZRemRangeHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ZScanHandler : public RedisCommandHandler
{
public:
    explicit ZScanHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ZUnionHandler : public RedisCommandHandler
{
public:
    explicit ZUnionHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ZUnionStoreHandler : public RedisCommandHandler
{
public:
    explicit ZUnionStoreHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ZInterHandler : public RedisCommandHandler
{
public:
    explicit ZInterHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

struct ZInterCardHandler : public RedisCommandHandler
{
public:
    explicit ZInterCardHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

struct ZInterStoreHandler : public RedisCommandHandler
{
public:
    explicit ZInterStoreHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ZRandMemberHandler : public RedisCommandHandler
{
public:
    explicit ZRandMemberHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ZRankHandler : public RedisCommandHandler
{
public:
    explicit ZRankHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ZRevRankHandler : public RedisCommandHandler
{
public:
    explicit ZRevRankHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ZDiffHandler : public RedisCommandHandler
{
public:
    explicit ZDiffHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ZDiffStoreHandler : public RedisCommandHandler
{
public:
    explicit ZDiffStoreHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ZIncrByHandler : public RedisCommandHandler
{
public:
    explicit ZIncrByHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class LLenHandler : public RedisCommandHandler
{
public:
    explicit LLenHandler(RedisServiceImpl *redis_impl) : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class LTrimHandler : public RedisCommandHandler
{
public:
    explicit LTrimHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class LIndexHandler : public RedisCommandHandler
{
public:
    explicit LIndexHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class LInsertHandler : public RedisCommandHandler
{
public:
    explicit LInsertHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class LPosHandler : public RedisCommandHandler
{
public:
    explicit LPosHandler(RedisServiceImpl *redis_impl) : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class LSetHandler : public RedisCommandHandler
{
public:
    explicit LSetHandler(RedisServiceImpl *redis_impl) : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class LMoveHandler : public RedisCommandHandler
{
public:
    explicit LMoveHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class RPopLPushHandler : public RedisCommandHandler
{
public:
    explicit RPopLPushHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class LRemHandler : public RedisCommandHandler
{
public:
    explicit LRemHandler(RedisServiceImpl *redis_impl) : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class LPushXHandler : public RedisCommandHandler
{
public:
    explicit LPushXHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class RPushXHandler : public RedisCommandHandler
{
public:
    explicit RPushXHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class BLMoveHandler : public RedisCommandHandler
{
public:
    explicit BLMoveHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class BLMPopHandler : public RedisCommandHandler
{
public:
    explicit BLMPopHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class BLPopHandler : public RedisCommandHandler
{
public:
    explicit BLPopHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class BRPopHandler : public RedisCommandHandler
{
public:
    explicit BRPopHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class BRPopLPushHandler : public RedisCommandHandler
{
public:
    explicit BRPopLPushHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class MSetHandler : public RedisCommandHandler
{
public:
    explicit MSetHandler(RedisServiceImpl *redis_impl) : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class MSetNxHandler : public RedisCommandHandler
{
public:
    explicit MSetNxHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class MGetHandler : public RedisCommandHandler
{
public:
    explicit MGetHandler(RedisServiceImpl *redis_impl) : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class HDelHandler : public RedisCommandHandler
{
public:
    explicit HDelHandler(RedisServiceImpl *redis_impl) : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class HExistsHandler : public RedisCommandHandler
{
public:
    explicit HExistsHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class HIncrbyHandler : public RedisCommandHandler
{
public:
    explicit HIncrbyHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class HIncrByFloatHandler : public RedisCommandHandler
{
public:
    explicit HIncrByFloatHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class HMGetHandler : public RedisCommandHandler
{
public:
    explicit HMGetHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class HKeysHandler : public RedisCommandHandler
{
public:
    explicit HKeysHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class HValsHandler : public RedisCommandHandler
{
public:
    explicit HValsHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class HRandFieldHandler : public RedisCommandHandler
{
public:
    explicit HRandFieldHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class HScanHandler : public RedisCommandHandler
{
public:
    explicit HScanHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class HSetNxHandler : public RedisCommandHandler
{
public:
    explicit HSetNxHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class HGetAllHandler : public RedisCommandHandler
{
public:
    explicit HGetAllHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class HLenHandler : public RedisCommandHandler
{
public:
    explicit HLenHandler(RedisServiceImpl *redis_impl) : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class HStrLenHandler : public RedisCommandHandler
{
public:
    explicit HStrLenHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class SAddHandler : public RedisCommandHandler
{
public:
    explicit SAddHandler(RedisServiceImpl *redis_impl) : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class SMembersHandler : public RedisCommandHandler
{
public:
    explicit SMembersHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class SRemHandler : public RedisCommandHandler
{
public:
    explicit SRemHandler(RedisServiceImpl *redis_impl) : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class SCardHandler : public RedisCommandHandler
{
public:
    explicit SCardHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class SDiffHandler : public RedisCommandHandler
{
public:
    explicit SDiffHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class SDiffStoreHandler : public RedisCommandHandler
{
public:
    explicit SDiffStoreHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class SInterHandler : public RedisCommandHandler
{
public:
    explicit SInterHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class SInterStoreHandler : public RedisCommandHandler
{
public:
    explicit SInterStoreHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class SInterCardHandler : public RedisCommandHandler
{
public:
    explicit SInterCardHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class SIsMemberHandler : public RedisCommandHandler
{
public:
    explicit SIsMemberHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class SMIsMemberHandler : public RedisCommandHandler
{
public:
    explicit SMIsMemberHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class SMoveHandler : public RedisCommandHandler
{
public:
    explicit SMoveHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class SUnionHandler : public RedisCommandHandler
{
public:
    explicit SUnionHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class SUnionStoreHandler : public RedisCommandHandler
{
public:
    explicit SUnionStoreHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class SRandMemberHandler : public RedisCommandHandler
{
public:
    explicit SRandMemberHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class SPopHandler : public RedisCommandHandler
{
public:
    explicit SPopHandler(RedisServiceImpl *redis_impl) : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class SScanHandler : public RedisCommandHandler
{
public:
    explicit SScanHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ExistsHandler : public RedisCommandHandler
{
public:
    explicit ExistsHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};
class FlushDBCommandHandler : public RedisCommandHandler
{
public:
    explicit FlushDBCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class FlushALLCommandHandler : public RedisCommandHandler
{
public:
    explicit FlushALLCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class SortHandler : public RedisCommandHandler
{
public:
    explicit SortHandler(RedisServiceImpl *redis_impl) : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ScanHandler : public RedisCommandHandler
{
public:
    explicit ScanHandler(RedisServiceImpl *redis_impl) : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class KeysHandler : public RedisCommandHandler
{
public:
    explicit KeysHandler(RedisServiceImpl *redis_impl) : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class DumpHandler : public RedisCommandHandler
{
public:
    explicit DumpHandler(RedisServiceImpl *redis_impl) : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class CreateVecIndexHandler : public RedisCommandHandler
{
public:
    explicit CreateVecIndexHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class InfoVecIndexHandler : public RedisCommandHandler
{
public:
    explicit InfoVecIndexHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class SubscribeHandler : public RedisCommandHandler
{
public:
    explicit SubscribeHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class UnsubscribeHandler : public RedisCommandHandler
{
public:
    explicit UnsubscribeHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class PSubscribeHandler : public RedisCommandHandler
{
public:
    explicit PSubscribeHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class PUnsubscribeHandler : public RedisCommandHandler
{
public:
    explicit PUnsubscribeHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class PublishHandler : public RedisCommandHandler
{
public:
    explicit PublishHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class RestoreHandler : public RedisCommandHandler
{
public:
    explicit RestoreHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

#ifdef WITH_FAULT_INJECT
class FaultInjectHandler : public RedisCommandHandler
{
public:
    explicit FaultInjectHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};
#endif

class MemoryHandler : public RedisCommandHandler
{
public:
    explicit MemoryHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ExpireCommandHandler : public RedisCommandHandler
{
public:
    explicit ExpireCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class PExpireCommandHandler : public RedisCommandHandler
{
public:
    explicit PExpireCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ExpireAtCommandHandler : public RedisCommandHandler
{
public:
    explicit ExpireAtCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class PExpireAtCommandHandler : public RedisCommandHandler
{
public:
    explicit PExpireAtCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class TTLCommandHandler : public RedisCommandHandler
{
public:
    explicit TTLCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class PTTLCommandHandler : public RedisCommandHandler
{
public:
    explicit PTTLCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class ExpireTimeCommandHandler : public RedisCommandHandler
{
public:
    explicit ExpireTimeCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class PExpireTimeCommandHandler : public RedisCommandHandler
{
public:
    explicit PExpireTimeCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class PersistCommandHandler : public RedisCommandHandler
{
public:
    explicit PersistCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class GetExCommandHandler : public RedisCommandHandler
{
public:
    explicit GetExCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

class TimeCommandHandler : public RedisCommandHandler
{
public:
    explicit TimeCommandHandler(RedisServiceImpl *redis_impl)
        : redis_impl_(redis_impl)
    {
    }

    brpc::RedisCommandHandlerResult Run(
        RedisConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool /*flush_batched*/) override;

private:
    RedisServiceImpl *redis_impl_;
};

}  // namespace EloqKV
