#include "data_store_service_client.h"

#include <glog/logging.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "data_store_service_scanner.h"
#include "eloq_key.h"
#include "metrics.h"
#include "thread_worker_pool.h"
#include "tx_service/include/cc/cc_map.h"
#include "tx_service/include/cc/local_cc_shards.h"
#include "tx_service/include/error_messages.h"

namespace EloqDS
{
#if ROCKSDB_CLOUD_FS()
DataStoreServiceClient::DataStoreServiceClient()
    : ds_serv_shutdown_indicator_(false), flying_remote_fetch_count_(0)
{
    remote_fetch_worker_ = std::make_unique<ThreadWorkerPool>(1);
    remote_fetch_worker_->SubmitWork([this] { CallRpcFetchRecordInBatch(); });
}

DataStoreServiceClient::~DataStoreServiceClient()
{
    {
        std::unique_lock<bthread::Mutex> lk(ds_service_mutex_);
        ds_serv_shutdown_indicator_.store(true, std::memory_order_release);
        ds_service_cv_.notify_all();
    }

    remote_fetch_worker_->Shutdown();
}

void DataStoreServiceClient::SetupConfig(
    const DataStoreServiceClusterManager &cluster_manager)
{
    for (const auto &[_, group] : cluster_manager.GetAllShards())
    {
        for (const auto &node : group.nodes_)
        {
            LOG(INFO) << "Node Hostname: " << node.host_name_
                      << ", Port: " << node.port_;
        }
    }
    cluster_manager_ = cluster_manager;
}

bool DataStoreServiceClient::Connect()
{
    return true;
}

bool DataStoreServiceClient::AppendPreBuiltTable(std::string_view table_name)
{
    return true;
}

void DataStoreServiceClient::ScheduleTimerTasks()
{
    LOG(ERROR) << "ScheduleTimerTasks not implemented";
    assert(false);
}

bool DataStoreServiceClient::InitializeClusterConfig(
    const std::unordered_map<uint32_t, std::vector<txservice::NodeConfig>>
        &ng_configs)
{
    uint32_t shard_id = cluster_manager_.GetShardIdByKey("");
    if (IsLocalShard(shard_id))
    {
        // convert txservice::NodeConfig to EloqDS::remote::ClusterNodeConfig
        std::unordered_map<uint32_t,
                           std::vector<EloqDS::remote::ClusterNodeConfig>>
            ng_configs_copy;
        for (const auto &ng : ng_configs)
        {
            std::vector<EloqDS::remote::ClusterNodeConfig> node_configs;
            for (const auto &node : ng.second)
            {
                EloqDS::remote::ClusterNodeConfig node_config;
                node_config.set_node_id(node.node_id_);
                node_config.set_host_name(node.host_name_);
                node_config.set_port(node.port_);
                node_config.set_is_candidate(node.is_candidate_);
                node_configs.push_back(std::move(node_config));
            }
            ng_configs_copy[ng.first] = std::move(node_configs);
        }
        return data_store_service_->InitializeClusterConfig(0, ng_configs_copy);
    }
    else
    {
        LOG(ERROR) << "InitializeClusterConfig does not has RPC implementation";
        assert(false);
    }
    return true;
}

bool DataStoreServiceClient::ReadClusterConfig(
    std::unordered_map<uint32_t, std::vector<txservice::NodeConfig>>
        &ng_configs,
    uint64_t &version,
    bool &uninitialized)
{
    bool ret = false;
    bool need_retry = true;
    uint32_t retry_count = 0;

    while (need_retry && retry_count < retry_limit_)
    {
        need_retry = false;

        const uint32_t req_shard_id = cluster_manager_.GetShardIdByKey("");
        if (IsLocalShard(req_shard_id))
        {
            bthread::Mutex mtx;
            bthread::ConditionVariable cv;
            bool done = false;
            ::EloqDS::remote::DataStoreError error_code =
                ::EloqDS::remote::DataStoreError::NO_ERROR;
            data_store_service_->ReadClusterConfig(
                req_shard_id,
                [this,
                 &need_retry,
                 &ng_configs,
                 &version,
                 &uninitialized,
                 &error_code,
                 &mtx,
                 &cv,
                 &done](std::unordered_map<
                            uint32_t,
                            std::vector<::EloqDS::remote::ClusterNodeConfig>>
                            &&res_ng_configs,
                        uint64_t res_version,
                        bool res_uninitialized,
                        const ::EloqDS::remote::CommonResult &result)
                {
                    if (result.error_code() !=
                        ::EloqDS::remote::DataStoreError::NO_ERROR)
                    {
                        LOG(ERROR) << "ReadClusterConfig failed, error code: "
                                   << static_cast<int>(result.error_code())
                                   << ", error message: " << result.error_msg();
                        error_code =
                            static_cast<::EloqDS::remote::DataStoreError>(
                                result.error_code());
                        uninitialized = res_uninitialized;
                        if (error_code == ::EloqDS::remote::DataStoreError::
                                              REQUESTED_NODE_NOT_OWNER)
                        {
                            cluster_manager_.HandleShardingError(result);
                            need_retry = true;
                        }
                    }
                    else
                    {
                        for (const auto &ng : res_ng_configs)
                        {
                            std::vector<txservice::NodeConfig> node_configs;
                            for (const auto &node : ng.second)
                            {
                                node_configs.push_back(
                                    txservice::NodeConfig(node.node_id(),
                                                          node.host_name(),
                                                          node.port(),
                                                          node.is_candidate()));
                            }
                            ng_configs[ng.first] = node_configs;
                        }
                        uninitialized = res_uninitialized;
                        // reset the default version if no error and initialized
                        if (!uninitialized)
                        {
                            version = res_version;
                        }
                    }

                    std::unique_lock<bthread::Mutex> lk(mtx);
                    done = true;
                    cv.notify_one();
                });

            std::unique_lock<bthread::Mutex> lk(mtx);
            while (!done)
            {
                cv.wait(lk);
            }
            ret = error_code == ::EloqDS::remote::DataStoreError::NO_ERROR;
        }
        else
        {
            ret = CallRpcReadClusterConfig(
                ng_configs, version, uninitialized, need_retry);
        }

        if (need_retry)
        {
            LOG(ERROR) << "ReadClusterConfig failed, retrying...";
            retry_count++;
        }
    }

    return ret;
}

bool DataStoreServiceClient::UpdateClusterConfig(
    const std::unordered_map<uint32_t, std::vector<txservice::NodeConfig>>
        &new_cnf,
    uint64_t version)
{
    bool ret = true;
    bool need_retry = true;
    uint32_t retry_count = 0;

    while (need_retry && retry_count < retry_limit_)
    {
        need_retry = false;

        const uint32_t req_shard_id = cluster_manager_.GetShardIdByKey("");
        if (IsLocalShard(req_shard_id))
        {
            std::unordered_map<uint32_t,
                               std::vector<EloqDS::remote::ClusterNodeConfig>>
                new_cnf_copy;
            for (const auto &ng : new_cnf)
            {
                std::vector<EloqDS::remote::ClusterNodeConfig> node_configs;
                for (const auto &node : ng.second)
                {
                    EloqDS::remote::ClusterNodeConfig node_config;
                    node_config.set_node_id(node.node_id_);
                    node_config.set_host_name(node.host_name_);
                    node_config.set_port(node.port_);
                    node_config.set_is_candidate(node.is_candidate_);
                    node_configs.push_back(std::move(node_config));
                }
                new_cnf_copy[ng.first] = std::move(node_configs);
            }

            bthread::Mutex mtx;
            bthread::ConditionVariable cv;
            bool done = false;
            data_store_service_->UpdateClusterConfig(
                req_shard_id,
                version,
                new_cnf_copy,
                [this, &ret, &need_retry, &mtx, &cv, &done](
                    const ::EloqDS::remote::CommonResult &result)
                {
                    ::EloqDS::remote::DataStoreError error_code =
                        static_cast<::EloqDS::remote::DataStoreError>(
                            result.error_code());
                    if (error_code !=
                        ::EloqDS::remote::DataStoreError::NO_ERROR)
                    {
                        LOG(ERROR) << "UpdateClusterConfig failed, error code: "
                                   << static_cast<int>(error_code)
                                   << ", error message: " << result.error_msg();
                        if (error_code == ::EloqDS::remote::DataStoreError::
                                              REQUESTED_NODE_NOT_OWNER)
                        {
                            cluster_manager_.HandleShardingError(result);
                            need_retry = true;
                        }
                        ret = false;
                    }
                    else
                    {
                        ret = true;
                    }

                    std::unique_lock<bthread::Mutex> lk(mtx);
                    done = true;
                    cv.notify_one();
                });

            std::unique_lock<bthread::Mutex> lk(mtx);
            while (!done)
            {
                cv.wait(lk);
            }
        }
        else
        {
            ret = CallRpcUpdateClusterConfig(new_cnf, version, need_retry);
        }

        if (need_retry)
        {
            LOG(ERROR) << "UpdateClusterConfig failed, retrying...";
            retry_count++;
        }
    }

    return ret;
}

bool DataStoreServiceClient::PutAll(std::vector<txservice::FlushRecord> &batch,
                                    const txservice::TableName &table_name,
                                    const txservice::TableSchema *table_schema,
                                    uint32_t node_group)
{
    bthread::Mutex mtx;
    bthread::ConditionVariable cv;
    bool done = false;
    bool ret = false;
    std::string kv_table_name =
        table_schema->GetKVCatalogInfo()->kv_table_name_;

    uint32_t req_shard_id = cluster_manager_.GetShardIdByKey("");
    if (IsLocalShard(req_shard_id))
    {
        std::vector<EloqDS::remote::BatchWriteRecordsRequest::record> records;
        uint64_t write_batch_size = 0;
        uint64_t now = txservice::LocalCcShards::ClockTsInMillseconds();
        for (auto &flush_rec : batch)
        {
            txservice::TxKey key = flush_rec.Key();
            const EloqKV::EloqKey *redis_key = key.GetKey<EloqKV::EloqKey>();
            if (flush_rec.payload_status_ == txservice::RecordStatus::Normal &&
                flush_rec.Payload()->GetTTL() > now)
            {
                records.emplace_back();
                ::EloqDS::remote::BatchWriteRecordsRequest::record *rec =
                    &records.back();
                auto &rec_buffer = *rec->mutable_value();
                EloqShare::SerializeFlushRecord(flush_rec, rec_buffer);
                write_batch_size += redis_key->Length();
                write_batch_size += rec_buffer.size();
                rec->set_key(redis_key->ToString());
            }
            else
            {
                write_batch_size += redis_key->Length();
                records.emplace_back();
                auto *rec = &records.back();
                rec->set_key(redis_key->ToString());
                rec->set_is_deleted(true);
            }

            if (write_batch_size >= 1024 * 1024 /*1MB hard coded for now*/)
            {
                done = false;
                data_store_service_->BatchWriteRecords(
                    req_shard_id,
                    std::move(records),
                    kv_table_name,
                    [this, &ret, &table_name, &batch, &mtx, &cv, &done](
                        const ::EloqDS::remote::CommonResult &result)
                    {
                        ::EloqDS::remote::DataStoreError error_code =
                            static_cast<::EloqDS::remote::DataStoreError>(
                                result.error_code());
                        if (error_code == ::EloqDS::remote::DataStoreError::
                                              REQUESTED_NODE_NOT_OWNER)
                        {
                            cluster_manager_.HandleShardingError(result);
                            // we don't need retry, let txservice handle it
                        }
                        ret = error_code ==
                              ::EloqDS::remote::DataStoreError::NO_ERROR;
                        if (!ret)
                        {
                            LOG(ERROR)
                                << "PutAll failed, table:"
                                << table_name.String()
                                << ", batch size:" << batch.size()
                                << ", error_message:" << result.error_msg();
                        }

                        std::unique_lock<bthread::Mutex> lk(mtx);
                        done = true;
                        cv.notify_one();
                    });

                std::unique_lock<bthread::Mutex> lk(mtx);
                while (!done)
                {
                    cv.wait(lk);
                }
                done = false;

                if (!ret)
                {
                    // Abort if current batch flush failed.
                    return false;
                }

                // collect metrics: flush rows
                if (metrics::enable_kv_metrics)
                {
                    metrics::kv_meter->Collect(
                        metrics::NAME_KV_FLUSH_ROWS_TOTAL,
                        records.size(),
                        "base");
                }
                records.clear();
                write_batch_size = 0;
            }
        }

        if (write_batch_size > 0)
        {
            done = false;
            data_store_service_->BatchWriteRecords(
                req_shard_id,
                std::move(records),
                kv_table_name,
                [this, &ret, &table_name, &batch, &mtx, &cv, &done](
                    const ::EloqDS::remote::CommonResult &result)
                {
                    ::EloqDS::remote::DataStoreError error_code =
                        static_cast<::EloqDS::remote::DataStoreError>(
                            result.error_code());
                    if (error_code == ::EloqDS::remote::DataStoreError::
                                          REQUESTED_NODE_NOT_OWNER)
                    {
                        cluster_manager_.HandleShardingError(result);
                        // we don't need retry, let txservice handle it
                    }

                    ret = error_code ==
                          ::EloqDS::remote::DataStoreError::NO_ERROR;
                    if (!ret)
                    {
                        LOG(ERROR)
                            << "PutAll failed, table:" << table_name.String()
                            << ", batch size:" << batch.size()
                            << ", error_message:" << result.error_msg();
                    }

                    std::unique_lock<bthread::Mutex> lk(mtx);
                    done = true;
                    cv.notify_one();
                });

            std::unique_lock<bthread::Mutex> lk(mtx);
            while (!done)
            {
                cv.wait(lk);
            }
            done = false;

            if (!ret)
            {
                return false;
            }

            // collect metrics: flush rows
            if (metrics::enable_kv_metrics)
            {
                metrics::kv_meter->Collect(
                    metrics::NAME_KV_FLUSH_ROWS_TOTAL, records.size(), "base");
            }
        }
    }
    else
    {
        ret = CallRpcPutAll(batch, table_name, table_schema, node_group);
    }

    return ret;
}

bool DataStoreServiceClient::CkptEnd(const txservice::TableName &table_name,
                                     const txservice::TableSchema *schema,
                                     uint32_t node_group,
                                     uint64_t version)
{
    // We don't need implement retry here because in the case of data shard
    // move, we need to redo ckpt from start
    bool ret = false;
    const uint32_t req_shard_id = cluster_manager_.GetShardIdByKey("");
    if (IsLocalShard(req_shard_id))
    {
        const std::string kv_cf_name =
            schema->GetKVCatalogInfo()->kv_table_name_;
        bthread::Mutex mtx;
        bthread::ConditionVariable cv;
        bool done = false, success = false;
        data_store_service_->CkptEnd(
            req_shard_id,
            kv_cf_name,
            [this, &done, &mtx, &cv, &success](
                const ::EloqDS::remote::CommonResult &result)
            {
                std::unique_lock<bthread::Mutex> lk(mtx);
                auto error_code = static_cast<::EloqDS::remote::DataStoreError>(
                    result.error_code());
                done = true;
                success = (static_cast<::EloqDS::remote::DataStoreError>(
                               error_code) ==
                           ::EloqDS::remote::DataStoreError::NO_ERROR);
                if (error_code ==
                    ::EloqDS::remote::DataStoreError::REQUESTED_NODE_NOT_OWNER)
                {
                    cluster_manager_.HandleShardingError(result);
                    // we don't need retry, let txservice handle it
                }
                cv.notify_one();
            });

        std::unique_lock<bthread::Mutex> lk(mtx);
        while (!done)
        {
            cv.wait(lk);
        }
        ret = success;
    }
    else
    {
        ret = CallRpcCkptEnd(table_name, schema, version);
    }
    return ret;
}

void DataStoreServiceClient::UpsertTable(
    const txservice::TableSchema *old_table_schema,
    const txservice::TableSchema *new_table_schema,
    txservice::OperationType op_type,
    uint64_t commit_ts,
    txservice::NodeGroupId ng_id,
    int64_t tx_term,
    txservice::CcHandlerResult<txservice::Void> *hd_res,
    const txservice::AlterTableInfo *alter_table_info,
    txservice::CcRequestBase *cc_req,
    txservice::CcShard *ccs,
    txservice::CcErrorCode *err_code)
{
    std::string table_name = old_table_schema->GetBaseTableName().String();
    std::string old_schema_img =
        old_table_schema->GetKVCatalogInfo()->kv_table_name_;
    std::string new_schema_img =
        new_table_schema->GetKVCatalogInfo()->kv_table_name_;
    UpsertTableWithRetry(table_name,
                         old_schema_img,
                         new_schema_img,
                         op_type,
                         commit_ts,
                         ng_id,
                         tx_term,
                         hd_res,
                         alter_table_info,
                         cc_req,
                         ccs,
                         err_code,
                         0);
}

void DataStoreServiceClient::UpsertTableWithRetry(
    const std::string table_name,
    const std::string old_schema_img,
    const std::string new_schema_img,
    txservice::OperationType op_type,
    uint64_t commit_ts,
    txservice::NodeGroupId ng_id,
    int64_t tx_term,
    txservice::CcHandlerResult<txservice::Void> *hd_res,
    const txservice::AlterTableInfo *alter_table_info,
    txservice::CcRequestBase *cc_req,
    txservice::CcShard *ccs,
    txservice::CcErrorCode *err_code,
    int retry_count)
{
    const uint32_t req_shard_id = cluster_manager_.GetShardIdByKey("");
    if (IsLocalShard(req_shard_id))
    {
        int64_t term;
        if (!txservice::IsStandbyTx(tx_term))
        {
            term = txservice::Sharder::Instance().TryPinNodeGroupData(ng_id);
        }
        else
        {
            // for standby node
            term = txservice::Sharder::Instance().TryPinStandbyNodeGroupData();
        }

        if (term < 0)
        {
            if (hd_res != nullptr)
            {
                hd_res->SetError(txservice::CcErrorCode::TX_NODE_NOT_LEADER);
            }
            else
            {
                *err_code = txservice::CcErrorCode::NG_TERM_CHANGED;
                ccs->Enqueue(cc_req);
            }
            return;
        }
        std::shared_ptr<void> defer_unpin(
            nullptr,
            [ng_id](void *)
            { txservice::Sharder::Instance().UnpinNodeGroupData(ng_id); });

        if (term != tx_term)
        {
            if (hd_res != nullptr)
            {
                hd_res->SetError(txservice::CcErrorCode::NG_TERM_CHANGED);
            }
            else
            {
                *err_code = txservice::CcErrorCode::NG_TERM_CHANGED;
                ccs->Enqueue(cc_req);
            }
            return;
        }
        data_store_service_->UpsertTable(
            req_shard_id,
            table_name,
            old_schema_img,
            new_schema_img,
            EloqShare::UpsertTableOperationTypeConverter(op_type),
            commit_ts,
            [this,
             &table_name,
             &old_schema_img,
             &new_schema_img,
             op_type,
             commit_ts,
             ng_id,
             tx_term,
             hd_res,
             alter_table_info,
             err_code,
             cc_req,
             ccs,
             defer_unpin,
             retry_count](const ::EloqDS::remote::CommonResult &result)
            {
                ::EloqDS::remote::DataStoreError error_code =
                    static_cast<::EloqDS::remote::DataStoreError>(
                        result.error_code());
                if (error_code ==
                    ::EloqDS::remote::DataStoreError::REQUESTED_NODE_NOT_OWNER)
                {
                    cluster_manager_.HandleShardingError(result);
                    if (retry_count < retry_limit_)
                    {
                        CallRpcUpsertTable(table_name,
                                           old_schema_img,
                                           new_schema_img,
                                           op_type,
                                           commit_ts,
                                           ng_id,
                                           tx_term,
                                           hd_res,
                                           alter_table_info,
                                           cc_req,
                                           ccs,
                                           err_code,
                                           (retry_count + 1));
                        return;
                    }
                }

                if (hd_res)
                {
                    hd_res->SetError(
                        error_code == ::EloqDS::remote::DataStoreError::NO_ERROR
                            ? txservice::CcErrorCode::NO_ERROR
                            : txservice::CcErrorCode::DATA_STORE_ERR);
                }
                else if (err_code)
                {
                    *err_code =
                        error_code == ::EloqDS::remote::DataStoreError::NO_ERROR
                            ? txservice::CcErrorCode::NO_ERROR
                            : txservice::CcErrorCode::DATA_STORE_ERR;
                    ccs->Enqueue(cc_req);
                }
                else
                {
                    assert(false);
                }
            });
    }
    else
    {
        CallRpcUpsertTable(table_name,
                           old_schema_img,
                           new_schema_img,
                           op_type,
                           commit_ts,
                           ng_id,
                           tx_term,
                           hd_res,
                           alter_table_info,
                           cc_req,
                           ccs,
                           err_code,
                           retry_count);
    }
}

void DataStoreServiceClient::FetchTableCatalog(
    const txservice::TableName &ccm_table_name,
    txservice::FetchCatalogCc *fetch_cc)
{
    FetchTableCatalogWithRetry(ccm_table_name, fetch_cc, 0);
}

void DataStoreServiceClient::FetchTableCatalogWithRetry(
    const txservice::TableName &ccm_table_name,
    txservice::FetchCatalogCc *fetch_cc,
    uint16_t retry_count)
{
    const uint32_t req_shard_id = cluster_manager_.GetShardIdByKey("");
    std::string ccm_table_name_str = ccm_table_name.String();
    if (IsLocalShard(req_shard_id))
    {
        data_store_service_->FetchTableCatalog(
            req_shard_id,
            ccm_table_name.String(),
            [this, &ccm_table_name_str, fetch_cc, &retry_count](
                std::string &&schema_image,
                bool found,
                uint64_t version_ts,
                const ::EloqDS::remote::CommonResult &result)
            {
                ::EloqDS::remote::DataStoreError error_code =
                    static_cast<::EloqDS::remote::DataStoreError>(
                        result.error_code());
                DLOG(INFO) << "FetchTableCatalog: "
                           << fetch_cc->CatalogName().String()
                           << ", found: " << found
                           << ", version_ts: " << version_ts
                           << ", error_code: " << static_cast<int>(error_code);
                if (error_code ==
                    ::EloqDS::remote::DataStoreError::REQUESTED_NODE_NOT_OWNER)
                {
                    cluster_manager_.HandleShardingError(result);
                    // retry FetchTableCatalog
                    if (retry_count < retry_limit_)
                    {
                        DLOG(INFO) << "Retry FetchTableCatalog: "
                                   << fetch_cc->CatalogName().String()
                                   << ", retry_cnt: " << retry_count;
                        CallRpcFetchTableCatalog(
                            ccm_table_name_str, fetch_cc, retry_count + 1);
                        return;
                    }
                }

                fetch_cc->CatalogImage() = schema_image;
                fetch_cc->CommitTs() = version_ts;
                fetch_cc->SetFinish(found ? txservice::RecordStatus::Normal
                                          : txservice::RecordStatus::Deleted,
                                    static_cast<int>(error_code));
            });
    }
    else
    {
        CallRpcFetchTableCatalog(ccm_table_name_str, fetch_cc, retry_count);
    }
}

void DataStoreServiceClient::FetchCurrentTableStatistics(
    const txservice::TableName &ccm_table_name,
    txservice::FetchTableStatisticsCc *fetch_cc)
{
    LOG(ERROR) << "FetchCurrentTableStatistics not implemented";
    assert(false);
}

void DataStoreServiceClient::FetchTableStatistics(
    const txservice::TableName &ccm_table_name,
    txservice::FetchTableStatisticsCc *fetch_cc)
{
    LOG(ERROR) << "FetchTableStatistics not implemented";
    assert(false);
}

bool DataStoreServiceClient::UpsertTableStatistics(
    const txservice::TableName &ccm_table_name,
    const std::unordered_map<txservice::TableName,
                             std::pair<uint64_t, std::vector<txservice::TxKey>>>
        &sample_pool_map,
    uint64_t version)
{
    LOG(ERROR) << "UpsertTableStatistics not implemented";
    assert(false);
    return true;
}

void DataStoreServiceClient::FetchTableRanges(
    txservice::FetchTableRangesCc *fetch_cc)
{
    LOG(ERROR) << "FetchTableRanges not implemented";
    assert(false);
}

void DataStoreServiceClient::FetchRangeSlices(
    txservice::FetchRangeSlicesReq *fetch_cc)
{
    LOG(ERROR) << "FetchRangeSlices not implemented";
    assert(false);
}

bool DataStoreServiceClient::DeleteOutOfRangeData(
    const txservice::TableName &table_name,
    int32_t partition_id,
    const txservice::TxKey *start_key,
    const txservice::TableSchema *table_schema)
{
    LOG(ERROR) << "DeleteOutOfRangeData not implemented";
    assert(false);
    return true;
}

bool DataStoreServiceClient::GetNextRangePartitionId(
    const txservice::TableName &tablename,
    uint32_t range_cnt,
    int32_t &out_next_partition_id,
    int retry_count)
{
    LOG(ERROR) << "GetNextRangePartitionId not implemented";
    assert(false);
    return true;
}

bool DataStoreServiceClient::Read(const txservice::TableName &table_name,
                                  const txservice::TxKey &key,
                                  txservice::TxRecord &rec,
                                  bool &found,
                                  uint64_t &version_ts,
                                  const txservice::TableSchema *table_schema)
{
    LOG(ERROR) << "Read not implemented";
    return true;
}

std::unique_ptr<txservice::store::DataStoreScanner>
DataStoreServiceClient::ScanForward(
    const txservice::TableName &table_name,
    uint32_t ng_id,
    const txservice::TxKey &start_key,
    bool inclusive,
    uint8_t key_parts,
    const std::vector<txservice::store::DataStoreSearchCond> &search_cond,
    const txservice::KeySchema *key_schema,
    const txservice::RecordSchema *rec_schema,
    const txservice::KVCatalogInfo *kv_info,
    bool scan_foward)
{
    std::unique_ptr<DataStoreServiceScanner> scanner =
        std::make_unique<DataStoreServiceScanner>(this,
                                                  key_schema,
                                                  rec_schema,
                                                  table_name,
                                                  kv_info,
                                                  start_key,
                                                  inclusive,
                                                  search_cond,
                                                  scan_foward);
    scanner->Init();
    return scanner;
}

txservice::store::DataStoreHandler::DataStoreOpStatus
DataStoreServiceClient::LoadRangeSlice(
    const txservice::TableName &table_name,
    const txservice::KVCatalogInfo *kv_info,
    uint32_t range_partition_id,
    txservice::LoadRangeSliceRequest *load_slice_req)
{
    LOG(ERROR) << "LoadRangeSlice not implemented";
    assert(false);
    return txservice::store::DataStoreHandler::DataStoreOpStatus::Error;
}

bool DataStoreServiceClient::UpdateRangeSlices(
    const txservice::TableName &table_name,
    uint64_t version,
    txservice::TxKey range_start_key,
    std::vector<const txservice::StoreSlice *> slices,
    int32_t partition_id,
    uint64_t range_version)
{
    LOG(ERROR) << "UpdateRangeSlices not implemented";
    assert(false);
    return true;
}

bool DataStoreServiceClient::UpsertRanges(
    const txservice::TableName &table_name,
    std::vector<txservice::SplitRangeInfo> range_info,
    uint64_t version)
{
    LOG(ERROR) << "UpsertRanges not implemented";
    assert(false);
    return true;
}

bool DataStoreServiceClient::FetchTable(const txservice::TableName &table_name,
                                        std::string &schema_image,
                                        bool &found,
                                        uint64_t &version_ts) const
{
    LOG(ERROR) << "FetchTable not implemented";
    assert(false);
    return true;
}

bool DataStoreServiceClient::DiscoverAllTableNames(
    std::vector<std::string> &norm_name_vec,
    const std::function<void()> *yield_fptr,
    const std::function<void()> *resume_fptr) const
{
    LOG(ERROR) << "DiscoverAllTableNames not implemented";
    assert(false);
    return true;
}

bool DataStoreServiceClient::UpsertDatabase(std::string_view db,
                                            std::string_view definition) const
{
    LOG(ERROR) << "UpsertDatabase not implemented";
    assert(false);
    return true;
}

bool DataStoreServiceClient::DropDatabase(std::string_view db) const
{
    LOG(ERROR) << "DropDatabase not implemented";
    assert(false);
    return true;
}

bool DataStoreServiceClient::FetchDatabase(
    std::string_view db,
    std::string &definition,
    bool &found,
    const std::function<void()> *yield_fptr,
    const std::function<void()> *resume_fptr) const
{
    LOG(ERROR) << "FetchDatabase not implemented";
    assert(false);
    return true;
}

bool DataStoreServiceClient::FetchAllDatabase(
    std::vector<std::string> &dbnames,
    const std::function<void()> *yield_fptr,
    const std::function<void()> *resume_fptr) const
{
    LOG(ERROR) << "FetchAllDatabase not implemented";
    assert(false);
    return true;
}

bool DataStoreServiceClient::DropKvTable(const std::string &kv_table_name) const
{
    LOG(ERROR) << "DropKvTable not implemented";
    assert(false);
    return true;
}

void DataStoreServiceClient::DropKvTableAsync(
    const std::string &kv_table_name) const
{
    LOG(ERROR) << "DropKvTableAsync not implemented";
    assert(false);
}

std::string DataStoreServiceClient::CreateKVCatalogInfo(
    const txservice::TableSchema *table_schema) const
{
    LOG(ERROR) << "CreateKVCatalogInfo not implemented";
    assert(false);
    return "";
}

txservice::KVCatalogInfo::uptr DataStoreServiceClient::DeserializeKVCatalogInfo(
    const std::string &kv_info_str, size_t &offset) const
{
    LOG(ERROR) << "DeserializeKVCatalogInfo not implemented";
    assert(false);
    return nullptr;
}

std::string DataStoreServiceClient::CreateNewKVCatalogInfo(
    const txservice::TableName &table_name,
    const txservice::TableSchema *current_table_schema,
    txservice::AlterTableInfo &alter_table_info)
{
    LOG(ERROR) << "CreateNewKVCatalogInfo not implemented";
    assert(false);
    return "";
}

bool DataStoreServiceClient::PutArchivesAll(
    uint32_t node_group,
    const txservice::TableName &table_name,
    const txservice::KVCatalogInfo *kv_info,
    std::vector<txservice::FlushRecord> &batch)
{
    LOG(ERROR) << "PutArchivesAll not implemented";
    assert(false);
    return true;
}

bool DataStoreServiceClient::CopyBaseToArchive(
    std::vector<txservice::TxKey> &batch,
    uint32_t node_group,
    const txservice::TableName &table_name,
    const txservice::TableSchema *table_schema)
{
    LOG(ERROR) << "CopyBaseToArchive not implemented";
    assert(false);
    return true;
}

bool DataStoreServiceClient::FetchArchives(
    const txservice::TableName &table_name,
    const txservice::KVCatalogInfo *kv_info,
    const txservice::TxKey &key,
    std::vector<txservice::VersionTxRecord> &archives,
    uint64_t from_ts)
{
    LOG(ERROR) << "FetchArchives not implemented";
    assert(false);
    return true;
}

bool DataStoreServiceClient::FetchVisibleArchive(
    const txservice::TableName &table_name,
    const txservice::KVCatalogInfo *kv_info,
    const txservice::TxKey &key,
    const uint64_t upper_bound_ts,
    txservice::TxRecord &rec,
    txservice::RecordStatus &rec_status,
    uint64_t &commit_ts)
{
    LOG(ERROR) << "FetchVisibleArchive not implemented";
    assert(false);
    return true;
}

bool DataStoreServiceClient::NeedCopyRange() const
{
    LOG(ERROR) << "NeedCopyRange not implemented";
    assert(false);
    return true;
}

void DataStoreServiceClient::RestoreTxCache(txservice::NodeGroupId cc_ng_id,
                                            int64_t cc_ng_term)
{
    LOG(ERROR) << "RestoreTxCache not implemented";
    assert(false);
}

bool DataStoreServiceClient::OnLeaderStart(uint32_t *next_leader_node)
{
    return true;
}

void DataStoreServiceClient::OnStartFollowing()
{
}

void DataStoreServiceClient::OnShutdown()
{
}

bool DataStoreServiceClient::IsLocalShard(uint32_t shard_id)
{
    // this is a temporary solution for scale up scenario (from one smaller node
    // to another bigger node)
    return cluster_manager_.IsOwnerOfShard(shard_id);
}

txservice::store::DataStoreHandler::DataStoreOpStatus
DataStoreServiceClient::FetchRecord(txservice::FetchRecordCc *fetch_cc)
{
    if (metrics::enable_kv_metrics)
    {
        fetch_cc->start_ = metrics::Clock::now();
    }

    uint32_t req_shard_id = cluster_manager_.GetShardIdByKey("");
    if (IsLocalShard(req_shard_id))
    {
        auto key = ::EloqDS::remote::FetchRecordsRequest::key();
        key.set_key_str(fetch_cc->tx_key_.ToString());
        key.set_kv_table_name_str(
            fetch_cc->table_schema_->GetKVCatalogInfo()->kv_table_name_);
        key.set_table_type(static_cast<EloqDS::remote::CcTableType>(
            fetch_cc->table_name_->Type()));

        bool ret = data_store_service_->FetchRecords(
            req_shard_id,
            std::vector<::EloqDS::remote::FetchRecordsRequest::key>({key}),
            [this, fetch_cc](
                std::vector<::EloqDS::remote::FetchRecordsResponse::record>
                    &&records,
                const ::EloqDS::remote::CommonResult &result)
            {
                if (metrics::enable_kv_metrics)
                {
                    metrics::kv_meter->CollectDuration(
                        metrics::NAME_KV_READ_DURATION, fetch_cc->start_);
                    metrics::kv_meter->Collect(metrics::NAME_KV_READ_TOTAL, 1);
                }

                ::EloqDS::remote::DataStoreError error_code =
                    static_cast<::EloqDS::remote::DataStoreError>(
                        result.error_code());
                if (error_code ==
                    ::EloqDS::remote::DataStoreError::REQUESTED_NODE_NOT_OWNER)
                {
                    cluster_manager_.HandleShardingError(result);
                    uint16_t &retry_cnt = fetch_cc->RetryCnt();
                    if (retry_cnt < retry_limit_)
                    {
                        retry_cnt++;
                        CallRpcFetchRecord(fetch_cc);
                        return;
                    }
                    retry_cnt = 0;
                }
                fetch_cc->rec_status_ = records[0].is_deleted()
                                            ? txservice::RecordStatus::Deleted
                                            : txservice::RecordStatus::Normal;
                fetch_cc->rec_str_ = records[0].payload();
                fetch_cc->rec_ts_ = records[0].version();
                fetch_cc->SetFinish(static_cast<int>(error_code));
            });

        return ret ? DataStoreOpStatus::Success : DataStoreOpStatus::Error;
    }
    else
    {
        return CallRpcFetchRecord(fetch_cc);
    }
}

void DataStoreServiceClient::ScanOpenWithRetry(
    EloqDS::remote::ScanRequest &scan_req,
    std::function<void(const EloqDS::remote::ScanResponse &)> on_finish,
    uint16_t retry_count)
{
    const uint32_t req_shard_id = cluster_manager_.GetShardIdByKey("");
    if (IsLocalShard(req_shard_id))
    {
        scan_req.set_req_shard_id(req_shard_id);
        data_store_service_->ScanOpen(
            scan_req,
            [this, &scan_req, &retry_count, on_finish](
                const ::EloqDS::remote::ScanResponse &response)
            {
                auto &result = response.result();
                ::EloqDS::remote::DataStoreError error_code =
                    static_cast<::EloqDS::remote::DataStoreError>(
                        result.error_code());
                DLOG(INFO) << "Scan open: result size: "
                           << response.items_size()
                           << ", error_code: " << static_cast<int>(error_code)
                           << ", error_msg: " << result.error_msg();
                if (error_code ==
                    ::EloqDS::remote::DataStoreError::REQUESTED_NODE_NOT_OWNER)
                {
                    cluster_manager_.HandleShardingError(result);
                    if (retry_count < retry_limit_)
                    {
                        DLOG(INFO)
                            << "Retry ScanOpen: retry_cnt: " << retry_count;
                        CallRpcScanOpen(scan_req, on_finish, retry_count + 1);
                        return;
                    }
                }

                on_finish(response);
            });
    }
    else
    {
        CallRpcScanOpen(scan_req, on_finish, retry_count);
    }
}

void DataStoreServiceClient::CallRpcScanOpen(
    EloqDS::remote::ScanRequest &scan_req,
    std::function<void(const EloqDS::remote::ScanResponse &)> on_finish,
    uint16_t retry_count)
{
    DLOG(INFO) << "CallRpcScanOpen, retry_count: " << retry_count;
    uint32_t req_shard_id = cluster_manager_.GetShardIdByKey("");
    auto channel =
        cluster_manager_.GetDataStoreServiceChannelByShardId(req_shard_id);
    if (!channel)
    {
        DSSNode dss_node = cluster_manager_.GetDSSNodeByKey("");
        LOG(ERROR) << "CallRpcScanOpen can not get service channel to ip: "
                   << dss_node.host_name_ << " port: " << dss_node.port_;
        return;
    }

    EloqDS::remote::DataStoreRpcService_Stub stub(channel.get());
    auto *closure = new ScanClosure(
        *this, ScanOpType::SCAN_OPEN, scan_req, on_finish, retry_count);
    brpc::Controller &cntl = *closure->Controller();
    cntl.set_timeout_ms(5000);
    auto *response = closure->ScanResponse();
    stub.ScanOpen(&cntl, &scan_req, response, closure);
}

void DataStoreServiceClient::ScanNextWithRetry(
    EloqDS::remote::ScanRequest &scan_req,
    std::function<void(const EloqDS::remote::ScanResponse &)> on_finish,
    uint16_t retry_count)
{
    const uint32_t req_shard_id = cluster_manager_.GetShardIdByKey("");
    if (IsLocalShard(req_shard_id))
    {
        scan_req.set_req_shard_id(req_shard_id);
        data_store_service_->ScanNext(
            scan_req,
            [this, &scan_req, &retry_count, on_finish](
                const ::EloqDS::remote::ScanResponse &response)
            {
                auto &result = response.result();
                ::EloqDS::remote::DataStoreError error_code =
                    static_cast<::EloqDS::remote::DataStoreError>(
                        result.error_code());
                DLOG(INFO) << "Scan next: result size: "
                           << response.items_size()
                           << ", error_code: " << static_cast<int>(error_code)
                           << ", error_msg: " << result.error_msg();
                if (error_code ==
                    ::EloqDS::remote::DataStoreError::REQUESTED_NODE_NOT_OWNER)
                {
                    cluster_manager_.HandleShardingError(result);
                    if (retry_count < retry_limit_)
                    {
                        DLOG(INFO)
                            << "Retry ScanNext: retry_cnt: " << retry_count;
                        CallRpcScanNext(scan_req, on_finish, retry_count + 1);
                        return;
                    }
                }

                on_finish(response);
            });
    }
    else
    {
        CallRpcScanNext(scan_req, on_finish, retry_count);
    }
}

void DataStoreServiceClient::CallRpcScanNext(
    EloqDS::remote::ScanRequest &scan_req,
    std::function<void(const EloqDS::remote::ScanResponse &)> on_finish,
    uint16_t retry_count)
{
    DLOG(INFO) << "CallRpcScanOpen, retry_count: " << retry_count;
    uint32_t req_shard_id = cluster_manager_.GetShardIdByKey("");
    auto channel =
        cluster_manager_.GetDataStoreServiceChannelByShardId(req_shard_id);
    if (!channel)
    {
        DSSNode dss_node = cluster_manager_.GetDSSNodeByKey("");
        LOG(ERROR) << "CallRpcScanNext can not get service channel to ip: "
                   << dss_node.host_name_ << " port: " << dss_node.port_;
        return;
    }

    EloqDS::remote::DataStoreRpcService_Stub stub(channel.get());
    auto *closure = new ScanClosure(
        *this, ScanOpType::SCAN_NEXT, scan_req, on_finish, retry_count);
    brpc::Controller &cntl = *closure->Controller();
    cntl.set_timeout_ms(5000);
    auto *response = closure->ScanResponse();
    stub.ScanNext(&cntl, &scan_req, response, closure);
}

void DataStoreServiceClient::ScanCloseWithRetry(
    EloqDS::remote::ScanRequest &scan_req,
    std::function<void(const EloqDS::remote::ScanResponse &)> on_finish,
    uint16_t retry_count)
{
    const uint32_t req_shard_id = cluster_manager_.GetShardIdByKey("");
    if (IsLocalShard(req_shard_id))
    {
        scan_req.set_req_shard_id(req_shard_id);
        data_store_service_->ScanClose(
            scan_req,
            [this, &scan_req, &retry_count, on_finish](
                const ::EloqDS::remote::ScanResponse &response)
            {
                auto &result = response.result();
                ::EloqDS::remote::DataStoreError error_code =
                    static_cast<::EloqDS::remote::DataStoreError>(
                        result.error_code());
                DLOG(INFO) << "Scan close: "
                           << ", error_code: " << static_cast<int>(error_code)
                           << ", error_msg: " << result.error_msg();
                if (error_code ==
                    ::EloqDS::remote::DataStoreError::REQUESTED_NODE_NOT_OWNER)
                {
                    cluster_manager_.HandleShardingError(result);
                    if (retry_count < retry_limit_)
                    {
                        DLOG(INFO)
                            << "Retry ScanClose: retry_cnt: " << retry_count;
                        CallRpcScanClose(scan_req, on_finish, retry_count + 1);
                        return;
                    }
                }

                on_finish(response);
            });
    }
    else
    {
        CallRpcScanClose(scan_req, on_finish, retry_count);
    }
}

void DataStoreServiceClient::CallRpcScanClose(
    EloqDS::remote::ScanRequest &scan_req,
    std::function<void(const EloqDS::remote::ScanResponse &)> on_finish,
    uint16_t retry_count)
{
    DLOG(INFO) << "CallRpcScanClose, retry_count: " << retry_count;
    uint32_t req_shard_id = cluster_manager_.GetShardIdByKey("");
    auto channel =
        cluster_manager_.GetDataStoreServiceChannelByShardId(req_shard_id);
    if (!channel)
    {
        DSSNode dss_node = cluster_manager_.GetDSSNodeByKey("");
        LOG(ERROR) << "CallRpcScanClose can not get service channel to ip: "
                   << dss_node.host_name_ << " port: " << dss_node.port_;
        return;
    }

    EloqDS::remote::DataStoreRpcService_Stub stub(channel.get());
    auto *closure = new ScanClosure(
        *this, ScanOpType::SCAN_CLOSE, scan_req, on_finish, retry_count);
    brpc::Controller &cntl = *closure->Controller();
    cntl.set_timeout_ms(5000);
    auto *response = closure->ScanResponse();
    stub.ScanClose(&cntl, &scan_req, response, closure);
}

void DataStoreServiceClient::CallRpcFetchRecordInBatch()
{
    while (true)
    {
        std::unique_lock<bthread::Mutex> lk(ds_service_mutex_);
        ds_service_cv_.wait(lk);
        if (ds_serv_shutdown_indicator_.load(std::memory_order_acquire) &&
            remote_fetch_cc_queue_.empty())
        {
            lk.unlock();
            break;
        }

        if (remote_fetch_cc_queue_.empty())
        {
            lk.unlock();
            continue;
        }

        // if flying remote fetch count is too high, wait for 5ms
        uint32_t sleep_rounds = 0;
        while (flying_remote_fetch_count_.load(std::memory_order_acquire) >=
                   50 &&
               sleep_rounds < 10)
        {
            DLOG(INFO) << "sleep flying_remote_fetch_count_: "
                       << flying_remote_fetch_count_.load(
                              std::memory_order_acquire);
            lk.unlock();
            bthread_usleep(1000);
            sleep_rounds++;
            lk.lock();
        }

        uint32_t req_shard_id = cluster_manager_.GetShardIdByKey("");
        auto channel =
            cluster_manager_.GetDataStoreServiceChannelByShardId(req_shard_id);
        if (!channel)
        {
            DSSNode dss_node = cluster_manager_.GetDSSNodeByKey("");
            LOG(ERROR) << "CallRpcFetchRecordInBatch can not get service "
                          "channel to primary node "
                       << dss_node.host_name_ << ":" << dss_node.port_;
            // sleep a while wait for channel becoming ready
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        FetchRecordsRpcClosure *remote_fetch_closure =
            new FetchRecordsRpcClosure(
                100, *this, req_shard_id, 0, flying_remote_fetch_count_);
        std::unique_ptr<FetchRecordsRpcClosure> closure_guard(
            remote_fetch_closure);
        remote_fetch_closure->SetChannel(channel);
        auto *req = remote_fetch_closure->FetchRecordsRequest();

        uint16_t batch_size = 0;
        while (!remote_fetch_cc_queue_.empty() && batch_size < 100)
        {
            auto *fetch_cc = remote_fetch_cc_queue_.front();
            remote_fetch_cc_queue_.pop_front();

            remote_fetch_closure->AddFetchCc(fetch_cc);
            auto *key = req->add_keys();
            std::string key_str = fetch_cc->tx_key_.ToString();
            key->set_key_str(std::move(key_str));
            key->set_kv_table_name_str(
                fetch_cc->table_schema_->GetKVCatalogInfo()->kv_table_name_);
            key->set_table_type(static_cast<EloqDS::remote::CcTableType>(
                fetch_cc->table_name_->Type()));
            batch_size++;
        }

        // nothing to do if no req has been added
        if (batch_size == 0)
        {
            continue;
        }

        remote_fetch_closure->Controller()->set_timeout_ms(5000);
        remote_fetch_closure->Controller()->set_write_to_socket_in_background(
            true);
        EloqDS::remote::DataStoreRpcService_Stub stub(channel.get());
        stub.FetchRecords(remote_fetch_closure->Controller(),
                          remote_fetch_closure->FetchRecordsRequest(),
                          remote_fetch_closure->FetchRecordsResponse(),
                          remote_fetch_closure);
        flying_remote_fetch_count_.fetch_add(1, std::memory_order_acq_rel);

        closure_guard.release();
    }
}

txservice::store::DataStoreHandler::DataStoreOpStatus
DataStoreServiceClient::CallRpcFetchRecord(txservice::FetchRecordCc *fetch_cc)
{
    if (flying_remote_fetch_count_.load(std::memory_order_relaxed) < 2)
    {
        flying_remote_fetch_count_.fetch_add(1, std::memory_order_acq_rel);

        uint32_t req_shard_id = cluster_manager_.GetShardIdByKey("");
        auto channel =
            cluster_manager_.GetDataStoreServiceChannelByShardId(req_shard_id);
        if (!channel)
        {
            return DataStoreOpStatus::Error;
        }
        FetchRecordsRpcClosure *closure = new FetchRecordsRpcClosure(
            fetch_cc, *this, req_shard_id, 0, flying_remote_fetch_count_);
        closure->SetChannel(channel);
        auto *req = closure->FetchRecordsRequest();
        auto *key = req->add_keys();
        std::string key_str = fetch_cc->tx_key_.ToString();
        key->set_key_str(std::move(key_str));
        key->set_kv_table_name_str(
            fetch_cc->table_schema_->GetKVCatalogInfo()->kv_table_name_);
        key->set_table_type(static_cast<EloqDS::remote::CcTableType>(
            fetch_cc->table_name_->Type()));
        closure->Controller()->set_timeout_ms(5000);
        closure->Controller()->set_write_to_socket_in_background(true);
        EloqDS::remote::DataStoreRpcService_Stub stub(channel.get());
        stub.FetchRecords(closure->Controller(),
                          closure->FetchRecordsRequest(),
                          closure->FetchRecordsResponse(),
                          closure);
    }
    else
    {
        std::unique_lock<bthread::Mutex> lk(ds_service_mutex_);
        remote_fetch_cc_queue_.push_back(fetch_cc);
        DLOG(INFO) << "RemoteFetchRecord queue size: "
                   << remote_fetch_cc_queue_.size()
                   << " flying_remote_fetch_count_: "
                   << flying_remote_fetch_count_.load(
                          std::memory_order_relaxed);
        ds_service_cv_.notify_one();
    }

    return DataStoreOpStatus::Success;
}

bool DataStoreServiceClient::CallRpcPutAll(
    std::vector<txservice::FlushRecord> &batch,
    const txservice::TableName &table_name,
    const txservice::TableSchema *table_schema,
    uint32_t node_group)
{
    DLOG(INFO) << "RemotePutAll, table: " << table_name.String()
               << " batch size: " << batch.size();
    if (batch.empty())
    {
        return true;
    }

    uint32_t req_shard_id = cluster_manager_.GetShardIdByKey("");
    auto channel =
        cluster_manager_.GetDataStoreServiceChannelByShardId(req_shard_id);
    if (!channel)
    {
        DSSNode dss_node = cluster_manager_.GetDSSNodeByKey("");
        LOG(ERROR) << "Can not get service channel to ip: "
                   << dss_node.host_name_ << " port: " << dss_node.port_;
        return false;
    }

    EloqDS::remote::DataStoreRpcService_Stub stub(channel.get());
    EloqDS::remote::BatchWriteRecordsRequest req;
    EloqDS::remote::BatchWriteRecordsResponse resp;
    brpc::Controller cntl;
    cntl.set_timeout_ms(5000);

    // set kv table name
    const std::string &kv_cf_name =
        table_schema->GetKVCatalogInfo()->kv_table_name_;
    req.set_kv_table_name(kv_cf_name);

    uint64_t write_batch_size = 0;
    uint64_t now = txservice::LocalCcShards::ClockTsInMillseconds();
    for (auto &flush_rec : batch)
    {
        txservice::TxKey key = flush_rec.Key();
        const EloqKV::EloqKey *redis_key = key.GetKey<EloqKV::EloqKey>();
        if (flush_rec.payload_status_ == txservice::RecordStatus::Normal &&
            flush_rec.Payload()->GetTTL() > now)
        {
            auto *rec = req.add_records();
            std::string &rec_str = *rec->mutable_value();
            EloqShare::SerializeFlushRecord(flush_rec, rec_str);
            write_batch_size += redis_key->Length();
            write_batch_size += rec_str.size();
            rec->set_key(redis_key->ToString());
        }
        else
        {
            write_batch_size += redis_key->Length();
            auto *rec = req.add_records();
            rec->set_key(redis_key->ToString());
            rec->set_is_deleted(true);
        }

        if (write_batch_size >= 1024 * 1024 /*1MB hard coded for now*/)
        {
            stub.BatchWriteRecords(&cntl, &req, &resp, nullptr);
            if (cntl.Failed())
            {
                auto &result = resp.result();
                LOG(ERROR) << "CallRpcPutAll failed, table:"
                           << table_name.String() << ", result:"
                           << static_cast<int>(result.error_code())
                           << ", batch size:" << req.records_size()
                           << ", error: " << cntl.ErrorText()
                           << ", error code: " << cntl.ErrorCode();
                return false;
            }

            auto &result = resp.result();
            if (result.error_code() != 0)
            {
                LOG(ERROR) << "CallRpcPutAll failed, table:"
                           << table_name.String() << ", result:"
                           << static_cast<int>(result.error_code())
                           << ", batch size:" << req.records_size()
                           << ", error msg: " << result.error_msg();
                if (result.error_code() ==
                    ::EloqDS::remote::DataStoreError::REQUESTED_NODE_NOT_OWNER)
                {
                    cluster_manager_.HandleShardingError(result);
                }

                return false;
            }
            // collect metrics: flush rows
            if (metrics::enable_kv_metrics)
            {
                metrics::kv_meter->Collect(metrics::NAME_KV_FLUSH_ROWS_TOTAL,
                                           req.records_size(),
                                           "base");
            }
            // reset request and response
            resp.Clear();
            req.clear_records();
            write_batch_size = 0;
        }
    }

    if (write_batch_size > 0)
    {
        stub.BatchWriteRecords(&cntl, &req, &resp, nullptr);
        if (cntl.Failed())
        {
            auto &result = resp.result();
            LOG(ERROR) << "CallRpcPutAll failed, table:" << table_name.String()
                       << ", result:" << static_cast<int>(result.error_code())
                       << ", batch size:" << req.records_size()
                       << ", error: " << cntl.ErrorText()
                       << ", error code: " << cntl.ErrorCode();
            return false;
        }

        auto &result = resp.result();
        if (result.error_code() != 0)
        {
            LOG(ERROR) << "CallRpcPutAll failed, table:" << table_name.String()
                       << ", result:" << static_cast<int>(result.error_code())
                       << ", error msg: " << result.error_msg()
                       << ", batch size:" << req.records_size();
            if (result.error_code() ==
                ::EloqDS::remote::DataStoreError::REQUESTED_NODE_NOT_OWNER)
            {
                cluster_manager_.HandleShardingError(result);
            }

            return false;
        }
        // collect metrics: flush rows
        if (metrics::enable_kv_metrics)
        {
            metrics::kv_meter->Collect(
                metrics::NAME_KV_FLUSH_ROWS_TOTAL, req.records_size(), "base");
        }
    }

    return true;
}

bool DataStoreServiceClient::CallRpcCkptEnd(
    const txservice::TableName &table_name,
    const txservice::TableSchema *schema,
    uint64_t version)
{
    uint32_t req_shard_id = cluster_manager_.GetShardIdByKey("");
    auto channel =
        cluster_manager_.GetDataStoreServiceChannelByShardId(req_shard_id);
    if (!channel)
    {
        DSSNode dss_node = cluster_manager_.GetDSSNodeByKey("");
        LOG(ERROR) << "Can not get service channel to ip: "
                   << dss_node.host_name_ << " port: " << dss_node.port_;
        return false;
    }
    EloqDS::remote::DataStoreRpcService_Stub stub(channel.get());
    EloqDS::remote::CkptEndRequest req;
    EloqDS::remote::CkptEndResponse resp;
    brpc::Controller cntl;
    cntl.set_timeout_ms(5000);
    const std::string &kv_cf_name = schema->GetKVCatalogInfo()->kv_table_name_;
    req.set_kv_table_name(kv_cf_name);
    stub.CkptEnd(&cntl, &req, &resp, nullptr);
    if (cntl.Failed())
    {
        auto &result = resp.result();
        LOG(ERROR) << "CallRpcCkptEnd failed, table: " << table_name.String()
                   << ", result: " << static_cast<int>(result.error_code())
                   << ", error: " << cntl.ErrorText()
                   << ", error code: " << cntl.ErrorCode();
        return false;
    }

    auto &result = resp.result();
    auto error_code =
        static_cast<::EloqDS::remote::DataStoreError>(result.error_code());
    if (error_code != 0)
    {
        LOG(ERROR) << "CallRpcCkptEnd failed, table: " << table_name.String()
                   << ", result: " << static_cast<int>(error_code)
                   << " , error msg: " << result.error_msg();
        if (error_code ==
            ::EloqDS::remote::DataStoreError::REQUESTED_NODE_NOT_OWNER)
        {
            cluster_manager_.HandleShardingError(result);
        }
        return false;
    }

    return true;
}

void DataStoreServiceClient::CallRpcFetchTableCatalog(
    const std::string &ccm_table_name_str,
    txservice::FetchCatalogCc *fetch_cc,
    uint16_t retry_count)
{
    uint32_t req_shard_id = cluster_manager_.GetShardIdByKey("");
    auto channel =
        cluster_manager_.GetDataStoreServiceChannelByShardId(req_shard_id);
    if (!channel)
    {
        LOG(ERROR) << "Can not get service channel to primary node";
        fetch_cc->SetFinish(
            txservice::RecordStatus::Unknown,
            static_cast<int>(txservice::CcErrorCode::DATA_STORE_ERR));
        return;
    }

    EloqDS::remote::DataStoreRpcService_Stub stub(channel.get());
    auto *closure = new FetchTableCatalogClosure(
        fetch_cc, *this, req_shard_id, retry_count);
    EloqDS::remote::FetchTableCatalogRequest *req =
        closure->FetchTableCatalogRequest();
    req->set_table_name(ccm_table_name_str);

    stub.FetchTableCatalog(closure->Controller(),
                           req,
                           closure->FetchTableCatalogResponse(),
                           closure);
}

void DataStoreServiceClient::CallRpcUpsertTable(
    const std::string &table_name_str,
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
    uint16_t retry_cnt)
{
    switch (op_type)
    {
    case txservice::OperationType::CreateTable:
        LOG(ERROR) << "EloqKVDataStoreServiceClient::UpsertTable::CreateTable "
                      "not implemented";
        assert(false);
        break;
    case txservice::OperationType::TruncateTable:
    {
        int64_t term;
        if (!txservice::IsStandbyTx(tx_term))
        {
            term = txservice::Sharder::Instance().TryPinNodeGroupData(ng_id);
        }
        else
        {
            // for standby node
            term = txservice::Sharder::Instance().TryPinStandbyNodeGroupData();
        }
        if (term < 0)
        {
            if (hd_res != nullptr)
            {
                hd_res->SetError(txservice::CcErrorCode::TX_NODE_NOT_LEADER);
            }
            else
            {
                *err_code = txservice::CcErrorCode::NG_TERM_CHANGED;
                ccs->Enqueue(cc_req);
            }
            return;
        }
        std::shared_ptr<void> defer_unpin(
            nullptr,
            [ng_id](void *)
            { txservice::Sharder::Instance().UnpinNodeGroupData(ng_id); });

        if (term != tx_term)
        {
            if (hd_res != nullptr)
            {
                hd_res->SetError(txservice::CcErrorCode::NG_TERM_CHANGED);
            }
            else
            {
                *err_code = txservice::CcErrorCode::NG_TERM_CHANGED;
                ccs->Enqueue(cc_req);
            }
            return;
        }

        uint32_t req_shard_id = cluster_manager_.GetShardIdByKey("");
        auto channel =
            cluster_manager_.GetDataStoreServiceChannelByShardId(req_shard_id);
        EloqDS::remote::DataStoreRpcService_Stub stub(channel.get());
        // Create closure
        auto *closure = new UpsertTableClosure(*this,
                                               req_shard_id,
                                               ng_id,
                                               tx_term,
                                               alter_table_info,
                                               retry_cnt,
                                               hd_res,
                                               err_code,
                                               cc_req,
                                               ccs,
                                               defer_unpin);
        EloqDS::remote::UpsertTableRequest *req = closure->UpsertTableRequest();
        brpc::Controller &cntl = *closure->Controller();
        cntl.set_timeout_ms(5000);
        req->set_op_type(EloqShare::UpsertTableOperationTypeConverter(op_type));
        req->set_commit_ts(commit_ts);
        req->set_table_name_str(table_name_str);
        req->set_old_schema_img(old_schema_img);
        req->set_new_schema_img(new_schema_img);

        stub.UpsertTable(&cntl, req, closure->UpsertTableResponse(), closure);
    }
    break;
    case txservice::OperationType::DropTable:
    case txservice::OperationType::Update:
    case txservice::OperationType::AddIndex:
    case txservice::OperationType::DropIndex:
        LOG(ERROR) << "EloqKVDataStoreServiceClient::UpsertTable::DropTable/"
                      "Update/AddIndex/"
                      "DropIndex not implemented";
        assert(false);
        break;
    default:
        LOG(ERROR) << "Unsupported command for "
                      "RocksDBHanlder::UpsertTable, op_type: "
                   << static_cast<int>(op_type);
        break;
    }
}

bool DataStoreServiceClient::CallRpcReadClusterConfig(
    std::unordered_map<uint32_t, std::vector<txservice::NodeConfig>>
        &ng_configs,
    uint64_t &version,
    bool &uninitialized,
    bool &needs_retry)
{
    needs_retry = false;

    uint32_t req_shard_id = cluster_manager_.GetShardIdByKey("");
    auto channel = GetDataStoreServiceChannelByShardId(req_shard_id);

    bool ret = false;
    EloqDS::remote::DataStoreRpcService_Stub stub(channel.get());
    EloqDS::remote::ReadClusterConfigRequest req;
    EloqDS::remote::ReadClusterConfigResponse resp;
    brpc::Controller cntl;
    req.set_key_shard_code(req_shard_id);
    cntl.set_timeout_ms(5000);
    stub.ReadClusterConfig(&cntl, &req, &resp, nullptr);
    if (cntl.Failed())
    {
        auto &result = resp.result();
        DLOG(ERROR) << "CallRpcReadClusterConfig failed, result:"
                    << static_cast<int>(result.error_code())
                    << ", error: " << cntl.ErrorText()
                    << ", error code: " << cntl.ErrorCode();
        uninitialized = false;
        ret = false;
    }
    else if (resp.result().error_code() != 0)
    {
        auto &result = resp.result();
        DLOG(ERROR) << "CallRpcReadClusterConfig failed, result:"
                    << static_cast<int>(result.error_code());

        if (result.error_code() ==
            ::EloqDS::remote::DataStoreError::REQUESTED_NODE_NOT_OWNER)
        {
            cluster_manager_.HandleShardingError(result);
            needs_retry = true;
        }
        uninitialized = false;
        ret = false;
    }
    else
    {
        // restore values from response
        version = resp.version();
        uninitialized = resp.uninitialized();
        DLOG(INFO) << "CallRpcReadClusterConfig success, version: " << version
                   << ", uninitialized flag: " << uninitialized;
        for (auto &ng_config : resp.cluster_configs())
        {
            uint32_t ng_id = ng_config.node_group_id();
            DLOG(INFO) << "Node group id: " << ng_id;
            std::vector<txservice::NodeConfig> node_configs;
            for (auto &node_config : ng_config.node_configs())
            {
                DLOG(INFO) << "Node id: " << node_config.node_id()
                           << ", host: " << node_config.host_name()
                           << ", port: " << node_config.port()
                           << ", candidate: " << node_config.is_candidate();
                node_configs.push_back(
                    txservice::NodeConfig(node_config.node_id(),
                                          node_config.host_name(),
                                          node_config.port(),
                                          node_config.is_candidate()));
            }
            ng_configs[ng_id] = std::move(node_configs);
        }
        ret = true;
    }

    return ret;
}

bool DataStoreServiceClient::CallRpcUpdateClusterConfig(
    const std::unordered_map<uint32_t, std::vector<txservice::NodeConfig>>
        &new_cnf,
    uint64_t version,
    bool &needs_retry)
{
    uint32_t req_shard_id = cluster_manager_.GetShardIdByKey("");
    auto channel =
        cluster_manager_.GetDataStoreServiceChannelByShardId(req_shard_id);
    if (!channel)
    {
        LOG(ERROR) << "Can not get service channel to primary node";
        return false;
    }

    EloqDS::remote::DataStoreRpcService_Stub stub(channel.get());
    EloqDS::remote::UpdateClusterConfigRequest req;
    EloqDS::remote::UpdateClusterConfigResponse resp;
    brpc::Controller cntl;
    cntl.set_timeout_ms(5000);
    req.set_version(version);
    for (const auto &ng : new_cnf)
    {
        auto *ng_config = req.add_cluster_configs();
        ng_config->set_node_group_id(ng.first);
        for (const auto &node : ng.second)
        {
            auto *node_config = ng_config->add_node_configs();
            node_config->set_node_id(node.node_id_);
            node_config->set_host_name(node.host_name_);
            node_config->set_port(node.port_);
            node_config->set_is_candidate(node.is_candidate_);
        }
    }

    stub.UpdateClusterConfig(&cntl, &req, &resp, nullptr);
    if (cntl.Failed())
    {
        auto &result = resp.result();
        LOG(ERROR) << "CallRpcUpdateClusterConfig failed, result:"
                   << static_cast<int>(result.error_code())
                   << ", error: " << cntl.ErrorText()
                   << ", error code: " << cntl.ErrorCode();
        return false;
    }

    auto &result = resp.result();
    if (result.error_code() != 0)
    {
        LOG(ERROR) << "RemoteUpdateClusterConfig failed, result:"
                   << static_cast<int>(result.error_code())
                   << ", error msg: " << result.error_msg();
        if (result.error_code() ==
            ::EloqDS::remote::DataStoreError::REQUESTED_NODE_NOT_OWNER)
        {
            cluster_manager_.HandleShardingError(result);
            needs_retry = true;
        }
        return false;
    }

    return true;
}

std::shared_ptr<brpc::Channel>
DataStoreServiceClient::GetDataStoreServiceChannelByShardId(uint32_t shard_id)
{
    return cluster_manager_.GetDataStoreServiceChannelByShardId(shard_id);
}

std::shared_ptr<brpc::Channel>
DataStoreServiceClient::GetDataStoreServiceChannel(const DSSNode &node)
{
    return cluster_manager_.GetDataStoreServiceChannel(node);
}

std::shared_ptr<brpc::Channel>
DataStoreServiceClient::UpdateDataStoreServiceChannelByShardId(
    uint32_t shard_id)
{
    return cluster_manager_.UpdateDataStoreServiceChannelByShardId(shard_id);
}

std::shared_ptr<brpc::Channel>
DataStoreServiceClient::UpdateDataStoreServiceChannel(const DSSNode &node)
{
    return cluster_manager_.UpdateDataStoreServiceChannel(node);
}
#endif
}  // namespace EloqDS
