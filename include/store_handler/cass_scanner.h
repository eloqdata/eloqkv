#pragma once

#include <algorithm>
#include <memory>
#include <queue>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "cass/include/cassandra.h"
#include "cass_handler_typed.h"
#include "eloq_key.h"
#include "kv_store.h"
#include "partition.h"
#include "redis_object.h"
#include "tx_service/include/store/data_store_scanner.h"
#include "tx_service/include/tx_service.h"

namespace EloqDS
{

class CassScanner : public txservice::store::DataStoreScanner
{
public:
    CassScanner(
        CassSession *cass_session,
        const std::string &keyspace_name,
        const txservice::KeySchema *key_sch,
        const txservice::RecordSchema *rec_sch,
        const txservice::TableName &table_name,
        const txservice::KVCatalogInfo *kv_info,
        const EloqKV::EloqKey *start_key,
        bool inclusive,
        const std::vector<txservice::store::DataStoreSearchCond> &pushdown_cond,
        bool scan_forward)
        : cass_session_(cass_session),
          keyspace_name_v_(keyspace_name),
          key_sch_(key_sch),
          rec_sch_(rec_sch),
          table_name_(table_name.StringView(), table_name.Type()),
          kv_info_(kv_info),
          start_key_(start_key),
          inclusive_(inclusive),
          scan_forward_(scan_forward),
          pushdown_condition_(pushdown_cond),
          scan_prepared_(nullptr),
          initialized_(false)
    {
        assert(table_name_.Type() == txservice::TableType::Primary ||
               table_name_.Type() == txservice::TableType::Secondary ||
               table_name_.Type() == txservice::TableType::UniqueSecondary);
    }

    std::string_view ErrorMessage(CassFuture *future);

    virtual ~CassScanner();

protected:
    CassError BuildScanPartitionPrepared();
    std::string BuildPushedCondStr();
    std::pair<CassStatement *, CassFuture *> BuildScanPartitionStatement(
        int32_t pk1, int16_t pk2, size_t page_size);

    template <typename KeyT>
    void EncodeCassRow(const CassRow *row,
                       const txservice::RecordSchema *rec_sch,
                       KeyT *key,
                       txservice::TxRecord *rec,
                       uint64_t &version_ts_,
                       bool &deleted_)
    {
        cass_value_get_int64(cass_row_get_column_by_name(row, "___version___"),
                             reinterpret_cast<int64_t *>(&version_ts_));

        cass_bool_t is_deleted = cass_bool_t::cass_false;
        cass_value_get_bool(cass_row_get_column_by_name(row, "___deleted___"),
                            static_cast<cass_bool_t *>(&is_deleted));
        deleted_ = (is_deleted == cass_bool_t::cass_true);
        size_t packed_len = 0, unpack_len = 0;
        const cass_byte_t *packed_key = NULL, *unpack_info = NULL;
        if (table_name_.Type() == txservice::TableType::Primary)
        {
            cass_value_get_bytes(
                cass_row_get_column_by_name(row, "___mono_key___"),
                &packed_key,
                &packed_len);
        }
        else
        {
            cass_value_get_bytes(
                cass_row_get_column_by_name(row, "___mono_key___"),
                &packed_key,
                &packed_len);
        }
        // key->PackedValue().clear();
        // key->PackedValue().resize(packed_len);
        // memcpy(key->PackedValue().data(), packed_key, packed_len);
        CassHandlerTyped::ParseKeyFromBlob(key, packed_key, packed_len);

        if (!deleted_)
        {
            cass_value_get_bytes(
                cass_row_get_column_by_name(row, "___unpack_info___"),
                &unpack_info,
                &unpack_len);
            // rec->SetUnpackInfo(unpack_info, unpack_len);
            CassHandlerTyped::ParseUnpackInfoFromBlob(
                rec, unpack_info, unpack_len);
        }
    }

