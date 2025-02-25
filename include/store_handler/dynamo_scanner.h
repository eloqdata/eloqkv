#pragma once

#include <aws/dynamodb/DynamoDBClient.h>
#include <aws/dynamodb/model/QueryRequest.h>
#include <aws/dynamodb/model/QueryResult.h>

#include <algorithm>
#include <memory>
#include <queue>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "eloq_key.h"
#include "kv_store.h"
#include "partition.h"
#include "redis_object.h"
#include "tx_service.h"
#include "tx_service/include/store/data_store_scanner.h"

namespace EloqDS
{
extern const std::string dynamo_partition_key_attribute_name;
extern const std::string dynamo_sort_key_attribute_name;

class DynamoScanner : public txservice::store::DataStoreScanner
{
public:
    DynamoScanner(
        const std::string &keyspace,
        Aws::DynamoDB::DynamoDBClient *client,
        const txservice::KeySchema *key_sch,
        const txservice::RecordSchema *rec_sch,
        const txservice::TableName &table_name,
        const txservice::KVCatalogInfo *kv_info,
        const EloqKV::EloqKey *start_key,
        bool inclusive,
        const std::vector<txservice::store::DataStoreSearchCond> &pushdown_cond,
        bool scan_forward)
        : keyspace_(keyspace),
          dynamo_client_(client),
          key_sch_(key_sch),
          rec_sch_(rec_sch),
          table_name_(table_name.StringView(), table_name.Type()),
          kv_info_(kv_info),
          start_key_(start_key),
          inclusive_(inclusive),
          scan_forward_(scan_forward),
          pushdown_condition_(pushdown_cond)
    {
        assert(table_name_.Type() == txservice::TableType::Primary ||
               table_name_.Type() == txservice::TableType::Secondary ||
               table_name_.Type() == txservice::TableType::UniqueSecondary);
    }

    virtual ~DynamoScanner()
    {
    }

protected:
    void SetPushedCond();
    bool IsScanWithPushdownCondition();
    bool IsScanWithStartKey();
    void BuildScanPartitionRequest();
#ifdef ON_KEY_OBJECT
    void EncodeDynamoRowObj(
        const Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue> &row,
        ScanHeapTuple<EloqKV::EloqKey, EloqKV::RedisEloqObject> &heap_tuple,
        const txservice::RecordSchema *rec_sch);
#endif

protected:
    const std::string keyspace_;
    const Aws::DynamoDB::DynamoDBClient *dynamo_client_;
    // primary key or secondary key schema
    const txservice::KeySchema *key_sch_;
    const txservice::RecordSchema *rec_sch_;
    const txservice::TableName
        table_name_;  // not string owner, sv -> MysqlTableSchema
    const txservice::KVCatalogInfo *kv_info_;
    // Scan key (sort key in DynamoDB)
    const EloqKV::EloqKey *start_key_;
    const bool inclusive_;
    bool scan_forward_;
    const std::vector<txservice::store::DataStoreSearchCond>
        pushdown_condition_;
    Aws::DynamoDB::Model::QueryRequest scan_req_;
    bool initialized_{false};
    bool full_column_scan_{true};
    std::unordered_set<std::string_view> scan_columns_name_;
};
template <bool Direction>
class HashPartitionDynamoScanner : public DynamoScanner
{
public:
    HashPartitionDynamoScanner(
        const std::string &keyspace,
        Aws::DynamoDB::DynamoDBClient *client,
        const txservice::KeySchema *key_sch,
        const txservice::RecordSchema *rec_sch,
        const txservice::TableName &table_name,
        const txservice::KVCatalogInfo *kv_info,
        const EloqKV::EloqKey &start_key,
        bool inclusive,
        const std::vector<txservice::store::DataStoreSearchCond> &pushdown_cond)
        : DynamoScanner(keyspace,
                        client,
                        key_sch,
                        rec_sch,
                        table_name,
                        kv_info,
                        &start_key,
                        inclusive,
                        pushdown_cond,
                        Direction)
    {
    }

    ~HashPartitionDynamoScanner()
    {
    }

    void Current(txservice::TxKey &key,
                 const txservice::TxRecord *&rec,
                 uint64_t &version_ts,
                 bool &deleted) override;
    bool MoveNext() override;
    void End() override;

private:
    bool Init();
    Aws::DynamoDB::Model::QueryOutcomeCallable ScanPartition(
        int32_t partition,
        int32_t page_limit,
        Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue> *start_key =
            nullptr);
    bool AddPartitionResult(Aws::DynamoDB::Model::QueryOutcomeCallable &future,
                            int32_t partition);

private:
    using CompareFunc = std::conditional_t<
        Direction,
        CacheCompare<EloqKV::EloqKey, EloqKV::RedisEloqObject>,
        CacheReverseCompare<EloqKV::EloqKey, EloqKV::RedisEloqObject>>;
    std::priority_queue<
        ScanHeapTuple<EloqKV::EloqKey, EloqKV::RedisEloqObject>,
        std::vector<ScanHeapTuple<EloqKV::EloqKey, EloqKV::RedisEloqObject>>,
        CompareFunc>
        heap_cache_;

    // map from partition id to remaining tuples from this partition in
    // heap_cache_ and last_key from previous query request.
    std::unordered_map<
        int32_t,
        std::pair<int32_t,
                  Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue>>>
        scan_res_;
};

}  // namespace EloqDS
