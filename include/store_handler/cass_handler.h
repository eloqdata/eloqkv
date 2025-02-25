#pragma once

#include <algorithm>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "bthread/timer_thread.h"
#include "cass/include/cassandra.h"
#include "cass_handler_typed.h"
#include "eloq_key.h"
#include "meter.h"
#include "range_slice.h"
#include "tx_key.h"
#include "tx_service/include/catalog_key_record.h"
#include "tx_service/include/constants.h"
#include "tx_service/include/range_record.h"
#include "tx_service/include/store/data_store_handler.h"
#include "tx_service/include/store/data_store_scanner.h"
#include "tx_service/include/tx_worker_pool.h"
#include "tx_service/include/util.h"

namespace EloqDS
{
// cass meter
extern std::unique_ptr<metrics::Meter> kv_meter;

class CassBatchExecutor
{
    static constexpr uint32_t MaxTuplesBytesSize = 4 * 1024;

public:
    CassBatchExecutor(CassSession *session);
    ~CassBatchExecutor();
    CassError AddBatchStatement(CassStatement *stmt, uint32_t tuple_byte_size);
    bool HasStatements();
    void Execute();
    CassError Wait();
    CassError Retry();
    uint16_t PendingFutureCount()
    {
        return futures_.size();
    }
    bool IsFull()
    {
        return batch_tuple_size_ >= MaxTuplesBytesSize;
    }

    enum FlushTableType
    {
        Base,
        Archive,
        None
    };
    FlushTableType flush_table_type_{FlushTableType::None};

private:
    /**
     * This enum is for indexing tuples more readably.
     */
    enum TuplePos
    {
        FUTURE = 0,
        BATCH,
        BATCH_SIZE
    };

    CassBatch *batch_{nullptr};
    CassSession *session_{nullptr};
    std::vector<std::tuple<CassFuture *, CassBatch *, uint16_t>> futures_;
    int32_t current_batch_size_{0};
    uint32_t batch_tuple_size_{0};
};

template <typename KeyT>
class BatchReadExecutor
{
public:
    BatchReadExecutor(CassSession *session,
                      uint32_t max_futures_size,
                      const txservice::TableName &table_name,
                      const txservice::TableSchema *table_schema,
                      std::vector<txservice::FlushRecord> &results)
        : session_(session),
          max_futures_size_(max_futures_size),
          table_name_(table_name),
          table_schema_(table_schema),
          results_(results)
    {
    }

    ~BatchReadExecutor()
    {
        session_ = nullptr;

        for (auto fut_it = futures_.begin(); fut_it != futures_.end(); fut_it++)
        {
            if (std::get<0>(*fut_it))
            {
                cass_future_free(std::get<0>(*fut_it));
            }
            if (std::get<1>(*fut_it))
            {
                cass_statement_free(std::get<1>(*fut_it));
            }
        }
    }
    // add statement and execute
    bool AddStatement(CassStatement *stmt)
    {
        assert(futures_.size() < max_futures_size_);
        futures_.emplace_back(
            cass_session_execute(session_, stmt), stmt, nullptr);
        return true;
    }

    bool AddStatement(CassStatement *stmt, const KeyT *key)
    {
        assert(futures_.size() < max_futures_size_);
        futures_.emplace_back(cass_session_execute(session_, stmt), stmt, key);
        return true;
    }

