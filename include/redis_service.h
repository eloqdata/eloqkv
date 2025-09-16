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

#include <brpc/redis.h>
#include <bthread/task_group.h>

#include <cassert>
#include <cstddef>
#include <memory>  //std::unique_ptr
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "eloq_metrics/include/meter.h"
#include "error_messages.h"
#include "pub_sub_manager.h"
#include "store_handler/kv_store.h"

#if (defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_S3) ||                      \
     defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_GCS) ||                     \
     defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB) ||                               \
     defined(DATA_STORE_TYPE_ELOQDSS_ELOQSTORE))
#define ELOQDS 1
#endif

#if ELOQDS
#include "data_store_service.h"
#include "store_handler/data_store_service_client.h"
#endif

#if defined(DATA_STORE_TYPE_DYNAMODB) ||                                       \
    defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_S3) ||                       \
    defined(LOG_STATE_TYPE_RKDB_S3)
#include <aws/core/Aws.h>
#endif

#if defined(DATA_STORE_TYPE_ROCKSDB)
#include "store_handler/rocksdb_handler.h"
#endif

#ifdef DATA_STORE_TYPE_DYNAMODB
#include "store_handler/dynamo_handler.h"
#endif

#if (WITH_LOG_SERVICE)
#include "log_server.h"
#endif

#include "eloq_catalog_factory.h"
#include "lua_interpreter.h"
#include "redis_command.h"
#include "tx_request.h"

extern "C"
{
#include "lua/src/lauxlib.h"
#include "lua/src/lua.h"
#include "lua/src/lualib.h"
}

namespace bthread
{
extern BAIDU_THREAD_LOCAL TaskGroup *tls_task_group;
};

namespace moodycamel
{
template <typename T, typename Traits>
class ConcurrentQueue;
};

using namespace txservice;

namespace EloqKV
{
extern int databases;
extern std::string requirepass;
extern std::string redis_ip_port;
extern brpc::Acceptor *server_acceptor;

enum struct HostStatus : uint8_t
{
    Online = 0,
    Loading,
    Failed,
};
struct HostNetworkInfo
{
    HostNetworkInfo() = default;
    HostNetworkInfo(const HostNetworkInfo &rhs)
        : ip(rhs.ip),
          port(rhs.port),
          host_names(rhs.host_names),
          node_id(rhs.node_id),
          status(rhs.status)
    {
    }

    HostNetworkInfo(HostNetworkInfo &&rhs)
        : ip(std::move(rhs.ip)),
          port(rhs.port),
          host_names(std::move(rhs.host_names)),
          node_id(rhs.node_id),
          status(rhs.status)
    {
    }

    HostNetworkInfo &operator=(const HostNetworkInfo &rhs) = default;

    std::string ip{};
    // redis port
    uint16_t port{0};
    std::vector<std::string> host_names{};
    // replica node status: failed, online, loading
    uint32_t node_id{0};
    HostStatus status{HostStatus::Failed};

    // map node id to a 40-character globally unique
    // string for client used. "00000000...<leader_node_id>".
    std::string host_id() const
    {
        std::string node_id_str = std::to_string(node_id);
        std::string host_id_str;
        host_id_str.append((40U - node_id_str.size()), '0');
        host_id_str.append(node_id_str);
        return host_id_str;
    }

    void append_host_id_to(std::string &to_append) const
    {
        std::string node_id_str = std::to_string(node_id);
        to_append.append((40U - node_id_str.size()), '0');
        to_append.append(node_id_str);
    }
};

struct SlotPair
{
    uint16_t start_slot_range;
    uint16_t end_slot_range;
};
struct SlotInfo : public SlotPair
{
    std::list<HostNetworkInfo> hosts;

    friend bool operator<(const SlotInfo &lhs, const SlotInfo &rhs)
    {
        return lhs.start_slot_range < rhs.start_slot_range;
    }
};

struct RedisTable
{
    TableName table_name_;
};

class RedisCommandHandler;
class RedisConnectionContext;
class MultiTransactionHandler;

const std::unordered_set<std::string> redis_config_keys = {
    "slowlog-log-slower-than",
    "slowlog-max-len",
};

class RedisServiceImpl : public brpc::RedisService
{
public:
    explicit RedisServiceImpl(const std::string &config_file,
                              const char *version);
    ~RedisServiceImpl() override;

