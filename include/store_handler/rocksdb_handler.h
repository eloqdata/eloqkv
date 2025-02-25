#pragma once

#include <brpc/stream.h>
#include <bthread/condition_variable.h>
#include <unistd.h>

#include <condition_variable>
#include <deque>
#include <fstream>
#include <memory>
#include <queue>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cc_req_base.h"
#include "cc_shard.h"
#include "error_messages.h"
#include "kv_store.h"

#if ROCKSDB_CLOUD_FS()
#include "rocksdb/cloud/db_cloud.h"
#else
#include "rocksdb/db.h"
#endif
#include "redis_object.h"
#include "rocksdb_config.h"
#include "store_util.h"
#include "tx_service/include/store/data_store_handler.h"
#include "tx_service/include/tx_worker_pool.h"

namespace EloqKV
{

class RocksDBHandlerImpl;

struct RocksDBCatalogInfo : public txservice::KVCatalogInfo
{
public:
    using uptr = std::unique_ptr<RocksDBCatalogInfo>;
    RocksDBCatalogInfo()
    {
    }
    RocksDBCatalogInfo(const std::string &kv_table_name,
                       const std::string &kv_index_names){};
    ~RocksDBCatalogInfo()
    {
    }
    std::string Serialize() const override
    {
        return "";
    }
    void Deserialize(const char *buf, size_t &offset) override
    {
    }
};

class RocksDBHandler : public txservice::store::DataStoreHandler
{
public:
    explicit RocksDBHandler(const EloqShare::RocksDBConfig &config,
                            bool create_if_missing,
                            bool tx_enable_cache_replacement);

    ~RocksDBHandler();

    // Override all the virtual functions in DataStoreHandler
    bool Connect() override;

    void ScheduleTimerTasks() override;

    /**
     * Initialize cluster config based on the based in ips and ports. This
     * should only be called during bootstrap.
     */
    bool InitializeClusterConfig(
        const std::unordered_map<uint32_t, std::vector<txservice::NodeConfig>>
            &ng_configs) override;

    /**
     * Read cluster config from kv store cluster config table.
     */
    bool ReadClusterConfig(
        std::unordered_map<uint32_t, std::vector<txservice::NodeConfig>>
            &ng_configs,
        uint64_t &version,
        bool &uninitialized) override;

    /**
     * @brief flush entries in \@param batch to base table or skindex table
     * in data store, stop and return false if node_group is not longer
     * leader.
     * @param batch
     * @param table_name base table name or sk index name
     * @param table_schema
     * @param schema_ts
     * @param node_group
     * @return whether all entries are written to data store successfully
     */
    bool PutAll(std::vector<txservice::FlushRecord> &batch,
                const txservice::TableName &table_name,
                const txservice::TableSchema *table_schema,
                uint32_t node_group) override;

    /**
     * @brief indicate end of flush entries in a single ckpt for \@param
     * batch to base table or skindex table in data store, stop and return
     * false if node_group is not longer leader.
     * @param table_name base table name or sk index name
     * @param node_group
     * @return whether all entries are written to data store successfully
     */
    bool CkptEnd(const txservice::TableName &table_name,
                 const txservice::TableSchema *schema,
                 uint32_t node_group,
                 uint64_t version) override;

    void UpsertTable(
        const txservice::TableSchema *old_table_schema,
        const txservice::TableSchema *new_table_schema,
        txservice::OperationType op_type,
        uint64_t write_time,
        txservice::NodeGroupId ng_id,
        int64_t tx_term,
        txservice::CcHandlerResult<txservice::Void> *hd_res,
        const txservice::AlterTableInfo *alter_table_info = nullptr,
        txservice::CcRequestBase *cc_req = nullptr,
        txservice::CcShard *ccs = nullptr,
        txservice::CcErrorCode *err_code = nullptr) override;

    void FetchTableCatalog(const txservice::TableName &ccm_table_name,
                           txservice::FetchCatalogCc *fetch_cc) override;

    void FetchCurrentTableStatistics(
        const txservice::TableName &ccm_table_name,
        txservice::FetchTableStatisticsCc *fetch_cc) override;

    void FetchTableStatistics(
        const txservice::TableName &ccm_table_name,
        txservice::FetchTableStatisticsCc *fetch_cc) override;

    bool UpsertTableStatistics(
        const txservice::TableName &ccm_table_name,
        const std::unordered_map<
            txservice::TableName,
            std::pair<uint64_t, std::vector<txservice::TxKey>>>
            &sample_pool_map,
        uint64_t version) override;

    void FetchTableRanges(txservice::FetchTableRangesCc *fetch_cc) override;

