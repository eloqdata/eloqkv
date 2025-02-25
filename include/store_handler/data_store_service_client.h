#pragma once

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "data_store_service.h"
#include "store_util.h"
#include "thread_worker_pool.h"
#include "tx_service/include/cc/cc_shard.h"
#include "tx_service/include/sharder.h"
#include "tx_service/include/store/data_store_handler.h"
namespace EloqDS
{
#if ROCKSDB_CLOUD_FS()
class DataStoreServiceClient : public txservice::store::DataStoreHandler
{
public:
    DataStoreServiceClient();
    ~DataStoreServiceClient();

    DataStoreServiceClient(
        const DataStoreServiceClusterManager &cluster_manager,
        DataStoreService *data_store_service = nullptr)
        : ds_serv_shutdown_indicator_(false),
          cluster_manager_(cluster_manager),
          data_store_service_(data_store_service),
          flying_remote_fetch_count_(0)
    {
        remote_fetch_worker_ = std::make_unique<ThreadWorkerPool>(1);
        remote_fetch_worker_->SubmitWork([this]
                                         { CallRpcFetchRecordInBatch(); });
        if (data_store_service_ != nullptr)
        {
            data_store_service_->AddListenerForUpdateConfig(
                [this](const DataStoreServiceClusterManager &cluster_manager)
                { this->SetupConfig(cluster_manager); });
        }
    }

    // The maximum number of retries for RPC requests.
    static const int retry_limit_ = 2;

    /**
     * Connect to remote data store service.
     */
    void SetupConfig(const DataStoreServiceClusterManager &config);

    void ConnectToLocalDataStoreService(
        std::unique_ptr<DataStoreService> ds_serv);

    bool AppendPreBuiltTable(std::string_view table_name);

    // ==============================================
    // Group: Functions Inherit from DataStoreHandler
    // ==============================================

    // Override all the virtual functions in DataStoreHandler
    bool Connect() override;

    bool IsSharedStorage() const override
    {
        return true;
    }

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
        uint64_t commit_ts,
        txservice::NodeGroupId ng_id,
        int64_t tx_term,
        txservice::CcHandlerResult<txservice::Void> *hd_res,
        const txservice::AlterTableInfo *alter_table_info = nullptr,
        txservice::CcRequestBase *cc_req = nullptr,
        txservice::CcShard *ccs = nullptr,
        txservice::CcErrorCode *err_code = nullptr) override;

    void UpsertTableWithRetry(
        const std::string table_name,
        const std::string old_schema_img,
        const std::string new_schema_img,
        txservice::OperationType op_type,
        uint64_t commit_ts,
        txservice::NodeGroupId ng_id,
        int64_t tx_term,
        txservice::CcHandlerResult<txservice::Void> *hd_res,
        const txservice::AlterTableInfo *alter_table_info = nullptr,
        txservice::CcRequestBase *cc_req = nullptr,
        txservice::CcShard *ccs = nullptr,
        txservice::CcErrorCode *err_code = nullptr,
        int retry_count = 0);

    void FetchTableCatalog(const txservice::TableName &ccm_table_name,
                           txservice::FetchCatalogCc *fetch_cc) override;

    void FetchTableCatalogWithRetry(const txservice::TableName &ccm_table_name,
                                    txservice::FetchCatalogCc *fetch_cc,
                                    uint16_t retry_count);

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

    DataStoreOpStatus FetchRecord(txservice::FetchRecordCc *fetch_cc) override;

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

    void RestoreTxCache(txservice::NodeGroupId cc_ng_id,
                        int64_t cc_ng_term) override;

    bool OnLeaderStart(uint32_t *next_leader_node) override;

    void OnStartFollowing() override;

    void OnShutdown() override;

    // =======================================
    // Group: Functions for Scan
    // =======================================
    void ScanOpenWithRetry(
        EloqDS::remote::ScanRequest &scan_req,
        std::function<void(const EloqDS::remote::ScanResponse &)> on_finish,
        uint16_t retry_count);

