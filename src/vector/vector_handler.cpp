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
#include <utility>

#include "log_object.h"
#include "tx_request.h"
#include "tx_util.h"

namespace EloqVec
{
// Using declarations instead of using-directive
using txservice::AbortTx;
using txservice::CommitTx;
using txservice::EloqStringKey;
using txservice::EloqStringRecord;
using txservice::LocalCcShards;
using txservice::OperationType;
using txservice::ReadTxRequest;
using txservice::RecordStatus;
using txservice::TransactionExecution;
using txservice::TxErrorCode;
using txservice::TxKey;
using txservice::TxRecord;

inline std::string build_metadata_key(const std::string &name)
{
    std::string key_pattern("vector_index:");
    key_pattern.append(name).append(":metadata");
    return key_pattern;
}

VectorMetadata::VectorMetadata(const IndexConfig &vec_spec)
    : name_(vec_spec.name),
      dimension_(vec_spec.dimension),
      algorithm_(vec_spec.algorithm),
      metric_(vec_spec.distance_metric),
      alg_params_(vec_spec.params),
      file_path_(vec_spec.storage_path)
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
     * bufferThreshold | size | createdTs
     * 1. nameLen is a 2-byte integer representing the length of the name. 2.
     * name is a string. 3. dimension is a 8-byte integer representing the
     * dimension. 4. algorithm is a 1-byte integer representing the
     * algorithm. 5. metric is a 1-byte integer representing the metric. 6.
     * paramCount is a 4-byte integer representing the number of algorithm
     * parameters. 7. For each parameter: keyLen (4-byte) | key (string) |
     * valueLen (4-byte) | value (string). 8. filePathLen is a 4-byte integer
     * representing the length of the file path. 9. filePath is a string. 10.
     * bufferThreshold is a 8-byte integer representing the buffer
     * threshold. 11. size is a 8-byte integer representing the size of the
     * index. 12. createdTs is a 8-byte integer representing the creation
     * timestamp.
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

    len_sizeof = sizeof(size_t);
    val_ptr = reinterpret_cast<const char *>(&buffer_threshold_);
    encoded_str.append(val_ptr, len_sizeof);

    len_sizeof = sizeof(size_t);
    val_ptr = reinterpret_cast<const char *>(&size_);
    encoded_str.append(val_ptr, len_sizeof);

    len_sizeof = sizeof(uint64_t);
    val_ptr = reinterpret_cast<const char *>(&created_ts_);
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

    buffer_threshold_ = *reinterpret_cast<const size_t *>(buf + offset);
    offset += sizeof(size_t);

    size_ = *reinterpret_cast<const size_t *>(buf + offset);
    offset += sizeof(size_t);

    created_ts_ = *reinterpret_cast<const uint64_t *>(buf + offset);
    created_ts_ = created_ts_ == 0 ? version : created_ts_;
    offset += sizeof(uint64_t);

    last_persist_ts_ = version;
    assert(offset == buff_size);
}

