#include "INIReader.h"
#include "data_substrate.h"
#include "log_server.h"
#include "log_utils.h"
#include "sharder.h"
#if defined(LOG_STATE_TYPE_RKDB_CLOUD)
#include "rocksdb_cloud_config.h"
#endif

DEFINE_string(txlog_service_list, "", "Log group servers configuration");
DEFINE_int32(txlog_group_replica_num, 3, "Replica number of one log group");
DEFINE_string(log_service_data_path, "", "path for log_service data");

DEFINE_string(txlog_rocksdb_storage_path,
              "",
              "Storage path for txlog rocksdb state");
DEFINE_uint32(txlog_rocksdb_max_write_buffer_number,
              8,
              "Max write buffer number");
DEFINE_string(txlog_rocksdb_sst_files_size_limit,
              "500MB",
              "The total RocksDB sst files size before purge");
DEFINE_uint32(txlog_rocksdb_scan_threads,
              1,
              "The number of rocksdb scan threads");

DEFINE_uint32(txlog_rocksdb_max_background_jobs, 12, "Max background jobs");
DEFINE_string(txlog_rocksdb_target_file_size_base,
              "64MB",
              "Target file size base for rocksdb");

DEFINE_uint32(logserver_snapshot_interval, 600, "logserver_snapshot interval");

DEFINE_bool(enable_txlog_request_checkpoint,
            true,
            "Enable txlog server sending checkpoint requests when the criteria "
            "are met.");

#if defined(LOG_STATE_TYPE_RKDB_CLOUD)
DEFINE_string(txlog_rocksdb_cloud_region,
              "ap-northeast-1",
              "Cloud service regin");
DEFINE_string(txlog_rocksdb_cloud_bucket_name,
              "rocksdb-cloud-test",
              "Cloud storage bucket name");
DEFINE_string(txlog_rocksdb_cloud_bucket_prefix,
              "txlog-",
              "Cloud storage bucket prefix");
DEFINE_string(txlog_rocksdb_cloud_object_path,
              "eloqkv_txlog",
              "Cloud storage object path, if not set, will use bucket name and "
              "prefix");
DEFINE_uint32(
    txlog_rocksdb_cloud_ready_timeout,
    10,
    "Timeout before rocksdb cloud becoming ready on new log group leader");
DEFINE_uint32(txlog_rocksdb_cloud_file_deletion_delay,
              3600,
              "The file deletion delay for rocksdb cloud file");
DEFINE_uint32(txlog_rocksdb_cloud_log_retention_days,
              90,
              "The number of days for which logs should be retained");
DEFINE_string(txlog_rocksdb_cloud_log_purger_schedule,
              "00:00:01",
              "Time (in regular format: HH:MM:SS) to run log purger daily, "
              "deleting logs older than log_retention_days.");
DEFINE_uint32(txlog_in_mem_data_log_queue_size_high_watermark,
              50 * 10000,
              "In memory data log queue max size");
DEFINE_string(txlog_rocksdb_cloud_endpoint_url,
              "",
              "Endpoint url of cloud storage service");
DEFINE_string(txlog_rocksdb_cloud_sst_file_cache_size,
              "1GB",
              "Local sst cache size for txlog");
DEFINE_uint32(txlog_rocksdb_cloud_sst_file_cache_num_shard_bits,
              5,
              "TxLog RocksDB Cloud SST file cache num shard bits");
#endif
#ifdef WITH_CLOUD_AZ_INFO
DEFINE_string(txlog_rocksdb_cloud_prefer_zone,
              "",
              "user preferred deployed availability zone");
DEFINE_string(txlog_rocksdb_cloud_current_zone,
              "",
              "the log service server node deployed on currently");
#endif

DEFINE_uint32(check_replay_log_size_interval_sec,
              10,
              "The interval for checking replay log size.");

