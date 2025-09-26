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
#include "tx_request.h"
#include "tx_util.h"
#include "tx_worker_pool.h"

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

VectorMetadata::VectorMetadata(const IndexConfig &vec_spec)
    : name_(vec_spec.name),
      dimension_(vec_spec.dimension),
      algorithm_(vec_spec.algorithm),
      metric_(vec_spec.distance_metric),
      alg_params_(vec_spec.params),
      file_path_(vec_spec.storage_path),
      persist_threshold_(vec_spec.persist_threshold)
{
    uint64_t ts = LocalCcShards::ClockTs();
    file_path_.append("/")
        .append(name_)
        .append("-")
        .append(std::to_string(ts))
        .append(".index");
    created_ts_ = 0;
    last_persist_ts_ = 0;
}

void VectorMetadata::Encode(std::string &encoded_str) const
{
    /**
     * The format of the encoded metadata:
     * nameLen | name | dimension | algorithm | metric | paramCount | key1Len |
     * key1 | value1Len | value1 | ... | filePathLen | filePath |
     * persistThreshold | createdTs | lastPersistTs
     * 1. nameLen is a 2-byte integer representing the length of the name. 2.
     * name is a string. 3. dimension is a 8-byte integer representing the
     * dimension. 4. algorithm is a 1-byte integer representing the
     * algorithm. 5. metric is a 1-byte integer representing the metric. 6.
     * paramCount is a 4-byte integer representing the number of algorithm
     * parameters. 7. For each parameter: keyLen (4-byte) | key (string) |
     * valueLen (4-byte) | value (string). 8. filePathLen is a 4-byte integer
     * representing the length of the file path. 9. filePath is a string. 10.
     * persistThreshold is a 8-byte integer representing the persist
     * threshold. 11. createdTs is a 8-byte integer representing the creation
     * timestamp. 12. lastPersistTs is a 8-byte integer representing the last
     * persist timestamp.
     */
    uint16_t name_len = static_cast<uint16_t>(name_.size());
    size_t len_sizeof = sizeof(uint16_t);
    const char *val_ptr = reinterpret_cast<const char *>(&name_len);
    encoded_str.append(val_ptr, len_sizeof);
    encoded_str.append(name_.data(), name_len);

    len_sizeof = sizeof(size_t);
    val_ptr = reinterpret_cast<const char *>(&dimension_);
    encoded_str.append(val_ptr, len_sizeof);

    len_sizeof = sizeof(uint8_t);
    val_ptr = reinterpret_cast<const char *>(&algorithm_);
    encoded_str.append(val_ptr, len_sizeof);

    len_sizeof = sizeof(uint8_t);
    val_ptr = reinterpret_cast<const char *>(&metric_);
    encoded_str.append(val_ptr, len_sizeof);

    // Serialize algorithm parameters with the following format:
    // param_count | key1_len | key1 | value1_len | value1 | key2_len | key2 |
    // value2_len | value2 | ...
    uint32_t param_count = static_cast<uint32_t>(alg_params_.size());
    len_sizeof = sizeof(uint32_t);
    val_ptr = reinterpret_cast<const char *>(&param_count);
    encoded_str.append(val_ptr, len_sizeof);

    // Serialize each key-value pair
    for (const auto &param : alg_params_)
    {
        // Write key length and key
        uint32_t key_len = static_cast<uint32_t>(param.first.size());
        val_ptr = reinterpret_cast<const char *>(&key_len);
        encoded_str.append(val_ptr, len_sizeof);
        encoded_str.append(param.first.data(), key_len);

        // Write value length and value
        uint32_t value_len = static_cast<uint32_t>(param.second.size());
        val_ptr = reinterpret_cast<const char *>(&value_len);
        encoded_str.append(val_ptr, len_sizeof);
        encoded_str.append(param.second.data(), value_len);
    }

    len_sizeof = sizeof(uint32_t);
    uint32_t file_path_len = static_cast<uint32_t>(file_path_.size());
    val_ptr = reinterpret_cast<const char *>(&file_path_len);
    encoded_str.append(val_ptr, len_sizeof);
    encoded_str.append(file_path_.data(), file_path_len);

    len_sizeof = sizeof(int64_t);
    val_ptr = reinterpret_cast<const char *>(&persist_threshold_);
    encoded_str.append(val_ptr, len_sizeof);

    len_sizeof = sizeof(uint64_t);
    val_ptr = reinterpret_cast<const char *>(&created_ts_);
    encoded_str.append(val_ptr, len_sizeof);

    // persist ts
    len_sizeof = sizeof(uint64_t);
    val_ptr = reinterpret_cast<const char *>(&last_persist_ts_);
    encoded_str.append(val_ptr, len_sizeof);
}