    void FetchRangeSlices(txservice::FetchRangeSlicesReq *fetch_cc) override;

    bool DeleteOutOfRangeDataInternal(std::string delete_from_partition_sql,
                                      int32_t partition_id,
                                      const txservice::TxKey *start_k);

    bool DeleteOutOfRangeData(
        const txservice::TableName &table_name,
        int32_t partition_id,
        const txservice::TxKey *start_key,
        const txservice::TableSchema *table_schema) override;

    bool GetNextRangePartitionId(const txservice::TableName &tablename,
                                 uint32_t range_cnt,
                                 int32_t &out_next_partition_id,
                                 int retry_count) override;

    bool Read(const txservice::TableName &table_name,
              const txservice::TxKey &key,
              txservice::TxRecord &rec,
              bool &found,
              uint64_t &version_ts,
              const txservice::TableSchema *table_schema) override;

#ifdef ON_KEY_OBJECT
    static void SerializeFlushRecord(const txservice::FlushRecord &flush_rec,
                                     std::vector<char> &buf);
    static void DeserializeRecord(const char *payload,
                                  const size_t payload_size,
                                  std::string &rec_str,
                                  bool &is_deleted,
                                  int64_t &version_ts);
    static void DeserializeToTxRecord(const char *payload,
                                      const size_t payload_size,
                                      txservice::TxRecord::Uptr &typed_rec,
                                      bool &is_deleted,
                                      int64_t &version_ts);

    txservice::store::DataStoreHandler::DataStoreOpStatus FetchRecord(
        txservice::FetchRecordCc *fetch_cc) override;
    rocksdb::ColumnFamilyHandle *GetColumnFamilyHandler(const std::string &cf);
    void ResetColumnFamilyHandler(const std::string &old_cf,
                                  const std::string &new_cf,
                                  rocksdb::ColumnFamilyHandle *cfh);
#endif

    std::unique_ptr<txservice::store::DataStoreScanner> ScanForward(
        const txservice::TableName &table_name,
        uint32_t ng_id,
        const txservice::TxKey &start_key,
        bool inclusive,
        uint8_t key_parts,
        const std::vector<txservice::store::DataStoreSearchCond> &search_cond,
        const txservice::KeySchema *key_schema,
        const txservice::RecordSchema *rec_schema,
        const txservice::KVCatalogInfo *kv_info,
        bool scan_foward) override;

    txservice::store::DataStoreHandler::DataStoreOpStatus LoadRangeSlice(
        const txservice::TableName &table_name,
        const txservice::KVCatalogInfo *kv_info,
        uint32_t range_partition_id,
        txservice::LoadRangeSliceRequest *load_slice_req) override;

    bool UpdateRangeSlices(const txservice::TableName &table_name,
                           uint64_t version,
                           txservice::TxKey range_start_key,
                           std::vector<const txservice::StoreSlice *> slices,
                           int32_t partition_id,
                           uint64_t range_version) override;

    bool UpsertRanges(const txservice::TableName &table_name,
                      std::vector<txservice::SplitRangeInfo> range_info,
                      uint64_t version) override;

    bool FetchTable(const txservice::TableName &table_name,
                    std::string &schema_image,
                    bool &found,
                    uint64_t &version_ts) const override;

    bool DiscoverAllTableNames(
        std::vector<std::string> &norm_name_vec,
        const std::function<void()> *yield_fptr = nullptr,
        const std::function<void()> *resume_fptr = nullptr) const override;

    //-- database
    bool UpsertDatabase(std::string_view db,
                        std::string_view definition) const override;
    bool DropDatabase(std::string_view db) const override;
    bool FetchDatabase(
        std::string_view db,
        std::string &definition,
        bool &found,
        const std::function<void()> *yield_fptr = nullptr,
        const std::function<void()> *resume_fptr = nullptr) const override;
    bool FetchAllDatabase(
        std::vector<std::string> &dbnames,
        const std::function<void()> *yield_fptr = nullptr,
        const std::function<void()> *resume_fptr = nullptr) const override;

    bool DropKvTable(const std::string &kv_table_name) const override;

    void DropKvTableAsync(const std::string &kv_table_name) const override;

    void SetTxService(txservice::TxService *tx_service);

    std::string CreateKVCatalogInfo(
        const txservice::TableSchema *table_schema) const override;
    txservice::KVCatalogInfo::uptr DeserializeKVCatalogInfo(
        const std::string &kv_info_str, size_t &offset) const override;

    std::string CreateNewKVCatalogInfo(
        const txservice::TableName &table_name,
        const txservice::TableSchema *current_table_schema,
        txservice::AlterTableInfo &alter_table_info) override;

