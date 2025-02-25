#include "store_handler/data_store_service_scanner.h"

#include <glog/logging.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace EloqDS
{
#if ROCKSDB_CLOUD_FS()
DataStoreServiceScanner::DataStoreServiceScanner(
    DataStoreServiceClient *client,
    const txservice::KeySchema *key_sch,
    const txservice::RecordSchema *rec_sch,
    const txservice::TableName &table_name,
    const txservice::KVCatalogInfo *kv_info,
    const txservice::TxKey &start_key,
    bool inclusive,
    const std::vector<txservice::store::DataStoreSearchCond> &pushdown_cond,
    bool scan_forward)
    : client_(client),
      key_sch_(key_sch),
      rec_sch_(rec_sch),
      table_name_(table_name),
      kv_info_(kv_info),
      inclusive_(inclusive),
      scan_forward_(scan_forward),
      pushdown_condition_(pushdown_cond),
      batch_size_(100),
      initialized_(false),
      session_id_("")
{
    assert(client_ != nullptr);
    const EloqKV::EloqKey *start_key_ptr = start_key.GetKey<EloqKV::EloqKey>();
    if (start_key_ptr == EloqKV::EloqKey::NegativeInfinity())
    {
        last_key_is_neg_inf_ = true;
    }
    else if (start_key_ptr == EloqKV::EloqKey::PositiveInfinity())
    {
        last_key_is_pos_inf_ = true;
    }
    else
    {
        last_key_ = std::make_unique<std::string>(start_key_ptr->KVSerialize());
    }
}

DataStoreServiceScanner::~DataStoreServiceScanner()
{
    if (initialized_)
    {
        CloseScan();
    }
}

bool DataStoreServiceScanner::Init()
{
    if (initialized_)
    {
        return true;
    }

    initialized_ = true;

    if (!client_)
    {
        LOG(ERROR) << "Invalid scanner parameters: client is null";
        return false;
    }

    request_.clear_session_id();
    request_.set_inclusive(inclusive_);
    request_.set_kv_table_name_str(kv_info_->kv_table_name_);
    request_.set_batch_size(batch_size_);
    request_.set_scan_forward(scan_forward_);
    for (const auto &cond : pushdown_condition_)
    {
        auto *pb_cond = request_.add_search_conditions();
        pb_cond->set_field_name(cond.field_name_);
        pb_cond->set_op(cond.op_);
        pb_cond->set_value(cond.val_str_);
    }

    if (!FetchNextBatch())
    {
        LOG(ERROR) << "Failed to fetch initial batch";
        return false;
    }

    return true;
}

bool DataStoreServiceScanner::MoveNext()
{
    if (!initialized_ && !Init())
    {
        return false;
    }

    if (!result_cache_.empty())
    {
        const auto &item = result_cache_.front();
        // fill the current key and record strings
        current_item_ = std::move(item);
        result_cache_.pop_front();
        return true;
    }

    return FetchNextBatch();
}

void DataStoreServiceScanner::Current(txservice::TxKey &key,
                                      const txservice::TxRecord *&rec,
                                      uint64_t &version_ts,
                                      bool &deleted)
{
    assert(initialized_);

    while (true)
    {
        if (IsRunOutOfData())
        {
            key = txservice::TxKey();
            rec = nullptr;
            version_ts = 0;
            deleted = false;
            return;
        }

        const std::string &key_str = current_item_.key();
        const std::string &val_str = current_item_.value();
        bool is_deleted = false;
        int64_t version = 0;
        current_key_ = std::make_unique<EloqKV::EloqKey>(key_str);
        EloqShare::DeserializeToTxRecord(
            val_str.c_str(), val_str.size(), current_rec_, is_deleted, version);
        assert(is_deleted == false);

        bool fail = false;
        for (auto &cond : pushdown_condition_)
        {
            if (cond.field_name_ == "type")
            {
                EloqKV::RedisObjectType type_cond =
                    static_cast<EloqKV::RedisObjectType>(cond.val_str_[0]);
                EloqKV::RedisObjectType type_rec =
                    static_cast<EloqKV::RedisEloqObject *>(current_rec_.get())
                        ->ObjectType();
                if (type_cond != type_rec)
                {
                    fail = true;
                    break;
                }
            }
        }

        if (fail)
        {
            MoveNext();
            continue;
        }

        key = txservice::TxKey(current_key_.get());
        rec = current_rec_.get();
        version_ts = version;
        deleted = is_deleted;
        break;
    }
}

void DataStoreServiceScanner::End()
{
    initialized_ = false;
    result_cache_.clear();
}

bool DataStoreServiceScanner::CloseScan()
{
    if (!client_)
    {
        LOG(ERROR) << "Invalid scanner parameters: client is null";
        return false;
    }

    bthread::Mutex mutex;
    bthread::ConditionVariable cv;
    bool done = false;
    bool ret = false;

    if (!initialized_)
    {
        ret = true;
        done = true;
    }
    else if (IsRunOutOfData())
    {
        // no need to close scan, since the scan is already closed on the server
        ret = true;
        done = true;
    }
    else if (!session_id_.empty())
    {
        request_.clear_session_id();
        request_.set_session_id(session_id_);

        client_->ScanCloseWithRetry(
            request_,
            [&ret, &done, &cv, &mutex](
                const EloqDS::remote::ScanResponse &response)
            {
                auto &result = response.result();
                if (result.error_code() !=
                    EloqDS::remote::DataStoreError::NO_ERROR)
                {
                    LOG(ERROR)
                        << "Failed to close scan: " << result.error_msg();
                    ret = false;
                }
                else
                {
                    ret = true;
                }

                std::unique_lock<bthread::Mutex> lk(mutex);
                done = true;
                cv.notify_one();
            },
            0);

        std::unique_lock<bthread::Mutex> lk(mutex);
        while (!done)
        {
            cv.wait(lk);
        }
    }

    return ret;
}

bool DataStoreServiceScanner::IsRunOutOfData()
{
    return initialized_ && last_key_ == nullptr && !last_key_is_neg_inf_ &&
           !last_key_is_pos_inf_ && result_cache_.empty();
}

bool DataStoreServiceScanner::FetchNextBatch()
{
    // data is drain
    if (IsRunOutOfData())
    {
        return false;
    }

    if (!client_)
    {
        LOG(ERROR) << "Invalid scanner parameters: client is null";
        return false;
    }

    // clear the result cache
    result_cache_.clear();

    if (initialized_)
    {
        request_.clear_inclusive();
        request_.set_inclusive(false);
    }

    request_.clear_start_key();
    request_.clear_start_key_is_neg_inf();
    request_.clear_start_key_is_pos_inf();

    if (last_key_is_neg_inf_)
    {
        request_.set_start_key_is_neg_inf(true);
    }
    else if (last_key_is_pos_inf_)
    {
        request_.set_start_key_is_pos_inf(true);
    }
    else
    {
        request_.set_start_key(*last_key_);
    }

    request_.clear_session_id();
    if (!session_id_.empty())
    {
        request_.set_session_id(session_id_);
    }

    bthread::Mutex mutex;
    bthread::ConditionVariable cv;
    bool done, ret = true;

    done = false;
    ret = true;

    if (session_id_.empty())
    {
        // scan open
        client_->ScanOpenWithRetry(
            request_,
            [this, &ret, &done, &cv, &mutex](
                const EloqDS::remote::ScanResponse &response)
            {
                auto common_result = response.result();
                if (common_result.error_code() !=
                    EloqDS::remote::DataStoreError::NO_ERROR)
                {
                    LOG(ERROR)
                        << "Failed to open scan: " << common_result.error_msg();

                    std::unique_lock<bthread::Mutex> lk(mutex);
                    ret = false;
                    done = true;
                    cv.notify_one();
                    return;
                }

                for (const auto &item : response.items())
                {
                    result_cache_.push_back(std::move(item));
                }

                // set the session id
                session_id_ = response.session_id();
                // session id should not be empty when data is not drained
                assert(!session_id_.empty() ||
                       result_cache_.size() < batch_size_);

                // if the result is less than batch size, it means
                // the scan is done
                if (result_cache_.size() < batch_size_)
                {
                    last_key_ = nullptr;
                    // no need to close scan, since the scan is already closed
                    // on the server
                }
                else
                {
                    // set the last key when the result is full
                    last_key_ = std::make_unique<std::string>(
                        result_cache_.back().key());
                }
                last_key_is_neg_inf_ = false;
                last_key_is_pos_inf_ = false;

                std::unique_lock<bthread::Mutex> lk(mutex);
                done = true;
                cv.notify_one();
            },
            0);
    }
    else
    {
        // scan next
        client_->ScanNextWithRetry(
            request_,
            [this, &ret, &done, &cv, &mutex](
                const EloqDS::remote::ScanResponse &response)
            {
                auto common_result = response.result();
                if (common_result.error_code() !=
                    EloqDS::remote::DataStoreError::NO_ERROR)
                {
                    LOG(ERROR) << "Failed to fetch next batch: "
                               << common_result.error_msg();

                    std::unique_lock<bthread::Mutex> lk(mutex);
                    ret = false;
                    done = true;
                    cv.notify_one();
                    return;
                }

                for (const auto &item : response.items())
                {
                    result_cache_.push_back(std::move(item));
                }

                session_id_ = response.session_id();
                // session id should not be empty when data is not drained
                assert(!session_id_.empty() ||
                       result_cache_.size() < batch_size_);

                // if the result is less than batch size, it means
                // the scan is done
                if (result_cache_.size() < batch_size_)
                {
                    last_key_ = nullptr;
                    // no need to close scan, since the scan is already closed
                    // on the server
                }
                else
                {
                    // set the last key when the result is full
                    last_key_ = std::make_unique<std::string>(
                        result_cache_.back().key());
                }
                last_key_is_neg_inf_ = false;
                last_key_is_pos_inf_ = false;

                std::unique_lock<bthread::Mutex> lk(mutex);
                done = true;
                cv.notify_one();
            },
            0);
    }

    std::unique_lock<bthread::Mutex> lk(mutex);
    while (!done)
    {
        cv.wait(lk);
    }

    // either the above scan is done or failed
    if (result_cache_.empty())
    {
        if (!ret)
        {
            LOG(ERROR) << "Failed to fetch next batch";
            last_key_ = nullptr;
            CloseScan();
        }

        return ret;
    }
    else
    {
        return MoveNext();
    }
}
#endif
}  // namespace EloqDS
