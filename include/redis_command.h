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

#include <absl/types/span.h>
#include <brpc/redis_reply.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "eloq_key.h"
#include "eloq_string.h"
#include "local_cc_shards.h"
#include "output_handler.h"
#include "redis_errors.h"
#include "redis_object.h"
#include "redis_replier.h"
#include "tx_command.h"
#include "tx_key.h"
#include "tx_request.h"
#include "vector_index.h"

/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#define OBJ_SET_NX (1 << 0)     // Set if key not exists.
#define OBJ_SET_XX (1 << 1)     // Set if key exists.
#define OBJ_SET_SETNX (1 << 2)  // Set if command is setnx.
#define OBJ_GET_SET (1 << 3)    // Get old value and return before set newvalue
#define OBJ_SET_EX (1 << 4)     // Set expired time for SET or GET commands.
#define OBJ_SET_KEEPTTL (1 << 4)  // Keep expired time for SET or GET commands.

#define CONFIG_SET (1 << 0)  // Set if config set.
#define CONFIG_GET (1 << 1)  // Set if config get.

#define SLOWLOG_GET (1 << 0)
#define SLOWLOG_RESET (1 << 1)
#define SLOWLOG_LEN (1 << 2)

namespace EloqKV
{
enum struct RedisCommandType
{
    NOOP = 0,

    // Connection management
    ECHO,
    PING,
    AUTH,
    QUIT,
    SELECT,
    CLIENT,
    UNKNOWN,

    // string commands
    GET,
    SET,
    GETDEL,
    SETNX,
    GETSET,
    MGET,
    MSET,
    MSETNX,
    STRLEN,
    PSETEX,
    SETEX,
    GETBIT,
    GETRANGE,
    INCRBYFLOAT,
    SETBIT,
    SETRANGE,
    APPEND,
    SUBSTR,
    INCR,
    INCRBY,
    DECR,
    DECRBY,
    BITCOUNT,
    BITFIELD,
    BITFIELD_RO,
    BITPOS,
    BITOP,

    // list commands
    BLOCKPOP,
    BLOCKDISCARD,
    BLMOVE,
    BLMPOP,
    BLPOP,
    BRPOP,
    BRPOPLPUSH,
    LINDEX,
    LINSERT,
    LPOS,
    LLEN,
    LPOP,
    LPUSH,
    LPUSHX,
    LRANGE,
    LMOVE,
    LMOVEPOP,
    LMOVEPUSH,
    LREM,
    LSET,
    LTRIM,

    RPOP,
    RPOPLPUSH,
    RPUSH,
    RPUSHX,
    LMPOP,

    // hash commands
    HGET,
    HSET,
    HSETNX,
    HLEN,
    HSTRLEN,
    HINCRBY,
    HINCRBYFLOAT,
    HMGET,
    HKEYS,
    HVALS,
    HGETALL,
    HEXISTS,
    HDEL,
    HRANDFIELD,
    HSCAN,

    // sorted set commands
    ZADD,
    ZCOUNT,
    ZCARD,
    ZRANGE,
    ZRANGESTORE,
    ZREM,
    ZSCORE,
    ZRANGEBYSCORE,
    ZRANGEBYLEX,
    ZRANGEBYRANK,
    ZREMRANGE,
    ZLEXCOUNT,
    ZPOPMIN,
    ZPOPMAX,
    ZSCAN,
    ZUNION,
    ZUNIONSTORE,
    ZINTER,
    ZINTERCARD,
    ZINTERSTORE,
    ZRANDMEMBER,
    ZRANK,
    ZREVRANK,
    ZREVRANGE,
    ZREVRANGEBYSCORE,
    ZREVRANGEBYLEX,
    ZMSCORE,
    ZMPOP,
    ZDIFF,
    ZDIFFSTORE,
    ZINCRBY,
    SZSCAN,

    // set commands
    SADD,
    SMEMBERS,
    SREM,
    SCARD,
    SDIFF,
    SDIFFSTORE,
    SINTER,
    SINTERCARD,
    SINTERSTORE,
    SISMEMBER,
    SMISMEMBER,
    SMOVE,
    SPOP,
    SRANDMEMBER,
    SUNION,
    SUNIONSTORE,
    SSCAN,

    STORE_LIST,
    SORTABLE_LOAD,
    MHGET,
    SORT,

    // transaction commands
    WATCH,
    UNWATCH,
    MULTI,
    EXEC,
    DISCARD,
    BEGIN,
    COMMIT,
    ROLLBACK,

    // generic commands
    SCAN,
    KEYS,
    TYPE,
    DEL,
    EXISTS,
    DUMP,
    RESTORE,

    // server managment commands
    DBSIZE,
    INFO,
    COMMAND,
    CONFIG,
    CLUSTER,
    FAILOVER,
    FLUSHDB,
    FLUSHALL,
    READONLY,
    SLOWLOG,

    // scripting and functions commands
    EVAL,
    EVALSHA,
#ifdef WITH_FAULT_INJECT
    // debug commands
    FAULT_INJECT,
#endif

    MEMORY_USAGE,

    // ttl commands
    TTL,
    PTTL,
    EXPIRETIME,
    PEXPIRETIME,
    EXPIRE,
    PEXPIRE,
    EXPIREAT,
    PEXPIREAT,
    PERSIST,
    GETEX,
    RECOVER,
    TIME,

    // pubsub commands
    PUBLISH,
    SUBSCRIBE,
    UNSUBSCRIBE,
    PSUBSCRIBE,
    PUNSUBSCRIBE,

    // vector index commands
    ELOQVEC_CREATE,
    ELOQVEC_INFO,
};

enum RedisResultType
{
    StringResult,
    IntResult,
    DoubleResult,
    ListResult,
    ZsetResult,
    HashResult,
    HashSetResult,
    ScanResult,
    SortableLoadResult,
    SortResult,
    SetScanResult
};

enum ResultType
{
    String,
    Double,
    Int,
    Int64_t,
    StringVector,
    Int64Vector,
    NilableStringVector,
    StringDoubleVector,
    EloqStringType,
    PairInt32Str,
};

extern const std::vector<std::pair<const char *, RedisCommandType>>
    command_types;
extern const std::array<std::pair<unsigned long long, const char *>, 25>
    command_flags;
extern const std::array<std::pair<unsigned long long, const char *>, 22>
    acl_catorgories;
extern const std::array<std::pair<unsigned long long, const char *>, 12>
    doc_flag_names;
RedisCommandType CommandType(std::string_view cmd);

struct HostNetworkInfo;
struct SlotInfo;
class RedisServiceImpl;
class RedisConnectionContext;
struct BucketScanCursor;
struct SlowLogEntry
{
    SlowLogEntry() = default;
    SlowLogEntry(const SlowLogEntry &rhs)
        : id_(rhs.id_),
          timestamp_(rhs.timestamp_),
          execution_time_(rhs.execution_time_),
          cmd_(rhs.cmd_),
          client_addr_(rhs.client_addr_),
          client_name_(rhs.client_name_)
    {
    }

    SlowLogEntry(SlowLogEntry &&rhs)
        : id_(rhs.id_),
          timestamp_(rhs.timestamp_),
          execution_time_(rhs.execution_time_),
          cmd_(std::move(rhs.cmd_)),
          client_addr_(rhs.client_addr_),
          client_name_(rhs.client_name_)
    {
    }

    SlowLogEntry &operator=(SlowLogEntry &&rhs)
    {
        id_ = rhs.id_;
        timestamp_ = rhs.timestamp_;
        execution_time_ = rhs.execution_time_;
        cmd_ = std::move(rhs.cmd_);
        client_addr_ = rhs.client_addr_;
        client_name_ = rhs.client_name_;
        return *this;
    }

    uint64_t id_{0};
    uint64_t timestamp_{0};
    uint32_t execution_time_{0};
    std::vector<std::string> cmd_;
    butil::EndPoint client_addr_;
    std::string client_name_;
};

struct RedisCommandResult : public txservice::TxCommandResult
{
    RedisCommandResult() = default;
    explicit RedisCommandResult(int32_t err_code) : err_code_(err_code)
    {
    }
    int32_t err_code_{RD_NIL};
    void Serialize(std::string &buf) const;
    void Deserialize(const char *buf, size_t &offset);
};

struct RedisStringResult : public RedisCommandResult
{
    void Serialize(std::string &buf) const override;
    void Deserialize(const char *buf, size_t &offset) override;
    std::string str_;
    int64_t int_ret_{0};
    // For BitField, if value is overflow and overflow type is fail, set
    // bool=false, result output NIL, or set bool=true, result output int64_t
    std::vector<std::pair<bool, int64_t>> vct_int_;
};

struct RedisIntResult : public RedisCommandResult
{
    void Serialize(std::string &buf) const override;
    void Deserialize(const char *buf, size_t &offset) override;
    int64_t int_val_{0};
};

struct RedisListResult : public RedisCommandResult
{
    void Serialize(std::string &buf) const override;
    void Deserialize(const char *buf, size_t &offset) override;

    std::variant<std::vector<int64_t>,
                 std::vector<std::string>,
                 std::string,
                 EloqString>
        result_;
    int64_t ret_{0};
};

struct RedisSetScanResult : public RedisCommandResult
{
    void Serialize(std::string &buf) const override;
    void Deserialize(const char *buf, size_t &offset) override;

    std::variant<std::vector<EloqString>,
                 std::vector<std::pair<EloqString, double>>>
        result_;
};

struct RedisZsetResult : public RedisCommandResult
{
    void Serialize(std::string &buf) const override;
    void Deserialize(const char *buf, size_t &offset) override;

    std::variant<EloqString,
                 double,
                 int,
                 std::vector<EloqString>,
                 std::pair<int32_t, std::string>>
        result_;
};

struct RedisHashResult : public RedisCommandResult
{
    void Serialize(std::string &buf) const override;
    void Deserialize(const char *buf, size_t &offset) override;

    std::variant<int64_t,
                 std::string,
                 std::vector<std::string>,
                 std::vector<std::optional<std::string>>>
        result_;
};

struct RedisHashSetResult : public RedisCommandResult
{
    void Serialize(std::string &buf) const override;
    void Deserialize(const char *buf, size_t &offset) override;

    std::vector<EloqString> string_list_;
    int32_t ret_{0};
};

struct RedisScanResult : public RedisCommandResult
{
    void Serialize(std::string &buf) const override
    {
        abort();
    }
    void Deserialize(const char *buf, size_t &offset) override
    {
        abort();
    }

    uint64_t cursor_id_{0};
    std::vector<std::string> vct_key_;  // All key return to client
};

// Result of SortableLoadCommand
struct RedisSortableLoadResult : public RedisCommandResult
{
    void Serialize(std::string &buf) const override;
    void Deserialize(const char *buf, size_t &offset) override;

    RedisObjectType obj_type_;
    std::vector<std::string> elems_;
};

struct RedisSortResult : public RedisCommandResult
{
    RedisSortResult() = default;
    explicit RedisSortResult(int32_t err_code) : RedisCommandResult(err_code)
    {
    }

    void Serialize(std::string &buf) const override;
    void Deserialize(const char *buf, size_t &offset) override;

    std::vector<std::optional<std::string>> result_;
};

struct RedisCommand : public txservice::TxCommand
{
    std::unique_ptr<TxCommand> Clone() override
    {
        // Read only commands need no Clone.
        return nullptr;
    }
    bool IsVolatile() override
    {
        return is_volatile_;
    }
    void SetVolatile() override
    {
        is_volatile_ = true;
    }

    std::unique_ptr<TxCommand> RetireExpiredTTLObjectCommand() const override;

    // OutputResult takes an abstract OutputHandler*, as the result could
    // be output to lua state or brpc::RedisReplay
    virtual void OutputResult(OutputHandler *reply) const = 0;
    // TRUE: This cmd will expire after execute, if need to commit, it should be
    // clone. FALSE: This cmd will exist until commited.
    bool is_volatile_{false};
};

struct RedisMultiObjectCommand : public txservice::MultiObjectTxCommand
{
    // OutputResult takes an abstract OutputHandler*, as the result could
    // be output to lua state or brpc::RedisReplay
    virtual void OutputResult(OutputHandler *reply) const = 0;

    std::vector<txservice::TxKey> *KeyPointers() override
    {
        return &vct_key_ptrs_[curr_step_];
    }

    std::vector<txservice::TxKey> *KeyPointers(size_t step) override
    {
        return &vct_key_ptrs_[step];
    }

    std::vector<txservice::TxCommand *> *CommandPointers() override
    {
        return &vct_cmd_ptrs_[curr_step_];
    }

    bool IsFinished() override
    {
        return curr_step_ >= vct_key_ptrs_.size();
    }

    bool IsLastStep() override
    {
        return (curr_step_ + 1) == vct_key_ptrs_.size();
    }

    size_t CmdSteps() override
    {
        return vct_key_ptrs_.size();
    }

    void IncrSteps() override
    {
        curr_step_++;
    }

    void SetVolatile()
    {
        for (auto &vct_cmd : vct_cmd_ptrs_)
        {
            for (auto &cmd : vct_cmd)
            {
                cmd->SetVolatile();
            }
        }
    }

    // The MultiObjectTxCommand can be split multi steps to run. Only the
    // previous step finished or failed, it can go to next step and stop the
    // entire command according to the result of HandleMiddleResult.
    // vct_key_ptrs_ and vct_cmd_ptrs_ should have the same size and one element
    // shouble be one step in them.
    std::vector<std::vector<txservice::TxKey>> vct_key_ptrs_;
    std::vector<std::vector<txservice::TxCommand *>> vct_cmd_ptrs_;
    // The current step, according it to get keys and commands.
    size_t curr_step_{0};
};

/**
 * DirectCommand is a command that can be executed directly on the brpc,
 * it will not invoke tx_service.
 */
struct DirectCommand
{
    virtual ~DirectCommand() = default;
    virtual void Execute(RedisServiceImpl *redis_impl,
                         RedisConnectionContext *ctx) = 0;
    virtual void OutputResult(OutputHandler *reply) const = 0;
};

/**
 * DirectRequest is a request holding a DirectCommand.
 */
struct DirectRequest
{
    DirectRequest() = default;
    DirectRequest(RedisConnectionContext *ctx,
                  std::unique_ptr<DirectCommand> cmd)
        : ctx_(ctx), cmd_(std::move(cmd))
    {
    }

    void Execute(RedisServiceImpl *redis_impl_);

    void OutputResult(OutputHandler *reply) const;

    RedisConnectionContext *ctx_;

    std::unique_ptr<DirectCommand> cmd_;

    bool operator<(const DirectRequest &r1) const
    {
        return false;
    }
};

/**
 * CustomCommand is the kind of commands that have complex execution
 * process and should implement its execution instead of just call
 * "RedisServiceImpl::ExecuteCommand()" directly.
 *
 */
struct CustomCommand
{
    virtual ~CustomCommand() = default;
    virtual bool Execute(RedisServiceImpl *redis_impl,
                         RedisConnectionContext *ctx,
                         const txservice::TableName *table,
                         txservice::TransactionExecution *txm,
                         OutputHandler *output,
                         bool auto_commit) = 0;
    virtual void OutputResult(OutputHandler *reply,
                              RedisConnectionContext *ctx) const = 0;
};

/**
 * CustomCommandRequest is a request holding a CustomCommand.
 */
struct CustomCommandRequest
{
    CustomCommandRequest() = default;
    CustomCommandRequest(const txservice::TableName *tbl,
                         std::unique_ptr<CustomCommand> cmd)
        : table_(tbl), cmd_(std::move(cmd))
    {
    }

    bool Execute(RedisServiceImpl *redis_impl,
                 RedisConnectionContext *ctx,
                 txservice::TransactionExecution *txm,
                 OutputHandler *output,
                 bool auto_commit);

    const txservice::TableName *table_;
    std::unique_ptr<CustomCommand> cmd_;

    bool operator<(const CustomCommandRequest &r1) const
    {
        return false;
    }
};

struct EchoCommand : public DirectCommand
{
    EchoCommand() = default;
    explicit EchoCommand(std::string_view value_view) : value_(value_view)
    {
    }

    void Execute(RedisServiceImpl *redis_impl,
                 RedisConnectionContext *ctx) override;

    void OutputResult(OutputHandler *reply) const override;

    EloqString value_;
};

struct PingCommand : public DirectCommand
{
    PingCommand() = default;
    explicit PingCommand(bool empty, std::string_view value_view)
        : empty_(empty), reply_value_(value_view)
    {
    }

    void Execute(RedisServiceImpl *redis_impl,
                 RedisConnectionContext *ctx) override;

    void OutputResult(OutputHandler *reply) const override;

    bool empty_{};

    EloqString reply_value_;
};

struct AuthCommand : public DirectCommand
{
    AuthCommand() = default;
    AuthCommand(std::string_view username, std::string_view password)
        : username_(username), password_(password)
    {
    }

    void Execute(RedisServiceImpl *redis_impl,
                 RedisConnectionContext *ctx) override;
    void OutputResult(OutputHandler *reply) const override;

    std::string_view username_;
    std::string_view password_;

    RedisCommandResult result_;
};

struct SelectCommand : public DirectCommand
{
    SelectCommand() = default;
    explicit SelectCommand(int db_id) : db_id_(db_id)
    {
    }

    void Execute(RedisServiceImpl *redis_impl,
                 RedisConnectionContext *ctx) override;
    void OutputResult(OutputHandler *reply) const override;

    int db_id_;

    RedisCommandResult result_;
};

struct InfoCommand : public DirectCommand
{
    InfoCommand() = default;
    explicit InfoCommand(std::unordered_set<std::string> &&set_section)
        : set_section_(std::move(set_section))
    {
    }

    void Execute(RedisServiceImpl *redis_impl,
                 RedisConnectionContext *ctx) override;

    void OutputResult(OutputHandler *reply) const override;

    std::unordered_set<std::string> set_section_;
    std::string config_file_;
    uint64_t uptime_in_secs_{0};
    uint32_t node_id_{0};
    uint32_t node_count_{0};
    uint32_t tcp_port_{0};
    uint32_t core_num_{0};
    std::string enable_data_store_;
    std::string enable_wal_;
    uint32_t node_memory_limit_mb_{0};
    uint32_t node_log_limit_mb_{0};

    int64_t data_memory_allocated_{0};
    int64_t data_memory_committed_{0};
    uint64_t last_ckpt_ts_{0};
    int event_dispatcher_num_{0};
    const char *version_{nullptr};

    int64_t conn_received_count_{0};
    int64_t conn_rejected_count_{0};
    int64_t connecting_count_{0};
    uint64_t max_connection_count_{0};
    int64_t blocked_clients_count_{0};
    int64_t cmd_read_count_{0};
    int64_t cmd_write_count_{0};
    int64_t multi_cmd_count_{0};
    int64_t cmds_per_sec_{0};
    double cmd_latency_{0};
    std::vector<int64_t> dbsizes_;
};

struct CommandCommand : public DirectCommand
{
    CommandCommand() = default;

    void Execute(RedisServiceImpl *redis_impl,
                 RedisConnectionContext *ctx) override;

    void OutputResult(OutputHandler *reply) const override;
};

struct CommandInfoCommand : public CommandCommand
{
    CommandInfoCommand() = default;

    void Execute(RedisServiceImpl *redis_impl,
                 RedisConnectionContext *ctx) override;

    void OutputResult(OutputHandler *reply) const override;

    std::vector<std::string> command_names_;
};

struct CommandCountCommand : public CommandCommand
{
    CommandCountCommand() = default;

    void Execute(RedisServiceImpl *redis_impl,
                 RedisConnectionContext *ctx) override;

    void OutputResult(OutputHandler *reply) const override;
};

struct CommandListCommand : public CommandCommand
{
    CommandListCommand() = default;

    void Execute(RedisServiceImpl *redis_impl,
                 RedisConnectionContext *ctx) override;

    void OutputResult(OutputHandler *reply) const override;
};

struct ClusterCommand : public DirectCommand
{
    ClusterCommand() = default;

    void Execute(RedisServiceImpl *redis_impl,
                 RedisConnectionContext *ctx) override;

    void OutputResult(OutputHandler *reply) const override;
};

struct ClusterInfoCommand : public ClusterCommand
{
    ClusterInfoCommand() = default;

    void Execute(RedisServiceImpl *redis_impl,
                 RedisConnectionContext *ctx) override;

    void OutputResult(OutputHandler *reply) const override;

    std::vector<std::string> result_;
};

struct ClusterNodesCommand : public ClusterCommand
{
    ClusterNodesCommand() = default;

    void Execute(RedisServiceImpl *redis_impl,
                 RedisConnectionContext *ctx) override;

    void OutputResult(OutputHandler *reply) const override;

    // The output of the command is just a space-separated CSV string, where
    // each line represents a node in the cluster.
    // Each line is composed of the following fields:
    //    <id> <ip:port@cport[,hostname]> <flags> <master> <ping-sent>
    //    <pong-recv> <config-epoch> <link-state> <slot> <slot> ... <slot>
    std::vector<std::string> result_;
};

struct ClusterSlotsCommand : public ClusterCommand
{
    ClusterSlotsCommand() = default;

    void Execute(RedisServiceImpl *redis_impl,
                 RedisConnectionContext *ctx) override;

    void OutputResult(OutputHandler *reply) const override;