    CassError Wait()
    {
        for (auto fut_it = futures_.begin(); fut_it != futures_.end();)
        {
            CassFuture *&future = std::get<0>(*fut_it);
            if (future == nullptr)  // skip invalid future, it should be retried
            {
                fut_it++;
                continue;
            }
            CassError rc = cass_future_error_code(future);
            if (rc == CASS_OK)  // remove successful futures
            {
                /* This will also block until the query returns */
                const CassResult *result = cass_future_get_result(future);
                /* The future can be freed immediately after getting the result
                 * object
                 */
                cass_future_free(future);
                ParseReadResult(result, std::get<2>(*fut_it));
                cass_statement_free(std::get<1>(*fut_it));
                fut_it = futures_.erase(fut_it);
                cass_result_free(result);
            }
            else
            {
                // Print error sql for notice
                const char *error_message;
                size_t error_message_length;
                cass_future_error_message(
                    future, &error_message, &error_message_length);
                LOG(ERROR) << "ParallelCassStmtExecute Error: "
                           << error_message;
                // delete future pointer
                cass_future_free(future);
                future = nullptr;

                ++fut_it;
            }
        }

        if (futures_.size() == 0)
        {
            return CASS_OK;
        }
        else
        {
            return CASS_ERROR_LAST_ENTRY;
        }
    }
    CassError Retry()
    {
        for (auto fut_it = futures_.begin(); fut_it != futures_.end(); fut_it++)
        {
            if (std::get<0>(*fut_it) == nullptr)
            {
                std::get<0>(*fut_it) =
                    cass_session_execute(session_, std::get<1>(*fut_it));
            }
        }

        return Wait();
    }
    uint16_t PendingFutureCount()
    {
        return futures_.size();
    }

    void ParseReadResult(const CassResult *result, const EloqKV::EloqKey *key)
    {
        const CassRow *row = cass_result_first_row(result);

        if (row != NULL)
        {
            auto &ref = results_.emplace_back();
            ref.SetKey(txservice::TxKey(key));

            uint16_t record_col_cnt = 0;
            const txservice::RecordSchema *rec_sch =
                table_schema_->RecordSchema();
            record_col_cnt =
                CassHandlerTyped::PayloadColumnCount(table_name_, rec_sch);

            int64_t *ts = reinterpret_cast<int64_t *>(&ref.commit_ts_);
            cass_value_get_int64(cass_row_get_column(row, record_col_cnt + 2),
                                 ts);

            cass_bool_t deleted = cass_false;
            cass_value_get_bool(cass_row_get_column(row, record_col_cnt + 3),
                                &deleted);

            if (deleted == cass_false)
            {
                txservice::TxRecord::Uptr record =
                    CassHandlerTyped::NewTxRecord(table_name_);
                CassHandlerTyped::ParsePayloadFromCassRow(
                    row, 0, record.get(), rec_sch, table_name_);

#ifndef ON_KEY_OBJECT
                ref.SetPayload(std::move(record));
#else
                assert(false);
                ref.SetPayload(record.get());
#endif
                ref.payload_status_ = txservice::RecordStatus::Normal;
            }
            else
            {
                ref.payload_status_ = txservice::RecordStatus::Deleted;
            }
        }
    }

private:
    CassSession *session_{nullptr};
    std::vector<std::tuple<CassFuture *, CassStatement *, const KeyT *>>
        futures_;
    const uint32_t max_futures_size_;
    const txservice::TableName &table_name_;
    const txservice::TableSchema *table_schema_;
    std::vector<txservice::FlushRecord> &results_;
};

enum struct CassPreparedType
{
    Insert = 0,
    InsertTTL,
    Delete,
    Read,
    ScanSlice,
    ScanLastSlice,
    SnapshotScanSlice,
    SnapshotScanLastSlice,
    UpdateSlice,
    UpdateSliceSizes,
    EnumSize,  // For using enum class as array index.
};

class CassHandler : public txservice::store::DataStoreHandler
{
public:
    CassHandler(const std::string &endpoints,
                const int port,
                const std::string &username,
                const std::string &password,
                const std::string &keyspace_name,
                const std::string &keyspace_class,
                const std::string &replicate_factor,
                const int queue_size_io,
                bool bootstrap,
                bool ddl_skip_kv,
                uint32_t write_batch = 25,
                uint32_t max_futures = 32,
                uint32_t worker_pool_size = 1);

    ~CassHandler();

    /**
     * Connect to Cassandra service and use target keyspace.
     */
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
     * @brief flush entries in \@param batch to base table or skindex table in
     * data store, stop and return false if node_group is not longer leader.
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

    static void OnFetchCurrentTableStatistics(CassFuture *future,
                                              void *fetch_req);

    void FetchTableStatistics(
        const txservice::TableName &ccm_table_name,
        txservice::FetchTableStatisticsCc *fetch_cc) override;

    static void OnFetchTableStatistics(CassFuture *future, void *fetch_req);

