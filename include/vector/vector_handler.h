/**
 * @file vector_handler.h
 * @brief Vector operations handler for EloqVec
 *
 * This file defines the vector_handler class that provides static methods
 * for vector index operations including create, search, info, add, and drop
 * operations.
 *
 * @author EloqData Inc.
 * @date 2025
 */

#pragma once

#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "eloq_string_key_record.h"
#include "tx_execution.h"
#include "tx_service.h"
#include "vector_index.h"
#include "vector_type.h"

namespace EloqVec
{

/**
 * @brief Metadata about a vector index
 *
 * This class contains the metadata about a vector index.
 */
class VectorMetadata
{
public:
    VectorMetadata() = default;
    explicit VectorMetadata(const IndexConfig &vec_spec);
    ~VectorMetadata() = default;

    void Encode(std::string &encoded_str) const;
    void Decode(const char *buf,
                size_t buff_size,
                size_t &offset,
                uint64_t version);

    const std::string &VecName() const
    {
        return name_;
    }

    Algorithm VecAlgorithm() const
    {
        return algorithm_;
    }

    DistanceMetric VecMetric() const
    {
        return metric_;
    }

    const std::unordered_map<std::string, std::string> &VecAlgParams() const
    {
        return alg_params_;
    }

    uint64_t CreatedTs() const
    {
        return created_ts_;
    }

    uint64_t LastPersistTs() const
    {
        return last_persist_ts_;
    }

    size_t Dimension() const
    {
        return dimension_;
    }

    const std::string &FilePath() const
    {
        return file_path_;
    }

    void SetFilePath(const std::string &path)
    {
        file_path_ = path;
    }

    void SetLastPersistTs(uint64_t ts)
    {
        last_persist_ts_ = ts;
    }

    size_t BufferThreshold() const
    {
        return buffer_threshold_;
    }

private:
    std::string name_{""};
    size_t dimension_{0};
    Algorithm algorithm_{Algorithm::HNSW};
    DistanceMetric metric_{DistanceMetric::L2SQ};
    std::unordered_map<std::string, std::string> alg_params_;
    std::string file_path_{""};
    size_t buffer_threshold_{10000};
    uint64_t created_ts_{0};
    uint64_t last_persist_ts_{0};
};

/**
 * @brief Handler class for vector operations
 *
 * This class provides methods for managing vector indices and
 * performing vector operations within the transaction system.
 * Uses singleton pattern for global access.
 */
class VectorHandler
{
public:
    /**
     * @brief Initialize the handler instance
     *
     * @param tx_service Transaction service
     */
    static void InitHandlerInstance(
        txservice::TxService *tx_service,
        txservice::TxWorkerPool *vector_index_worker_pool,
        std::string &vector_index_data_path);
    /**
     * @brief Get the singleton instance of VectorHandler
     *
     * @return Reference to the singleton VectorHandler instance
     */
    static VectorHandler &Instance();

    ~VectorHandler() = default;

    /**
     * @brief Create a new vector index
     *
     * @param idx_spec Configuration for the vector index to create
     * @return Result of the create operation
     */
    VectorOpResult Create(const IndexConfig &idx_spec);

    /**
     * @brief Drop (delete) a vector index
     *
     * @param name Name of the vector index to drop
     * @return Result of the drop operation
     */
    VectorOpResult Drop(const std::string &name);

    /**
     * @brief Get information about a vector index
     *
     * @param name Name of the vector index to query
     * @param txm Transaction execution context
     * @param metadata (OUT) Metadata of the vector index
     * @return Result of the info operation
     */
    VectorOpResult Info(const std::string &name,
                        txservice::TransactionExecution *txm,
                        VectorMetadata &metadata);

    /**
     * @brief Search for similar vectors in an index
     *
     * @param name Name of the vector index to search
     * @param query_vector Query vector
     * @param k_count Number of nearest neighbors to return
     * @param txm Transaction execution context
     * @param vector_result (OUT) Result of the search vectors
     * @return Result of the search operation
     */
    VectorOpResult Search(const std::string &name,
                          const std::vector<float> &query_vector,
                          size_t k_count,
                          txservice::TransactionExecution *txm,
                          SearchResult &vector_result);

    /**
     * @brief Add a vector entry to an index
     *
     * @param name Name of the vector index
     * @param id Unique identifier for the vector
     * @param vector Vector data
     * @param txm Transaction execution context
     * @return Result of the add operation
     */
    VectorOpResult Add(const std::string &name,
                       uint64_t id,
                       const std::vector<float> &vector,
                       txservice::TransactionExecution *txm);