    void CallRpcScanOpen(
        EloqDS::remote::ScanRequest &scan_req,
        std::function<void(const EloqDS::remote::ScanResponse &)> on_finish,
        uint16_t retry_count);

    void ScanNextWithRetry(
        EloqDS::remote::ScanRequest &scan_req,
        std::function<void(const EloqDS::remote::ScanResponse &)> on_finish,
        uint16_t retry_count);

    void CallRpcScanNext(
        EloqDS::remote::ScanRequest &scan_req,
        std::function<void(const EloqDS::remote::ScanResponse &)> on_finish,
        uint16_t retry_count);

    void ScanCloseWithRetry(
        EloqDS::remote::ScanRequest &scan_req,
        std::function<void(const EloqDS::remote::ScanResponse &)> on_finish,
        uint16_t retry_count);

    void CallRpcScanClose(
        EloqDS::remote::ScanRequest &scan_req,
        std::function<void(const EloqDS::remote::ScanResponse &)> on_finish,
        uint16_t retry_count);

    // ==================================================
    // Group: Functions to Call DataStoreServiceRpcServer
    // ==================================================

    void CallRpcFetchRecordInBatch();

    txservice::store::DataStoreHandler::DataStoreOpStatus CallRpcFetchRecord(
        txservice::FetchRecordCc *fetch_cc);

    bool CallRpcPutAll(std::vector<txservice::FlushRecord> &batch,
                       const txservice::TableName &table_name,
                       const txservice::TableSchema *table_schema,
                       uint32_t node_group);

    bool CallRpcCkptEnd(const txservice::TableName &table_name,
                        const txservice::TableSchema *schema,
                        uint64_t version);

    void CallRpcFetchTableCatalog(const std::string &ccm_table_name_str,
                                  txservice::FetchCatalogCc *fetch_cc,
                                  uint16_t retry_count);

    void CallRpcUpsertTable(const std::string &table_name,
                            const std::string &old_schema_img,
                            const std::string &new_schema_img,
                            txservice::OperationType op_type,
                            uint64_t commit_ts,
                            txservice::NodeGroupId ng_id,
                            int64_t tx_term,
                            txservice::CcHandlerResult<txservice::Void> *hd_res,
                            const txservice::AlterTableInfo *alter_table_info,
                            txservice::CcRequestBase *cc_req,
                            txservice::CcShard *ccs,
                            txservice::CcErrorCode *err_code,
                            uint16_t retry_cnt);

    bool CallRpcReadClusterConfig(
        std::unordered_map<uint32_t, std::vector<txservice::NodeConfig>>
            &ng_configs,
        uint64_t &version,
        bool &uninitialized,
        bool &need_retry);

    bool CallRpcUpdateClusterConfig(
        const std::unordered_map<uint32_t, std::vector<txservice::NodeConfig>>
            &new_cnf,
        uint64_t version,
        bool &need_retry);

    void HandleShardingError(const ::EloqDS::remote::CommonResult &result)
    {
        cluster_manager_.HandleShardingError(result);
    }

    std::shared_ptr<brpc::Channel> GetDataStoreServiceChannelByShardId(
        uint32_t shard_id);
    std::shared_ptr<brpc::Channel> UpdateDataStoreServiceChannelByShardId(
        uint32_t shard_id);
    std::shared_ptr<brpc::Channel> GetDataStoreServiceChannel(
        const DSSNode &node);
    std::shared_ptr<brpc::Channel> UpdateDataStoreServiceChannel(
        const DSSNode &node);

private:
    /**
     * @brief Check if the shard_id is local to the current node.
     * @param shard_id
     * @return true if the shard_id is local to the current node.
     */
    bool IsLocalShard(uint32_t shard_id);

    bthread::Mutex ds_service_mutex_;
    bthread::ConditionVariable ds_service_cv_;
    std::atomic<bool> ds_serv_shutdown_indicator_;