    bool UpsertTableStatistics(
        const txservice::TableName &ccm_table_name,
        const std::unordered_map<
            txservice::TableName,
            std::pair<uint64_t, std::vector<txservice::TxKey>>>
            &sample_pool_map,
        uint64_t version) override;

    void FetchTableRanges(txservice::FetchTableRangesCc *fetch_cc) override;
    static void OnFetchTableRanges(CassFuture *future, void *fetch_req);

    void FetchRangeSlices(txservice::FetchRangeSlicesReq *fetch_cc) override;
    static void OnFetchRangeSlices(CassFuture *future, void *fetch_req);

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
                                 int retry_count) override
    {
        LOG(ERROR) << "CassHandler::GetNextRangePartitionId not implemented!";
        assert(false);
        return false;
    }

    bool Read(const txservice::TableName &table_name,
              const txservice::TxKey &key,
              txservice::TxRecord &rec,
              bool &found,
              uint64_t &version_ts,
              const txservice::TableSchema *table_schema) override;

#ifdef ON_KEY_OBJECT
    txservice::store::DataStoreHandler::DataStoreOpStatus FetchRecord(
        txservice::FetchRecordCc *fetch_cc) override;

    static void OnFetchRecord(CassFuture *future, void *fetch_req);
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

    const CassPrepared *GetCachedPreparedStmt(const std::string &kv_table_name,
                                              uint64_t table_schema_ts,
                                              CassPreparedType stmt_type);

    const CassPrepared *CachePreparedStmt(const std::string &kv_table_name,
                                          uint64_t table_schema_ts,
                                          const CassPrepared *prepared_stmt,
                                          CassPreparedType stmt_type);

    bool CreateCachedPrepareStmt(const std::string &kv_table_name,
                                 uint64_t table_schema_ts,
                                 CassPreparedType stmt_type);

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

    void SetTxService(txservice::TxService *tx_service)
    {
        tx_service_ = tx_service;
    }

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
     * @brief  Get the latest visible(commit_ts <= upper_bound_ts) historical
     * version.
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

    static CassStatement *BuildStatement(CassSession *cass_session,
                                         const std::string &stmt_str);

    // Execute stmt_str in cassandra
    static std::pair<const CassResult *, CassStatement *> ExecuteStatement(
        CassSession *cass_session,
        const std::string &stmt_str,
        std::function<void(CassStatement *)> stmt_setup,
        bool return_cass_stmt = false);

    // Execute receive_row function on CassRow returned by stmt_str
    static bool ExecuteSelectStatement(
        CassSession *cass_session,
        const std::string &stmt_str,
        std::function<void(CassStatement *)> stmt_setup,
        std::function<bool(const CassRow *)> receive_row,
        int page_size);

    bool UpdateClusterConfig(
        const std::unordered_map<uint32_t, std::vector<txservice::NodeConfig>>
            &new_cnf,
        uint64_t version) override;

    bool NeedCopyRange() const override
    {
        return true;
    }

    bool InitPreBuiltTables();
    // call this function before Connect().
    bool AppendPreBuiltTable(std::string_view table_name)
    {
        pre_built_table_names_.emplace(table_name, table_name);
        return true;
    }

private:
    struct CallbackData
    {
        CallbackData() = delete;
        CallbackData(CassSession *session,
                     txservice::CcHandlerResult<txservice::Void> *hd_res)
            : session_(session), hd_res_(hd_res)
        {
        }

        CassSession *session_;
        txservice::CcHandlerResult<txservice::Void> *hd_res_;
    };

    struct UpsertTableData : public CallbackData
    {
        UpsertTableData() = delete;
        UpsertTableData(
            CassHandler *cass_hd,
            const txservice::TableName *table_name,
            const txservice::TableSchema *old_schema,
            const txservice::TableSchema *schema,
            txservice::OperationType op_type,
            CassSession *session,
            uint64_t write_time,
            bool is_bootstrap,
            bool ddl_skip_kv,
            std::shared_ptr<void> defer_unpin,
            txservice::CcHandlerResult<txservice::Void> *hd_res,
            txservice::TxService *tx_service,
            const txservice::AlterTableInfo *alter_table_info = nullptr);