    /**
     * @brief Write batch historical versions into DataStore.
     *
     */
    bool PutArchivesAll(uint32_t node_group,
                        const txservice::TableName &table_name,
                        const txservice::KVCatalogInfo *kv_info,
                        std::vector<txservice::FlushRecord> &batch) override;
    /**
     * @brief Copy record from base/sk table to mvcc_archives.
     */
    bool CopyBaseToArchive(std::vector<txservice::TxKey> &batch,
                           uint32_t node_group,
                           const txservice::TableName &table_name,
                           const txservice::TableSchema *table_schema) override;

    /**
     * @brief  Get the latest visible(commit_ts <= upper_bound_ts)
     * historical version.
     */
    bool FetchVisibleArchive(const txservice::TableName &table_name,
                             const txservice::KVCatalogInfo *kv_info,
                             const txservice::TxKey &key,
                             const uint64_t upper_bound_ts,
                             txservice::TxRecord &rec,
                             txservice::RecordStatus &rec_status,
                             uint64_t &commit_ts) override;

    /**
     * @brief  Fetch all archives whose commit_ts >= from_ts.
     */
    bool FetchArchives(const txservice::TableName &table_name,
                       const txservice::KVCatalogInfo *kv_info,
                       const txservice::TxKey &key,
                       std::vector<txservice::VersionTxRecord> &archives,
                       uint64_t from_ts) override;

    bool UpdateClusterConfig(
        const std::unordered_map<uint32_t, std::vector<txservice::NodeConfig>>
            &new_cnf,
        uint64_t version) override;

    bool NeedCopyRange() const override;

    // call this function before Connect().
    bool AppendPreBuiltTable(std::string_view table_name)
    {
        pre_built_tables_.emplace(table_name, table_name);
        return true;
    }

    void RestoreTxCache(txservice::NodeGroupId cc_ng_id,
                        int64_t cc_ng_term) override;

    void ParallelIterateTable(
        txservice::NodeGroupId cc_ng_id,
        uint64_t cc_ng_term,
        std::shared_ptr<std::queue<std::pair<std::string, std::string>>>
            table_names,
        uint16_t core_cnt,
        std::shared_ptr<bthread::Mutex> task_mutex,
        const size_t batch_size,
        const size_t concurrent_cc_count,
        std::shared_ptr<std::atomic<txservice::CcErrorCode>>
            cancel_data_loading_on_error,
        std::shared_ptr<std::atomic<uint16_t>> on_flying_count);

    bool OnLeaderStart(uint32_t *next_leader_node) override;

    void OnStartFollowing() override;

    void OnShutdown() override;

    virtual void Shutdown() = 0;

protected:
    virtual bool StartDB(bool is_ng_leader,
                         uint32_t *next_leader = nullptr) = 0;

#if ROCKSDB_CLOUD_FS()
    virtual rocksdb::DBCloud *GetDBPtr() = 0;
#else
    virtual rocksdb::DB *GetDBPtr() const = 0;
#endif

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
    size_t periodic_compaction_seconds_;
    std::string dialy_offpeak_time_utc_;
    const std::string db_path_;
    const std::string ckpt_path_;
    const std::string backup_path_;
    const std::string received_snapshot_path_;
    bool create_db_if_missing_{false};
    bool tx_enable_cache_replacement_{true};

    std::unordered_map<std::string, std::string> pre_built_tables_;
    std::unordered_map<std::string,
                       std::unique_ptr<rocksdb::ColumnFamilyHandle>>
        column_families_;
    std::unique_ptr<txservice::TxWorkerPool> query_worker_pool_;
    std::shared_mutex db_mux_;
    std::mutex ddl_mux_;
    // Worker that replace snapshot, load snapshot, delete expired snapshot.
    // These operations must be done in sequence.
    std::unique_ptr<txservice::TxWorkerPool> db_manage_worker_;

    struct UpsertTableReq
    {
        UpsertTableReq(const txservice::TableSchema *old_table_schema,
                       const txservice::TableSchema *table_schema,
                       txservice::OperationType op_type,
                       uint64_t write_time,
                       txservice::NodeGroupId ng_id,
                       int64_t tx_term,
                       const txservice::AlterTableInfo *alter_table_info,
                       txservice::CcRequestBase *cc_req,
                       txservice::CcShard *ccs,
                       txservice::CcErrorCode *err_code)
            : old_table_schema_(old_table_schema),
              table_schema_(table_schema),
              op_type_(op_type),
              write_time_(write_time),
              ng_id_(ng_id),
              tx_term_(tx_term),
              alter_table_info_(alter_table_info),
              cc_req_(cc_req),
              ccs_(ccs),
              err_code_(err_code)
        {
        }

