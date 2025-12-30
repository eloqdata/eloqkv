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
#include "redis_service.h"

#include <absl/types/span.h>
#include <bthread/mutex.h>
#include <bthread/task_group.h>
#include <butil/strings/string_piece.h>
#include <butil/strings/string_util.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <sys/types.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "INIReader.h"
#include "catalog_factory.h"
#include "data_substrate.h"
#include "eloq_metrics/include/metrics.h"
#include "eloqkv_key.h"
#include "error_messages.h"
#include "kv_store.h"
#include "lua_interpreter.h"
#include "metrics_registry_impl.h"
#include "redis_command.h"
#include "redis_connection_context.h"
#include "redis_handler.h"
#include "redis_metrics.h"
#include "redis_stats.h"
#include "redis_string_match.h"
#include "sharder.h"
#include "tx_key.h"
// #include "store_handler/rocksdb_config.h"
#include "eloqkv_catalog_factory.h"
#include "tx_execution.h"
#include "tx_request.h"
#include "tx_service.h"
#include "tx_service_metrics.h"
#include "tx_util.h"
#include "type.h"

extern "C"
{
#include "crcspeed/crc64speed.h"
}
namespace bthread
{
extern BAIDU_THREAD_LOCAL TaskGroup *tls_task_group;
};

namespace brpc
{
DECLARE_int32(event_dispatcher_num);
}

EloqKV::RedisCatalogFactory catalog_factory;
// DEFINE all gflags here

DECLARE_int32(node_memory_limit_mb);
DEFINE_bool(cc_notify,
            true,
            "Notify the txrequest sender when cc request finishes");
DECLARE_bool(cmd_read_catalog);
DEFINE_int32(eloqkv_port, 6379, "EloqKv Port");
#ifdef VECTOR_INDEX_ENABLED
DEFINE_int32(vector_index_worker_num,
             1,
             "Number of Vector Index worker threads");
#endif
DEFINE_bool(cluster_mode,
            false,
            "enable cluster mode even if there is only one node group, "
            "compatible with redis cluster protocol");

DEFINE_int32(slow_log_threshold,
             10000,
             "Threshold for logging a query as slow query.");

DEFINE_uint32(slow_log_max_length,
              128,
              "Max number of logs kept in slow query log.");

DEFINE_bool(enable_redis_stats, true, "Enable to collect redis statistics.");
DEFINE_bool(enable_cmd_sort, false, "Enable to sort command in Multi-Exec.");
DEFINE_string(isolation_level,
              "ReadCommitted",
              "Isolation level of simple commands.");
DEFINE_string(protocol,
              "OccRead",
              "Concurrency control protocol of simple commands.");
DEFINE_string(txn_isolation_level,
              "RepeatableRead",
              "Isolation level of MULTI/EXEC and Lua transactions.");
DEFINE_string(
    txn_protocol,
    "OCC",
    "Concurrency control protocol of MULTI/EXEC and Lua transactions.");

DEFINE_uint32(proxy_port_offset, 0, "Proxy port offset.");

DEFINE_bool(retry_on_occ_error, true, "Retry transaction on OCC caused error.");