        UpsertTableData *MarkPKTableForUpserting()
        {
            upserting_table_name_ = table_name_;
            return this;
        }

        UpsertTableData *MarkNextSKTableForUpserting();

        /*
         * @brief If the main table has indexes
         */
        bool HasSKTable();
        /*
         * @brief Rewind indexes iterator to its beginning
         */
        void RewindSKTableIteratorMarkFirstForUpserting();
        /*
         * @brief Is the indexes iterator go to the end
         */
        bool IsSKTableIteratorEnd();

        const txservice::TableName *GetMarkedUpsertingTableName()
        {
            return upserting_table_name_;
        }

        CassHandler *cass_hd_;
        const txservice::TableName *table_name_;
        const txservice::TableName *upserting_table_name_;
        const txservice::TableSchema *old_table_schema_;
        const txservice::TableSchema *table_schema_;
        std::unordered_map<
            uint,
            std::pair<txservice::TableName, txservice::SecondaryKeySchema>>::
            const_iterator indexes_it_;
        txservice::OperationType op_type_;
        std::unordered_map<txservice::TableName, std::string>::const_iterator
            add_indexes_it_;
        std::unordered_map<txservice::TableName, std::string>::const_iterator
            drop_indexes_it_;
        uint64_t write_time_;
        std::atomic_int32_t ref_count_;
        bool is_bootstrap_;
        bool ddl_skip_kv_;
        txservice::TxService *tx_service_;
        std::unordered_map<txservice::TableName, int32_t> initial_partition_id_;
        std::unordered_map<txservice::TableName, bool>
            partition_id_initialized_;
        const txservice::AlterTableInfo *alter_table_info_{nullptr};

        std::shared_ptr<void> defer_unpin_{nullptr};
    };

    struct RenameTableData : public CallbackData
    {
        std::string old_name_;
        std::string new_name_;
    };

    struct CreateIndexData : public CallbackData
    {
        std::string table_name_;
        std::string index_name_;
        const txservice::TableSchema *table_schema_;
    };

    struct ScanSliceData
    {
        ScanSliceData(
            txservice::LoadRangeSliceRequest *load_slice_req,
            CassSession *session,
            int32_t partition_id,
            const txservice::TableName *table_name,
            bool ddl_skip_kv,
            std::shared_ptr<void> defer_unpin,
            const txservice::KVCatalogInfo *kv_info = nullptr,
            CassHandler *handler = nullptr,
            CassPreparedType prepare_type = CassPreparedType::ScanSlice)
            : load_slice_req_(load_slice_req),
              session_(session),
              range_partition_id_(partition_id),
              table_name_(table_name),
              ddl_skip_kv_(ddl_skip_kv),
              defer_unpin_(defer_unpin),
              kv_info_(kv_info),
              handler_(handler),
              prepare_type_(prepare_type)
        {
        }

        txservice::LoadRangeSliceRequest *load_slice_req_;
        CassSession *session_;
        int32_t range_partition_id_;
        const txservice::TableName *table_name_;
        bool ddl_skip_kv_;
        CassStatement *scan_stmt_{nullptr};

        std::shared_ptr<void> defer_unpin_{nullptr};
        const txservice::KVCatalogInfo *kv_info_{nullptr};
        CassHandler *handler_{nullptr};
        CassPreparedType prepare_type_;

        metrics::TimePoint start_;
    };

    struct FetchRangeSpecData
    {
        FetchRangeSpecData(txservice::FetchTableRangesCc *fetch_cc,
                           CassSession *session,
                           CassStatement *stmt)
            : fetch_cc_(fetch_cc), session_(session), stmt_(stmt)
        {
        }

        txservice::FetchTableRangesCc *fetch_cc_;
        CassSession *session_;
        CassStatement *stmt_;
    };

    static void OnUpsertCassTable(CassFuture *future, void *data);
    static void UpsertSkTable(UpsertTableData *table_data);
    static void UpsertCatalog(UpsertTableData *table_data);
    static void OnUpsertCatalog(CassFuture *future, void *data);
    static void OnFetchCatalog(CassFuture *future, void *fetch_req);

    static void UpsertInitialRangePartitionIdInternal(
        UpsertTableData *table_data,
        const txservice::TableName &table_name,
        void (*on_upsert_initial_range_partition_id_function)(CassFuture *,
                                                              void *));