    bool Init(brpc::Server &brpc_server);

    bool InitTxLogService(
        uint32_t node_id,
        int txlog_group_replica_num,
        const std::string &log_path,
        const std::string &local_ip,
        uint16_t local_tx_port,
        bool enable_brpc_builtin_services,
        const INIReader &config_reader,
        std::unordered_map<uint32_t, std::vector<NodeConfig>> &ng_configs,
        std::vector<std::string> &txlog_ips,
        std::vector<uint16_t> &txlog_ports);

    void Stop();

    // The number of master nodes serving at least one hash slot in the cluster.
    uint32_t RedisClusterSize();

    // The total number of known nodes in the cluster
    uint32_t RedisClusterNodesCount();

    uint16_t TxPortToRedisPort(uint16_t tx_port) const
    {
        return tx_port - 10000;
    }
    uint16_t RedisPortToTxPort(uint16_t redis_port) const
    {
        return redis_port + 10000;
    }

    // Get all replica node status of node groups.
    // nodes_info is as: map{ng_id,[nodes_info,...]}
    void GetReplicaNodesStatus(
        std::unordered_map<uint32_t, std::vector<HostNetworkInfo>> &nodes_info)
        const;

    void GetNodeSlotsInfo(
        const std::unordered_map<NodeGroupId, NodeId> &ng_leaders,
        std::unordered_map<NodeId, std::vector<SlotPair>> &nodes_slots) const;

    // For ClusterNodes command. Results will be returned through arg 'info'.
    void RedisClusterNodes(std::vector<std::string> &info);

    // For ClusterSlots command. Results will be returned through arg 'info'.
    void RedisClusterSlots(std::vector<SlotInfo> &info);

    std::string GenerateMovedErrorMessage(uint16_t slot_num);

    std::unique_ptr<brpc::ConnectionContext> NewConnectionContext(
        brpc::Socket *socket) const override;

    const TableName *RedisTableName(int db_id) const;
    size_t GetRedisTableCount() const;

    bool AuthRequired(const RedisConnectionContext *ctx,
                      const butil::StringPiece &command) const;

    brpc::RedisCommandHandlerResult DispatchCommand(
        brpc::ConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool flush_batched) const override;

    // Call this function to register `handler` that can handle command `name`.
    bool AddCommandHandler(const std::string &name,
                           RedisCommandHandler *handler);

    // This function should not be touched by user and used by brpc deverloper
    // only.
    RedisCommandHandler *FindCommandHandler(
        const butil::StringPiece &name) const;

    TransactionExecution *NewTxm(IsolationLevel iso_level, CcProtocol protocol);

    bool ScriptFlush();

    bool ScriptLoad(const std::vector<butil::StringPiece> &args,
                    brpc::RedisReply *output);

    bool ScriptExists(const std::vector<butil::StringPiece> &args,
                      brpc::RedisReply *output);

    bool Evalsha(const RedisConnectionContext *ctx,
                 const std::vector<butil::StringPiece> &args,
                 brpc::RedisReply *output);

    TxErrorCode MultiExec(
        std::vector<std::variant<DirectRequest,
                                 ObjectCommandTxRequest,
                                 MultiObjectCommandTxRequest,
                                 CustomCommandRequest>> &cmd_reqs,
        std::vector<std::vector<std::string>> &cmd_args,
        brpc::RedisReply *reply,
        TransactionExecution *txm,
        RedisConnectionContext *ctx);

    bool EvalLua(const RedisConnectionContext *ctx,
                 const std::vector<butil::StringPiece> &args,
                 brpc::RedisReply *output);

    void GenericCommand(RedisConnectionContext *ctx,
                        TransactionExecution *txm,
                        const std::vector<std::string> &cmd_arg_list,
                        OutputHandler *output);

    void ExecuteGetConfig(ConfigCommand *cmd);

    void ExecuteSetConfig(ConfigCommand *cmd);

    bool ExecuteCommand(RedisConnectionContext *ctx,
                        DirectCommand *cmd,
                        OutputHandler *output);
    /**
     * @return There was no error during the process of the command. (That does
     * not represent whether the results of the command are successful.)
     */
    bool ExecuteCommand(RedisConnectionContext *ctx,
                        txservice::TransactionExecution *txm,
                        const EloqKey &key,
                        RedisCommand *cmd,
                        OutputHandler *output,
                        bool auto_commit,
                        bool always_redirect = true);

