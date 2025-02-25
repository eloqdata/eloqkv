#pragma once

#include "catalog_factory.h"
#include "local_cc_shards.h"
#include "redis_command.h"
#include "redis_list_object.h"
#include "redis_string_object.h"
#include "store_handler/kv_store.h"
#include "tx_command.h"
#include "tx_key.h"

#if KV_DATA_STORE_TYPE == KV_DATA_STORE_TYPE_CASSANDRA
#include "store_handler/cass_handler.h"
#elif KV_DATA_STORE_TYPE == KV_DATA_STORE_TYPE_DYNAMODB
#include "store_handler/dynamo_handler.h"
#elif KV_DATA_STORE_TYPE == KV_DATA_STORE_TYPE_ROCKSDB
#include "store_handler/rocksdb_handler.h"
#endif

namespace EloqKV
{

class RedisKeySchema : public txservice::KeySchema
{
public:
    explicit RedisKeySchema(uint64_t key_schema_ts) : schema_ts_(key_schema_ts)
    {
    }

    using Uptr = std::unique_ptr<RedisKeySchema>;

    bool CompareKeys(const txservice::TxKey &key1,
                     const txservice::TxKey &key2,
                     size_t *const start_column_diff) const override
    {
        *start_column_diff = -1;
        return true;
    }

    uint16_t ExtendKeyParts() const override
    {
        return 0;  // mock return
    }

    uint64_t SchemaTs() const override
    {
        return schema_ts_;
    }

    // The timestamp when this key was created. For redis, it's the same with
    // table's schema ts.
    uint64_t schema_ts_{1};
};

struct RedisTableSchema : public txservice::TableSchema
{
public:
    RedisTableSchema(const txservice::TableName &redis_table_name,
                     const std::string &catalog_image,
                     uint64_t version);

    const txservice::TableName &GetBaseTableName() const override
    {
        return redis_table_name_;
    }

    const txservice::KeySchema *KeySchema() const override
    {
        return key_schema_.get();
    }

    const txservice::RecordSchema *RecordSchema() const override
    {
        return nullptr;
    }

    const std::string &SchemaImage() const override
    {
        return kv_info_->kv_table_name_;
    }

    const std::unordered_map<
        uint,
        std::pair<txservice::TableName, txservice::SecondaryKeySchema>>
        *GetIndexes() const override
    {
        assert(false);
        return nullptr;
    }

    txservice::KVCatalogInfo *GetKVCatalogInfo() const override
    {
        return kv_info_.get();
        // return nullptr;
    }

    void BindStatistics(
        std::shared_ptr<txservice::Statistics> statistics) override
    {
    }

    void SetKVCatalogInfo(const std::string &kv_info_str) override
    {
    }

    std::shared_ptr<txservice::Statistics> StatisticsObject() const override
    {
        return nullptr;
    }

    uint64_t Version() const override
    {
        return version_;
    }

    std::string_view VersionStringView() const override
    {
        static std::string version_str = std::to_string(version_);
        return {version_str.data(), version_str.length()};
    }

    std::vector<txservice::TableName> IndexNames() const override
    {
        return std::vector<txservice::TableName>();
    }

    size_t IndexesSize() const override
    {
        return 0;
    }

    const txservice::SecondaryKeySchema *IndexKeySchema(
        const txservice::TableName &index_name) const override
    {
        assert(false);
        return nullptr;
    }

    std::unique_ptr<txservice::TxCommand> CreateTxCommand(
        std::string_view cmd_image) const override;

    bool HasAutoIncrement() const override
    {
        return false;
    }
    const txservice::TableName *GetSequenceTableName() const override
    {
        return nullptr;
    }
    std::pair<txservice::TxKey, txservice::TxRecord::Uptr>
    GetSequenceKeyAndInitRecord(
        const txservice::TableName &table_name) const override
    {
        return {txservice::TxKey(), nullptr};
    }
    void AddDirtyIndex(const txservice::TableName &index_name) override
    {
    }
    const std::unordered_set<txservice::TableName> *DirtyIndexNames()
        const override
    {
        return nullptr;
    }
    void CleanDirtyInfo() override
    {
    }

    void SetVersion(uint64_t version)
    {
        version_ = version;
    }

private:
    txservice::TableName redis_table_name_;
    txservice::KVCatalogInfo::uptr kv_info_{nullptr};
    RedisKeySchema::Uptr key_schema_{nullptr};
    uint64_t version_{1};
};

class RedisCatalogFactory : public txservice::CatalogFactory
{
public:
    RedisCatalogFactory() = default;
    ~RedisCatalogFactory() override = default;

    txservice::TableSchema::uptr CreateTableSchema(
        const txservice::TableName &table_name,
        const std::string &catalog_image,
        uint64_t version) override;

    txservice::CcMap::uptr CreatePkCcMap(
        const txservice::TableName &table_name,
        const txservice::TableSchema *table_schema,
        bool ccm_has_full_entries,
        txservice::CcShard *shard,
        txservice::NodeGroupId cc_ng_id) override;

    txservice::CcMap::uptr CreateSkCcMap(
        const txservice::TableName &table_name,
        const txservice::TableSchema *table_schema,
        txservice::CcShard *shard,
        txservice::NodeGroupId cc_ng_id) override;

    txservice::CcMap::uptr CreateRangeMap(
        const txservice::TableName &range_table_name,
        const txservice::TableSchema *table_schema,
        uint64_t schema_ts,
        txservice::CcShard *shard,
        txservice::NodeGroupId ng_id) override;

    std::unique_ptr<txservice::CcScanner> CreatePkCcmScanner(
        txservice::ScanDirection direction,
        const txservice::KeySchema *key_schema) override;

    std::unique_ptr<txservice::CcScanner> CreateSkCcmScanner(
        txservice::ScanDirection direction,
        const txservice::KeySchema *compound_key_schema) override;

    std::unique_ptr<txservice::CcScanner> CreateRangeCcmScanner(
        txservice::ScanDirection direction,
        const txservice::KeySchema *key_schema,
        const txservice::TableName &range_table_name) override;

    std::unique_ptr<txservice::Statistics> CreateTableStatistics(
        const txservice::TableSchema *table_schema,
        txservice::NodeGroupId cc_ng_id) override;

    std::unique_ptr<txservice::Statistics> CreateTableStatistics(
        const txservice::TableSchema *table_schema,
        std::unordered_map<txservice::TableName,
                           std::pair<uint64_t, std::vector<txservice::TxKey>>>
            sample_pool_map,
        txservice::CcShard *ccs,
        txservice::NodeGroupId cc_ng_id) override;

    std::unique_ptr<txservice::TableRangeEntry> CreateTableRange(
        txservice::TxKey start_key,
        uint64_t version_ts,
        int64_t partition_id,
        std::unique_ptr<txservice::StoreRange> slices = nullptr) override
    {
        return nullptr;
    }

    txservice::TxKey NegativeInfKey() override;

    txservice::TxKey PositiveInfKey() override;

    size_t KeyHash(const char *buf,
                   size_t offset,
                   const txservice::KeySchema *key_schema) const override;
};
}  // namespace EloqKV