    static void UpsertLastRangePartitionIdInternal(
        UpsertTableData *table_data,
        const txservice::TableName &table_name,
        void (*on_upsert_last_range_partition_id_function)(CassFuture *,
                                                           void *));

    static void IterateSkIndexes(
        CassFuture *future,
        void *data,
        void (*step_function)(UpsertTableData *table_data),
        void (*on_step_function)(CassFuture *future, void *data),
        bool (*prepare_next_step_data)(UpsertTableData *table_data),
        void (*next_step_function)(UpsertTableData *table_data),
        void (*on_next_step_function)(CassFuture *future, void *data));

    static void OnUpsertDone(
        CassFuture *future,
        void *data,
        void (*next_step_function)(UpsertTableData *table_data),
        void (*on_next_step_function)(CassFuture *future, void *data));

    static bool PrepareUpsertSkTableIterator(UpsertTableData *table_data);
    static void PrepareTableRanges(UpsertTableData *table_data);
    static void OnPrepareTableRanges(CassFuture *future, void *data);
    static void CheckTableRangesVersion(UpsertTableData *table_data);
    static void OnCheckTableRangesVersion(CassFuture *future, void *data);
    static void UpsertInitialRangePartitionId(UpsertTableData *table_data);
    static void OnUpsertInitialRangePartitionId(CassFuture *future, void *data);
    static void UpsertLastRangePartitionId(UpsertTableData *table_data);
    static void OnUpsertLastRangePartitionId(CassFuture *future, void *data);

    static void UpsertTableStatistics(UpsertTableData *table_data);
    static void OnUpsertTableStatistics(CassFuture *future, void *data);

    static void UpsertSequence(UpsertTableData *table_data);
    static void OnUpsertSequence(CassFuture *future, void *data);

    static void OnLoadRangeSlice(CassFuture *future, void *data);

    static void CleanDefunctKvTables(void *store_hd);

    const CassPrepared *GetInsertPrepared(
        const txservice::TableName &table_name,
        const txservice::TableSchema *table_schema,
        const bool with_ttl = false);
    const CassPrepared *GetDeletePrepared(
        const txservice::TableName &table_name,
        const txservice::TableSchema *table_schema);
    const CassPrepared *GetReadPrepared(
        const txservice::TableName &table_name,
        const txservice::TableSchema *table_schema);

    CassStatement *PutAllCreateStatement(
        // const txservice::TableType table_type,
        const txservice::TableName &table_name,
        const CassPrepared *insert_prepared,
        const CassPrepared *insert_ttl_prepared,
        const CassPrepared *delete_prepared,
        const txservice::FlushRecord &ckpt_rec,
        const txservice::RecordSchema *rec_schema,
        int32_t pk1,
        int16_t pk2);

    bool PutAllExecute(const txservice::TableName &table_name,
                       const CassPrepared *insert_prepared,
                       const CassPrepared *insert_ttl_prepared,
                       const CassPrepared *delete_prepared,
                       std::vector<txservice::FlushRecord> &batch,
                       /*const MysqlTableSchema *table_schema,*/
                       const txservice::TableSchema *table_schema,
                       uint32_t node_group);

    bool ListKvTableCTimeMore1d(std::set<std::string> &kv_table_names) const;

    bool ListVisibleKvTable(std::set<std::string> &kv_table_names) const;

    // Simplify fetching error message from cassandra driver api
    static std::string_view ErrorMessage(CassFuture *future);

    static std::string GenerateUUID();

    class CachedPrepared
    {
    public:
        enum struct CachedPreparedStatus
        {
            None = 0,
            BeingBuilt,
            Cached
        };

        CachedPrepared(uint64_t table_schema_ts)
            : table_schema_ts_(table_schema_ts)
        {
        }

        ~CachedPrepared()
        {
            FreePrepared();
        }

        void FreePrepared()
        {
            std::unique_lock<std::shared_mutex> lock(s_mux_);
            for (auto &prepared_pair : cass_prepared_stmts_)
            {
                const CassPrepared *&prepared = std::get<0>(prepared_pair);
                if (prepared)
                {
                    cass_prepared_free(prepared);
                    prepared = nullptr;
                }
            }
        }