    /**
     * @brief Update a vector entry in an index
     *
     * @param name Name of the vector index
     * @param id Unique identifier for the vector
     * @param vector Vector data
     * @param txm Transaction execution context
     * @return Result of the update operation
     */
    VectorOpResult Update(const std::string &name,
                          uint64_t id,
                          const std::vector<float> &vector,
                          txservice::TransactionExecution *txm);

    /**
     * @brief Delete a vector entry from an index
     *
     * @param name Name of the vector index
     * @param id Unique identifier for the vector
     * @param txm Transaction execution context
     * @return Result of the delete operation
     */
    VectorOpResult Delete(const std::string &name,
                          uint64_t id,
                          txservice::TransactionExecution *txm);

    /**
     * @brief Persist a vector index to disk and truncate its log
     *
     * This method performs the complete persistence workflow:
     * 1. Creates its own transaction for atomicity
     * 2. Determines the truncation point for the log
     * 3. Truncates the log object
     * 4. Saves the index to a new file
     * 5. Updates metadata with new file path and persisted sequence ID
     * 6. Commits the transaction
     * 7. Removes the old file (post-commit)
     *
     * @param name Name of the vector index to persist
     * @param force Whether to persist the index even if it doesn't have enough
     * items
     * @return VectorOpResult indicating success or failure
     */
    VectorOpResult PersistIndex(const std::string &name, bool force = false);

    const std::string &VectorIndexDataPath() const
    {
        return vector_index_data_path_;
    }

private:
    // Private constructor to prevent instantiation
    explicit VectorHandler(txservice::TxService *tx_service,
                           txservice::TxWorkerPool *vector_index_worker_pool,
                           std::string &vector_index_data_path)
        : tx_service_(tx_service),
          vector_index_worker_pool_(vector_index_worker_pool),
          vector_index_data_path_(vector_index_data_path)
    {
    }

    // Delete copy and move constructors/operators
    VectorHandler(const VectorHandler &) = delete;
    VectorHandler(VectorHandler &&) = delete;
    VectorHandler &operator=(const VectorHandler &) = delete;
    VectorHandler &operator=(VectorHandler &&) = delete;

    /**
     * @brief Get or create a vector index from cache
     *
     * This function handles the logic of getting an index from cache,
     * checking version compatibility, and initializing if needed.
     *
     * @param name Index name
     * @param h_record Record containing encoded metadata
     * @param index_version Current index version from storage
     * @param txm Transaction execution context
     * @return Pair of the vector index and the result of the operation
     */
    std::pair<std::shared_ptr<VectorIndex>, VectorOpResult> GetOrCreateIndex(
        const std::string &name,
        const txservice::TxRecord::Uptr &h_record,
        uint64_t index_version,
        txservice::TransactionExecution *txm);

    /**
     * @brief Create a vector index with given configuration and initialize it
     *
     * @param h_record Record containing encoded metadata
     * @param index_version Current index version from storage
     * @param txm Transaction execution context
     * @return Pair of the vector index and the result of the operation
     */
    std::pair<std::shared_ptr<VectorIndex>, VectorOpResult>
    CreateAndInitializeIndex(const txservice::TxRecord::Uptr &h_record,
                             uint64_t index_version,
                             txservice::TransactionExecution *txm);

    /**
     * @brief Apply log items to the index
     *
     * @param idx_name Index name
     * @param to_id The end sequence id of the log items to apply(inclusive)
     * @param index_sptr Index object
     * @param txm Transaction execution context
     * @return Result of the apply operation
     */
    VectorOpResult ApplyLogItems(const std::string &idx_name,
                                 std::shared_ptr<VectorIndex> index_sptr,
                                 uint64_t to_id,
                                 txservice::TransactionExecution *txm);

    // Transaction service
    txservice::TxService *tx_service_{nullptr};
    // Vector index worker pool
    txservice::TxWorkerPool *vector_index_worker_pool_{nullptr};
    // Vector index data path
    std::string vector_index_data_path_{""};
    // Vector index cache with thread safety
    mutable std::shared_mutex vec_indexes_mutex_;
    // map of index name to index version and index object
    std::unordered_map<std::string,
                       std::pair<uint64_t, std::shared_ptr<VectorIndex>>>
        vec_indexes_;
    std::unordered_set<std::string> pending_persist_indexes_;
};

}  // namespace EloqVec