    // The output of the command is just a space-separated CSV string, where
    // each line represents a node in the cluster.
    // Each line is composed of the following fields:
    //    <id> <ip:port@cport[,hostname]> <flags> <master> <ping-sent>
    //    <pong-recv> <config-epoch> <link-state> <slot> <slot> ... <slot>
    std::vector<SlotInfo> result_;
};

struct ClusterKeySlotCommand : public ClusterCommand
{
    ClusterKeySlotCommand() = default;
    explicit ClusterKeySlotCommand(std::string_view key_str)
        : key_(key_str), result_(-1)
    {
    }

    void Execute(RedisServiceImpl *redis_impl,
                 RedisConnectionContext *ctx) override;

    void OutputResult(OutputHandler *reply) const override;

    EloqKey key_;

    int32_t result_;
};

struct FailoverCommand : public DirectCommand
{
    FailoverCommand() = default;
    explicit FailoverCommand(std::string target_host, uint16_t target_port)
        : target_host_(target_host), target_port_(target_port)
    {
    }

    void Execute(RedisServiceImpl *redis_impl,
                 RedisConnectionContext *ctx) override;

    void OutputResult(OutputHandler *reply) const override;

    std::string target_host_;
    uint16_t target_port_{0};
    bool failover_succeed_{false};
    std::string error_message_;
};

struct ClientCommand : public DirectCommand
{
    /**
     * Detail output: https://redis.io/commands/client-list/
     */
    class ClientInfo
    {
    public:
        ClientInfo() = default;
        ClientInfo(const brpc::Socket *socket,
                   const RedisConnectionContext *ctx);
        std::string ToString() const;

        friend std::ostream &operator<<(std::ostream &out,
                                        const ClientInfo &client);

    private:
        int64_t id_{0};
        butil::EndPoint addr_;
        butil::EndPoint laddr_;
        int fd_{0};
        std::string_view name_;
        int64_t age_{0};
        int64_t idle_{0};
        std::string flags_;  // unset yet
        int db_{0};
        int64_t sub_{0};               // unset yet
        int64_t psub_{0};              // unset yet
        int64_t ssub_{0};              // unset yet
        int64_t multi_{0};             // unset yet
        int64_t watch_{0};             // unset yet
        uint64_t qbuf_{0};             // unset yet
        uint64_t qbuf_free_{0};        // unset yet
        int64_t argv_mem_{0};          // unset yet
        int64_t multi_mem_{0};         // unset yet
        uint64_t obl_{0};              // unset yet
        uint64_t oll_{0};              // unset yet
        uint64_t omem_{0};             // unset yet
        uint64_t tot_mem_{0};          // unset yet
        uint64_t events_{0};           // unset yet
        std::string cmd_;              // unset yet
        std::string user_{"default"};  // unset yet
        uint64_t redir_;               // unset yet
        std::string resp_;             // unset yet
        std::string lib_name_;
        std::string lib_ver_;
    };

    ClientCommand() = default;

    void Execute(RedisServiceImpl *redis_impl,
                 RedisConnectionContext *ctx) override;

    void OutputResult(OutputHandler *reply) const override;
};

struct ClientSetNameCommand : public ClientCommand
{
    ClientSetNameCommand() = default;

    explicit ClientSetNameCommand(std::string_view connection_name)
        : connection_name_(connection_name)
    {
    }

    void Execute(RedisServiceImpl *redis_impl,
                 RedisConnectionContext *ctx) override;

    void OutputResult(OutputHandler *reply) const override;

    std::string_view connection_name_;
};

struct ClientGetNameCommand : public ClientCommand
{
    ClientGetNameCommand() = default;

    void Execute(RedisServiceImpl *redis_impl,
                 RedisConnectionContext *ctx) override;

    void OutputResult(OutputHandler *reply) const override;

    std::string_view connection_name_;
};

struct ClientSetInfoCommand : public ClientCommand
{
    enum struct Attribute
    {
        LIB_NAME,
        LIB_VER,
    };

    ClientSetInfoCommand() = default;

    ClientSetInfoCommand(Attribute attribute, std::string_view value)
        : attribute_(attribute), value_(value)
    {
    }

    void Execute(RedisServiceImpl *redis_impl,
                 RedisConnectionContext *ctx) override;

    void OutputResult(OutputHandler *reply) const override;

    Attribute attribute_;
    std::string_view value_;
};

struct ClientIdCommand : public ClientCommand
{
    ClientIdCommand() = default;

    void Execute(RedisServiceImpl *redis_impl,
                 RedisConnectionContext *ctx) override;

    void OutputResult(OutputHandler *reply) const override;

    int64_t client_id_;
};

struct ClientInfoCommand : public ClientCommand
{
    ClientInfoCommand() = default;

    void Execute(RedisServiceImpl *redis_impl,
                 RedisConnectionContext *ctx) override;

    void OutputResult(OutputHandler *reply) const override;

    std::optional<ClientInfo> client_;
};

struct ClientListCommand : public ClientCommand
{
    enum struct Type
    {
        NORMAL,
        MASTER,
        SLAVE,
        PUBSUB,
    };

    ClientListCommand() = default;

    explicit ClientListCommand(Type type) : type_(type)
    {
    }

    explicit ClientListCommand(std::vector<int64_t> client_id_vec)
        : client_id_vec_(std::move(client_id_vec))
    {
    }

    void Execute(RedisServiceImpl *redis_impl,
                 RedisConnectionContext *ctx) override;

    void OutputResult(OutputHandler *reply) const override;

    static bool GetTypeByName(const char *name, Type *type);

    std::optional<Type> type_;
    std::optional<std::vector<int64_t>> client_id_vec_;

    std::vector<ClientInfo> client_info_vec_;
};

struct ClientKillCommand : public ClientCommand
{
    ClientKillCommand() = default;

    explicit ClientKillCommand(butil::EndPoint addr)
        : old_style_syntax_(true), addr_(std::move(addr))
    {
    }

    void Execute(RedisServiceImpl *redis_impl,
                 RedisConnectionContext *ctx) override;

    void OutputResult(OutputHandler *reply) const override;

    bool old_style_syntax_;
    std::optional<butil::EndPoint> addr_;

    int64_t killed_{0};
};

struct DBSizeCommand : public DirectCommand
{
    DBSizeCommand() = default;

    void Execute(RedisServiceImpl *redis_impl,
                 RedisConnectionContext *ctx) override;

    static std::vector<int64_t> FetchDBSize(
        std::vector<txservice::TableName> table_names);

    void OutputResult(OutputHandler *reply) const override;
    int64_t total_db_size_{0};
};

struct ReadOnlyCommand : public DirectCommand
{
    ReadOnlyCommand() = default;

    void Execute(RedisServiceImpl *redis_impl,
                 RedisConnectionContext *ctx) override;

    void OutputResult(OutputHandler *reply) const override;
};

struct ConfigCommand : public DirectCommand
{
    ConfigCommand() = default;

    explicit ConfigCommand(std::vector<std::string_view> keys)
        : flag_(CONFIG_GET), keys_(keys)
    {
    }

    explicit ConfigCommand(std::vector<std::string_view> keys,
                           std::vector<std::string_view> values)
        : flag_(CONFIG_SET), keys_(keys), values_(values)
    {
    }

    void Execute(RedisServiceImpl *redis_impl,
                 RedisConnectionContext *ctx) override;

    void OutputResult(OutputHandler *reply) const override;

    int8_t flag_{0};
    std::vector<std::string_view> keys_;
    std::vector<std::string_view> values_;
    std::vector<std::string> results_;
};

struct TimeCommand : public DirectCommand
{
    TimeCommand() = default;

    void Execute(RedisServiceImpl *redis_impl,
                 RedisConnectionContext *ctx) override
    {
        time_ = txservice::LocalCcShards::ClockTs();
    }

    void OutputResult(OutputHandler *reply) const override
    {
        reply->OnArrayStart(2);
        uint64_t seconds = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::microseconds(time_))
                               .count();
        reply->OnString(std::to_string(seconds));
        reply->OnString(std::to_string(time_ % 1000000));
        reply->OnArrayEnd();
    }

    uint64_t time_;
};

struct SlowLogCommand : public DirectCommand
{
    SlowLogCommand() = default;
    explicit SlowLogCommand(int8_t flag, int count)
        : flag_(flag), count_(count), len_(0)
    {
    }

    void Execute(RedisServiceImpl *redis_impl,
                 RedisConnectionContext *ctx) override;

    void OutputResult(OutputHandler *reply) const override;

    int8_t flag_{0};
    int count_;
    std::list<SlowLogEntry> results_;
    uint32_t len_;
};

struct PublishCommand : public DirectCommand
{
    PublishCommand() = default;
    PublishCommand(std::string_view chan, std::string_view msg)
        : chan_(chan), message_(msg) {};
    void Execute(RedisServiceImpl *redis_impl,
                 RedisConnectionContext *ctx) override;

    void OutputResult(OutputHandler *reply) const override;

    std::string_view chan_{};
    std::string_view message_{};
    int received_{};
};

struct StringCommand : public RedisCommand
{
    std::unique_ptr<txservice::TxRecord> CreateObject(
        const std::string *image) const override;

    std::unique_ptr<txservice::TxCommandResult> CreateCommandResult()
        const override
    {
        return std::make_unique<RedisStringResult>();
    }

    txservice::TxCommandResult *GetResult() override
    {
        return &result_;
    }

    static bool CheckTypeMatch(const txservice::TxRecord &obj);
    RedisStringResult result_;
};

struct SetCommand : public StringCommand
{
    SetCommand() = default;
    explicit SetCommand(std::string_view value_view) : value_(value_view)
    {
    }

    explicit SetCommand(std::string_view value_view,
                        int flag,
                        uint64_t obj_expire_ts = UINT64_MAX)
        : value_(value_view), flag_(flag), obj_expire_ts_(obj_expire_ts)
    {
    }

    /**
     * Deep copy the command arguments.
     * @param rhs
     */
    SetCommand(const SetCommand &rhs);
    SetCommand(SetCommand &&rhs) = default;
    ~SetCommand() override = default;
    SetCommand &operator=(const SetCommand &rhs) = delete;
    SetCommand &operator=(SetCommand &&rhs) = default;

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<SetCommand>(*this);
    }

    bool IsReadOnly() const override
    {
        return false;
    }

    bool ProceedOnNonExistentObject() const override
    {
        if (flag_ & OBJ_SET_XX)
        {
            return false;
        }
        return true;
    }

    bool ProceedOnExistentObject() const override
    {
        if (flag_ & OBJ_SET_NX && !(flag_ & OBJ_GET_SET))
        {
            return false;
        }
        return true;
    }

    bool IsOverwrite() const override
    {
        return true;
    }

    bool IgnoreOldValue() const override
    {
        if (flag_ & OBJ_SET_XX || flag_ & OBJ_SET_NX || flag_ & OBJ_GET_SET ||
            flag_ & OBJ_SET_KEEPTTL)
        {
            return false;
        }
        return true;
    }

    bool WillSetTTL() const override
    {
        if (obj_expire_ts_ < UINT64_MAX)
        {
            return true;
        }

        return false;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *obj_ptr) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    EloqString value_;
    int flag_{0};
    // The expired time for this object. Value UINT64_MAX means this object will
    // not expire for ever.
    uint64_t obj_expire_ts_{UINT64_MAX};
};

struct GetCommand : public StringCommand
{
    GetCommand() = default;

    GetCommand(const GetCommand &rhs) = delete;
    GetCommand(GetCommand &&rhs) = default;
    ~GetCommand() override = default;
    GetCommand &operator=(const GetCommand &rhs) = delete;
    GetCommand &operator=(GetCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void OutputResult(OutputHandler *reply) const override;

    void Serialize(std::string &str) const override
    {
        uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::GET);
        str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
    }

    void Deserialize(std::string_view cmd_img) override
    {
        // Not need to do anything
    }
};

struct GetDelCommand : public StringCommand
{
    GetDelCommand() = default;

    GetDelCommand(const GetDelCommand &rhs) = default;
    GetDelCommand(GetDelCommand &&rhs) = default;
    ~GetDelCommand() override = default;
    GetDelCommand &operator=(const GetDelCommand &rhs) = delete;
    GetDelCommand &operator=(GetDelCommand &&rhs) = delete;

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<GetDelCommand>(*this);
    }

    bool IsReadOnly() const override
    {
        return false;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *obj_ptr) override;

    void OutputResult(OutputHandler *reply) const override;

    void Serialize(std::string &str) const override
    {
        uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::GETDEL);
        str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
    }

    void Deserialize(std::string_view cmd_img) override
    {
        // Not need to do anything
    }
};

struct StrLenCommand : public StringCommand
{
    StrLenCommand() = default;

    StrLenCommand(const StrLenCommand &rhs) = delete;
    StrLenCommand(StrLenCommand &&rhs) = default;
    ~StrLenCommand() override = default;
    StrLenCommand &operator=(const StrLenCommand &rhs) = delete;
    StrLenCommand &operator=(StrLenCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void OutputResult(OutputHandler *reply) const override;

    void Serialize(std::string &str) const override
    {
        uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::STRLEN);
        str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
    }

    void Deserialize(std::string_view cmd_img) override
    {
        // Not need to do anything
    }
};

struct GetBitCommand : public StringCommand
{
    GetBitCommand() = default;
    explicit GetBitCommand(int64_t offset) : offset_(offset)
    {
    }

    GetBitCommand(const GetBitCommand &rhs) = delete;
    GetBitCommand(GetBitCommand &&rhs) = default;
    ~GetBitCommand() override = default;
    GetBitCommand &operator=(const GetBitCommand &rhs) = delete;
    GetBitCommand &operator=(GetBitCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void OutputResult(OutputHandler *reply) const override;

    void Serialize(std::string &str) const override;
    void Deserialize(std::string_view cmd_img) override;

    int64_t offset_;
};

struct GetRangeCommand : public StringCommand
{
    GetRangeCommand() = default;
    explicit GetRangeCommand(int64_t start, int64_t end)
        : start_(start), end_(end)
    {
    }

    GetRangeCommand(const GetRangeCommand &rhs) = delete;
    GetRangeCommand(GetRangeCommand &&rhs) = default;
    ~GetRangeCommand() override = default;
    GetRangeCommand &operator=(const GetRangeCommand &rhs) = delete;
    GetRangeCommand &operator=(GetRangeCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void OutputResult(OutputHandler *reply) const override;

    void Serialize(std::string &str) const override;
    void Deserialize(std::string_view cmd_img) override;

    int64_t start_;
    int64_t end_;
};

struct BitCountCommand : public StringCommand
{
    enum class OffsetType
    {
        BIT = 0,
        BYTE
    };

    BitCountCommand() = default;

    BitCountCommand(int64_t start, int64_t end, OffsetType type)
        : start_(start), end_(end), type_(type)
    {
    }

    BitCountCommand(const BitCountCommand &) = delete;
    BitCountCommand &operator=(const BitCountCommand &) = delete;
    BitCountCommand(BitCountCommand &&) = default;
    BitCountCommand &operator=(BitCountCommand &&) = delete;
    ~BitCountCommand() override = default;

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void OutputResult(OutputHandler *reply) const override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_img) override;

    bool IsOffsetByBit() const
    {
        return type_ == OffsetType::BIT;
    }

    int64_t start_;
    int64_t end_;
    OffsetType type_;
};

struct SetBitCommand : public StringCommand
{
    SetBitCommand() = default;
    explicit SetBitCommand(int64_t offset, int8_t val)
        : offset_(offset), value_(val)
    {
    }

    /**
     * Deep copy the command arguments.
     * @param rhs
     */
    SetBitCommand(const SetBitCommand &rhs);
    SetBitCommand(SetBitCommand &&rhs) = default;
    ~SetBitCommand() override = default;
    SetBitCommand &operator=(const SetBitCommand &rhs) = delete;
    SetBitCommand &operator=(SetBitCommand &&rhs) = delete;

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<SetBitCommand>(*this);
    }

    bool IsReadOnly() const override
    {
        return false;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return true;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *obj_ptr) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    int64_t offset_;
    int8_t value_;  // Its value should only be 1 or 0
};

struct SetRangeCommand : public StringCommand
{
    SetRangeCommand() = default;
    explicit SetRangeCommand(int64_t offset, std::string_view val)
        : offset_(offset), value_(val)
    {
    }

    /**
     * Deep copy the command arguments.
     * @param rhs
     */
    SetRangeCommand(const SetRangeCommand &rhs);
    SetRangeCommand(SetRangeCommand &&rhs) = default;
    ~SetRangeCommand() override = default;
    SetRangeCommand &operator=(const SetRangeCommand &rhs) = delete;
    SetRangeCommand &operator=(SetRangeCommand &&rhs) = delete;

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<SetRangeCommand>(*this);
    }

    bool IsReadOnly() const override
    {
        return false;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return true;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *obj_ptr) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    int64_t offset_;
    EloqString value_;
};

struct AppendCommand : public StringCommand
{
    explicit AppendCommand(std::string_view value) : value_(value)
    {
    }

    AppendCommand() = default;
    AppendCommand(const AppendCommand &rhs);
    AppendCommand(AppendCommand &&rhs) = default;
    ~AppendCommand() override = default;
    AppendCommand &operator=(const AppendCommand &rhs) = delete;
    AppendCommand &operator=(AppendCommand &&rhs) = delete;

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<AppendCommand>(*this);
    }

    bool IsReadOnly() const override
    {
        return false;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return true;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *obj_ptr) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    EloqString value_;
};

struct BitFieldCommand : public StringCommand
{
    enum class OpType : int8_t
    {
        GET = 0,
        SET,
        INCR
    };
    enum class Overflow : int8_t
    {
        WRAP,
        SAT,
        FAIL
    };
    struct SubCommand
    {
        OpType op_type_;
        bool encoding_i_;       // encoding is signed or not
        int8_t encoding_bits_;  // bits of encoding
        Overflow ovf_{Overflow::WRAP};
        int64_t offset_;
        int64_t value_;
    };

    explicit BitFieldCommand(std::vector<SubCommand> &&vct)
        : vct_sub_cmd_(std::move(vct))
    {
    }

    BitFieldCommand() = default;
    BitFieldCommand(const BitFieldCommand &rhs);
    BitFieldCommand(BitFieldCommand &&rhs) = default;
    ~BitFieldCommand() override = default;
    BitFieldCommand &operator=(const BitFieldCommand &rhs) = delete;
    BitFieldCommand &operator=(BitFieldCommand &&rhs) = delete;

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<BitFieldCommand>(*this);
    }

    bool IsReadOnly() const override
    {
        return false;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return true;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *obj_ptr) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    std::vector<SubCommand> vct_sub_cmd_;
};

struct BitPosCommand : public StringCommand
{
    explicit BitPosCommand(uint8_t bit_val, int64_t start, int64_t end)
        : bit_val_(bit_val), start_(start), end_(end)
    {
    }

    BitPosCommand() = default;
    BitPosCommand(const BitPosCommand &rhs) = default;
    BitPosCommand(BitPosCommand &&rhs) = default;
    ~BitPosCommand() override = default;
    BitPosCommand &operator=(const BitPosCommand &rhs) = delete;
    BitPosCommand &operator=(BitPosCommand &&rhs) = delete;

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<BitPosCommand>(*this);
    }

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    uint8_t bit_val_;  // Must be 0 or 1
    int64_t start_;
    int64_t end_;
};

struct ListCommand : public RedisCommand
{
    std::unique_ptr<txservice::TxRecord> CreateObject(
        const std::string *image) const override;

    std::unique_ptr<txservice::TxCommandResult> CreateCommandResult()
        const override
    {
        return std::make_unique<RedisListResult>();
    }

    txservice::TxCommandResult *GetResult() override
    {
        return &result_;
    }

    static bool CheckTypeMatch(const txservice::TxRecord &obj);

    RedisListResult result_;
};

struct LPushCommand : public ListCommand
{
    LPushCommand() = default;
    explicit LPushCommand(std::vector<EloqString> &&para_list)
        : elements_(std::move(para_list))
    {
    }

    LPushCommand(const LPushCommand &rhs);
    LPushCommand(LPushCommand &&rhs) = default;
    ~LPushCommand() override = default;
    LPushCommand &operator=(const LPushCommand &other) = delete;
    LPushCommand &operator=(LPushCommand &&other) = delete;

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<LPushCommand>(*this);
    }

    bool IsReadOnly() const override
    {
        return false;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return true;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *obj_ptr) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    std::vector<EloqString> elements_;
};

struct LPopCommand : public ListCommand
{
    LPopCommand() = default;
    explicit LPopCommand(int64_t count) : count_(count)
    {
    }

    LPopCommand(const LPopCommand &rhs) = default;
    LPopCommand(LPopCommand &&rhs) = default;
    ~LPopCommand() override = default;
    LPopCommand &operator=(const LPopCommand &other) = delete;
    LPopCommand &operator=(LPopCommand &&rhs) = delete;

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<LPopCommand>(*this);
    }

    bool IsReadOnly() const override
    {
        return false;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *obj_ptr) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    int64_t count_{-1};
};

struct RPushCommand : public ListCommand
{
    RPushCommand() = default;
    explicit RPushCommand(std::vector<EloqString> &&para_list)
        : elements_(std::move(para_list))
    {
    }