        const CassPrepared *GetPreparedStmt(CassPreparedType stmt_type) const
        {
            std::shared_lock<std::shared_mutex> lock(s_mux_);
            return cass_prepared_stmts_[static_cast<size_t>(stmt_type)].first;
        }

        std::pair<const CassPrepared *, bool> SetPreparedStmtNx(
            CassPreparedType stmt_type, const CassPrepared *prepared_stmt)
        {
            std::unique_lock<std::shared_mutex> lock(s_mux_);
            std::get<1>(cass_prepared_stmts_[static_cast<size_t>(stmt_type)]) =
                CachedPreparedStatus::Cached;
            const CassPrepared *&prepared_stmt_ref = std::get<0>(
                cass_prepared_stmts_[static_cast<size_t>(stmt_type)]);
            if (prepared_stmt_ref == nullptr)
            {
                assert(prepared_stmt != nullptr);
                prepared_stmt_ref = prepared_stmt;
                return {prepared_stmt_ref, true};
            }
            else
            {
                return {prepared_stmt_ref, false};
            }
        }

        uint64_t GetTableSchemaTs() const
        {
            std::shared_lock<std::shared_mutex> lock(s_mux_);
            return table_schema_ts_;
        }

        void SetTableSchemaTs(uint64_t schema_ts)
        {
            std::unique_lock<std::shared_mutex> lock(s_mux_);
            table_schema_ts_ = schema_ts;
        }

        CachedPreparedStatus GetCachedStatus(CassPreparedType stmt_type) const
        {
            std::shared_lock<std::shared_mutex> lock(s_mux_);
            return cass_prepared_stmts_[static_cast<size_t>(stmt_type)].second;
        }

        void SetCachedStatus(CassPreparedType stmt_type,
                             CachedPreparedStatus cached_status)
        {
            std::unique_lock<std::shared_mutex> lock(s_mux_);
            std::get<1>(cass_prepared_stmts_[static_cast<size_t>(stmt_type)]) =
                cached_status;
        }

    private:
        std::array<std::pair<const CassPrepared *, CachedPreparedStatus>,
                   static_cast<size_t>(CassPreparedType::EnumSize)>
            cass_prepared_stmts_{
                std::make_pair(nullptr, CachedPreparedStatus::None)};

        uint64_t table_schema_ts_{0};

        mutable std::shared_mutex s_mux_;
    };

    bool InitializeKeySpace();
    bool CreateMvccArchivesTable();
    void DecodeArchiveRowFromCassRow(const txservice::TableName &table_name,
                                     const CassRow *row,
                                     txservice::TxRecord &payload,
                                     txservice::RecordStatus &payload_status,
                                     uint64_t &commit_ts);

    CassCluster *cluster_;
    CassSession *session_;
    uint32_t write_batch_;
    uint32_t max_futures_;

    std::unordered_map<std::string, CachedPrepared> prepared_cache_;
    std::shared_mutex s_mux_;
    const std::string keyspace_name_;
    const std::string keyspace_class_;
    const std::string replicate_factor_;
    bool is_bootstrap_{false};
    // flag used to skip create/drop table on kv store to speed up test case.
    bool ddl_skip_kv_{false};

    // table names and their kv table names
    std::unordered_map<std::string, std::string> pre_built_table_names_;

    // Currently worker_pool_ is used to clean defunct kv tables, and its size
    // is only `1`.
    txservice::TxWorkerPool worker_pool_;

    bthread::TimerThread timer_thd_;

    // cassandra collection limits 64KB
    constexpr static uint32_t collection_max_size_{64 * 1024};
};

struct CassCatalogInfo : public txservice::KVCatalogInfo
{
public:
    using uptr = std::unique_ptr<CassCatalogInfo>;
    CassCatalogInfo()
    {
    }
    CassCatalogInfo(const std::string &kv_table_name,
                    const std::string &kv_index_names);
    ~CassCatalogInfo()
    {
    }
    std::string Serialize() const override;
    void Deserialize(const char *buf, size_t &offset) override;
};
}  // namespace EloqDS
