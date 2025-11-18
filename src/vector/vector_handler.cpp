/**
 * @file vector_handler.cpp
 * @brief Implementation of vector operations handler for EloqVec
 *
 * This file implements the vector_handler class methods for vector index
 * operations including create, search, info, add, and drop operations.
 *
 * @author EloqData Inc.
 * @date 2025
 */

#include "vector_handler.h"

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <utility>

#include "hnsw_vector_index.h"
#include "log_object.h"
#include "predicate.h"
#include "tx_request.h"
#include "tx_util.h"
#include "tx_worker_pool.h"
#include "vector_util.h"

namespace EloqVec
{
// Using declarations instead of using-directive
using txservice::AbortTx;
using txservice::CcProtocol;
using txservice::CommitTx;
using txservice::EloqStringKey;
using txservice::EloqStringRecord;
using txservice::IsolationLevel;
using txservice::LocalCcShards;
using txservice::NewTxInit;
using txservice::OperationType;
using txservice::ReadTxRequest;
using txservice::RecordStatus;
using txservice::TransactionExecution;
using txservice::TxKey;
using txservice::TxRecord;
using txservice::TxService;
using txservice::UpsertTxRequest;

// Constants for sharded log operations
constexpr uint32_t VECTOR_INDEX_LOG_SHARD_COUNT = 1024;

inline std::string build_metadata_key(const std::string &name)
{
    std::string key_pattern("vector_index:");
    key_pattern.append(name).append(":metadata");
    return key_pattern;
}

inline std::string build_log_name(const std::string &name)
{
    std::string name_pattern("vector_index:");
    name_pattern.append(name);
    return name_pattern;
}

inline void serialize_vector(const std::vector<float> &vector,
                             std::string &result)
{
    size_t vec_size = vector.size();
    result.reserve(vec_size * sizeof(float));
    result.append(reinterpret_cast<const char *>(vector.data()),
                  vec_size * sizeof(float));
}

inline void deserialize_vector(const std::string &vec_str,
                               std::vector<float> &result)
{
    size_t vec_size = vec_str.size() / sizeof(float);
    result.reserve(vec_size);
    size_t offset = 0;
    for (size_t i = 0; i < vec_size; ++i)
    {
        result.emplace_back(
            *reinterpret_cast<const float *>(vec_str.data() + offset));
        offset += sizeof(float);
    }
}

static std::unique_ptr<VectorHandler> vector_handler_instance = nullptr;
static std::once_flag vector_handler_once;

bool VectorHandler::InitHandlerInstance(
    TxService *tx_service,
    txservice::TxWorkerPool *vector_index_worker_pool,
    std::string &vector_index_data_path,
    const CloudConfig *cloud_config)
{
    std::call_once(vector_handler_once,
                   [tx_service,
                    vector_index_worker_pool,
                    &vector_index_data_path,
                    cloud_config]() -> bool
                   {
                       vector_handler_instance = std::unique_ptr<VectorHandler>(
                           new VectorHandler(tx_service,
                                             vector_index_worker_pool,
                                             vector_index_data_path,
                                             cloud_config));
                       if (!vector_handler_instance->initialized_)
                       {
                           vector_handler_instance.reset();
                           return false;
                       }
                       return true;
                   });
    return vector_handler_instance != nullptr &&
           vector_handler_instance->initialized_;
}

void VectorHandler::DestroyHandlerInstance()
{
    vector_handler_instance.reset();
}

VectorHandler &VectorHandler::Instance()
{
    if (!vector_handler_instance)
    {
        throw std::runtime_error("VectorHandler instance is not initialized");
    }
    return *vector_handler_instance;
}

/**
 * @brief Create a new vector index metadata entry and initialize its lifecycle
 * log.
 *
 * Creates metadata for the vector index described by idx_spec in the internal
 * metadata table and adds a corresponding log object. If an index with the
 * same name already exists this function does not modify state.
 *
 * @param idx_spec Index configuration (name, dimension, algorithm, storage
 * path, etc.).
 * @return VectorOpResult indicating the outcome:
 *   - SUCCEED: metadata and log were created successfully.
 *   - INDEX_EXISTED: an index with the given name already exists.
 *   - INDEX_META_OP_FAILED: a metadata table or log operation failed.
 */
VectorOpResult VectorHandler::Create(const VectorIndexMetadata &index_metadata)
{
    TransactionExecution *txm =
        NewTxInit(tx_service_, IsolationLevel::RepeatableRead, CcProtocol::OCC);
    // For the internal table, there is no need to acquire read lock on catalog.
    const std::string &index_name = index_metadata.Name();
    std::string h_key = build_metadata_key(index_name);
    TxKey tx_key = EloqStringKey::Create(h_key.c_str(), h_key.size());
    TxRecord::Uptr h_record = EloqStringRecord::Create();
    ReadTxRequest read_req(
        &vector_index_meta_table, 0, &tx_key, h_record.get(), true);
    txm->Execute(&read_req);
    read_req.Wait();
    if (read_req.IsError())
    {
        AbortTx(txm);
        return VectorOpResult::INDEX_META_OP_FAILED;
    }

    if (read_req.Result().first == RecordStatus::Normal)
    {
        CommitTx(txm);
        // The index already exists
        return VectorOpResult::INDEX_EXISTED;
    }

    // The index does not exist, create it
    // 1. Check if the configuration is valid
    const IndexConfig &config = index_metadata.Config();
    bool is_valid = false;
    switch (config.algorithm)
    {
    case Algorithm::HNSW:
    {
        is_valid = HNSWVectorIndex::validate_config(config);
        break;
    }
    default:
        is_valid = false;
        break;
    }

    if (!is_valid)
    {
        AbortTx(txm);
        return VectorOpResult::INDEX_INIT_FAILED;
    }

    // 2. serialize the index metadata
    std::string serialized_str;
    index_metadata.Serialize(serialized_str);
    h_record->SetEncodedBlob(
        reinterpret_cast<const unsigned char *>(serialized_str.data()),
        serialized_str.size());
    // 3. upsert the metadata to the internal table
    // There is no need to check the schema ts of the internal table.
    UpsertTxRequest upsert_req(&vector_index_meta_table,
                               std::move(tx_key),
                               std::move(h_record),
                               OperationType::Insert);
    txm->Execute(&upsert_req);
    upsert_req.Wait();
    if (upsert_req.IsError())
    {
        AbortTx(txm);
        return VectorOpResult::INDEX_META_OP_FAILED;
    }

    // 4. create the sharded log objects
    LogError log_err = LogObject::create_sharded_logs(
        build_log_name(index_name), VECTOR_INDEX_LOG_SHARD_COUNT, txm);
    if (log_err != LogError::SUCCESS)
    {
        AbortTx(txm);
        return VectorOpResult::INDEX_META_OP_FAILED;
    }

    // 5. commit the transaction
    auto res_pair = CommitTx(txm);
    return res_pair.first ? VectorOpResult::SUCCEED
                          : VectorOpResult::INDEX_META_OP_FAILED;
}

/**
 * @brief Remove a vector index's metadata entry and its associated log.
 *
 * Attempts to delete the stored metadata for the index named @p name from the
 * internal metadata table and removes the corresponding log object.
 *
 * On success the metadata entry and log are removed; the index file and any
 * in-memory index instance are not modified by this function (TODO).
 *
 * @param name Index name whose metadata and log should be deleted.
 * @return VectorOpResult
 *   - VectorOpResult::SUCCEED on successful deletion.
 *   - VectorOpResult::INDEX_NOT_EXIST if no metadata entry exists for @p name.
 *   - VectorOpResult::INDEX_META_OP_FAILED if a metadata read/upsert or log
 *     removal operation fails.
 */
VectorOpResult VectorHandler::Drop(const std::string &name)
{
    TransactionExecution *txm =
        NewTxInit(tx_service_, IsolationLevel::RepeatableRead, CcProtocol::OCC);
    // 1. Check if the index exists by reading from vector_index_meta_table
    std::string h_key = build_metadata_key(name);
    TxKey tx_key = EloqStringKey::Create(h_key.c_str(), h_key.size());
    TxRecord::Uptr h_record = EloqStringRecord::Create();
    ReadTxRequest read_req(
        &vector_index_meta_table, 0, &tx_key, h_record.get(), true);
    txm->Execute(&read_req);
    read_req.Wait();
    if (read_req.IsError())
    {
        AbortTx(txm);
        return VectorOpResult::INDEX_META_OP_FAILED;
    }

    // 2. Check if the index exists
    if (read_req.Result().first != RecordStatus::Normal)
    {
        assert(read_req.Result().first == RecordStatus::Deleted);
        CommitTx(txm);
        // The index does not exist
        return VectorOpResult::INDEX_NOT_EXIST;
    }

    // 3. Deserialize metadata to get file path before deletion
    uint64_t index_version = read_req.Result().second;
    VectorIndexMetadata metadata;
    const char *blob_data = h_record->EncodedBlobData();
    size_t blob_size = h_record->EncodedBlobSize();
    size_t offset = 0;
    metadata.Deserialize(blob_data, blob_size, offset, index_version);
    const std::string &file_path = metadata.FilePath();

    // 4. The index exists, delete it using OperationType::Delete
    UpsertTxRequest upsert_req(&vector_index_meta_table,
                               std::move(tx_key),
                               nullptr,
                               OperationType::Delete);
    txm->Execute(&upsert_req);
    upsert_req.Wait();
    if (upsert_req.IsError())
    {
        AbortTx(txm);
        return VectorOpResult::INDEX_META_OP_FAILED;
    }

    // 5. delete the sharded log objects
    LogError log_err = LogObject::remove_sharded_logs(
        build_log_name(name), VECTOR_INDEX_LOG_SHARD_COUNT, txm);
    if (log_err != LogError::SUCCESS)
    {
        AbortTx(txm);
        return VectorOpResult::INDEX_META_OP_FAILED;
    }

    auto res_pair = CommitTx(txm);
    if (!res_pair.first)
    {
        return VectorOpResult::INDEX_META_OP_FAILED;
    }

    // 6. Delete the index file and the vector index object in memory
    {
        std::lock_guard<std::shared_mutex> lock(vec_indexes_mutex_);
        // Remove from cache
        vec_indexes_.erase(name);
    }

    // Delete the physical file
    if (!file_path.empty() && std::filesystem::exists(file_path))
    {
        try
        {
            std::filesystem::remove(file_path);
            // Delete the file from the cloud service
            if (cloud_manager_)
            {
                cloud_manager_->DeleteFile(
                    file_path.substr(vector_index_data_path_.size()));
            }
        }
        catch (const std::filesystem::filesystem_error &e)
        {
            LOG(WARNING) << "Failed to delete index file: " << file_path
                         << ", error: " << e.what();
        }
    }

    return VectorOpResult::SUCCEED;
}

VectorOpResult VectorHandler::Info(const std::string &name,
                                   VectorIndexMetadata &metadata)
{
    TransactionExecution *txm =
        NewTxInit(tx_service_, IsolationLevel::RepeatableRead, CcProtocol::OCC);
    // 1. Check if the index exists by reading from vector_index_meta_table
    std::string h_key = build_metadata_key(name);
    TxKey tx_key = EloqStringKey::Create(h_key.c_str(), h_key.size());
    TxRecord::Uptr h_record = EloqStringRecord::Create();
    ReadTxRequest read_req(
        &vector_index_meta_table, 0, &tx_key, h_record.get());
    txm->Execute(&read_req);
    read_req.Wait();
    if (read_req.IsError())
    {
        AbortTx(txm);
        return VectorOpResult::INDEX_META_OP_FAILED;
    }

    // 2. Check if the index exists
    if (read_req.Result().first != RecordStatus::Normal)
    {
        assert(read_req.Result().first == RecordStatus::Deleted);
        CommitTx(txm);
        // The index does not exist
        return VectorOpResult::INDEX_NOT_EXIST;
    }

    uint64_t index_version = read_req.Result().second;

    // 3. The index exists, deserialize the metadata
    const char *blob_data = h_record->EncodedBlobData();
    size_t blob_size = h_record->EncodedBlobSize();
    size_t offset = 0;
    metadata.Deserialize(blob_data, blob_size, offset, index_version);

    CommitTx(txm);
    return VectorOpResult::SUCCEED;
}

VectorOpResult VectorHandler::Search(const std::string &name,
                                     const std::vector<float> &query_vector,
                                     size_t k_count,
                                     size_t thread_id,
                                     const std::string_view &filter_json,
                                     SearchResult &vector_result)
{
    TransactionExecution *txm =
        NewTxInit(tx_service_, IsolationLevel::RepeatableRead, CcProtocol::OCC);
    // 1. Check if the index exists by reading from vector_index_meta_table
    std::string h_key = build_metadata_key(name);
    TxKey tx_key = EloqStringKey::Create(h_key.c_str(), h_key.size());
    TxRecord::Uptr h_record = EloqStringRecord::Create();
    ReadTxRequest read_req(
        &vector_index_meta_table, 0, &tx_key, h_record.get());
    txm->Execute(&read_req);
    read_req.Wait();
    if (read_req.IsError())
    {
        AbortTx(txm);
        return VectorOpResult::INDEX_META_OP_FAILED;
    }

    // 2. Check if the index exists
    if (read_req.Result().first != RecordStatus::Normal)
    {
        assert(read_req.Result().first == RecordStatus::Deleted);
        CommitTx(txm);
        // The index does not exist
        return VectorOpResult::INDEX_NOT_EXIST;
    }

    // 3. Get or create the vector index from cache
    auto [cache, get_result] = GetOrCreateIndex(name, h_record, txm);
    if (get_result != VectorOpResult::SUCCEED)
    {
        AbortTx(txm);
        return get_result;
    }

    // 4. Parse filter JSON if provided
    std::optional<std::function<bool(VectorId)>> filter_func = std::nullopt;
    std::optional<PredicateExpression> filter_expr = std::nullopt;
    if (!filter_json.empty())
    {
        // Parse JSON filter with schema
        filter_expr = std::make_optional<PredicateExpression>();
        if (!filter_expr->Parse(filter_json, cache.metadata_->Metadata()))
        {
            AbortTx(txm);
            return VectorOpResult::METADATA_OP_FAILED;
        }

        // Create lightweight filter function
        filter_func =
            [&filter_expr = filter_expr.value(),
             &schema = cache.metadata_->Metadata()](VectorId vector_id) -> bool
        {
            std::vector<size_t> offsets;
            schema.Decode(vector_id.metadata_, offsets);
            return filter_expr.Evaluate(vector_id.metadata_, offsets, schema);
        };
    }

    // 5. Perform vector search with optional filter
    auto search_result = cache.index_->search(
        query_vector, k_count, thread_id, vector_result, false, filter_func);
    CommitTx(txm);
    return search_result.error;
}

VectorOpResult VectorHandler::Add(const std::string &name,
                                  uint64_t id,
                                  const std::vector<float> &vector,
                                  const std::string_view &metadata)
{
    TransactionExecution *txm =
        NewTxInit(tx_service_, IsolationLevel::RepeatableRead, CcProtocol::OCC);
    // 1. Check if the index exists by reading from vector_index_meta_table
    std::string h_key = build_metadata_key(name);
    TxKey tx_key = EloqStringKey::Create(h_key.c_str(), h_key.size());
    TxRecord::Uptr h_record = EloqStringRecord::Create();
    ReadTxRequest read_req(
        &vector_index_meta_table, 0, &tx_key, h_record.get());
    txm->Execute(&read_req);
    read_req.Wait();
    if (read_req.IsError())
    {
        AbortTx(txm);
        return VectorOpResult::INDEX_META_OP_FAILED;
    }
    // 2. Check if the index exists
    if (read_req.Result().first != RecordStatus::Normal)
    {
        assert(read_req.Result().first == RecordStatus::Deleted);
        CommitTx(txm);
        // The index does not exist
        return VectorOpResult::INDEX_NOT_EXIST;
    }
    // 3. Get or create the vector index from cache
    auto [cache, get_result] = GetOrCreateIndex(name, h_record, txm);
    if (get_result != VectorOpResult::SUCCEED)
    {
        AbortTx(txm);
        return get_result;
    }

    // 4. Parse and validate metadata JSON
    std::vector<char> encoded_metadata;
    if (!metadata.empty() &&
        !cache.metadata_->Metadata().Encode(metadata, encoded_metadata))
    {
        AbortTx(txm);
        return VectorOpResult::METADATA_OP_FAILED;
    }

    // 5. Add item to log object.
    VectorId vector_id(id, std::move(encoded_metadata));
    std::string serialized_id;
    vector_id.Serialize(serialized_id);
    std::string serialized_vector;
    serialize_vector(vector, serialized_vector);
    uint64_t log_id = 0;
    uint64_t log_count = 0;
    uint64_t ts = LocalCcShards::ClockTs();
    std::vector<log_item_t> log_items;
    log_items.emplace_back(
        LogOperationType::INSERT, serialized_id, serialized_vector, ts, 0);
    LogError log_err =
        LogObject::append_log_sharded(build_log_name(name),
                                      std::to_string(id),
                                      VECTOR_INDEX_LOG_SHARD_COUNT,
                                      log_items,
                                      log_id,
                                      log_count,
                                      txm);
    if (log_err != LogError::SUCCESS)
    {
        AbortTx(txm);
        return VectorOpResult::INDEX_LOG_OP_FAILED;
    }

    // 6. Add the vector to the index
    auto add_result = cache.index_->add(vector, vector_id);
    if (add_result.error == VectorOpResult::SUCCEED)
    {
        auto res_pair = CommitTx(txm);
        if (!res_pair.first)
        {
            // Remove the vector from the index cache.
            cache.index_->remove(vector_id);
            return VectorOpResult::INDEX_ADD_FAILED;
        }

        if (cache.metadata_->PersistThreshold() == -1)
        {
            // No need to persist the index.
            return VectorOpResult::SUCCEED;
        }

        // Persist the index if it has enough items
        std::lock_guard<std::shared_mutex> lock(vec_indexes_mutex_);
        uint64_t estimate_log_count = log_count * VECTOR_INDEX_LOG_SHARD_COUNT;
        if (pending_persist_indexes_.find(name) ==
                pending_persist_indexes_.end() &&
            estimate_log_count >=
                static_cast<uint64_t>(cache.metadata_->PersistThreshold()))
        {
            pending_persist_indexes_.insert(name);
            vector_index_worker_pool_->SubmitWork(
                [name](size_t thread_id)
                { VectorHandler::Instance().PersistIndex(name); });
        }
    }
    else
    {
        AbortTx(txm);
    }

    return add_result.error;
}

VectorOpResult VectorHandler::Update(const std::string &name,
                                     uint64_t id,
                                     const std::vector<float> &vector,
                                     const std::string_view &metadata)
{
    TransactionExecution *txm =
        NewTxInit(tx_service_, IsolationLevel::RepeatableRead, CcProtocol::OCC);
    // 1. Check if the index exists by reading from vector_index_meta_table
    std::string h_key = build_metadata_key(name);
    TxKey tx_key = EloqStringKey::Create(h_key.c_str(), h_key.size());
    TxRecord::Uptr h_record = EloqStringRecord::Create();
    ReadTxRequest read_req(
        &vector_index_meta_table, 0, &tx_key, h_record.get());
    txm->Execute(&read_req);
    read_req.Wait();
    if (read_req.IsError())
    {
        AbortTx(txm);
        return VectorOpResult::INDEX_META_OP_FAILED;
    }

    // 2. Check if the index exists
    if (read_req.Result().first != RecordStatus::Normal)
    {
        assert(read_req.Result().first == RecordStatus::Deleted);
        CommitTx(txm);
        // The index does not exist
        return VectorOpResult::INDEX_NOT_EXIST;
    }

    // 3. Get or create the vector index from cache
    auto [cache, get_result] = GetOrCreateIndex(name, h_record, txm);
    if (get_result != VectorOpResult::SUCCEED)
    {
        AbortTx(txm);
        return get_result;
    }

    // 4. Parse and validate metadata JSON
    std::vector<char> encoded_metadata;
    if (!metadata.empty() &&
        !cache.metadata_->Metadata().Encode(metadata, encoded_metadata))
    {
        AbortTx(txm);
        return VectorOpResult::METADATA_OP_FAILED;
    }

    // 5. Add item to log object.
    VectorId vector_id(id, std::move(encoded_metadata));
    std::string serialized_id;
    vector_id.Serialize(serialized_id);
    std::string serialized_vector;
    serialize_vector(vector, serialized_vector);
    uint64_t log_id = 0;
    uint64_t log_count = 0;
    uint64_t ts = LocalCcShards::ClockTs();
    std::vector<log_item_t> log_items;
    log_items.emplace_back(
        LogOperationType::UPDATE, serialized_id, serialized_vector, ts, 0);
    LogError log_err =
        LogObject::append_log_sharded(build_log_name(name),
                                      std::to_string(id),
                                      VECTOR_INDEX_LOG_SHARD_COUNT,
                                      log_items,
                                      log_id,
                                      log_count,
                                      txm);
    if (log_err != LogError::SUCCESS)
    {
        AbortTx(txm);
        return VectorOpResult::INDEX_LOG_OP_FAILED;
    }

    // 6. Update the vector in the index
    std::vector<float> update_vec;
    auto idx_res = cache.index_->get(vector_id, update_vec);
    if (idx_res.error != VectorOpResult::SUCCEED || update_vec.size() == 0)
    {
        // The index operation failed or the vector with this id does not exist.
        AbortTx(txm);
        return idx_res.error;
    }

    idx_res = cache.index_->update(vector, vector_id);
    if (idx_res.error == VectorOpResult::SUCCEED)
    {
        auto res_pair = CommitTx(txm);
        if (!res_pair.first)
        {
            // Restore the vector to the index cache.
            cache.index_->update(update_vec, vector_id);
            return VectorOpResult::INDEX_UPDATE_FAILED;
        }

        if (cache.metadata_->PersistThreshold() == -1)
        {
            // No need to persist the index.
            return VectorOpResult::SUCCEED;
        }

        // Persist the index if it has enough items
        uint64_t estimate_log_count = log_count * VECTOR_INDEX_LOG_SHARD_COUNT;
        std::lock_guard<std::shared_mutex> lock(vec_indexes_mutex_);
        if (pending_persist_indexes_.find(name) ==
                pending_persist_indexes_.end() &&
            estimate_log_count >=
                static_cast<uint64_t>(cache.metadata_->PersistThreshold()))
        {
            pending_persist_indexes_.insert(name);
            vector_index_worker_pool_->SubmitWork(
                [name](size_t thread_id)
                { VectorHandler::Instance().PersistIndex(name); });
        }
    }
    else
    {
        AbortTx(txm);
    }

    return idx_res.error;
}

VectorOpResult VectorHandler::Delete(const std::string &name, uint64_t id)
{
    TransactionExecution *txm =
        NewTxInit(tx_service_, IsolationLevel::RepeatableRead, CcProtocol::OCC);
    // 1. Check if the index exists by reading from vector_index_meta_table
    std::string h_key = build_metadata_key(name);
    TxKey tx_key = EloqStringKey::Create(h_key.c_str(), h_key.size());
    TxRecord::Uptr h_record = EloqStringRecord::Create();
    ReadTxRequest read_req(
        &vector_index_meta_table, 0, &tx_key, h_record.get());
    txm->Execute(&read_req);
    read_req.Wait();
    if (read_req.IsError())
    {
        AbortTx(txm);
        return VectorOpResult::INDEX_META_OP_FAILED;
    }

    // 2. Check if the index exists
    if (read_req.Result().first != RecordStatus::Normal)
    {
        assert(read_req.Result().first == RecordStatus::Deleted);
        CommitTx(txm);
        // The index does not exist
        return VectorOpResult::INDEX_NOT_EXIST;
    }

    // 3. Get or create the vector index from cache
    auto [cache, get_result] = GetOrCreateIndex(name, h_record, txm);
    if (get_result != VectorOpResult::SUCCEED)
    {
        AbortTx(txm);
        return get_result;
    }

    // Add item to log object.
    VectorId vector_id(id);
    std::string serialized_id;
    vector_id.Serialize(serialized_id);
    std::string serialized_empty_vec;
    uint64_t log_id = 0;
    uint64_t log_count = 0;
    uint64_t ts = LocalCcShards::ClockTs();
    std::vector<log_item_t> log_items;
    log_items.emplace_back(
        LogOperationType::DELETE, serialized_id, serialized_empty_vec, ts, 0);
    LogError log_err =
        LogObject::append_log_sharded(build_log_name(name),
                                      std::to_string(id),
                                      VECTOR_INDEX_LOG_SHARD_COUNT,
                                      log_items,
                                      log_id,
                                      log_count,
                                      txm);
    if (log_err != LogError::SUCCESS)
    {
        AbortTx(txm);
        return VectorOpResult::INDEX_LOG_OP_FAILED;
    }

    // 4. Delete the vector from the index
    std::vector<float> deleted_vector;
    auto idx_res = cache.index_->get(vector_id, deleted_vector);
    if (idx_res.error != VectorOpResult::SUCCEED || deleted_vector.size() == 0)
    {
        // The index operation failed or the vector with this id does not exist.
        AbortTx(txm);
        return idx_res.error;
    }

    idx_res = cache.index_->remove(vector_id);
    if (idx_res.error == VectorOpResult::SUCCEED)
    {
        auto res_pair = CommitTx(txm);
        if (!res_pair.first)
        {
            // Restore the vector to the index cache.
            cache.index_->add(deleted_vector, vector_id);
            return VectorOpResult::INDEX_DELETE_FAILED;
        }

        if (cache.metadata_->PersistThreshold() == -1)
        {
            // No need to persist the index.
            return VectorOpResult::SUCCEED;
        }

        // Persist the index if it has enough items
        std::lock_guard<std::shared_mutex> lock(vec_indexes_mutex_);
        uint64_t estimate_log_count = log_count * VECTOR_INDEX_LOG_SHARD_COUNT;
        if (pending_persist_indexes_.find(name) ==
                pending_persist_indexes_.end() &&
            estimate_log_count >=
                static_cast<uint64_t>(cache.metadata_->PersistThreshold()))
        {
            pending_persist_indexes_.insert(name);
            vector_index_worker_pool_->SubmitWork(
                [name](size_t thread_id)
                { VectorHandler::Instance().PersistIndex(name); });
        }
    }
    else
    {
        AbortTx(txm);
    }

    return idx_res.error;
}

VectorOpResult VectorHandler::BatchAdd(
    const std::string &name,
    const std::vector<uint64_t> &ids,
    const std::vector<std::vector<float>> &vectors,
    const std::vector<std::string_view> &metadata_list)
{
    // Check if the ids and vectors are valid
    if (ids.empty() || ids.size() != vectors.size())
    {
        return VectorOpResult::INDEX_ADD_FAILED;
    }

    // Check if metadata_list size matches
    if (!metadata_list.empty() && metadata_list.size() != ids.size())
    {
        return VectorOpResult::INDEX_ADD_FAILED;
    }

    TransactionExecution *txm =
        NewTxInit(tx_service_, IsolationLevel::RepeatableRead, CcProtocol::OCC);

    // 1. Check if the index exists by reading from vector_index_meta_table
    std::string h_key = build_metadata_key(name);
    TxKey tx_key = EloqStringKey::Create(h_key.c_str(), h_key.size());
    TxRecord::Uptr h_record = EloqStringRecord::Create();
    ReadTxRequest read_req(
        &vector_index_meta_table, 0, &tx_key, h_record.get());
    txm->Execute(&read_req);
    read_req.Wait();
    if (read_req.IsError())
    {
        AbortTx(txm);
        return VectorOpResult::INDEX_META_OP_FAILED;
    }

    // 2. Check if the index exists
    if (read_req.Result().first != RecordStatus::Normal)
    {
        assert(read_req.Result().first == RecordStatus::Deleted);
        CommitTx(txm);
        return VectorOpResult::INDEX_NOT_EXIST;
    }

    // 3. Get or create the vector index from cache
    auto [cache, get_result] = GetOrCreateIndex(name, h_record, txm);
    if (get_result != VectorOpResult::SUCCEED)
    {
        AbortTx(txm);
        return get_result;
    }

    // 4. Parse and validate metadata JSON list
    std::vector<std::vector<char>> encoded_metadata_list;
    encoded_metadata_list.resize(ids.size());
    if (!metadata_list.empty())
    {
        for (size_t i = 0; i < metadata_list.size(); ++i)
        {
            std::vector<char> encoded_metadata;
            if (!metadata_list[i].empty() &&
                !cache.metadata_->Metadata().Encode(metadata_list[i],
                                                    encoded_metadata))
            {
                AbortTx(txm);
                return VectorOpResult::METADATA_OP_FAILED;
            }
            encoded_metadata_list[i] = std::move(encoded_metadata);
        }
    }

    // 5. Group ids by shard id
    // Sort the shard id to avoid potential deadlock.
    std::map<uint32_t, std::vector<size_t>> shard_group;
    for (size_t i = 0; i < ids.size(); i++)
    {
        uint32_t shard_id = LogObject::get_shard_id(
            std::to_string(ids[i]), VECTOR_INDEX_LOG_SHARD_COUNT);
        // Reserve memory for new shard to reduce frequent allocation
        auto res_pair = shard_group.try_emplace(shard_id);
        if (res_pair.second)
        {
            res_pair.first->second.reserve(ids.size());
        }
        res_pair.first->second.push_back(i);
    }

    // 6. Add items to log object by shard
    uint64_t ts = LocalCcShards::ClockTs();
    uint64_t log_id = 0;
    uint64_t log_count = 0;
    // NOTE: the sequence of vector_ids is not same as the sequence of ids,
    // because the vector_ids are constructed in the order of shard_group.
    std::vector<VectorId> vector_ids;
    vector_ids.reserve(ids.size());
    // Process each shard
    for (const auto &shard_pair : shard_group)
    {
        const std::vector<size_t> &indices = shard_pair.second;

        std::vector<log_item_t> log_items;
        log_items.reserve(indices.size());

        // Process each index in this shard
        for (size_t idx : indices)
        {
            // Serialize the vector at this index
            vector_ids.emplace_back(ids[idx],
                                    std::move(encoded_metadata_list[idx]));
            std::string serialized_id;
            vector_ids.back().Serialize(serialized_id);
            std::string serialized_vector;
            serialize_vector(vectors[idx], serialized_vector);
            // Create log item
            log_items.emplace_back(LogOperationType::INSERT,
                                   serialized_id,
                                   serialized_vector,
                                   ts,
                                   0);
        }

        // Append all items for this shard
        LogError log_err = LogObject::append_log_sharded(
            build_log_name(name),
            std::to_string(ids[indices[0]]),  // Use first id as shard key
            VECTOR_INDEX_LOG_SHARD_COUNT,
            log_items,
            log_id,
            log_count,
            txm);

        if (log_err != LogError::SUCCESS)
        {
            AbortTx(txm);
            return VectorOpResult::INDEX_LOG_OP_FAILED;
        }
    }

    // 7. Add the vectors to the index
    auto add_result = cache.index_->add_batch(vectors, vector_ids);
    if (add_result.error == VectorOpResult::SUCCEED)
    {
        auto res_pair = CommitTx(txm);
        if (!res_pair.first)
        {
            for (const VectorId &vector_id : vector_ids)
            {
                cache.index_->remove(vector_id);
            }
            return VectorOpResult::INDEX_ADD_FAILED;
        }

        if (cache.metadata_->PersistThreshold() == -1)
        {
            // No need to persist the index.
            return VectorOpResult::SUCCEED;
        }

        // Persist the index if it has enough items
        std::lock_guard<std::shared_mutex> lock(vec_indexes_mutex_);
        uint64_t estimate_log_count = log_count * VECTOR_INDEX_LOG_SHARD_COUNT;
        if (pending_persist_indexes_.find(name) ==
                pending_persist_indexes_.end() &&
            estimate_log_count >=
                static_cast<uint64_t>(cache.metadata_->PersistThreshold()))
        {
            pending_persist_indexes_.insert(name);
            vector_index_worker_pool_->SubmitWork(
                [name](size_t thread_id)
                { VectorHandler::Instance().PersistIndex(name); });
        }
    }
    else
    {
        AbortTx(txm);
    }

    return add_result.error;
}

std::pair<VectorHandler::IndexCache, VectorOpResult>
VectorHandler::GetOrCreateIndex(const std::string &name,
                                const TxRecord::Uptr &h_record,
                                TransactionExecution *txm)
{
    // First, try to get read lock to check cache (single lookup!)
    {
        std::shared_lock<std::shared_mutex> read_lock(vec_indexes_mutex_);
        auto it = vec_indexes_.find(name);
        if (it != vec_indexes_.end())
        {
            return {it->second, VectorOpResult::SUCCEED};
        }
    }

    // Need to create new index and initialize it
    {
        std::lock_guard<std::shared_mutex> write_lock(vec_indexes_mutex_);
        // Double-checked locking pattern
        auto it = vec_indexes_.find(name);
        if (it != vec_indexes_.end())
        {
            return {it->second, VectorOpResult::SUCCEED};
        }

        // Create and initialize the index
        auto [cache_entry, init_result] =
            CreateAndInitializeIndex(h_record, txm);
        if (init_result != VectorOpResult::SUCCEED)
        {
            return {IndexCache{}, init_result};
        }

        // Insert unified cache entry into the cache
        auto res = vec_indexes_.emplace(name, std::move(cache_entry));
        assert(res.second);

        return {res.first->second, VectorOpResult::SUCCEED};
    }
}

std::pair<VectorHandler::IndexCache, VectorOpResult>
VectorHandler::CreateAndInitializeIndex(const TxRecord::Uptr &h_record,
                                        TransactionExecution *txm)
{
    // Deserialize metadata from h_record
    auto metadata_sptr = std::make_shared<VectorIndexMetadata>();
    const char *blob_data = h_record->EncodedBlobData();
    size_t blob_size = h_record->EncodedBlobSize();
    size_t offset = 0;
    uint64_t unused_version = 0;
    metadata_sptr->Deserialize(blob_data, blob_size, offset, unused_version);

    // Get IndexConfig directly from metadata
    const IndexConfig &config = metadata_sptr->Config();
    const std::string &index_name = metadata_sptr->Name();
    const std::string &file_path = metadata_sptr->FilePath();

    std::shared_ptr<VectorIndex> index_sptr = nullptr;
    switch (config.algorithm)
    {
    case Algorithm::HNSW:
        index_sptr = std::make_shared<HNSWVectorIndex>();
        break;
    default:
        return {IndexCache{}, VectorOpResult::INDEX_INIT_FAILED};
    }

    // Check if the index file exists
    if (file_path.find(initial_timestamp_sv) == std::string::npos)
    {
        bool local_file_exists = std::filesystem::exists(file_path);
        if (!cloud_manager_ && !local_file_exists)
        {
            return {IndexCache{}, VectorOpResult::INDEX_INIT_FAILED};
        }
        else if (cloud_manager_)
        {
            // Remove the index file if it exists in local.
            std::filesystem::remove(file_path);
            // Download the index file from the cloud service
            if (!cloud_manager_->DownloadFile(
                    file_path.substr(vector_index_data_path_.size()),
                    file_path))
            {
                return {IndexCache{}, VectorOpResult::INDEX_INIT_FAILED};
            }
        }
    }

    // Initialize the index with config and file path
    if (!index_sptr->initialize(config, file_path))
    {
        return {IndexCache{}, VectorOpResult::INDEX_INIT_FAILED};
    }

    // Apply log items to the index
    LogError res = LogObject::exists_sharded(
        build_log_name(index_name), VECTOR_INDEX_LOG_SHARD_COUNT, txm);
    if (res == LogError::SUCCESS)
    {
        auto apply_result =
            ApplyLogItems(index_name, index_sptr, UINT64_MAX, txm);
        if (apply_result != VectorOpResult::SUCCEED)
        {
            return {IndexCache{}, apply_result};
        }
    }
    else if (res == LogError::STORAGE_ERROR)
    {
        return {IndexCache{}, VectorOpResult::INDEX_META_OP_FAILED};
    }

    return {IndexCache{index_sptr, metadata_sptr}, VectorOpResult::SUCCEED};
}

VectorOpResult VectorHandler::PersistIndex(const std::string &name, bool force)
{
    // 1. Create a new transaction for this persistence operation
    TransactionExecution *txm =
        NewTxInit(tx_service_, IsolationLevel::RepeatableRead, CcProtocol::OCC);

    // 2. Check if the index exists and get its metadata
    std::string h_key = build_metadata_key(name);
    TxKey tx_key = EloqStringKey::Create(h_key.c_str(), h_key.size());
    TxRecord::Uptr h_record = EloqStringRecord::Create();
    ReadTxRequest read_req(
        &vector_index_meta_table, 0, &tx_key, h_record.get(), true);
    txm->Execute(&read_req);
    read_req.Wait();
    if (read_req.IsError())
    {
        AbortTx(txm);
        std::lock_guard<std::shared_mutex> lock(vec_indexes_mutex_);
        pending_persist_indexes_.erase(name);
        return VectorOpResult::INDEX_META_OP_FAILED;
    }

    if (read_req.Result().first != RecordStatus::Normal)
    {
        AbortTx(txm);
        std::lock_guard<std::shared_mutex> lock(vec_indexes_mutex_);
        pending_persist_indexes_.erase(name);
        return VectorOpResult::INDEX_NOT_EXIST;
    }

    uint64_t index_version = read_req.Result().second;
    // 2. Deserialize metadata
    VectorIndexMetadata metadata;
    const char *blob_data = h_record->EncodedBlobData();
    size_t blob_size = h_record->EncodedBlobSize();
    size_t offset = 0;
    metadata.Deserialize(blob_data, blob_size, offset, index_version);
    // 3. Get the current index instance from cache
    std::shared_ptr<VectorIndex> index_sptr = nullptr;
    {
        std::shared_lock<std::shared_mutex> read_lock(vec_indexes_mutex_);
        auto it = vec_indexes_.find(name);
        if (it != vec_indexes_.end())
        {
            index_sptr = it->second.index_;
        }
    }
    if (!index_sptr)
    {
        AbortTx(txm);
        std::lock_guard<std::shared_mutex> lock(vec_indexes_mutex_);
        pending_persist_indexes_.erase(name);
        return VectorOpResult::INDEX_NOT_EXIST;
    }

    // 4. Truncate all sharded logs up to the current tail, this will also
    // acquire write intent on log object which blocks other writes to the
    // log/vector index.
    std::string log_name = build_log_name(name);
    uint64_t log_count_after_truncate;
    LogError truncate_result = LogObject::truncate_all_sharded_logs(
        log_name, VECTOR_INDEX_LOG_SHARD_COUNT, log_count_after_truncate, txm);
    if (truncate_result != LogError::SUCCESS)
    {
        AbortTx(txm);
        std::lock_guard<std::shared_mutex> lock(vec_indexes_mutex_);
        pending_persist_indexes_.erase(name);
        return VectorOpResult::INDEX_META_OP_FAILED;
    }
    // 5. Generate new file path with timestamp
    std::string old_file_path = metadata.FilePath();
    std::string new_file_path = old_file_path;

    // Replace the timestamp in the filename with current timestamp
    size_t last_dash = new_file_path.find_last_of('-');
    size_t last_dot = new_file_path.find_last_of('.');
    if (last_dash != std::string::npos && last_dot != std::string::npos &&
        last_dash < last_dot)
    {
        uint64_t current_ts = LocalCcShards::ClockTs();
        new_file_path = new_file_path.substr(0, last_dash + 1) +
                        std::to_string(current_ts) +
                        new_file_path.substr(last_dot);
    }
    else
    {
        // Fallback: append timestamp
        uint64_t current_ts = LocalCcShards::ClockTs();
        new_file_path += "-" + std::to_string(current_ts);
    }
    // 6. Save the index to the new file
    if (!index_sptr->save(new_file_path))
    {
        AbortTx(txm);
        std::lock_guard<std::shared_mutex> lock(vec_indexes_mutex_);
        pending_persist_indexes_.erase(name);
        return VectorOpResult::INDEX_META_OP_FAILED;
    }
    // 8. Update metadata with new file path and persistence info
    metadata.SetFilePath(new_file_path);
    metadata.SetLastPersistTs(LocalCcShards::ClockTs());
    // 9. Serialize updated metadata
    std::string serialized_metadata;
    metadata.Serialize(serialized_metadata);
    h_record->SetEncodedBlob(
        reinterpret_cast<const unsigned char *>(serialized_metadata.data()),
        serialized_metadata.size());
    // 10. Update metadata in storage
    UpsertTxRequest upsert_req(&vector_index_meta_table,
                               std::move(tx_key),
                               std::move(h_record),
                               OperationType::Update);
    txm->Execute(&upsert_req);
    upsert_req.Wait();
    if (upsert_req.IsError())
    {
        AbortTx(txm);
        std::lock_guard<std::shared_mutex> lock(vec_indexes_mutex_);
        pending_persist_indexes_.erase(name);
        return VectorOpResult::INDEX_META_OP_FAILED;
    }

    // 11. Upload the new file to the cloud service
    if (cloud_manager_ &&
        !cloud_manager_->UploadFile(
            new_file_path,
            new_file_path.substr(vector_index_data_path_.size())))
    {
        AbortTx(txm);
        std::lock_guard<std::shared_mutex> lock(vec_indexes_mutex_);
        pending_persist_indexes_.erase(name);
        return VectorOpResult::INDEX_META_OP_FAILED;
    }

    // 12. Commit the transaction
    auto commit_result = CommitTx(txm);
    if (!commit_result.first)
    {
        std::lock_guard<std::shared_mutex> lock(vec_indexes_mutex_);
        pending_persist_indexes_.erase(name);
        return VectorOpResult::INDEX_META_OP_FAILED;
    }
    // 13. Remove old file (after successful commit)
    if (!old_file_path.empty() && old_file_path != new_file_path &&
        std::filesystem::exists(old_file_path))
    {
        try
        {
            std::filesystem::remove(old_file_path);
            // Delete the file from the cloud service
            if (cloud_manager_)
            {
                cloud_manager_->DeleteFile(
                    old_file_path.substr(vector_index_data_path_.size()));
            }
        }
        catch (const std::filesystem::filesystem_error &e)
        {
            LOG(WARNING) << "Failed to delete old index file: " << old_file_path
                         << ", error: " << e.what();
            // Don't fail the operation if old file deletion fails
        }
    }
    std::lock_guard<std::shared_mutex> lock(vec_indexes_mutex_);
    pending_persist_indexes_.erase(name);

    return VectorOpResult::SUCCEED;
}

VectorOpResult VectorHandler::ApplyLogItems(
    const std::string &idx_name,
    std::shared_ptr<VectorIndex> index_sptr,
    uint64_t to_id,
    TransactionExecution *txm)
{
    std::string log_name = build_log_name(idx_name);
    std::vector<log_item_t> log_items;
    if (LogObject::scan_sharded_log(
            log_name, VECTOR_INDEX_LOG_SHARD_COUNT, log_items, txm) !=
        LogError::SUCCESS)
    {
        LOG(ERROR) << "Failed to scan log items for index: " << idx_name;
        return VectorOpResult::INDEX_LOG_OP_FAILED;
    }
    // Apply log items to the index
    std::vector<float> update_vec;
    std::vector<std::vector<float>> insert_vecs;
    std::vector<VectorId> insert_ids;
    insert_vecs.reserve(log_items.size());
    insert_ids.reserve(log_items.size());

    auto apply_batch_add = [&]() -> bool
    {
        if (insert_ids.size() > 0)
        {
            assert(insert_vecs.size() == insert_ids.size());
            auto add_result = index_sptr->add_batch(insert_vecs, insert_ids);
            if (add_result.error != VectorOpResult::SUCCEED)
            {
                LOG(ERROR) << "ApplyLogItems: Failed to insert vector with "
                              "batch size: "
                           << insert_vecs.size();
                return false;
            }
            insert_vecs.clear();
            insert_ids.clear();
        }
        return true;
    };

    for (const auto &item : log_items)
    {
        switch (item.operation_type)
        {
        case LogOperationType::INSERT:
        {
            insert_vecs.emplace_back();
            deserialize_vector(item.value, insert_vecs.back());
            VectorId &vector_id = insert_ids.emplace_back();
            size_t offset = 0;
            vector_id.Deserialize(item.key.data(), offset);
            assert(offset == item.key.size());
            break;
        }
        case LogOperationType::UPDATE:
        {
            // Apply batch add first
            if (!apply_batch_add())
            {
                return VectorOpResult::INDEX_LOG_OP_FAILED;
            }

            update_vec.clear();
            deserialize_vector(item.value, update_vec);
            VectorId vector_id;
            size_t offset = 0;
            vector_id.Deserialize(item.key.data(), offset);
            assert(offset == item.key.size());
            auto update_result = index_sptr->update(update_vec, vector_id);
            if (update_result.error != VectorOpResult::SUCCEED)
            {
                LOG(ERROR) << "ApplyLogItems: Failed to update vector: "
                           << item.key;
                return VectorOpResult::INDEX_LOG_OP_FAILED;
            }
            break;
        }
        case LogOperationType::DELETE:
        {
            // Apply batch add first
            if (!apply_batch_add())
            {
                return VectorOpResult::INDEX_LOG_OP_FAILED;
            }

            VectorId vector_id;
            size_t offset = 0;
            vector_id.Deserialize(item.key.data(), offset);
            assert(offset == item.key.size());
            auto delete_result = index_sptr->remove(vector_id);
            if (delete_result.error != VectorOpResult::SUCCEED)
            {
                LOG(ERROR) << "ApplyLogItems: Failed to delete vector: "
                           << item.key;
                return VectorOpResult::INDEX_LOG_OP_FAILED;
            }
            break;
        }
        default:
        {
            LOG(ERROR) << "ApplyLogItems: Invalid operation type: "
                       << static_cast<uint32_t>(item.operation_type);
            return VectorOpResult::INDEX_LOG_OP_FAILED;
        }
        }
    }

    // Apply the remaining batch add for insert
    if (!apply_batch_add())
    {
        return VectorOpResult::INDEX_LOG_OP_FAILED;
    }

    return VectorOpResult::SUCCEED;
}
}  // namespace EloqVec