    /**
     * Deep copy the command arguments.
     * @param rhs
     */
    RPushCommand(const RPushCommand &rhs);
    RPushCommand(RPushCommand &&rhs) = default;
    ~RPushCommand() override = default;
    RPushCommand &operator=(const RPushCommand &other) = delete;
    RPushCommand &operator=(RPushCommand &&rhs) = delete;

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<RPushCommand>(*this);
    }

    bool IsReadOnly() const override
    {
        return false;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return true;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *obj_ptr) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    std::vector<EloqString> elements_;
};

struct RPopCommand : public ListCommand
{
    RPopCommand() = default;
    explicit RPopCommand(int64_t count) : count_(count)
    {
    }

    RPopCommand(const RPopCommand &rhs) = default;
    RPopCommand(RPopCommand &&rhs) = default;
    ~RPopCommand() override = default;
    RPopCommand &operator=(const RPopCommand &other) = delete;
    RPopCommand &operator=(RPopCommand &&rhs) = delete;

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<RPopCommand>(*this);
    }

    bool IsReadOnly() const override
    {
        return false;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *obj_ptr) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    int64_t count_{-1};
};

// LMPopCommand is not executed on RedisObject directly.
struct LMPopCommand : RedisMultiObjectCommand
{
    LMPopCommand() = default;
    LMPopCommand(std::vector<EloqKey> &&keys,
                 bool from_left,
                 int64_t count = 1);

    LMPopCommand(const LMPopCommand &rhs) = default;
    LMPopCommand(LMPopCommand &&rhs) = default;
    ~LMPopCommand() = default;
    LMPopCommand &operator=(const LMPopCommand &other) = delete;
    LMPopCommand &operator=(LMPopCommand &&rhs) = delete;

    void OutputResult(OutputHandler *reply) const override;
    bool HandleMiddleResult() override
    {
        return pop_cmd_->result_.err_code_ == RD_NIL ||
               (pop_cmd_->result_.err_code_ == RD_OK &&
                pop_cmd_->result_.ret_ == 0);
    }

    bool IsPassed() const override
    {
        return pop_cmd_->result_.err_code_ == RD_OK ||
               pop_cmd_->result_.err_code_ == RD_NIL;
    }

    std::vector<EloqKey> keys_{};
    std::unique_ptr<ListCommand> pop_cmd_;
    // int64_t count_{0};

    // RedisListResult result_;
    // int32_t result_key_idx_{-1};
};

struct LMovePushCommand : public ListCommand
{
    LMovePushCommand() = default;
    explicit LMovePushCommand(bool is_left) : is_left_(is_left)
    {
    }

    LMovePushCommand(const LMovePushCommand &rhs);
    LMovePushCommand(LMovePushCommand &&rhs) = default;
    ~LMovePushCommand() override = default;
    LMovePushCommand &operator=(const LMovePushCommand &other) = delete;
    LMovePushCommand &operator=(LMovePushCommand &&rhs) = delete;

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<LMovePushCommand>(*this);
    }

    bool IsReadOnly() const override
    {
        return false;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return true;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *obj_ptr) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    bool is_left_;
    EloqString element_;
    bool is_in_the_middle_stage_{true};
};

struct LMovePopCommand : public ListCommand
{
    LMovePopCommand() = default;
    explicit LMovePopCommand(bool is_left) : is_left_(is_left)
    {
    }

    LMovePopCommand(const LMovePopCommand &rhs);
    LMovePopCommand(LMovePopCommand &&rhs) = delete;
    ~LMovePopCommand() override = default;
    LMovePopCommand &operator=(const LMovePopCommand &other) = delete;
    LMovePopCommand &operator=(LMovePopCommand &&rhs) = delete;

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<LMovePopCommand>(*this);
    }

    bool IsReadOnly() const override
    {
        return false;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *obj_ptr) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    bool is_left_;
};

struct LRangeCommand : public ListCommand
{
    LRangeCommand() = default;
    LRangeCommand(int64_t start, int64_t end) : start_(start), end_(end)
    {
    }

    LRangeCommand(const LRangeCommand &rhs) = delete;
    LRangeCommand(LRangeCommand &&rhs) = default;
    ~LRangeCommand() override = default;
    LRangeCommand &operator=(const LRangeCommand &other) = delete;
    LRangeCommand &operator=(LRangeCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    int32_t start_{};
    int32_t end_{};
};

struct LLenCommand : public ListCommand
{
    LLenCommand() = default;

    LLenCommand(const LLenCommand &rhs) = delete;
    LLenCommand(LLenCommand &&rhs) = default;
    ~LLenCommand() override = default;
    LLenCommand &operator=(const LLenCommand &other) = delete;
    LLenCommand &operator=(LLenCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;
    void Serialize(std::string &str) const override;
    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;
};

struct LTrimCommand : public ListCommand
{
    LTrimCommand() = default;
    explicit LTrimCommand(int64_t start, int64_t end) : start_(start), end_(end)
    {
    }

    LTrimCommand(const LTrimCommand &rhs) = default;
    LTrimCommand(LTrimCommand &&rhs) = default;
    ~LTrimCommand() override = default;
    LTrimCommand &operator=(const LTrimCommand &other) = delete;
    LTrimCommand &operator=(LTrimCommand &&rhs) = delete;

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<LTrimCommand>(*this);
    }

    bool IsReadOnly() const override
    {
        return false;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *obj_ptr) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    int64_t start_;
    int64_t end_;
};

struct LIndexCommand : public ListCommand
{
    LIndexCommand() = default;
    explicit LIndexCommand(int64_t index) : index_(index)
    {
    }

    LIndexCommand(const LIndexCommand &rhs) = delete;
    LIndexCommand(LIndexCommand &&rhs) = default;
    ~LIndexCommand() override = default;
    LIndexCommand &operator=(const LIndexCommand &other) = delete;
    LIndexCommand &operator=(LIndexCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    int64_t index_;
};

struct LInsertCommand : public ListCommand
{
    LInsertCommand() = default;
    explicit LInsertCommand(bool is_before,
                            std::string_view pivot,
                            std::string_view element)
        : is_before_(is_before), pivot_(pivot), element_(element)
    {
    }

    LInsertCommand(const LInsertCommand &rhs);
    LInsertCommand(LInsertCommand &&rhs) = default;
    ~LInsertCommand() override = default;
    LInsertCommand &operator=(const LInsertCommand &other) = delete;
    LInsertCommand &operator=(LInsertCommand &&rhs) = delete;

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<LInsertCommand>(*this);
    }

    bool IsReadOnly() const override
    {
        return false;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *obj_ptr) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    bool is_before_;
    EloqString pivot_;
    EloqString element_;
};

struct LPosCommand : public ListCommand
{
    LPosCommand() = default;
    explicit LPosCommand(std::string_view element,
                         int64_t rank,
                         int64_t count,
                         uint64_t len)
        : element_(element), rank_(rank), count_(count), len_(len)
    {
    }

    LPosCommand(const LPosCommand &rhs) = delete;
    LPosCommand(LPosCommand &&rhs) = default;
    ~LPosCommand() override = default;
    LPosCommand &operator=(const LPosCommand &other) = delete;
    LPosCommand &operator=(LPosCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    EloqString element_;

    int64_t rank_;
    int64_t count_{-1};
    uint64_t len_;
};

struct LSetCommand : public ListCommand
{
    LSetCommand() = default;
    explicit LSetCommand(int64_t index, std::string_view element)
        : index_(index), element_(element)
    {
    }

    LSetCommand(const LSetCommand &rhs);
    LSetCommand(LSetCommand &&rhs) = default;
    ~LSetCommand() override = default;
    LSetCommand &operator=(const LSetCommand &other) = delete;
    LSetCommand &operator=(LSetCommand &&rhs) = delete;

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<LSetCommand>(*this);
    }

    bool IsReadOnly() const override
    {
        return false;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *obj_ptr) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    int64_t index_;
    EloqString element_;
};

struct LRemCommand : public ListCommand
{
    LRemCommand() = default;
    explicit LRemCommand(int64_t count, std::string_view element)
        : count_(count), element_(element)
    {
    }

    LRemCommand(const LRemCommand &rhs);
    LRemCommand(LRemCommand &&rhs) = default;
    ~LRemCommand() override = default;
    LRemCommand &operator=(const LRemCommand &other) = delete;
    LRemCommand &operator=(LRemCommand &&rhs) = delete;

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<LRemCommand>(*this);
    }

    bool IsReadOnly() const override
    {
        return false;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *obj_ptr) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    int64_t count_;
    EloqString element_;
};

struct LPushXCommand : public ListCommand
{
    LPushXCommand() = default;
    explicit LPushXCommand(std::vector<EloqString> &&para_list)
        : elements_(std::move(para_list))
    {
    }

    /**
     * Deep copy the command arguments.
     * @param rhs
     */
    LPushXCommand(const LPushXCommand &rhs);
    LPushXCommand(LPushXCommand &&rhs) = default;
    ~LPushXCommand() override = default;
    LPushXCommand &operator=(const LPushXCommand &other) = delete;
    LPushXCommand &operator=(LPushXCommand &&rhs) = delete;

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<LPushXCommand>(*this);
    }

    bool IsReadOnly() const override
    {
        return false;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *obj_ptr) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    std::vector<EloqString> elements_;
};

struct RPushXCommand : public ListCommand
{
    RPushXCommand() = default;
    explicit RPushXCommand(std::vector<EloqString> &&para_list)
        : elements_(std::move(para_list))
    {
    }

    /**
     * Deep copy the command arguments.
     * @param rhs
     */
    RPushXCommand(const RPushXCommand &rhs);
    RPushXCommand(RPushXCommand &&rhs) = default;
    ~RPushXCommand() override = default;
    RPushXCommand &operator=(const RPushXCommand &other) = delete;
    RPushXCommand &operator=(RPushXCommand &&rhs) = delete;

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<RPushXCommand>(*this);
    }

    bool IsReadOnly() const override
    {
        return false;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *obj_ptr) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    std::vector<EloqString> elements_;
};

struct BlockLPopCommand : public ListCommand
{
    BlockLPopCommand() = default;
    BlockLPopCommand(txservice::BlockOperation op_type,
                     bool is_left,
                     uint32_t count)
        : op_type_(op_type), is_left_(is_left), count_(count)
    {
    }

    BlockLPopCommand(const BlockLPopCommand &rhs) = default;
    BlockLPopCommand(BlockLPopCommand &&rhs) = default;
    ~BlockLPopCommand() override = default;
    BlockLPopCommand &operator=(const BlockLPopCommand &other) = delete;
    BlockLPopCommand &operator=(BlockLPopCommand &&rhs) = delete;

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<BlockLPopCommand>(*this);
    }

    bool IsReadOnly() const override
    {
        return false;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return true;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *obj_ptr) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    bool AblePopBlockRequest(txservice::TxObject *object) const override;

    txservice::BlockOperation GetBlockOperationType() override
    {
        return op_type_;
    }

    txservice::BlockOperation op_type_;
    bool is_left_;    // True: pop from left aside; False: pop from right aside
    uint32_t count_;  // The max number of elements to pop
};

struct BlockDiscardCommand : public ListCommand
{
    BlockDiscardCommand() = default;

    BlockDiscardCommand(const BlockDiscardCommand &rhs) = default;
    BlockDiscardCommand(BlockDiscardCommand &&rhs) = default;
    ~BlockDiscardCommand() override = default;
    BlockDiscardCommand &operator=(const BlockDiscardCommand &other) = delete;
    BlockDiscardCommand &operator=(BlockDiscardCommand &&rhs) = delete;

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<BlockDiscardCommand>(*this);
    }

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    txservice::BlockOperation GetBlockOperationType() override
    {
        return txservice::BlockOperation::Discard;
    }
};

struct BLMoveCommand : public RedisMultiObjectCommand
{
public:
    BLMoveCommand() = default;
    BLMoveCommand(EloqKey &&skey,
                  BlockLPopCommand &&sour_cmd,
                  EloqKey &&dkey,
                  LMovePushCommand &&dest_cmd,
                  uint64_t ts_expired);

    void OutputResult(OutputHandler *reply) const override;

    bool HandleMiddleResult() override;

    bool ForwardResult() override;

    bool IsPassed() const override
    {
        if (curr_step_ == 3)
        {
            return dest_cmd_->result_.err_code_ == RD_OK ||
                   dest_cmd_->result_.err_code_ == RD_NIL;
        }
        else
        {
            return false;
        }
    }

    bool IsExpired() const override
    {
        if (curr_step_ != 1)
        {
            return false;
        }

        return txservice::LocalCcShards::ClockTs() > ts_expired_;
    }

    uint32_t NumOfFinishBlockCommands() const override
    {
        return (curr_step_ == 1 && !has_forwarded_) ? 1 : 0;
    }

    bool IsBlockCommand() override
    {
        return true;
    }

    std::unique_ptr<EloqKey> sour_key_;
    std::unique_ptr<BlockLPopCommand> sour_cmd_;
    std::unique_ptr<EloqKey> dest_key_;
    std::unique_ptr<LMovePushCommand> dest_cmd_;
    std::unique_ptr<BlockDiscardCommand> discard_cmd_;
    uint64_t ts_expired_;
    bool has_forwarded_{false};  // Called ForwardResult or not
};

struct BLMPopCommand : public RedisMultiObjectCommand
{
public:
    BLMPopCommand() = default;
    BLMPopCommand(std::vector<EloqKey> &&v_key,
                  std::vector<BlockLPopCommand> &&v_cmd,
                  uint64_t ts_expired,
                  bool is_mpop);
    void OutputResult(OutputHandler *reply) const override;

    bool HandleMiddleResult() override;

    bool ForwardResult() override;

    bool IsPassed() const override
    {
        if (curr_step_ < vct_key_.size())
        {
            return vct_cmd_[curr_step_].result_.err_code_ == RD_OK;
        }
        else if (curr_step_ == vct_key_.size())
        {
            if (vct_key_.size() == 1)
            {
                return vct_cmd_[0].result_.err_code_ == RD_OK &&
                       vct_cmd_[0].result_.ret_ > 0;
            }
            else
            {
                return pos_cmd_ >= 0;
            }
        }
        else if (curr_step_ == vct_key_.size() + 1)
        {
            return vct_cmd_[pos_cmd_].result_.err_code_ == RD_OK &&
                   vct_cmd_[pos_cmd_].result_.ret_ > 0;
        }
        else
        {
            return false;
        }
    }

    bool IsExpired() const override
    {
        if (curr_step_ != vct_key_.size())
        {
            return false;
        }

        return txservice::LocalCcShards::ClockTs() > ts_expired_;
    }

    uint32_t NumOfFinishBlockCommands() const override
    {
        return (curr_step_ == vct_key_.size() && !has_forwarded_) ? 1 : 0;
    }

    bool IsBlockCommand() override
    {
        return true;
    }

    std::vector<EloqKey> vct_key_;
    std::vector<BlockLPopCommand> vct_cmd_;
    std::unique_ptr<BlockDiscardCommand> discard_cmd_;
    uint64_t ts_expired_;
    int pos_cmd_{-1};            // The position of command that pop elements
    bool has_forwarded_{false};  // Called ForwardResult or not
    bool is_mpop_{false};
};

struct IntOpCommand : public RedisCommand
{
    IntOpCommand() = default;
    explicit IntOpCommand(int64_t incr) : incr_(incr)
    {
    }

    IntOpCommand(const IntOpCommand &rhs) = default;
    IntOpCommand(IntOpCommand &&rhs) = default;
    ~IntOpCommand() override = default;
    IntOpCommand &operator=(const IntOpCommand &rhs) = delete;
    IntOpCommand &operator=(IntOpCommand &&rhs) = delete;

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<IntOpCommand>(*this);
    }

    bool IsReadOnly() const override
    {
        return false;
    }

    std::unique_ptr<txservice::TxRecord> CreateObject(
        const std::string *image) const override;

    std::unique_ptr<txservice::TxCommandResult> CreateCommandResult()
        const override
    {
        return std::make_unique<RedisIntResult>();
    }

    txservice::TxCommandResult *GetResult() override
    {
        return &result_;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return true;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *obj_ptr) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    static bool CheckTypeMatch(const txservice::TxRecord &obj);

    RedisIntResult result_;
    // The increment value.
    int64_t incr_{};
};

struct FloatOpCommand : public RedisCommand
{
    FloatOpCommand() = default;
    explicit FloatOpCommand(long double incr) : incr_(incr)
    {
    }

    FloatOpCommand(const FloatOpCommand &rhs) = default;
    FloatOpCommand(FloatOpCommand &&rhs) = default;
    ~FloatOpCommand() override = default;
    FloatOpCommand &operator=(const FloatOpCommand &rhs) = delete;
    FloatOpCommand &operator=(FloatOpCommand &&rhs) = delete;

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<FloatOpCommand>(*this);
    }

    bool IsReadOnly() const override
    {
        return false;
    }

    std::unique_ptr<txservice::TxRecord> CreateObject(
        const std::string *image) const override;

    std::unique_ptr<txservice::TxCommandResult> CreateCommandResult()
        const override
    {
        return std::make_unique<RedisStringResult>();
    }

    txservice::TxCommandResult *GetResult() override
    {
        return &result_;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return true;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *obj_ptr) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    static bool CheckTypeMatch(const txservice::TxRecord &obj);

    RedisStringResult result_;
    // The increment value.
    long double incr_{};
};

struct TypeCommand : public RedisCommand
{
    std::unique_ptr<txservice::TxCommand> Clone() override
    {
        return std::make_unique<TypeCommand>(*this);
    }

    std::unique_ptr<txservice::TxRecord> CreateObject(
        const std::string *image) const override
    {
        return nullptr;
    }

    bool IsReadOnly() const override
    {
        return true;
    }

    bool IsOverwrite() const override
    {
        return false;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    std::unique_ptr<txservice::TxCommandResult> CreateCommandResult()
        const override
    {
        return std::make_unique<RedisStringResult>();
    }

    txservice::TxCommandResult *GetResult() override
    {
        return &result_;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    RedisStringResult result_;
};

struct DelCommand : public RedisCommand
{
    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<DelCommand>(*this);
    }

    std::unique_ptr<txservice::TxRecord> CreateObject(
        const std::string *image) const override
    {
        return nullptr;
    }

    bool IsReadOnly() const override
    {
        return false;
    }

    bool IsOverwrite() const override
    {
        return true;
    }

    bool IsDelete() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    std::unique_ptr<txservice::TxCommandResult> CreateCommandResult()
        const override
    {
        return std::make_unique<RedisIntResult>();
    }

    txservice::TxCommandResult *GetResult() override
    {
        return &result_;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *obj_ptr) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    RedisIntResult result_;
};

struct ExistsCommand : public RedisCommand
{
    explicit ExistsCommand(int cnt = 1) : cnt_(cnt)
    {
    }

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<ExistsCommand>(*this);
    }

    std::unique_ptr<txservice::TxRecord> CreateObject(
        const std::string *image) const override
    {
        return nullptr;
    }

    bool IsReadOnly() const override
    {
        return true;
    }

    bool IsOverwrite() const override
    {
        return false;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    std::unique_ptr<txservice::TxCommandResult> CreateCommandResult()
        const override
    {
        return std::make_unique<RedisIntResult>();
    }

    txservice::TxCommandResult *GetResult() override
    {
        return &result_;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *obj_ptr) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    // occurrence count of the key, for OutputResult
    int32_t cnt_{};

    RedisIntResult result_;
};

enum struct ZRangeType
{
    ZRANGE_AUTO = 0,
    ZRANGE_RANK,
    ZRANGE_SCORE,
    ZRANGE_LEX,
};

enum struct ZRangeDirection
{
    ZRANGE_DIRECTION_AUTO = 0,
    ZRANGE_DIRECTION_FORWARD,
    ZRANGE_DIRECTION_REVERSE
};

// This struct export query Direction And Constraints
struct ZRangeConstraint
{
    ZRangeConstraint() = default;
    ZRangeConstraint(bool opt_withscores,
                     int opt_offset,
                     int opt_limt,
                     ZRangeType rangetype,
                     ZRangeDirection direction)
        : opt_withscores_(opt_withscores),
          opt_offset_(opt_offset),
          opt_limit_(opt_limt),
          rangetype_(rangetype),
          direction_(direction)
    {
    }
    ZRangeConstraint(ZRangeConstraint &rhs) = default;
    ZRangeConstraint(ZRangeConstraint &&rhs) = default;
    ZRangeConstraint &operator=(ZRangeConstraint &rhs) = delete;
    ZRangeConstraint &operator=(ZRangeConstraint &&rhs) = delete;

    bool opt_withscores_ = false;
    int opt_offset_ = 0;
    int opt_limit_ = -1;
    ZRangeType rangetype_ = ZRangeType::ZRANGE_AUTO;
    // represent [REV] flag in ZRANGE command
    ZRangeDirection direction_ = ZRangeDirection::ZRANGE_DIRECTION_AUTO;
};

// This struct passes the parameter information(start_place and end_place)
// of zrange
struct ZRangeSpec
{
    bool null_ = false;      // true iff min is + or max is -
    bool minex_ = false;     // if true mean not include min in range;
    bool maxex_ = false;     // if true mean not include max in range;
    bool negative_ = false;  // negative sign(-) used in XXXbylex
    bool positive_ = false;  // positive sign(+) used in XXXbylex
    std::variant<int, double, EloqString> opt_start_;
    std::variant<int, double, EloqString> opt_end_;