    /**
     * @return There is no tx request error during the process of the command.
     * (That does not represent whether the results of the command are
     * successful.)
     */
    bool ExecuteCommand(RedisConnectionContext *ctx,
                        txservice::TransactionExecution *txm,
                        RedisMultiObjectCommand *cmd,
                        OutputHandler *output,
                        bool auto_commit,
                        bool always_redirect = true);

    /**
     * @return There is no tx request error during the process of the command.
     * (That does not represent whether the results of the command are
     * successful.)
     */
    bool ExecuteCommand(RedisConnectionContext *ctx,
                        txservice::TransactionExecution *txm,
                        const TableName *table,
                        const EloqKey &key,
                        ZScanCommand *cmd,
                        OutputHandler *output,
                        bool auto_commit);

    /**
     * output and error may be equal or not equal. For example, the sort command
     * execute subcommands load->mget/mhget(with by)->mget/mhget(with
     * gets)->store. If subcommand failed, it should write error and abort, else
     * it should skip write output.
     *
     * @return Whether the TxRequest is successfully executed. (That does not
     * represent whether the results of the command are successful.)
     */
    bool ExecuteTxRequest(TransactionExecution *txm,
                          ObjectCommandTxRequest *tx_req,
                          OutputHandler *output,
                          OutputHandler *error);

    /**
     * output and error may be equal or not equal. For example, the sort command
     * execute subcommands load->mget/mhget(with by)->mget/mhget(with
     * gets)->store. If subcommand failed, it should write error and abort, else
     * it should skip write output.
     *
     * @return Whether the TxRequest is successfully executed. (That does not
     * represent whether the results of the command are successful.)
     */
    bool ExecuteMultiObjTxRequest(TransactionExecution *txm,
                                  MultiObjectCommandTxRequest *tx_req,
                                  OutputHandler *output,
                                  OutputHandler *error);

    bool ExecuteUpsertTableTxRequest(TransactionExecution *txm,
                                     UpsertTableTxRequest *tx_req,
                                     OutputHandler *output,
                                     bool wait_result = true);

    bool ExecuteCommand(RedisConnectionContext *ctx,
                        TransactionExecution *txm,
                        SortCommand *cmd,
                        OutputHandler *output,
                        bool auto_commit);

    bool IsRecordTTLExpired(const txservice::TxRecord *rec,
                            txservice::LocalCcShards *local_cc_shards);

    bool ExecuteCommand(RedisConnectionContext *ctx,
                        TransactionExecution *txm,
                        const TableName *table,
                        ScanCommand *cmd,
                        OutputHandler *output,
                        bool auto_commit);

    bool ExecuteCommand(RedisConnectionContext *ctx,
                        TransactionExecution *txm,
                        const TableName *table,
                        CreateVecIndexCommand *cmd,
                        OutputHandler *output,
                        bool auto_commit);

    bool ExecuteCommand(RedisConnectionContext *ctx,
                        TransactionExecution *txm,
                        const TableName *table,
                        InfoVecIndexCommand *cmd,
                        OutputHandler *output,
                        bool auto_commit);

#ifdef WITH_FAULT_INJECT
    bool ExecuteCommand(RedisConnectionContext *ctx,
                        TransactionExecution *txm,
                        RedisFaultInjectCommand *cmd,
                        OutputHandler *output,
                        bool auto_commit);
#endif

    bool ExecuteFlushDBCommand(RedisConnectionContext *ctx,
                               TransactionExecution *txm,
                               OutputHandler *output,
                               bool auto_commit);

    bool ExecuteFlushALLCommand(RedisConnectionContext *ctx,
                                OutputHandler *output,
                                bool auto_commit,
                                IsolationLevel iso_level_,
                                CcProtocol cc_protocol_);

    void CollectConnectionsMetrics(brpc::Server &server);

    metrics::Meter *GetMeter(std::size_t core_id) const;

    void Subscribe(const std::vector<std::string_view> &chans,
                   RedisConnectionContext *client);

    void Unsubscribe(const std::vector<std::string_view> &chans,
                     RedisConnectionContext *client);

    void PSubscribe(const std::vector<std::string_view> &patterns,
                    RedisConnectionContext *client);

