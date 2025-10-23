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

#include "data_substrate.h"

#include <gflags/gflags.h>
#include <sys/resource.h>

#include <sstream>
#include <string>

#include "INIReader.h"
#include "log_server.h"
#include "store/data_store_handler.h"
#include "tx_service.h"
#include "sequences/sequences.h"

#ifdef ELOQDS
#include "eloq_data_store_service/data_store_service.h"
#endif


// Data Substrate gflags definitions
DEFINE_string(eloq_data_path, "/tmp/eloq_data", "Data substrate data path");
DEFINE_int32(core_number, 8, "Number of cores for data substrate");
DEFINE_bool(enable_heap_defragment, false, "Enable heap defragmentation");
DEFINE_bool(enable_wal, true, "Enable WAL");
DEFINE_bool(enable_data_store, true, "Enable data store");
DEFINE_bool(enable_io_uring, false, "Enable io_uring as the IO engine");
DEFINE_bool(raft_log_async_fsync, false,
            "Whether raft log fsync is performed asynchronously (blocking the "
            "bthread) instead of blocking the worker thread");
DEFINE_bool(bootstrap, false, "init system tables and exit");
DEFINE_bool(enable_cache_replacement, true, "Enable cache replacement");
DEFINE_bool(enable_mvcc, false, "Enable mvcc");

// Network configuration gflags
DEFINE_string(tx_ip, "127.0.0.1", "Local tx service IP address");
DEFINE_int32(tx_port, 16379, "Local tx service port");
DEFINE_string(ip_port_list, "", "IP list for cluster");
DEFINE_string(standby_ip_port_list, "", "Standby IP list");
DEFINE_string(voter_ip_port_list, "", "Voter IP list");

DEFINE_string(cluster_config_file, "", "Cluster configuration file");
DEFINE_int32(node_group_replica_num, 3, "Node group replica number");
DEFINE_bool(
    bind_all, false,
    "Listen on all interfaces if enabled, otherwise listen on local.ip");
DEFINE_uint32(maxclients, 10000, "maxclients");
DEFINE_string(log_file_name_prefix,
               "eloqdb.log",
               "Sets the prefix for log files. Default is 'eloqdb.log'");
// Declare as weak symbols to allow optional linking
extern txservice::CatalogFactory *eloqsql_catalog_factory __attribute__((weak));
extern txservice::CatalogFactory *eloqkv_catalog_factory __attribute__((weak));
extern txservice::SystemHandler *eloqsql_system_handler __attribute__((weak));

namespace brpc
{
DECLARE_int32(event_dispatcher_num);
}
const auto NUM_VCPU= std::thread::hardware_concurrency();

// Global DataSubstrate instance
std::unique_ptr<DataSubstrate> g_data_substrate= nullptr;

std::unique_ptr<DataSubstrate>
DataSubstrate::Initialize(const std::string &config_file_path)
{
  auto data_substrate= std::make_unique<DataSubstrate>();

  INIReader config_file_reader(config_file_path);
  if (!config_file_path.empty() && config_file_reader.ParseError() != 0)
  {
    LOG(ERROR) << "Failed to parse config file: " << config_file_path;
    return nullptr;
  }
  // Phase 1: Load core and network configuration
  if (!data_substrate->LoadCoreAndNetworkConfig(config_file_reader))
  {
    return nullptr;
  }

  // Phase 2: Register engines
  if (!data_substrate->RegisterEngines())
  {
    return nullptr;
  }

  // Phase 3: Initialize log service
  if (!data_substrate->InitializeLogService(config_file_reader))
  {
    return nullptr;
  }

  // Phase 4: Initialize storage handler
  if (data_substrate->GetCoreConfig().enable_data_store &&
      !data_substrate->InitializeStorageHandler(config_file_reader))
  {
    return nullptr;
  }

  // Phase 5: Initialize metrics
  if (!data_substrate->InitializeMetrics(config_file_reader))
  {
    return nullptr;
  }


  // Phase 6: Initialize TxService
  if (!data_substrate->InitializeTxService(config_file_reader))
  {
    return nullptr;
  }
  return data_substrate;
}

bool DataSubstrate::InitializeGlobal(const std::string &config_file_path)
{
  if (g_data_substrate != nullptr)
  {
    LOG(WARNING) << "Global DataSubstrate already initialized";
    return true;
  }

  g_data_substrate= Initialize(config_file_path);
  if (g_data_substrate == nullptr)
  {
    LOG(ERROR) << "Failed to initialize global DataSubstrate";
    return false;
  }

  LOG(INFO) << "Global DataSubstrate initialized successfully";
  return true;
}

DataSubstrate *DataSubstrate::GetGlobal() { return g_data_substrate.get(); }