    // remote data store service configuration
    DataStoreServiceClusterManager cluster_manager_;

    // point to the data store service if it is colocated
    DataStoreService *data_store_service_;

    std::atomic<uint64_t> flying_remote_fetch_count_{0};
    // Work queue for fetch records from primary node
    std::deque<txservice::FetchRecordCc *> remote_fetch_cc_queue_;
    std::unique_ptr<ThreadWorkerPool> remote_fetch_worker_;

    // =======================================
    // Group: Functions for DSS Cluster Status
    // =======================================
    //
};

class FetchTableCatalogClosure : public ::google::protobuf::Closure
{
public:
    FetchTableCatalogClosure(txservice::FetchCatalogCc *fetch_cc,
                             DataStoreServiceClient &store_hd,
                             uint32_t req_shard_id,
                             uint16_t retry_count)
        : fetch_cc_(fetch_cc),
          ds_service_client_(store_hd),
          req_shard_id_(req_shard_id),
          retry_count_(retry_count)
    {
        cntl_.set_timeout_ms(5000);
    }

    void Run() override
    {
        std::unique_ptr<FetchTableCatalogClosure> self_guard(this);

        if (cntl_.Failed())
        {
            LOG(ERROR) << "Failed for fetch table catalog RPC request of ng#"
                       << fetch_cc_->GetNodeGroupId()
                       << ", with Error code: " << cntl_.ErrorCode()
                       << ". Error Msg: " << cntl_.ErrorText();
            if (cntl_.ErrorCode() != brpc::EOVERCROWDED &&
                cntl_.ErrorCode() != EAGAIN &&
                cntl_.ErrorCode() != brpc::ERPCTIMEDOUT)
            {
                ds_service_client_.UpdateDataStoreServiceChannelByShardId(
                    req_shard_id_);
            }
            fetch_cc_->SetFinish(
                txservice::RecordStatus::Unknown,
                static_cast<int>(txservice::CcErrorCode::DATA_STORE_ERR));
            return;
        }

        if (!fetch_cc_->ValidTermCheck())
        {
            fetch_cc_->SetFinish(
                txservice::RecordStatus::Unknown,
                static_cast<int>(txservice::CcErrorCode::NG_TERM_CHANGED));
            return;
        }

        auto &result = response_.result();
        if (result.error_code() != 0)
        {
            if (result.error_code() ==
                ::EloqDS::remote::DataStoreError::REQUESTED_NODE_NOT_OWNER)
            {
                ds_service_client_.HandleShardingError(result);
                if (retry_count_ < ds_service_client_.retry_limit_)
                {
                    ds_service_client_.FetchTableCatalogWithRetry(
                        fetch_cc_->CatalogName(), fetch_cc_, retry_count_ + 1);
                    return;
                }
            }

            fetch_cc_->SetFinish(
                txservice::RecordStatus::Unknown,
                static_cast<int>(txservice::CcErrorCode::DATA_STORE_ERR));
            return;
        }

        fetch_cc_->CatalogImage() = response_.schema_img();
        fetch_cc_->CommitTs() = response_.version_ts();
        fetch_cc_->SetFinish(
            response_.found() ? txservice::RecordStatus::Normal
                              : txservice::RecordStatus::Deleted,
            static_cast<int>(txservice::CcErrorCode::NO_ERROR));
    }

    brpc::Controller *Controller()
    {
        return &cntl_;
    }

    EloqDS::remote::FetchTableCatalogResponse *FetchTableCatalogResponse()
    {
        return &response_;
    }

    EloqDS::remote::FetchTableCatalogRequest *FetchTableCatalogRequest()
    {
        return &request_;
    }

private:
    txservice::FetchCatalogCc *fetch_cc_;
    DataStoreServiceClient &ds_service_client_;
    uint32_t req_shard_id_;
    uint16_t retry_count_{0};
    brpc::Controller cntl_;
    EloqDS::remote::FetchTableCatalogRequest request_;
    EloqDS::remote::FetchTableCatalogResponse response_;
};