    ZRangeSpec() = default;
    ZRangeSpec(const ZRangeSpec &rhs)
        : minex_(rhs.minex_),
          maxex_(rhs.maxex_),
          negative_(rhs.negative_),
          positive_(rhs.positive_)
    {
        if (std::holds_alternative<int>(rhs.opt_start_))
        {
            opt_start_ = std::get<int>(rhs.opt_start_);
        }
        else if (std::holds_alternative<double>(rhs.opt_start_))
        {
            opt_start_ = std::get<double>(rhs.opt_start_);
        }
        else if (std::holds_alternative<EloqString>(rhs.opt_start_))
        {
            opt_start_ = std::get<EloqString>(rhs.opt_start_).Clone();
        }
        else
        {
            assert(false);
        }

        if (std::holds_alternative<int>(rhs.opt_end_))
        {
            opt_end_ = std::get<int>(rhs.opt_end_);
        }
        else if (std::holds_alternative<double>(rhs.opt_end_))
        {
            opt_end_ = std::get<double>(rhs.opt_end_);
        }
        else if (std::holds_alternative<EloqString>(rhs.opt_end_))
        {
            opt_end_ = std::get<EloqString>(rhs.opt_end_).Clone();
        }
        else
        {
            assert(false);
        }
    }
    ZRangeSpec(ZRangeSpec &&rhs) = default;

    ZRangeSpec &operator=(ZRangeSpec &rhs) = delete;
    ZRangeSpec &operator=(ZRangeSpec &&rhs) = delete;
};

struct ZsetCommand : public RedisCommand
{
    std::unique_ptr<txservice::TxRecord> CreateObject(
        const std::string *image) const override;

    std::unique_ptr<txservice::TxCommandResult> CreateCommandResult()
        const override
    {
        return std::make_unique<RedisZsetResult>();
    }

    txservice::TxCommandResult *GetResult() override
    {
        return &zset_result_;
    }

    static bool CheckTypeMatch(const txservice::TxRecord &obj);

    RedisZsetResult zset_result_;
};

/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#define ZADD_IN_INCR (1 << 0)  // Increment the score instead of setting it.
#define ZADD_IN_NX (1 << 1)    // Don't touch elements already existing.
#define ZADD_IN_XX (1 << 2)    // Only touch elements already existing.
#define ZADD_IN_GT (1 << 3)  // Only update existing when new scores are higher.
#define ZADD_IN_LT (1 << 4)  // Only update existing when new scores are lower.
#define ZADD_OUT_CH (1 << 5)       // Include add and change
#define ZADD_FORCE_CLEAR (1 << 6)  // Clean all old elements if exist in object

// This struct passes the parameter information of zrange
class ZParams
{
public:
    inline bool XX() const
    {
        return (flags & ZADD_IN_XX) != 0;
    }

    inline bool NX() const
    {
        return (flags & ZADD_IN_NX) != 0;
    }

    inline bool GT() const
    {
        return (flags & ZADD_IN_GT) != 0;
    }

    inline bool LT() const
    {
        return (flags & ZADD_IN_LT) != 0;
    }

    inline bool INCR() const
    {
        return (flags & ZADD_IN_INCR) != 0;
    }

    inline bool CH() const
    {
        return (flags & ZADD_OUT_CH) != 0;
    }

    inline bool ForceClear() const
    {
        return (flags & ZADD_FORCE_CLEAR) != 0;
    }

    uint8_t flags = 0;  // mask of ZADD_IN_ macros.
};

struct ZPopCommand : public ZsetCommand
{
    enum class PopType
    {
        POPMIN = 0,
        POPMAX
    };

    ZPopCommand() = default;

    explicit ZPopCommand(PopType pop_type, int64_t count = 1)
        : pop_type_(pop_type), count_(count)
    {
    }

    ZPopCommand(const ZPopCommand &rhs) = default;
    ZPopCommand(ZPopCommand &&rhs) = default;
    ~ZPopCommand() override = default;
    ZPopCommand &operator=(const ZPopCommand &rhs) = delete;
    ZPopCommand &operator=(ZPopCommand &&rhs) = delete;

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<ZPopCommand>(*this);
    }

    bool IsReadOnly() const override
    {
        return false;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *object) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    PopType pop_type_;
    int64_t count_;
};

struct ZLexCountCommand : public ZsetCommand
{
    ZLexCountCommand() = default;

    explicit ZLexCountCommand(ZRangeSpec &&spec) : spec_(std::move(spec))
    {
    }

    ZLexCountCommand(const ZLexCountCommand &rhs) = delete;
    ZLexCountCommand(ZLexCountCommand &&rhs) = default;
    ~ZLexCountCommand() override = default;
    ZLexCountCommand &operator=(const ZLexCountCommand &rhs) = delete;
    ZLexCountCommand &operator=(ZLexCountCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    ZRangeSpec spec_;
};

struct ZAddCommand : public ZsetCommand
{
    enum ElementType
    {
        monostate = 0,
        pair,
        vector
    } type_;

    ZAddCommand() = default;
    ZAddCommand(std::vector<std::pair<double, EloqString>> &&mp,
                ZParams &params,
                ElementType &&type)
        : type_(type), elements_(std::move(mp)), params_(params)
    {
    }
    ZAddCommand(std::pair<double, EloqString> &&mp,
                ZParams &params,
                ElementType &&type)
        : type_(type), elements_(std::move(mp)), params_(params)
    {
    }

    ZAddCommand(const ZAddCommand &rhs);
    ZAddCommand(ZAddCommand &&rhs) = default;
    ~ZAddCommand() override = default;
    ZAddCommand &operator=(const ZAddCommand &other) = delete;
    ZAddCommand &operator=(ZAddCommand &&rhs) = delete;

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<ZAddCommand>(*this);
    }

    bool IsReadOnly() const override
    {
        return false;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return !params_.XX();
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *object) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    std::variant<std::monostate,
                 std::pair<double, EloqString>,
                 std::vector<std::pair<double, EloqString>>>
        elements_;
    ZParams params_;
    bool is_in_the_middle_stage_{false};
};

struct ZRangeCommand : public ZsetCommand
{
    ZRangeCommand() = default;
    ZRangeCommand(ZRangeConstraint &constraint,
                  ZRangeSpec &&spec,
                  bool multi_type = false)
        : constraint_(constraint),
          spec_(std::move(spec)),
          multi_type_range_(multi_type)
    {
    }

    ZRangeCommand(const ZRangeCommand &rhs) = delete;
    ZRangeCommand(ZRangeCommand &&rhs) = default;
    ~ZRangeCommand() override = default;
    ZRangeCommand &operator=(const ZRangeCommand &rhs) = delete;
    ZRangeCommand &operator=(ZRangeCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    ZRangeConstraint constraint_;
    ZRangeSpec spec_;
    // If able to range on set object or other type in future, used for commands
    // ZDiff, ZDiffStore
    bool multi_type_range_{false};
};

struct ZRemRangeCommand : public ZsetCommand
{
    ZRemRangeCommand() = default;
    ZRemRangeCommand(ZRangeType &range_type, ZRangeSpec &&spec)
        : range_type_(range_type), spec_(std::move(spec))
    {
    }

    ZRemRangeCommand(const ZRemRangeCommand &rhs) = default;
    ZRemRangeCommand(ZRemRangeCommand &&rhs) = default;
    ~ZRemRangeCommand() override = default;
    ZRemRangeCommand &operator=(const ZRemRangeCommand &rhs) = delete;
    ZRemRangeCommand &operator=(ZRemRangeCommand &&rhs) = delete;

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<ZRemRangeCommand>(*this);
    }

    bool IsReadOnly() const override
    {
        return false;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *object) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    ZRangeType range_type_ = ZRangeType::ZRANGE_AUTO;
    ZRangeSpec spec_;
};

struct ZCountCommand : public ZsetCommand
{
    ZCountCommand() = default;
    explicit ZCountCommand(ZRangeSpec &&spec) : spec_(std::move(spec))
    {
    }

    ZCountCommand(const ZCountCommand &rhs) = delete;
    ZCountCommand(ZCountCommand &&rhs) = default;
    ~ZCountCommand() override = default;
    ZCountCommand &operator=(const ZCountCommand &rhs) = delete;
    ZCountCommand &operator=(ZCountCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;
    void Serialize(std::string &str) const override;
    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    ZRangeSpec spec_;
};

struct ZCardCommand : public ZsetCommand
{
    ZCardCommand() = default;
    ZCardCommand(const ZCardCommand &rhs) = delete;
    ZCardCommand(ZCardCommand &&rhs) = default;
    ~ZCardCommand() override = default;
    ZCardCommand &operator=(const ZCardCommand &rhs) = delete;
    ZCardCommand &operator=(ZCardCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;
    void Serialize(std::string &str) const override;
    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;
};

struct ZRemCommand : public ZsetCommand
{
    ZRemCommand() = default;
    explicit ZRemCommand(std::vector<EloqString> &&elements)
        : elements_(std::move(elements))
    {
    }

    ZRemCommand(const ZRemCommand &rhs);
    ZRemCommand(ZRemCommand &&rhs) = default;
    ~ZRemCommand() override = default;
    ZRemCommand &operator=(const ZRemCommand &other) = delete;
    ZRemCommand &operator=(ZRemCommand &&rhs) = delete;

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<ZRemCommand>(*this);
    }

    bool IsReadOnly() const override
    {
        return false;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *object) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    std::vector<EloqString> elements_;
};

struct ZScoreCommand : public ZsetCommand
{
    ZScoreCommand() = default;
    explicit ZScoreCommand(std::string_view element) : element_(element)
    {
    }

    ZScoreCommand(const ZScoreCommand &rhs) = delete;
    ZScoreCommand(ZScoreCommand &&rhs) = default;
    ~ZScoreCommand() override = default;
    ZScoreCommand &operator=(const ZScoreCommand &other) = delete;
    ZScoreCommand &operator=(ZScoreCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    EloqString element_;
};

struct ZScanCommand : public ZsetCommand
{
    ZScanCommand() = default;
    explicit ZScanCommand(bool with_cursor,
                          EloqString &&cursor_field,
                          double cursor_score,
                          bool match,
                          EloqString &&pattern,
                          uint64_t count)
        : with_cursor_(with_cursor),
          cursor_field_(std::move(cursor_field)),
          cursor_score_(cursor_score),
          match_(match),
          pattern_(std::move(pattern)),
          count_(count)
    {
    }

    ZScanCommand(const ZScanCommand &rhs) = delete;
    ZScanCommand(ZScanCommand &&rhs) = default;
    ~ZScanCommand() override = default;
    ZScanCommand &operator=(const ZScanCommand &rhs) = delete;
    ZScanCommand &operator=(ZScanCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    void OutputResult(OutputHandler *reply, RedisConnectionContext *ctx) const;

    static std::string SerializeCursorZNode(const EloqString &field,
                                            const double &score)
    {
        std::string output;
        output.append(reinterpret_cast<const char *>(&score), sizeof(double));
        uint32_t field_size = field.Length();
        output.append(reinterpret_cast<const char *>(&field_size),
                      sizeof(uint32_t));
        output.append(field.Data(), field_size);
        return output;
    }

    static void DeserializeCursorZNode(const std::string &str,
                                       EloqString &field,
                                       double &score)
    {
        assert(str.size() > sizeof(double) + sizeof(uint32_t));
        const char *ptr = str.data();
        score = *reinterpret_cast<const double *>(ptr);
        ptr += sizeof(double);

        uint32_t key_size = *reinterpret_cast<const uint32_t *>(ptr);
        ptr += sizeof(uint32_t);

        field = EloqString(ptr, key_size);
        ptr += key_size;
    }

    // int64_t cursor_;
    bool with_cursor_{false};
    EloqString cursor_field_;
    double cursor_score_;
    bool match_{false};
    EloqString pattern_;
    uint64_t count_;
};

struct ZScanWrapper : public CustomCommand
{
    ZScanWrapper(EloqKey &&key, ZScanCommand &&cmd)
        : key_(std::move(key)), cmd_(std::move(cmd))
    {
    }

    ZScanWrapper(const ZScanWrapper &rhs) = delete;
    ZScanWrapper(ZScanWrapper &&rhs) = delete;
    ~ZScanWrapper() = default;
    ZScanWrapper &operator=(const ZScanWrapper &other) = delete;
    ZScanWrapper &operator=(ZScanWrapper &&rhs) = delete;

    bool Execute(RedisServiceImpl *redis_impl,
                 RedisConnectionContext *ctx,
                 const txservice::TableName *table,
                 txservice::TransactionExecution *txm,
                 OutputHandler *output,
                 bool auto_commit) override;

    void OutputResult(OutputHandler *reply,
                      RedisConnectionContext *ctx) const override
    {
        cmd_.OutputResult(reply, ctx);
    }

private:
    EloqKey key_;
    ZScanCommand cmd_;
};

struct ZRandMemberCommand : public ZsetCommand
{
    ZRandMemberCommand() = default;

    explicit ZRandMemberCommand(int64_t count,
                                bool with_scores,
                                bool count_provided)
        : count_(count),
          with_scores_(with_scores),
          count_provided_(count_provided)
    {
    }

    ZRandMemberCommand(const ZRandMemberCommand &rhs) = delete;
    ZRandMemberCommand(ZRandMemberCommand &&rhs) = default;
    ~ZRandMemberCommand() override = default;
    ZRandMemberCommand &operator=(const ZRandMemberCommand &rhs) = delete;
    ZRandMemberCommand &operator=(ZRandMemberCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    int64_t count_;
    bool with_scores_;
    // All int64_t numbers have a meaning when used as count; therefore, we
    // cannot find a special number to represent that count is not provided.
    bool count_provided_;
};

struct ZRankCommand : public ZsetCommand
{
    ZRankCommand() = default;

    explicit ZRankCommand(std::string_view member, bool with_score, bool is_rev)
        : member_(member), with_score_(with_score), is_rev_(is_rev)
    {
    }

    ZRankCommand(const ZRankCommand &rhs) = delete;
    ZRankCommand(ZRankCommand &&rhs) = default;
    ~ZRankCommand() override = default;
    ZRankCommand &operator=(const ZRankCommand &rhs) = delete;
    ZRankCommand &operator=(ZRankCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    EloqString member_;
    bool with_score_;
    bool is_rev_;
};

struct ZMScoreCommand : public ZsetCommand
{
    ZMScoreCommand() = default;
    explicit ZMScoreCommand(std::vector<EloqString> &&vct_elm)
        : vct_elm_(std::move(vct_elm))
    {
    }

    ZMScoreCommand(const ZMScoreCommand &rhs) = delete;
    ZMScoreCommand(ZMScoreCommand &&rhs) = default;
    ~ZMScoreCommand() override = default;
    ZMScoreCommand &operator=(const ZMScoreCommand &other) = delete;
    ZMScoreCommand &operator=(ZMScoreCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    std::vector<EloqString> vct_elm_;
};

struct ZMPopCommand : public RedisMultiObjectCommand
{
public:
    ZMPopCommand() = default;
    ZMPopCommand(std::vector<EloqKey> &&keys, std::vector<ZPopCommand> &&cmds)
        : keys_(std::move(keys)), cmds_(std::move(cmds))
    {
        assert(keys_.size() == cmds_.size());
        vct_key_ptrs_.reserve(keys_.size());
        vct_cmd_ptrs_.reserve(cmds_.size());
        for (const auto &key : keys_)
        {
            std::vector<txservice::TxKey> vct;
            vct.emplace_back(txservice::TxKey(&key));
            vct_key_ptrs_.push_back(std::move(vct));
        }

        for (auto &cmd : cmds_)
        {
            std::vector<txservice::TxCommand *> vct;
            vct.emplace_back(&cmd);
            vct_cmd_ptrs_.push_back(std::move(vct));
        }
    }

    void OutputResult(OutputHandler *reply) const override
    {
        auto &cmd = cmds_[curr_step_];
        if (cmd.zset_result_.err_code_ == RD_OK)
        {
            auto &vct_res =
                std::get<std::vector<EloqString>>(cmd.zset_result_.result_);
            if (vct_res.size() > 0)
            {
                reply->OnArrayStart(2);
                if (found_before_last_)
                {
                    reply->OnString(keys_[curr_step_].KVSerialize());
                }
                else
                {
                    reply->OnString(keys_[keys_.size() - 1].KVSerialize());
                }
                reply->OnArrayStart(vct_res.size() / 2);
                for (size_t i = 0; i < vct_res.size() / 2; ++i)
                {
                    reply->OnArrayStart(2);
                    reply->OnString(vct_res[2 * i].StringView());
                    reply->OnString(vct_res[2 * i + 1].StringView());
                    reply->OnArrayEnd();
                }
                reply->OnArrayEnd();
                reply->OnArrayEnd();
            }
        }
        else if (cmd.zset_result_.err_code_ != RD_NIL)
        {
            reply->OnError(
                redis_get_error_messages(cmd.zset_result_.err_code_));
        }
        else
        {
            reply->OnNil();
        }
    }

    bool HandleMiddleResult() override
    {
        if (cmds_[curr_step_].zset_result_.err_code_ == RD_NIL)
        {
            return true;
        }
        else
        {
            found_before_last_ = true;
            return false;
        }
    }

    bool IsPassed() const override
    {
        auto &cmd = cmds_[curr_step_];
        return cmd.zset_result_.err_code_ == RD_OK ||
               cmd.zset_result_.err_code_ == RD_NIL;
    }

    std::vector<EloqKey> keys_;
    bool found_before_last_{false};  // true when selected key is not last
    std::vector<ZPopCommand> cmds_;
};

struct ZDiffCommand : public RedisMultiObjectCommand
{
public:
    ZDiffCommand() = default;
    ZDiffCommand(std::vector<EloqKey> &&keys, std::vector<ZRangeCommand> &&cmds)
        : keys_(std::move(keys)), cmds_(std::move(cmds))
    {
        assert(keys_.size() == cmds_.size());
        vct_key_ptrs_.resize(1);
        vct_key_ptrs_[0].reserve(keys_.size());
        vct_cmd_ptrs_.resize(1);
        vct_cmd_ptrs_[0].reserve(cmds_.size());

        for (const auto &key : keys_)
        {
            vct_key_ptrs_[0].emplace_back(txservice::TxKey(&key));
        }
        for (auto &cmd : cmds_)
        {
            vct_cmd_ptrs_[0].emplace_back(&cmd);
        }
    }

    void OutputResult(OutputHandler *reply) const override;

    std::vector<EloqKey> keys_;
    std::vector<ZRangeCommand> cmds_;
};

struct ZDiffStoreCommand : public RedisMultiObjectCommand
{
public:
    ZDiffStoreCommand() = default;
    ZDiffStoreCommand(std::vector<EloqKey> &&keys,
                      std::vector<ZRangeCommand> &&cmds,
                      const std::string_view &key)
        : keys_(std::move(keys)),
          cmds_(std::move(cmds)),
          key_store_(std::make_unique<EloqKey>(key)),
          cmd_store_(std::make_unique<ZAddCommand>())
    {
        cmd_store_->is_in_the_middle_stage_ = true;

        assert(keys_.size() == cmds_.size());
        cmd_store_->params_.flags = ZADD_FORCE_CLEAR;
        vct_key_ptrs_.resize(2);
        vct_key_ptrs_[0].reserve(keys_.size());
        vct_cmd_ptrs_.resize(2);
        vct_cmd_ptrs_[0].reserve(cmds_.size());

        for (const auto &key : keys_)
        {
            vct_key_ptrs_[0].emplace_back(&key);
        }
        for (auto &cmd : cmds_)
        {
            vct_cmd_ptrs_[0].push_back(&cmd);
        }

        vct_key_ptrs_[1].emplace_back(key_store_.get());
        vct_cmd_ptrs_[1].emplace_back(cmd_store_.get());
    }

    bool HandleMiddleResult() override;
    void OutputResult(OutputHandler *reply) const override;

    std::vector<EloqKey> keys_;
    std::vector<ZRangeCommand> cmds_;

    std::unique_ptr<EloqKey> key_store_;
    std::unique_ptr<ZAddCommand> cmd_store_;
};

struct HashCommand : public RedisCommand
{
    std::unique_ptr<txservice::TxRecord> CreateObject(
        const std::string *image) const override;

    std::unique_ptr<txservice::TxCommandResult> CreateCommandResult()
        const override
    {
        return std::make_unique<RedisHashResult>();
    }

    txservice::TxCommandResult *GetResult() override
    {
        return &result_;
    }

    static bool CheckTypeMatch(const txservice::TxRecord &obj);

    RedisHashResult result_;
};

struct HSetCommand : public HashCommand
{
    enum struct SubType
    {
        HSET,
        HMSET,
    };

    HSetCommand() = default;
    explicit HSetCommand(
        HSetCommand::SubType sub_type,
        std::vector<std::pair<EloqString, EloqString>> &&field_value_pairs)
        : sub_type_(sub_type), field_value_pairs_(std::move(field_value_pairs))
    {
    }

    HSetCommand(const HSetCommand &other);
    HSetCommand(HSetCommand &&other) = default;
    ~HSetCommand() override = default;
    HSetCommand &operator=(const HSetCommand &rhs) = delete;
    HSetCommand &operator=(HSetCommand &&rhs) = delete;

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<HSetCommand>(*this);
    }

    bool IsReadOnly() const override
    {
        return false;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return true;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *obj_ptr) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    SubType sub_type_{SubType::HSET};

    std::vector<std::pair<EloqString, EloqString>> field_value_pairs_;
};

struct HGetCommand : public HashCommand
{
    HGetCommand() = default;
    explicit HGetCommand(std::string_view sv) : field_(sv)
    {
    }