void VectorMetadata::Decode(const char *buf,
                            size_t buff_size,
                            size_t &offset,
                            uint64_t version)
{
    uint16_t name_len = *reinterpret_cast<const uint16_t *>(buf + offset);
    offset += sizeof(uint16_t);
    name_.clear();
    name_.reserve(name_len);
    std::copy(buf + offset, buf + offset + name_len, std::back_inserter(name_));
    offset += name_len;

    dimension_ = *reinterpret_cast<const size_t *>(buf + offset);
    offset += sizeof(size_t);

    algorithm_ = static_cast<Algorithm>(
        *reinterpret_cast<const uint8_t *>(buf + offset));
    offset += sizeof(uint8_t);

    metric_ = static_cast<DistanceMetric>(
        *reinterpret_cast<const uint8_t *>(buf + offset));
    offset += sizeof(uint8_t);

    uint32_t param_count = *reinterpret_cast<const uint32_t *>(buf + offset);
    offset += sizeof(uint32_t);
    // Clear existing parameters
    alg_params_.clear();
    // Deserialize each key-value pair
    for (uint32_t i = 0; i < param_count; ++i)
    {
        // Read key length and key
        uint32_t key_len = *reinterpret_cast<const uint32_t *>(buf + offset);
        offset += sizeof(uint32_t);
        std::string key(buf + offset, key_len);
        offset += key_len;

        // Read value length and value
        uint32_t value_len = *reinterpret_cast<const uint32_t *>(buf + offset);
        offset += sizeof(uint32_t);
        std::string value(buf + offset, value_len);
        offset += value_len;

        // Store the key-value pair
        alg_params_.emplace(std::move(key), std::move(value));
    }

    uint32_t file_path_len = *reinterpret_cast<const uint32_t *>(buf + offset);
    offset += sizeof(uint32_t);
    file_path_.clear();
    file_path_.reserve(file_path_len);
    std::copy(buf + offset,
              buf + offset + file_path_len,
              std::back_inserter(file_path_));
    offset += file_path_len;

    persist_threshold_ = *reinterpret_cast<const int64_t *>(buf + offset);
    offset += sizeof(int64_t);

    created_ts_ = *reinterpret_cast<const uint64_t *>(buf + offset);
    created_ts_ = created_ts_ == 0 ? version : created_ts_;
    offset += sizeof(uint64_t);

    // Deserialize persistence tracking fields
    last_persist_ts_ = *reinterpret_cast<const uint64_t *>(buf + offset);
    offset += sizeof(uint64_t);
}

static std::unique_ptr<VectorHandler> vector_handler_instance = nullptr;
static std::once_flag vector_handler_once;