class FetchRecordsRpcClosure : public ::google::protobuf::Closure
{
public:
    FetchRecordsRpcClosure(size_t size,
                           DataStoreServiceClient &store_hd,
                           uint32_t req_shard_id,
                           uint16_t retry_count,
                           std::atomic<uint64_t> &onflying_req_count)
        : ds_service_client_(store_hd),
          req_shard_id_(req_shard_id),
          retry_count_(retry_count),
          onflying_req_count_(onflying_req_count)
    {
        fetch_ccs_.reserve(size);
    }

    FetchRecordsRpcClosure(txservice::FetchRecordCc *fetch_cc,
                           DataStoreServiceClient &store_hd,
                           uint32_t req_shard_id,
                           uint16_t retry_count,
                           std::atomic<uint64_t> &onflying_req_count)
        : ds_service_client_(store_hd),
          req_shard_id_(req_shard_id),
          retry_count_(retry_count),
          onflying_req_count_(onflying_req_count)
    {
        fetch_ccs_.reserve(1);
        fetch_ccs_.push_back(fetch_cc);
    }

    FetchRecordsRpcClosure(const FetchRecordsRpcClosure &rhs) = delete;
    FetchRecordsRpcClosure(FetchRecordsRpcClosure &&rhs) = delete;

    void AddFetchCc(txservice::FetchRecordCc *fetch_cc)
    {
        fetch_ccs_.push_back(fetch_cc);
    }

    // Run() will be called when rpc request is processed by cc node service.
    void Run() override
    {
        onflying_req_count_.fetch_sub(1, std::memory_order_acq_rel);
        if (metrics::enable_kv_metrics)
        {
            metrics::TimePoint end_ts = metrics::Clock::now();
            for (auto *fetch_cc : fetch_ccs_)
            {
                metrics::kv_meter->Collect(
                    metrics::NAME_KV_READ_DURATION,
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        end_ts - fetch_cc->start_)
                        .count());
            }
            metrics::kv_meter->Collect(metrics::NAME_KV_READ_TOTAL,
                                       fetch_ccs_.size());
        }
        // Free closure on exit
        std::unique_ptr<FetchRecordsRpcClosure> self_guard(this);