    HGetCommand(const HGetCommand &rhs) = delete;
    HGetCommand(HGetCommand &&rhs) = default;
    ~HGetCommand() override = default;
    HGetCommand &operator=(const HGetCommand &rhs) = delete;
    HGetCommand &operator=(HGetCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    EloqString field_;
};

struct HLenCommand : public HashCommand
{
    HLenCommand() = default;
    HLenCommand(const HLenCommand &rhs) = delete;
    HLenCommand(HLenCommand &&rhs) = default;
    ~HLenCommand() override = default;
    HLenCommand &operator=(const HLenCommand &rhs) = delete;
    HLenCommand &operator=(HLenCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;
    void Serialize(std::string &str) const override
    {
        uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::HLEN);
        str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
    }

    void Deserialize(std::string_view cmd_image) override
    {
        // Not need to do anything
    }

    void OutputResult(OutputHandler *reply) const override;
};

struct HStrLenCommand : public HashCommand
{
    HStrLenCommand() = default;
    explicit HStrLenCommand(std::string_view field) : field_(field)
    {
    }

    HStrLenCommand(const HStrLenCommand &rhs) = delete;
    HStrLenCommand(HStrLenCommand &&rhs) = default;
    ~HStrLenCommand() override = default;
    HStrLenCommand &operator=(const HStrLenCommand &rhs) = delete;
    HStrLenCommand &operator=(HStrLenCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;
    void Serialize(std::string &str) const override;
    void Deserialize(std::string_view cmd_image) override;
    void OutputResult(OutputHandler *reply) const override;

    EloqString field_;
};

struct HDelCommand : public HashCommand
{
    HDelCommand() = default;
    explicit HDelCommand(std::vector<EloqString> &&del_list)
        : del_list_(std::move(del_list))
    {
    }

    HDelCommand(const HDelCommand &rhs);
    HDelCommand(HDelCommand &&rhs) = default;
    ~HDelCommand() override = default;
    HDelCommand &operator=(const HDelCommand &rhs) = delete;
    HDelCommand &operator=(HDelCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return false;
    }

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<HDelCommand>(*this);
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *const) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    std::vector<EloqString> del_list_;
};

struct HExistsCommand : public HashCommand
{
    HExistsCommand() = default;
    explicit HExistsCommand(std::string_view key) : key_(key)
    {
    }

    HExistsCommand(const HExistsCommand &rhs) = delete;
    HExistsCommand(HExistsCommand &&rhs) = default;
    ~HExistsCommand() override = default;
    HExistsCommand &operator=(const HExistsCommand &rhs) = delete;
    HExistsCommand &operator=(HExistsCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    EloqString key_;
};

struct HGetAllCommand : public HashCommand
{
    HGetAllCommand() = default;

    HGetAllCommand(const HGetAllCommand &rhs) = delete;
    HGetAllCommand(HGetAllCommand &&rhs) = default;
    ~HGetAllCommand() override = default;
    HGetAllCommand &operator=(const HGetAllCommand &rhs) = delete;
    HGetAllCommand &operator=(HGetAllCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void Serialize(std::string &str) const override
    {
        uint8_t type = static_cast<uint8_t>(RedisCommandType::HGETALL);
        str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));
    }

    void Deserialize(std::string_view cmd_image) override
    {
        // Not need to do anything
    }

    void OutputResult(OutputHandler *reply) const override;
};

struct HIncrByCommand : public HashCommand
{
    HIncrByCommand() = default;
    explicit HIncrByCommand(std::string_view sv, int64_t score)
        : field_(sv), score_(score)
    {
    }

    HIncrByCommand(const HIncrByCommand &rhs);
    HIncrByCommand(HIncrByCommand &&rhs) = default;
    ~HIncrByCommand() override = default;
    HIncrByCommand &operator=(const HIncrByCommand &rhs) = delete;
    HIncrByCommand &operator=(HIncrByCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return false;
    }

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<HIncrByCommand>(*this);
    }

    bool ProceedOnNonExistentObject() const override
    {
        return true;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *const) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    EloqString field_;
    int64_t score_;
};

struct HIncrByFloatCommand : public HashCommand
{
    HIncrByFloatCommand() = default;
    explicit HIncrByFloatCommand(std::string_view sv, long double incr)
        : field_(sv), incr_(incr)
    {
    }

    HIncrByFloatCommand(const HIncrByFloatCommand &rhs);
    HIncrByFloatCommand(HIncrByFloatCommand &&rhs) = default;
    ~HIncrByFloatCommand() override = default;
    HIncrByFloatCommand &operator=(const HIncrByFloatCommand &rhs) = delete;
    HIncrByFloatCommand &operator=(HIncrByFloatCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return false;
    }

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<HIncrByFloatCommand>(*this);
    }

    bool ProceedOnNonExistentObject() const override
    {
        return true;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *const) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    EloqString field_;
    long double incr_;
};

struct HMGetCommand : public HashCommand
{
    HMGetCommand() = default;
    explicit HMGetCommand(std::vector<EloqString> &&fields)
        : fields_(std::move(fields))
    {
    }

    HMGetCommand(const HMGetCommand &other) = delete;
    HMGetCommand(HMGetCommand &&other) = default;
    ~HMGetCommand() override = default;
    HMGetCommand &operator=(const HMGetCommand &rhs) = delete;
    HMGetCommand &operator=(HMGetCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    std::vector<EloqString> fields_;
};

struct HKeysCommand : public HashCommand
{
    HKeysCommand() = default;

    HKeysCommand(const HKeysCommand &other) = delete;
    HKeysCommand(HKeysCommand &&other) = default;
    ~HKeysCommand() override = default;
    HKeysCommand &operator=(const HKeysCommand &rhs) = delete;
    HKeysCommand &operator=(HKeysCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void Serialize(std::string &str) const override
    {
        uint8_t type = static_cast<uint8_t>(RedisCommandType::HKEYS);
        str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));
    }

    void Deserialize(std::string_view cmd_image) override
    {
        // Not need to do anything
    }

    void OutputResult(OutputHandler *reply) const override;
};

struct HValsCommand : public HashCommand
{
    HValsCommand() = default;

    HValsCommand(const HValsCommand &other) = delete;
    HValsCommand(HValsCommand &&other) = default;
    ~HValsCommand() override = default;
    HValsCommand &operator=(const HValsCommand &rhs) = delete;
    HValsCommand &operator=(HValsCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void Serialize(std::string &str) const override
    {
        uint8_t type = static_cast<uint8_t>(RedisCommandType::HVALS);
        str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));
    }

    void Deserialize(std::string_view cmd_image) override
    {
    }

    void OutputResult(OutputHandler *reply) const override;
};

struct HSetNxCommand : public HashCommand
{
    HSetNxCommand() = default;
    explicit HSetNxCommand(std::string_view key, std::string_view value)
        : key_(key), value_(value)
    {
    }

    HSetNxCommand(const HSetNxCommand &rhs);
    HSetNxCommand(HSetNxCommand &&rhs) = default;
    ~HSetNxCommand() override = default;
    HSetNxCommand &operator=(const HSetNxCommand &rhs) = delete;
    HSetNxCommand &operator=(HSetNxCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return false;
    }

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<HSetNxCommand>(*this);
    }

    bool ProceedOnNonExistentObject() const override
    {
        return true;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *obj_ptr) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    EloqString key_, value_;
};

struct HRandFieldCommand : public HashCommand
{
    HRandFieldCommand() : count_(1), with_count_(false), with_values_(false)
    {
    }
    HRandFieldCommand(int64_t count, bool with_values)
        : count_(count), with_count_(true), with_values_(with_values)
    {
    }

    HRandFieldCommand(const HRandFieldCommand &rhs) = delete;
    HRandFieldCommand(HRandFieldCommand &&rhs) noexcept = default;
    ~HRandFieldCommand() override = default;
    HRandFieldCommand &operator=(const HRandFieldCommand &rhs) = delete;
    HRandFieldCommand &operator=(HRandFieldCommand &&rhs) noexcept = delete;

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;
    void Serialize(std::string &str) const override;
    void Deserialize(std::string_view cmd_image) override;
    void OutputResult(OutputHandler *reply) const override;

    int64_t count_{0};
    bool with_count_{false};
    bool with_values_{false};
};

struct HScanCommand : public HashCommand
{
    HScanCommand() = default;
    explicit HScanCommand(int64_t cursor,
                          bool match,
                          EloqString &&pattern,
                          uint64_t count,
                          bool novalues)
        : novalues_(novalues),
          match_(match),
          pattern_(std::move(pattern)),
          count_(count),
          cursor_(cursor)
    {
    }

    HScanCommand(const HScanCommand &rhs) = delete;
    HScanCommand(HScanCommand &&rhs) = default;
    ~HScanCommand() override = default;
    HScanCommand &operator=(const HScanCommand &rhs) = delete;
    HScanCommand &operator=(HScanCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    bool novalues_{false};
    bool match_{false};
    EloqString pattern_{};
    uint64_t count_{0};
    int64_t cursor_{0};
};

struct HashSetCommand : public RedisCommand
{
    std::unique_ptr<txservice::TxRecord> CreateObject(
        const std::string *image) const override;

    std::unique_ptr<txservice::TxCommandResult> CreateCommandResult()
        const override
    {
        return std::make_unique<RedisHashSetResult>();
    }

    txservice::TxCommandResult *GetResult() override
    {
        return &result_;
    }

    static bool CheckTypeMatch(const txservice::TxRecord &obj);

    RedisHashSetResult result_;
};

struct SAddCommand : public HashSetCommand
{
    SAddCommand() = default;
    explicit SAddCommand(std::vector<EloqString> &&paras,
                         bool force_remove_add = false,
                         bool is_in_the_middle_stage = false)
        : vct_paras_(std::move(paras)),
          force_remove_add_(force_remove_add),
          is_in_the_middle_stage_(is_in_the_middle_stage)
    {
    }

    SAddCommand(const SAddCommand &rhs);
    SAddCommand(SAddCommand &&rhs) = default;
    ~SAddCommand() = default;
    SAddCommand &operator=(const SAddCommand &rhs) = delete;
    SAddCommand &operator=(SAddCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return false;
    }

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<SAddCommand>(*this);
    }

    bool ProceedOnNonExistentObject() const override
    {
        return true;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *obj_ptr) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    std::vector<EloqString> vct_paras_;
    // Some multi commands(for example sunionstore) need to call this command,
    //  If the object has exist with other data type or with elements, it will
    //  force to delete the old object first, then create a new hashset object.
    bool force_remove_add_{false};
    bool is_in_the_middle_stage_{false};
};

struct SMembersCommand : public HashSetCommand
{
    SMembersCommand() = default;

    SMembersCommand(const SMembersCommand &rhs) = delete;
    SMembersCommand(SMembersCommand &&rhs) = default;
    ~SMembersCommand() = default;
    SMembersCommand &operator=(const SMembersCommand &rhs) = delete;
    SMembersCommand &operator=(SMembersCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;
};

struct SRemCommand : public HashSetCommand
{
    SRemCommand() = default;
    explicit SRemCommand(std::vector<EloqString> &&paras)
        : vct_paras_(std::move(paras))
    {
    }

    SRemCommand(const SRemCommand &rhs);
    SRemCommand(SRemCommand &&rhs) = default;
    ~SRemCommand() override = default;
    SRemCommand &operator=(const SRemCommand &rhs) = delete;
    SRemCommand &operator=(SRemCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return false;
    }

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<SRemCommand>(*this);
    }

    bool ProceedOnNonExistentObject() const override
    {
        return true;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *obj_ptr) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override
    {
        if (result_.err_code_ == RD_OK)
        {
            reply->OnInt(result_.ret_);
        }
        else
        {
            reply->OnError(redis_get_error_messages(result_.err_code_));
        }
    }

    std::vector<EloqString> vct_paras_;
};

struct SCardCommand : public HashSetCommand
{
    SCardCommand() = default;

    SCardCommand(const SCardCommand &rhs) = delete;
    SCardCommand(SCardCommand &&rhs) = default;
    ~SCardCommand() override = default;
    SCardCommand &operator=(const SCardCommand &rhs) = delete;
    SCardCommand &operator=(SCardCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;
};

struct SIsMemberCommand : public HashSetCommand
{
    SIsMemberCommand() = default;
    explicit SIsMemberCommand(std::string_view member) : str_member_(member)
    {
    }

    SIsMemberCommand(const SIsMemberCommand &rhs) = delete;
    SIsMemberCommand(SIsMemberCommand &&rhs) = default;
    ~SIsMemberCommand() = default;
    SIsMemberCommand &operator=(const SIsMemberCommand &rhs) = delete;
    SIsMemberCommand &operator=(SIsMemberCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    EloqString str_member_;
};

struct SMIsMemberCommand : public HashSetCommand
{
    SMIsMemberCommand() = default;
    explicit SMIsMemberCommand(std::vector<EloqString> &&members)
        : members_(std::move(members))
    {
    }

    SMIsMemberCommand(const SMIsMemberCommand &rhs) = delete;
    SMIsMemberCommand(SMIsMemberCommand &&rhs) = default;
    ~SMIsMemberCommand() = default;
    SMIsMemberCommand &operator=(const SMIsMemberCommand &rhs) = delete;
    SMIsMemberCommand &operator=(SMIsMemberCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    std::vector<EloqString> members_;
};

struct SRandMemberCommand : public HashSetCommand
{
    SRandMemberCommand() = default;
    explicit SRandMemberCommand(int64_t count, bool count_provided)
        : count_(count), count_provided_(count_provided)
    {
    }

    SRandMemberCommand(const SRandMemberCommand &rhs) = delete;
    SRandMemberCommand(SRandMemberCommand &&rhs) = default;
    ~SRandMemberCommand() = default;
    SRandMemberCommand &operator=(const SRandMemberCommand &rhs) = delete;
    SRandMemberCommand &operator=(SRandMemberCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    int64_t count_;
    // All int64_t numbers have a meaning when used as count; therefore, we
    // cannot find a special number to represent that count is not provided.
    bool count_provided_;
};

struct SPopCommand : public HashSetCommand
{
    SPopCommand() = default;
    explicit SPopCommand(int32_t count) : count_(count)
    {
    }

    SPopCommand(const SPopCommand &rhs) = default;
    SPopCommand(SPopCommand &&rhs) = default;
    ~SPopCommand() override = default;
    SPopCommand &operator=(const SPopCommand &rhs) = delete;
    SPopCommand &operator=(SPopCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return false;
    }

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<SPopCommand>(*this);
    }

    bool ProceedOnNonExistentObject() const override
    {
        return true;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *obj_ptr) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    int64_t count_{-1};
};

// Helper command for ZUNION/ZINTER/ZDIFF
// ZUNION/ZINTER/ZDIFF supports computation with regular sets.
// So we need to add a helper command that can scan both a normal set and a zset
struct SZScanCommand : public RedisCommand
{
    SZScanCommand() = default;

    explicit SZScanCommand(bool withscores) : withscores_(withscores)
    {
    }

    SZScanCommand(const SZScanCommand &rhs) = delete;
    SZScanCommand(SZScanCommand &&rhs) = default;
    ~SZScanCommand() = default;
    SZScanCommand &operator=(const SZScanCommand &rhs) = delete;
    SZScanCommand &operator=(SZScanCommand &&rhs) = delete;

    std::unique_ptr<txservice::TxRecord> CreateObject(
        const std::string *image) const override
    {
        return nullptr;
    }

    std::unique_ptr<txservice::TxCommandResult> CreateCommandResult()
        const override
    {
        return std::make_unique<RedisSetScanResult>();
    }

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    txservice::TxCommandResult *GetResult() override
    {
        return &set_scan_result_;
    }

    bool withscores_{false};
    RedisSetScanResult set_scan_result_;
};

struct SScanCommand : public HashSetCommand
{
    SScanCommand() = default;
    explicit SScanCommand(int64_t cursor,
                          bool match,
                          std::string_view pattern,
                          uint64_t count)
        : match_(match), pattern_(pattern), count_(count), cursor_(cursor)
    {
    }

    SScanCommand(const SScanCommand &rhs) = delete;
    SScanCommand(SScanCommand &&rhs) = default;
    ~SScanCommand() = default;
    SScanCommand &operator=(const SScanCommand &rhs) = delete;
    SScanCommand &operator=(SScanCommand &&rhs) = delete;

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    bool match_{false};
    EloqString pattern_{};
    uint64_t count_{0};
    int64_t cursor_{0};
};

struct MSetCommand : public RedisMultiObjectCommand
{
public:
    MSetCommand() = default;
    MSetCommand(std::vector<EloqKey> &&keys, std::vector<SetCommand> &&cmds)
        : keys_(std::move(keys)), cmds_(std::move(cmds))
    {
        assert(keys_.size() == cmds_.size());
        vct_key_ptrs_.emplace_back();
        vct_key_ptrs_[0].reserve(keys_.size());
        vct_cmd_ptrs_.emplace_back();
        vct_cmd_ptrs_[0].reserve(cmds_.size());

        for (const auto &key : keys_)
        {
            vct_key_ptrs_[0].emplace_back(&key);
        }
        for (auto &cmd : cmds_)
        {
            vct_cmd_ptrs_[0].emplace_back(&cmd);
        }
    }

    void OutputResult(OutputHandler *reply) const override
    {
        reply->OnStatus(redis_get_error_messages(RD_OK));
    }

    std::vector<EloqKey> keys_;
    std::vector<SetCommand> cmds_;
};

struct MGetCommand : public RedisMultiObjectCommand
{
public:
    MGetCommand() = default;
    MGetCommand(
        std::vector<EloqKey> &&keys,
        std::vector<GetCommand> &&cmds,
        std::optional<std::vector<GetCommand *>> raw_cmds = std::nullopt)
        : keys_(std::move(keys)),
          cmds_(std::move(cmds)),
          raw_cmds_(raw_cmds ? std::move(*raw_cmds)
                             : std::vector<GetCommand *>())
    {
        assert(keys_.size() == cmds_.size());
        vct_key_ptrs_.emplace_back();
        vct_key_ptrs_[0].reserve(keys_.size());
        vct_cmd_ptrs_.emplace_back();
        vct_cmd_ptrs_[0].reserve(cmds_.size());

        for (const auto &key : keys_)
        {
            vct_key_ptrs_[0].emplace_back(&key);
        }
        for (auto &cmd : cmds_)
        {
            vct_cmd_ptrs_[0].emplace_back(&cmd);
        }
    }

    void OutputResult(OutputHandler *reply) const override
    {
        reply->OnArrayStart(raw_cmds_.size());
        for (const auto cmd : raw_cmds_)
        {
            int32_t err_code = cmd->result_.err_code_;
            if (err_code == RD_OK || err_code == RD_NIL)
            {
                cmd->OutputResult(reply);
            }
            else
            {
                assert(err_code == RD_ERR_WRONG_TYPE);
                // MGET treat wrong type as nil
                reply->OnNil();
            }
        }
        reply->OnArrayEnd();
    }

    std::vector<EloqKey> keys_;
    std::vector<GetCommand> cmds_;
    // for repeated key, store the original sequence for OutputResult
    std::vector<GetCommand *> raw_cmds_;
};

struct MDelCommand : public RedisMultiObjectCommand
{
public:
    MDelCommand() = default;
    MDelCommand(std::vector<EloqKey> &&keys, std::vector<DelCommand> &&cmds)
        : keys_(std::move(keys)), cmds_(std::move(cmds))
    {
        assert(keys_.size() == cmds_.size());
        vct_key_ptrs_.emplace_back();
        vct_key_ptrs_[0].reserve(keys_.size());
        vct_cmd_ptrs_.emplace_back();
        vct_cmd_ptrs_[0].reserve(cmds_.size());

        for (const auto &key : keys_)
        {
            vct_key_ptrs_[0].emplace_back(&key);
        }
        for (auto &cmd : cmds_)
        {
            vct_cmd_ptrs_[0].emplace_back(&cmd);
        }
    }

    void OutputResult(OutputHandler *reply) const override
    {
        int64_t cnt = 0;
        for (const auto &cmd : cmds_)
        {
            cnt += cmd.result_.int_val_;
        }
        reply->OnInt(cnt);
    }

    std::vector<EloqKey> keys_;
    std::vector<DelCommand> cmds_;
};

struct MExistsCommand : public RedisMultiObjectCommand
{
public:
    MExistsCommand() = default;
    MExistsCommand(std::vector<EloqKey> &&keys,
                   std::vector<ExistsCommand> &&cmds)
        : keys_(std::move(keys)), cmds_(std::move(cmds))
    {
        assert(keys_.size() == cmds_.size());
        vct_key_ptrs_.emplace_back();
        vct_key_ptrs_[0].reserve(keys_.size());
        vct_cmd_ptrs_.emplace_back();
        vct_cmd_ptrs_[0].reserve(cmds_.size());

        for (const auto &key : keys_)
        {
            vct_key_ptrs_[0].emplace_back(&key);
        }
        for (auto &cmd : cmds_)
        {
            vct_cmd_ptrs_[0].emplace_back(&cmd);
        }
    }

    void OutputResult(OutputHandler *reply) const override
    {
        int64_t cnt = 0;
        for (const auto &cmd : cmds_)
        {
            cnt += cmd.result_.int_val_;
        }
        reply->OnInt(cnt);
    }