VectorHandler &VectorHandler::Instance()
{
    static VectorHandler instance;
    return instance;
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
 *
 * @return VectorOpResult indicating the outcome:
 *   - SUCCEED: metadata and log were created successfully.
 *   - INDEX_EXISTED: an index with the given name already exists.
 *   - INDEX_META_OP_FAILED: a metadata table or log operation failed.
 */
VectorOpResult VectorHandler::Create(const IndexConfig &idx_spec,
                                     txservice::TransactionExecution *txm)
{
    // For the internal table, there is no need to acquire read lock on catalog.
    std::string h_key = build_metadata_key(idx_spec.name);
    TxKey tx_key = EloqStringKey::Create(h_key.c_str(), h_key.size());
    TxRecord::Uptr h_record = EloqStringRecord::Create();
    ReadTxRequest read_req(&vector_index_meta_table,
                           0,
                           &tx_key,
                           h_record.get(),
                           true,  /* is_for_write */
                           false, /* is_for_share */
                           false, /* read_local */
                           0,
                           false,
                           false,
                           false,
                           nullptr,
                           nullptr,
                           nullptr);
    txm->Execute(&read_req);
    read_req.Wait();
    if (read_req.IsError())
    {
        return VectorOpResult::INDEX_META_OP_FAILED;
    }

    if (read_req.Result().first == RecordStatus::Normal)
    {
        // The index already exists
        return VectorOpResult::INDEX_EXISTED;
    }

    // The index does not exist, create it
    // 1. encode the index metadata
    VectorMetadata vec_meta(idx_spec);
    std::string encoded_str;
    vec_meta.Encode(encoded_str);
    h_record->SetEncodedBlob(
        reinterpret_cast<const unsigned char *>(encoded_str.data()),
        encoded_str.size());
    // 2. upsert the metadata to the internal table
    // There is no need to check the schema ts of the internal table.
    txservice::UpsertTxRequest upsert_req(&vector_index_meta_table,
                                          std::move(tx_key),
                                          std::move(h_record),
                                          OperationType::Insert,
                                          nullptr,
                                          nullptr);
    txm->Execute(&upsert_req);
    upsert_req.Wait();
    if (upsert_req.IsError())
    {
        return VectorOpResult::INDEX_META_OP_FAILED;
    }

    // 3. create the log object
    LogError log_err =
        LogObject::create_log("vector_index:" + idx_spec.name, txm);
    if (log_err != LogError::SUCCESS)
    {
        return VectorOpResult::INDEX_META_OP_FAILED;
    }

    // TODO(ysw): create the vector index object in memory.

    return VectorOpResult::SUCCEED;
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
VectorOpResult VectorHandler::Drop(const std::string &name,
                                   txservice::TransactionExecution *txm)
{
    // 1. Check if the index exists by reading from vector_index_meta_table
    std::string h_key = build_metadata_key(name);
    TxKey tx_key = EloqStringKey::Create(h_key.c_str(), h_key.size());
    TxRecord::Uptr h_record = EloqStringRecord::Create();
    ReadTxRequest read_req(&vector_index_meta_table,
                           0,
                           &tx_key,
                           h_record.get(),
                           true,  /* is_for_write */
                           false, /* is_for_share */
                           false, /* read_local */
                           0,
                           false,
                           false,
                           false,
                           nullptr,
                           nullptr,
                           nullptr);
    txm->Execute(&read_req);
    read_req.Wait();
    if (read_req.IsError())
    {
        return VectorOpResult::INDEX_META_OP_FAILED;
    }

    // 2. Check if the index exists
    if (read_req.Result().first != RecordStatus::Normal)
    {
        assert(read_req.Result().first == RecordStatus::Deleted);
        // The index does not exist
        return VectorOpResult::INDEX_NOT_EXIST;
    }

    // 3. The index exists, delete it using OperationType::Delete
    TxErrorCode err_code = txm->TxUpsert(vector_index_meta_table,
                                         0,
                                         std::move(tx_key),
                                         nullptr,
                                         OperationType::Delete);
    if (err_code != TxErrorCode::NO_ERROR)
    {
        return VectorOpResult::INDEX_META_OP_FAILED;
    }

    // 4. delete the log object
    LogError log_err = LogObject::remove_log("vector_index:" + name, txm);
    if (log_err != LogError::SUCCESS)
    {
        return VectorOpResult::INDEX_META_OP_FAILED;
    }

    // TODO(ysw): delete the index file and the vector index object in memory.
    return VectorOpResult::SUCCEED;
}

VectorOpResult VectorHandler::Info(const std::string &name,
                                   txservice::TransactionExecution *txm,
                                   VectorMetadata &metadata)
{
    // 1. Check if the index exists by reading from vector_index_meta_table
    std::string h_key = build_metadata_key(name);
    TxKey tx_key = EloqStringKey::Create(h_key.c_str(), h_key.size());
    TxRecord::Uptr h_record = EloqStringRecord::Create();
    ReadTxRequest read_req(&vector_index_meta_table,
                           0,
                           &tx_key,
                           h_record.get(),
                           false, /* is_for_write */
                           false, /* is_for_share */
                           false, /* read_local */
                           0,
                           false,
                           false,
                           false,
                           nullptr,
                           nullptr,
                           nullptr);
    txm->Execute(&read_req);
    read_req.Wait();
    if (read_req.IsError())
    {
        return VectorOpResult::INDEX_META_OP_FAILED;
    }

    // 2. Check if the index exists
    if (read_req.Result().first != RecordStatus::Normal)
    {
        assert(read_req.Result().first == RecordStatus::Deleted);
        // The index does not exist
        return VectorOpResult::INDEX_NOT_EXIST;
    }

    uint64_t index_version = read_req.Result().second;

    // 3. The index exists, decode the metadata
    const char *blob_data = h_record->EncodedBlobData();
    size_t blob_size = h_record->EncodedBlobSize();
    size_t offset = 0;
    metadata.Decode(blob_data, blob_size, offset, index_version);

    return VectorOpResult::SUCCEED;
}

VectorOpResult VectorHandler::Search(
    const std::string &name,
    const std::vector<float> &query_vector,
    size_t k_count,
    const std::unordered_map<std::string, std::string> &search_params,
    txservice::TransactionExecution *txm,
    SearchResult &vector_result)
{
    // 1. Check if the index exists by reading from vector_index_meta_table
    std::string h_key = build_metadata_key(name);
    TxKey tx_key = EloqStringKey::Create(h_key.c_str(), h_key.size());
    TxRecord::Uptr h_record = EloqStringRecord::Create();
    ReadTxRequest read_req(&vector_index_meta_table,
                           0,
                           &tx_key,
                           h_record.get(),
                           false, /* is_for_write */
                           false, /* is_for_share */
                           false, /* read_local */
                           0,
                           false,
                           false,
                           false,
                           nullptr,
                           nullptr,
                           nullptr);
    txm->Execute(&read_req);
    read_req.Wait();
    if (read_req.IsError())
    {
        return VectorOpResult::INDEX_META_OP_FAILED;
    }

    // 2. Check if the index exists
    if (read_req.Result().first != RecordStatus::Normal)
    {
        assert(read_req.Result().first == RecordStatus::Deleted);
        // The index does not exist
        return VectorOpResult::INDEX_NOT_EXIST;
    }

    uint64_t index_version = read_req.Result().second;

    // 3. The index exists, decode the metadata
    VectorMetadata metadata;
    const char *blob_data = h_record->EncodedBlobData();
    size_t blob_size = h_record->EncodedBlobSize();
    size_t offset = 0;
    metadata.Decode(blob_data, blob_size, offset, index_version);

    // 4. Validate query vector dimensions match index configuration
    // if (query_vector.size() != metadata.Dimension())
    // {
    //     return VectorOpResult::UNKNOWN;  // TODO: Add specific error code for
    //                                      // dimension mismatch
    // }

    // 5. TODO: Perform actual vector search using the appropriate algorithm
    // This would involve:
    // - Loading the vector index from storage (metadata.FilePath())
    // - Using the appropriate algorithm (metadata.Algorithm())
    // - Applying the distance metric (metadata.Metric())
    // - Executing the search with k_count and search_params
    // - Populating vector_result with IDs and distances

    // For now, return empty results as placeholder
    vector_result.ids.clear();
    vector_result.distances.clear();
    vector_result.vectors.clear();

    return VectorOpResult::SUCCEED;
}

VectorOpResult VectorHandler::Add(const std::string &name,
                                  uint64_t id,
                                  const std::vector<float> &vector,
                                  txservice::TransactionExecution *txm)
{
    // TODO: Implement vector addition to index
    // This should:
    // 1. Validate the index exists and is writable
    // 2. Extract vector data from the record
    // 3. Validate vector dimensions match index configuration
    // 4. Add the vector to the index with the provided key
    // 5. Update index statistics and metadata
    return VectorOpResult::SUCCEED;
}

VectorOpResult VectorHandler::Update(const std::string &name,
                                     uint64_t id,
                                     const std::vector<float> &vector,
                                     txservice::TransactionExecution *txm)
{
    // TODO: Implement vector update to index
    // This should:
    // 1. Validate the index exists and is writable
    // 2. Extract vector data from the record
    // 3. Validate vector dimensions match index configuration
    // 4. Update the vector in the index with the provided key
    // 5. Update index statistics and metadata
    return VectorOpResult::SUCCEED;
}

VectorOpResult VectorHandler::Delete(const std::string &name,
                                     uint64_t id,
                                     txservice::TransactionExecution *txm)
{
    // TODO: Implement vector deletion from index
    // This should:
    // 1. Validate the index exists and is writable
    // 2. Extract vector data from the record
    // 3. Validate vector dimensions match index configuration
    // 4. Delete the vector from the index with the provided key
    // 5. Update index statistics and metadata
    return VectorOpResult::SUCCEED;
}
}  // namespace EloqVec
