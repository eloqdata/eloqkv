#include "namespace/gc.h"

#include <glog/logging.h>
#include "sharder.h"

#include "b255.h"
#include "eloqkv_key.h"
#include "namespace/prefix.h"
#include "namespace/storage.h"
#include "redis_command.h"
#include "redis_service.h"
#include "tx_execution.h"
#include "tx_request.h"
#include "tx_util.h"

namespace EloqKV
{

void NamespaceGc::Start()
{
    if (bthread_start_background(&gc_tid_, nullptr, DaemonRoutine, this) != 0)
    {
        LOG(ERROR) << "Failed to start namespace GC daemon bthread";
    }
}

void NamespaceGc::Stop()
{
    if (gc_tid_ != 0)
    {
        bthread_join(gc_tid_, nullptr);
        gc_tid_ = 0;
    }
}

void *NamespaceGc::DaemonRoutine(void *arg)
{
    NamespaceGc *gc = static_cast<NamespaceGc *>(arg);
    gc->RunDaemon();
    return nullptr;
}

void NamespaceGc::RunDaemon()
{
    LOG(INFO) << "Namespace GC daemon started.";
    while (!server_->IsStopping())
    {
        if (FLAGS_cluster_mode)
        {
            if (txservice::Sharder::Instance().NodeId() != txservice::Sharder::Instance().LeaderNodeId(0))
            {
                for (int i = 0; i < 50; ++i)
                {
                    if (server_->IsStopping())
                    {
                        break;
                    }
                    bthread_usleep(100 * 1000);
                }
                continue;
            }
        }
        std::vector<std::string> gc_records = ScanGCRecords();
        if (server_->IsStopping())
        {
            break;
        }

        if (gc_records.empty())
        {
            for (int i = 0; i < 300; ++i)
            {
                if (server_->IsStopping())
                {
                    break;
                }
                bthread_usleep(100 * 1000);  // 100ms
            }
            continue;
        }

        for (const auto &gc_key : gc_records)
        {
            if (server_->IsStopping())
            {
                break;
            }

            size_t first_colon = gc_key.find(':', 2);
            if (first_colon == std::string::npos)
            {
                DeleteGCRecord(gc_key);
                continue;
            }
            std::string encoded_ns_id = gc_key.substr(2, first_colon - 2);
            std::string epoch_str = gc_key.substr(first_colon + 1);
            uint64_t epoch = 0;
            try
            {
                epoch = std::stoull(epoch_str);
            }
            catch (...)
            {
                DeleteGCRecord(gc_key);
                continue;
            }

            std::string old_prefix =
                NamespacePrefix::MakePrefix(encoded_ns_id, epoch);

            bool cleanup_complete = CleanPrefixKeys(old_prefix);
            if (cleanup_complete)
            {
                DeleteGCRecord(gc_key);
            }

            bthread_usleep(10 * 1000);  // 10ms
        }
    }
    LOG(INFO) << "Namespace GC daemon stopped.";
}

std::vector<std::string> NamespaceGc::ScanGCRecords()
{
    std::vector<std::string> gc_records;
    TransactionExecution *txm =
        server_->NewTxm(IsolationLevel::ReadCommitted, CcProtocol::OccRead);
    if (txm == nullptr)
    {
        return {};
    }

    const TableName &table_name = *(server_->NamespaceTableName());
    CatalogKey catalog_key(table_name);
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
        txservice::AbortTx(txm);
        return {};
    }
    uint64_t schema_version = catalog_rec.SchemaTs();

    EloqKey start_key = EloqKey::Raw(kGcPrefix);
    EloqKey end_key = EloqKey::Raw(kGcScanEnd);

    TxKey start_tx_key(&start_key);
    TxKey end_tx_key(&end_key);

    txservice::BucketScanSavePoint save_point;
    ScanOpenTxRequest scan_open(&table_name,
                                schema_version,
                                ScanIndexType::Primary,
                                &start_tx_key,
                                true,
                                &end_tx_key,
                                false,
                                ScanDirection::Forward,
                                false,
                                false,
                                false,
                                false,
                                true,
                                false,
                                true,
                                false,
                                nullptr,
                                nullptr,
                                txm,
                                -1,
                                "",
                                &save_point);

    txm->Execute(&scan_open);
    scan_open.Wait();
    if (scan_open.IsError() || scan_open.Result() == UINT64_MAX)
    {
        txservice::AbortTx(txm);
        return {};
    }

    uint64_t scan_alias = scan_open.Result();
    size_t plan_size = save_point.PlanSize();

    for (size_t current_index = 0; current_index < plan_size; ++current_index)
    {
        txservice::BucketScanPlan plan = save_point.PickPlan(current_index);
        std::vector<txservice::ScanBatchTuple> scan_batch;
        ScanBatchTxRequest scan_batch_req(scan_alias,
                                          table_name,
                                          &scan_batch,
                                          nullptr,
                                          nullptr,
                                          txm,
                                          -1,
                                          "",
                                          &plan);
        txm->Execute(&scan_batch_req);
        scan_batch_req.Wait();

        if (scan_batch_req.IsError())
        {
            txservice::AbortTx(txm);
            return {};
        }

        for (const auto &tuple : scan_batch)
        {
            if (tuple.status_ == txservice::RecordStatus::Normal)
            {
                gc_records.push_back(tuple.key_.ToString());
            }
        }
    }

    txservice::AbortTx(txm);
    return gc_records;
}

