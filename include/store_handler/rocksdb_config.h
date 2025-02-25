#pragma once

#include <string>

#include "INIReader.h"
#include "store_handler/kv_store.h"

#if KV_DATA_STORE_TYPE == KV_DATA_STORE_TYPE_ROCKSDB

#if ROCKSDB_CLOUD_FS()
#include "rocksdb/cloud/db_cloud.h"
#endif

namespace EloqShare
{
struct RocksDBConfig
{
    explicit RocksDBConfig(const INIReader &config_reader,
                           const std::string &eloq_data_path);
    RocksDBConfig(const RocksDBConfig &) = default;

    std::string info_log_level_;
    bool enable_stats_;
    size_t stats_dump_period_sec_;
    std::string storage_path_;
    size_t max_write_buffer_number_;
    size_t max_background_jobs_;
    size_t max_background_flush_;
    size_t max_background_compaction_;
    size_t target_file_size_base_bytes_;
    size_t target_file_size_multiplier_;
    size_t write_buffer_size_bytes_;
    bool use_direct_io_for_flush_and_compaction_;
    bool use_direct_io_for_read_;
    size_t level0_stop_writes_trigger_;
    size_t level0_slowdown_writes_trigger_;
    size_t level0_file_num_compaction_trigger_;
    size_t max_bytes_for_level_base_bytes_;
    size_t max_bytes_for_level_multiplier_;
    std::string compaction_style_;
    size_t soft_pending_compaction_bytes_limit_bytes_;
    size_t hard_pending_compaction_bytes_limit_bytes_;
    size_t max_subcompactions_;
    size_t write_rate_limit_bytes_;
    size_t query_worker_num_;
    size_t batch_write_size_;
    size_t periodic_compaction_seconds_;
    std::string dialy_offpeak_time_utc_;
    size_t snapshot_sync_worker_num_;
};

#if ROCKSDB_CLOUD_FS()

struct RocksDBCloudConfig
{
    explicit RocksDBCloudConfig(const INIReader &config);

    RocksDBCloudConfig(const RocksDBCloudConfig &) = default;

    std::string aws_access_key_id_;
    std::string aws_secret_key_;
    std::string bucket_name_;
    std::string bucket_prefix_;
    std::string region_;
    uint64_t sst_file_cache_size_;
    uint32_t db_ready_timeout_us_;
    uint32_t db_file_deletion_delay_;
    std::string s3_endpoint_url_;
    size_t warm_up_thread_num_;
};

inline rocksdb::Status NewCloudFileSystem(
    const rocksdb::CloudFileSystemOptions &cfs_options,
    rocksdb::CloudFileSystem **cfs)
{
    rocksdb::Status status;
    // Create a cloud file system
#if (ROCKSDB_CLOUD_FS_TYPE == ROCKSDB_CLOUD_FS_TYPE_S3)
    // AWS s3 file system
    status = rocksdb::CloudFileSystemEnv::NewAwsFileSystem(
        rocksdb::FileSystem::Default(), cfs_options, nullptr, cfs);
#elif (ROCKSDB_CLOUD_FS_TYPE == ROCKSDB_CLOUD_FS_TYPE_GCS)
    // Google cloud storage file system
    status = rocksdb::CloudFileSystemEnv::NewGcpFileSystem(
        rocksdb::FileSystem::Default(), cfs_options, nullptr, cfs);
#endif
    return status;
};
#endif
}  // namespace EloqShare
#endif
