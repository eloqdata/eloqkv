#include "dynamo_scanner.h"

#include <utility>

#include "constants.h"
#include "dynamo_handler.h"
#include "dynamo_handler_typed.h"
#include "partition.h"

namespace EloqDS
{
using Aws::DynamoDB::Model::AttributeValue;
using Aws::DynamoDB::Model::QueryOutcome;
using Aws::DynamoDB::Model::QueryOutcomeCallable;
using Aws::DynamoDB::Model::QueryRequest;

const std::string dynamo_partition_key_attribute_name = "pk1_";
const std::string dynamo_sort_key_attribute_name = "___mono_key___";
// Max number of items returned on a dynamo query request.
const int dynamo_scan_page_limit = 1000;

bool DynamoScanner::IsScanWithPushdownCondition()
{
    return !pushdown_condition_.empty();
}

bool DynamoScanner::IsScanWithStartKey()
{
    return start_key_ != nullptr &&
           start_key_->Type() != txservice::KeyType::PositiveInf &&
           start_key_->Type() != txservice::KeyType::NegativeInf;
}

#ifdef ON_KEY_OBJECT
void DynamoScanner::EncodeDynamoRowObj(
    const Aws::Map<Aws::String, AttributeValue> &row,
    ScanHeapTuple<EloqKV::EloqKey, EloqKV::RedisEloqObject> &heap_tuple,
    const txservice::RecordSchema *rec_sch)
{
    heap_tuple.version_ts_ = std::stoull(row.at("___version___").GetN());
    heap_tuple.deleted_ = row.at("___deleted___").GetBool();
    Aws::Utils::ByteBuffer packed_key =
        row.at(dynamo_sort_key_attribute_name).GetB();
    heap_tuple.key_ = std::make_unique<EloqKV::EloqKey>(
        (const char *) packed_key.GetUnderlyingData(), packed_key.GetLength());
}
#endif

/**
 * @brief Build up scan_req_
 *
 */
void DynamoScanner::BuildScanPartitionRequest()
{
    initialized_ = true;
    // Setup the scan req;
    scan_req_.SetConsistentRead(true);
    const DynamoCatalogInfo *dynamo_info =
        static_cast<const DynamoCatalogInfo *>(kv_info_);
    if (table_name_.Type() == txservice::TableType::Secondary)
    {
        scan_req_.SetTableName(keyspace_ + '.' +
                               dynamo_info->kv_index_names_.at(table_name_));
    }
    else
    {
        scan_req_.SetTableName(keyspace_ + '.' + dynamo_info->kv_table_name_);
    }

    scan_req_.AddExpressionAttributeNames("#pk",
                                          dynamo_partition_key_attribute_name);
    scan_req_.AddExpressionAttributeNames("#mk",
                                          dynamo_sort_key_attribute_name);
    scan_req_.AddExpressionAttributeNames("#up", "___unpack_info___");
    scan_req_.AddExpressionAttributeNames("#vs", "___version___");
    scan_req_.AddExpressionAttributeNames("#dl", "___deleted___");

    std::string proj_expr = "#mk, #up, #vs, #dl";

#ifdef ON_KEY_OBJECT
    scan_req_.AddExpressionAttributeNames("#pl", "___payload___");
    proj_expr += ", #pl";
#endif

    scan_req_.SetProjectionExpression(std::move(proj_expr));

    std::string key_cond = "#pk = :pk";
    if (IsScanWithStartKey())
    {
        key_cond += " AND #mk";
        if (inclusive_)
        {
            if (scan_forward_)
            {
                key_cond.append(" >= :sk");
            }
            else
            {
                key_cond.append(" <= :sk");
            }
        }
        else
        {
            if (scan_forward_)
            {
                key_cond.append(" > :sk");
            }
            else
            {
                key_cond.append(" < :sk");
            }
        }
    }

    scan_req_.SetKeyConditionExpression(std::move(key_cond));

    if (table_name_.Type() != txservice::TableType::Secondary &&
        IsScanWithPushdownCondition())
    {
        SetPushedCond();
    }

    if (!scan_forward_)
    {
        scan_req_.SetScanIndexForward(false);
    }
}

void DynamoScanner::SetPushedCond()
{
    std::string pushed_str;
    for (size_t i = 0; i < pushdown_condition_.size(); ++i)
    {
        auto &cond = pushdown_condition_[i];
        if (pushed_str.size())
        {
            pushed_str.append(" AND ");
        }

        std::string attr = ":c" + std::to_string(i);
        pushed_str.append(cond.field_name_ + " " + cond.op_ + " " + attr);

        AttributeValue att_val;

        switch (cond.data_type_)
        {
        case txservice::store::DataStoreDataType::Numeric:
            att_val.SetN(cond.val_str_);
            break;
        case txservice::store::DataStoreDataType::String:
            att_val.SetS(cond.val_str_);
            break;
        case txservice::store::DataStoreDataType::Blob:
        {
            Aws::Utils::ByteBuffer val_buffer(
                reinterpret_cast<const unsigned char *>(cond.val_str_.data()),
                cond.val_str_.length());
            att_val.SetB(std::move(val_buffer));
            break;
        }
        case txservice::store::DataStoreDataType::Bool:
        {
            if (cond.val_str_ == "true" || cond.val_str_ == "TRUE")
            {
                att_val.SetBool(true);
            }
            else
            {
                att_val.SetBool(false);
            }

            break;
        }
        default:
            // Type is certain for pushdown conditions, should not be here.
            assert(false);
        }

        scan_req_.AddExpressionAttributeValues(std::move(attr),
                                               std::move(att_val));
    }

    if (pushdown_condition_.size() >= 3)
    {
        // If scan has object type filter, add attribute name ___payload___
        scan_req_.AddExpressionAttributeNames("#pl", "___payload___");
    }

    scan_req_.SetFilterExpression(std::move(pushed_str));
}

/**
 * @brief Scan a partition and return a query request future. Will use start
 * key as exclusive start key, otherwise use start_key_ passed in from mysql.
 *
 * @tparam Direction
 * @param partition
 * @param page_limit max number of items returned per scan, or 1MB of data if
 * <= 0
 * @param start_key
 * @return QueryOutcomeCallable
 */
template <bool Direction>
QueryOutcomeCallable HashPartitionDynamoScanner<Direction>::ScanPartition(
    int32_t partition,
    int32_t page_limit,
    Aws::Map<Aws::String, AttributeValue> *start_key)
{
    // AddExpressionAttributeValues will not override existing values, so
    // we cannot reuse the same QueryRequest object across different partitions.
    // Need to make a new copied object with each partition.
    QueryRequest scan_req(scan_req_);
    if (start_key != nullptr)
    {
        scan_req.SetExclusiveStartKey(std::move(*start_key));
    }
    AttributeValue partition_key;
    partition_key.SetN(partition);
    scan_req.AddExpressionAttributeValues(":pk", std::move(partition_key));

    if (IsScanWithStartKey())
    {
        // Aws::Utils::ByteBuffer packed_key(
        //     reinterpret_cast<const unsigned char *>(
        //         start_key_->PackedValueSlice().data()),
        //     start_key_->PackedValueSlice().size());
        AttributeValue sort_key;
        // sort_key.SetB(std::move(packed_key));
        DynamoHandlerTyped::BindDynamoReqForKey(sort_key, *start_key_);
        scan_req.AddExpressionAttributeValues(":sk", std::move(sort_key));
    }

    if (page_limit > 0)
    {
        scan_req.SetLimit(page_limit);
    }
    return dynamo_client_->QueryCallable(scan_req);
}

/**
 * @brief Retrieve result from query request future. Add results to heap_cache_
 * add emplace to scan_res_ number of items scanned from this partition.
 *
 * @tparam Direction
 * @param future
 * @param partition
 * @return true
 * @return false
 */
template <bool Direction>
bool HashPartitionDynamoScanner<Direction>::AddPartitionResult(
    QueryOutcomeCallable &future, int32_t partition)
{
    assert(future.valid());
    const QueryOutcome &result = future.get();
    if (!result.IsSuccess())
    {
        LOG(INFO) << "DynamoDB query request failed, "
                  << result.GetError().GetExceptionName() << ": "
                  << result.GetError().GetMessage();
        return false;
    }

    const Aws::Vector<Aws::Map<Aws::String, AttributeValue>> &rows =
        result.GetResult().GetItems();
    if (rows.size() == 0 &&
        result.GetResult().GetLastEvaluatedKey().size() != 0)
    {
        // The page_limit we set is the number of items scanned before
        // filtering. So if we have passed in pushdown condition to the scan
        // request, we might get less items than page_limit, or even 0. But that
        // does not necessarilly mean the scan has completed. Continue the scan
        // if last evaluated key is not null.
        auto end_key = result.GetResult().GetLastEvaluatedKey();
        QueryOutcomeCallable future =
            ScanPartition(partition, dynamo_scan_page_limit, &end_key);
        return AddPartitionResult(future, partition);
    }
    if (rows.size())
    {
        for (auto &row : rows)
        {
            ScanHeapTuple<EloqKV::EloqKey, EloqKV::RedisEloqObject> heap_tuple(
                partition);
#ifdef ON_KEY_OBJECT
            EncodeDynamoRowObj(row, heap_tuple, rec_sch_);
#else
            heap_tuple.rec_ = DynamoHandlerTyped::NewTxRecord(table_name_);
            EncodeDynamoRow(row, heap_tuple, rec_sch_);
#endif
            heap_cache_.push(std::move(heap_tuple));
        }
        scan_res_.insert_or_assign(
            partition,
            std::make_pair(rows.size(),
                           result.GetResult().GetLastEvaluatedKey()));
    }

    return true;
}

template <bool Direction>
bool HashPartitionDynamoScanner<Direction>::Init()
{
    BuildScanPartitionRequest();
    scan_res_.reserve(1024);

#ifdef USE_ONE_CASS_SHARD
    QueryOutcomeCallable scan_future = ScanPartition(0, dynamo_scan_page_limit);
    if (!AddPartitionResult(scan_future, 0))
    {
        return false;
    }

#else
    std::vector<QueryOutcomeCallable> future_vec;
    future_vec.reserve(1024);

    for (size_t partition = 0; partition < 1024; ++partition)
    {
        future_vec.emplace_back(
            ScanPartition(partition, dynamo_scan_page_limit));
    }

    for (size_t partition = 0; partition < 1024; ++partition)
    {
        if (!AddPartitionResult(future_vec.at(partition), partition))
        {
            return false;
        }
    }
#endif
    initialized_ = true;
    return true;
}

template <bool Direction>
void HashPartitionDynamoScanner<Direction>::Current(
    txservice::TxKey &key,
    const txservice::TxRecord *&rec,
    uint64_t &version_ts,
    bool &deleted)
{
    if (heap_cache_.size() == 0)
    {
        key = TxKey();
        rec = nullptr;
        return;
    }

    const ScanHeapTuple<EloqKV::EloqKey, EloqKV::RedisEloqObject> &top =
        heap_cache_.top();
    key = TxKey(top.key_.get());
    rec = top.rec_.get();
    version_ts = top.version_ts_;
    deleted = top.deleted_;
}

template <bool Direction>
bool HashPartitionDynamoScanner<Direction>::MoveNext()
{
    if (!initialized_)
    {
        if (!Init())
        {
            return false;
        }
        return true;
    }

    if (heap_cache_.size() == 0)
    {
        return true;
    }

    const ScanHeapTuple<EloqKV::EloqKey, EloqKV::RedisEloqObject> &top =
        heap_cache_.top();
    // sid is the partition id used in this query.
    uint32_t partition = top.sid_;
    heap_cache_.pop();

    if (--scan_res_.at(partition).first == 0)
    {
        if (scan_res_.at(partition).second.size())
        {
            QueryOutcomeCallable scan_future =
                ScanPartition(partition,
                              dynamo_scan_page_limit,
                              &scan_res_.at(partition).second);
            if (!AddPartitionResult(scan_future, partition))
            {
                return false;
            }
        }
    }
    return true;
}

template <bool Direction>
void HashPartitionDynamoScanner<Direction>::End()
{
}
template class HashPartitionDynamoScanner<true>;
template class HashPartitionDynamoScanner<false>;

}  // namespace EloqDS