namespace EloqKV
{
const auto NUM_VCPU = std::thread::hardware_concurrency();

// Global pub sub manager and publish function for eloqkv
PubSubManager eloqkv_pub_sub_mgr;
std::function<void(std::string_view, std::string_view)> eloqkv_publish_func =
    [](std::string_view chan, std::string_view msg)
{ eloqkv_pub_sub_mgr.Publish(chan, msg); };
int databases;
std::string requirepass;
std::string redis_ip_port;
brpc::Acceptor *server_acceptor = nullptr;

// The maximum size of a object.
constexpr uint64_t MAX_OBJECT_SIZE = 256 * 1024 * 1024;  // 256MB
#if defined(DATA_STORE_TYPE_ELOQDSS_ELOQSTORE)
constexpr uint64_t MAX_KEY_SIZE = 2048;
#else
constexpr uint64_t MAX_KEY_SIZE = 32 * 1024 * 1024;  // 32MB
#endif

// Maintain slow log for each bthread task group.

std::vector<std::unique_ptr<bthread::Mutex>> slow_log_mutexes_;
std::vector<std::vector<SlowLogEntry>> slow_log_;
std::atomic<int> slow_log_threshold_;
std::atomic<uint32_t> slow_log_max_length_;
std::vector<uint32_t> next_slow_log_idx_;
std::vector<uint32_t> slow_log_len_;
std::vector<uint32_t> next_slow_log_unique_id_;

RedisServiceImpl::RedisServiceImpl(const std::string &config_file,
                                   const char *version)
{
    version_ = version;
    config_file_ = config_file;
}

bool RedisServiceImpl::Init(brpc::Server &brpc_server)
{
    INIReader config_reader(config_file_);

    if (!config_file_.empty() && config_reader.ParseError() != 0)
    {
        LOG(ERROR) << "Error: Can't load config file.";
        return false;
    }

    // Engine registration: EloqKv
    auto &ds = DataSubstrate::Instance();

    databases = config_reader.GetInteger("local", "databases", 16);

    // Prebuilt Redis tables (same names as today: data_table_0 ..
    // data_table_15)
    std::vector<std::pair<txservice::TableName, std::string>> prebuilt_tables;
    prebuilt_tables.reserve(databases);

    for (int i = 0; i < databases; i++)
    {
        std::string table_name("data_table_");
        table_name.append(std::to_string(i));
        TableName redis_table_name(
            std::move(table_name), TableType::Primary, TableEngine::EloqKv);
        std::string image = redis_table_name.String();
#if defined(DATA_STORE_TYPE_CASSANDRA)
        EloqDS::CassCatalogInfo temp_kv_info(image, "");
        auto kv_info_str = temp_kv_info.Serialize();
        image = EloqDS::SerializeSchemaImage("", kv_info_str, "");
#elif defined(DATA_STORE_TYPE_DYNAMODB)
        // TODO(lokax):
#elif defined(DATA_STORE_TYPE_ROCKSDB)
        // TODO(lokax):
#endif
        prebuilt_tables.emplace_back(redis_table_name, image);
        redis_table_names_.push_back(redis_table_name);
    }

    // EloqKV-specific metrics (follow commented metrics_init.cpp patterns)
    std::vector<std::tuple<metrics::Name,
                           metrics::Type,
                           std::vector<metrics::LabelGroup>>>
        engine_metrics;

    for (const auto &[cmd, _] : EloqKV::command_types)
    {
        std::vector<metrics::LabelGroup> label_groups = {{"type", {cmd}}};

        engine_metrics.push_back(
            std::make_tuple(metrics::NAME_REDIS_COMMAND_DURATION,
                            metrics::Type::Histogram,
                            label_groups));
        engine_metrics.push_back(
            std::make_tuple(metrics::NAME_REDIS_COMMAND_TOTAL,
                            metrics::Type::Counter,
                            label_groups));
    }

    for (const auto &access_type : {"read", "write"})
    {
        engine_metrics.push_back(std::make_tuple(
            metrics::NAME_REDIS_COMMAND_AGGREGATED_TOTAL,
            metrics::Type::Counter,
            std::vector<metrics::LabelGroup>{{"access_type", {access_type}}}));
        engine_metrics.push_back(std::make_tuple(
            metrics::NAME_REDIS_COMMAND_AGGREGATED_DURATION,
            metrics::Type::Histogram,
            std::vector<metrics::LabelGroup>{{"access_type", {access_type}}}));
    }

    if (!ds.RegisterEngine(txservice::TableEngine::EloqKv,
                           &catalog_factory,
                           /*system_handler=*/nullptr,
                           std::move(prebuilt_tables),
                           std::move(engine_metrics),
                           eloqkv_publish_func))
    {
        LOG(ERROR) << "Failed to register EloqKV engine with DataSubstrate";
        return false;
    }

    requirepass = config_reader.GetString("local", "requirepass", "");

    redis_port_ = !CheckCommandLineFlagIsDefault("eloqkv_port")
                      ? FLAGS_eloqkv_port
                      : config_reader.GetInteger(
                            "local", "eloqkv_port", FLAGS_eloqkv_port);
    proxy_port_offset_ = !CheckCommandLineFlagIsDefault("proxy_port_offset")
                             ? FLAGS_proxy_port_offset
                             : config_reader.GetInteger(
        "local", "proxy_port_offset", 0);
    FLAGS_cluster_mode = !CheckCommandLineFlagIsDefault("cluster_mode")
                             ? FLAGS_cluster_mode
                             : config_reader.GetBoolean(
                                   "local", "cluster_mode", FLAGS_cluster_mode);
    FLAGS_cc_notify =
        !CheckCommandLineFlagIsDefault("cc_notify")
            ? FLAGS_cc_notify
            : config_reader.GetBoolean("local", "cc_notify", FLAGS_cc_notify);

    FLAGS_cmd_read_catalog =
        !CheckCommandLineFlagIsDefault("cmd_read_catalog")
            ? FLAGS_cmd_read_catalog
            : config_reader.GetBoolean(
                  "local", "cmd_read_catalog", FLAGS_cmd_read_catalog);

    slow_log_threshold_ =
        !CheckCommandLineFlagIsDefault("slow_log_threshold")
            ? FLAGS_slow_log_threshold
            : config_reader.GetInteger("local", "slow_log_threshold", 10000);

    slow_log_max_length_ =
        !CheckCommandLineFlagIsDefault("slow_log_max_length")
            ? FLAGS_slow_log_max_length
            : config_reader.GetInteger("local", "slow_log_max_length", 128);

    if (slow_log_threshold_ < -1)
    {
        // slowlog threshold master >= -1
        slow_log_threshold_ = -1;
    }

    config_.try_emplace("slowlog-log-slower-than",
                        std::to_string(slow_log_threshold_));
    config_.try_emplace("slowlog-max-len",
                        std::to_string(slow_log_max_length_));

    std::srand(std::time(nullptr));
    crc64speed_init_big();

    enable_redis_stats_ =
        !CheckCommandLineFlagIsDefault("enable_redis_stats")
            ? FLAGS_enable_redis_stats
            : config_reader.GetBoolean(
                  "local", "enable_redis_stats", FLAGS_enable_redis_stats);
    if (enable_redis_stats_)
    {
        RedisStats::ExposeBVar();
    }

    start_sec_ = butil::cpuwide_time_s();

    if (CheckCommandLineFlagIsDefault("enable_cmd_sort"))
    {
        bool enable_cmd_sort = config_reader.GetBoolean(
            "local", "enable_cmd_sort", FLAGS_enable_cmd_sort);
        GFLAGS_NAMESPACE::SetCommandLineOption(
            "enable_cmd_sort", enable_cmd_sort ? "true" : "false");
    }

    // Initialize simple commands' isolation level and concurrency control
    // protocol.
    std::string isolation_level =
        !CheckCommandLineFlagIsDefault("isolation_level")
            ? FLAGS_isolation_level
            : config_reader.GetString(
                  "local", "isolation_level", FLAGS_isolation_level);
    std::string cc_protocol =
        !CheckCommandLineFlagIsDefault("protocol")
            ? FLAGS_protocol
            : config_reader.GetString("local", "protocol", FLAGS_protocol);
    IsolationLevel iso_level;
    CcProtocol protocol;
    // Support ReadCommitted and RepeatableRead.
    if (strcasecmp(isolation_level.c_str(), "RepeatableRead") == 0)
    {
        iso_level = txservice::IsolationLevel::RepeatableRead;
    }
    else if (strcasecmp(isolation_level.c_str(), "ReadCommitted") == 0)
    {
        iso_level = txservice::IsolationLevel::ReadCommitted;
    }
    else
    {
        LOG(ERROR) << "Unsupported isolation level: " << isolation_level;
        return false;
    }
    // Support OCC, OccRead and Locking.
    if (strcasecmp(cc_protocol.c_str(), "OCC") == 0)
    {
        protocol = txservice::CcProtocol::OCC;
    }
    else if (strcasecmp(cc_protocol.c_str(), "OCCRead") == 0)
    {
        protocol = txservice::CcProtocol::OccRead;
    }
    else if (strcasecmp(cc_protocol.c_str(), "Locking") == 0)
    {
        protocol = txservice::CcProtocol::Locking;
    }
    else
    {
        LOG(ERROR) << "Unsupported concurrency control protocol: "
                   << cc_protocol;
        return false;
    }
    RedisCommandHandler::Initialize(iso_level, protocol);
    AddHandlers();

    // Initialize MULTI/EXEC and Lua transactions' isolation level and
    // concurrency control protocol.
    std::string txn_iso_level =
        !CheckCommandLineFlagIsDefault("txn_isolation_level")
            ? FLAGS_txn_isolation_level
            : config_reader.GetString(
                  "local", "txn_isolation_level", FLAGS_txn_isolation_level);
    std::string txn_protocol =
        !CheckCommandLineFlagIsDefault("txn_protocol")
            ? FLAGS_txn_protocol
            : config_reader.GetString(
                  "local", "txn_protocol", FLAGS_txn_protocol);

    // Support ReadCommitted and RepeatableRead.
    if (strcasecmp(txn_iso_level.c_str(), "RepeatableRead") == 0)
    {
        txn_isolation_level_ = IsolationLevel::RepeatableRead;
    }
    else if (strcasecmp(txn_iso_level.c_str(), "ReadCommitted") == 0)
    {
        txn_isolation_level_ = IsolationLevel::ReadCommitted;
    }
    else
    {
        LOG(ERROR) << "Unsupported txn isolation level: " << txn_iso_level
                   << ", RepeatableRead is used by default.";
        return false;
    }

    // Support OCC, OccRead and Locking.
    if (strcasecmp(txn_protocol.c_str(), "OCC") == 0)
    {
        txn_protocol_ = CcProtocol::OCC;
    }
    else if (strcasecmp(txn_protocol.c_str(), "OCCRead") == 0)
    {
        txn_protocol_ = CcProtocol::OccRead;
    }
    else if (strcasecmp(txn_protocol.c_str(), "Locking") == 0)
    {
        txn_protocol_ = CcProtocol::Locking;
    }
    else
    {
        LOG(ERROR) << "Unsupported txn concurrency control protocol: "
                   << txn_protocol << ", OCC is used by default.";
        return false;
    }

    retry_on_occ_error_ =
        !CheckCommandLineFlagIsDefault("retry_on_occ_error")
            ? FLAGS_retry_on_occ_error
            : config_reader.GetBoolean(
                  "local", "retry_on_occ_error", FLAGS_retry_on_occ_error);

#ifdef VECTOR_INDEX_ENABLED
    int vector_index_worker_num =
        !CheckCommandLineFlagIsDefault("vector_index_worker_num")
            ? FLAGS_vector_index_worker_num
            : config_reader.GetInteger("local",
                                       "vector_index_worker_num",
                                       FLAGS_vector_index_worker_num);
    if (vector_index_worker_num > 0)
    {
        vector_index_worker_pool_ =
            std::make_unique<txservice::TxWorkerPool>(vector_index_worker_num);
    }
    // Vector handler initialization moved to Start() since it requires
    // tx_service_
#endif

    return true;
}

bool RedisServiceImpl::Start(brpc::Server &brpc_server)
{
    auto &ds = DataSubstrate::Instance();

    // Attach to TxService and DataStoreHandler now that DataSubstrate is
    // started.

    tx_service_ = ds.GetTxService();
    if (tx_service_ == nullptr)
    {
        LOG(ERROR) << "Error: TxService is not initialized.";
        return false;
    }
    bool enable_store = ds.GetCoreConfig().enable_data_store;
    store_hd_ = ds.GetStoreHandler();
    if (enable_store && store_hd_ == nullptr)
    {
        LOG(ERROR) << "Error: DataStoreHandler is not initialized.";
        return false;
    }
    core_num_ = ds.GetCoreConfig().core_num;
    if (core_num_ == 0)
    {
        LOG(ERROR) << "Error: core_num is 0.";
        return false;
    }
    node_memory_limit_mb_ = FLAGS_node_memory_limit_mb;
    skip_kv_ = !ds.GetCoreConfig().enable_data_store;
    skip_wal_ = !ds.GetCoreConfig().enable_wal;

    // Size slow_log_* structures based on core_num_
    slow_log_mutexes_.clear();
    for (size_t core_idx = 0; core_idx < core_num_; ++core_idx)
    {
        slow_log_mutexes_.emplace_back(std::make_unique<bthread::Mutex>());
    }

    slow_log_.resize(core_num_);
    next_slow_log_idx_.resize(core_num_, 0);
    for (size_t core_idx = 0; core_idx < core_num_; ++core_idx)
    {
        // high 16 bits: core_id
        // low 48 bits: counter
        next_slow_log_unique_id_.push_back(core_idx << 48);
    }

    slow_log_len_.resize(core_num_, 0);

    // Each task group will maintain its own slow log to avoid race
    // condition.
    for (uint32_t i = 0; i < core_num_; ++i)
    {
        slow_log_[i].resize(slow_log_max_length_);
    }

    // Compute redis_ip_port using network config
    std::string local_ip = ds.GetNetworkConfig().local_ip;
    if (ds.GetNetworkConfig().bind_all)
    {
        redis_ip_port = "0.0.0.0:" + std::to_string(redis_port_);
    }
    else
    {
        redis_ip_port = local_ip + ":" + std::to_string(redis_port_);
    }

    DLOG(INFO) << "Local EloqKv server ip port: " << redis_ip_port;

    // Check bootstrap and handle it
    if (ds.GetCoreConfig().bootstrap)
    {
        Stop();
        ds.Shutdown();

        LOG(INFO) << "bootstrap done !!!";

        if (!FLAGS_alsologtostderr)
        {
            std::cout << "bootstrap done !!!" << std::endl;
        }

#if BRPC_WITH_GLOG
        google::ShutdownGoogleLogging();
#endif

        exit(0);
    }

    // Metrics-related initialization that depends on
    // DataSubstrate::InitializeMetrics
    stopping_indicator_.store(false, std::memory_order_release);

    if (metrics::enable_metrics)
    {
        redis_cmd_current_rounds_.resize(core_num_);
        for (auto &vec : redis_cmd_current_rounds_)
        {
            vec.resize(command_types.size() + 10, 1);
        }

        // The metrics registry and redis_meter are already created by
        // DataSubstrate. We only need to start our collector thread here.
        metrics_collector_thd_ =
            std::thread(&RedisServiceImpl::CollectConnectionsMetrics,
                        this,
                        std::ref(brpc_server));
    }

#ifdef VECTOR_INDEX_ENABLED
    // Initialize vector handler now that tx_service_ is available
    if (vector_index_worker_pool_ != nullptr)
    {
        INIReader config_reader(config_file_);
        EloqVec::CloudConfig vector_cloud_config(config_reader);
        const std::string &data_path = ds.GetCoreConfig().data_path;
        if (!EloqVec::VectorHandler::InitHandlerInstance(
                tx_service_,
                vector_index_worker_pool_.get(),
                data_path,
                &vector_cloud_config))
        {
            LOG(ERROR) << "Failed to initialize vector handler instance";
            return false;
        }
    }
#endif

    return true;
}

void RedisServiceImpl::Stop()
{
#ifdef VECTOR_INDEX_ENABLED
    EloqVec::VectorHandler::DestroyHandlerInstance();
    if (vector_index_worker_pool_ != nullptr)
    {
        LOG(INFO) << "Shutting down the vector index worker pool.";
        vector_index_worker_pool_->Shutdown();
        vector_index_worker_pool_ = nullptr;
        LOG(INFO) << "Vector index worker pool shut down.";
    }
#endif
    // stopping other service like metrics collector
    stopping_indicator_.store(true, std::memory_order_release);
    if (metrics_collector_thd_.has_value() &&
        metrics_collector_thd_->joinable())
    {
        metrics_collector_thd_->join();
    }

    if (enable_redis_stats_)
    {
        RedisStats::HideBVar();
    }
}

// The number of master nodes serving at least one hash slot in the cluster.
uint32_t RedisServiceImpl::RedisClusterSize()
{
    return txservice::Sharder::Instance().NodeGroupCount();
}

// The total number of known nodes in the cluster
uint32_t RedisServiceImpl::RedisClusterNodesCount()
{
    return txservice::Sharder::Instance().GetNodeCount();
}

void RedisServiceImpl::GetReplicaNodesStatus(
    std::unordered_map<uint32_t, std::vector<HostNetworkInfo>> &nodes_status)
    const
{
    // map{ng_id,[nodes_info,...]}
    std::vector<std::unique_ptr<txservice::remote::FetchNodeInfoRequest>>
        req_vec;
    std::vector<std::unique_ptr<txservice::remote::FetchNodeInfoResponse>>
        resp_vec;
    std::vector<std::unique_ptr<brpc::Controller>> cntl_vec;
    std::unordered_map<NodeGroupId, std::vector<NodeConfig>> ng_configs =
        Sharder::Instance().GetNodeGroupConfigs();
    for (const auto &[ng_id, nodes] : ng_configs)
    {
        for (auto &node : nodes)
        {
            if (node.is_candidate_)
            {
                auto it = nodes_status.try_emplace(ng_id);
                auto &ref1 = it.first->second.emplace_back();
                ref1.ip = node.host_name_;
                ref1.node_id = node.node_id_;
                ref1.port = TxPortToRedisPort(node.port_);

                std::shared_ptr<brpc::Channel> channel =
                    Sharder::Instance().GetCcNodeServiceChannel(node.node_id_);
                if (!channel)
                {
                    ref1.status = HostStatus::Failed;
                    continue;
                }

                req_vec.emplace_back(
                    std::make_unique<
                        txservice::remote::FetchNodeInfoRequest>());
                resp_vec.emplace_back(
                    std::make_unique<
                        txservice::remote::FetchNodeInfoResponse>());
                cntl_vec.emplace_back(std::make_unique<brpc::Controller>());

                auto *req = req_vec.back().get();
                auto *resp = resp_vec.back().get();
                auto *cntl = cntl_vec.back().get();

                req->set_ng_id(ng_id);
                req->set_node_id(node.node_id_);
                cntl->set_timeout_ms(1000);
                cntl->set_max_retry(2);
                txservice::remote::CcRpcService_Stub stub(channel.get());
                stub.FetchNodeInfo(cntl, req, resp, brpc::DoNothing());
            }
        }
    }

    for (auto &ref : cntl_vec)
    {
        // wait all rpc call
        brpc::Join(ref->call_id());
    }

    for (size_t i = 0; i < resp_vec.size(); i++)
    {
        // handler results
        auto *resp = resp_vec.at(i).get();
        auto *cntl = cntl_vec.at(i).get();

        // use the original request to get ng_id and node_id since response
        // might not be set if rpc call fails
        uint32_t ng_id = req_vec.at(i)->ng_id();
        uint32_t node_id = req_vec.at(i)->node_id();

        auto &ng_ref = nodes_status.at(ng_id);
        HostNetworkInfo *node_info = nullptr;
        for (auto &node_ref : ng_ref)
        {
            if (node_ref.node_id == node_id)
            {
                node_info = &node_ref;
                break;
            }
        }
        assert(node_info != nullptr);

        if (cntl->Failed())
        {
            DLOG(INFO) << "Fetch node info rpc call failed, "
                       << cntl->ErrorText();
            node_info->status = HostStatus::Failed;
        }
        else if (resp->status() == txservice::remote::NodeStatus::Loading)
        {
            node_info->status = HostStatus::Loading;
        }
        else if (resp->status() == txservice::remote::NodeStatus::Online)
        {
            node_info->status = HostStatus::Online;
        }
    }
}

void RedisServiceImpl::GetNodeSlotsInfo(
    const std::unordered_map<NodeGroupId, NodeId> &ng_leaders,
    std::unordered_map<NodeId, std::vector<SlotPair>> &nodes_slots) const
{
    LocalCcShards *local_cc_shards =
        txservice::Sharder::Instance().GetLocalCcShards();
    auto native_ng = txservice::Sharder::Instance().NativeNodeGroup();
    std::vector<std::pair<uint16_t, NodeGroupId>> bucket_owners =
        local_cc_shards->GetAllBucketOwners(native_ng);

    // split buckets_in_ng for all ngs' leader node and sort bucket_id for each
    // ng leader node
    std::unordered_map<NodeId, std::vector<uint16_t>> owner_to_bucket_ids;
    uint32_t node_group_cnt = static_cast<uint32_t>(ng_leaders.size());
    for (const auto &[ng_id, leader_node_id] : ng_leaders)
    {
        // uint32_t leader_node_id = ng_leaders.at(ng_id);

        // One node maybe multi ngs' leader.
        auto it = owner_to_bucket_ids.find(leader_node_id);
        if (it == owner_to_bucket_ids.end())
        {
            owner_to_bucket_ids.emplace(leader_node_id,
                                        std::vector<uint16_t>());
            owner_to_bucket_ids.at(leader_node_id)
                .reserve(bucket_owners.size() / node_group_cnt + 1);
        }
        else
        {
            auto current_size = owner_to_bucket_ids.at(leader_node_id).size();
            owner_to_bucket_ids.at(leader_node_id)
                .reserve(bucket_owners.size() / node_group_cnt + 1 +
                         current_size);
        }

        auto &bucket_ids = owner_to_bucket_ids.at(leader_node_id);
        for (const auto &pair : bucket_owners)
        {
            if (pair.second == ng_id)
            {
                bucket_ids.push_back(pair.first);
            }
        }
    }

    for (auto &[node_id, bucket_ids] : owner_to_bucket_ids)
    {
        if (bucket_ids.empty())
        {
            continue;
        }

        // map buckets to slots
        assert(txservice::total_range_buckets == 1024);
        std::vector<uint32_t> slot_ids;
        // slots count is 16 times of bucket count
        slot_ids.reserve(bucket_ids.size() * 16);
        for (uint16_t bucket_id : bucket_ids)
        {
            for (int i = 0; i < 16; i++)
            {
                slot_ids.push_back(bucket_id + i * 1024);
            }
        }

        std::sort(slot_ids.begin(), slot_ids.end());

        auto res = nodes_slots.try_emplace(node_id);
        std::vector<SlotPair> &slots_vec = res.first->second;
        SlotPair *slot_pair = &(slots_vec.emplace_back());
        slot_pair->start_slot_range = slot_ids.front();
        slot_pair->end_slot_range = slot_ids.front();

        for (size_t idx = 1; idx < slot_ids.size(); idx++)
        {
            if (slot_ids[idx - 1] + 1 != slot_ids[idx])
            {
                slot_pair = &(slots_vec.emplace_back());
                slot_pair->start_slot_range = slot_ids[idx];
            }
            slot_pair->end_slot_range = slot_ids[idx];
        }
    }
}

// For ClusterNodes command. Results will be returned through arg 'info'.
void RedisServiceImpl::RedisClusterNodes(std::vector<std::string> &info)
{
    auto all_node_groups = txservice::Sharder::Instance().AllNodeGroups();
    std::unordered_map<NodeGroupId, NodeId> ng_leaders;
    for (uint32_t ng_id : *all_node_groups)
    {
        auto leader_node = txservice::Sharder::Instance().LeaderNodeId(ng_id);
        ng_leaders.try_emplace(ng_id, leader_node);
    }

    std::unordered_map<NodeId, std::vector<SlotPair>> nodes_slots;
    GetNodeSlotsInfo(ng_leaders, nodes_slots);

    std::unordered_map<uint32_t, std::vector<HostNetworkInfo>> replicas_info;
    GetReplicaNodesStatus(replicas_info);

    for (auto &[ng_id, replicas] : replicas_info)
    {
        auto ng_it = ng_leaders.find(ng_id);
        if (ng_it != ng_leaders.end())
        {
            auto leader_node_id = ng_it->second;

            for (HostNetworkInfo &replica : replicas)
            {
                std::string node_info_str;
                node_info_str.append(replica.host_id());

                // ip:port@cport
                node_info_str.append(" ");
                node_info_str.append(replica.ip + ":" +
                                     std::to_string(ApplyPortOffset(replica.port)));
                node_info_str.append("@0");

                // flags
                node_info_str.append(" ");
                if (txservice::Sharder::Instance().NodeId() == replica.node_id)
                {
                    node_info_str.append("myself,");
                }

                if (replica.node_id == leader_node_id)
                {
                    node_info_str.append("master");
                    // master-node-id
                    node_info_str.append(" ");
                    node_info_str.append("-");
                }
                else
                {
                    node_info_str.append("slave");
                    // master-node-id
                    node_info_str.append(" ");
                    std::string node_id_str = std::to_string(leader_node_id);
                    node_info_str.append((40U - node_id_str.size()), '0');
                    node_info_str.append(node_id_str);
                }

                // ping-sent
                node_info_str.append(" ");
                node_info_str.append("0");

                // pong-recv
                node_info_str.append(" ");
                node_info_str.append("0");

                // config-epoch
                node_info_str.append(" ");
                node_info_str.append("0");

                // link-state
                node_info_str.append(" ");
                if (replica.status != HostStatus::Online)
                {
                    node_info_str.append("disconnected");
                }
                else
                {
                    node_info_str.append("connected");
                }

                // slot range (only master node has this part)
                if (replica.node_id == leader_node_id &&
                    replica.status == HostStatus::Online)
                {
                    node_info_str.append(" ");
                    auto slots_it = nodes_slots.find(replica.node_id);
                    if (slots_it != nodes_slots.end())
                    {
                        for (SlotPair &slot_pair : slots_it->second)
                        {
                            node_info_str.append(
                                std::to_string(slot_pair.start_slot_range));
                            node_info_str.append("-");
                            node_info_str.append(
                                std::to_string(slot_pair.end_slot_range));
                            node_info_str.append(" ");
                        }
                        node_info_str.pop_back();
                    }
                }
                info.push_back(node_info_str);
            }  // end-for
        }
    }  // end-for
}

// For ClusterSlots command. Results will be returned through arg 'info'.
void RedisServiceImpl::RedisClusterSlots(std::vector<SlotInfo> &info)
{
    auto all_node_groups = txservice::Sharder::Instance().AllNodeGroups();

    std::unordered_map<NodeGroupId, NodeId> ng_leaders;
    for (uint32_t ng_id : *all_node_groups)
    {
        auto leader_node = txservice::Sharder::Instance().LeaderNodeId(ng_id);
        ng_leaders.try_emplace(ng_id, leader_node);
    }

    std::unordered_map<NodeId, std::vector<SlotPair>> nodes_slots;
    GetNodeSlotsInfo(ng_leaders, nodes_slots);

    std::unordered_map<uint32_t, std::vector<HostNetworkInfo>> replicas_info;
    GetReplicaNodesStatus(replicas_info);

    for (auto &[ng_id, ng_replicas] : replicas_info)
    {
        auto ng_it = ng_leaders.find(ng_id);
        if (ng_it != ng_leaders.end())
        {
            auto leader_node_id = ng_it->second;
            auto slots_it = nodes_slots.find(leader_node_id);
            if (slots_it == nodes_slots.end() || slots_it->second.empty())
            {
                continue;
            }

            bool has_online_replica = false;
            for (auto replica : ng_replicas)
            {
                if (replica.status == HostStatus::Online)
                {
                    has_online_replica = true;
                    break;
                }
            }
            if (!has_online_replica)
            {
                continue;
            }

            for (SlotPair slot_pair : slots_it->second)
            {
                auto &slot_info = info.emplace_back();
                slot_info.start_slot_range = slot_pair.start_slot_range;
                slot_info.end_slot_range = slot_pair.end_slot_range;

                for (auto replica : ng_replicas)
                {
                    // Apply port offset before adding to slot_info
                    replica.port = ApplyPortOffset(replica.port);
                    
                    if (replica.node_id == leader_node_id)
                    {
                        slot_info.hosts.push_front(replica);
                    }
                    else if (replica.status == HostStatus::Online)
                    {
                        slot_info.hosts.push_back(replica);
                    }
                    else
                    {
                        continue;
                    }
                }
            }
        }  // end-if
    }  // end-for

    if (info.size() > 1)
    {
        std::sort(info.begin(), info.end());
    }
}

std::string RedisServiceImpl::GenerateMovedErrorMessage(uint16_t slot_id)
{
    std::vector<SlotInfo> slot_infos;
    RedisClusterSlots(slot_infos);

    std::string error_msg("MOVED ");
    error_msg.append(std::to_string(slot_id));
    for (auto &slot_info : slot_infos)
    {
        if (slot_info.start_slot_range <= slot_id &&
            slot_info.end_slot_range >= slot_id)
        {
            error_msg.append(" ");
            error_msg.append(slot_info.hosts.front().ip);
            error_msg.append(":");
            error_msg.append(std::to_string(slot_info.hosts.front().port));
            return error_msg;
        }
    }

    error_msg.append(" UNKNOWN");

    LOG(ERROR) << "slot not found";
    return error_msg;
}

template <typename Subtype, typename T>
bool RedisServiceImpl::SendTxRequestAndWaitResult(
    TransactionExecution *txm,
    TemplateTxRequest<Subtype, T> *tx_req,
    OutputHandler *error)
{
    bool success = SendTxRequest(txm, tx_req, error);
    if (!success)
    {
        return false;
    }

    tx_req->Wait();

    if (tx_req->IsError())
    {
        if (tx_req->ErrorCode() == TxErrorCode::TX_INIT_FAIL)
        {
            // The Txm fails to initialize. The TxRequest hasn't been processed.
            // If the TxRequest is auto_commit, abort the txm manually.
            if (auto *obj_cmd_tx_req =
                    dynamic_cast<ObjectCommandTxRequest *>(tx_req))
            {
                if (obj_cmd_tx_req->auto_commit_)
                {
                    AbortTx(txm);
                }
            }
            else if (auto *multi_tx_req =
                         dynamic_cast<MultiObjectCommandTxRequest *>(tx_req))
            {
                if (multi_tx_req->auto_commit_)
                {
                    AbortTx(txm);
                }
            }
        }
        else if (tx_req->ErrorCode() ==
                     TxErrorCode::WRITE_REQUEST_ON_SLAVE_NODE &&
                 !FLAGS_cluster_mode &&
                 Sharder::Instance().NodeGroupCount() ==
                     1)  // In sentinel mode, write request on a
                         // slave node is not allowed.
        {
            if (error != nullptr)
            {
                error->OnError(
                    "READONLY You can't write against a read only replica.");
            }
            return false;
        }
        else if (tx_req->ErrorCode() == TxErrorCode::DATA_NOT_ON_LOCAL_NODE ||
                 tx_req->ErrorCode() ==
                     TxErrorCode::WRITE_REQUEST_ON_SLAVE_NODE)
        {
            // If the tx request is a write request on a slave node in cluster
            // mode, we need to generate a MOVED error message.
            if (auto object_tx_req =
                    dynamic_cast<ObjectCommandTxRequest *>(tx_req))
            {
                auto key = object_tx_req->Key();
                uint16_t slot_id = key->Hash() & 0x3fff;

                if (error != nullptr)
                {
                    std::string error_msg = GenerateMovedErrorMessage(slot_id);
                    error->OnError(error_msg);
                }
                return false;
            }
            else if (auto multi_object_tx_req =
                         dynamic_cast<MultiObjectCommandTxRequest *>(tx_req))
            {
                RedisMultiObjectCommand *multi_obj_cmd =
                    static_cast<RedisMultiObjectCommand *>(
                        multi_object_tx_req->Command());
                std::vector<txservice::TxKey> *keys =
                    multi_obj_cmd->KeyPointers(0);
                assert(!keys->empty());
                // first key slot
                uint16_t slot_id = keys->at(0).Hash() & 0x3fff;

                if (error != nullptr)
                {
                    std::string error_msg = GenerateMovedErrorMessage(slot_id);
                    error->OnError(error_msg);
                }

                return false;
            }
            else
            {
                LOG(WARNING) << "MOVED for unknown type of tx request";
                if (error != nullptr)
                {
                    error->OnError("MOVED for unknown type of tx request");
                }
                return false;
            }
        }

        LOG(WARNING) << "txn: " << txm->TxNumber() << " "
                     << typeid(*tx_req).name()
                     << " error: " << tx_req->ErrorMsg();
        if (error != nullptr)
        {
            error->OnError(tx_req->ErrorMsg());
        }
        return false;
    }

    return true;
}

bool RedisServiceImpl::SendTxRequest(TransactionExecution *txm,
                                     TxRequest *tx_req,
                                     OutputHandler *error)
{
    assert(txm != nullptr && tx_req != nullptr);
    int err = txm->Execute(tx_req);
    if (err != 0)
    {
        LOG(WARNING) << "txn: " << txm->TxNumber() << " SendTxRequest "
                     << typeid(*tx_req).name() << " fail";
        if (error != nullptr)
        {
            error->OnError(TxErrorMessage(
                TxErrorCode::TX_REQUEST_TO_COMMITTED_ABORTED_TX));
        }
        tx_req->SetError(TxErrorCode::TX_REQUEST_TO_COMMITTED_ABORTED_TX);
        return false;
    }
    return true;
}

std::unique_ptr<LuaInterpreter> RedisServiceImpl::GetLuaInterpreter()
{
    std::unique_ptr<LuaInterpreter> interpreter;
    bool success = lua_interpreters_.try_dequeue(interpreter);
    if (!success)
    {
        return std::make_unique<LuaInterpreter>();
    }
    return interpreter;
}

void RedisServiceImpl::CleanAndReturnLuaInterpreter(
    std::unique_ptr<LuaInterpreter> lua_state)
{
    lua_state->CleanStack();
    lua_interpreters_.enqueue(std::move(lua_state));
}

void RedisServiceImpl::AddHandlers()
{
    auto &ping_hd =
        hd_vec_.emplace_back(std::make_unique<PingCommandHandler>(this));
    AddCommandHandler("ping", ping_hd.get());

    auto &auth_hd =
        hd_vec_.emplace_back(std::make_unique<AuthCommandHandler>(this));
    AddCommandHandler("auth", auth_hd.get());

    auto &select_hd =
        hd_vec_.emplace_back(std::make_unique<SelectCommandHandler>(this));
    AddCommandHandler("select", select_hd.get());

    auto &config_hd =
        hd_vec_.emplace_back(std::make_unique<ConfigCommandHandler>(this));
    AddCommandHandler("config", config_hd.get());

    auto &dbsize_hd =
        hd_vec_.emplace_back(std::make_unique<DBSizeCommandHandler>(this));
    AddCommandHandler("dbsize", dbsize_hd.get());

    auto &readonly_hd =
        hd_vec_.emplace_back(std::make_unique<ReadOnlyCommandHandler>(this));
    AddCommandHandler("readonly", readonly_hd.get());

    auto &info_hd =
        hd_vec_.emplace_back(std::make_unique<InfoCommandHandler>(this));
    AddCommandHandler("info", info_hd.get());

    auto &command_hd =
        hd_vec_.emplace_back(std::make_unique<CommandCommandHandler>(this));
    AddCommandHandler("command", command_hd.get());

    auto &cluster_hd =
        hd_vec_.emplace_back(std::make_unique<ClusterCommandHandler>(this));
    AddCommandHandler("cluster", cluster_hd.get());

    auto &sentinel_hd =
        hd_vec_.emplace_back(std::make_unique<SentinelCommandHandler>(this));
    AddCommandHandler("sentinel", sentinel_hd.get());

    auto &failover_hd =
        hd_vec_.emplace_back(std::make_unique<FailoverCommandHandler>(this));
    AddCommandHandler("failover", failover_hd.get());

    auto &client_hd =
        hd_vec_.emplace_back(std::make_unique<ClientCommandHandler>(this));
    AddCommandHandler("client", client_hd.get());

    auto &get_hd =
        hd_vec_.emplace_back(std::make_unique<GetCommandHandler>(this));
    AddCommandHandler("get", get_hd.get());

    auto &gd_hd =
        hd_vec_.emplace_back(std::make_unique<GetDelCommandHandler>(this));
    AddCommandHandler("getdel", gd_hd.get());

    auto &set_hd =
        hd_vec_.emplace_back(std::make_unique<SetCommandHandler>(this));
    AddCommandHandler("set", set_hd.get());

    auto &setnx_hd =
        hd_vec_.emplace_back(std::make_unique<SetNXCommandHandler>(this));
    AddCommandHandler("setnx", setnx_hd.get());

    auto &getset_hd =
        hd_vec_.emplace_back(std::make_unique<GetSetCommandHandler>(this));
    AddCommandHandler("getset", getset_hd.get());

    auto &strlen_hd =
        hd_vec_.emplace_back(std::make_unique<StrLenCommandHandler>(this));
    AddCommandHandler("strlen", strlen_hd.get());

    auto &setex_hd =
        hd_vec_.emplace_back(std::make_unique<SetExCommandHandler>(this));
    AddCommandHandler("setex", setex_hd.get());

    auto &psetex_hd =
        hd_vec_.emplace_back(std::make_unique<PSetExCommandHandler>(this));
    AddCommandHandler("psetex", psetex_hd.get());

    auto &getbit_hd =
        hd_vec_.emplace_back(std::make_unique<GetBitCommandHandler>(this));
    AddCommandHandler("getbit", getbit_hd.get());

    auto &getrange_hd =
        hd_vec_.emplace_back(std::make_unique<GetRangeCommandHandler>(this));
    AddCommandHandler("getrange", getrange_hd.get());

    auto &setbit_hd =
        hd_vec_.emplace_back(std::make_unique<SetBitCommandHandler>(this));
    AddCommandHandler("setbit", setbit_hd.get());

    auto &setrange_hd =
        hd_vec_.emplace_back(std::make_unique<SetRangeCommandHandler>(this));
    AddCommandHandler("setrange", setrange_hd.get());

    auto &append_hd =
        hd_vec_.emplace_back(std::make_unique<AppendCommandHandler>(this));
    AddCommandHandler("append", append_hd.get());

    auto &bf_hd =
        hd_vec_.emplace_back(std::make_unique<BitFieldCommandHandler>(this));
    AddCommandHandler("bitfield", bf_hd.get());

    auto &bfro_hd =
        hd_vec_.emplace_back(std::make_unique<BitFieldRoCommandHandler>(this));
    AddCommandHandler("bitfield_ro", bfro_hd.get());

    auto &bpos_hd =
        hd_vec_.emplace_back(std::make_unique<BitPosCommandHandler>(this));
    AddCommandHandler("bitpos", bpos_hd.get());

    auto &bop_hd =
        hd_vec_.emplace_back(std::make_unique<BitOpCommandHandler>(this));
    AddCommandHandler("bitop", bop_hd.get());

    auto &substr_hd =
        hd_vec_.emplace_back(std::make_unique<SubStrCommandHandler>(this));
    AddCommandHandler("substr", substr_hd.get());

    auto &float_hd =
        hd_vec_.emplace_back(std::make_unique<IncrByFloatCommandHandler>(this));
    AddCommandHandler("incrbyfloat", float_hd.get());

    auto &bitcount_hd =
        hd_vec_.emplace_back(std::make_unique<BitCountCommandHandler>(this));
    AddCommandHandler("bitcount", bitcount_hd.get());

    auto &echo_hd =
        hd_vec_.emplace_back(std::make_unique<EchoCommandHandler>(this));
    AddCommandHandler("echo", echo_hd.get());

    auto &lrange_hd =
        hd_vec_.emplace_back(std::make_unique<LRangeHandler>(this));
    AddCommandHandler("lrange", lrange_hd.get());

    auto &rpush_hd = hd_vec_.emplace_back(std::make_unique<RPushHandler>(this));
    AddCommandHandler("rpush", rpush_hd.get());

    auto &multi_hd = hd_vec_.emplace_back(std::make_unique<MultiHandler>(this));
    AddCommandHandler("multi", multi_hd.get());

    auto &begin_hd = hd_vec_.emplace_back(std::make_unique<BeginHandler>(this));
    AddCommandHandler("begin", begin_hd.get());

    auto &commit_hd =
        hd_vec_.emplace_back(std::make_unique<CommitHandler>(this));
    AddCommandHandler("commit", commit_hd.get());

    auto &rollback_hd =
        hd_vec_.emplace_back(std::make_unique<RollbackHandler>(this));
    AddCommandHandler("rollback", rollback_hd.get());

    auto &discard_hd =
        hd_vec_.emplace_back(std::make_unique<DiscardHandler>(this));
    AddCommandHandler("discard", discard_hd.get());

    auto &eval_hd = hd_vec_.emplace_back(std::make_unique<EvalHandler>(this));
    AddCommandHandler("eval", eval_hd.get());

    auto &script_hd =
        hd_vec_.emplace_back(std::make_unique<ScriptHandler>(this));
    AddCommandHandler("script", script_hd.get());

    auto &evalsha_hd =
        hd_vec_.emplace_back(std::make_unique<EvalshaHandler>(this));
    AddCommandHandler("evalsha", evalsha_hd.get());

    auto &hset_hd = hd_vec_.emplace_back(std::make_unique<HSetHandler>(this));
    AddCommandHandler("hset", hset_hd.get());
    AddCommandHandler("hmset", hset_hd.get());

    auto &hget_hd = hd_vec_.emplace_back(std::make_unique<HGetHandler>(this));
    AddCommandHandler("hget", hget_hd.get());

    auto &lpush_hd = hd_vec_.emplace_back(std::make_unique<LPushHandler>(this));
    AddCommandHandler("lpush", lpush_hd.get());

    auto &lpop_hd = hd_vec_.emplace_back(std::make_unique<LPopHandler>(this));
    AddCommandHandler("lpop", lpop_hd.get());

    auto &rpop_hd = hd_vec_.emplace_back(std::make_unique<RPopHandler>(this));
    AddCommandHandler("rpop", rpop_hd.get());

    auto &lmpop_hd = hd_vec_.emplace_back(std::make_unique<LMPopHandler>(this));
    AddCommandHandler("lmpop", lmpop_hd.get());

    auto &blmove_hd =
        hd_vec_.emplace_back(std::make_unique<BLMoveHandler>(this));
    AddCommandHandler("blmove", blmove_hd.get());

    auto &blmpop_hd =
        hd_vec_.emplace_back(std::make_unique<BLMPopHandler>(this));
    AddCommandHandler("blmpop", blmpop_hd.get());

    auto &blpop_hd = hd_vec_.emplace_back(std::make_unique<BLPopHandler>(this));
    AddCommandHandler("blpop", blpop_hd.get());

    auto &brpop_hd = hd_vec_.emplace_back(std::make_unique<BRPopHandler>(this));
    AddCommandHandler("brpop", brpop_hd.get());

    auto &brplp_hd =
        hd_vec_.emplace_back(std::make_unique<BRPopLPushHandler>(this));
    AddCommandHandler("brpoplpush", brplp_hd.get());

    auto &incr_hd = hd_vec_.emplace_back(std::make_unique<IncrHandler>(this));
    AddCommandHandler("incr", incr_hd.get());

    auto &decr_hd = hd_vec_.emplace_back(std::make_unique<DecrHandler>(this));
    AddCommandHandler("decr", decr_hd.get());

    auto &incrby_hd =
        hd_vec_.emplace_back(std::make_unique<IncrByHandler>(this));
    AddCommandHandler("incrby", incrby_hd.get());

    auto &decrby_hd =
        hd_vec_.emplace_back(std::make_unique<DecrByHandler>(this));
    AddCommandHandler("decrby", decrby_hd.get());

    auto &type_hd = hd_vec_.emplace_back(std::make_unique<TypeHandler>(this));
    AddCommandHandler("type", type_hd.get());

    auto &del_hd = hd_vec_.emplace_back(std::make_unique<DelHandler>(this));
    AddCommandHandler("del", del_hd.get());

    auto &exists_hd =
        hd_vec_.emplace_back(std::make_unique<ExistsHandler>(this));
    AddCommandHandler("exists", exists_hd.get());

    auto &zadd_hd = hd_vec_.emplace_back(std::make_unique<ZAddHandler>(this));
    AddCommandHandler("zadd", zadd_hd.get());

    auto &zrange_hd =
        hd_vec_.emplace_back(std::make_unique<ZRangeHandler>(this));
    AddCommandHandler("zrange", zrange_hd.get());

    auto &zrangestore_hd =
        hd_vec_.emplace_back(std::make_unique<ZRangeStoreHandler>(this));
    AddCommandHandler("zrangestore", zrangestore_hd.get());

    auto &zrem_hd = hd_vec_.emplace_back(std::make_unique<ZRemHandler>(this));
    AddCommandHandler("zrem", zrem_hd.get());

    auto &zscore_hd =
        hd_vec_.emplace_back(std::make_unique<ZScoreHandler>(this));
    AddCommandHandler("zscore", zscore_hd.get());

    auto &zmscore_hd =
        hd_vec_.emplace_back(std::make_unique<ZMScoreHandler>(this));
    AddCommandHandler("zmscore", zmscore_hd.get());

    auto &zmpop_hd = hd_vec_.emplace_back(std::make_unique<ZMPopHandler>(this));
    AddCommandHandler("zmpop", zmpop_hd.get());

    auto &zlexcount_hd =
        hd_vec_.emplace_back(std::make_unique<ZLexCountHandler>(this));
    AddCommandHandler("zlexcount", zlexcount_hd.get());

    auto &zpopmin_hd =
        hd_vec_.emplace_back(std::make_unique<ZPopMinHandler>(this));
    AddCommandHandler("zpopmin", zpopmin_hd.get());

    auto &zpopmax_hd =
        hd_vec_.emplace_back(std::make_unique<ZPopMaxHandler>(this));
    AddCommandHandler("zpopmax", zpopmax_hd.get());

    auto &zcount_hd =
        hd_vec_.emplace_back(std::make_unique<ZCountHandler>(this));
    AddCommandHandler("zcount", zcount_hd.get());

    auto &zcard_hd = hd_vec_.emplace_back(std::make_unique<ZCardHandler>(this));
    AddCommandHandler("zcard", zcard_hd.get());

    auto &zrangbylex_hd =
        hd_vec_.emplace_back(std::make_unique<ZRangeByLexHandler>(this));
    AddCommandHandler("zrangebylex", zrangbylex_hd.get());

    auto &zrangebyrank_hd =
        hd_vec_.emplace_back(std::make_unique<ZRangeByRankHandler>(this));
    AddCommandHandler("zrangebyrank", zrangebyrank_hd.get());

    auto &zrangebyscore_hd =
        hd_vec_.emplace_back(std::make_unique<ZRangeByScoreHandler>(this));
    AddCommandHandler("zrangebyscore", zrangebyscore_hd.get());

    auto &zrevrange_hd =
        hd_vec_.emplace_back(std::make_unique<ZRevRangeHandler>(this));
    AddCommandHandler("zrevrange", zrevrange_hd.get());

    auto &zrevrangebylex_hd =
        hd_vec_.emplace_back(std::make_unique<ZRevRangeByLexHandler>(this));
    AddCommandHandler("zrevrangebylex", zrevrangebylex_hd.get());

    auto &zrevrangebyscore_hd =
        hd_vec_.emplace_back(std::make_unique<ZRevRangeByScoreHandler>(this));
    AddCommandHandler("zrevrangebyscore", zrevrangebyscore_hd.get());

    auto &zremrang_hd =
        hd_vec_.emplace_back(std::make_unique<ZRemRangeHandler>(this));
    AddCommandHandler("zremrangebyscore", zremrang_hd.get());
    AddCommandHandler("zremrangebylex", zremrang_hd.get());
    AddCommandHandler("zremrangebyrank", zremrang_hd.get());

    auto &zscan_hd = hd_vec_.emplace_back(std::make_unique<ZScanHandler>(this));
    AddCommandHandler("zscan", zscan_hd.get());
    auto &zunion_hd =
        hd_vec_.emplace_back(std::make_unique<ZUnionHandler>(this));
    AddCommandHandler("zunion", zunion_hd.get());

    auto &zunion_store_hd =
        hd_vec_.emplace_back(std::make_unique<ZUnionStoreHandler>(this));
    AddCommandHandler("zunionstore", zunion_store_hd.get());

    auto &zinter_hd =
        hd_vec_.emplace_back(std::make_unique<ZInterHandler>(this));
    AddCommandHandler("zinter", zinter_hd.get());

    auto &zinter_card_hd =
        hd_vec_.emplace_back(std::make_unique<ZInterCardHandler>(this));
    AddCommandHandler("zintercard", zinter_card_hd.get());

    auto &zinter_store_hd =
        hd_vec_.emplace_back(std::make_unique<ZInterStoreHandler>(this));
    AddCommandHandler("zinterstore", zinter_store_hd.get());

    auto &zrm_hd =
        hd_vec_.emplace_back(std::make_unique<ZRandMemberHandler>(this));
    AddCommandHandler("zrandmember", zrm_hd.get());

    auto &zrank_hd = hd_vec_.emplace_back(std::make_unique<ZRankHandler>(this));
    AddCommandHandler("zrank", zrank_hd.get());

    auto &zrevrank_hd =
        hd_vec_.emplace_back(std::make_unique<ZRevRankHandler>(this));
    AddCommandHandler("zrevrank", zrevrank_hd.get());

    auto &zdiff_hd = hd_vec_.emplace_back(std::make_unique<ZDiffHandler>(this));
    AddCommandHandler("zdiff", zdiff_hd.get());

    auto &zds_hd =
        hd_vec_.emplace_back(std::make_unique<ZDiffStoreHandler>(this));
    AddCommandHandler("zdiffstore", zds_hd.get());

    auto &zib_hd = hd_vec_.emplace_back(std::make_unique<ZIncrByHandler>(this));
    AddCommandHandler("zincrby", zib_hd.get());

    auto &mset_hd = hd_vec_.emplace_back(std::make_unique<MSetHandler>(this));
    AddCommandHandler("mset", mset_hd.get());

    auto &msetnx_hd =
        hd_vec_.emplace_back(std::make_unique<MSetNxHandler>(this));
    AddCommandHandler("msetnx", msetnx_hd.get());

    auto &mget_hd = hd_vec_.emplace_back(std::make_unique<MGetHandler>(this));
    AddCommandHandler("mget", mget_hd.get());

    auto &hdel_hd = hd_vec_.emplace_back(std::make_unique<HDelHandler>(this));
    AddCommandHandler("hdel", hdel_hd.get());

    auto &hexists_hd =
        hd_vec_.emplace_back(std::make_unique<HExistsHandler>(this));
    AddCommandHandler("hexists", hexists_hd.get());

    auto &hincrby_hd =
        hd_vec_.emplace_back(std::make_unique<HIncrbyHandler>(this));
    AddCommandHandler("hincrby", hincrby_hd.get());

    auto &hibf_hd =
        hd_vec_.emplace_back(std::make_unique<HIncrByFloatHandler>(this));
    AddCommandHandler("hincrbyfloat", hibf_hd.get());

    auto &hmget_hd = hd_vec_.emplace_back(std::make_unique<HMGetHandler>(this));
    AddCommandHandler("hmget", hmget_hd.get());

    auto &hkeys_hd = hd_vec_.emplace_back(std::make_unique<HKeysHandler>(this));
    AddCommandHandler("hkeys", hkeys_hd.get());

    auto &hvals_hd = hd_vec_.emplace_back(std::make_unique<HValsHandler>(this));
    AddCommandHandler("hvals", hvals_hd.get());

    auto &hrandfield_hd =
        hd_vec_.emplace_back(std::make_unique<HRandFieldHandler>(this));
    AddCommandHandler("hrandfield", hrandfield_hd.get());

    auto &hscan_hd = hd_vec_.emplace_back(std::make_unique<HScanHandler>(this));
    AddCommandHandler("hscan", hscan_hd.get());

    auto &hsetnx_hd =
        hd_vec_.emplace_back(std::make_unique<HSetNxHandler>(this));
    AddCommandHandler("hsetnx", hsetnx_hd.get());

    auto &hgetall =
        hd_vec_.emplace_back(std::make_unique<HGetAllHandler>(this));
    AddCommandHandler("hgetall", hgetall.get());

    auto &hlen_hd = hd_vec_.emplace_back(std::make_unique<HLenHandler>(this));
    AddCommandHandler("hlen", hlen_hd.get());

    auto &hstrlen_hd =
        hd_vec_.emplace_back(std::make_unique<HStrLenHandler>(this));
    AddCommandHandler("hstrlen", hstrlen_hd.get());

    auto &llen_hd = hd_vec_.emplace_back(std::make_unique<LLenHandler>(this));
    AddCommandHandler("llen", llen_hd.get());

    auto &ltrim_hd = hd_vec_.emplace_back(std::make_unique<LTrimHandler>(this));
    AddCommandHandler("ltrim", ltrim_hd.get());

    auto &lindex_hd =
        hd_vec_.emplace_back(std::make_unique<LIndexHandler>(this));
    AddCommandHandler("lindex", lindex_hd.get());

    auto &linsert_hd =
        hd_vec_.emplace_back(std::make_unique<LInsertHandler>(this));
    AddCommandHandler("linsert", linsert_hd.get());

    auto &lpos_hd = hd_vec_.emplace_back(std::make_unique<LPosHandler>(this));
    AddCommandHandler("lpos", lpos_hd.get());

    auto &lset_hd = hd_vec_.emplace_back(std::make_unique<LSetHandler>(this));
    AddCommandHandler("lset", lset_hd.get());

    auto &lmove_hd = hd_vec_.emplace_back(std::make_unique<LMoveHandler>(this));
    AddCommandHandler("lmove", lmove_hd.get());

    auto &rpoplpush_hd =
        hd_vec_.emplace_back(std::make_unique<RPopLPushHandler>(this));
    AddCommandHandler("rpoplpush", rpoplpush_hd.get());

    auto &lrem_hd = hd_vec_.emplace_back(std::make_unique<LRemHandler>(this));
    AddCommandHandler("lrem", lrem_hd.get());

    auto &lpushx_hd =
        hd_vec_.emplace_back(std::make_unique<LPushXHandler>(this));
    AddCommandHandler("lpushx", lpushx_hd.get());

    auto &rpushx_hd =
        hd_vec_.emplace_back(std::make_unique<RPushXHandler>(this));
    AddCommandHandler("rpushx", rpushx_hd.get());

    auto &sadd_hd = hd_vec_.emplace_back(std::make_unique<SAddHandler>(this));
    AddCommandHandler("sadd", sadd_hd.get());

    auto &smem_hd =
        hd_vec_.emplace_back(std::make_unique<SMembersHandler>(this));
    AddCommandHandler("smembers", smem_hd.get());

    auto &srem_hd = hd_vec_.emplace_back(std::make_unique<SRemHandler>(this));
    AddCommandHandler("srem", srem_hd.get());

    auto &scard_hd = hd_vec_.emplace_back(std::make_unique<SCardHandler>(this));
    AddCommandHandler("scard", scard_hd.get());

    auto &sdiff_hd = hd_vec_.emplace_back(std::make_unique<SDiffHandler>(this));
    AddCommandHandler("sdiff", sdiff_hd.get());

    auto &sds_hd =
        hd_vec_.emplace_back(std::make_unique<SDiffStoreHandler>(this));
    AddCommandHandler("sdiffstore", sds_hd.get());

    auto &sinter_hd =
        hd_vec_.emplace_back(std::make_unique<SInterHandler>(this));
    AddCommandHandler("sinter", sinter_hd.get());

    auto &sinters_hd =
        hd_vec_.emplace_back(std::make_unique<SInterStoreHandler>(this));
    AddCommandHandler("sinterstore", sinters_hd.get());

    auto &sinterc_hd =
        hd_vec_.emplace_back(std::make_unique<SInterCardHandler>(this));
    AddCommandHandler("sintercard", sinterc_hd.get());

    auto &sism_hd =
        hd_vec_.emplace_back(std::make_unique<SIsMemberHandler>(this));
    AddCommandHandler("sismember", sism_hd.get());

    auto &smism_hd =
        hd_vec_.emplace_back(std::make_unique<SMIsMemberHandler>(this));
    AddCommandHandler("smismember", smism_hd.get());

    auto &smove_hd = hd_vec_.emplace_back(std::make_unique<SMoveHandler>(this));
    AddCommandHandler("smove", smove_hd.get());

    auto &sunion_hd =
        hd_vec_.emplace_back(std::make_unique<SUnionHandler>(this));
    AddCommandHandler("sunion", sunion_hd.get());

    auto &sus_hd =
        hd_vec_.emplace_back(std::make_unique<SUnionStoreHandler>(this));
    AddCommandHandler("sunionstore", sus_hd.get());

    auto &srm_hd =
        hd_vec_.emplace_back(std::make_unique<SRandMemberHandler>(this));
    AddCommandHandler("srandmember", srm_hd.get());

    auto &spop_hd = hd_vec_.emplace_back(std::make_unique<SPopHandler>(this));
    AddCommandHandler("spop", spop_hd.get());

    auto &sscan_hd = hd_vec_.emplace_back(std::make_unique<SScanHandler>(this));
    AddCommandHandler("sscan", sscan_hd.get());

    auto &sort_hd = hd_vec_.emplace_back(std::make_unique<SortHandler>(this));
    AddCommandHandler("sort", sort_hd.get());
    AddCommandHandler("sort_ro", sort_hd.get());

    auto &scan_hd = hd_vec_.emplace_back(std::make_unique<ScanHandler>(this));
    AddCommandHandler("scan", scan_hd.get());

    auto &keys_hd = hd_vec_.emplace_back(std::make_unique<KeysHandler>(this));
    AddCommandHandler("keys", keys_hd.get());

#ifdef VECTOR_INDEX_ENABLED
    auto &create_vec_index_hd =
        hd_vec_.emplace_back(std::make_unique<CreateVecIndexHandler>(this));
    AddCommandHandler("eloqvec.create", create_vec_index_hd.get());

    auto &info_vec_index_hd =
        hd_vec_.emplace_back(std::make_unique<InfoVecIndexHandler>(this));
    AddCommandHandler("eloqvec.info", info_vec_index_hd.get());

    auto &drop_vec_index_hd =
        hd_vec_.emplace_back(std::make_unique<DropVecIndexHandler>(this));
    AddCommandHandler("eloqvec.drop", drop_vec_index_hd.get());

    auto &add_vec_index_hd =
        hd_vec_.emplace_back(std::make_unique<AddVecIndexHandler>(this));
    AddCommandHandler("eloqvec.add", add_vec_index_hd.get());

    auto &badd_vec_index_hd =
        hd_vec_.emplace_back(std::make_unique<BAddVecIndexHandler>(this));
    AddCommandHandler("eloqvec.badd", badd_vec_index_hd.get());

    auto &update_vec_index_hd =
        hd_vec_.emplace_back(std::make_unique<UpdateVecIndexHandler>(this));
    AddCommandHandler("eloqvec.update", update_vec_index_hd.get());

    auto &delete_vec_index_hd =
        hd_vec_.emplace_back(std::make_unique<DeleteVecIndexHandler>(this));
    AddCommandHandler("eloqvec.delete", delete_vec_index_hd.get());

    auto &search_vec_index_hd =
        hd_vec_.emplace_back(std::make_unique<SearchVecIndexHandler>(this));
    AddCommandHandler("eloqvec.search", search_vec_index_hd.get());
#endif

    auto &dump_hd = hd_vec_.emplace_back(std::make_unique<DumpHandler>(this));
    AddCommandHandler("dump", dump_hd.get());

    auto &restore_hd =
        hd_vec_.emplace_back(std::make_unique<RestoreHandler>(this));
    AddCommandHandler("restore", restore_hd.get());

    auto &flushdb_hd =
        hd_vec_.emplace_back(std::make_unique<FlushDBCommandHandler>(this));
    AddCommandHandler("flushdb", flushdb_hd.get());

    auto &flushall_hd =
        hd_vec_.emplace_back(std::make_unique<FlushALLCommandHandler>(this));
    AddCommandHandler("flushall", flushall_hd.get());

    auto &subscribe_hd =
        hd_vec_.emplace_back(std::make_unique<SubscribeHandler>(this));
    AddCommandHandler("subscribe", subscribe_hd.get());

    auto &unsubscribe_hd =
        hd_vec_.emplace_back(std::make_unique<UnsubscribeHandler>(this));
    AddCommandHandler("unsubscribe", unsubscribe_hd.get());

    auto &psubscribe_hd =
        hd_vec_.emplace_back(std::make_unique<PSubscribeHandler>(this));
    AddCommandHandler("psubscribe", psubscribe_hd.get());

    auto &punsubscribe_hd =
        hd_vec_.emplace_back(std::make_unique<PUnsubscribeHandler>(this));
    AddCommandHandler("punsubscribe", punsubscribe_hd.get());

    auto &publish_hd =
        hd_vec_.emplace_back(std::make_unique<PublishHandler>(this));
    AddCommandHandler("publish", publish_hd.get());
#ifdef WITH_FAULT_INJECT
    auto &fault_inject_hd =
        hd_vec_.emplace_back(std::make_unique<FaultInjectHandler>(this));
    AddCommandHandler("fault_inject", fault_inject_hd.get());
#endif
    auto &mu_hd = hd_vec_.emplace_back(std::make_unique<MemoryHandler>(this));
    AddCommandHandler("memory", mu_hd.get());

    auto &expire_hd =
        hd_vec_.emplace_back(std::make_unique<ExpireCommandHandler>(this));
    AddCommandHandler("expire", expire_hd.get());

    auto &pexpire_hd =
        hd_vec_.emplace_back(std::make_unique<PExpireCommandHandler>(this));
    AddCommandHandler("pexpire", pexpire_hd.get());

    auto &expireat_hd =
        hd_vec_.emplace_back(std::make_unique<ExpireAtCommandHandler>(this));
    AddCommandHandler("expireat", expireat_hd.get());

    auto &pexpireat_hd =
        hd_vec_.emplace_back(std::make_unique<PExpireAtCommandHandler>(this));
    AddCommandHandler("pexpireat", pexpireat_hd.get());

    auto &ttl_hd =
        hd_vec_.emplace_back(std::make_unique<TTLCommandHandler>(this));
    AddCommandHandler("ttl", ttl_hd.get());

    auto &pttl_hd =
        hd_vec_.emplace_back(std::make_unique<PTTLCommandHandler>(this));
    AddCommandHandler("pttl", pttl_hd.get());

    auto &expire_time_hd =
        hd_vec_.emplace_back(std::make_unique<ExpireTimeCommandHandler>(this));
    AddCommandHandler("expiretime", expire_time_hd.get());

    auto &pexpire_time_hd =
        hd_vec_.emplace_back(std::make_unique<PExpireTimeCommandHandler>(this));
    AddCommandHandler("pexpiretime", pexpire_time_hd.get());

    auto &persist_hd =
        hd_vec_.emplace_back(std::make_unique<PersistCommandHandler>(this));
    AddCommandHandler("persist", persist_hd.get());

    auto &getex_hd =
        hd_vec_.emplace_back(std::make_unique<GetExCommandHandler>(this));
    AddCommandHandler("getex", getex_hd.get());

    auto &time_hd =
        hd_vec_.emplace_back(std::make_unique<TimeCommandHandler>(this));
    AddCommandHandler("time", time_hd.get());

    auto &slowlog_hd =
        hd_vec_.emplace_back(std::make_unique<SlowLogCommandHandler>(this));
    AddCommandHandler("slowlog", slowlog_hd.get());
}

TransactionExecution *RedisServiceImpl::NewTxm(IsolationLevel iso_level,
                                               CcProtocol protocol)
{
    TransactionExecution *txm;
#ifdef EXT_TX_PROC_ENABLED
    if (bthread::tls_task_group && bthread::tls_task_group->group_id_ >= 0)
    {
        txm = tx_service_->NewTx(bthread::tls_task_group->group_id_);
    }
    else
    {
        txm = tx_service_->NewTx();
    }
#else
    txm = tx_service_->NewTx();
#endif
    CODE_FAULT_INJECTOR("txm_iso_level_read_committed", {
        LOG(INFO) << "FaultInject txm_iso_level_read_committed"
                  << "txID: " << txm->TxNumber();
        iso_level = IsolationLevel::ReadCommitted;
    });

    txm->InitTx(iso_level, protocol);
    return txm;
}

template <class... Ts>
struct overload : Ts...
{
    using Ts::operator()...;
};
template <class... Ts>
overload(Ts...) -> overload<Ts...>;

TxErrorCode RedisServiceImpl::MultiExec(
    std::vector<std::variant<DirectRequest,
                             ObjectCommandTxRequest,
                             MultiObjectCommandTxRequest,
                             CustomCommandRequest>> &cmd_reqs,
    std::vector<std::vector<std::string>> &cmd_args,
    brpc::RedisReply *reply,
    TransactionExecution *txm,
    RedisConnectionContext *ctx)
{
    RedisReplier redis_reply(reply);
    if (cmd_reqs.empty())
    {
        redis_reply.OnNil();
        return TxErrorCode::NO_ERROR;
    }

    if (txm == nullptr)
    {
        // Init transaction state machine and execute the requests.
        txm = NewTxm(txn_isolation_level_, txn_protocol_);
    }

    std::vector<std::pair<std::variant<DirectRequest,
                                       ObjectCommandTxRequest,
                                       MultiObjectCommandTxRequest,
                                       CustomCommandRequest> *,
                          std::vector<std::string> *>>
        cmd_req_ptrs;

    assert(cmd_reqs.size() == cmd_args.size());
    for (size_t i = 0; i < cmd_reqs.size(); ++i)
    {
        cmd_req_ptrs.emplace_back(&cmd_reqs[i], &cmd_args[i]);
    }

    // Use insert sort to sort the TxRequest by keys.
    // to reduce the deadlock.
    // For example, if there are two transactions.
    // tx A:
    // MULTI
    // SET foo foo
    // SET bar bar
    // EXEC
    // tx B:
    // MULTI
    // set bar bar
    // set foo foo
    // EXEC
    // If each tx run the first command, and wait for the second,
    // deadlock occurs.
    // Sorting the commands by keys can avoid situations above.
    // However, mset a a b b and mset b b a a can still lead to
    // deadlocks.
    if (FLAGS_enable_cmd_sort)
    {
        std::stable_sort(
            cmd_req_ptrs.begin(),
            cmd_req_ptrs.end(),
            [](std::pair<std::variant<DirectRequest,
                                      ObjectCommandTxRequest,
                                      MultiObjectCommandTxRequest,
                                      CustomCommandRequest> *,
                         std::vector<std::string> *> p1,
               std::pair<std::variant<DirectRequest,
                                      ObjectCommandTxRequest,
                                      MultiObjectCommandTxRequest,
                                      CustomCommandRequest> *,
                         std::vector<std::string> *> p2)
            {
                DirectRequest *dt1 = std::get_if<DirectRequest>(p1.first);
                DirectRequest *dt2 = std::get_if<DirectRequest>(p2.first);
                if (dt1 != nullptr || dt2 != nullptr)
                {
                    return false;
                }
                CustomCommandRequest *mst1 =
                    std::get_if<CustomCommandRequest>(p1.first);
                CustomCommandRequest *mst2 =
                    std::get_if<CustomCommandRequest>(p2.first);
                if (mst1 != nullptr || mst2 != nullptr)
                {
                    return false;
                }
                ObjectCommandTxRequest *ot1 =
                    std::get_if<ObjectCommandTxRequest>(p1.first);
                ObjectCommandTxRequest *ot2 =
                    std::get_if<ObjectCommandTxRequest>(p2.first);
                MultiObjectCommandTxRequest *mt1 =
                    std::get_if<MultiObjectCommandTxRequest>(p1.first);
                MultiObjectCommandTxRequest *mt2 =
                    std::get_if<MultiObjectCommandTxRequest>(p2.first);
                if (ot1 != nullptr)
                {
                    return ot2 != nullptr ? *ot1 < *ot2 : *ot1 < *mt2;
                }
                else
                {
                    assert(mt1 != nullptr);
                    return ot2 != nullptr ? *mt1 < *ot2 : *mt1 < *mt2;
                }
            });
    }

    // stop execution and return nil reply if error
    TxErrorCode tx_err_code = TxErrorCode::NO_ERROR;
    for (auto &tx_req_pair : cmd_req_ptrs)
    {
        if (tx_err_code != TxErrorCode::NO_ERROR)
        {
            break;
        }
        auto tx_req_ptr = tx_req_pair.first;
        auto cmd_args_ptr = tx_req_pair.second;
        const std::string_view cmd_type_str(cmd_args_ptr->front().data(),
                                            cmd_args_ptr->front().size());
        const RedisCommandType cmd_type = CommandType(cmd_type_str);

        bool is_collecting_duration_round = false;
        if (metrics::enable_metrics)
        {
            is_collecting_duration_round =
                CheckAndUpdateRedisCmdRound(cmd_type);
        }
        metrics::TimePoint start, end;
        uint64_t duration = 0;

        int slow_log_threshold =
            slow_log_threshold_.load(std::memory_order_relaxed);
        uint32_t slow_log_max_length =
            slow_log_max_length_.load(std::memory_order_relaxed);

        // Collect duration if metrics is enabled or if we need to record slow
        // query.
        bool collect_duration =
            ((metrics::enable_metrics && is_collecting_duration_round) ||
             (slow_log_threshold >= 0 && slow_log_max_length > 0)) &&
            cmd_type != RedisCommandType::UNKNOWN;
        if (collect_duration)
        {
            start = metrics::Clock::now();
        }

        std::visit(
            overload{[this](DirectRequest &req) { req.Execute(this); },
                     [this, txm, &tx_err_code](ObjectCommandTxRequest &req)
                     {
                         req.TrySetTxm(txm);
                         bool success = this->ExecuteTxRequest(
                             txm, &req, nullptr, nullptr);
                         if (!success)
                         {
                             tx_err_code = req.ErrorCode();
                         }
                     },
                     [this, txm, &tx_err_code](MultiObjectCommandTxRequest &req)
                     {
                         req.TrySetTxm(txm);
                         bool success = this->ExecuteMultiObjTxRequest(
                             txm, &req, nullptr, nullptr);
                         if (!success)
                         {
                             tx_err_code = req.ErrorCode();
                         }
                     },
                     [this, ctx, txm, &tx_err_code](CustomCommandRequest &req)
                     {
                         bool success =
                             req.Execute(this, ctx, txm, nullptr, false);
                         if (!success)
                         {
                             tx_err_code = TxErrorCode::UNDEFINED_ERR;
                         }
                     }},
            *tx_req_ptr);

        if (collect_duration && tx_err_code == TxErrorCode::NO_ERROR)
        {
            end = metrics::Clock::now();
            duration = std::chrono::duration_cast<std::chrono::microseconds>(
                           end - start)
                           .count();

            if (slow_log_threshold == 0 ||
                (slow_log_threshold > 0 &&
                 duration > static_cast<uint64_t>(slow_log_threshold)))
            {
                uint32_t group_id = bthread::tls_task_group->group_id_;
                std::lock_guard<bthread::Mutex> slow_log_lk(
                    *slow_log_mutexes_[group_id]);
                if (!slow_log_[group_id].empty())
                {
                    uint32_t next_idx = next_slow_log_idx_[group_id]++;
                    if (next_slow_log_idx_[group_id] >=
                        slow_log_[group_id].size())
                    {
                        next_slow_log_idx_[group_id] = 0;
                    }

                    if (slow_log_len_[group_id] < slow_log_[group_id].size())
                    {
                        slow_log_len_[group_id]++;
                    }

                    slow_log_[group_id][next_idx].id_ =
                        next_slow_log_unique_id_[group_id]++;
                    slow_log_[group_id][next_idx].execution_time_ = duration;
                    slow_log_[group_id][next_idx].timestamp_ =
                        end.time_since_epoch().count();
                    slow_log_[group_id][next_idx].cmd_.clear();
                    for (auto &arg : *cmd_args_ptr)
                    {
                        if (slow_log_[group_id][next_idx].cmd_.size() == 31 &&
                            cmd_args_ptr->size() > 32)
                        {
                            // slow logs are truncated to 32 args
                            std::string truncated_cmd =
                                "... (" +
                                std::to_string(cmd_args_ptr->size() - 31) +
                                " more arguments)";
                            slow_log_[group_id][next_idx].cmd_.push_back(
                                truncated_cmd);
                            break;
                        }

                        if (arg.size() > 128)
                        {
                            // arg longer than 128 chars are truncated
                            std::string truncated_arg =
                                arg.substr(0, 128) + "... (" +
                                std::to_string(arg.size() - 128) +
                                " more bytes)";
                            slow_log_[group_id][next_idx].cmd_.push_back(
                                truncated_arg);
                        }
                        else
                        {
                            slow_log_[group_id][next_idx].cmd_.push_back(arg);
                        }
                    }
                    if (ctx->socket != nullptr)
                    {
                        slow_log_[group_id][next_idx].client_addr_ =
                            ctx->socket->remote_side();
                    }
                    else
                    {
                        slow_log_[group_id][next_idx].client_addr_.reset();
                    }

                    slow_log_[group_id][next_idx].client_name_ =
                        ctx->connection_name;
                }
            }
        }

        if (metrics::enable_metrics && cmd_type != RedisCommandType::UNKNOWN &&
            tx_err_code == txservice::TxErrorCode::NO_ERROR)
        {
            auto access_type = GetCommandAccessType(cmd_type_str);
            auto core_id = bthread::tls_task_group->group_id_;
            auto meter = GetMeter(core_id);
            if (is_collecting_duration_round && collect_duration)
            {
                assert(collect_duration);
                meter->Collect(metrics::NAME_REDIS_COMMAND_DURATION,
                               duration,
                               cmd_type_str);
                if (access_type == "read" || access_type == "write")
                {
                    meter->Collect(
                        metrics::NAME_REDIS_COMMAND_AGGREGATED_DURATION,
                        duration,
                        access_type);
                }
            }
            meter->Collect(metrics::NAME_REDIS_COMMAND_TOTAL, 1, cmd_type_str);
            if (access_type == "read" || access_type == "write")
            {
                meter->Collect(metrics::NAME_REDIS_COMMAND_AGGREGATED_TOTAL,
                               1,
                               access_type);
            }
        }
    }

    auto txn = txm->TxNumber();
    if (tx_err_code != txservice::TxErrorCode::NO_ERROR)
    {
        LOG(WARNING) << "txn: " << txn
                     << " Error occurs in MultiExec, abort txn, "
                     << TxErrorMessage(tx_err_code);
        // abort txn
        AbortTx(txm);
        // set nil
        redis_reply.OnNil();
        return tx_err_code;
    }

    // Commit and wait.
    auto [success, err_code] = CommitTx(txm);
    if (!success)
    {
        LOG(WARNING) << "txn: " << txn
                     << " MultiExec commit error: " << TxErrorMessage(err_code);
        redis_reply.OnNil();
        return err_code;
    }

    // Output the commands' result.
    redis_reply.OnArrayStart(cmd_reqs.size());
    for (auto &tx_req : cmd_reqs)
    {
        std::visit(
            overload{[&redis_reply](DirectRequest &req)
                     { req.OutputResult(&redis_reply); },
                     [&redis_reply](ObjectCommandTxRequest &req)
                     {
                         const auto &command =
                             static_cast<const RedisCommand &>(*req.Command());
                         command.OutputResult(&redis_reply);
                     },
                     [&redis_reply](MultiObjectCommandTxRequest &req)
                     {
                         const auto &command =
                             static_cast<const RedisMultiObjectCommand &>(
                                 *req.Command());
                         command.OutputResult(&redis_reply);
                     },
                     [&redis_reply, ctx](CustomCommandRequest &req)
                     { req.cmd_->OutputResult(&redis_reply, ctx); }},
            tx_req);
    }
    redis_reply.OnArrayEnd();

    return TxErrorCode::NO_ERROR;
}

bool RedisServiceImpl::ScriptFlush()
{
    std::unique_lock<std::shared_mutex> lock(script_mutex_);
    scripts_.clear();
    return true;
}

bool RedisServiceImpl::ScriptExists(const std::vector<butil::StringPiece> &args,
                                    brpc::RedisReply *output)
{
    std::shared_lock<std::shared_mutex> lock(script_mutex_);
    output->SetArray(args.size() - 2);
    for (size_t i = 2; i < args.size(); ++i)
    {
        if (scripts_.find({args[i].data(), args[i].size()}) == scripts_.end())
        {
            (*output)[i - 2].SetInteger(0);
        }
        else
        {
            (*output)[i - 2].SetInteger(1);
        }
    }
    return true;
}

bool RedisServiceImpl::ScriptLoad(const std::vector<butil::StringPiece> &args,
                                  brpc::RedisReply *output)
{
    assert(args[0] == "script");

    std::unique_ptr<LuaInterpreter> interpreter = GetLuaInterpreter();
    std::string_view script_body = {args[2].data(), args[2].size()};

    auto [success, result] = interpreter->CreateFunction(script_body);
    if (!success)
    {
        // result is the error message
        output->SetError("ERR Error compiling script (new function): " +
                         result);
        CleanAndReturnLuaInterpreter(std::move(interpreter));
        return false;
    }

    // result is the sha of script_body
    std::string &sha = result;
    std::unique_lock<std::shared_mutex> lock(script_mutex_);
    scripts_[sha] = script_body;
    lock.unlock();

    output->SetString(sha);
    CleanAndReturnLuaInterpreter(std::move(interpreter));

    return true;
}

bool RedisServiceImpl::Evalsha(const RedisConnectionContext *ctx,
                               const std::vector<butil::StringPiece> &args,
                               brpc::RedisReply *output)
{
    assert(args[0] == "evalsha");

    if (Sharder::Instance().CheckShutdownStatus())
    {
        output->SetError(
            redis_get_error_messages(RD_ERR_ClUSTER_IS_SHUTTING_DOWN));
        return false;
    }

    std::string hash = {args[1].data(), args[1].size()};
    std::transform(hash.begin(),
                   hash.end(),
                   hash.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    std::shared_lock<std::shared_mutex> lock(script_mutex_);
    const auto iter = scripts_.find(hash);
    if (iter == scripts_.end())
    {
        output->SetError("NOSCRIPT No matching script. Please use EVAL.");
        return false;
    }
    std::vector<butil::StringPiece> eval_args = args;
    std::string script_body = iter->second;
    eval_args[1] = script_body;
    lock.unlock();

    return EvalLua(ctx, eval_args, output);
}

bool RedisServiceImpl::EvalLua(const RedisConnectionContext *ctx,
                               const std::vector<butil::StringPiece> &args,
                               brpc::RedisReply *output)
{
    // the script content stores in args[1]
    // 1. parse script and keys and args
    assert(args[0] == "eval" || args[0] == "evalsha");
    //
    //    LOG(INFO) << "received eval command: ";
    //    for (auto sp : args)
    //    {
    //        LOG(INFO) << sp;
    //    }
    //    LOG(INFO) << "";

    std::string_view script_body;
    int32_t num_keys = 0;
    std::vector<std::string_view> script_keys;
    std::vector<std::string_view> script_args;

    // get lua script body
    script_body = {args[1].data(), args[1].size()};

    if (args.size() >= 3)
    {
        // get keys
        const butil::StringPiece num_keys_arg = args[2];
        auto [ptr, ec] =
            std::from_chars(num_keys_arg.data(),
                            num_keys_arg.data() + num_keys_arg.size(),
                            num_keys);
        if (ec != std::errc{} ||
            ptr != num_keys_arg.data() + num_keys_arg.size())
        {
            output->SetError(
                "ERR Number of keys is not an integer or out of range");
            return false;
        }
        else if (num_keys < 0)
        {
            output->SetError("ERR Number of keys can't be negative");
            return false;
        }
        else if (static_cast<size_t>(num_keys) > args.size() - 3)
        {
            output->SetError(
                "ERR Number of keys can't be greater than number of args");
            return false;
        }
        script_keys.reserve(num_keys);
        size_t arg_idx = 3;
        for (int32_t i = 0; i < num_keys; i++)
        {
            script_keys.emplace_back(args[arg_idx].data(),
                                     args[arg_idx].size());
            arg_idx++;
        }

        // get script args
        for (; arg_idx < args.size(); arg_idx++)
        {
            script_args.emplace_back(args[arg_idx].data(),
                                     args[arg_idx].size());
        }
    }

    while (true)
    {
        TransactionExecution *txm = NewTxm(txn_isolation_level_, txn_protocol_);

        // get lua lua_state
        std::unique_ptr<LuaInterpreter> interpreter = GetLuaInterpreter();

        // Populate the argv and keys table accordingly to the arguments that
        // EVAL received.
        interpreter->SetGlobalArray("KEYS", script_keys);
        interpreter->SetGlobalArray("ARGV", script_args);

        interpreter->SetConnectionContext(*ctx);

        // set the function to call Redis command into lua interpreter
        RedisServiceImpl *redis_service = this;
        interpreter->SetScriptRedisHook(
            [txm, redis_service](RedisConnectionContext *ctx,
                                 const std::vector<std::string> &args,
                                 OutputHandler *reply)
            {
                assert(txm != nullptr);
                redis_service->GenericCommand(ctx, txm, args, reply);
            });

        // execute script body

        auto [success, result] = interpreter->CreateFunction(script_body);
        if (!success)
        {
            const std::string &error_msg = result;
            output->SetError("ERR Error compiling script (new function): " +
                             error_msg);
            AbortTx(txm);
            CleanAndReturnLuaInterpreter(std::move(interpreter));
            return false;
        }

        const std::string &sha = result;
        if (args[0] != "evalsha")
        {
            std::unique_lock<std::shared_mutex> lock(script_mutex_);
            scripts_[sha] = args[1].as_string();
            lock.unlock();
        }
        std::string error;
        bool ok = interpreter->CallFunction(sha, &error);
        if (!ok)
        {
            // abort tx
            AbortTx(txm);
            CleanAndReturnLuaInterpreter(std::move(interpreter));
            if ((error.find("ERR OCC break repeatable read isolation level.") !=
                     std::string::npos ||
                 error.find(
                     "ERR Transaction failed due to write-write conflicts.") !=
                     std::string::npos) &&
                retry_on_occ_error_)
            {
                continue;
            }
            LOG(WARNING) << "EvalLua error: " << error;
            output->SetError(error);

            return false;
        }

        if (txm != nullptr)
        {
            // commit tx
            auto [success, err_code] = CommitTx(txm);
            if (!success)
            {
                CleanAndReturnLuaInterpreter(std::move(interpreter));
                if ((err_code == TxErrorCode::OCC_BREAK_REPEATABLE_READ ||
                     err_code == TxErrorCode::WRITE_WRITE_CONFLICT) &&
                    retry_on_occ_error_)
                {
                    continue;
                }
                const std::string &err_msg = TxErrorMessage(err_code);
                LOG(WARNING) << " EvalLua commit error: " << err_msg;
                output->SetError(err_msg);
                return false;
            }
            // txm might be recycled after commit, so it is not safe to use it
            // anymore. Reset it back to null so that it cannot be accessed in
            // SerializeResult.
            txm = nullptr;
        }

        interpreter->LuaReplyToRedisReply(output);

        CleanAndReturnLuaInterpreter(std::move(interpreter));
        break;
    }
    return true;
}

bool RedisServiceImpl::ExecuteCommand(RedisConnectionContext *ctx,
                                      DirectCommand *cmd,
                                      OutputHandler *output)
{
    cmd->Execute(this, ctx);
    cmd->OutputResult(output);
    return true;
}

bool RedisServiceImpl::ExecuteCommand(RedisConnectionContext *ctx,
                                      txservice::TransactionExecution *txm,
                                      const EloqKey &key,
                                      RedisCommand *cmd,
                                      OutputHandler *output,
                                      bool auto_commit,
                                      bool always_redirect)
{
    if (key.Length() > MAX_KEY_SIZE)
    {
        if (output != nullptr)
        {
            output->OnError(redis_get_error_messages(RD_ERR_KEY_TOO_BIG));
        }
        if (auto_commit)
        {
            AbortTx(txm);
        }
        return false;
    }
    if (FLAGS_cc_notify && (!auto_commit || skip_wal_) && ctx->txm == nullptr)
    {
        bthread::TaskGroup *resume_group = bthread::tls_task_group;
        bthread_t resume_tid = resume_group->current_tid();

        std::function<void()> yield_func = []()
        {
            // Block current bthread when wait for the result.
            bthread_block();
        };
        std::function<void()> resume_func = [resume_group, resume_tid]()
        {
            // Resume this bthread when the result returns.
            resume_group->resume_bound_task(resume_tid);
        };

        ObjectCommandTxRequest tx_req(RedisTableName(ctx->db_id),
                                      &key,
                                      cmd,
                                      auto_commit,
                                      always_redirect,
                                      txm,
                                      &yield_func,
                                      &resume_func);

        return ExecuteTxRequest(txm, &tx_req, output, output);
    }
    else
    {
        ObjectCommandTxRequest tx_req(RedisTableName(ctx->db_id),
                                      &key,
                                      cmd,
                                      auto_commit,
                                      always_redirect,
                                      txm);
        return ExecuteTxRequest(txm, &tx_req, output, output);
    }
}

bool RedisServiceImpl::ExecuteCommand(RedisConnectionContext *ctx,
                                      txservice::TransactionExecution *txm,
                                      RedisMultiObjectCommand *cmd,
                                      OutputHandler *output,
                                      bool auto_commit,
                                      bool always_redirect)
{
    MultiObjectCommandTxRequest tx_req(
        RedisTableName(ctx->db_id), cmd, auto_commit, always_redirect, txm);

    return ExecuteMultiObjTxRequest(txm, &tx_req, output, output);
}

bool RedisServiceImpl::ExecuteCommand(RedisConnectionContext *ctx,
                                      txservice::TransactionExecution *txm,
                                      const TableName *table,
                                      const EloqKey &key,
                                      ZScanCommand *cmd,
                                      OutputHandler *output,
                                      bool auto_commit)
{
    ObjectCommandTxRequest tx_req(table, &key, cmd, auto_commit, true, txm);

    auto res = ExecuteTxRequest(txm, &tx_req, nullptr, output);
    if (res && output != nullptr)
    {
        cmd->OutputResult(output, ctx);
    }
    return res;
}

std::string GetCurrentTimeAsString()
{
    auto now = std::chrono::system_clock::now();
    std::time_t now_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_time = *std::localtime(&now_time_t);
    std::ostringstream oss;
    oss << std::put_time(&tm_time, "_%Y_%m_%d_%H_%M_%S");
    return oss.str();
}

bool RedisServiceImpl::ExecuteFlushDBCommand(
    RedisConnectionContext *ctx,
    txservice::TransactionExecution *txm,
    EloqKV::OutputHandler *output,
    bool auto_commit)
{
    const TableName *redis_table_name = RedisTableName(ctx->db_id);

    // load table if not exists since drop table
    // does not handle nonexistent table.
    CatalogKey catalog_key(*redis_table_name);
    TxKey cat_tx_key(&catalog_key);
    CatalogRecord catalog_rec;
    ReadTxRequest read_req(&txservice::catalog_ccm_name,
                           0,
                           &cat_tx_key,
                           &catalog_rec,
                           true,
                           false,
                           true,
                           0,
                           false,
                           false,
                           false,
                           nullptr,
                           nullptr,
                           txm);
    txm->Execute(&read_req);
    read_req.Wait();
    if (read_req.IsError())
    {
        if (read_req.ErrorCode() == TxErrorCode::DATA_NOT_ON_LOCAL_NODE)
        {
            // This ng is in standby mode. Return a ReadOnly error to the client
            // so that it can be redirected to the primary node.
            output->OnError(
                "READONLY You can't write against a read only replica.");
        }
        else
        {
            output->OnError(read_req.ErrorMsg());
        }
        if (auto_commit)
        {
            AbortTx(txm);
        }
        return false;
    }

    // The schema image only contains kv_table_name.
    std::string new_image = redis_table_name->String();
    new_image.append(GetCurrentTimeAsString());
    new_image.append("_" + std::to_string(txm->TxNumber()));

#if defined(DATA_STORE_TYPE_CASSANDRA)
    EloqDS::CassCatalogInfo temp_kv_info(new_image, "");
    auto kv_info_str = temp_kv_info.Serialize();
    new_image = EloqDS::SerializeSchemaImage("", kv_info_str, "");
#elif defined(DATA_STORE_TYPE_DYNAMODB)
    // TODO(lokax):
#elif defined(DATA_STORE_TYPE_ROCKSDB)
    // TODO(lokax):
#endif

    UpsertTableTxRequest delete_table_tx_req(
        redis_table_name,
        &catalog_rec.Schema()->SchemaImage(),
        catalog_rec.Schema()->Version(),
        &new_image,
        OperationType::TruncateTable,
        nullptr,
        nullptr,
        nullptr,
        txm);
    bool succeed =
        ExecuteUpsertTableTxRequest(txm, &delete_table_tx_req, output, true);
    if (auto_commit)
    {
        if (succeed)
        {
            CommitTx(txm);
        }
        else
        {
            AbortTx(txm);
        }
    }
    return succeed;
}

bool RedisServiceImpl::ExecuteFlushALLCommand(RedisConnectionContext *ctx,
                                              EloqKV::OutputHandler *output,
                                              bool auto_commit,
                                              IsolationLevel iso_level_,
                                              CcProtocol cc_protocol_)
{
    assert(auto_commit);

    std::vector<TransactionExecution *> txm_pool;
    txm_pool.reserve(redis_table_names_.size());

    bool succeed = false;
    for (const auto &redis_table_name : redis_table_names_)
    {
        TransactionExecution *txm = NewTxm(iso_level_, cc_protocol_);
        txm_pool.emplace_back(txm);

        // load table if not exists since drop table
        // does not handle nonexistent table.
        CatalogKey catalog_key(redis_table_name);
        TxKey cat_tx_key(&catalog_key);
        CatalogRecord catalog_rec;
        ReadTxRequest read_req(&txservice::catalog_ccm_name,
                               0,
                               &cat_tx_key,
                               &catalog_rec,
                               true,
                               false,
                               true,
                               0,
                               false,
                               false,
                               false,
                               nullptr,
                               nullptr,
                               txm);
        txm->Execute(&read_req);
        read_req.Wait();
        if (read_req.IsError())
        {
            if (read_req.ErrorCode() == TxErrorCode::DATA_NOT_ON_LOCAL_NODE)
            {
                // This ng is in standby mode. Return a ReadOnly error to the
                // client so that it can be redirected to the primary node.
                output->OnError(
                    "READONLY You can't write against a read only replica.");
            }
            else
            {
                output->OnError(read_req.ErrorMsg());
            }
            AbortTx(txm);

            return false;
        }

        // The schema image only contains kv_table_name.
        std::string new_image = redis_table_name.String();
        new_image.append(GetCurrentTimeAsString());
        new_image.append("_" + std::to_string(txm->TxNumber()));

#if defined(DATA_STORE_TYPE_CASSANDRA)
        EloqDS::CassCatalogInfo temp_kv_info(new_image, "");
        auto kv_info_str = temp_kv_info.Serialize();
        new_image = EloqDS::SerializeSchemaImage("", kv_info_str, "");
#elif defined(DATA_STORE_TYPE_DYNAMODB)
        // TODO(lokax):
#elif defined(DATA_STORE_TYPE_ROCKSDB)
        // TODO(lokax):
#endif

        UpsertTableTxRequest truncate_table_tx_req(
            &redis_table_name,
            &catalog_rec.Schema()->SchemaImage(),
            catalog_rec.Schema()->Version(),
            &new_image,
            OperationType::TruncateTable,
            nullptr,
            nullptr,
            nullptr,
            txm);
        succeed = ExecuteUpsertTableTxRequest(
            txm, &truncate_table_tx_req, output, true);

        if (!succeed)
        {
            LOG(INFO) << "Failed to flushall on table: "
                      << redis_table_name.StringView();
            break;
        }
    }

    if (succeed)
    {
        for (TransactionExecution *txm : txm_pool)
        {
            CommitTx(txm);
        }
    }
    else
    {
        for (TransactionExecution *txm : txm_pool)
        {
            AbortTx(txm);
        }
    }

    return succeed;
}

void RedisServiceImpl::GenericCommand(RedisConnectionContext *ctx,
                                      TransactionExecution *txm,
                                      const std::vector<std::string> &cmd_args,
                                      EloqKV::OutputHandler *output)
{
    std::vector<std::string_view> cmd_arg_list;
    for (const auto &str : cmd_args)
    {
        cmd_arg_list.emplace_back(str);
    }
    // parse command and args, generate TxRequest
    assert(!cmd_arg_list.empty());
    assert(txm != nullptr);
    const RedisCommandType cmd_type = CommandType(cmd_arg_list[0]);
    std::string err_msg;

    bool is_collecting_duration_round = false;
    if (metrics::enable_metrics)
    {
        is_collecting_duration_round = CheckAndUpdateRedisCmdRound(cmd_type);
    }

    int slow_log_threshold =
        slow_log_threshold_.load(std::memory_order_relaxed);
    uint32_t slow_log_max_length =
        slow_log_max_length_.load(std::memory_order_relaxed);

    uint64_t duration = 0;
    // Collect duration if metrics is enabled or if we need to record slow
    // query.
    bool collect_duration =
        ((metrics::enable_metrics && is_collecting_duration_round) ||
         (slow_log_threshold >= 0 && slow_log_max_length > 0)) &&
        cmd_type != RedisCommandType::UNKNOWN;

    metrics::TimePoint start, end;
    if (collect_duration)
    {
        start = metrics::Clock::now();
    }

    switch (cmd_type)
    {
    case RedisCommandType::ECHO:
    {
        auto [success, cmd] = ParseEchoCommand(cmd_arg_list, output);
        if (success)
        {
            ExecuteCommand(ctx, &cmd, output);
        }
        break;
    }
    case RedisCommandType::PING:
    {
        auto [success, cmd] = ParsePingCommand(cmd_arg_list, output);
        if (success)
        {
            ExecuteCommand(ctx, &cmd, output);
        }
        break;
    }
    case RedisCommandType::SELECT:
    {
        auto [success, cmd] = ParseSelectCommand(cmd_arg_list, output);
        if (success)
        {
            ExecuteCommand(ctx, &cmd, output);
        }
        break;
    }
    case RedisCommandType::CONFIG:
    {
        auto [success, cmd] = ParseConfigCommand(cmd_arg_list, output);
        if (success)
        {
            ExecuteCommand(ctx, &cmd, output);
        }
        break;
    }
    case RedisCommandType::DBSIZE:
    {
        auto [success, cmd] = ParseDBSizeCommand(cmd_arg_list, output);
        if (success)
        {
            ExecuteCommand(ctx, &cmd, output);
        }
        break;
    }
    case RedisCommandType::PUBLISH:
    {
        auto [success, cmd] = ParsePublishCommand(cmd_arg_list, output);
        if (success)
        {
            ExecuteCommand(ctx, &cmd, output);
        }
        break;
    }
    case RedisCommandType::READONLY:
    {
        auto [success, cmd] = ParseReadOnlyCommand(cmd_arg_list, output);
        if (success)
        {
            ExecuteCommand(ctx, &cmd, output);
        }
        break;
    }
    case RedisCommandType::INFO:
    {
        auto [success, cmd] = ParseInfoCommand(cmd_arg_list, output);
        if (success)
        {
            ExecuteCommand(ctx, &cmd, output);
        }
        break;
    }
    case RedisCommandType::CLUSTER:
    {
        auto [success, cmd] = ParseClusterCommand(cmd_arg_list, output);
        if (success)
        {
            ExecuteCommand(ctx, cmd.get(), output);
        }
        break;
    }
    case RedisCommandType::FAILOVER:
    {
        auto [success, cmd] = ParseFailoverCommand(cmd_arg_list, output);
        if (success)
        {
            ExecuteCommand(ctx, cmd.get(), output);
        }
        break;
    }
    case RedisCommandType::COMMAND:
    {
        auto [success, cmd] = ParseCommandCommand(cmd_arg_list, output);
        if (success)
        {
            ExecuteCommand(ctx, cmd.get(), output);
        }
        break;
    }
    case RedisCommandType::GET:
    {
        auto [success, key, cmd] = ParseGetCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::GETDEL:
    {
        auto [success, key, cmd] = ParseGetDelCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::SET:
    case RedisCommandType::SETNX:
    case RedisCommandType::GETSET:
    case RedisCommandType::SETEX:
    case RedisCommandType::PSETEX:
    {
        auto [success, key, cmd] = ParseSetCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::STRLEN:
    {
        auto [success, key, cmd] = ParseStrLenCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::GETBIT:
    {
        auto [success, key, cmd] = ParseGetBitCommand(cmd_arg_list, output);
        if (success)
        {
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::GETRANGE:
    {
        auto [success, key, cmd] = ParseGetRangeCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::SETBIT:
    {
        auto [success, key, cmd] = ParseSetBitCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::APPEND:
    {
        auto [success, key, cmd] = ParseAppendCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::SETRANGE:
    {
        auto [success, key, cmd] = ParseSetRangeCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::INCRBYFLOAT:
    {
        auto [success, key, cmd] =
            ParseIncrByFloatCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::BITCOUNT:
    {
        auto [success, key, cmd] = ParseBitCountCommand(cmd_arg_list, output);

        if (success)
        {
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::LRANGE:
    {
        auto [success, key, cmd] = ParseLRangeCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::RPUSH:
    {
        auto [success, key, cmd] = ParseRPushCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::HSET:
    {
        auto [success, key, cmd] = ParseHSetCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::HSETNX:
    {
        auto [success, key, cmd] = ParseHSetNxCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::HGET:
    {
        auto [success, key, cmd] = ParseHGetCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::HLEN:
    {
        auto [success, key, cmd] = ParseHLenCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::HSTRLEN:
    {
        auto [success, key, cmd] = ParseHStrLenCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::HINCRBY:
    {
        auto [success, key, cmd] = ParseHIncrByCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::HINCRBYFLOAT:
    {
        auto [success, key, cmd] =
            ParseHIncrByFloatCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::HMGET:
    {
        auto [success, key, cmd] = ParseHMGetCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::HKEYS:
    {
        auto [success, key, cmd] = ParseHKeysCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::HVALS:
    {
        auto [success, key, cmd] = ParseHValsCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::HGETALL:
    {
        auto [success, key, cmd] = ParseHGetAllCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::HEXISTS:
    {
        auto [success, key, cmd] = ParseHExistsCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::HDEL:
    {
        auto [success, key, cmd] = ParseHDelCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::HRANDFIELD:
    {
        auto [success, key, cmd] = ParseHRandFieldCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::HSCAN:
    {
        auto [success, key, cmd] = ParseHScanCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::LPUSH:
    {
        auto [success, key, cmd] = ParseLPushCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::LPOP:
    {
        auto [success, key, cmd] = ParseLPopCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::LINDEX:
    {
        auto [success, key, cmd] = ParseLIndexCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::LINSERT:
    {
        auto [success, key, cmd] = ParseLInsertCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::LLEN:
    {
        auto [success, key, cmd] = ParseLLenCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::LTRIM:
    {
        auto [success, key, cmd] = ParseLTrimCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::LPOS:
    {
        auto [success, key, cmd] = ParseLPosCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::LSET:
    {
        auto [success, key, cmd] = ParseLSetCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::LMOVE:
    {
        auto [success, cmd] = ParseLMoveCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::RPOPLPUSH:
    {
        auto [success, cmd] = ParseRPopLPushCommand(cmd_arg_list, output);
        if (success)
        {
            ExecuteCommand(ctx, txm, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::LREM:
    {
        auto [success, key, cmd] = ParseLRemCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::LPUSHX:
    {
        auto [success, key, cmd] = ParseLPushXCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::RPUSHX:
    {
        auto [success, key, cmd] = ParseRPushXCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::ZADD:
    {
        auto [success, key, cmd] = ParseZAddCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::ZCOUNT:
    {
        auto [success, key, cmd] = ParseZCountCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::ZCARD:
    {
        auto [success, key, cmd] = ParseZCardCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::ZRANDMEMBER:
    {
        auto [success, key, cmd] =
            ParseZRandMemberCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::ZRANGEBYLEX:
    {
        auto [success, key, cmd] =
            ParseZRangeByLexCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::ZRANGEBYRANK:
    {
        auto [success, key, cmd] =
            ParseZRangeByRankCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::ZRANGEBYSCORE:
    {
        auto [success, key, cmd] =
            ParseZRangeByScoreCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::ZRANK:
    {
        auto [success, key, cmd] = ParseZRankCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::ZREM:
    {
        auto [success, key, cmd] = ParseZRemCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::ZREVRANGEBYLEX:
    {
        auto [success, key, cmd] =
            ParseZRevRangeByLexCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::ZREVRANGEBYSCORE:
    {
        auto [success, key, cmd] =
            ParseZRevRangeByScoreCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::ZREVRANGE:
    {
        auto [success, key, cmd] = ParseZRevRangeCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::ZREVRANK:
    {
        auto [success, key, cmd] = ParseZRevRankCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::ZSCORE:
    {
        auto [success, key, cmd] = ParseZScoreCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::ZSCAN:
    {
        auto [success, key, cmd] = ParseZScanCommand(ctx, cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(
                ctx, txm, RedisTableName(ctx->db_id), key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::RPOP:
    {
        auto [success, key, cmd] = ParseRPopCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::INCR:
    {
        auto [success, key, cmd] = ParseIncrCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::DECR:
    {
        auto [success, key, cmd] = ParseDecrCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::INCRBY:
    {
        auto [success, key, cmd] = ParseIncrByCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::DECRBY:
    {
        auto [success, key, cmd] = ParseDecrByCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::TYPE:
    {
        auto [success, key, cmd] = ParseTypeCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::DEL:
    {
        auto [success, cmd] = ParseDelCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::EXISTS:
    {
        auto [success, cmd] = ParseExistsCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::EXPIRE:
    {
        auto [success, key, cmd] =
            ParseExpireCommand(cmd_arg_list, output, false, false);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::PEXPIRE:
    {
        auto [success, key, cmd] =
            ParseExpireCommand(cmd_arg_list, output, true, false);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::EXPIREAT:
    {
        auto [success, key, cmd] =
            ParseExpireCommand(cmd_arg_list, output, false, true);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::PEXPIREAT:
    {
        auto [success, key, cmd] =
            ParseExpireCommand(cmd_arg_list, output, true, true);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::TTL:
    {
        auto [success, key, cmd] =
            ParseTTLCommand(cmd_arg_list, output, false, false, false);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::PTTL:
    {
        auto [success, key, cmd] =
            ParseTTLCommand(cmd_arg_list, output, true, false, false);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::EXPIRETIME:
    {
        auto [success, key, cmd] =
            ParseTTLCommand(cmd_arg_list, output, false, true, false);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::PEXPIRETIME:
    {
        auto [success, key, cmd] =
            ParseTTLCommand(cmd_arg_list, output, false, false, true);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::PERSIST:
    {
        auto [success, key, cmd] = ParsePersistCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::GETEX:
    {
        auto [success, key, cmd] = ParseGetExCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::MSET:
    {
        auto [success, cmd] = ParseMSetCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::MGET:
    {
        auto [success, cmd] = ParseMGetCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::SADD:
    {
        auto [success, key, cmd] = ParseSAddCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::SMEMBERS:
    {
        auto [success, key, cmd] = ParseSMembersCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::SREM:
    {
        auto [success, key, cmd] = ParseSRemCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::SCARD:
    {
        auto [success, key, cmd] = ParseSCardCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::SDIFF:
    {
        auto [success, cmd] = ParseSDiffCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::SDIFFSTORE:
    {
        auto [success, cmd] = ParseSDiffStoreCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::SINTER:
    {
        auto [success, cmd] = ParseSInterCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::SINTERSTORE:
    {
        auto [success, cmd] = ParseSInterStoreCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::SINTERCARD:
    {
        auto [success, cmd] = ParseSInterCardCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::SISMEMBER:
    {
        auto [success, key, cmd] = ParseSIsMemberCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::SMISMEMBER:
    {
        auto [success, key, cmd] = ParseSMIsMemberCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::SMOVE:
    {
        auto [success, cmd] = ParseSMoveCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::SUNION:
    {
        auto [success, cmd] = ParseSUnionCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::SUNIONSTORE:
    {
        auto [success, cmd] = ParseSUnionStoreCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::SRANDMEMBER:
    {
        auto [success, key, cmd] =
            ParseSRandMemberCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::SPOP:
    {
        auto [success, key, cmd] = ParseSPopCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::SSCAN:
    {
        auto [success, key, cmd] = ParseSScanCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::SORT:
    {
        auto [success, cmd] = ParseSortCommand(cmd_arg_list, output);
        if (success)
        {
            ExecuteCommand(ctx, txm, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::FLUSHDB:
    {
        LOG(INFO) << "Do not support ddl in lua script.";
        break;
    }
    case RedisCommandType::FLUSHALL:
    {
        LOG(INFO) << "Do not support ddl in lua script.";
        break;
    }
    case RedisCommandType::ZPOPMIN:
    {
        auto [success, key, cmd] = ParseZPopMinCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::ZPOPMAX:
    {
        auto [success, key, cmd] = ParseZPopMaxCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::ZLEXCOUNT:
    {
        auto [success, key, cmd] = ParseZLexCountCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::ZUNION:
    {
        auto [success, cmd] = ParseZUnionCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::ZUNIONSTORE:
    {
        auto [success, cmd] = ParseZUnionStoreCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::ZINTER:
    {
        auto [success, cmd] = ParseZInterCommand(cmd_arg_list, output);
        if (success)
        {
            ExecuteCommand(ctx, txm, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::ZINTERCARD:
    {
        auto [success, cmd] = ParseZInterCardCommand(cmd_arg_list, output);
        if (success)
        {
            ExecuteCommand(ctx, txm, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::ZINTERSTORE:
    {
        auto [success, cmd] = ParseZInterStoreCommand(cmd_arg_list, output);
        if (success)
        {
            ExecuteCommand(ctx, txm, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::ZREMRANGE:
    {
        auto [success, key, cmd] = ParseZRemRangeCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::ZMSCORE:
    {
        auto [success, key, cmd] = ParseZMScoreCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::ZMPOP:
    {
        auto [success, cmd] = ParseZMPopCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::ZRANGE:
    {
        auto [success, key, cmd] = ParseZRangeCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::ZRANGESTORE:
    {
        auto [success, cmd] = ParseZRangeStoreCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::ZDIFF:
    {
        auto [success, cmd] = ParseZDiffCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::ZDIFFSTORE:
    {
        auto [success, cmd] = ParseZDiffStoreCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::ZINCRBY:
    {
        auto [success, key, cmd] = ParseZIncrByCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::BITFIELD:
    {
        auto [success, key, cmd] = ParseBitFieldCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::BITFIELD_RO:
    {
        auto [success, key, cmd] = ParseBitFieldRoCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::BITPOS:
    {
        auto [success, key, cmd] = ParseBitPosCommand(cmd_arg_list, output);
        if (success)
        {
            cmd.SetVolatile();
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::BITOP:
    {
        auto [success, cmd] = ParseBitOpCommand(cmd_arg_list, output);
        if (success)
        {
            ExecuteCommand(ctx, txm, &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::SCAN:
    {
        auto [success, cmd] = ParseScanCommand(ctx, cmd_arg_list, output);
        if (success)
        {
            ExecuteCommand(
                ctx, txm, RedisTableName(ctx->db_id), &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::KEYS:
    {
        auto [success, cmd] = ParseKeysCommand(cmd_arg_list, output);
        if (success)
        {
            ExecuteCommand(
                ctx, txm, RedisTableName(ctx->db_id), &cmd, output, false);
        }
        break;
    }
    case RedisCommandType::MEMORY_USAGE:
    {
        auto [success, key, cmd] =
            ParseMemoryUsageCommand(cmd_arg_list, output);
        if (success)
        {
            ExecuteCommand(ctx, txm, key, &cmd, output, false);
        }
        break;
    }
    default:
        LOG(WARNING) << "Lua unsupported command type: " << cmd_arg_list[0];
        output->OnError("Unknown Redis command called from script");
    }

    if (collect_duration)
    {
        end = metrics::Clock::now();
        duration =
            std::chrono::duration_cast<std::chrono::microseconds>(end - start)
                .count();

        if (slow_log_threshold == 0 ||
            (slow_log_threshold > 0 &&
             duration > static_cast<uint64_t>(slow_log_threshold)))
        {
            uint32_t group_id = bthread::tls_task_group->group_id_;
            std::lock_guard<bthread::Mutex> slow_log_lk(
                *slow_log_mutexes_[group_id]);
            if (!slow_log_[group_id].empty())
            {
                uint32_t next_idx = next_slow_log_idx_[group_id]++;
                if (next_slow_log_idx_[group_id] >= slow_log_[group_id].size())
                {
                    next_slow_log_idx_[group_id] = 0;
                }

                if (slow_log_len_[group_id] < slow_log_[group_id].size())
                {
                    slow_log_len_[group_id]++;
                }

                slow_log_[group_id][next_idx].id_ =
                    next_slow_log_unique_id_[group_id]++;
                slow_log_[group_id][next_idx].execution_time_ = duration;
                slow_log_[group_id][next_idx].timestamp_ =
                    end.time_since_epoch().count();
                slow_log_[group_id][next_idx].cmd_.clear();
                for (auto &arg : cmd_args)
                {
                    if (slow_log_[group_id][next_idx].cmd_.size() == 31 &&
                        cmd_args.size() > 32)
                    {
                        // slow logs are truncated to 32 args
                        std::string truncated_cmd =
                            "... (" + std::to_string(cmd_args.size() - 31) +
                            " more arguments)";
                        slow_log_[group_id][next_idx].cmd_.push_back(
                            truncated_cmd);
                        break;
                    }

                    if (arg.size() > 128)
                    {
                        // arg longer than 128 chars are truncated
                        std::string truncated_arg =
                            arg.substr(0, 128) + "... (" +
                            std::to_string(arg.size() - 128) + " more bytes)";
                        slow_log_[group_id][next_idx].cmd_.push_back(
                            truncated_arg);
                    }
                    else
                    {
                        slow_log_[group_id][next_idx].cmd_.push_back(arg);
                    }
                }
                if (ctx->socket != nullptr)
                {
                    slow_log_[group_id][next_idx].client_addr_ =
                        ctx->socket->remote_side();
                }
                else
                {
                    slow_log_[group_id][next_idx].client_addr_.reset();
                }
                slow_log_[group_id][next_idx].client_name_ =
                    ctx->connection_name;
            }
        }
    }

    if (metrics::enable_metrics && cmd_type != RedisCommandType::UNKNOWN)
    {
        std::string_view cmd_type_sv = cmd_arg_list[0];
        std::string_view access_type = GetCommandAccessType(cmd_type_sv);
        auto core_id = bthread::tls_task_group->group_id_;
        auto meter = GetMeter(core_id);
        if (is_collecting_duration_round)
        {
            assert(collect_duration);
            meter->Collect(
                metrics::NAME_REDIS_COMMAND_DURATION, duration, cmd_type_sv);
            if (access_type == "read" || access_type == "write")
            {
                meter->Collect(metrics::NAME_REDIS_COMMAND_AGGREGATED_DURATION,
                               duration,
                               access_type);
            }
        }
        meter->Collect(metrics::NAME_REDIS_COMMAND_TOTAL, 1, cmd_type_sv);
        if (access_type == "read" || access_type == "write")
        {
            meter->Collect(
                metrics::NAME_REDIS_COMMAND_AGGREGATED_TOTAL, 1, access_type);
        }
    }
    return;
}

bool RedisServiceImpl::ExecuteTxRequest(
    txservice::TransactionExecution *txm,
    txservice::ObjectCommandTxRequest *tx_req,
    EloqKV::OutputHandler *output,
    EloqKV::OutputHandler *error)
{
    assert(txm != nullptr);
    assert(!tx_req->auto_commit_ ||
           (dynamic_cast<ZScanCommand *>(tx_req->Command()) != nullptr ||
            (output != nullptr)));
    // if (tx_req->auto_commit_)
    // {
    //     assert(output != nullptr);
    // }

    if (enable_redis_stats_)
    {
        if (tx_req->Command()->IsReadOnly())
        {
            RedisStats::IncrReadCommand();
        }
        else
        {
            RedisStats::IncrWriteCommand();
        }
    }
    bool success = SendTxRequestAndWaitResult(txm, tx_req, error);

    if (!success)
    {
        // The output must have been set if it's not nullptr.
        return false;
    }

    assert(tx_req->Result() != txservice::RecordStatus::Unknown);

    if (output != nullptr)
    {
        // output result to output
        const auto &command =
            static_cast<const RedisCommand &>(*tx_req->Command());

        command.OutputResult(output);
    }

    return true;
}

bool RedisServiceImpl::ExecuteMultiObjTxRequest(
    TransactionExecution *txm,
    MultiObjectCommandTxRequest *tx_req,
    OutputHandler *output,
    OutputHandler *error)
{
    assert(txm != nullptr);
    MultiObjectTxCommand *mcmd = tx_req->Command();

    if (enable_redis_stats_)
    {
        RedisStats::IncrMultiObjectCommand();
        // RedisStats::IncrCmdPerSec();
        if (mcmd->IsBlockCommand())
        {
            RedisStats::IncrBlockClient();
        }
    }

    // If command has multi step and tx_req->auto_commit_ is set,
    // tx_req->auto_commit_ will be reset to "false" at middle step and
    // reset to initial value at last step.
    bool auto_commit_original = tx_req->auto_commit_;

    if (!mcmd->IsLastStep())
    {
        tx_req->auto_commit_ = false;
    }

    do
    {
        if (mcmd->IsLastStep())
        {
            tx_req->auto_commit_ = auto_commit_original;
        }

        bool success = SendTxRequestAndWaitResult(txm, tx_req, error);
        if (!success)
        {
            // The output must have been set if it's not nullptr and error
            // occurs. And if tx_req.auto_commit_ is true, tx has been
            // aborted in TxExecution.
            if (auto_commit_original && !tx_req->auto_commit_)
            {
                AbortTx(txm);
            }

            if (enable_redis_stats_ && mcmd->IsBlockCommand())
            {
                RedisStats::DecrBlockClient();
            }
            return false;
        }

        if (!mcmd->IsLastStep())
        {
            if (mcmd->HandleMiddleResult())
            {
                mcmd->IncrSteps();
                tx_req->Reset();
            }
            else
            {
                break;
            }
        }
        else
        {
            break;
        }
    } while (!mcmd->IsFinished());

    if (output != nullptr)
    {
        // output result
        const auto &command =
            static_cast<const RedisMultiObjectCommand &>(*tx_req->Command());
        command.OutputResult(output);
    }

    // For multi-stage command, tx_req->auto_commit_ has been reset to
    // "false" at middle stage. Here should judge tx_req's initial and
    // current "auto_commit" value. If tx_req->auto_commit_ is true, the req
    // was committed in tx_service.
    if (auto_commit_original && !tx_req->auto_commit_)
    {
        if (mcmd->IsPassed())
        {
            CommitTx(txm);
        }
        else
        {
            AbortTx(txm);
        }
    }

    if (enable_redis_stats_ && mcmd->IsBlockCommand())
    {
        RedisStats::DecrBlockClient();
    }
    return true;
}

bool RedisServiceImpl::ExecuteUpsertTableTxRequest(TransactionExecution *txm,
                                                   UpsertTableTxRequest *tx_req,
                                                   OutputHandler *output,
                                                   bool wait_result)
{
    assert(txm != nullptr);
    txm->Execute(tx_req);
    if (wait_result)
    {
        tx_req->Wait();

        if (tx_req->IsError() ||
            tx_req->Result() != txservice::UpsertResult::Succeeded)
        {
            LOG(INFO) << "commit error: " << tx_req->ErrorMsg();
            output->OnError(tx_req->ErrorMsg());
            return false;
        }

        output->OnStatus("OK");
    }
    return true;
}

void RedisServiceImpl::ExecuteGetConfig(ConfigCommand *cmd)
{
    bool expected = false;
    while (!config_accessing_.compare_exchange_strong(
        expected, true, std::memory_order_acq_rel))
    {
        bthread_usleep(1000);
        expected = false;
    }
    std::unordered_set<std::string> smallcase_set;
    for (auto &c : config_)
    {
        for (size_t i = 0; i < cmd->keys_.size(); ++i)
        {
            if (stringmatchlen(cmd->keys_[i].data(),
                               cmd->keys_[i].length(),
                               c.first.data(),
                               c.first.length(),
                               1))
            {
                std::string small_case = c.first;
                std::transform(small_case.begin(),
                               small_case.end(),
                               small_case.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                if (smallcase_set.find(small_case) == smallcase_set.end())
                {
                    cmd->results_.emplace_back(c.first);
                    cmd->results_.emplace_back(c.second);
                    smallcase_set.insert(small_case);
                }
            }
        }
    }
    config_accessing_.store(false, std::memory_order_release);
}

void RedisServiceImpl::ExecuteSetConfig(ConfigCommand *cmd)
{
    bool expected = false;
    while (!config_accessing_.compare_exchange_strong(
        expected, true, std::memory_order_acq_rel))
    {
        bthread_usleep(1000);
        expected = false;
    }
    for (size_t i = 0; i < cmd->keys_.size(); ++i)
    {
        config_[std::string(cmd->keys_[i])] = std::string(cmd->values_[i]);
        if (cmd->keys_[i] == "slowlog-log-slower-than")
        {
            slow_log_threshold_ = std::stoul(std::string(cmd->values_[i]));
        }
        else if (cmd->keys_[i] == "slowlog-max-len")
        {
            ResizeSlowLog(std::stoul(std::string(cmd->values_[i])));
        }
    }
    config_accessing_.store(false, std::memory_order_release);
}

bool RedisServiceImpl::ExecuteCommand(RedisConnectionContext *ctx,
                                      txservice::TransactionExecution *txm,
                                      SortCommand *cmd,
                                      EloqKV::OutputHandler *output,
                                      bool auto_commit)
{
    const TableName *table_name = RedisTableName(ctx->db_id);

    RedisObjectType obj_type = RedisObjectType::Unknown;
    std::vector<std::string> load_vector;
    bool success = SortCommand::Load(
        this, txm, output, table_name, cmd->sort_key_, obj_type, load_vector);
    if (!success)
    {
        if (auto_commit)
        {
            AbortTx(txm);
        }
        return false;
    }

    std::vector<SortCommand::SortObject> sort_vector;
    sort_vector.reserve(load_vector.size());

    if (cmd->by_pattern_.has_value() && !cmd->by_pattern_->ContainStarSymbol())
    {
        /* When sorting a set with no sort specified, we must sort the
         * output so the result is consistent across scripting and
         * replication.
         *
         * The other types (list, sorted set) will retain their native order
         * even if no sort order is requested, so they remain stable across
         * scripting and replication. */
        if (obj_type == RedisObjectType::Set &&
            (cmd->store_destination_.has_value() || ctx->socket == nullptr))
        {
            cmd->ForceAlphaSort();
        }
    }

    if (!cmd->by_pattern_.has_value())
    {
        if (cmd->alpha_)
        {
            SortCommand::PrepareStringSortObjects(load_vector, sort_vector);
        }
        else
        {
            success = SortCommand::PrepareScoreSortObjects(
                load_vector, sort_vector, output);
            if (!success)
            {
                if (auto_commit)
                {
                    AbortTx(txm);
                }
                return false;
            }
        }
    }
    else if (!cmd->by_pattern_->ContainStarSymbol())
    {
        // Skip sort.
        SortCommand::PrepareStringSortObjects(load_vector, sort_vector);
    }
    else
    {
        std::vector<SortCommand::AccessKey> access_keys;
        access_keys.reserve(load_vector.size());
        for (const std::string &id : load_vector)
        {
            access_keys.push_back(cmd->by_pattern_->MakeAccessKey(id));
        }

        std::vector<std::optional<std::string>> by_values;
        success = SortCommand::MultiGet(this,
                                        txm,
                                        output,
                                        table_name,
                                        cmd->by_pattern_.value(),
                                        access_keys,
                                        by_values);
        if (!success)
        {
            if (auto_commit)
            {
                AbortTx(txm);
            }
            return false;
        }

        assert(load_vector.size() == by_values.size());
        if (cmd->alpha_)
        {
            SortCommand::PrepareStringSortObjects(
                load_vector, by_values, sort_vector);
        }
        else
        {
            success = SortCommand::PrepareScoreSortObjects(
                load_vector, by_values, sort_vector, output);
            if (!success)
            {
                if (auto_commit)
                {
                    AbortTx(txm);
                }
                return false;
            }
        }
    }

    if (!cmd->by_pattern_.has_value() || cmd->by_pattern_->ContainStarSymbol())
    {
        std::stable_sort(
            sort_vector.begin(), sort_vector.end(), cmd->LessFunc());
    }
    else if (cmd->by_pattern_.has_value() &&
             !cmd->by_pattern_->ContainStarSymbol() && cmd->desc_)
    {
        std::reverse(sort_vector.begin(), sort_vector.end());
    }

    absl::Span<const SortCommand::SortObject> sort_span =
        cmd->Limit(sort_vector);

    std::vector<std::optional<std::string>> &sort_result = cmd->result_.result_;

    if (cmd->get_pattern_vec_.empty())
    {
        sort_result.reserve(sort_span.size());
        for (const SortCommand::SortObject &sort_obj : sort_span)
        {
            sort_result.emplace_back(sort_obj.id_);
        }
    }
    else
    {
        std::vector<std::vector<std::optional<std::string>>> get_values_vec(
            cmd->get_pattern_vec_.size());
        for (size_t i = 0; i < cmd->get_pattern_vec_.size(); i++)
        {
            const SortCommand::Pattern &get_pattern = cmd->get_pattern_vec_[i];
            std::vector<std::optional<std::string>> &get_values =
                get_values_vec[i];

            if (get_pattern.IsPoundSymbol())
            {
                get_values.reserve(sort_span.size());
                for (const SortCommand::SortObject &sort_obj : sort_span)
                {
                    get_values.emplace_back(sort_obj.id_);
                }
            }
            else if (!get_pattern.ContainStarSymbol())
            {
                // "If we can't find '*' in the pattern we return NULL as
                // to GET a fixed key does not make sense."
                get_values.resize(sort_span.size());
            }
            else
            {
                std::vector<SortCommand::AccessKey> access_keys;
                access_keys.reserve(sort_span.size());
                for (const SortCommand::SortObject &sort_obj : sort_span)
                {
                    access_keys.push_back(
                        get_pattern.MakeAccessKey(sort_obj.id_));
                }

                success = SortCommand::MultiGet(this,
                                                txm,
                                                output,
                                                table_name,
                                                get_pattern,
                                                access_keys,
                                                get_values);
                if (!success)
                {
                    if (auto_commit)
                    {
                        AbortTx(txm);
                    }
                    return false;
                }
            }
        }

        sort_result.reserve(sort_span.size() * get_values_vec.size());
        for (size_t i = 0; i < sort_span.size(); i++)
        {
            for (size_t j = 0; j < get_values_vec.size(); j++)
            {
                std::optional<std::string> &value = get_values_vec[j][i];
                sort_result.emplace_back(std::move(value));
            }
        }
    }

    if (cmd->store_destination_.has_value())
    {
        std::vector<EloqString> v;
        v.reserve(sort_result.size());
        for (const std::optional<std::string> &elem : sort_result)
        {
            v.emplace_back(elem.has_value() ? std::string_view(elem.value())
                                            : "");
        }
        success = SortCommand::Store(this,
                                     txm,
                                     output,
                                     table_name,
                                     cmd->store_destination_.value(),
                                     std::move(v));
        if (!success)
        {
            if (auto_commit)
            {
                AbortTx(txm);
            }
            return false;
        }
    }

    cmd->OutputResult(output);
    CommitTx(txm);
    return true;
}

#ifdef WITH_FAULT_INJECT
bool RedisServiceImpl::ExecuteCommand(RedisConnectionContext *ctx,
                                      TransactionExecution *txm,
                                      RedisFaultInjectCommand *cmd,
                                      OutputHandler *output,
                                      bool auto_commit)
{
    FaultInjectTxRequest fi_req(cmd->fault_name_,
                                cmd->fault_paras_,
                                cmd->vct_node_id_,
                                nullptr,
                                nullptr,
                                txm);

    bool success = SendTxRequestAndWaitResult(txm, &fi_req, output);
    if (!success)
    {
        // The output must have been set if it's not nullptr and error
        // occurs.
        if (auto_commit)
        {
            AbortTx(txm);
        }
        return false;
    }

    cmd->OutputResult(output, ctx);

    if (auto_commit)
    {
        CommitTx(txm);
    }

    return true;
}
#endif

bool RedisServiceImpl::IsRecordTTLExpired(
    const txservice::TxRecord *rec, txservice::LocalCcShards *local_cc_shards)
{
    if (rec != nullptr && rec->HasTTL())
    {
        if (rec->GetTTL() < local_cc_shards->TsBaseInMillseconds())
        {
            return true;
        }
    }

    return false;
}

bool RedisServiceImpl::ExecuteCommand(RedisConnectionContext *ctx,
                                      TransactionExecution *txm,
                                      const TableName *redis_table_name,
                                      ScanCommand *cmd,
                                      OutputHandler *output,
                                      bool auto_commit)
{
    TxKey start_tx_key;
    TxKey end_tx_key;
    EloqKey prefix_key;
    EloqKey prefix_key_next;
    std::string_view pattern = cmd->pattern_.StringView();
    bool start_inclusive = false;
    bool end_inclusive = false;

    if (!pattern.empty())
    {
        size_t pos = pattern.find('*');
        if (pos != std::string_view::npos && pos != 0)
        {
            std::string_view prefix = pattern.substr(0, pos);
            prefix_key = EloqKey(prefix);

            start_tx_key = TxKey(&prefix_key);
            start_inclusive = true;

            std::string prefix_next = std::string(prefix);
            bool increased = false;
            for (int i = static_cast<int>(prefix_next.size()) - 1; i >= 0; --i)
            {
                auto c = static_cast<unsigned char>(prefix_next[i]);
                if (c != 0xFF)
                {
                    prefix_next[i] = static_cast<char>(c + 1);
                    prefix_next.resize(i + 1);
                    increased = true;
                    break;
                }
            }

            if (increased)
            {
                prefix_key_next =
                    EloqKey(prefix_next.data(), prefix_next.size());
                end_tx_key = TxKey(&prefix_key_next);
            }
            else
            {
                end_tx_key = TxKey(EloqKey::PositiveInfinity());
            }
        }
        else
        {
            start_tx_key = TxKey(EloqKey::NegativeInfinity());
            end_tx_key = TxKey(EloqKey::PositiveInfinity());
        }
    }
    else
    {
        start_tx_key = TxKey(EloqKey::NegativeInfinity());
        end_tx_key = TxKey(EloqKey::PositiveInfinity());
    }

    bool is_ckpt = false;
    bool is_for_write = false;
    bool is_for_share = false;
    bool is_covering_keys = false;
    bool is_require_keys = true;
    bool is_require_recs = false;
    bool is_require_sort = true;
    bool is_read_local = false;

    // bool is_scan_end = true;
    std::vector<std::string> &vct_rst = cmd->result_.vct_key_;
    int64_t obj_cnt = 0;
    std::unique_ptr<BucketScanCursor> scan_cursor_owner = nullptr;
    size_t cache_idx = 0;
    if (cmd->scan_cursor_ == 0)
    {
        scan_cursor_owner = std::make_unique<BucketScanCursor>();
        cmd->scan_cursor_ = scan_cursor_owner.get();
    }
    else
    {
        assert(cmd->scan_cursor_ != nullptr);
        if (cmd->scan_cursor_->obj_type_ != cmd->obj_type_ ||
            cmd->scan_cursor_->cmd_pattern_ != cmd->pattern_.StringView())
        {
            if (auto_commit)
            {
                AbortTx(txm);
            }
            if (output != nullptr)
            {
                output->OnError(
                    redis_get_error_messages(RD_ERR_INVALID_CURSOR));
            }
            return false;
        }

        cache_idx = cmd->scan_cursor_->cache_idx_;
        for (cache_idx = cmd->scan_cursor_->cache_idx_;
             cache_idx < cmd->scan_cursor_->cache_.size();
             ++cache_idx)
        {
            if (cmd->count_ > 0 && obj_cnt >= cmd->count_)
            {
                break;
            }

            obj_cnt++;
            vct_rst.emplace_back(cmd->scan_cursor_->cache_[cache_idx]);
        }
    }

    BucketScanSavePoint *save_point = &cmd->scan_cursor_->save_point_;
    bool more_plan = save_point->prev_pause_idx_ == UINT64_MAX ||
                     save_point->prev_pause_idx_ < save_point->PlanSize();

    if ((cmd->count_ < 0 || obj_cnt < cmd->count_) && more_plan)
    {
        // Fetch catalog and acquire read lock on catalog table
        CatalogKey catalog_key(*redis_table_name);
        TxKey cat_tx_key(&catalog_key);
        CatalogRecord catalog_rec;
        ReadTxRequest read_req(&txservice::catalog_ccm_name,
                               0,
                               &cat_tx_key,
                               &catalog_rec,
                               false,
                               false,
                               true,
                               0,
                               false,
                               false,
                               false,
                               nullptr,
                               nullptr,
                               txm);
        txm->Execute(&read_req);
        read_req.Wait();
        if (read_req.IsError())
        {
            if (auto_commit)
            {
                AbortTx(txm);
            }
            if (output != nullptr)
            {
                output->OnError(read_req.ErrorMsg());
            }
            return false;
        }

        uint64_t schema_version = catalog_rec.SchemaTs();

        // scan command is non blocking command, we disable filter pushdown
        // only pushdown for keys command
        bool filter_pushdown = false;
        if (cmd->count_ == -1)
        {
            filter_pushdown = true;
        }

        ScanOpenTxRequest scan_open(
            redis_table_name,
            schema_version,
            ScanIndexType::Primary,
            &start_tx_key,
            start_inclusive,
            &end_tx_key,
            end_inclusive,
            ScanDirection::Forward,
            is_ckpt,
            is_for_write,
            is_for_share,
            is_covering_keys,
            is_require_keys,
            is_require_recs,
            is_require_sort,
            is_read_local,
            nullptr,
            nullptr,
            txm,
            filter_pushdown ? static_cast<int32_t>(cmd->obj_type_) : -1,
            filter_pushdown ? cmd->pattern_.StringView() : "",
            save_point);

        bool success = SendTxRequestAndWaitResult(txm, &scan_open, output);
        if (!success)
        {
            if (scan_open.tx_result_.ErrorCode() == TxErrorCode::INVALID_CURSOR)
            {
                if (cmd->scan_cursor_->cursor_id_ != 0)
                {
                    ctx->RemoveBucketScanCursor();
                }
            }

            // The output must have been set if it's not nullptr and error
            // occurs.
            if (auto_commit)
            {
                AbortTx(txm);
            }
            return false;
        }

        uint64_t scan_alias = scan_open.Result();
        assert(scan_alias != UINT64_MAX);

        size_t current_index = 0;
        if (save_point->prev_pause_idx_ != UINT64_MAX)
        {
            current_index = save_point->prev_pause_idx_;
        }

        size_t plan_size = save_point->PlanSize();

        BucketScanPlan plan = save_point->PickPlan(current_index);
        std::vector<txservice::ScanBatchTuple> scan_batch;
        std::vector<txservice::UnlockTuple> unlock_batch;

        while (current_index < plan_size)
        {
            scan_batch.clear();
            ScanBatchTxRequest scan_batch_req(
                scan_alias,
                *redis_table_name,
                &scan_batch,
                nullptr,
                nullptr,
                txm,
                filter_pushdown ? static_cast<int32_t>(cmd->obj_type_) : -1,
                filter_pushdown ? cmd->pattern_.StringView() : "",
                &plan);
            success = SendTxRequestAndWaitResult(txm, &scan_batch_req, output);
            if (!success)
            {
                if (scan_batch_req.tx_result_.ErrorCode() ==
                    TxErrorCode::INVALID_CURSOR)
                {
                    if (cmd->scan_cursor_->cursor_id_ != 0)
                    {
                        ctx->RemoveBucketScanCursor();
                    }
                }

                // add trailing tuple into read set
                txm->CloseTxScan(scan_alias, *redis_table_name, unlock_batch);

                // The output must have been set if it's not nullptr and
                // error occurs.
                if (auto_commit)
                {
                    AbortTx(txm);
                }
                return false;
            }

            size_t scan_batch_idx = 0;
            for (; scan_batch_idx < scan_batch.size(); ++scan_batch_idx)
            {
                const ScanBatchTuple &tuple = scan_batch[scan_batch_idx];
                const std::string_view sv =
                    tuple.key_.GetKey<EloqKey>()->StringView();

                if (tuple.status_ != RecordStatus::Normal)
                {
                    continue;
                }

                if (!filter_pushdown)
                {
                    if (static_cast<int32_t>(cmd->obj_type_) != -1 &&
                        tuple.object_type_ !=
                            static_cast<int32_t>(cmd->obj_type_))
                    {
                        obj_cnt++;
                        if (cmd->count_ > 0 && obj_cnt >= cmd->count_)
                        {
                            scan_batch_idx++;
                            break;
                        }
                        else
                        {
                            continue;
                        }
                    }

                    if (cmd->pattern_.Length() > 0 &&
                        stringmatchlen(cmd->pattern_.Data(),
                                       cmd->pattern_.Length(),
                                       sv.data(),
                                       sv.size(),
                                       0) == 0)
                    {
                        obj_cnt++;
                        if (cmd->count_ > 0 && obj_cnt >= cmd->count_)
                        {
                            scan_batch_idx++;
                            break;
                        }
                        else
                        {
                            continue;
                        }
                    }
                }

                vct_rst.emplace_back(sv);
                obj_cnt++;

                if (cmd->count_ > 0 && obj_cnt >= cmd->count_)
                {
                    scan_batch_idx++;
                    // is_scan_end = false;
                    break;
                }
            }

            if (cmd->count_ > 0 && obj_cnt >= cmd->count_)
            {
                cache_idx = 0;
                cmd->scan_cursor_->cache_idx_ = 0;
                cmd->scan_cursor_->cache_.clear();

                for (size_t idx = scan_batch_idx; idx < scan_batch.size();
                     ++idx)
                {
                    const ScanBatchTuple &tuple = scan_batch[idx];

                    if (tuple.status_ != RecordStatus::Normal)
                    {
                        continue;
                    }

                    const std::string_view sv =
                        tuple.key_.GetKey<EloqKey>()->StringView();

                    if (!filter_pushdown)
                    {
                        if (static_cast<int32_t>(cmd->obj_type_) != -1 &&
                            tuple.object_type_ !=
                                static_cast<int32_t>(cmd->obj_type_))
                        {
                            continue;
                        }

                        if (cmd->pattern_.Length() > 0 &&
                            stringmatchlen(cmd->pattern_.Data(),
                                           cmd->pattern_.Length(),
                                           sv.data(),
                                           sv.size(),
                                           0) == 0)
                        {
                            continue;
                        }
                    }

                    // unlock_batch.emplace_back(
                    //    tuple.cce_addr_, tuple.version_ts_, tuple.status_);
                    cmd->scan_cursor_->cache_.emplace_back(sv);
                }

                if (scan_batch_req.Result())
                {
                    if (current_index + 1 == plan_size)
                    {
                        // no more data
                        save_point->prev_pause_idx_ = plan_size;
                        save_point->pause_position_.clear();
                    }
                    else
                    {
                        save_point->prev_pause_idx_ = current_index;
                        save_point->pause_position_ = plan.CurrentPosition();
                    }
                }
                else
                {
                    save_point->prev_pause_idx_ = current_index;
                    save_point->pause_position_ = plan.CurrentPosition();
                }
                break;
            }

            // current plan is finished, move to next plan
            if (scan_batch_req.Result())
            {
                current_index++;
                if (current_index < plan_size)
                {
                    plan = save_point->PickPlan(current_index);
                }
                else
                {
                    cache_idx = 0;
                    cmd->scan_cursor_->cache_idx_ = 0;
                    cmd->scan_cursor_->cache_.clear();
                    save_point->prev_pause_idx_ = plan_size;
                    save_point->pause_position_.clear();
                }
            }
        }

        txm->CloseTxScan(scan_alias, *redis_table_name, unlock_batch);
    }
    else
    {
        // update cache idx
        cmd->scan_cursor_->cache_idx_ = cache_idx;
    }

    bool is_scan_end = true;
    if (save_point->prev_pause_idx_ == save_point->PlanSize() &&
        cmd->scan_cursor_->cache_idx_ == cmd->scan_cursor_->cache_.size())
    {
        for (const auto &[node_group_id, bucket_scan_progress] :
             save_point->pause_position_)
        {
            for (const auto &[core_idx, progress] : bucket_scan_progress)
            {
                if (!progress.AllFinished())
                {
                    is_scan_end = false;
                }
            }
        }
    }
    else
    {
        is_scan_end = false;
    }

    if (is_scan_end)
    {
        ctx->RemoveBucketScanCursor();
        cmd->result_.cursor_id_ = 0;
        cmd->scan_cursor_ = nullptr;
    }
    else
    {
        if (cmd->scan_cursor_->cursor_id_ == 0)
        {
            scan_cursor_owner->obj_type_ = cmd->obj_type_;
            scan_cursor_owner->cmd_pattern_ = cmd->pattern_.String();
            cmd->result_.cursor_id_ =
                ctx->CreateBucketScanCursor(std::move(scan_cursor_owner));
        }
        else
        {
            cmd->result_.cursor_id_ = ctx->UpdateBucketScanCursor();
        }
    }

    if (output != nullptr)
    {
        cmd->OutputResult(output, ctx);
    }

    if (auto_commit)
    {
        CommitTx(txm);
    }

    return true;
}

#ifdef VECTOR_INDEX_ENABLED
bool RedisServiceImpl::ExecuteCommand(RedisConnectionContext *ctx,
                                      CreateVecIndexCommand *cmd,
                                      OutputHandler *output)
{
    if (vector_index_worker_pool_ == nullptr)
    {
        output->OnError("ERR Vector Index not enabled");
        return false;
    }
    EloqVec::VectorOpResult res = EloqVec::VectorOpResult::SUCCEED;
    bthread::Mutex mux;
    bthread::ConditionVariable cv;
    bool finished = false;
    {
        std::unique_lock<bthread::Mutex> lk(mux);
        vector_index_worker_pool_->SubmitWork(
            [cmd, &res, &mux, &cv, &finished](size_t)
            {
                // Create VectorIndexMetadata
                EloqVec::VectorIndexMetadata index_metadata(
                    std::move(cmd->index_name_.String()),
                    std::move(cmd->index_config_),
                    std::move(cmd->record_metadata_),
                    cmd->persist_threshold_,
                    EloqVec::VectorHandler::Instance().VectorIndexDataPath());

                res = EloqVec::VectorHandler::Instance().Create(index_metadata);
                std::unique_lock<bthread::Mutex> lk(mux);
                finished = true;
                cv.notify_one();
            });
        while (!finished)
        {
            cv.wait(lk);
        }
    }

    cmd->result_.err_code_ = res == EloqVec::VectorOpResult::SUCCEED
                                 ? RD_OK
                                 : RD_ERR_VECTOR_INDX_CREATE_FAILED;
    cmd->OutputResult(output, ctx);
    return true;
}

bool RedisServiceImpl::ExecuteCommand(RedisConnectionContext *ctx,
                                      InfoVecIndexCommand *cmd,
                                      OutputHandler *output)
{
    if (vector_index_worker_pool_ == nullptr)
    {
        output->OnError("ERR Vector Index not enabled");
        return false;
    }
    EloqVec::VectorOpResult res = EloqVec::VectorOpResult::SUCCEED;
    bthread::Mutex mux;
    bthread::ConditionVariable cv;
    bool finished = false;
    {
        std::unique_lock<bthread::Mutex> lk(mux);
        vector_index_worker_pool_->SubmitWork(
            [cmd, &res, &mux, &cv, &finished](size_t)
            {
                res = EloqVec::VectorHandler::Instance().Info(
                    cmd->index_name_.String(), cmd->metadata_);
                std::unique_lock<bthread::Mutex> lk(mux);
                finished = true;
                cv.notify_one();
            });
        while (!finished)
        {
            cv.wait(lk);
        }
    }

    cmd->result_.err_code_ = res == EloqVec::VectorOpResult::SUCCEED
                                 ? RD_OK
                                 : RD_ERR_VECTOR_INDX_INFO_FAILED;
    cmd->OutputResult(output, ctx);
    return true;
}

bool RedisServiceImpl::ExecuteCommand(RedisConnectionContext *ctx,
                                      DropVecIndexCommand *cmd,
                                      OutputHandler *output)
{
    if (vector_index_worker_pool_ == nullptr)
    {
        output->OnError("ERR Vector Index not enabled");
        return false;
    }
    EloqVec::VectorOpResult res = EloqVec::VectorOpResult::SUCCEED;
    bthread::Mutex mux;
    bthread::ConditionVariable cv;
    bool finished = false;
    {
        std::unique_lock<bthread::Mutex> lk(mux);
        vector_index_worker_pool_->SubmitWork(
            [cmd, &res, &mux, &cv, &finished](size_t)
            {
                res = EloqVec::VectorHandler::Instance().Drop(
                    cmd->index_name_.String());
                std::unique_lock<bthread::Mutex> lk(mux);
                finished = true;
                cv.notify_one();
            });
        while (!finished)
        {
            cv.wait(lk);
        }
    }
    cmd->result_.err_code_ = res == EloqVec::VectorOpResult::SUCCEED
                                 ? RD_OK
                                 : RD_ERR_VECTOR_INDX_DROP_FAILED;
    cmd->OutputResult(output, ctx);
    return true;
}

bool RedisServiceImpl::ExecuteCommand(RedisConnectionContext *ctx,
                                      AddVecIndexCommand *cmd,
                                      OutputHandler *output)
{
    if (vector_index_worker_pool_ == nullptr)
    {
        output->OnError("ERR Vector Index not enabled");
        return false;
    }
    EloqVec::VectorOpResult res = EloqVec::VectorOpResult::SUCCEED;
    bthread::Mutex mux;
    bthread::ConditionVariable cv;
    bool finished = false;
    {
        std::unique_lock<bthread::Mutex> lk(mux);
        vector_index_worker_pool_->SubmitWork(
            [cmd, &res, &mux, &cv, &finished](size_t)
            {
                res = EloqVec::VectorHandler::Instance().Add(
                    cmd->index_name_.String(),
                    cmd->key_,
                    cmd->vector_,
                    cmd->metadata_.StringView());
                std::unique_lock<bthread::Mutex> lk(mux);
                finished = true;
                cv.notify_one();
            });
        while (!finished)
        {
            cv.wait(lk);
        }
    }

    cmd->result_.err_code_ = res == EloqVec::VectorOpResult::SUCCEED
                                 ? RD_OK
                                 : RD_ERR_VECTOR_INDX_ADD_FAILED;
    cmd->OutputResult(output, ctx);
    return true;
}

bool RedisServiceImpl::ExecuteCommand(RedisConnectionContext *ctx,
                                      BAddVecIndexCommand *cmd,
                                      OutputHandler *output)
{
    if (vector_index_worker_pool_ == nullptr)
    {
        output->OnError("ERR Vector Index not enabled");
        return false;
    }
    EloqVec::VectorOpResult res = EloqVec::VectorOpResult::SUCCEED;
    bthread::Mutex mux;
    bthread::ConditionVariable cv;
    bool finished = false;
    {
        std::unique_lock<bthread::Mutex> lk(mux);
        vector_index_worker_pool_->SubmitWork(
            [cmd, &res, &mux, &cv, &finished](size_t)
            {
                std::vector<std::string_view> metadata_list;
                metadata_list.reserve(cmd->metadata_list_.size());
                for (const auto &metadata : cmd->metadata_list_)
                {
                    metadata_list.emplace_back(metadata.StringView());
                }
                res = EloqVec::VectorHandler::Instance().BatchAdd(
                    cmd->index_name_.String(),
                    cmd->keys_,
                    cmd->vectors_,
                    metadata_list);
                std::unique_lock<bthread::Mutex> lk(mux);
                finished = true;
                cv.notify_one();
            });
        while (!finished)
        {
            cv.wait(lk);
        }
    }

    cmd->result_.err_code_ = res == EloqVec::VectorOpResult::SUCCEED
                                 ? RD_OK
                                 : RD_ERR_VECTOR_INDX_ADD_FAILED;
    cmd->OutputResult(output, ctx);
    return true;
}

bool RedisServiceImpl::ExecuteCommand(RedisConnectionContext *ctx,
                                      UpdateVecIndexCommand *cmd,
                                      OutputHandler *output)
{
    if (vector_index_worker_pool_ == nullptr)
    {
        output->OnError("ERR Vector Index not enabled");
        return false;
    }
    EloqVec::VectorOpResult res = EloqVec::VectorOpResult::SUCCEED;
    bthread::Mutex mux;
    bthread::ConditionVariable cv;
    bool finished = false;
    {
        std::unique_lock<bthread::Mutex> lk(mux);
        vector_index_worker_pool_->SubmitWork(
            [cmd, &res, &mux, &cv, &finished](size_t)
            {
                res = EloqVec::VectorHandler::Instance().Update(
                    cmd->index_name_.String(),
                    cmd->key_,
                    cmd->vector_,
                    cmd->metadata_.StringView());
                std::unique_lock<bthread::Mutex> lk(mux);
                finished = true;
                cv.notify_one();
            });
        while (!finished)
        {
            cv.wait(lk);
        }
    }

    cmd->result_.err_code_ = res == EloqVec::VectorOpResult::SUCCEED
                                 ? RD_OK
                                 : RD_ERR_VECTOR_INDX_UPDATE_FAILED;
    cmd->OutputResult(output, ctx);
    return true;
}

bool RedisServiceImpl::ExecuteCommand(RedisConnectionContext *ctx,
                                      DeleteVecIndexCommand *cmd,
                                      OutputHandler *output)
{
    if (vector_index_worker_pool_ == nullptr)
    {
        output->OnError("ERR Vector Index not enabled");
        return false;
    }
    EloqVec::VectorOpResult res = EloqVec::VectorOpResult::SUCCEED;
    bthread::Mutex mux;
    bthread::ConditionVariable cv;
    bool finished = false;
    {
        std::unique_lock<bthread::Mutex> lk(mux);
        vector_index_worker_pool_->SubmitWork(
            [cmd, &res, &mux, &cv, &finished](size_t)
            {
                res = EloqVec::VectorHandler::Instance().Delete(
                    cmd->index_name_.String(), cmd->key_);
                std::unique_lock<bthread::Mutex> lk(mux);
                finished = true;
                cv.notify_one();
            });
        while (!finished)
        {
            cv.wait(lk);
        }
    }

    cmd->result_.err_code_ = res == EloqVec::VectorOpResult::SUCCEED
                                 ? RD_OK
                                 : RD_ERR_VECTOR_INDX_DELETE_FAILED;
    cmd->OutputResult(output, ctx);
    return true;
}

bool RedisServiceImpl::ExecuteCommand(RedisConnectionContext *ctx,
                                      SearchVecIndexCommand *cmd,
                                      OutputHandler *output)
{
    if (vector_index_worker_pool_ == nullptr)
    {
        output->OnError("ERR Vector Index not enabled");
        return false;
    }
    EloqVec::VectorOpResult res = EloqVec::VectorOpResult::SUCCEED;
    bthread::Mutex mux;
    bthread::ConditionVariable cv;
    bool finished = false;
    {
        std::unique_lock<bthread::Mutex> lk(mux);
        vector_index_worker_pool_->SubmitWork(
            [cmd, &res, &mux, &cv, &finished](size_t thread_id)
            {
                res = EloqVec::VectorHandler::Instance().Search(
                    cmd->index_name_.String(),
                    cmd->vector_,
                    cmd->k_count_,
                    thread_id,
                    cmd->filter_json_.StringView(),
                    cmd->search_res_);
                std::unique_lock<bthread::Mutex> lk(mux);
                finished = true;
                cv.notify_one();
            });
        while (!finished)
        {
            cv.wait(lk);
        }
    }

    cmd->result_.err_code_ = res == EloqVec::VectorOpResult::SUCCEED
                                 ? RD_OK
                                 : RD_ERR_VECTOR_INDX_SEARCH_FAILED;
    cmd->OutputResult(output, ctx);
    return true;
}
#endif

const TableName *RedisServiceImpl::RedisTableName(int db_id) const
{
    return &redis_table_names_[db_id];
}

size_t RedisServiceImpl::GetRedisTableCount() const
{
    return redis_table_names_.size();
}

bool RedisServiceImpl::AuthRequired(const RedisConnectionContext *ctx,
                                    const butil::StringPiece &command) const
{
    if (requirepass.empty())
    {
        return false;
    }
    else
    {
        if (ctx->authenticated)
        {
            return false;
        }
        else
        {
            constexpr std::array<std::string_view, 4> cmds_no_auth = {
                "auth", "hello", "quit", "reset"};
            return std::find(
                       cmds_no_auth.begin(),
                       cmds_no_auth.end(),
                       std::string_view(command.data(), command.size())) ==
                   cmds_no_auth.end();
        }
    }
}

std::unique_ptr<brpc::ConnectionContext> RedisServiceImpl::NewConnectionContext(
    brpc::Socket *socket) const
{
    return std::make_unique<RedisConnectionContext>(
        socket, const_cast<PubSubManager *>(&eloqkv_pub_sub_mgr));
}

brpc::RedisCommandHandlerResult RedisServiceImpl::DispatchCommand(
    brpc::ConnectionContext *conn_ctx,
    const std::vector<butil::StringPiece> &args,
    brpc::RedisReply *output,
    bool flush_batched) const
{
    brpc::RedisCommandHandlerResult result = brpc::REDIS_CMD_HANDLED;

    RedisConnectionContext *ctx =
        static_cast<RedisConnectionContext *>(conn_ctx);

    if (AuthRequired(ctx, args[0]))
    {
        output->SetError("NOAUTH Authentication required.");
        return result;
    }

    if (Sharder::Instance().CheckShutdownStatus())
    {
        output->SetError(
            redis_get_error_messages(RD_ERR_ClUSTER_IS_SHUTTING_DOWN));
        return result;
    }

    const std::string_view cmd_type_str(args[0].data(), args[0].size());
    const RedisCommandType cmd_type = CommandType(cmd_type_str);

    bool is_collecting_duration_round = false;
    if (metrics::enable_metrics)
    {
        is_collecting_duration_round = CheckAndUpdateRedisCmdRound(cmd_type);
    }
    metrics::TimePoint start, end;
    uint64_t duration = 0;

    // Collect duration if metrics is enabled or if we need to record slow
    // query.
    // cmds are queued in transaction, not executed immediately. So we don't
    // need to collect duration for them. They are collected in MultiExec.
    int slow_log_threshold =
        slow_log_threshold_.load(std::memory_order_relaxed);
    uint32_t slow_log_max_length =
        slow_log_max_length_.load(std::memory_order_relaxed);
    bool collect_duration =
        ((metrics::enable_metrics && is_collecting_duration_round) ||
         (slow_log_threshold >= 0 && slow_log_max_length > 0)) &&
        cmd_type != RedisCommandType::UNKNOWN && !ctx->in_multi_transaction;
    if (collect_duration)
    {
        start = metrics::Clock::now();
    }

    if (ctx->in_multi_transaction)
    {
        assert(ctx->multi_transaction_handler != nullptr);
        result = ctx->multi_transaction_handler->Run(
            ctx, args, output, flush_batched);
        if (result == brpc::REDIS_CMD_HANDLED)
        {
            ctx->multi_transaction_handler.reset(NULL);
            ctx->in_multi_transaction = false;
        }
        else
        {
            assert(result == brpc::REDIS_CMD_CONTINUE);
        }
    }
    else if (args[0] == "watch" || args[0] == "unwatch")
    {
        if (ctx->txm != nullptr)
        {
            output->SetError("ERR WATCH inside a BEGIN transaction");
            return result;
        }
        if (!ctx->multi_transaction_handler)
        {
            ctx->multi_transaction_handler.reset(
                new MultiTransactionHandler(this));
            ctx->in_multi_transaction = false;
        }
        if (!ctx->multi_transaction_handler)
        {
            output->SetError("ERR Transaction not supported.");
        }
        else
        {
            result = ctx->multi_transaction_handler->Run(
                ctx, args, output, flush_batched);
            if (args[0] == "unwatch" && result == brpc::REDIS_CMD_HANDLED)
            {
                ctx->multi_transaction_handler.reset(nullptr);
                ctx->in_multi_transaction = false;
            }
        }
    }
    else
    {
        // The command is a simple command. Find the command handler to
        // process it.
        // TODO(zkl): consider removing command handler and dispatching the
        //  command via a function table.
        RedisCommandHandler *ch = FindCommandHandler(args[0]);
        if (!ch)
        {
            char buf[64];
            snprintf(buf,
                     sizeof(buf),
                     "ERR unknown command `%s`",
                     args[0].as_string().c_str());
            output->SetError(buf);
        }
        else
        {
            result = ch->Run(ctx, args, output, flush_batched);
            if (result == brpc::REDIS_CMD_CONTINUE)
            {
                // No need to collect duration for commands in transaction since
                // they're just buffered now, not actually executed.
                collect_duration = false;
                if (ctx->multi_transaction_handler == nullptr)
                {
                    ctx->multi_transaction_handler.reset(
                        new MultiTransactionHandler(this));
                }
                if (ctx->multi_transaction_handler != nullptr)
                {
                    ctx->multi_transaction_handler->Begin();
                    ctx->in_multi_transaction = true;
                }
                else
                {
                    output->SetError("ERR Transaction not supported.");
                }
            }
            else
            {
                assert(result == brpc::REDIS_CMD_HANDLED);
            }
        }
    }

    if (collect_duration && !output->is_error())
    {
        end = metrics::Clock::now();
        duration =
            std::chrono::duration_cast<std::chrono::microseconds>(end - start)
                .count();
        if (slow_log_threshold == 0 ||
            (slow_log_threshold > 0 &&
             duration > static_cast<uint64_t>(slow_log_threshold)))
        {
            uint32_t group_id = bthread::tls_task_group->group_id_;
            std::lock_guard<bthread::Mutex> slow_log_lk(
                *slow_log_mutexes_[group_id]);
            if (!slow_log_[group_id].empty())
            {
                uint32_t next_idx = next_slow_log_idx_[group_id]++;
                if (next_slow_log_idx_[group_id] >= slow_log_[group_id].size())
                {
                    next_slow_log_idx_[group_id] = 0;
                }

                if (slow_log_len_[group_id] < slow_log_[group_id].size())
                {
                    slow_log_len_[group_id]++;
                }

                slow_log_[group_id][next_idx].id_ =
                    next_slow_log_unique_id_[group_id]++;
                slow_log_[group_id][next_idx].execution_time_ = duration;
                slow_log_[group_id][next_idx].timestamp_ =
                    end.time_since_epoch().count();
                slow_log_[group_id][next_idx].cmd_.clear();
                for (auto &arg : args)
                {
                    if (slow_log_[group_id][next_idx].cmd_.size() == 31 &&
                        args.size() > 32)
                    {
                        // slow logs are truncated to 32 args
                        std::string truncated_cmd =
                            "... (" + std::to_string(args.size() - 31) +
                            " more arguments)";
                        slow_log_[group_id][next_idx].cmd_.push_back(
                            truncated_cmd);
                        break;
                    }

                    if (arg.size() > 128)
                    {
                        // arg longer than 128 chars are truncated
                        std::string truncated_arg =
                            arg.substr(0, 128).as_string() + "... (" +
                            std::to_string(arg.size() - 128) + " more bytes)";
                        slow_log_[group_id][next_idx].cmd_.push_back(
                            truncated_arg);
                    }
                    else
                    {
                        slow_log_[group_id][next_idx].cmd_.push_back(
                            arg.as_string());
                    }
                }
                if (ctx->socket != nullptr)
                {
                    slow_log_[group_id][next_idx].client_addr_ =
                        ctx->socket->remote_side();
                }
                else
                {
                    slow_log_[group_id][next_idx].client_addr_.reset();
                }

                slow_log_[group_id][next_idx].client_name_ =
                    ctx->connection_name;
            }
        }
    }

    if (metrics::enable_metrics && cmd_type != RedisCommandType::UNKNOWN &&
        !output->is_error())
    {
        auto access_type = GetCommandAccessType(cmd_type_str);
        auto core_id = bthread::tls_task_group->group_id_;
        auto meter = GetMeter(core_id);
        if (is_collecting_duration_round && collect_duration)
        {
            assert(collect_duration);
            meter->Collect(
                metrics::NAME_REDIS_COMMAND_DURATION, duration, cmd_type_str);
            if (access_type == "read" || access_type == "write")
            {
                meter->Collect(metrics::NAME_REDIS_COMMAND_AGGREGATED_DURATION,
                               duration,
                               access_type);
            }
        }
        meter->Collect(metrics::NAME_REDIS_COMMAND_TOTAL, 1, cmd_type_str);
        if (access_type == "read" || access_type == "write")
        {
            meter->Collect(
                metrics::NAME_REDIS_COMMAND_AGGREGATED_TOTAL, 1, access_type);
        }
    }
    return result;
}

void RedisServiceImpl::ResetSlowLog()
{
    if (slow_log_threshold_.load(std::memory_order_relaxed) < 0 ||
        slow_log_max_length_.load(std::memory_order_relaxed) == 0)
    {
        return;
    }

    for (size_t core_idx = 0; core_idx < core_num_; ++core_idx)
    {
        std::lock_guard<bthread::Mutex> slow_log_lk(
            *slow_log_mutexes_[core_idx]);
        slow_log_len_[core_idx] = 0;
    }
}

void RedisServiceImpl::GetSlowLog(std::list<SlowLogEntry> &results, int count)
{
    int slow_log_threshold =
        slow_log_threshold_.load(std::memory_order_relaxed);
    uint32_t slow_log_max_length =
        slow_log_max_length_.load(std::memory_order_relaxed);

    if (slow_log_threshold < 0 || slow_log_max_length == 0)
    {
        return;
    }

    if (count < 0 || count > static_cast<int>(slow_log_max_length))
    {
        count = slow_log_max_length;
    }

    for (size_t core_idx = 0; core_idx < core_num_; ++core_idx)
    {
        uint32_t group_id = core_idx;
        std::lock_guard<bthread::Mutex> slow_log_lk(
            *slow_log_mutexes_[group_id]);

        int32_t idx = next_slow_log_idx_[group_id] - 1;
        if (idx < 0)
        {
            idx = slow_log_[group_id].size() - 1;
        }
        // merge existing result list with slow log on this task group
        auto result_iter = results.begin();
        int result_idx = 0;
        for (uint32_t i = 0; i < slow_log_len_[group_id]; ++i)
        {
            // Sort the result in timestamp descending order
            while (result_iter != results.end() &&
                   result_iter->timestamp_ >
                       slow_log_[group_id][idx].timestamp_ &&
                   result_idx < count)
            {
                result_idx++;
                result_iter++;
            }

            if (result_idx >= count)
            {
                break;
            }
            result_iter = results.insert(result_iter, slow_log_[group_id][idx]);
            result_iter++;
            result_idx++;

            idx--;
            if (idx < 0)
            {
                idx = slow_log_[group_id].size() - 1;
            }
        }

        if (results.size() >= static_cast<size_t>(count))
        {
            results.resize(count);
        }
    }
}

uint32_t RedisServiceImpl::GetSlowLogLen() const
{
    int slow_log_threshold =
        slow_log_threshold_.load(std::memory_order_relaxed);
    uint32_t slow_log_max_length =
        slow_log_max_length_.load(std::memory_order_relaxed);

    if (slow_log_threshold < 0 || slow_log_max_length == 0)
    {
        return 0;
    }

    uint32_t len = 0;
    for (size_t core_idx = 0; core_idx < core_num_; ++core_idx)
    {
        std::lock_guard<bthread::Mutex> slow_log_lk(
            *slow_log_mutexes_[core_idx]);
        len += slow_log_len_[core_idx];
    }

    return len > slow_log_max_length ? slow_log_max_length : len;
}

void RedisServiceImpl::ResizeSlowLog(uint32_t len)
{
    std::vector<std::unique_lock<bthread::Mutex>> locks;
    locks.reserve(slow_log_mutexes_.size());
    for (auto &mtx : slow_log_mutexes_)
    {
        locks.emplace_back(*mtx);
    }
    // Update slow log max length.
    slow_log_max_length_ = len;

    for (size_t core_idx = 0; core_idx < core_num_; ++core_idx)
    {
        auto &slog_log = slow_log_[core_idx];
        // Resize slow log to new length and copy the old entries.
        std::vector<SlowLogEntry> new_slog_log;
        new_slog_log.resize(len);
        int idx = next_slow_log_idx_[core_idx] - slow_log_len_[core_idx];
        if (idx < 0)
        {
            idx = slog_log.size() + idx;
        }
        for (uint32_t i = 0; i < slow_log_len_[core_idx] && i < len; ++i)
        {
            new_slog_log[i] = std::move(slog_log[idx]);
            idx++;
            if (idx >= static_cast<int32_t>(slog_log.size()))
            {
                idx = 0;
            }
        }
        slog_log = std::move(new_slog_log);
        if (slow_log_len_[core_idx] > len)
        {
            slow_log_len_[core_idx] = len;
        }
        if (slow_log_len_[core_idx] == len)
        {
            next_slow_log_idx_[core_idx] = 0;
        }
        else
        {
            next_slow_log_idx_[core_idx] = slow_log_len_[core_idx];
        }

        // unlock the mutex
        locks[core_idx].unlock();
    }
}

bool RedisServiceImpl::AddCommandHandler(const std::string &name,
                                         RedisCommandHandler *handler)
{
    std::string lcname = StringToLowerASCII(name);
    auto it = command_map_.find(lcname);
    if (it != command_map_.end())
    {
        LOG(ERROR) << "Failed to add command name=" << name << ", it exists";
        return false;
    }
    command_map_[lcname] = handler;
    return true;
}

RedisCommandHandler *RedisServiceImpl::FindCommandHandler(
    const butil::StringPiece &name) const
{
    auto it = command_map_.find(name.as_string());
    if (it != command_map_.end())
    {
        return it->second;
    }
    return nullptr;
}

struct ThreadLocal
{
    std::vector<GetCommand> get_pool_;
    std::vector<SetCommand> set_pool_;
    std::vector<LRangeCommand> lrange_pool_;
    std::vector<RPushCommand> rpush_pool_;
    std::vector<IntOpCommand> incr_pool_;
};

void RedisServiceImpl::CollectConnectionsMetrics(brpc::Server &server)
{
    while (!stopping_indicator_.load(std::memory_order_acquire))
    {
        brpc::ServerStatistics srv_status;
        server.GetStat(&srv_status);
        metrics::redis_meter->Collect(metrics::NAME_REDIS_CONNECTION_COUNT,
                                      srv_status.connection_count);

        for (size_t core_idx = 0; core_idx < core_num_; ++core_idx)
        {
            std::lock_guard<bthread::Mutex> slow_log_lk(
                *slow_log_mutexes_[core_idx]);
            metrics::redis_meter->Collect(metrics::NAME_REDIS_SLOW_LOG_LEN,
                                          slow_log_len_[core_idx],
                                          std::to_string(core_idx));
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    LOG(INFO) << "Brpc server metrics collector stopped";
    return;
}

bool RedisServiceImpl::CheckAndUpdateRedisCmdRound(
    RedisCommandType cmd_type) const
{
    auto core_id = bthread::tls_task_group->group_id_;
    auto current_round =
        redis_cmd_current_rounds_[core_id][static_cast<size_t>(cmd_type)]++;
    return (current_round % metrics::collect_redis_command_duration_round) == 0;
}

std::string_view RedisServiceImpl::GetCommandAccessType(
    const std::string_view &cmd_type) const
{
    static std::string_view none_type = "none";
    auto pair = cmd_access_types_.find(cmd_type);
    if (pair != cmd_access_types_.end())
    {
        return pair->second;
    }
    return none_type;
}

void RedisServiceImpl::Subscribe(const std::vector<std::string_view> &chans,
                                 EloqKV::RedisConnectionContext *client)
{
    eloqkv_pub_sub_mgr.Subscribe(chans, client);
}

void RedisServiceImpl::Unsubscribe(const std::vector<std::string_view> &chans,
                                   EloqKV::RedisConnectionContext *client)
{
    eloqkv_pub_sub_mgr.Unsubscribe(chans, client);
}

void RedisServiceImpl::PSubscribe(const std::vector<std::string_view> &patterns,
                                  EloqKV::RedisConnectionContext *client)
{
    eloqkv_pub_sub_mgr.PSubscribe(patterns, client);
}

void RedisServiceImpl::PUnsubscribe(
    const std::vector<std::string_view> &patterns,
    EloqKV::RedisConnectionContext *client)
{
    eloqkv_pub_sub_mgr.PUnsubscribe(patterns, client);
}

int RedisServiceImpl::Publish(std::string_view chan, std::string_view msg)
{
    // publish to remote nodes in cluster mode
    TransactionExecution *txm = NewTxm(txservice::IsolationLevel::ReadCommitted,
                                       txservice::CcProtocol::OccRead);
    PublishTxRequest req(chan, msg, txm);
    SendTxRequestAndWaitResult(txm, &req, nullptr);

    // publish to local clients. only clients that are connected to the same
    // node as the publishing client are included in the count
    return eloqkv_pub_sub_mgr.Publish(chan, msg);
}

metrics::Meter *RedisServiceImpl::GetMeter(std::size_t core_id) const
{
    return tx_service_->CcShards().GetCcShard(core_id)->GetMeter();
};

size_t RedisServiceImpl::MaxConnectionCount() const
{
    auto &ds = DataSubstrate::Instance();
    return ds.GetCoreConfig().maxclients;
}

}  // namespace EloqKV
