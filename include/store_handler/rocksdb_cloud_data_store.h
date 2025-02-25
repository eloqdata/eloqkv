#pragma once

#include <rocksdb/db.h>

#include <atomic>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "data_store.h"
#include "data_store_service.h"
#include "rocksdb_config.h"
#include "store_util.h"

namespace EloqDS
{
#if ROCKSDB_CLOUD_FS()

/**
 * @brief Wrapper class for cache iterator with TTL at the data store service to
 * avoid creating iterator for each scan request from client.
 */
class RocksDBIteratorTTLWrapper : public TTLWrapper
{
public:
    explicit RocksDBIteratorTTLWrapper(rocksdb::Iterator *iter) : iter_(iter)
    {
    }

    ~RocksDBIteratorTTLWrapper()
    {
        delete iter_;
    }

    rocksdb::Iterator *GetIter()
    {
        return iter_;
    }

private:
    rocksdb::Iterator *iter_;
};

class RocksDBCloudDataStore : public DataStore
{
public:
    RocksDBCloudDataStore(const EloqShare::RocksDBCloudConfig &cloud_config,
                          const EloqShare::RocksDBConfig &config,
                          bool create_if_missing,
                          bool tx_enable_cache_replacement,
                          uint32_t shard_id,
                          DataStoreService *data_store_service);

    ~RocksDBCloudDataStore();

    rocksdb::S3ClientFactory BuildS3ClientFactory(const std::string &endpoint);

    bool StartDB(std::string cookie = "",
                 std::string prev_cookie = "") override;

    void Shutdown() override;

    bool AppendPreBuiltTable(std::string_view table_name);

    // =======================================================
    // Group: External function interface called directly by DataStoreService
    // delegate to DataStoreServiceClient
    // =======================================================

    bool Connect() override;

    bool InitializeClusterConfig(
        uint32_t req_shard_id,
        const std::unordered_map<
            uint32_t,
            std::vector<::EloqDS::remote::ClusterNodeConfig>> &ng_configs)
        override;

    void ReadClusterConfig(
        uint32_t req_shard_id,
        std::function<void(
            std::unordered_map<uint32_t,
                               std::vector<::EloqDS::remote::ClusterNodeConfig>>
                &&ng_configs,
            uint64_t version,
            bool uninitialized,
            const ::EloqDS::remote::CommonResult &result)> on_finish) override;

    void UpdateClusterConfig(
        uint32_t req_shard_id,
        uint64_t version,
        const std::unordered_map<
            uint32_t,
            std::vector<::EloqDS::remote::ClusterNodeConfig>> &new_cnf,
        std::function<void(const ::EloqDS::remote::CommonResult &result)>
            on_finish) override;

    void BatchFetchRecords(
        uint32_t req_shard_id,
        std::vector<::EloqDS::remote::FetchRecordsRequest::key> &&keys,
        std::function<void(
            std::vector<::EloqDS::remote::FetchRecordsResponse::record>
                &&records,
            const ::EloqDS::remote::CommonResult &result)> on_finish) override;

    void BatchWriteRecords(
        uint32_t req_shard_id,
        std::vector<EloqDS::remote::BatchWriteRecordsRequest::record> &&batch,
        const std::string &kv_table_name,
        std::function<void(const ::EloqDS::remote::CommonResult &result)>
            on_finish) override;

    void CkptEnd(
        uint32_t req_shard_id,
        const std::string &table_name,
        std::function<void(const ::EloqDS::remote::CommonResult &result)>
            on_finish) override;

    void UpsertTable(
        uint32_t req_shard_id,
        const std::string table_name,
        const std::string old_schema_img,
        const std::string new_schema_img,
        ::EloqDS::remote::UpsertTableOperationType op_type,
        uint64_t commit_ts,
        std::function<void(const ::EloqDS::remote::CommonResult &result)>
            on_finish) override;