DataSubstrate::~DataSubstrate()
{
  // Note: We can't call Shutdown() here because TxService is an incomplete
  // type The actual shutdown will be handled by the component that owns the
  // TxService For now, just reset the pointers
  tx_service_= nullptr;
  store_hd_= nullptr;
#ifdef ELOQDS
  data_store_service_= nullptr;
#endif
  log_server_= nullptr;
  metrics_registry_= nullptr;
}

void DataSubstrate::Shutdown()
{
#ifdef ELOQ_MODULE_ELOQSQL
  system_handler_->Shutdown();
  system_handler_ = nullptr;
#endif
  if (tx_service_ != nullptr)
  {
    LOG(INFO) << "Shutting down the tx service.";
    tx_service_->Shutdown();
    LOG(INFO) << "Tx service shut down.";
  }

  if (store_hd_ != nullptr)
  {
    LOG(INFO) << "Shutting down the storage handler.";
    store_hd_= nullptr; // Wait for all in-fight requests complete.
#if ELOQDS
    if (data_store_service_ != nullptr)
    {
      data_store_service_= nullptr;
    }
#endif
    LOG(INFO) << "Storage handler shut down.";
  }

#if (WITH_LOG_SERVICE)
  if (log_server_ != nullptr)
  {
    LOG(INFO) << "Shutting down the internal logservice.";
    log_server_= nullptr;
    LOG(INFO) << "Internal logservice shut down.";
  }
#endif
  txservice::Sequences::Destory();

#if defined(DATA_STORE_TYPE_DYNAMODB) ||                                      \
    defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_S3) ||                      \
    defined(LOG_STATE_TYPE_RKDB_S3)
  Aws::ShutdownAPI(aws_options_);
#endif

  tx_service_= nullptr;
}

bool DataSubstrate::RegisterEngines()
{
  engines_[0]= {nullptr, txservice::TableEngine::EloqSql, false};
  engines_[1]= {nullptr, txservice::TableEngine::EloqKv, false};
  engines_[2]= {nullptr, txservice::TableEngine::EloqDoc, false};

  prebuilt_tables_.try_emplace(txservice::Sequences::table_name_, std::string(txservice::Sequences::table_name_sv_));
#ifdef ELOQ_MODULE_ELOQSQL
  engines_[0].enable_engine= true;
  if (eloqsql_catalog_factory) {
    engines_[0].catalog_factory= eloqsql_catalog_factory;
  }
  if (eloqsql_system_handler) {
    system_handler_ = eloqsql_system_handler;
  }
#endif

#ifdef ELOQ_MODULE_ELOQKV
  engines_[1].enable_engine= true;
  if (eloqkv_catalog_factory) {
    engines_[1].catalog_factory= eloqkv_catalog_factory;
  }
  for (uint16_t i= 0; i < 16; i++)
  {
    std::string table_name("data_table_");
    table_name.append(std::to_string(i));
    txservice::TableName redis_table_name(std::move(table_name),
                                          txservice::TableType::Primary,
                                          txservice::TableEngine::EloqKv);
    std::string image= redis_table_name.String();
    prebuilt_tables_.try_emplace(redis_table_name, image);
  }
#endif

  return true;
}