    void PUnsubscribe(const std::vector<std::string_view> &patterns,
                      RedisConnectionContext *client);

    int Publish(std::string_view chan, std::string_view msg);

    bool IsEnableRedisStats() const
    {
        return enable_redis_stats_;
    }
    const char *GetEnableDataStore() const
    {
        return skip_kv_ ? "off" : "on";
    }
    const char *GetEnableWal() const
    {
        return skip_wal_ ? "off" : "on";
    }
    uint32_t GetCoreNum() const
    {
        return core_num_;
    }
    uint32_t GetRedisPort() const
    {
        return redis_port_;
    }
    uint64_t GetStartSecond() const
    {
        return start_sec_;
    }
    std::string GetConfigFile() const
    {
        return config_file_;
    }
    uint32_t GetNodeMemoryLimitMB() const
    {
        return node_memory_limit_mb_;
    }
    uint32_t GetNodeLogLimitMB() const
    {
        return node_log_limit_mb_;
    }
    int GetEventDispatcherNum() const
    {
        return event_dispatcher_num_;
    }
    TxService *GetTxService()
    {
        return tx_service_.get();
    }
    const char *GetVersion() const
    {
        return version_;
    }
    txservice::IsolationLevel GetTxnIsolationLevel() const
    {
        return txn_isolation_level_;
    }
    txservice::CcProtocol GetTxnProtocol() const
    {
        return txn_protocol_;
    }

    void ResetSlowLog();

    void GetSlowLog(std::list<SlowLogEntry> &results, int count);

    uint32_t GetSlowLogLen() const;

    void ResizeSlowLog(uint32_t len);

    size_t MaxConnectionCount() const;

private:
    static bool SendTxRequest(TransactionExecution *txm,
                              TxRequest *tx_req,
                              OutputHandler *error);

    /**
     *
     * @tparam Subtype
     * @tparam T
     * @param txm
     * @param tx_req
     * @param error
     * @return true if the request succeeds and no error occurs.
     */
    template <typename Subtype, typename T>
    bool SendTxRequestAndWaitResult(TransactionExecution *txm,
                                    TemplateTxRequest<Subtype, T> *tx_req,
                                    OutputHandler *error);

    std::unique_ptr<LuaInterpreter> GetLuaInterpreter();

    void CleanAndReturnLuaInterpreter(std::unique_ptr<LuaInterpreter>);

    void AddHandlers();

    bool InitMetricsRegistry();

    /**
     * Currently, this method only register connection-related metrics here.
     * After registration is completed, the background thread collects data at a
     * frequency of once per second.
     */
    void RegisterRedisMetrics();

    inline bool CheckAndUpdateRedisCmdRound(RedisCommandType cmd_type) const;

    inline std::string_view GetCommandAccessType(
        const std::string_view &cmd_type) const;

private:
    typedef std::unordered_map<std::string, RedisCommandHandler *> CommandMap;
    CommandMap command_map_;

    std::unique_ptr<TxService> tx_service_;
#if defined(DATA_STORE_TYPE_DYNAMODB) ||                                       \
    defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_S3) ||                       \
    defined(LOG_STATE_TYPE_RKDB_S3)
    Aws::SDKOptions aws_options_;
#endif

#if defined(DATA_STORE_TYPE_DYNAMODB)
    std::unique_ptr<EloqDS::DynamoHandler> store_hd_;
#elif defined(DATA_STORE_TYPE_ROCKSDB)
    std::unique_ptr<RocksDBHandlerImpl> store_hd_;
#elif ELOQDS
    std::unique_ptr<EloqDS::DataStoreServiceClient> store_hd_;
    std::unique_ptr<EloqDS::DataStoreService> data_store_service_;
#endif

#if (WITH_LOG_SERVICE)
    std::unique_ptr<::txlog::LogServer> log_server_;
#endif
    RedisCatalogFactory catalog_factory_;

    std::vector<TableName> redis_table_names_;

    std::vector<std::unique_ptr<RedisCommandHandler>> hd_vec_;

    moodycamel::ConcurrentQueue<std::unique_ptr<LuaInterpreter>>
        lua_interpreters_;

    std::shared_mutex script_mutex_;
    std::unordered_map<std::string, std::string> scripts_;