    bool IsScanWithPushdownCondition();
    bool IsScanWithStartKey();
#ifdef ON_KEY_OBJECT
    // Only used for object
    template <typename KeyT>
    void EncodeCassRowObj(const CassRow *row,
                          const txservice::RecordSchema *rec_sch,
                          KeyT *key,
                          uint64_t &version_ts_,
                          bool &deleted_)
    {
        cass_value_get_int64(cass_row_get_column_by_name(row, "___version___"),
                             reinterpret_cast<int64_t *>(&version_ts_));

        cass_bool_t is_deleted = cass_bool_t::cass_false;
        cass_value_get_bool(cass_row_get_column_by_name(row, "___deleted___"),
                            &is_deleted);
        deleted_ = (is_deleted == cass_bool_t::cass_true);
        assert(table_name_.Type() == txservice::TableType::Primary);

        size_t packed_len = 0;
        const cass_byte_t *packed_key = NULL;

        cass_value_get_bytes(cass_row_get_column_by_name(row, "___mono_key___"),
                             &packed_key,
                             &packed_len);

        key->KVDeserialize(reinterpret_cast<const char *>(packed_key),
                           packed_len);
    }
#endif
protected:
    CassSession *cass_session_;
    const std::string_view keyspace_name_v_;
    // primary key or secondary key schema
    const txservice::KeySchema *key_sch_;
    const txservice::RecordSchema *rec_sch_;
    const txservice::TableName
        table_name_;  // not string owner, sv -> MysqlTableSchema
    const txservice::KVCatalogInfo *kv_info_;
    const EloqKV::EloqKey *start_key_;  // pk or (sk,pk)
    const bool inclusive_;
    bool scan_forward_;
    const std::vector<txservice::store::DataStoreSearchCond>
        pushdown_condition_;
    const CassPrepared *scan_prepared_{nullptr};
    bool initialized_{false};
};

template <bool Direction>
class HashPartitionCassScanner : public CassScanner
{
public:
    HashPartitionCassScanner(
        CassSession *cass_session,
        const std::string &keyspace_name,
        const txservice::KeySchema *key_sch,
        const txservice::RecordSchema *rec_sch,
        const txservice::TableName &table_name,
        const txservice::KVCatalogInfo *kv_info,
        const EloqKV::EloqKey &start_key,
        bool inclusive,
        const std::vector<txservice::store::DataStoreSearchCond> &pushdown_cond)
        : CassScanner(cass_session,
                      keyspace_name,
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

    ~HashPartitionCassScanner()
    {
        for (size_t sid = 0; sid < shard_scan_res_.size(); ++sid)
        {
            if (shard_scan_res_.at(sid) != nullptr)
            {
                cass_result_free(shard_scan_res_.at(sid));
            }

            if (shard_scan_it_.at(sid) != nullptr)
            {
                cass_iterator_free(shard_scan_it_.at(sid));
            }

            cass_statement_free(shard_scan_st_.at(sid));
        }
    }

    bool AddShardScan(CassStatement *scan_st, CassFuture *scan_future);
    void Current(txservice::TxKey &key,
                 const txservice::TxRecord *&rec,
                 uint64_t &version_ts,
                 bool &deleted_) override;
    bool MoveNext() override;
    void End() override;

private:
    bool Init();

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
    std::vector<const CassResult *> shard_scan_res_;
    std::vector<CassIterator *> shard_scan_it_;
    std::vector<CassStatement *> shard_scan_st_;
};

#ifdef RANGE_PARTITION_ENABLED
class RangePartitionCassScanner : public CassScanner
{
public:
    RangePartitionCassScanner(
        CassSession *cass_session,
        const std::string &keyspace_name,
        const txservice::KeySchema *key_sch,
        const txservice::RecordSchema *rec_sch,
        const txservice::TableName &table_name,
        uint32_t ng_id,
        const txservice::KVCatalogInfo *kv_info,
        const txservice::TxKey &start_key,
        bool inclusive,
        const std::vector<txservice::store::DataStoreSearchCond> &pushdown_cond,
        bool scan_forward,
        txservice::TxService *tx_service)
        : CassScanner(cass_session,
                      keyspace_name,
                      key_sch,
                      rec_sch,
                      table_name,
                      kv_info,
                      nullptr,
                      inclusive,
                      pushdown_cond,
                      scan_forward),
          tx_service_(tx_service),
          ng_id_(ng_id),
          partition_iterator_(nullptr),
          scan_st_(nullptr),
          scan_res_(std::unique_ptr<
                    const CassResult,
                    decltype(&RangePartitionCassScanner::CassResultFree)>(
              nullptr, &cass_result_free)),
          scan_it_(nullptr),
          scan_finished_(false),
          start_key_holder_(nullptr),
          current_key_(nullptr),
          current_rec_(nullptr)
    {
        // Copy the start_key as class local variable, since the start_key could
        // be a stack variable
        if (start_key.Type() == KeyType::NegativeInf ||
            start_key.Type() == KeyType::PositiveInf)
        {
            start_key_ = &start_key;
        }
        else
        {
            start_key_holder_ = start_key.Clone();
            start_key_ = start_key_holder_.get();
        }
        current_key_ = CassHandlerTyped::NewTxKey(table_name);
        current_rec_ = CassHandlerTyped::NewTxRecord(table_name);
    }

    ~RangePartitionCassScanner()
    {
        if (scan_it_ != nullptr)
        {
            cass_iterator_free(scan_it_);
        }
        if (scan_st_ != nullptr)
        {
            cass_statement_free(scan_st_);
        }
        if (!scan_finished_)
        {
            // TODO(Xiao Ji): remove the nullptr check. This is a unnecessary
            // check, since partition iterator must be there if range partition
            // is enabled, but some other bugs may cause the cass_scanner is not
            // been initialized correctly
            if (partition_iterator_ != nullptr)
            {
                partition_iterator_->ReleaseReadLocks();
            }
        }
    }

    void Current(const txservice::TxKey *&key,
                 const txservice::TxRecord *&rec,
                 uint64_t &version_ts,
                 bool &deleted) override;
    bool MoveNext() override;
    void End() override;

private:
    bool ScanNextPartition();
    bool CassIteratorNext();
    bool Init();
    // For purpose of working around the compiling error on default deleter
    static void CassResultFree(const CassResult *result)
    {
        // This won't be called unless the scan_res_.get() is not nullptr when
        // destructor
        cass_result_free(result);
    };

private:
    txservice::TxService *tx_service_{nullptr};
    uint32_t ng_id_;
    std::unique_ptr<PartitionIterator> partition_iterator_;
    CassStatement *scan_st_{nullptr};
    std::unique_ptr<const CassResult,
                    decltype(&RangePartitionCassScanner::CassResultFree)>
        scan_res_;
    CassIterator *scan_it_{nullptr};
    bool scan_finished_{false};
    txservice::TxKey::Uptr start_key_holder_;
    txservice::TxKey::Uptr current_key_;
    txservice::TxRecord::Uptr current_rec_;
    uint64_t current_version_ts_;
    bool current_deleted_;
};
#endif
}  // namespace EloqDS