    void FetchTableCatalog(
        uint32_t req_shard_id,
        const std::string &table_name,
        std::function<void(std::string &&schema_image,
                           bool found,
                           uint64_t version_ts,
                           const ::EloqDS::remote::CommonResult &result)>
            on_finish) override;

    void SwitchToReadOnly() override;
    void SwitchToReadWrite() override;
    DSShardStatus FetchDSShardStatus() const;

    std::string GenerateSessionId();

    void ScanOpen(
        const ::EloqDS::remote::ScanRequest &scan_req,
        std::function<void(const ::EloqDS::remote::ScanResponse &response)>
            on_finish) override;

    void ScanNext(
        const ::EloqDS::remote::ScanRequest &scan_req,
        std::function<void(const ::EloqDS::remote::ScanResponse &response)>
            on_finish) override;

    void ScanClose(
        const ::EloqDS::remote::ScanRequest &scan_req,
        std::function<void(const ::EloqDS::remote::ScanResponse &response)>
            on_finish) override;

protected:
    void WaitForPendingWrites();
    void IncreaseWriteCounter();
    void DecreaseWriteCounter();
    rocksdb::ColumnFamilyHandle *GetColumnFamilyHandler(const std::string &cf);
    void ResetColumnFamilyHandler(const std::string &old_cf,
                                  const std::string &new_cf,
                                  rocksdb::ColumnFamilyHandle *cfh);

    bool OpenCloudDB(const rocksdb::CloudFileSystemOptions &cfs_options);

    rocksdb::DBCloud *GetDBPtr();

    void ScanIterTTLCheckWorker();

private:
    rocksdb::InfoLogLevel info_log_level_;
    const bool enable_stats_;
    const uint32_t stats_dump_period_sec_;
    const std::string storage_path_;
    const size_t max_write_buffer_number_;
    const size_t max_background_jobs_;
    const size_t max_background_flushes_;
    const size_t max_background_compactions_;
    const size_t target_file_size_base_;
    const size_t target_file_size_multiplier_;
    const size_t write_buff_size_;
    const bool use_direct_io_for_flush_and_compaction_;
    const bool use_direct_io_for_read_;
    const size_t level0_stop_writes_trigger_;
    const size_t level0_slowdown_writes_trigger_;
    const size_t level0_file_num_compaction_trigger_;
    const size_t max_bytes_for_level_base_;
    const size_t max_bytes_for_level_multiplier_;
    const std::string compaction_style_;
    const size_t soft_pending_compaction_bytes_limit_;
    const size_t hard_pending_compaction_bytes_limit_;
    const size_t max_subcompactions_;
    const size_t write_rate_limit_;
    const size_t batch_write_size_;
    const size_t periodic_compaction_seconds_;
    const std::string dialy_offpeak_time_utc_;
    const std::string db_path_;
    const std::string ckpt_path_;
    const std::string backup_path_;
    const std::string received_snapshot_path_;
    const bool create_db_if_missing_{false};
    bool tx_enable_cache_replacement_{true};
    std::unique_ptr<EloqShare::TTLCompactionFilter> ttl_compaction_filter_{
        nullptr};

    std::unordered_map<std::string, std::string> pre_built_tables_;
    std::unordered_map<std::string,
                       std::unique_ptr<rocksdb::ColumnFamilyHandle>>
        column_families_;
    std::unique_ptr<ThreadWorkerPool> query_worker_pool_;
    std::shared_mutex db_mux_;
    std::mutex ddl_mux_;

    const EloqShare::RocksDBCloudConfig cloud_config_;
    rocksdb::CloudFileSystemOptions cfs_options_;
    std::shared_ptr<rocksdb::FileSystem> cloud_fs_{nullptr};
    std::unique_ptr<rocksdb::Env> cloud_env_{nullptr};
    rocksdb::DBCloud *db_{nullptr};

    std::atomic<size_t> ongoing_write_requests_{0};
};
#endif
}  // namespace EloqDS
