#pragma once

#include "kv_store.h"

#if ROCKSDB_CLOUD_FS()
#include "rocksdb/cloud/db_cloud.h"
#else
#include "rocksdb/db.h"
#endif

#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "eloq_key.h"
#include "redis_object.h"
#include "rocksdb_config.h"
#include "tx_service/include/store/data_store_handler.h"

namespace EloqKV
{

class RocksDBScanner : public txservice::store::DataStoreScanner
{
public:
    RocksDBScanner(
        rocksdb::DB *db,
        rocksdb::ColumnFamilyHandle *cfh,
        const txservice::KeySchema *key_sch,
        const txservice::RecordSchema *rec_sch,
        const txservice::TableName &table_name,
        const txservice::KVCatalogInfo *kv_info,
        const EloqKV::EloqKey *start_key,
        bool inclusive,
        const std::vector<txservice::store::DataStoreSearchCond> &pushdown_cond,
        bool scan_forward)
        : db_(db),
          cfh_(cfh),
          key_sch_(key_sch),
          rec_sch_(rec_sch),
          table_name_(table_name),
          kv_info_(kv_info),
          start_key_(*start_key),
          inclusive_(inclusive),
          scan_forward_(scan_forward),
          pushdown_condition_(pushdown_cond),
          initialized_(false),
          iter_(nullptr),
          current_key_(nullptr),
          current_rec_(nullptr)
    {
        assert(table_name_.Type() == txservice::TableType::Primary ||
               table_name_.Type() == txservice::TableType::Secondary ||
               table_name_.Type() == txservice::TableType::UniqueSecondary);
    };

    void Current(txservice::TxKey &key,
                 const txservice::TxRecord *&rec,
                 uint64_t &version_ts,
                 bool &deleted_) override;

    bool MoveNext() override;

    void End() override;

    bool Init();

private:
    rocksdb::DB *db_;
    rocksdb::ColumnFamilyHandle *cfh_{nullptr};
    const std::string_view keyspace_name_v_;
    // primary key or secondary key schema
    const txservice::KeySchema *key_sch_;
    const txservice::RecordSchema *rec_sch_;
    const txservice::TableName
        table_name_;  // not string owner, sv -> TableSchema
    const txservice::KVCatalogInfo *kv_info_;
    const EloqKV::EloqKey start_key_;  // pk or (sk,pk)
    const bool inclusive_;
    bool scan_forward_;
    const std::vector<txservice::store::DataStoreSearchCond>
        pushdown_condition_;
    bool initialized_{false};

    std::unique_ptr<rocksdb::Iterator> iter_{nullptr};
    std::unique_ptr<EloqKV::EloqKey> current_key_{nullptr};
    txservice::TxRecord::Uptr current_rec_{nullptr};
};
}  // namespace EloqKV