    std::vector<EloqKey> keys_;
    std::vector<ExistsCommand> cmds_;
};

struct SDiffCommand : public RedisMultiObjectCommand
{
public:
    SDiffCommand() = default;
    SDiffCommand(std::vector<EloqKey> &&keys,
                 std::vector<SMembersCommand> &&cmds)
        : keys_(std::move(keys)), cmds_(std::move(cmds))
    {
        assert(keys_.size() == cmds_.size());
        vct_key_ptrs_.emplace_back();
        vct_key_ptrs_[0].reserve(keys_.size());
        vct_cmd_ptrs_.emplace_back();
        vct_cmd_ptrs_[0].reserve(cmds_.size());

        for (const auto &key : keys_)
        {
            vct_key_ptrs_[0].emplace_back(&key);
        }
        for (auto &cmd : cmds_)
        {
            vct_cmd_ptrs_[0].emplace_back(&cmd);
        }
    }

    void OutputResult(OutputHandler *reply) const override;

    std::vector<EloqKey> keys_;
    std::vector<SMembersCommand> cmds_;
};

struct ZRangeStoreCommand : public RedisMultiObjectCommand
{
public:
    ZRangeStoreCommand() = default;
    ZRangeStoreCommand(const std::string_view &key_search,
                       ZRangeConstraint &constraint,
                       ZRangeSpec &&spec,
                       const std::string_view &key)
        : key_search_(std::make_unique<EloqKey>(key_search)),
          cmd_search_(
              std::make_unique<ZRangeCommand>(constraint, std::move(spec))),
          key_store_(std::make_unique<EloqKey>(key)),
          cmd_store_(std::make_unique<ZAddCommand>())

    {
        cmd_store_->params_.flags = ZADD_FORCE_CLEAR;
        vct_key_ptrs_.resize(2);
        vct_cmd_ptrs_.resize(2);
        vct_key_ptrs_[0].emplace_back(key_search_.get());
        vct_cmd_ptrs_[0].emplace_back(cmd_search_.get());
        vct_key_ptrs_[1].emplace_back(key_store_.get());
        vct_cmd_ptrs_[1].emplace_back(cmd_store_.get());
    }

    ZRangeStoreCommand(const ZRangeStoreCommand &rhs) = delete;
    ZRangeStoreCommand(ZRangeStoreCommand &&rhs) = default;
    ~ZRangeStoreCommand() override = default;
    ZRangeStoreCommand &operator=(const ZRangeStoreCommand &rhs) = delete;
    ZRangeStoreCommand &operator=(ZRangeStoreCommand &&rhs) = delete;

    bool HandleMiddleResult() override;
    void OutputResult(OutputHandler *reply) const override;

    std::unique_ptr<EloqKey> key_search_;
    std::unique_ptr<ZRangeCommand> cmd_search_;

    std::unique_ptr<EloqKey> key_store_;
    std::unique_ptr<ZAddCommand> cmd_store_;
};

struct SDiffStoreCommand : public RedisMultiObjectCommand
{
public:
    SDiffStoreCommand() = default;
    SDiffStoreCommand(std::vector<EloqKey> &&keys,
                      std::vector<SMembersCommand> &&cmds,
                      const std::string_view &key)
        : keys_(std::move(keys)),
          cmds_(std::move(cmds)),
          key_store_(std::make_unique<EloqKey>(key)),
          cmd_store_(std::make_unique<SAddCommand>(
              std::vector<EloqString>(), true, true))
    {
        assert(keys_.size() == cmds_.size());
        vct_key_ptrs_.resize(2);
        vct_key_ptrs_[0].reserve(keys_.size());
        vct_cmd_ptrs_.resize(2);
        vct_cmd_ptrs_[0].reserve(cmds_.size());

        for (const auto &key : keys_)
        {
            vct_key_ptrs_[0].emplace_back(&key);
        }
        for (auto &cmd : cmds_)
        {
            vct_cmd_ptrs_[0].push_back(&cmd);
        }

        vct_key_ptrs_[1].emplace_back(key_store_.get());
        vct_cmd_ptrs_[1].emplace_back(cmd_store_.get());
    }

    bool HandleMiddleResult() override;
    void OutputResult(OutputHandler *reply) const override;

    std::vector<EloqKey> keys_;
    std::vector<SMembersCommand> cmds_;

    std::unique_ptr<EloqKey> key_store_;
    std::unique_ptr<SAddCommand> cmd_store_;
};

struct SInterCommand : public RedisMultiObjectCommand
{
public:
    SInterCommand() = default;
    SInterCommand(std::vector<EloqKey> &&keys,
                  std::vector<SMembersCommand> &&cmds)
        : keys_(std::move(keys)), cmds_(std::move(cmds))
    {
        assert(keys_.size() == cmds_.size());
        vct_key_ptrs_.resize(1);
        vct_key_ptrs_[0].reserve(keys_.size());
        vct_cmd_ptrs_.resize(1);
        vct_cmd_ptrs_[0].reserve(cmds_.size());

        for (const auto &key : keys_)
        {
            vct_key_ptrs_[0].emplace_back(&key);
        }
        for (auto &cmd : cmds_)
        {
            vct_cmd_ptrs_[0].emplace_back(&cmd);
        }
    }

    void OutputResult(OutputHandler *reply) const override;

    std::vector<EloqKey> keys_;
    std::vector<SMembersCommand> cmds_;
};

struct SInterCardCommand : public RedisMultiObjectCommand
{
public:
    SInterCardCommand() = default;
    SInterCardCommand(std::vector<EloqKey> &&keys,
                      std::vector<SMembersCommand> &&cmds,
                      int64_t limit)
        : keys_(std::move(keys)), cmds_(std::move(cmds)), limit_(limit)
    {
        assert(keys_.size() == cmds_.size());
        vct_key_ptrs_.resize(1);
        vct_key_ptrs_[0].reserve(keys_.size());
        vct_cmd_ptrs_.resize(1);
        vct_cmd_ptrs_[0].reserve(cmds_.size());

        for (const auto &key : keys_)
        {
            vct_key_ptrs_[0].emplace_back(&key);
        }
        for (auto &cmd : cmds_)
        {
            vct_cmd_ptrs_[0].emplace_back(&cmd);
        }
    }

    void OutputResult(OutputHandler *reply) const override;

    std::vector<EloqKey> keys_;
    std::vector<SMembersCommand> cmds_;
    int64_t limit_{INT64_MAX};
};

struct SInterStoreCommand : public RedisMultiObjectCommand
{
public:
    SInterStoreCommand() = default;
    SInterStoreCommand(std::vector<EloqKey> &&keys,
                       std::vector<SMembersCommand> &&cmds,
                       const std::string_view &key)
        : keys_(std::move(keys)),
          cmds_(std::move(cmds)),
          key_store_(std::make_unique<EloqKey>(key)),
          cmd_store_(std::make_unique<SAddCommand>(
              std::vector<EloqString>(), true, true))
    {
        assert(keys_.size() == cmds_.size());
        vct_key_ptrs_.resize(2);
        vct_key_ptrs_[0].reserve(keys_.size());
        vct_cmd_ptrs_.resize(2);
        vct_cmd_ptrs_[0].reserve(cmds_.size());

        for (const auto &key : keys_)
        {
            vct_key_ptrs_[0].emplace_back(&key);
        }
        for (auto &cmd : cmds_)
        {
            vct_cmd_ptrs_[0].emplace_back(&cmd);
        }

        vct_key_ptrs_[1].emplace_back(key_store_.get());
        vct_cmd_ptrs_[1].emplace_back(cmd_store_.get());
    }

    bool HandleMiddleResult() override;
    void OutputResult(OutputHandler *reply) const override;

    std::vector<EloqKey> keys_;
    std::vector<SMembersCommand> cmds_;

    std::unique_ptr<EloqKey> key_store_;
    std::unique_ptr<SAddCommand> cmd_store_;
};

struct MSetNxCommand : public RedisMultiObjectCommand
{
public:
    MSetNxCommand() = default;
    ~MSetNxCommand() override = default;
    MSetNxCommand(MSetNxCommand &&) = default;
    MSetNxCommand(const MSetNxCommand &) = delete;
    MSetNxCommand &operator=(const MSetNxCommand &) = delete;
    MSetNxCommand &operator=(MSetNxCommand &&) = delete;

    MSetNxCommand(std::vector<EloqKey> &&keys,
                  std::vector<ExistsCommand> &&exist_cmds,
                  std::vector<SetCommand> &&set_cmds)
        : keys_(std::move(keys)),
          exist_cmds_(std::move(exist_cmds)),
          set_cmds_(std::move(set_cmds))
    {
        assert(keys_.size() == exist_cmds_.size() &&
               keys_.size() == set_cmds_.size());
        vct_key_ptrs_.resize(2);
        vct_key_ptrs_[0].reserve(keys_.size());
        vct_key_ptrs_[1].reserve(keys_.size());
        vct_cmd_ptrs_.resize(2);
        vct_cmd_ptrs_[0].reserve(exist_cmds_.size());
        vct_cmd_ptrs_[1].reserve(set_cmds_.size());

        for (size_t idx = 0; idx < keys_.size(); ++idx)
        {
            vct_key_ptrs_[0].emplace_back(&keys_[idx]);
            vct_cmd_ptrs_[0].emplace_back(&exist_cmds_[idx]);

            vct_key_ptrs_[1].emplace_back(&keys_[idx]);
            vct_cmd_ptrs_[1].emplace_back(&set_cmds_[idx]);
        }
    }

    bool HandleMiddleResult() override;
    void OutputResult(OutputHandler *reply) const override;
    bool IsPassed() const override
    {
        if (curr_step_ == 0)
        {
            for (const auto &cmd : exist_cmds_)
            {
                assert(cmd.result_.err_code_ == RD_OK ||
                       cmd.result_.err_code_ == RD_NIL);
                (void) cmd;
            }
        }
        else
        {
            for (const auto &cmd : set_cmds_)
            {
                assert(cmd.result_.err_code_ == RD_OK);
                (void) cmd;
            }
        }

        // This command always returns true. So we dont't abort transaction due
        // this command failed
        return true;
    }

private:
    std::vector<EloqKey> keys_;

    // MSETNX is atomic, so all given keys are set at once. It is not possible
    // for clients to see that some of the keys were updated while others are
    // unchanged.
    std::vector<ExistsCommand> exist_cmds_;
    std::vector<SetCommand> set_cmds_;
};

struct SMoveCommand : public RedisMultiObjectCommand
{
public:
    SMoveCommand() = default;
    SMoveCommand(EloqKey &&key_src,
                 SRemCommand &&cmd_src,
                 EloqKey &&key_dst,
                 SAddCommand &&cmd_dst)
        : key_src_(std::make_unique<EloqKey>(std::move(key_src))),
          cmd_src_(std::make_unique<SRemCommand>(std::move(cmd_src))),
          key_dst_(std::make_unique<EloqKey>(std::move(key_dst))),
          cmd_dst_(std::make_unique<SAddCommand>(std::move(cmd_dst)))
    {
        vct_key_ptrs_.resize(2);
        vct_cmd_ptrs_.resize(2);

        vct_key_ptrs_[0].emplace_back(key_src_.get());
        vct_cmd_ptrs_[0].emplace_back(cmd_src_.get());

        vct_key_ptrs_[1].emplace_back(key_dst_.get());
        vct_cmd_ptrs_[1].emplace_back(cmd_dst_.get());
    }

    bool HandleMiddleResult() override;
    void OutputResult(OutputHandler *reply) const override;
    bool IsPassed() const override
    {
        if (cmd_src_->result_.err_code_ != RD_OK ||
            cmd_dst_->result_.err_code_ != RD_OK)
        {
            return false;
        }
        return true;
    }

    std::unique_ptr<EloqKey> key_src_;
    std::unique_ptr<SRemCommand> cmd_src_;

    std::unique_ptr<EloqKey> key_dst_;
    std::unique_ptr<SAddCommand> cmd_dst_;
};

struct LMoveCommand : public RedisMultiObjectCommand
{
public:
    LMoveCommand() = default;
    LMoveCommand(std::string_view key_src,
                 bool is_left_for_src,
                 std::string_view key_dst,
                 bool is_left_for_dst)
        : key_src_(std::make_unique<EloqKey>(key_src)),
          cmd_src_(std::make_unique<LMovePopCommand>(is_left_for_src)),
          key_dst_(std::make_unique<EloqKey>(key_dst)),
          cmd_dst_(std::make_unique<LMovePushCommand>(is_left_for_dst))
    {
        vct_key_ptrs_.resize(2);
        vct_cmd_ptrs_.resize(2);

        vct_key_ptrs_[0].emplace_back(key_src_.get());
        vct_cmd_ptrs_[0].emplace_back(cmd_src_.get());

        vct_key_ptrs_[1].emplace_back(key_dst_.get());
        vct_cmd_ptrs_[1].emplace_back(cmd_dst_.get());
    }

    bool HandleMiddleResult() override;
    void OutputResult(OutputHandler *reply) const override;
    bool IsPassed() const override
    {
        if (cmd_src_->result_.err_code_ != RD_OK ||
            cmd_dst_->result_.err_code_ != RD_OK)
        {
            return false;
        }
        return true;
    }

    std::unique_ptr<EloqKey> key_src_;
    std::unique_ptr<LMovePopCommand> cmd_src_;

    std::unique_ptr<EloqKey> key_dst_;
    std::unique_ptr<LMovePushCommand> cmd_dst_;
};

struct RPopLPushCommand : public LMoveCommand
{
public:
    RPopLPushCommand() = default;
    RPopLPushCommand(std::string_view key_src, std::string_view key_dst)
        : LMoveCommand(key_src, false, key_dst, true)
    {
    }
};

enum class AggregateType
{
    SUM = 0,
    MIN,
    MAX
};

class AggregateUtil
{
public:
    static auto GetUpdateFunction(AggregateType aggregate_type)
    {
        switch (aggregate_type)
        {
        case AggregateType::SUM:
        {
            return AggregateUtil::UpdateSumValue;
            break;
        }
        case AggregateType::MIN:
        {
            return AggregateUtil::UpdateMinValue;
            break;
        }
        case AggregateType::MAX:
        {
            return AggregateUtil::UpdateMaxValue;
            break;
        }
        default:
        {
            assert(false && "Dead branch");
            return AggregateUtil::UpdateSumValue;
            break;
        }
        }
    }

    static double CaculateNewScore(double score, double weight)
    {
        if (weight == 0)
        {
            return 0;
        }
        double new_score = score;

        if (score == std::numeric_limits<double>::infinity())
        {
            if (weight < 0)
            {
                new_score = std::numeric_limits<double>::infinity() * -1;
            }
        }
        else if (score == std::numeric_limits<double>::infinity() * -1)
        {
            if (weight < 0)
            {
                new_score = std::numeric_limits<double>::infinity();
            }
        }
        else
        {
            new_score = score * weight;
        }
        return new_score;
    }

private:
    static void UpdateMinValue(double &dest, double src)
    {
        dest = std::min(dest, src);
    }

    static void UpdateMaxValue(double &dest, double src)
    {
        dest = std::max(dest, src);
    }

    static void UpdateSumValue(double &dest, double src)
    {
        if (src * -1 == dest)
        {
            dest = 0;
        }
        else
        {
            dest += src;
        }
    }
};

struct ZUnionCommand : public RedisMultiObjectCommand
{
public:
    ZUnionCommand() = default;
    ZUnionCommand(const ZUnionCommand &) = delete;
    ZUnionCommand &operator=(const ZUnionCommand &) = delete;
    ZUnionCommand(ZUnionCommand &&) = default;
    ZUnionCommand &operator=(ZUnionCommand &&) = delete;
    ~ZUnionCommand() override = default;

    ZUnionCommand(std::vector<EloqKey> &&keys,
                  std::vector<SZScanCommand> &&cmds,
                  AggregateType aggregate_type,
                  bool opt_withscores,
                  std::vector<double> weights)
        : aggregate_type_(aggregate_type),
          opt_withscores_(opt_withscores),
          weights_(std::move(weights)),
          keys_(std::move(keys)),
          cmds_(std::move(cmds))
    {
        assert(keys_.size() == cmds_.size());
        vct_key_ptrs_.resize(1);
        vct_key_ptrs_[0].reserve(keys_.size());
        vct_cmd_ptrs_.resize(1);
        vct_cmd_ptrs_[0].reserve(cmds_.size());

        for (const auto &key : keys_)
        {
            vct_key_ptrs_[0].emplace_back(&key);
        }

        for (auto &cmd : cmds_)
        {
            vct_cmd_ptrs_[0].emplace_back(&cmd);
        }
    }

    void OutputResult(OutputHandler *reply) const override;

    AggregateType aggregate_type_{AggregateType::SUM};
    bool opt_withscores_{false};
    std::vector<double> weights_;
    std::vector<EloqKey> keys_;
    std::vector<SZScanCommand> cmds_;
};

struct ZUnionStoreCommand : public RedisMultiObjectCommand
{
public:
    ZUnionStoreCommand() = default;

    ZUnionStoreCommand(const ZUnionCommand &) = delete;
    ZUnionStoreCommand &operator=(const ZUnionCommand &) = delete;
    ZUnionStoreCommand(ZUnionStoreCommand &&) = default;
    ZUnionStoreCommand &operator=(ZUnionStoreCommand &&) = delete;
    ~ZUnionStoreCommand() override = default;

    explicit ZUnionStoreCommand(std::vector<EloqKey> &&keys,
                                std::vector<SZScanCommand> &&cmds,
                                AggregateType aggregate_type,
                                std::vector<double> weights,
                                const std::string_view &key_store)
        : aggregate_type_(aggregate_type),
          weights_(std::move(weights)),
          keys_(std::move(keys)),
          cmds_(std::move(cmds)),
          key_store_(std::make_unique<EloqKey>(key_store)),
          cmd_store_(std::make_unique<ZAddCommand>())
    {
        cmd_store_->is_in_the_middle_stage_ = true;

        assert(keys_.size() == cmds_.size());
        vct_key_ptrs_.resize(2);
        vct_key_ptrs_[0].reserve(keys_.size());
        vct_cmd_ptrs_.resize(2);
        vct_cmd_ptrs_[0].reserve(cmds_.size());

        for (const auto &key : keys_)
        {
            vct_key_ptrs_[0].emplace_back(&key);
        }

        for (auto &cmd : cmds_)
        {
            vct_cmd_ptrs_[0].emplace_back(&cmd);
        }

        vct_key_ptrs_[1].emplace_back(key_store_.get());
        vct_cmd_ptrs_[1].emplace_back(cmd_store_.get());
    }

    bool HandleMiddleResult() override;
    void OutputResult(OutputHandler *reply) const override;

    bool IsPassed() const override
    {
        if (curr_step_ == 0)
        {
            for (const auto &cmd : cmds_)
            {
                if (cmd.set_scan_result_.err_code_ == RD_ERR_WRONG_TYPE)
                {
                    return false;
                }
            }
        }
        else
        {
            if (cmd_store_->zset_result_.err_code_ == RD_ERR_WRONG_TYPE)
            {
                return false;
            }
        }
        return true;
    }

private:
    AggregateType aggregate_type_{AggregateType::SUM};
    std::vector<double> weights_;
    std::vector<EloqKey> keys_;
    std::vector<SZScanCommand> cmds_;

    std::unique_ptr<EloqKey> key_store_;
    std::unique_ptr<ZAddCommand> cmd_store_;
};

struct ZInterCommand : public RedisMultiObjectCommand
{
public:
    ZInterCommand() = default;
    ZInterCommand(const ZInterCommand &) = delete;
    ZInterCommand &operator=(const ZInterCommand &) = delete;
    ZInterCommand(ZInterCommand &&) = default;
    ZInterCommand &operator=(ZInterCommand &&) = delete;
    ~ZInterCommand() override = default;

    ZInterCommand(std::vector<EloqKey> &&keys,
                  std::vector<SZScanCommand> &&cmds,
                  AggregateType aggregate_type,
                  bool opt_withscores,
                  std::vector<double> weights)
        : aggregate_type_(aggregate_type),
          opt_withscores_(opt_withscores),
          weights_(std::move(weights)),
          keys_(std::move(keys)),
          cmds_(std::move(cmds))
    {
        assert(keys_.size() == cmds_.size());

        vct_key_ptrs_.resize(1);
        vct_key_ptrs_[0].reserve(keys_.size());
        vct_cmd_ptrs_.resize(1);
        vct_cmd_ptrs_[0].reserve(cmds_.size());

        for (const auto &key : keys_)
        {
            vct_key_ptrs_[0].emplace_back(&key);
        }

        for (auto &cmd : cmds_)
        {
            vct_cmd_ptrs_[0].emplace_back(&cmd);
        }
    }

    void OutputResult(OutputHandler *reply) const override;

private:
    AggregateType aggregate_type_{AggregateType::SUM};
    bool opt_withscores_{false};
    std::vector<double> weights_;
    std::vector<EloqKey> keys_;
    std::vector<SZScanCommand> cmds_;
};

struct ZInterStoreCommand : public RedisMultiObjectCommand
{
public:
    ZInterStoreCommand() = default;

    ZInterStoreCommand(const ZInterStoreCommand &) = delete;
    ZInterStoreCommand &operator=(const ZInterStoreCommand &) = delete;
    ZInterStoreCommand(ZInterStoreCommand &&) = default;
    ZInterStoreCommand &operator=(ZInterStoreCommand &&) = delete;
    ~ZInterStoreCommand() override = default;