bool DataSubstrate::LoadCoreAndNetworkConfig(const INIReader &config_reader)
{
  // Load core data substrate configuration with gflags priority
  core_config_.data_path=
      !CheckCommandLineFlagIsDefault("eloq_data_path")
          ? FLAGS_eloq_data_path
          : config_reader.Get("local", "eloq_data_path", FLAGS_eloq_data_path);

  core_config_.enable_heap_defragment=
      !CheckCommandLineFlagIsDefault("enable_heap_defragment")
          ? FLAGS_enable_heap_defragment
          : config_reader.GetBoolean("local", "enable_heap_defragment",
                                     FLAGS_enable_heap_defragment);

  core_config_.enable_cache_replacement=
      !CheckCommandLineFlagIsDefault("enable_cache_replacement")
          ? FLAGS_enable_cache_replacement
          : config_reader.GetBoolean("local", "enable_cache_replacement",
                                     FLAGS_enable_cache_replacement);

  core_config_.maxclients=
      !CheckCommandLineFlagIsDefault("maxclients")
          ? FLAGS_maxclients
          : config_reader.GetInteger("local", "maxclients", FLAGS_maxclients);
  struct rlimit ulimit;
  ulimit.rlim_cur= FLAGS_maxclients;
  ulimit.rlim_max= FLAGS_maxclients;
  if (setrlimit(RLIMIT_NOFILE, &ulimit) == -1)
  {
    LOG(ERROR) << "Failed to set maxclients.";
    return false;
  }
  core_config_.bootstrap=
      !CheckCommandLineFlagIsDefault("bootstrap")
          ? FLAGS_bootstrap
          : config_reader.GetBoolean("local", "bootstrap", FLAGS_bootstrap);

  core_config_.enable_wal=
      !CheckCommandLineFlagIsDefault("enable_wal")
          ? FLAGS_enable_wal
          : config_reader.GetBoolean("local", "enable_wal", FLAGS_enable_wal);

  core_config_.enable_data_store=
      !CheckCommandLineFlagIsDefault("enable_data_store")
          ? FLAGS_enable_data_store
          : config_reader.GetBoolean("local", "enable_data_store",
                                     FLAGS_enable_data_store);
  if (core_config_.enable_wal && !core_config_.enable_data_store)
  {
    LOG(ERROR) << "When set enable_wal, should also set enable_data_store";
    return false;
  }

  if (!core_config_.enable_data_store && core_config_.enable_cache_replacement)
  {
    LOG(WARNING) << "When set enable_cache_replacement, should also set "
                    "enable_data_store, reset to false";
    core_config_.enable_cache_replacement= false;
  }

  const char *field_core= "core_number";
  core_config_.core_num= FLAGS_core_number;
  if (CheckCommandLineFlagIsDefault(field_core))
  {
    if (config_reader.HasValue("local", field_core))
    {
      core_config_.core_num= config_reader.GetInteger("local", field_core, 0);
      assert(core_config_.core_num);
    }
    else
    {
      if (!NUM_VCPU)
      {
        LOG(ERROR) << "config is missing: " << field_core;
        return false;
      }
      const uint min= 1;
      if (core_config_.enable_data_store)
      {
        core_config_.core_num= std::max(min, (NUM_VCPU * 3) / 5);
        LOG(INFO) << "give cpus to checkpointer " << core_config_.core_num;
      }
      else
      {
        core_config_.core_num= std::max(min, (NUM_VCPU * 7) / 10);
      }
      LOG(INFO) << "config is automatically set: " << field_core << "="
                << core_config_.core_num << ", vcpu=" << NUM_VCPU;
    }
  }

  // Load network and cluster configuration with gflags priority
  network_config_.local_ip=
      !CheckCommandLineFlagIsDefault("tx_ip")
          ? FLAGS_tx_ip
          : config_reader.Get("local", "tx_ip", FLAGS_tx_ip);

  network_config_.local_port=
      !CheckCommandLineFlagIsDefault("tx_port")
          ? FLAGS_tx_port
          : config_reader.GetInteger("local", "tx_port", FLAGS_tx_port);
  std::string local_ip_port= network_config_.local_ip + ":" +
                             std::to_string(network_config_.local_port);
  DLOG(INFO) << "Local tx service ip port: " << local_ip_port;

  network_config_.ip_list=
      !CheckCommandLineFlagIsDefault("ip_port_list")
          ? FLAGS_ip_port_list
          : config_reader.Get("cluster", "ip_port_list", local_ip_port);

  network_config_.standby_ip_list=
      !CheckCommandLineFlagIsDefault("standby_ip_port_list")
          ? FLAGS_standby_ip_port_list
          : config_reader.Get("cluster", "standby_ip_port_list", "");

  network_config_.voter_ip_list=
      !CheckCommandLineFlagIsDefault("voter_ip_port_list")
          ? FLAGS_voter_ip_port_list
          : config_reader.Get("cluster", "voter_ip_port_list", "");

  network_config_.cluster_config_file_path=
      !CheckCommandLineFlagIsDefault("cluster_config_file")
          ? FLAGS_cluster_config_file
          : config_reader.Get("cluster", "cluster_config_file",
                              FLAGS_cluster_config_file);

  network_config_.node_group_replica_num=
      !CheckCommandLineFlagIsDefault("node_group_replica_num")
          ? FLAGS_node_group_replica_num
          : config_reader.GetInteger("cluster", "node_group_replica_num",
                                     FLAGS_node_group_replica_num);

  network_config_.bind_all=
      !CheckCommandLineFlagIsDefault("bind_all")
          ? FLAGS_bind_all
          : config_reader.GetBoolean("local", "bind_all", FLAGS_bind_all);

  core_config_.enable_mvcc=
      !CheckCommandLineFlagIsDefault("enable_mvcc")
          ? FLAGS_enable_mvcc
          : config_reader.GetBoolean("local", "enable_mvcc",
                                     FLAGS_enable_mvcc);

  // Try to read cluster config from file. Cluster config file is written by
  // host manager when cluster config updates.
  if (network_config_.cluster_config_file_path.empty())
  {
    network_config_.cluster_config_file_path=
        core_config_.data_path + "/tx_service/cluster_config";
  }

  network_config_.cluster_config_version= 2;
  if (!txservice::ReadClusterConfigFile(
          network_config_.cluster_config_file_path, network_config_.ng_configs,
          network_config_.cluster_config_version))
  {
    // Read cluster topology from general config file in this case
    auto parse_res= txservice::ParseNgConfig(
        network_config_.ip_list, network_config_.standby_ip_list,
        network_config_.voter_ip_list, network_config_.ng_configs,
        network_config_.node_group_replica_num, 0);
    if (!parse_res)
    {
      LOG(ERROR) << "Failed to extract cluster configs from ip_port_list.";
      return false;
    }
  }

  // print out ng_configs
  for (auto &pair : network_config_.ng_configs)
  {
    DLOG(INFO) << "ng_id: " << pair.first;
    for (auto &node : pair.second)
    {
      DLOG(INFO) << "node_id: " << node.node_id_
                 << ", host_name: " << node.host_name_
                 << ", port: " << node.port_;
    }
  }

  // check whether this node is in cluster.
  bool found= false;
  network_config_.node_id= 0;
  network_config_.native_ng_id= 0;
  for (auto &pair : network_config_.ng_configs)
  {
    auto &ng_nodes= pair.second;
    for (size_t i= 0; i < ng_nodes.size(); i++)
    {
      if (ng_nodes[i].host_name_ == network_config_.local_ip &&
          ng_nodes[i].port_ == network_config_.local_port)
      {
        network_config_.node_id= ng_nodes[i].node_id_;
        found= true;
        if (ng_nodes[i].is_candidate_)
        {
          // found native_ng_id.
          network_config_.native_ng_id= pair.first;
          break;
        }
      }
    }
  }

  if (!found)
  {
    LOG(ERROR) << "!!!!!!!! Current node does not belong to the "
                  "cluster, startup is terminated !!!!!!!!";
    return false;
  }
  const char *field_ed= "event_dispatcher_num";
  uint num_iothreads= brpc::FLAGS_event_dispatcher_num;
  if (CheckCommandLineFlagIsDefault(field_ed))
  {
    if (config_reader.HasValue("local", field_ed))
    {
      num_iothreads= config_reader.GetInteger("local", field_ed, 0);
      assert(num_iothreads);
    }
    else
    {
      if (!NUM_VCPU)
      {
        LOG(ERROR) << "config is missing: " << field_ed;
        return false;
      }
      num_iothreads= std::max(uint(1), (core_config_.core_num + 5) / 6);
      LOG(INFO) << "config is automatically set: " << field_ed << "="
                << num_iothreads << ", vcpu=" << NUM_VCPU;
    }
  }
  GFLAGS_NAMESPACE::SetCommandLineOption(
      field_ed, std::to_string(num_iothreads).c_str());

  // Set bthread concurrency
  GFLAGS_NAMESPACE::SetCommandLineOption(
      "bthread_concurrency", std::to_string(core_config_.core_num).c_str());
#ifdef ELOQ_MODULE_ENABLED
  GFLAGS_NAMESPACE::SetCommandLineOption("worker_polling_time_us", "1000");
  GFLAGS_NAMESPACE::SetCommandLineOption("brpc_worker_as_ext_processor",
                                         "true");
#endif
  GFLAGS_NAMESPACE::SetCommandLineOption("use_pthread_event_dispatcher",
                                         "true");
  GFLAGS_NAMESPACE::SetCommandLineOption("max_body_size", "536870912");
  GFLAGS_NAMESPACE::SetCommandLineOption("graceful_quit_on_sigterm", "true");

  bool enable_io_uring=
      !CheckCommandLineFlagIsDefault("enable_io_uring")
          ? FLAGS_enable_io_uring
          : config_reader.GetBoolean("local", "enable_io_uring",
                                     FLAGS_enable_io_uring);

  bool raft_log_async_fsync=
      !CheckCommandLineFlagIsDefault("raft_log_async_fsync")
          ? FLAGS_raft_log_async_fsync
          : config_reader.GetBoolean("local", "raft_log_async_fsync",
                                     FLAGS_raft_log_async_fsync);
  if (raft_log_async_fsync && !enable_io_uring)
  {
    LOG(ERROR) << "Invalid config: when set `enable_io_uring`, "
                  "should also set `enable_io_uring`.";
    return false;
  }
  GFLAGS_NAMESPACE::SetCommandLineOption("use_io_uring",
                                         enable_io_uring ? "true" : "false");
  GFLAGS_NAMESPACE::SetCommandLineOption(
      "raft_use_bthread_fsync", raft_log_async_fsync ? "true" : "false");
#if defined(DATA_STORE_TYPE_DYNAMODB) ||                                      \
    defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_S3) ||                      \
    defined(LOG_STATE_TYPE_RKDB_S3)
  aws_options_.loggingOptions.logLevel= Aws::Utils::Logging::LogLevel::Info;
  Aws::InitAPI(aws_options_);
#endif

  return true;
}