void VectorHandler::InitHandlerInstance(
    TxService *tx_service,
    txservice::TxWorkerPool *vector_index_worker_pool,
    std::string &vector_index_data_path)
{
    std::call_once(
        vector_handler_once,
        [tx_service, vector_index_worker_pool, &vector_index_data_path]()
        {
            vector_handler_instance = std::unique_ptr<VectorHandler>(
                new VectorHandler(tx_service,
                                  vector_index_worker_pool,
                                  vector_index_data_path));
        });
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
VectorOpResult VectorHandler::Create(const IndexConfig &idx_spec)
{
    TransactionExecution *txm =
        NewTxInit(tx_service_, IsolationLevel::RepeatableRead, CcProtocol::OCC);
    // For the internal table, there is no need to acquire read lock on catalog.
    std::string h_key = build_metadata_key(idx_spec.name);
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
    bool is_valid = false;
    switch (idx_spec.algorithm)
    {
    case Algorithm::HNSW:
    {
        is_valid = HNSWVectorIndex::validate_config(idx_spec);
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

    // 2. encode the index metadata
    VectorMetadata vec_meta(idx_spec);
    std::string encoded_str;
    vec_meta.Encode(encoded_str);
    h_record->SetEncodedBlob(
        reinterpret_cast<const unsigned char *>(encoded_str.data()),
        encoded_str.size());
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
        build_log_name(idx_spec.name), VECTOR_INDEX_LOG_SHARD_COUNT, txm);
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

    // 3. Decode metadata to get file path before deletion
    uint64_t index_version = read_req.Result().second;
    VectorMetadata metadata;
    const char *blob_data = h_record->EncodedBlobData();
    size_t blob_size = h_record->EncodedBlobSize();
    size_t offset = 0;
    metadata.Decode(blob_data, blob_size, offset, index_version);
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
                                   VectorMetadata &metadata)
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

    // 3. The index exists, decode the metadata
    const char *blob_data = h_record->EncodedBlobData();
    size_t blob_size = h_record->EncodedBlobSize();
    size_t offset = 0;
    metadata.Decode(blob_data, blob_size, offset, index_version);

    CommitTx(txm);
    return VectorOpResult::SUCCEED;
}

VectorOpResult VectorHandler::Search(const std::string &name,
                                     const std::vector<float> &query_vector,
                                     size_t k_count,
                                     size_t thread_id,
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
    auto [index_sptr, get_result] = GetOrCreateIndex(name, h_record, txm);
    if (get_result != VectorOpResult::SUCCEED)
    {
        AbortTx(txm);
        return get_result;
    }

    // 4. Perform vector search
    auto search_result =
        index_sptr->search(query_vector, k_count, thread_id, vector_result);
    CommitTx(txm);
    return search_result.error;
}

VectorOpResult VectorHandler::Add(const std::string &name,
                                  uint64_t id,
                                  const std::vector<float> &vector)
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
    auto [index_sptr, get_result] = GetOrCreateIndex(name, h_record, txm);
    if (get_result != VectorOpResult::SUCCEED)
    {
        AbortTx(txm);
        return get_result;
    }
    // Add item to log object.
    std::string serialized_vector;
    serialize_vector(vector, serialized_vector);
    uint64_t log_id = 0;
    uint64_t log_count = 0;
    uint64_t ts = LocalCcShards::ClockTs();
    std::vector<log_item_t> log_items;
    log_items.emplace_back(
        LogOperationType::INSERT, std::to_string(id), serialized_vector, ts, 0);
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
    // 4. Add the vector to the index
    auto add_result = index_sptr->add(vector, id);
    if (add_result.error == VectorOpResult::SUCCEED)
    {
        auto res_pair = CommitTx(txm);
        if (!res_pair.first)
        {
            // Remove the vector from the index cache.
            index_sptr->remove(id);
            return VectorOpResult::INDEX_ADD_FAILED;
        }

        if (index_sptr->get_persist_threshold() == -1)
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
                static_cast<uint64_t>(index_sptr->get_persist_threshold()))
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
                                     const std::vector<float> &vector)
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
    auto [index_sptr, get_result] = GetOrCreateIndex(name, h_record, txm);
    if (get_result != VectorOpResult::SUCCEED)
    {
        AbortTx(txm);
        return get_result;
    }

    // Add item to log object.
    std::string serialized_vector;
    serialize_vector(vector, serialized_vector);
    uint64_t log_id = 0;
    uint64_t log_count = 0;
    uint64_t ts = LocalCcShards::ClockTs();
    std::vector<log_item_t> log_items;
    log_items.emplace_back(
        LogOperationType::UPDATE, std::to_string(id), serialized_vector, ts, 0);
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

    // 4. Update the vector in the index
    std::vector<float> update_vec;
    auto idx_res = index_sptr->get(id, update_vec);
    if (idx_res.error != VectorOpResult::SUCCEED || update_vec.size() == 0)
    {
        // The index operation failed or the vector with this id does not exist.
        AbortTx(txm);
        return idx_res.error;
    }

    idx_res = index_sptr->update(vector, id);
    if (idx_res.error == VectorOpResult::SUCCEED)
    {
        auto res_pair = CommitTx(txm);
        if (!res_pair.first)
        {
            // Restore the vector to the index cache.
            index_sptr->update(update_vec, id);
            return VectorOpResult::INDEX_UPDATE_FAILED;
        }

        if (index_sptr->get_persist_threshold() == -1)
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
                static_cast<uint64_t>(index_sptr->get_persist_threshold()))
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
    auto [index_sptr, get_result] = GetOrCreateIndex(name, h_record, txm);
    if (get_result != VectorOpResult::SUCCEED)
    {
        AbortTx(txm);
        return get_result;
    }

    // Add item to log object.
    std::string serialized_empty_vec;
    uint64_t log_id = 0;
    uint64_t log_count = 0;
    uint64_t ts = LocalCcShards::ClockTs();
    std::vector<log_item_t> log_items;
    log_items.emplace_back(LogOperationType::DELETE,
                           std::to_string(id),
                           serialized_empty_vec,
                           ts,
                           0);
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
    auto idx_res = index_sptr->get(id, deleted_vector);
    if (idx_res.error != VectorOpResult::SUCCEED || deleted_vector.size() == 0)
    {
        // The index operation failed or the vector with this id does not exist.
        AbortTx(txm);
        return idx_res.error;
    }

    idx_res = index_sptr->remove(id);
    if (idx_res.error == VectorOpResult::SUCCEED)
    {
        auto res_pair = CommitTx(txm);
        if (!res_pair.first)
        {
            // Restore the vector to the index cache.
            index_sptr->add(deleted_vector, id);
            return VectorOpResult::INDEX_DELETE_FAILED;
        }

        if (index_sptr->get_persist_threshold() == -1)
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
                static_cast<uint64_t>(index_sptr->get_persist_threshold()))
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
    const std::vector<std::vector<float>> &vectors)
{
    // Check if the ids and vectors are valid
    if (ids.empty() || ids.size() != vectors.size())
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
    auto [index_sptr, get_result] = GetOrCreateIndex(name, h_record, txm);
    if (get_result != VectorOpResult::SUCCEED)
    {
        AbortTx(txm);
        return get_result;
    }

    // Group ids by shard id
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

    // 4. Add items to log object by shard
    uint64_t ts = LocalCcShards::ClockTs();
    uint64_t log_id = 0;
    uint64_t log_count = 0;
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
            std::string serialized_vector;
            serialize_vector(vectors[idx], serialized_vector);
            // Create log item
            log_items.emplace_back(LogOperationType::INSERT,
                                   std::to_string(ids[idx]),
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

    // 4. Add the vectors to the index
    auto add_result = index_sptr->add_batch(vectors, ids);
    if (add_result.error == VectorOpResult::SUCCEED)
    {
        auto res_pair = CommitTx(txm);
        if (!res_pair.first)
        {
            for (const uint64_t id : ids)
            {
                index_sptr->remove(id);
            }
            return VectorOpResult::INDEX_ADD_FAILED;
        }

        if (index_sptr->get_persist_threshold() == -1)
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
                static_cast<uint64_t>(index_sptr->get_persist_threshold()))
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

std::pair<std::shared_ptr<VectorIndex>, VectorOpResult>
VectorHandler::GetOrCreateIndex(const std::string &name,
                                const TxRecord::Uptr &h_record,
                                TransactionExecution *txm)
{
    std::shared_ptr<VectorIndex> cached_index_sptr = nullptr;
    // First, try to get read lock to check cache
    {
        std::shared_lock<std::shared_mutex> read_lock(vec_indexes_mutex_);
        auto it = vec_indexes_.find(name);
        if (it != vec_indexes_.end())
        {
            cached_index_sptr = it->second;
            return {cached_index_sptr, VectorOpResult::SUCCEED};
        }
    }

    // Need to create new index and initialize it
    {
        std::lock_guard<std::shared_mutex> write_lock(vec_indexes_mutex_);
        // Double-checked locking pattern
        auto it = vec_indexes_.find(name);
        if (it != vec_indexes_.end())
        {
            cached_index_sptr = it->second;
            return {cached_index_sptr, VectorOpResult::SUCCEED};
        }

        // Create and initialize the index
        auto [index_sptr, init_result] =
            CreateAndInitializeIndex(h_record, txm);
        if (init_result != VectorOpResult::SUCCEED)
        {
            return {nullptr, init_result};
        }

        // Insert the index into the cache
        auto res_pair = vec_indexes_.emplace(name, std::move(index_sptr));
        assert(res_pair.second);
        cached_index_sptr = res_pair.first->second;
        return {cached_index_sptr, VectorOpResult::SUCCEED};
    }
}

std::pair<std::shared_ptr<VectorIndex>, VectorOpResult>
VectorHandler::CreateAndInitializeIndex(const TxRecord::Uptr &h_record,
                                        TransactionExecution *txm)
{
    // Decode metadata from h_record
    VectorMetadata metadata;
    const char *blob_data = h_record->EncodedBlobData();
    size_t blob_size = h_record->EncodedBlobSize();
    size_t offset = 0;
    uint64_t unused_version = 0;
    metadata.Decode(blob_data, blob_size, offset, unused_version);

    std::shared_ptr<VectorIndex> index_sptr = nullptr;
    switch (metadata.VecAlgorithm())
    {
    case Algorithm::HNSW:
        index_sptr = std::make_shared<HNSWVectorIndex>();
        break;
    default:
        return {nullptr, VectorOpResult::INDEX_INIT_FAILED};
    }

    // Construct IndexConfig from metadata
    IndexConfig config;
    config.name = metadata.VecName();
    config.dimension = metadata.Dimension();
    config.algorithm = metadata.VecAlgorithm();
    config.distance_metric = metadata.VecMetric();
    config.storage_path = metadata.FilePath();
    config.params = metadata.VecAlgParams();
    config.persist_threshold = metadata.PersistThreshold();

    // Initialize the index
    if (!index_sptr->initialize(config))
    {
        return {nullptr, VectorOpResult::INDEX_INIT_FAILED};
    }

    // Load the index from storage
    if (std::filesystem::exists(config.storage_path) &&
        !index_sptr->load(config.storage_path))
    {
        return {nullptr, VectorOpResult::INDEX_LOAD_FAILED};
    }

    // Apply log items to the index
    if (LogObject::exists(build_log_name(config.name), txm))
    {
        auto apply_result =
            ApplyLogItems(config.name, index_sptr, UINT64_MAX, txm);
        if (apply_result != VectorOpResult::SUCCEED)
        {
            return {nullptr, apply_result};
        }
    }

    return {index_sptr, VectorOpResult::SUCCEED};
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
    // 2. Decode metadata
    VectorMetadata metadata;
    const char *blob_data = h_record->EncodedBlobData();
    size_t blob_size = h_record->EncodedBlobSize();
    size_t offset = 0;
    metadata.Decode(blob_data, blob_size, offset, index_version);
    // 3. Get the current index instance from cache
    std::shared_ptr<VectorIndex> index_sptr = nullptr;
    {
        std::shared_lock<std::shared_mutex> read_lock(vec_indexes_mutex_);
        auto it = vec_indexes_.find(name);
        if (it != vec_indexes_.end())
        {
            index_sptr = it->second;
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
    // 9. Encode updated metadata
    std::string encoded_metadata;
    metadata.Encode(encoded_metadata);
    h_record->SetEncodedBlob(
        reinterpret_cast<const unsigned char *>(encoded_metadata.data()),
        encoded_metadata.size());
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
    // 11. Commit the transaction
    auto commit_result = CommitTx(txm);
    if (!commit_result.first)
    {
        std::lock_guard<std::shared_mutex> lock(vec_indexes_mutex_);
        pending_persist_indexes_.erase(name);
        return VectorOpResult::INDEX_META_OP_FAILED;
    }
    // 12. Remove old file (after successful commit)
    if (!old_file_path.empty() && old_file_path != new_file_path &&
        std::filesystem::exists(old_file_path))
    {
        try
        {
            std::filesystem::remove(old_file_path);
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
    std::vector<uint64_t> insert_ids;
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
            insert_ids.emplace_back(std::stoull(item.key));
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
            uint64_t id = std::stoull(item.key);
            auto update_result = index_sptr->update(update_vec, id);
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

            auto delete_result = index_sptr->remove(std::stoull(item.key));
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