    ZInterStoreCommand(std::vector<EloqKey> &&keys,
                       std::vector<SZScanCommand> &&cmds,
                       AggregateType aggregate_type,
                       std::vector<double> &&weights,
                       const std::string_view &key_store)
        : keys_(std::move(keys)),
          cmds_(std::move(cmds)),
          aggregate_type_(aggregate_type),
          weights_(weights),
          key_store_(std::make_unique<EloqKey>(key_store)),
          cmd_store_(std::make_unique<ZAddCommand>())
    {
        cmd_store_->is_in_the_middle_stage_ = true;

        assert(keys_.size() == cmds_.size());
        vct_key_ptrs_.resize(2);
        vct_cmd_ptrs_.resize(2);
        vct_key_ptrs_[0].reserve(keys_.size());
        vct_cmd_ptrs_[0].reserve(cmds_.size());

        for (const auto &key : keys_)
        {
            vct_key_ptrs_[0].emplace_back(&key);
        }

        for (auto &cmd : cmds_)
        {
            vct_cmd_ptrs_[0].emplace_back(&cmd);
        }

        vct_key_ptrs_[1].emplace_back(key_store_.get());
        vct_cmd_ptrs_[1].emplace_back(cmd_store_.get());
    }

    bool HandleMiddleResult() override;
    void OutputResult(OutputHandler *reply) const override;

    bool IsPassed() const override
    {
        if (curr_step_ == 0)
        {
            for (const auto &cmd : cmds_)
            {
                if (cmd.set_scan_result_.err_code_ == RD_ERR_WRONG_TYPE)
                {
                    return false;
                }
            }
        }
        else
        {
            if (cmd_store_->zset_result_.err_code_ == RD_ERR_WRONG_TYPE)
            {
                return false;
            }
        }
        return true;
    }

private:
    std::vector<EloqKey> keys_;
    std::vector<SZScanCommand> cmds_;
    AggregateType aggregate_type_{AggregateType::SUM};
    std::vector<double> weights_;
    std::unique_ptr<EloqKey> key_store_;
    std::unique_ptr<ZAddCommand> cmd_store_;
};

struct ZInterCardCommand : public RedisMultiObjectCommand
{
public:
    ZInterCardCommand() = default;
    ZInterCardCommand(const ZInterCardCommand &) = delete;
    ZInterCardCommand &operator=(const ZInterCardCommand &) = delete;
    ZInterCardCommand(ZInterCardCommand &&) = default;
    ZInterCardCommand &operator=(ZInterCardCommand &&) = delete;
    ~ZInterCardCommand() override = default;

    ZInterCardCommand(std::vector<EloqKey> &&keys,
                      std::vector<SZScanCommand> &&cmds,
                      int64_t limit)
        : keys_(std::move(keys)), cmds_(std::move(cmds)), limit_(limit)
    {
        assert(keys_.size() == cmds_.size());
        vct_key_ptrs_.resize(1);
        vct_cmd_ptrs_.resize(1);
        vct_key_ptrs_[0].reserve(keys_.size());
        vct_cmd_ptrs_[0].reserve(cmds_.size());

        for (const auto &key : keys_)
        {
            vct_key_ptrs_[0].emplace_back(&key);
        }

        for (auto &cmd : cmds_)
        {
            vct_cmd_ptrs_[0].emplace_back(&cmd);
        }
    }

    void OutputResult(OutputHandler *reply) const override;

private:
    std::vector<EloqKey> keys_;
    std::vector<SZScanCommand> cmds_;
    int64_t limit_{0};
};

struct SUnionCommand : public RedisMultiObjectCommand
{
public:
    SUnionCommand() = default;
    SUnionCommand(std::vector<EloqKey> &&keys,
                  std::vector<SMembersCommand> &&cmds)
        : keys_(std::move(keys)), cmds_(std::move(cmds))
    {
        assert(keys_.size() == cmds_.size());
        vct_key_ptrs_.resize(1);
        vct_key_ptrs_[0].reserve(keys_.size());
        vct_cmd_ptrs_.resize(1);
        vct_cmd_ptrs_[0].reserve(cmds_.size());

        for (const auto &key : keys_)
        {
            vct_key_ptrs_[0].emplace_back(&key);
        }
        for (auto &cmd : cmds_)
        {
            vct_cmd_ptrs_[0].emplace_back(&cmd);
        }
    }

    void OutputResult(OutputHandler *reply) const override;

    std::vector<EloqKey> keys_;
    std::vector<SMembersCommand> cmds_;
};
struct SUnionStoreCommand : public RedisMultiObjectCommand
{
public:
    SUnionStoreCommand() = default;
    SUnionStoreCommand(std::vector<EloqKey> &&keys,
                       std::vector<SMembersCommand> &&cmds,
                       const std::string_view &key)
        : keys_(std::move(keys)),
          cmds_(std::move(cmds)),
          key_store_(std::make_unique<EloqKey>(key)),
          cmd_store_(std::make_unique<SAddCommand>(
              std::vector<EloqString>(), true, true))
    {
        assert(keys_.size() == cmds_.size());
        vct_key_ptrs_.resize(2);
        vct_key_ptrs_[0].reserve(keys_.size());
        vct_cmd_ptrs_.resize(2);
        vct_cmd_ptrs_[0].reserve(cmds_.size());

        for (const auto &key : keys_)
        {
            vct_key_ptrs_[0].emplace_back(&key);
        }
        for (auto &cmd : cmds_)
        {
            vct_cmd_ptrs_[0].emplace_back(&cmd);
        }

        vct_key_ptrs_[1].emplace_back(key_store_.get());
        vct_cmd_ptrs_[1].emplace_back(cmd_store_.get());
    }

    bool HandleMiddleResult() override;
    void OutputResult(OutputHandler *reply) const override;

    std::vector<EloqKey> keys_;
    std::vector<SMembersCommand> cmds_;

    std::unique_ptr<EloqKey> key_store_;
    std::unique_ptr<SAddCommand> cmd_store_;
};

/**
 * StoreListCommand is not a real redis command, but a subcommand of sort
 * command. It is used to store the sorted list.
 */
struct StoreListCommand : public RedisCommand
{
    StoreListCommand()
    {
        result_.err_code_ = RD_OK;
    };
    explicit StoreListCommand(std::vector<EloqString> elements)
        : elements_(std::move(elements))
    {
        SetVolatile();
        result_.err_code_ = RD_OK;
    }

    StoreListCommand(const StoreListCommand &rhs);
    StoreListCommand(StoreListCommand &&rhs) = default;
    StoreListCommand &operator=(const StoreListCommand &rhs) = delete;
    StoreListCommand &operator=(StoreListCommand &&rhs) = delete;

    std::unique_ptr<txservice::TxRecord> CreateObject(
        const std::string *image) const override;

    std::unique_ptr<txservice::TxCommandResult> CreateCommandResult()
        const override
    {
        return std::make_unique<RedisIntResult>();
    }

    txservice::TxCommandResult *GetResult() override
    {
        return &result_;
    }

    std::unique_ptr<txservice::TxCommand> Clone() override
    {
        return std::make_unique<StoreListCommand>(*this);
    }

    bool IsReadOnly() const override
    {
        return false;
    }

    bool ProceedOnNonExistentObject() const override
    {
        // If nothing to store and the object was empty, cannot proceed.
        return !elements_.empty();
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    bool IsOverwrite() const override
    {
        return true;
    }

    bool IgnoreOldValue() const override
    {
        return !elements_.empty();
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *obj_ptr) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    std::vector<EloqString> elements_;

    RedisIntResult result_;
};

/**
 * SortableLoadCommand is not a redis command, but a subcommand of sort command.
 * It is used to fetch members from list/set/zset.
 */
struct SortableLoadCommand : public RedisCommand
{
    std::unique_ptr<txservice::TxRecord> CreateObject(
        const std::string *image) const override
    {
        return nullptr;
    }

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    std::unique_ptr<txservice::TxCommandResult> CreateCommandResult()
        const override
    {
        return std::make_unique<RedisListResult>();
    }

    txservice::TxCommandResult *GetResult() override
    {
        return &result_;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void OutputResult(OutputHandler *reply) const override;

    void Serialize(std::string &str) const override
    {
        uint8_t type = static_cast<uint8_t>(RedisCommandType::SORTABLE_LOAD);
        str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));
    }

    void Deserialize(std::string_view cmd_img) override
    {
    }

    RedisSortableLoadResult result_;
};

/**
 * MHGetCommand is not a redis command, but a subcommand of sort command.
 * It is used to get field from multiple hashmaps.
 */
struct MHGetCommand : public RedisMultiObjectCommand
{
public:
    MHGetCommand() = default;
    MHGetCommand(std::vector<EloqKey> &&keys, std::vector<HGetCommand> &&cmds)
        : keys_(std::move(keys)), cmds_(std::move(cmds))
    {
        assert(keys_.size() == cmds_.size());
        vct_key_ptrs_.resize(1);
        vct_key_ptrs_[0].reserve(keys_.size());
        vct_cmd_ptrs_.resize(1);
        vct_cmd_ptrs_[0].reserve(cmds_.size());

        for (const auto &key : keys_)
        {
            vct_key_ptrs_[0].emplace_back(&key);
        }
        for (auto &cmd : cmds_)
        {
            vct_cmd_ptrs_[0].emplace_back(&cmd);
        }
    }

    void OutputResult(OutputHandler *reply) const
    {
        reply->OnArrayStart(cmds_.size());
        for (const auto &cmd : cmds_)
        {
            int32_t err_code = cmd.result_.err_code_;
            if (err_code == RD_OK)
            {
                cmd.OutputResult(reply);
            }
            else
            {
                assert(err_code == RD_NIL || err_code == RD_ERR_WRONG_TYPE);
                reply->OnNil();
            }
        }
        reply->OnArrayEnd();
    }

    std::vector<EloqKey> keys_;
    std::vector<HGetCommand> cmds_;
};

/**
 * SORT is a multiple steps command.
 */
struct SortCommand
{
    struct AccessKey
    {
        std::string object_key_;
        std::optional<std::string> field_key_;
    };

    struct SortObject
    {
        SortObject(std::string_view id, double score) : id_(id), score_(score)
        {
        }

        SortObject(std::string_view id, std::string_view cmpkey)
            : id_(id), cmpkey_(cmpkey)
        {
        }

        std::string id_;
        double score_{0};
        std::string cmpkey_;
    };

    class Pattern
    {
    public:
        Pattern() = default;
        explicit Pattern(const std::string_view &pattern);

        bool IsPoundSymbol() const
        {
            return is_pound_symbol_;
        }

        bool ContainStarSymbol() const
        {
            return contain_star_symbol_;
        }

        bool ContainHashField() const
        {
            return hash_field_.has_value();
        }

        AccessKey MakeAccessKey(const std::string_view &arg) const;

    private:
        bool is_pound_symbol_{false};
        bool contain_star_symbol_{false};
        std::string_view key_prefix_;
        std::string_view key_postfix_;
        std::optional<std::string_view> hash_field_;
    };

    SortCommand() = default;

    SortCommand(const std::string_view &sort_key,
                int64_t offset,
                int64_t count,
                const std::optional<std::string_view> &by_pattern,
                const std::vector<std::string_view> &get_pattern_vec,
                bool desc,
                bool alpha,
                const std::optional<std::string_view> &store_destination);

    void OutputResult(OutputHandler *reply) const;

    void ForceAlphaSort()
    {
        alpha_ = true;
        by_pattern_.reset();
    }

    std::function<bool(const SortObject &a, const SortObject &b)> LessFunc()
        const;

    absl::Span<const SortObject> Limit(
        const std::vector<SortObject> &sort_vector) const;

    static bool Load(RedisServiceImpl *redis_impl,
                     txservice::TransactionExecution *txm,
                     OutputHandler *error,
                     const txservice::TableName *table_name,
                     const std::string_view &key,
                     RedisObjectType &obj_type,
                     std::vector<std::string> &values);

    static bool MultiGet(RedisServiceImpl *redis_impl,
                         txservice::TransactionExecution *txm,
                         OutputHandler *error,
                         const txservice::TableName *table_name,
                         const Pattern &pattern,
                         const std::vector<AccessKey> &access_keys,
                         std::vector<std::optional<std::string>> &values);

    static bool Store(RedisServiceImpl *redis_impl,
                      txservice::TransactionExecution *txm,
                      OutputHandler *error,
                      const txservice::TableName *table_name,
                      const std::string_view &key,
                      std::vector<EloqString> values);

    static void PrepareStringSortObjects(const std::vector<std::string> &ids,
                                         std::vector<SortObject> &sort_objs);

    static void PrepareStringSortObjects(
        const std::vector<std::string> &ids,
        const std::vector<std::optional<std::string>> &by_values,
        std::vector<SortObject> &sort_objs);

    static bool PrepareScoreSortObjects(const std::vector<std::string> &ids,
                                        std::vector<SortObject> &sort_objs,
                                        OutputHandler *error);

    static bool PrepareScoreSortObjects(
        const std::vector<std::string> &ids,
        const std::vector<std::optional<std::string>> &by_values,
        std::vector<SortObject> &sort_objs,
        OutputHandler *error);

    // Load the sortable object(list/set/zset).
    std::string_view sort_key_;
    int64_t offset_;
    int64_t count_;

    // Sort options.
    bool desc_{false};
    bool alpha_{false};

    // Load sort-by object if with option 'by' pattern.
    std::optional<Pattern> by_pattern_;

    // Load sort-get object if with option 'get' patterns.
    std::vector<Pattern> get_pattern_vec_;

    // Store result of the sort command.
    std::optional<std::string_view> store_destination_;

    RedisSortResult result_{RD_OK};
};

struct WatchCommand : public RedisCommand
{
    bool IsReadOnly() const override
    {
        return true;
    }

    std::unique_ptr<txservice::TxRecord> CreateObject(
        const std::string *image) const override
    {
        // should never be called
        assert(false);
        return nullptr;
    }

    std::unique_ptr<txservice::TxCommandResult> CreateCommandResult()
        const override
    {
        // should never be called
        assert(false);
        return nullptr;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return false;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override
    {
        assert(false);
        return txservice::ExecResult::Fail;
    }

    void Serialize(std::string &str) const override
    {
        uint8_t type = static_cast<uint8_t>(RedisCommandType::WATCH);
        str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));
    }

    void Deserialize(std::string_view cmd_image) override
    {
        // Not need to do anything
    }

    void OutputResult(OutputHandler *reply) const override
    {
        reply->OnStatus("OK");
    }

    txservice::TxCommandResult *GetResult() override
    {
        return nullptr;
    }
};

struct MWatchCommand : public RedisMultiObjectCommand
{
public:
    MWatchCommand() = default;
    MWatchCommand(std::vector<EloqKey> &&keys, std::vector<WatchCommand> &&cmds)
        : keys_(std::move(keys)), cmds_(std::move(cmds))
    {
        assert(keys_.size() == cmds_.size());
        vct_key_ptrs_.resize(1);
        vct_key_ptrs_[0].reserve(keys_.size());
        vct_cmd_ptrs_.resize(1);
        vct_cmd_ptrs_[0].reserve(cmds_.size());

        for (const auto &key : keys_)
        {
            vct_key_ptrs_[0].emplace_back(&key);
        }
        for (auto &cmd : cmds_)
        {
            vct_cmd_ptrs_[0].emplace_back(&cmd);
        }
    }

    void OutputResult(OutputHandler *reply) const override
    {
        // watch always return OK (for now)
        reply->OnStatus("OK");
    }

    std::vector<EloqKey> keys_;
    std::vector<WatchCommand> cmds_;
};

struct BitOpCommand : public RedisMultiObjectCommand
{
public:
    enum struct OpType
    {
        And,
        Or,
        Xor,
        Not
    };

    BitOpCommand() = default;
    BitOpCommand(std::vector<EloqKey> &&s_keys,
                 std::vector<GetCommand> &&s_cmds,
                 std::unique_ptr<EloqKey> &&dst_key,
                 std::unique_ptr<SetCommand> &&set_cmd,
                 std::unique_ptr<DelCommand> &&del_cmd,
                 OpType op_type);

    void OutputResult(OutputHandler *reply) const override;
    bool HandleMiddleResult() override;
    bool IsPassed() const override;

    std::vector<EloqKey> s_keys_;
    std::vector<GetCommand> s_cmds_;
    std::unique_ptr<EloqKey> dst_key_;
    std::unique_ptr<SetCommand> set_cmd_;
    std::unique_ptr<DelCommand> del_cmd_;
    bool to_del_{false};
    OpType op_type_;
    int err_pos_{-1};  // The position of failed source command if has or -1
    std::string str_dest_;
};

// NOTICE: "SCAN" and "KEYS" shares the ScanCommand.
struct ScanCommand : public CustomCommand
{
    ScanCommand() = default;
    // This constructor is used for "KEYS" command
    explicit ScanCommand(std::string_view pattern);
    // This constructor is used for "SCAN" command
    ScanCommand(BucketScanCursor *scan_cursor,
                std::string_view pattern,
                int64_t count,
                RedisObjectType obj_type);

    ScanCommand(const ScanCommand &rhs) = default;
    ScanCommand(ScanCommand &&rhs) = default;
    ScanCommand &operator=(const ScanCommand &rhs) = delete;
    ScanCommand &operator=(ScanCommand &&rhs) = delete;
    ~ScanCommand() = default;

    bool Execute(RedisServiceImpl *redis_impl,
                 RedisConnectionContext *ctx,
                 const txservice::TableName *table,
                 txservice::TransactionExecution *txm,
                 OutputHandler *output,
                 bool auto_commit) override;

    void OutputResult(OutputHandler *reply,
                      RedisConnectionContext *ctx) const override;

    bool IsKeysCmd() const
    {
        // For SCAN cmd, "count_" must not be negative.
        return count_ == -1;
    }

    BucketScanCursor *scan_cursor_{nullptr};
    EloqString pattern_;
    int64_t count_{0};
    RedisObjectType obj_type_;
    RedisScanResult result_;
};

struct CreateVecIndexCommand : public CustomCommand
{
    CreateVecIndexCommand() = default;

    // Constructor with required parameters
    CreateVecIndexCommand(
        std::string_view index_name,
        std::string_view hash_set_name,
        std::string_view vector_column_name,
        uint32_t dimensions,
        EloqVec::Algorithm algorithm,
        EloqVec::DistanceMetric metric_type,
        std::unordered_map<std::string, std::string> &&alg_params)
        : index_name_(index_name),
          hash_set_name_(hash_set_name),
          vector_column_name_(vector_column_name),
          dimensions_(dimensions),
          algorithm_(algorithm),
          metric_type_(metric_type),
          alg_params_(std::move(alg_params))
    {
    }

    CreateVecIndexCommand(const CreateVecIndexCommand &rhs) = delete;
    CreateVecIndexCommand(CreateVecIndexCommand &&rhs) = default;
    CreateVecIndexCommand &operator=(const CreateVecIndexCommand &rhs) = delete;
    CreateVecIndexCommand &operator=(CreateVecIndexCommand &&rhs) = delete;
    ~CreateVecIndexCommand() = default;

    bool Execute(RedisServiceImpl *redis_impl,
                 RedisConnectionContext *ctx,
                 const txservice::TableName *table,
                 txservice::TransactionExecution *txm,
                 OutputHandler *output,
                 bool auto_commit) override;

    void OutputResult(OutputHandler *reply,
                      RedisConnectionContext *ctx) const override;

    // Name of the vector index
    EloqString index_name_;
    // Name of the primary hash set to index
    EloqString hash_set_name_;
    // Column containing vector embeddings
    EloqString vector_column_name_;
    // Number of dimensions in the vectors
    uint32_t dimensions_;
    // HNSW or IVF (currently only HNSW supported)
    EloqVec::Algorithm algorithm_;
    // Distance metric (cosine, l2sq, ip)
    EloqVec::DistanceMetric metric_type_;

    // Algorithm specific parameters (such as for HNSW: connectivity,
    // ef_construct, ef_search, scalar_type, etc.)
    std::unordered_map<std::string, std::string> alg_params_;

    // Result storage
    RedisCommandResult result_;
};

struct InfoVecIndexCommand : public CustomCommand
{
    InfoVecIndexCommand() = default;

    // Constructor with required parameters
    explicit InfoVecIndexCommand(std::string_view index_name)
        : index_name_(index_name)
    {
    }

    InfoVecIndexCommand(const InfoVecIndexCommand &rhs) = delete;
    InfoVecIndexCommand(InfoVecIndexCommand &&rhs) = default;
    InfoVecIndexCommand &operator=(const InfoVecIndexCommand &rhs) = delete;
    InfoVecIndexCommand &operator=(InfoVecIndexCommand &&rhs) = delete;
    ~InfoVecIndexCommand() = default;

    bool Execute(RedisServiceImpl *redis_impl,
                 RedisConnectionContext *ctx,
                 const txservice::TableName *table,
                 txservice::TransactionExecution *txm,
                 OutputHandler *output,
                 bool auto_commit) override;

    void OutputResult(OutputHandler *reply,
                      RedisConnectionContext *ctx) const override;

    // Name of the vector index to get info for
    EloqString index_name_;