DEFINE_string(notify_checkpointer_threshold_size,
              "1GB",
              "When the replay log size reaches this threshold the txlog "
              "server sends a checkpoint request to tx_service.");


bool DataSubstrate::InitializeLogService(const INIReader &config_reader)
{
    std::string txlog_service =
        !CheckCommandLineFlagIsDefault("txlog_service_list")
            ? FLAGS_txlog_service_list
            : config_reader.GetString(
                  "cluster", "txlog_service_list", FLAGS_txlog_service_list);
    int txlog_group_replica_num =
        !CheckCommandLineFlagIsDefault("txlog_group_replica_num")
            ? FLAGS_txlog_group_replica_num
            : config_reader.GetInteger("cluster",
                                       "txlog_group_replica_num",
                                       FLAGS_txlog_group_replica_num);

    std::string log_service_data_path =
        !CheckCommandLineFlagIsDefault("log_service_data_path")
            ? FLAGS_log_service_data_path
            : config_reader.GetString("local", "log_service_data_path", "");
    std::string log_path("local://");
    if (log_service_data_path.empty())
    {
        log_path.append(core_config_.data_path);
    }
    else
    {
        log_path.append(log_service_data_path);
    }
    log_path.append("/log_service");
    // parse standalone txlog_service_list
    bool is_standalone_txlog_service = false;
    std::vector<std::string> &txlog_ips = log_service_config_.txlog_ips;
    std::vector<uint16_t> &txlog_ports = log_service_config_.txlog_ports;
    if (!txlog_service.empty())
    {
        is_standalone_txlog_service = true;
        std::string token;
        std::istringstream txlog_ip_port_list_stream(txlog_service);
        while (std::getline(txlog_ip_port_list_stream, token, ','))
        {
            size_t c_idx = token.find_first_of(':');
            if (c_idx != std::string::npos)
            {
                txlog_ips.emplace_back(token.substr(0, c_idx));
                uint16_t pt = std::stoi(token.substr(c_idx + 1));
                txlog_ports.emplace_back(pt);
            }
        }
    }
#if defined(WITH_LOG_SERVICE)
    else
    {
        uint32_t txlog_node_id = 0;
        uint32_t next_txlog_node_id = 0;
        uint16_t log_server_port = network_config_.local_port + 2;
        std::unordered_set<uint32_t> tx_node_ids;
        for (uint32_t ng = 0; ng < network_config_.ng_configs.size(); ng++)
        {
            for (uint32_t nidx = 0;
                 nidx < network_config_.ng_configs[ng].size();
                 nidx++)
            {
                if (tx_node_ids.count(
                        network_config_.ng_configs[ng][nidx].node_id_) == 0)
                {
                    tx_node_ids.insert(
                        network_config_.ng_configs[ng][nidx].node_id_);
                    if (network_config_.ng_configs[ng][nidx].port_ ==
                            network_config_.local_port &&
                        network_config_.ng_configs[ng][nidx].host_name_ ==
                            network_config_.local_ip)
                    {
                        // is local node, set txlog_node_id
                        txlog_node_id = next_txlog_node_id;
                    }
                    next_txlog_node_id++;
                    txlog_ports.emplace_back(
                        network_config_.ng_configs[ng][nidx].port_ + 2);
                    txlog_ips.emplace_back(
                        network_config_.ng_configs[ng][nidx].host_name_);
                }
            }
        }

        // Init local txlog service if it is not standalone txlog service
        // Init before store handler so they can init in parallel
        if (!is_standalone_txlog_service)
        {
            if (txlog_ips.empty() || txlog_ports.empty())
            {
                LOG(ERROR)
                    << "WAL is enabled but `txlog_service_list` is empty and "
                       "built-in log server initialization failed, "
                       "unable to proceed.";
                return false;
            }

            [[maybe_unused]] size_t txlog_rocksdb_scan_threads =
                !CheckCommandLineFlagIsDefault("txlog_rocksdb_scan_threads")
                    ? FLAGS_txlog_rocksdb_scan_threads
                    : config_reader.GetInteger(
                          "local",
                          "txlog_rocksdb_scan_threads",
                          FLAGS_txlog_rocksdb_scan_threads);

            size_t txlog_rocksdb_max_write_buffer_number =
                !CheckCommandLineFlagIsDefault(
                    "txlog_rocksdb_max_write_buffer_number")
                    ? FLAGS_txlog_rocksdb_max_write_buffer_number
                    : config_reader.GetInteger(
                          "local",
                          "txlog_rocksdb_max_write_buffer_number",
                          FLAGS_txlog_rocksdb_max_write_buffer_number);

            size_t txlog_rocksdb_max_background_jobs =
                !CheckCommandLineFlagIsDefault(
                    "txlog_rocksdb_max_background_jobs")
                    ? FLAGS_txlog_rocksdb_max_background_jobs
                    : config_reader.GetInteger(
                          "local",
                          "txlog_rocksdb_max_background_jobs",
                          FLAGS_txlog_rocksdb_max_background_jobs);

            size_t txlog_rocksdb_target_file_size_base_val =
                !CheckCommandLineFlagIsDefault(
                    "txlog_rocksdb_target_file_size_base")
                    ? txlog::parse_size(
                          FLAGS_txlog_rocksdb_target_file_size_base)
                    : txlog::parse_size(config_reader.GetString(
                          "local",
                          "txlog_rocksdb_target_file_size_base",
                          FLAGS_txlog_rocksdb_target_file_size_base));

            [[maybe_unused]] size_t logserver_snapshot_interval =
                !CheckCommandLineFlagIsDefault("logserver_snapshot_interval")
                    ? FLAGS_logserver_snapshot_interval
                    : config_reader.GetInteger(
                          "local",
                          "logserver_snapshot_interval",
                          FLAGS_logserver_snapshot_interval);

            [[maybe_unused]] bool enable_txlog_request_checkpoint =
                !CheckCommandLineFlagIsDefault(
                    "enable_txlog_request_checkpoint")
                    ? FLAGS_enable_txlog_request_checkpoint
                    : config_reader.GetBoolean(
                          "local",
                          "enable_txlog_request_checkpoint",
                          FLAGS_enable_txlog_request_checkpoint);

            [[maybe_unused]] size_t check_replay_log_size_interval_sec =
                !CheckCommandLineFlagIsDefault(
                    "check_replay_log_size_interval_sec")
                    ? FLAGS_check_replay_log_size_interval_sec
                    : config_reader.GetInteger(
                          "local",
                          "check_replay_log_size_interval_sec",
                          FLAGS_check_replay_log_size_interval_sec);

            [[maybe_unused]] size_t notify_checkpointer_threshold_size_val =
                !CheckCommandLineFlagIsDefault(
                    "notify_checkpointer_threshold_size")
                    ? txlog::parse_size(
                          FLAGS_notify_checkpointer_threshold_size)
                    : txlog::parse_size(config_reader.GetString(
                          "local",
                          "notify_checkpointer_threshold_size",
                          FLAGS_notify_checkpointer_threshold_size));

#if defined(LOG_STATE_TYPE_RKDB_ALL)
            std::string txlog_rocksdb_storage_path =
                !CheckCommandLineFlagIsDefault("txlog_rocksdb_storage_path")
                    ? FLAGS_txlog_rocksdb_storage_path
                    : config_reader.GetString(
                          "local", "txlog_rocksdb_storage_path", "");
            if (txlog_rocksdb_storage_path.empty())
            {
                // remove "local://" prefix from log_path
                txlog_rocksdb_storage_path = log_path.substr(8) + "/rocksdb";
            }

#if defined(LOG_STATE_TYPE_RKDB_CLOUD)
            txlog::RocksDBCloudConfig txlog_rocksdb_cloud_config;
#if defined(LOG_STATE_TYPE_RKDB_S3)
            txlog_rocksdb_cloud_config.aws_access_key_id_ =
                !CheckCommandLineFlagIsDefault("aws_access_key_id")
                    ? FLAGS_aws_access_key_id
                    : config_reader.GetString("store", "aws_access_key_id", "");
            txlog_rocksdb_cloud_config.aws_secret_key_ =
                !CheckCommandLineFlagIsDefault("aws_secret_key")
                    ? FLAGS_aws_secret_key
                    : config_reader.GetString("store", "aws_secret_key", "");
#endif /* LOG_STATE_TYPE_RKDB_S3 */
            txlog_rocksdb_cloud_config.endpoint_url_ =
                !CheckCommandLineFlagIsDefault(
                    "txlog_rocksdb_cloud_endpoint_url")
                    ? FLAGS_txlog_rocksdb_cloud_endpoint_url
                    : config_reader.GetString(
                          "local",
                          "txlog_rocksdb_cloud_endpoint_url",
                          FLAGS_txlog_rocksdb_cloud_endpoint_url);
            txlog_rocksdb_cloud_config.bucket_name_ =
                !CheckCommandLineFlagIsDefault(
                    "txlog_rocksdb_cloud_bucket_name")
                    ? FLAGS_txlog_rocksdb_cloud_bucket_name
                    : config_reader.GetString(
                          "local",
                          "txlog_rocksdb_cloud_bucket_name",
                          FLAGS_txlog_rocksdb_cloud_bucket_name);
            txlog_rocksdb_cloud_config.bucket_prefix_ =
                !CheckCommandLineFlagIsDefault(
                    "txlog_rocksdb_cloud_bucket_prefix")
                    ? FLAGS_txlog_rocksdb_cloud_bucket_prefix
                    : config_reader.GetString(
                          "local",
                          "txlog_rocksdb_cloud_bucket_prefix",
                          FLAGS_txlog_rocksdb_cloud_bucket_prefix);
            txlog_rocksdb_cloud_config.object_path_ =
                !CheckCommandLineFlagIsDefault(
                    "txlog_rocksdb_cloud_object_path")
                    ? FLAGS_txlog_rocksdb_cloud_object_path
                    : config_reader.GetString(
                          "local",
                          "txlog_rocksdb_cloud_object_path",
                          FLAGS_txlog_rocksdb_cloud_object_path);
            txlog_rocksdb_cloud_config.region_ =
                !CheckCommandLineFlagIsDefault("txlog_rocksdb_cloud_region")
                    ? FLAGS_txlog_rocksdb_cloud_region
                    : config_reader.GetString("local",
                                              "txlog_rocksdb_cloud_region",
                                              FLAGS_txlog_rocksdb_cloud_region);
            uint32_t db_ready_timeout_us =
                !CheckCommandLineFlagIsDefault(
                    "txlog_rocksdb_cloud_ready_timeout")
                    ? FLAGS_txlog_rocksdb_cloud_ready_timeout
                    : config_reader.GetInteger(
                          "local",
                          "txlog_rocksdb_cloud_ready_timeout",
                          FLAGS_txlog_rocksdb_cloud_ready_timeout);
            txlog_rocksdb_cloud_config.db_ready_timeout_us_ =
                db_ready_timeout_us * 1000 * 1000;
            txlog_rocksdb_cloud_config.db_file_deletion_delay_ =
                !CheckCommandLineFlagIsDefault(
                    "txlog_rocksdb_cloud_file_deletion_delay")
                    ? FLAGS_txlog_rocksdb_cloud_file_deletion_delay
                    : config_reader.GetInteger(
                          "local",
                          "txlog_rocksdb_cloud_file_deletion_delay",
                          FLAGS_txlog_rocksdb_cloud_file_deletion_delay);
            txlog_rocksdb_cloud_config.log_retention_days_ =
                !CheckCommandLineFlagIsDefault(
                    "txlog_rocksdb_cloud_log_retention_days")
                    ? FLAGS_txlog_rocksdb_cloud_log_retention_days
                    : config_reader.GetInteger(
                          "local",
                          "txlog_rocksdb_cloud_log_retention_days",
                          FLAGS_txlog_rocksdb_cloud_log_retention_days);
            txlog_rocksdb_cloud_config.sst_file_cache_num_shard_bits_ =
                !CheckCommandLineFlagIsDefault(
                    "txlog_rocksdb_cloud_sst_file_cache_num_shard_bits")
                    ? FLAGS_txlog_rocksdb_cloud_sst_file_cache_num_shard_bits
                    : config_reader.GetInteger(
                          "local",
                          "txlog_rocksdb_cloud_sst_file_cache_num_shard_bits",
                          FLAGS_txlog_rocksdb_cloud_sst_file_cache_num_shard_bits);
            txlog_rocksdb_cloud_config.sst_file_cache_size_ =
                !CheckCommandLineFlagIsDefault(
                    "txlog_rocksdb_cloud_sst_file_cache_size")
                    ? txlog::parse_size(
                          FLAGS_txlog_rocksdb_cloud_sst_file_cache_size)
                    : txlog::parse_size(config_reader.GetString(
                          "local",
                          "txlog_rocksdb_cloud_sst_file_cache_size",
                          FLAGS_txlog_rocksdb_cloud_sst_file_cache_size));
            std::tm log_purger_tm{};
            std::string log_purger_schedule =
                !CheckCommandLineFlagIsDefault(
                    "txlog_rocksdb_cloud_log_purger_schedule")
                    ? FLAGS_txlog_rocksdb_cloud_log_purger_schedule
                    : config_reader.GetString(
                          "local",
                          "txlog_rocksdb_cloud_log_purger_schedule",
                          FLAGS_txlog_rocksdb_cloud_log_purger_schedule);
            std::istringstream iss(log_purger_schedule);
            iss >> std::get_time(&log_purger_tm, "%H:%M:%S");

            if (iss.fail())
            {
                LOG(ERROR) << "Invalid time format." << std::endl;
            }
            else
            {
                txlog_rocksdb_cloud_config.log_purger_starting_hour_ =
                    log_purger_tm.tm_hour;
                txlog_rocksdb_cloud_config.log_purger_starting_minute_ =
                    log_purger_tm.tm_min;
                txlog_rocksdb_cloud_config.log_purger_starting_second_ =
                    log_purger_tm.tm_sec;
            }
            if (core_config_.bootstrap)
            {
                log_server_ = std::make_unique<::txlog::LogServer>(
                    txlog_node_id,
                    log_server_port,
                    txlog_ips,
                    txlog_ports,
                    log_path,
                    0,
                    txlog_group_replica_num,
#ifdef WITH_CLOUD_AZ_INFO
                    FLAGS_txlog_rocksdb_cloud_prefer_zone,
                    FLAGS_txlog_rocksdb_cloud_current_zone,
#endif
                    txlog_rocksdb_storage_path,
                    txlog_rocksdb_scan_threads,
                    txlog_rocksdb_cloud_config,
                    FLAGS_txlog_in_mem_data_log_queue_size_high_watermark,
                    txlog_rocksdb_max_write_buffer_number,
                    txlog_rocksdb_max_background_jobs,
                    txlog_rocksdb_target_file_size_base_val,
                    logserver_snapshot_interval);
            }
            else
            {
                log_server_ = std::make_unique<::txlog::LogServer>(
                    txlog_node_id,
                    log_server_port,
                    txlog_ips,
                    txlog_ports,
                    log_path,
                    0,
                    txlog_group_replica_num,
#ifdef WITH_CLOUD_AZ_INFO
                    FLAGS_txlog_rocksdb_cloud_prefer_zone,
                    FLAGS_txlog_rocksdb_cloud_current_zone,
#endif
                    txlog_rocksdb_storage_path,
                    txlog_rocksdb_scan_threads,
                    txlog_rocksdb_cloud_config,
                    FLAGS_txlog_in_mem_data_log_queue_size_high_watermark,
                    txlog_rocksdb_max_write_buffer_number,
                    txlog_rocksdb_max_background_jobs,
                    txlog_rocksdb_target_file_size_base_val,
                    logserver_snapshot_interval,
                    enable_txlog_request_checkpoint,
                    check_replay_log_size_interval_sec,
                    notify_checkpointer_threshold_size_val);
            }
#else
            size_t txlog_rocksdb_sst_files_size_limit_val =
                !CheckCommandLineFlagIsDefault(
                    "txlog_rocksdb_sst_files_size_limit")
                    ? txlog::parse_size(
                          FLAGS_txlog_rocksdb_sst_files_size_limit)
                    : txlog::parse_size(config_reader.GetString(
                          "local",
                          "txlog_rocksdb_sst_files_size_limit",
                          FLAGS_txlog_rocksdb_sst_files_size_limit));

            // Start internal logserver.
#if defined(OPEN_LOG_SERVICE)
            if (core_config_.bootstrap)
            {
                log_server_ = std::make_unique<::txlog::LogServer>(
                    txlog_node_id,
                    log_server_port,
                    log_path,
                    1,
                    txlog_rocksdb_sst_files_size_limit_val,
                    txlog_rocksdb_max_write_buffer_number,
                    txlog_rocksdb_max_background_jobs,
                    txlog_rocksdb_target_file_size_base_val);
            }
            else
            {
                log_server_ = std::make_unique<::txlog::LogServer>(
                    txlog_node_id,
                    log_server_port,
                    log_path,
                    1,
                    txlog_rocksdb_sst_files_size_limit_val,
                    txlog_rocksdb_max_write_buffer_number,
                    txlog_rocksdb_max_background_jobs,
                    txlog_rocksdb_target_file_size_base_val);
            }
#else
            if (core_config_.bootstrap)
            {
                log_server_ = std::make_unique<::txlog::LogServer>(
                    txlog_node_id,
                    log_server_port,
                    txlog_ips,
                    txlog_ports,
                    log_path,
                    0,
                    txlog_group_replica_num,
                    txlog_rocksdb_storage_path,
                    txlog_rocksdb_scan_threads,
                    txlog_rocksdb_sst_files_size_limit_val,
                    txlog_rocksdb_max_write_buffer_number,
                    txlog_rocksdb_max_background_jobs,
                    txlog_rocksdb_target_file_size_base_val,
                    logserver_snapshot_interval);
            }
            else
            {
                log_server_ = std::make_unique<::txlog::LogServer>(
                    txlog_node_id,
                    log_server_port,
                    txlog_ips,
                    txlog_ports,
                    log_path,
                    0,
                    txlog_group_replica_num,
                    txlog_rocksdb_storage_path,
                    txlog_rocksdb_scan_threads,
                    txlog_rocksdb_sst_files_size_limit_val,
                    txlog_rocksdb_max_write_buffer_number,
                    txlog_rocksdb_max_background_jobs,
                    txlog_rocksdb_target_file_size_base_val,
                    logserver_snapshot_interval,
                    enable_txlog_request_checkpoint,
                    check_replay_log_size_interval_sec,
                    notify_checkpointer_threshold_size_val);
            }
#endif
#endif
#endif
            DLOG(INFO) << "Log server started, node_id: " << txlog_node_id
                       << ", log_server_port: " << log_server_port
                       << ", txlog_group_replica_num: "
                       << txlog_group_replica_num << ", log_path: " << log_path;
            int err = log_server_->Start();

            if (err != 0)
            {
                LOG(ERROR) << "Failed to start the log service.";
                return false;
            }
        }
    }
#endif
    return true;
}