        const txservice::TableSchema *old_table_schema_;
        const txservice::TableSchema *table_schema_;
        txservice::OperationType op_type_;
        uint64_t write_time_;
        txservice::NodeGroupId ng_id_;
        int64_t tx_term_;
        const txservice::AlterTableInfo *alter_table_info_;
        txservice::CcRequestBase *cc_req_;
        txservice::CcShard *ccs_;
        txservice::CcErrorCode *err_code_;
    };
    std::mutex pending_ddl_mux_;
    std::queue<UpsertTableReq> pending_ddl_req_;
};

#if ROCKSDB_CLOUD_FS()
class RocksDBCloudHandlerImpl : public RocksDBHandler
{
public:
    explicit RocksDBCloudHandlerImpl(
        const EloqShare::RocksDBCloudConfig &cloud_config,
        const EloqShare::RocksDBConfig &config,
        bool create_if_missing,
        bool tx_enable_cache_replacement);

    ~RocksDBCloudHandlerImpl() override;

    bool IsSharedStorage() const override
    {
        return false;
    }

    bool CreateSnapshot(const std::string &snapshot_path,
                        std::vector<std::string> &snapshot_files);
    bool CreateSnapshotForStandby(
        std::vector<std::string> &snapshot_files) override;
    bool CreateSnapshotForBackup(
        const std::string &backup_name,
        std::vector<std::string> &snapshot_files) override;
    bool RemoveBackupSnapshot(const std::string &backup_name) override;

    bool SendSnapshotToRemote(uint32_t ng_id,
                              int64_t ng_term,
                              std::vector<std::string> &snapshot_files,
                              const std::string &remote_dest) override;

    bool OnSnapshotReceived(
        const txservice::remote::OnSnapshotSyncedRequest *req) override;

    std::string SnapshotSyncDestPath() const override;

    void Shutdown() override;

protected:
    rocksdb::DBCloud *GetDBPtr() override;
    bool StartDB(bool is_ng_leader, uint32_t *next_leader = nullptr) override;

private:
    bool SendFileToRemoteNode(const std::string &snapshot_path,
                              const std::string &remote_path);
    bool OpenCloudDB(const rocksdb::CloudFileSystemOptions &cfs_options,
                     bool is_ng_leader,
                     uint32_t *next_leader_node = nullptr);
    bool OverrideDB(const std::string &new_snapshot_path);
    const EloqShare::RocksDBCloudConfig cloud_config_;

    std::shared_ptr<rocksdb::FileSystem> cloud_fs_;
    std::unique_ptr<rocksdb::Env> cloud_env_;
    rocksdb::DBCloud *db_;
    std::unique_ptr<EloqShare::TTLCompactionFilter> ttl_compaction_filter_{
        nullptr};
};

#else

class RocksDBHandlerImpl : public RocksDBHandler
{
public:
    explicit RocksDBHandlerImpl(const EloqShare::RocksDBConfig &config,
                                bool create_if_missing = false,
                                bool tx_enable_cache_replacement = true);

    ~RocksDBHandlerImpl();

    bool IsSharedStorage() const override
    {
        return false;
    }

    bool CreateSnapshot(const std::string &snapshot_path,
                        std::vector<std::string> &snapshot_files);
    bool CreateSnapshotForStandby(
        std::vector<std::string> &snapshot_files) override;
    bool CreateSnapshotForBackup(
        const std::string &backup_name,
        std::vector<std::string> &snapshot_files) override;
    bool RemoveBackupSnapshot(const std::string &backup_name) override;

    bool SendSnapshotToRemote(uint32_t ng_id,
                              int64_t ng_term,
                              std::vector<std::string> &snapshot_files,
                              const std::string &remote_dest) override;

    bool OnSnapshotReceived(
        const txservice::remote::OnSnapshotSyncedRequest *req) override;

    std::string SnapshotSyncDestPath() const override;

    void Shutdown() override;

protected:
    rocksdb::DB *GetDBPtr() const override;
    bool StartDB(bool is_ng_leader, uint32_t *next_leader = nullptr) override;

private:
    bool SendFileToRemoteNode(const std::string &snapshot_path,
                              const std::string &remote_path);

    bool OverrideDB(const std::string &new_snapshot_path);
    rocksdb::DB *db_{nullptr};
    std::unique_ptr<EloqShare::TTLCompactionFilter> ttl_compaction_filter_{
        nullptr};
    std::unique_ptr<txservice::TxWorkerPool> rsync_task_pool_{nullptr};

    friend class RocksdbSnapshotCopier;
};

#endif

}  // namespace EloqKV