    // Result storage
    RedisCommandResult result_;
};

struct DumpCommand : public RedisCommand
{
    std::unique_ptr<txservice::TxRecord> CreateObject(
        const std::string *image) const override
    {
        return nullptr;
    }

    bool IsReadOnly() const override
    {
        return true;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    std::unique_ptr<txservice::TxCommandResult> CreateCommandResult()
        const override
    {
        return std::make_unique<RedisStringResult>();
    }

    txservice::TxCommandResult *GetResult() override
    {
        return &result_;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void OutputResult(OutputHandler *reply) const override;

    void Serialize(std::string &str) const override
    {
        uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::DUMP);
        str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
    }

    void Deserialize(std::string_view cmd_img) override
    {
    }

    RedisStringResult result_;

    static constexpr inline uint16_t dump_version_{0};
};

struct RestoreCommand : public RedisCommand
{
    RestoreCommand() = default;

    RestoreCommand(const char *data,
                   size_t size,
                   uint64_t expire_when,
                   bool replace)
        : value_(data, size), expire_when_(expire_when), replace_(replace)
    {
    }

    RestoreCommand(RestoreCommand &&rhs) = default;
    RestoreCommand(const RestoreCommand &rhs) = default;
    RestoreCommand &operator=(RestoreCommand &&rhs) = delete;
    RestoreCommand &operator=(const RestoreCommand &rhs) = delete;

    std::unique_ptr<txservice::TxRecord> CreateObject(
        const std::string *image) const override;

    std::unique_ptr<txservice::TxCommandResult> CreateCommandResult()
        const override
    {
        return std::make_unique<RedisIntResult>();
    }

    txservice::TxCommandResult *GetResult() override
    {
        return &result_;
    }

    std::unique_ptr<txservice::TxCommand> Clone() override
    {
        return std::make_unique<RestoreCommand>(*this);
    }

    bool IsReadOnly() const override
    {
        return false;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return true;
    }

    bool ProceedOnExistentObject() const override
    {
        return replace_;
    }

    bool IsOverwrite() const override
    {
        return replace_;
    }

    bool IgnoreOldValue() const override
    {
        return IsOverwrite();
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    txservice::TxObject *CommitOn(txservice::TxObject *obj_ptr) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    static bool VerifyDumpPayload(std::string_view dump_payload);

    std::string value_;
    uint64_t expire_when_{0};

    // If replace is set, the restore command takes effect anyway. If old key
    // doesn't exists and replace is not set, the restore command response
    // busykey error.
    bool replace_{false};
    uint64_t idle_time_sec_{0};  // unsupport
    uint64_t frequency_{0};      // unsupport

    // Set result_ defaults to RD_OK, instead of RD_NIl. When replace_ is not
    // set and old key exists, ExecuteOn won't be called.
    RedisCommandResult result_{RD_ERR_BUSY_KEY_EXIST};
};

#ifdef WITH_FAULT_INJECT
struct RedisFaultInjectCommand : public CustomCommand
{
    RedisFaultInjectCommand() = default;

    RedisFaultInjectCommand(std::string_view fault_name,
                            std::string_view fault_paras,
                            int vct_node_id);

    RedisFaultInjectCommand(const RedisFaultInjectCommand &) = delete;
    RedisFaultInjectCommand(RedisFaultInjectCommand &&) = default;

    RedisFaultInjectCommand &operator=(const RedisFaultInjectCommand &) =
        delete;
    RedisFaultInjectCommand &operator=(RedisFaultInjectCommand &&) = delete;

    bool Execute(RedisServiceImpl *redis_impl,
                 RedisConnectionContext *ctx,
                 const txservice::TableName *table,
                 txservice::TransactionExecution *txm,
                 OutputHandler *output,
                 bool auto_commit) override;

    void OutputResult(OutputHandler *reply,
                      RedisConnectionContext *ctx) const override;

    std::string fault_name_;
    std::string fault_paras_;
    std::vector<int> vct_node_id_;
};
#endif

struct RedisMemoryUsageCommand : public RedisCommand
{
    RedisMemoryUsageCommand() = default;
    explicit RedisMemoryUsageCommand(int64_t key_len) : key_len_(key_len)
    {
    }

    RedisMemoryUsageCommand(const RedisMemoryUsageCommand &rhs) = default;
    RedisMemoryUsageCommand(RedisMemoryUsageCommand &&rhs) = default;
    ~RedisMemoryUsageCommand() override = default;
    RedisMemoryUsageCommand &operator=(const RedisMemoryUsageCommand &rhs) =
        delete;
    RedisMemoryUsageCommand &operator=(RedisMemoryUsageCommand &&rhs) = delete;

    std::unique_ptr<TxCommand> Clone() override
    {
        return std::make_unique<RedisMemoryUsageCommand>(*this);
    }

    bool IsReadOnly() const override
    {
        return true;
    }

    std::unique_ptr<txservice::TxRecord> CreateObject(
        const std::string *image) const override;

    std::unique_ptr<txservice::TxCommandResult> CreateCommandResult()
        const override
    {
        return std::make_unique<RedisIntResult>();
    }

    txservice::TxCommandResult *GetResult() override
    {
        return &result_;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void Serialize(std::string &str) const override;

    void Deserialize(std::string_view cmd_image) override;

    void OutputResult(OutputHandler *reply) const override;

    static bool CheckTypeMatch(const txservice::TxRecord &obj);

    RedisIntResult result_;
    // The key's length, only used for OutputResult, do not need to Serialize,
    // Deserialize.
    int64_t key_len_;
};

struct RecoverObjectCommand : public RedisCommand
{
    RecoverObjectCommand() = default;

    std::unique_ptr<TxCommand> Clone() override
    {
        assert(false);
        return nullptr;
    }

    bool IsReadOnly() const override
    {
        return false;
    }

    bool IsOverwrite() const override
    {
        return true;
    }

    std::unique_ptr<txservice::TxRecord> CreateObject(
        const std::string *image) const override;

    std::unique_ptr<txservice::TxCommandResult> CreateCommandResult()
        const override
    {
        assert(false);
        return nullptr;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return true;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override
    {
        assert(false);
        return txservice::ExecResult::Write;
    }

    txservice::TxObject *CommitOn(txservice::TxObject *obj_ptr) override;

    void Serialize(std::string &str) const override;
    void Deserialize(std::string_view cmd_img) override;

    txservice::TxCommandResult *GetResult() override
    {
        assert(false);
        return nullptr;
    }

    void OutputResult(OutputHandler *reply) const override
    {
        assert(false);
    }

    std::string result_;
};

struct GetExCommand : public GetCommand
{
public:
    GetExCommand() : expire_ts_(UINT64_MAX), is_persist_(false)
    {
    }

    explicit GetExCommand(uint64_t expire_ts, bool is_persist)
        : expire_ts_(expire_ts), is_persist_(is_persist)
    {
    }

    std::unique_ptr<TxCommand> Clone() override;

    bool IsReadOnly() const override
    {
        return false;
    }

    bool IsOverwrite() const override
    {
        return false;
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::TxCommand *RecoverTTLObjectCommand() override
    {
        assert(recover_ttl_obj_cmd_ != nullptr);
        return recover_ttl_obj_cmd_.get();
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;
    txservice::TxObject *CommitOn(txservice::TxObject *obj_ptr) override;

    void Serialize(std::string &str) const override;
    void Deserialize(std::string_view cmd_img) override;

    bool WillSetTTL() const override
    {
        if (expire_ts_ < UINT64_MAX || is_persist_)
        {
            return true;
        }

        return false;
    }

    uint64_t expire_ts_;
    bool is_persist_;
    // create this cmd when ttl is reset and write to log, so kv can be skipped
    // when log replay
    std::unique_ptr<RecoverObjectCommand> recover_ttl_obj_cmd_{nullptr};
};

/*
 * Copyright (c) 2009-2016, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#define EXPIRE_NX (1 << 0)  // Set expiry only when the key has no expiry
#define EXPIRE_XX                                                              \
    (1 << 1)  // Set expiry only when the key has an existing expiry
#define EXPIRE_GT                                                              \
    (1 << 2)  // Set expiry only when the new expiry is greater than
              // current one
#define EXPIRE_LT                                                              \
    (1 << 3)  // Set expiry only when the new expiry is less than current
              // one

struct ExpireCommand : public RedisCommand
{
public:
    ExpireCommand() : expire_ts_(UINT64_MAX), flags_(0)
    {
    }

    explicit ExpireCommand(uint64_t expire_ts, uint8_t flags)
        : expire_ts_(expire_ts), flags_(flags)
    {
    }

    std::unique_ptr<TxCommand> Clone() override;

    bool IsReadOnly() const override
    {
        return false;
    }

    bool IsOverwrite() const override
    {
        return false;
    }

    std::unique_ptr<txservice::TxRecord> CreateObject(
        const std::string *image) const override
    {
        assert(false);
        return nullptr;
    }

    std::unique_ptr<txservice::TxCommandResult> CreateCommandResult()
        const override
    {
        return std::make_unique<RedisIntResult>();
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::TxCommand *RecoverTTLObjectCommand() override
    {
        assert(recover_ttl_obj_cmd_ != nullptr);
        return recover_ttl_obj_cmd_.get();
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;
    txservice::TxObject *CommitOn(txservice::TxObject *obj_ptr) override;

    void Serialize(std::string &str) const override;
    void Deserialize(std::string_view cmd_img) override;

    txservice::TxCommandResult *GetResult() override
    {
        return &result_;
    }

    bool WillSetTTL() const override
    {
        return true;
    }

    void OutputResult(OutputHandler *reply) const override;

    // TTL expire ts in micro seconds
    uint64_t expire_ts_;
    uint8_t flags_;
    RedisIntResult result_;
    // create this cmd when ttl is reset and write to log, so kv can be skipped
    // when log replay
    std::unique_ptr<RecoverObjectCommand> recover_ttl_obj_cmd_{nullptr};
};

struct TTLCommand : public RedisCommand
{
public:
    TTLCommand()
        : is_pttl_(false), is_expire_time_(false), is_pexpire_time_(false) {};

    explicit TTLCommand(bool is_pttl, bool is_expire_time, bool is_pexpire_time)
        : is_pttl_(is_pttl),
          is_expire_time_(is_expire_time),
          is_pexpire_time_(is_pexpire_time) {};

    std::unique_ptr<TxCommand> Clone() override;

    bool IsReadOnly() const override
    {
        return true;
    }

    std::unique_ptr<txservice::TxRecord> CreateObject(
        const std::string *image) const override
    {
        assert(false);
        return nullptr;
    }

    std::unique_ptr<txservice::TxCommandResult> CreateCommandResult()
        const override
    {
        return std::make_unique<RedisIntResult>();
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;

    void Serialize(std::string &str) const override;
    void Deserialize(std::string_view cmd_img) override;

    txservice::TxCommandResult *GetResult() override
    {
        return &result_;
    }

    void OutputResult(OutputHandler *reply) const override;

    RedisIntResult result_;
    bool is_pttl_;
    bool is_expire_time_;
    bool is_pexpire_time_;
};

struct PersistCommand : public RedisCommand
{
    PersistCommand() = default;

    std::unique_ptr<TxCommand> Clone() override;

    bool IsReadOnly() const override
    {
        return false;
    }

    bool IsOverwrite() const override
    {
        return false;
    }

    std::unique_ptr<txservice::TxRecord> CreateObject(
        const std::string *image) const override
    {
        assert(false);
        return nullptr;
    }

    std::unique_ptr<txservice::TxCommandResult> CreateCommandResult()
        const override
    {
        return std::make_unique<RedisCommandResult>();
    }

    bool ProceedOnNonExistentObject() const override
    {
        return false;
    }

    bool ProceedOnExistentObject() const override
    {
        return true;
    }

    txservice::ExecResult ExecuteOn(const txservice::TxObject &object) override;
    txservice::TxObject *CommitOn(txservice::TxObject *obj_ptr) override;

    txservice::TxCommand *RecoverTTLObjectCommand() override
    {
        assert(recover_ttl_obj_cmd_ != nullptr);
        return recover_ttl_obj_cmd_.get();
    }

    void Serialize(std::string &str) const override;
    void Deserialize(std::string_view cmd_img) override;

    txservice::TxCommandResult *GetResult() override
    {
        return &result_;
    }

    bool WillSetTTL() const override
    {
        return true;
    }

    void OutputResult(OutputHandler *reply) const override;

    RedisCommandResult result_;

    // create this cmd when ttl is reset and write to log, so kv can be skipped
    // when log replay
    std::unique_ptr<RecoverObjectCommand> recover_ttl_obj_cmd_{nullptr};
};

/**
 * Parse command from args, used by TransactionHandler.
 *
 * @param args
 * @param output
 * @return
 */
std::pair<bool,
          std::variant<DirectRequest,
                       txservice::ObjectCommandTxRequest,
                       txservice::MultiObjectCommandTxRequest,
                       CustomCommandRequest>>
ParseMultiCommand(RedisServiceImpl *redis_impl,
                  RedisConnectionContext *ctx,
                  const std::vector<std::string> &cmd_args,
                  OutputHandler *output,
                  txservice::TransactionExecution *txm);

std::tuple<bool, EchoCommand> ParseEchoCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, PingCommand> ParsePingCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, AuthCommand> ParseAuthCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, SelectCommand> ParseSelectCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, ConfigCommand> ParseConfigCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, InfoCommand> ParseInfoCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, std::unique_ptr<CommandCommand>> ParseCommandCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, std::unique_ptr<ClusterCommand>> ParseClusterCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, std::unique_ptr<FailoverCommand>> ParseFailoverCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, std::unique_ptr<ClientCommand>> ParseClientCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, DBSizeCommand> ParseDBSizeCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, SlowLogCommand> ParseSlowLogCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, PublishCommand> ParsePublishCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, ReadOnlyCommand> ParseReadOnlyCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, GetCommand> ParseGetCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, SetCommand> ParseSetCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, GetDelCommand> ParseGetDelCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, StrLenCommand> ParseStrLenCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, GetBitCommand> ParseGetBitCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, GetRangeCommand> ParseGetRangeCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, BitCountCommand> ParseBitCountCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, FloatOpCommand> ParseIncrByFloatCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, SetBitCommand> ParseSetBitCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, SetRangeCommand> ParseSetRangeCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, AppendCommand> ParseAppendCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, BitFieldCommand> ParseBitFieldCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, BitFieldCommand> ParseBitFieldRoCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, BitPosCommand> ParseBitPosCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, BitOpCommand> ParseBitOpCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, GetRangeCommand> ParseSubStrCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, LRangeCommand> ParseLRangeCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, RPushCommand> ParseRPushCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, HSetCommand> ParseHSetCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, HGetCommand> ParseHGetCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, LPushCommand> ParseLPushCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, LPopCommand> ParseLPopCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, RPopCommand> ParseRPopCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, LMPopCommand> ParseLMPopCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, IntOpCommand> ParseIncrCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, IntOpCommand> ParseDecrCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, IntOpCommand> ParseIncrByCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, IntOpCommand> ParseDecrByCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, ZAddCommand> ParseZAddCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, ZRangeCommand> ParseZRangeCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, ZCountCommand> ParseZCountCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, ZCardCommand> ParseZCardCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, ZRemCommand> ParseZRemCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, ZScoreCommand> ParseZScoreCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, ZRangeCommand> ParseZRangeByScoreCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, ZRangeCommand> ParseZRangeByLexCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, ZRemRangeCommand> ParseZRemRangeCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, ZRangeCommand> ParseZRangeByRankCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, ZRangeCommand> ParseZRevRangeCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, ZRangeCommand> ParseZRevRangeByScoreCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, ZRangeCommand> ParseZRevRangeByLexCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, ZPopCommand> ParseZPopMinCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, ZPopCommand> ParseZPopMaxCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, ZLexCountCommand> ParseZLexCountCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, ZScanCommand> ParseZScanCommand(
    RedisConnectionContext *ctx,
    const std::vector<std::string_view> &args,
    OutputHandler *output);

std::tuple<bool, EloqKey, ZRandMemberCommand> ParseZRandMemberCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, ZRankCommand> ParseZRankCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, ZRankCommand> ParseZRevRankCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, ZMScoreCommand> ParseZMScoreCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::pair<bool, ZMPopCommand> ParseZMPopCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, TypeCommand> ParseTypeCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, ZUnionCommand> ParseZUnionCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, ZUnionStoreCommand> ParseZUnionStoreCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, ZDiffCommand> ParseZDiffCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, ZDiffStoreCommand> ParseZDiffStoreCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, ZRangeStoreCommand> ParseZRangeStoreCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, ZAddCommand> ParseZIncrByCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, ZInterCommand> ParseZInterCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, ZInterCardCommand> ParseZInterCardCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, ZInterStoreCommand> ParseZInterStoreCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::pair<bool, MDelCommand> ParseDelCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::pair<bool, MExistsCommand> ParseExistsCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::pair<bool, MSetCommand> ParseMSetCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::pair<bool, MSetNxCommand> ParseMSetNxCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::pair<bool, MGetCommand> ParseMGetCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::pair<bool, MWatchCommand> ParseWatchCommand(
    const std::vector<std::string> &args, OutputHandler *output);

std::tuple<bool, EloqKey, HDelCommand> ParseHDelCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, HExistsCommand> ParseHExistsCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, HGetAllCommand> ParseHGetAllCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, HIncrByCommand> ParseHIncrByCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, HIncrByFloatCommand> ParseHIncrByFloatCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, HMGetCommand> ParseHMGetCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, HKeysCommand> ParseHKeysCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, HValsCommand> ParseHValsCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, HSetNxCommand> ParseHSetNxCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, HRandFieldCommand> ParseHRandFieldCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, HScanCommand> ParseHScanCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, HLenCommand> ParseHLenCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, HStrLenCommand> ParseHStrLenCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, LLenCommand> ParseLLenCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, LTrimCommand> ParseLTrimCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, LIndexCommand> ParseLIndexCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, LInsertCommand> ParseLInsertCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, LPosCommand> ParseLPosCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, LSetCommand> ParseLSetCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, LRemCommand> ParseLRemCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, LPushXCommand> ParseLPushXCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, RPushXCommand> ParseRPushXCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, SAddCommand> ParseSAddCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, SMembersCommand> ParseSMembersCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, SCardCommand> ParseSCardCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, SRemCommand> ParseSRemCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, SDiffCommand> ParseSDiffCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, SDiffStoreCommand> ParseSDiffStoreCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, SInterCommand> ParseSInterCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, SInterStoreCommand> ParseSInterStoreCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, SInterCardCommand> ParseSInterCardCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, SIsMemberCommand> ParseSIsMemberCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, SMIsMemberCommand> ParseSMIsMemberCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, SMoveCommand> ParseSMoveCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, LMoveCommand> ParseLMoveCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, RPopLPushCommand> ParseRPopLPushCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, SUnionCommand> ParseSUnionCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, SUnionStoreCommand> ParseSUnionStoreCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, SRandMemberCommand> ParseSRandMemberCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, SPopCommand> ParseSPopCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, SScanCommand> ParseSScanCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, SortCommand> ParseSortCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, ScanCommand> ParseScanCommand(
    RedisConnectionContext *ctx,
    const std::vector<std::string_view> &args,
    OutputHandler *output);

std::tuple<bool, ScanCommand> ParseKeysCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, BLMoveCommand> ParseBLMoveCommand(
    const std::vector<std::string_view> &args,
    OutputHandler *output,
    bool multi = false);

std::tuple<bool, BLMPopCommand> ParseBLMPopCommand(
    const std::vector<std::string_view> &args,
    OutputHandler *output,
    bool multi = false);

std::tuple<bool, BLMPopCommand> ParseBLPopCommand(
    const std::vector<std::string_view> &args,
    OutputHandler *output,
    bool multi = false);

std::tuple<bool, BLMPopCommand> ParseBRPopCommand(
    const std::vector<std::string_view> &args,
    OutputHandler *output,
    bool multi = false);

std::tuple<bool, BLMoveCommand> ParseBRPopLPushCommand(
    const std::vector<std::string_view> &args,
    OutputHandler *output,
    bool multi = false);

std::tuple<bool, EloqKey, DumpCommand> ParseDumpCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, RestoreCommand> ParseRestoreCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);
bool ParseFlushDBCommand(const std::vector<std::string_view> &args,
                         OutputHandler *output);
bool ParseFlushALLCommand(const std::vector<std::string_view> &args,
                          OutputHandler *output);
#ifdef WITH_FAULT_INJECT
std::tuple<bool, RedisFaultInjectCommand> ParseFaultInjectCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);
#endif

std::tuple<bool, EloqKey, RedisMemoryUsageCommand> ParseMemoryUsageCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, ExpireCommand> ParseExpireCommand(
    const std::vector<std::string_view> &args,
    OutputHandler *output,
    bool is_pexpire,
    bool is_expireat);

std::tuple<bool, EloqKey, TTLCommand> ParseTTLCommand(
    const std::vector<std::string_view> &args,
    OutputHandler *output,
    bool is_pttl,
    bool is_expire_time,
    bool is_pexpire_time);

std::tuple<bool, EloqKey, PersistCommand> ParsePersistCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, EloqKey, GetExCommand> ParseGetExCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, TimeCommand> ParseTimeCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, CreateVecIndexCommand> ParseCreateVecIndexCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

std::tuple<bool, InfoVecIndexCommand> ParseInfoVecIndexCommand(
    const std::vector<std::string_view> &args, OutputHandler *output);

}  // namespace EloqKV