    // use atomic variable to protect config_. We do not use mutex here because
    // we might jump to other task groups when updating config_, so using mutex
    // might cause system error since the mutex is not released on the same
    // thread that it is acquired.
    std::atomic_bool config_accessing_{false};
    std::unordered_map<std::string, std::string> config_;

    bool enable_redis_stats_;
    bool skip_kv_;
    bool skip_wal_;
    bool enable_cache_replacement_;
    uint32_t core_num_;
    uint32_t redis_port_;
    uint64_t start_sec_;  // The start second since The Epoch
    std::string config_file_;
    uint32_t node_memory_limit_mb_;
    uint32_t node_log_limit_mb_;
    int event_dispatcher_num_;
    const char *version_;
    // Isolation level and concurrency control protocol of MULTI/EXEC or lua
    // transactions. ReadCommitted and OccRead are always used for simple
    // commands.
    // To be consistent with Redis, Watch operations use RepeatableRead for
    // validation.
    IsolationLevel txn_isolation_level_{IsolationLevel::RepeatableRead};
    // To be consistent with Redis, Watch keys better use OCC or OccRead.
    CcProtocol txn_protocol_{CcProtocol::OCC};
    // Whether to retry transaction on OCC caused error. OCC break repeatable
    // read and write-write conflict are both retried.
    bool retry_on_occ_error_{false};

    std::unique_ptr<metrics::MetricsRegistry> metrics_registry_{nullptr};
    std::string metrics_port_{"18081"};
    metrics::CommonLabels redis_common_labels_{};
    std::unique_ptr<metrics::Meter> redis_meter_{nullptr};

    // Vector index related
    std::unique_ptr<txservice::TxWorkerPool> vector_index_worker_pool_{nullptr};