        if (cntl_.Failed())
        {
            // RPC failed.
            LOG(ERROR) << "Failed for fetch records RPC request "
                       << ", with Error code: " << cntl_.ErrorCode()
                       << ". Error Msg: " << cntl_.ErrorText();
            if (cntl_.ErrorCode() != brpc::EOVERCROWDED &&
                cntl_.ErrorCode() != EAGAIN &&
                cntl_.ErrorCode() != brpc::ERPCTIMEDOUT)
            {
                channel_ =
                    ds_service_client_.UpdateDataStoreServiceChannelByShardId(
                        req_shard_id_);
            }

            for (auto *fetch_cc : fetch_ccs_)
            {
                fetch_cc->SetFinish(
                    static_cast<int>(txservice::CcErrorCode::DATA_STORE_ERR));
            }
        }
        else
        {
            auto &result = response_.result();
            auto err_code = static_cast<::EloqDS::remote::DataStoreError>(
                result.error_code());

            if (err_code == ::EloqDS::remote::DataStoreError::NO_ERROR)
            {
                assert(fetch_ccs_.size() ==
                       static_cast<size_t>(response_.records_size()));
                for (int idx = 0; idx < response_.records_size(); idx++)
                {
                    auto &record = response_.records()[idx];
                    auto *fetch_cc = fetch_ccs_[idx];
                    if (!fetch_cc->ValidTermCheck())
                    {
                        fetch_cc->SetFinish(static_cast<int>(
                            txservice::CcErrorCode::NG_TERM_CHANGED));
                        continue;
                    }
                    fetch_cc->rec_status_ =
                        record.is_deleted() ? txservice::RecordStatus::Deleted
                                            : txservice::RecordStatus::Normal;
                    fetch_cc->rec_ts_ = record.version();
                    if (fetch_cc->rec_status_ ==
                        txservice::RecordStatus::Normal)
                    {
                        fetch_cc->rec_str_ = record.payload();
                    }
                    fetch_cc->SetFinish(static_cast<int>(err_code));
                }
            }
            else if (err_code ==
                     ::EloqDS::remote::DataStoreError::REQUESTED_NODE_NOT_OWNER)
            {
                ds_service_client_.HandleShardingError(result);
                auto &new_key_sharding = result.new_key_sharding();
                auto type = new_key_sharding.type();
                if (type ==
                    ::EloqDS::remote::KeyShardingErrorType::PrimaryNodeChanged)
                {
                    if (retry_count_ < ds_service_client_.retry_limit_)
                    {
                        // Retry if primary node has changed.
                        retry_count_++;
                        channel_ = ds_service_client_
                                       .GetDataStoreServiceChannelByShardId(
                                           req_shard_id_);
                        self_guard.release();
                        // retry for new primary node
                        DLOG(INFO) << "FetchRecords failed with "
                                   << static_cast<int>(err_code)
                                   << ". Retry fetch records service";
                        cntl_.Reset();
                        response_.Clear();
                        EloqDS::remote::DataStoreRpcService_Stub stub(
                            channel_.get());
                        cntl_.set_timeout_ms(5000);
                        cntl_.set_write_to_socket_in_background(true);
                        stub.FetchRecords(&cntl_, &request_, &response_, this);
                        onflying_req_count_.fetch_add(
                            1, std::memory_order_acq_rel);
                        return;
                    }
                    else
                    {
                        for (auto *fetch_cc : fetch_ccs_)
                        {
                            fetch_cc->SetFinish(static_cast<int>(
                                txservice::CcErrorCode::DATA_STORE_ERR));
                        }
                    }
                }
                else
                {
                    // Retry from start using the retry count on fetch_cc_
                    // if shard has changed.
                    for (auto *fetch_cc : fetch_ccs_)
                    {
                        uint16_t &cc_retry_cnt = fetch_cc->RetryCnt();
                        if (cc_retry_cnt < ds_service_client_.retry_limit_)
                        {
                            cc_retry_cnt++;
                            ds_service_client_.FetchRecord(fetch_cc);
                        }
                        else
                        {
                            cc_retry_cnt = 0;
                            fetch_cc->SetFinish(static_cast<int>(
                                txservice::CcErrorCode::DATA_STORE_ERR));
                        }
                    }
                }
            }
            else
            {
                for (auto *fetch_cc : fetch_ccs_)
                {
                    fetch_cc->SetFinish(static_cast<int>(
                        txservice::CcErrorCode::DATA_STORE_ERR));
                }
            }
        }
        channel_ = nullptr;
    }

    brpc::Controller *Controller()
    {
        return &cntl_;
    }

    EloqDS::remote::FetchRecordsResponse *FetchRecordsResponse()
    {
        return &response_;
    }

    EloqDS::remote::FetchRecordsRequest *FetchRecordsRequest()
    {
        return &request_;
    }

    void SetChannel(std::shared_ptr<brpc::Channel> channel)
    {
        channel_ = channel;
    }

    brpc::Channel *Channel()
    {
        return channel_.get();
    }

private:
    brpc::Controller cntl_;
    EloqDS::remote::FetchRecordsRequest request_;
    EloqDS::remote::FetchRecordsResponse response_;
    std::shared_ptr<brpc::Channel> channel_;
    DataStoreServiceClient &ds_service_client_;
    uint32_t req_shard_id_;
    uint16_t retry_count_{0};
    std::vector<txservice::FetchRecordCc *> fetch_ccs_;
    std::atomic<uint64_t> &onflying_req_count_;
};

