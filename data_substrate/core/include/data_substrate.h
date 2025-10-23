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

#include <gflags/gflags.h>

#include <cassert>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#if (defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_S3) ||                      \
     defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_GCS) ||                     \
     defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB) ||                               \
     defined(DATA_STORE_TYPE_ELOQDSS_ELOQSTORE))
#define ELOQDS 1
#endif

// Log state type
#if !defined(LOG_STATE_TYPE_RKDB_CLOUD)

// Only if LOG_STATE_TYPE_RKDB_CLOUD undefined
#if ((defined(LOG_STATE_TYPE_RKDB_S3) || defined(LOG_STATE_TYPE_RKDB_GCS)) &&  \
     !defined(LOG_STATE_TYPE_RKDB))
#define LOG_STATE_TYPE_RKDB_CLOUD 1
#endif

#endif

#if !defined(LOG_STATE_TYPE_RKDB_ALL)

// Only if LOG_STATE_TYPE_RKDB_ALL undefined
#if (defined(LOG_STATE_TYPE_RKDB_S3) || defined(LOG_STATE_TYPE_RKDB_GCS) ||    \
     defined(LOG_STATE_TYPE_RKDB))
#define LOG_STATE_TYPE_RKDB_ALL 1
#endif

#endif

#if defined(DATA_STORE_TYPE_DYNAMODB) ||                                       \
    (ROCKSDB_CLOUD_FS_TYPE == ROCKSDB_CLOUD_FS_TYPE_S3)
#include <aws/core/Aws.h>
#endif
#include "metrics.h"
#include "meter.h"
#include "type.h"

// Forward declaration for INIReader
class INIReader;

// Forward declarations
namespace txservice
{
class TxService;
struct NodeConfig;
class CatalogFactory;
class SystemHandler;
namespace store
{
class DataStoreHandler;
}
}  // namespace txservice

#ifdef ELOQDS
namespace EloqDS
{
class DataStoreService;
}  // namespace EloqDS
#endif

namespace txlog
{
class LogServer;
class LogAgent;
}  // namespace txlog

namespace metrics
{
class MetricsRegistry;
}  // namespace metrics

class DataSubstrate
{
public:
    struct EngineConfig
    {
        txservice::CatalogFactory *catalog_factory;
        txservice::TableEngine engine;
        bool enable_engine;
    };

    // Core data substrate configuration
    struct CoreConfig
    {
        std::string data_path;
        uint32_t core_num;
        bool enable_heap_defragment;
        bool enable_wal;
        bool enable_data_store;
        bool bootstrap;
        bool enable_cache_replacement;
        bool enable_mvcc;
        uint32_t maxclients;
    };

    // Network and cluster configuration
    struct NetworkConfig
    {
        std::string local_ip;
        uint32_t local_port;
        std::string ip_list;
        std::string standby_ip_list;
        std::string voter_ip_list;
        uint32_t node_group_replica_num;
        bool bind_all;
        // Parsed service lists
        std::unordered_map<uint32_t, std::vector<txservice::NodeConfig>>
            ng_configs;
        uint64_t cluster_config_version;
        uint32_t node_id;
        uint32_t native_ng_id;
        std::string cluster_config_file_path;
    };

    struct LogServiceConfig
    {
        std::vector<std::string> txlog_ips;
        std::vector<uint16_t> txlog_ports;
    };

    static std::unique_ptr<DataSubstrate> Initialize(
        const std::string &config_file_path);

    // Global initialization function
    static bool InitializeGlobal(const std::string &config_file_path);

    // Get global DataSubstrate instance
    static DataSubstrate *GetGlobal();

    // Shutdown DataSubstrate instance
    void Shutdown();

    // Component accessors
    txservice::TxService *GetTxService() const
    {
        return tx_service_.get();
    }
    txservice::store::DataStoreHandler *GetStoreHandler() const
    {
        return store_hd_.get();
    }
    txlog::LogServer *GetLogServer() const
    {
        return log_server_.get();
    }
    metrics::MetricsRegistry *GetMetricsRegistry() const
    {
        return metrics_registry_.get();
    }
    // Configuration accessors
    const CoreConfig &GetCoreConfig() const
    {
        return core_config_;
    }
    const NetworkConfig &GetNetworkConfig() const
    {
        return network_config_;
    }

    ~DataSubstrate();

    // Constructor (public for static factory method)
    DataSubstrate() = default;

private:
    // Private methods that call component initialization files
    bool LoadCoreAndNetworkConfig(const INIReader &config_file_reader);
    bool InitializeStorageHandler(const INIReader &config_file_reader);
    bool InitializeLogService(const INIReader &config_file_reader);
    bool InitializeTxService(const INIReader &config_file_reader);
    bool InitializeMetrics(const INIReader &config_file_reader);
    bool RegisterEngines();

    // Configuration storage
    CoreConfig core_config_;
    NetworkConfig network_config_;
    LogServiceConfig log_service_config_;
    metrics::CommonLabels tx_service_common_labels_{};
    std::vector<std::tuple<metrics::Name,
                           metrics::Type,
                           std::vector<metrics::LabelGroup>>>
        external_metrics = {};
    // TODO(liunyl): system handler is used to refresh auth related cache. In
    // converged db, there should only be one system handler.
    txservice::SystemHandler *system_handler_{nullptr};

    // Component instances
    std::unique_ptr<txservice::TxService> tx_service_;
#if defined(DATA_STORE_TYPE_DYNAMODB) ||                                       \
    defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_S3) ||                       \
    defined(LOG_STATE_TYPE_RKDB_S3)
    Aws::SDKOptions aws_options_;
#endif

    std::unique_ptr<txservice::store::DataStoreHandler> store_hd_;
#ifdef ELOQDS
    std::unique_ptr<EloqDS::DataStoreService> data_store_service_;
#endif
    std::unique_ptr<txlog::LogServer> log_server_;
    std::unique_ptr<metrics::MetricsRegistry> metrics_registry_{nullptr};

    // Engine registry
    EngineConfig engines_[NUM_EXTERNAL_ENGINES];

    std::unordered_map<txservice::TableName, std::string> prebuilt_tables_;
};

// Helper function to check if a gflag was set from command line
static inline bool CheckCommandLineFlagIsDefault(const char *name)
{
    gflags::CommandLineFlagInfo flag_info;
    bool flag_found = gflags::GetCommandLineFlagInfo(name, &flag_info);
    // Make sure the flag is declared.
    assert(flag_found);
    (void) flag_found;

    // Return `true` if the flag has the default value and has not been set
    // explicitly from the cmdline or via SetCommandLineOption
    return flag_info.is_default;
}