    mutable std::vector<std::vector<std::size_t>> redis_cmd_current_rounds_{};
    const metrics::Map<std::string_view, std::string_view> cmd_access_types_{
        {"append", "write"},
        {"bitcount", "read"},
        {"bitfield", "write"},
        {"bitfield_ro", "read"},
        {"bitop", "write"},
        {"bitpos", "read"},
        {"blmove", "write"},
        {"blmpop", "write"},
        {"blpop", "write"},
        {"brpop", "write"},
        {"brpoplpush", "write"},
        {"bzmpop", "write"},
        {"bzpopmax", "write"},
        {"bzpopmin", "write"},
        {"copy", "write"},
        {"dbsize", "read"},
        {"decr", "write"},
        {"decrby", "write"},
        {"del", "write"},
        {"dump", "read"},
        {"exists", "read"},
        {"expire", "write"},
        {"expireat", "write"},
        {"expiretime", "read"},
        {"flushall", "write"},
        {"flushdb", "write"},
        {"function delete", "write"},
        {"function flush", "write"},
        {"function load", "write"},
        {"function restore", "write"},
        {"geoadd", "write"},
        {"geodist", "read"},
        {"geohash", "read"},
        {"geopos", "read"},
        {"georadius", "write"},
        {"georadius_ro", "read"},
        {"georadiusbymember", "write"},
        {"georadiusbymember_ro", "read"},
        {"geosearch", "read"},
        {"geosearchstore", "write"},
        {"get", "read"},
        {"getbit", "read"},
        {"getdel", "write"},
        {"getex", "write"},
        {"getrange", "read"},
        {"getset", "write"},
        {"hdel", "write"},
        {"hexists", "read"},
        {"hget", "read"},
        {"hgetall", "read"},
        {"hincrby", "write"},
        {"hincrbyfloat", "write"},
        {"hkeys", "read"},
        {"hlen", "read"},
        {"hmget", "read"},
        {"hmset", "write"},
        {"hrandfield", "read"},
        {"hscan", "read"},
        {"hset", "write"},
        {"hsetnx", "write"},
        {"hstrlen", "read"},
        {"hvals", "read"},
        {"incr", "write"},
        {"incrby", "write"},
        {"incrbyfloat", "write"},
        {"keys", "read"},
        {"lcs", "read"},
        {"lindex", "read"},
        {"linsert", "write"},
        {"llen", "read"},
        {"lmove", "write"},
        {"lmpop", "write"},
        {"lolwut", "read"},
        {"lpop", "write"},
        {"lpos", "read"},
        {"lpush", "write"},
        {"lpushx", "write"},
        {"lrange", "read"},
        {"lrem", "write"},
        {"lset", "write"},
        {"ltrim", "write"},
        {"memory usage", "read"},
        {"mget", "read"},
        {"migrate", "write"},
        {"move", "write"},
        {"mset", "write"},
        {"msetnx", "write"},
        {"object encoding", "read"},
        {"object freq", "read"},
        {"object idletime", "read"},
        {"object refcount", "read"},
        {"persist", "write"},
        {"pexpire", "write"},
        {"pexpireat", "write"},
        {"pexpiretime", "read"},
        {"pfadd", "write"},
        {"pfcount", "read"},
        {"pfdebug", "write"},
        {"pfmerge", "write"},
        {"psetex", "write"},
        {"pttl", "read"},
        {"randomkey", "read"},
        {"rename", "write"},
        {"renamenx", "write"},
        {"restore", "write"},
        {"restore-asking", "write"},
        {"rpop", "write"},
        {"rpoplpush", "write"},
        {"rpush", "write"},
        {"rpushx", "write"},
        {"blmove", "write"},
        {"blmpop", "write"},
        {"blpop", "write"},
        {"brpop", "write"},
        {"brpoplpush", "write"},
        {"sadd", "write"},
        {"scan", "read"},
        {"scard", "read"},
        {"sdiff", "read"},
        {"sdiffstore", "write"},
        {"set", "write"},
        {"setbit", "write"},
        {"setex", "write"},
        {"setnx", "write"},
        {"setrange", "write"},
        {"sinter", "read"},
        {"sintercard", "read"},
        {"sinterstore", "write"},
        {"sismember", "read"},
        {"smembers", "read"},
        {"smismember", "read"},
        {"smove", "write"},
        {"sort", "write"},
        {"sort_ro", "read"},
        {"spop", "write"},
        {"srandmember", "read"},
        {"srem", "write"},
        {"sscan", "read"},
        {"strlen", "read"},
        {"substr", "read"},
        {"sunion", "read"},
        {"sunionstore", "write"},
        {"swapdb", "write"},
        {"touch", "read"},
        {"ttl", "read"},
        {"type", "read"},
        {"unlink", "write"},
        {"xack", "write"},
        {"xadd", "write"},
        {"xautoclaim", "write"},
        {"xclaim", "write"},
        {"xdel", "write"},
        {"xgroup create", "write"},
        {"xgroup createconsumer", "write"},
        {"xgroup delconsumer", "write"},
        {"xgroup destroy", "write"},
        {"xgroup setid", "write"},
        {"xinfo consumers", "read"},
        {"xinfo groups", "read"},
        {"xinfo stream", "read"},
        {"xlen", "read"},
        {"xpending", "read"},
        {"xrange", "read"},
        {"xread", "read"},
        {"xreadgroup", "write"},
        {"xrevrange", "read"},
        {"xsetid", "write"},
        {"xtrim", "write"},
        {"zadd", "write"},
        {"zcard", "read"},
        {"zcount", "read"},
        {"zdiff", "read"},
        {"zdiffstore", "write"},
        {"zincrby", "write"},
        {"zinter", "read"},
        {"zintercard", "read"},
        {"zinterstore", "write"},
        {"zlexcount", "read"},
        {"zmpop", "write"},
        {"zmscore", "read"},
        {"zpopmax", "write"},
        {"zpopmin", "write"},
        {"zrandmember", "read"},
        {"zrange", "read"},
        {"zrangebylex", "read"},
        {"zrangebyscore", "read"},
        {"zrangestore", "write"},
        {"zrank", "read"},
        {"zrem", "write"},
        {"zremrangebylex", "write"},
        {"zremrangebyrank", "write"},
        {"zremrangebyscore", "write"},
        {"zrevrange", "read"},
        {"zrevrangebylex", "read"},
        {"zrevrangebyscore", "read"},
        {"zrevrank", "read"},
        {"zscan", "read"},
        {"zscore", "read"},
        {"zunion", "read"},
        {"zunionstore", "write"},
    };

    PubSubManager pub_sub_mgr_;
    // general indicator for stopping service, e.g. metrics collector
    std::atomic<bool> stopping_indicator_{false};
    std::optional<std::thread> metrics_collector_thd_;

    friend class MultiTransactionHandler;
};

bool CheckCommandLineFlagIsDefault(const char *name);
}  // namespace EloqKV