class UpsertTableClosure : public ::google::protobuf::Closure
{
public:
    UpsertTableClosure(DataStoreServiceClient &store_hd,
                       const uint32_t req_shard_id,
                       txservice::NodeGroupId ng_id,
                       int64_t tx_term,
                       const txservice::AlterTableInfo *alter_table_info,
                       uint16_t retry_cnt,
                       txservice::CcHandlerResult<txservice::Void> *hd_res,
                       txservice::CcErrorCode *err_code,
                       txservice::CcRequestBase *cc_req,
                       txservice::CcShard *ccs,
                       std::shared_ptr<void> defer_unpin)
        : ds_service_client_(store_hd),
          req_shard_id_(req_shard_id),
          ng_id_(ng_id),
          tx_term_(tx_term),
          alter_table_info_(alter_table_info),
          retry_cnt_(retry_cnt),
          hd_res_(hd_res),
          err_code_(err_code),
          cc_req_(cc_req),
          ccs_(ccs),
          defer_unpin_(defer_unpin)
    {
    }

    void Run() override
    {
        std::unique_ptr<UpsertTableClosure> self_guard(this);
        if (cntl_.Failed())
        {
            if (cntl_.ErrorCode() != EAGAIN &&
                cntl_.ErrorCode() != brpc::ERPCTIMEDOUT &&
                cntl_.ErrorCode() != brpc::EOVERCROWDED)
            {
                ds_service_client_.UpdateDataStoreServiceChannelByShardId(
                    req_shard_id_);
            }

            LOG(ERROR)
                << "Failed for upsert table RPC request, with Error code: "
                << cntl_.ErrorCode() << ". Error Msg: " << cntl_.ErrorText();
            if (hd_res_ != nullptr)
            {
                hd_res_->SetError(txservice::CcErrorCode::DATA_STORE_ERR);
            }
            else
            {
                *err_code_ = txservice::CcErrorCode::DATA_STORE_ERR;
                ccs_->Enqueue(cc_req_);
            }
            return;
        }

        // No need to check term here since the node group data is pinned
        auto &result = response_.result();
        if (result.error_code() != 0)
        {
            if (result.error_code() ==
                EloqDS::remote::DataStoreError::REQUESTED_NODE_NOT_OWNER)
            {
                ds_service_client_.HandleShardingError(result);
                if (retry_cnt_ < ds_service_client_.retry_limit_)
                {
                    auto ds_op_type =
                        static_cast<::EloqDS::remote::UpsertTableOperationType>(
                            request_.op_type());

                    ds_service_client_.UpsertTableWithRetry(
                        request_.table_name_str(),
                        request_.old_schema_img(),
                        request_.new_schema_img(),
                        ::EloqShare::OperationTypeConverter(ds_op_type),
                        request_.commit_ts(),
                        ng_id_,
                        tx_term_,
                        hd_res_,
                        alter_table_info_,
                        cc_req_,
                        ccs_,
                        err_code_,
                        (retry_cnt_ + 1));
                    return;
                }
            }

            if (hd_res_ != nullptr)
            {
                hd_res_->SetError(txservice::CcErrorCode::DATA_STORE_ERR);
            }
            else
            {
                *err_code_ = txservice::CcErrorCode::DATA_STORE_ERR;
                ccs_->Enqueue(cc_req_);
            }
            return;
        }

        if (hd_res_ != nullptr)
        {
            hd_res_->SetFinished();
        }
        else
        {
            *err_code_ = txservice::CcErrorCode::NO_ERROR;
            ccs_->Enqueue(cc_req_);
        }
    }

    brpc::Controller *Controller()
    {
        return &cntl_;
    }

    EloqDS::remote::UpsertTableResponse *UpsertTableResponse()
    {
        return &response_;
    }