bool NamespaceGc::CleanPrefixKeys(const std::string &old_prefix)
{
    std::string old_prefix_next = NamespacePrefix::MakePrefixNext(old_prefix);
    const TableName &table_name = *(server_->NsDataTableName());

    while (true)
    {
        if (server_->IsStopping())
        {
            return false;
        }

        TransactionExecution *txm =
            server_->NewTxm(IsolationLevel::ReadCommitted, CcProtocol::OccRead);
        if (txm == nullptr)
        {
            bthread_usleep(100 * 1000);
            continue;
        }

        CatalogKey catalog_key(table_name);
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
            txservice::AbortTx(txm);
            bthread_usleep(100 * 1000);
            continue;
        }
        uint64_t schema_version = catalog_rec.SchemaTs();

        EloqKey start_key = EloqKey::Raw(old_prefix);
        EloqKey end_key = EloqKey::Raw(old_prefix_next);

        TxKey start_tx_key(&start_key);
        TxKey end_tx_key(&end_key);

        txservice::BucketScanSavePoint save_point;
        ScanOpenTxRequest scan_open(&table_name,
                                    schema_version,
                                    ScanIndexType::Primary,
                                    &start_tx_key,
                                    true,
                                    &end_tx_key,
                                    false,
                                    ScanDirection::Forward,
                                    false,
                                    false,
                                    false,
                                    false,
                                    true,
                                    false,
                                    true,
                                    false,
                                    nullptr,
                                    nullptr,
                                    txm,
                                    -1,
                                    "",
                                    &save_point);

        txm->Execute(&scan_open);
        scan_open.Wait();
        if (scan_open.IsError() || scan_open.Result() == UINT64_MAX)
        {
            txservice::AbortTx(txm);
            bthread_usleep(100 * 1000);
            continue;
        }

        uint64_t scan_alias = scan_open.Result();
        size_t plan_size = save_point.PlanSize();
        std::vector<std::string> keys_to_delete;
        bool scan_success = true;

        for (size_t current_index = 0; current_index < plan_size;
             ++current_index)
        {
            txservice::BucketScanPlan plan = save_point.PickPlan(current_index);
            std::vector<txservice::ScanBatchTuple> scan_batch;
            ScanBatchTxRequest scan_batch_req(scan_alias,
                                              table_name,
                                              &scan_batch,
                                              nullptr,
                                              nullptr,
                                              txm,
                                              -1,
                                              "",
                                              &plan);
            txm->Execute(&scan_batch_req);
            scan_batch_req.Wait();
            if (scan_batch_req.IsError())
            {
                scan_success = false;
                break;
            }

            for (const auto &tuple : scan_batch)
            {
                if (tuple.status_ == txservice::RecordStatus::Normal)
                {
                    keys_to_delete.push_back(tuple.key_.ToString());
                }
            }
        }

        if (!scan_success)
        {
            txservice::AbortTx(txm);
            return false;
        }

        txservice::AbortTx(txm);

        if (keys_to_delete.empty())
        {
            return true;
        }

        TransactionExecution *txm_del = server_->NewTxm(
            IsolationLevel::RepeatableRead, CcProtocol::Locking);
        if (txm_del == nullptr)
        {
            bthread_usleep(100 * 1000);
            continue;
        }

        std::vector<std::unique_ptr<EloqKey>> del_keys;
        std::vector<std::unique_ptr<DelCommand>> del_cmds;
        std::vector<std::unique_ptr<ObjectCommandTxRequest>> del_reqs;
        del_keys.reserve(keys_to_delete.size());
        del_cmds.reserve(keys_to_delete.size());
        del_reqs.reserve(keys_to_delete.size());

        bool delete_success = true;
        for (const auto &k : keys_to_delete)
        {
            auto key_obj = std::make_unique<EloqKey>(EloqKey::Raw(k));
            auto del_cmd = std::make_unique<DelCommand>();
            auto del_req = std::make_unique<ObjectCommandTxRequest>(
                &table_name,
                key_obj.get(),
                del_cmd.get(),
                /*auto_commit=*/false,
                /*always_redirect=*/true,
                txm_del);

            txm_del->Execute(del_req.get());
            del_req->Wait();
            if (del_req->IsError())
            {
                delete_success = false;
                break;
            }

            del_keys.push_back(std::move(key_obj));
            del_cmds.push_back(std::move(del_cmd));
            del_reqs.push_back(std::move(del_req));
        }

        if (delete_success)
        {
            auto [commit_success, commit_err] = txservice::CommitTx(txm_del);
            if (!commit_success)
            {
                delete_success = false;
            }
        }
        else
        {
            txservice::AbortTx(txm_del);
        }

        bthread_usleep(delete_success ? 5000 : 50000);
    }
}

void NamespaceGc::DeleteGCRecord(const std::string &gc_key)
{
    TransactionExecution *txm =
        server_->NewTxm(IsolationLevel::RepeatableRead, CcProtocol::Locking);
    if (txm == nullptr)
        return;

    EloqKey key_obj = EloqKey::Raw(gc_key);
    DelCommand del_cmd;
    ObjectCommandTxRequest del_req(server_->NamespaceTableName(),
                                   &key_obj,
                                   &del_cmd,
                                   /*auto_commit=*/false,
                                   /*always_redirect=*/true,
                                   txm);

    txm->Execute(&del_req);
    del_req.Wait();
    if (!del_req.IsError())
    {
        txservice::CommitTx(txm);
    }
    else
    {
        txservice::AbortTx(txm);
    }
}

}  // namespace EloqKV