    EloqDS::remote::UpsertTableRequest *UpsertTableRequest()
    {
        return &request_;
    }

private:
    DataStoreServiceClient &ds_service_client_;
    uint32_t req_shard_id_;
    txservice::NodeGroupId ng_id_;
    int64_t tx_term_;
    const txservice::AlterTableInfo *alter_table_info_;
    uint16_t retry_cnt_{0};
    brpc::Controller cntl_;
    EloqDS::remote::UpsertTableRequest request_;
    EloqDS::remote::UpsertTableResponse response_;
    txservice::CcHandlerResult<txservice::Void> *hd_res_;
    txservice::CcErrorCode *err_code_;
    txservice::CcRequestBase *cc_req_;
    txservice::CcShard *ccs_;
    std::shared_ptr<void> defer_unpin_;
};

enum class ScanOpType
{
    SCAN_OPEN,
    SCAN_NEXT,
    SCAN_CLOSE
};

class ScanClosure : public ::google::protobuf::Closure
{
public:
    ScanClosure(
        DataStoreServiceClient &store_hd,
        ScanOpType op_type,
        ::EloqDS::remote::ScanRequest &scan_req,
        std::function<void(const ::EloqDS::remote::ScanResponse &)> &on_finish,
        uint16_t retry_count)
        : ds_service_client_(store_hd),
          on_finish_(on_finish),
          retry_count_(retry_count),
          op_type_(op_type),
          scan_req_(scan_req)
    {
    }

    void Run() override
    {
        std::unique_ptr<ScanClosure> self_guard(this);

        if (cntl_.Failed())
        {
            LOG(ERROR) << "Failed for scan RPC request, with Error code: "
                       << cntl_.ErrorCode()
                       << ". Error Msg: " << cntl_.ErrorText();
            if (cntl_.ErrorCode() != EAGAIN &&
                cntl_.ErrorCode() != brpc::ERPCTIMEDOUT &&
                cntl_.ErrorCode() != brpc::EOVERCROWDED)
            {
                ds_service_client_.UpdateDataStoreServiceChannelByShardId(
                    scan_req_.req_shard_id());
            }

            auto *result = response_.mutable_result();
            result->set_error_code(static_cast<int>(
                ::EloqDS::remote::DataStoreError::READ_FAILED));
            result->set_error_msg("Scan failed due to rpc error");
            on_finish_(response_);
            return;
        }

        auto &result = response_.result();
        if (result.error_code() != ::EloqDS::remote::DataStoreError::NO_ERROR)
        {
            if (result.error_code() ==
                EloqDS::remote::DataStoreError::REQUESTED_NODE_NOT_OWNER)
            {
                ds_service_client_.HandleShardingError(result);
                if (retry_count_ < ds_service_client_.retry_limit_)
                {
                    switch (op_type_)
                    {
                    case ScanOpType::SCAN_OPEN:
                        ds_service_client_.ScanOpenWithRetry(
                            scan_req_, on_finish_, retry_count_ + 1);
                        break;
                    case ScanOpType::SCAN_NEXT:
                        ds_service_client_.ScanNextWithRetry(
                            scan_req_, on_finish_, retry_count_ + 1);
                        break;
                    case ScanOpType::SCAN_CLOSE:
                        ds_service_client_.ScanCloseWithRetry(
                            scan_req_, on_finish_, retry_count_ + 1);
                        break;
                    default:
                        LOG(ERROR) << "Invalid scan operation type";
                        break;
                    }
                    return;
                }
            }
        }

        on_finish_(response_);
    }

    brpc::Controller *Controller()
    {
        return &cntl_;
    }

    ::EloqDS::remote::ScanResponse *ScanResponse()
    {
        return &response_;
    }

    ::EloqDS::remote::ScanRequest *ScanRequest()
    {
        return &scan_req_;
    }

    ScanOpType OpType() const
    {
        return op_type_;
    }

private:
    DataStoreServiceClient &ds_service_client_;
    std::function<void(const ::EloqDS::remote::ScanResponse &)> on_finish_;
    uint16_t retry_count_;
    brpc::Controller cntl_;
    ScanOpType op_type_;
    ::EloqDS::remote::ScanRequest &scan_req_;
    ::EloqDS::remote::ScanResponse response_;
};
#endif
}  // namespace EloqDS
