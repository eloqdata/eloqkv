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
#include <unordered_set>
#include <vector>

#include "cloud_manager.h"
#include "eloq_string_key_record.h"
#include "tx_execution.h"
#include "tx_service.h"
#include "vector_index.h"
#include "vector_type.h"

namespace EloqVec
{

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
     * @param vector_index_worker_pool Vector index worker pool
     * @param vector_index_data_path Vector index data path
     * @param cloud_config Pointer to Cloud configuration (optional)
     * @return True if the handler instance is initialized successfully, false
     * otherwise
     */
    static bool InitHandlerInstance(
        txservice::TxService *tx_service,
        txservice::TxWorkerPool *vector_index_worker_pool,
        std::string &vector_index_data_path,
        const CloudConfig *cloud_config = nullptr);

    /**
     * @brief Destroy the singleton instance of VectorHandler
     */
    static void DestroyHandlerInstance();

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
     * @param index_metadata Metadata for the vector index to create
     * @return Result of the create operation
     */
    VectorOpResult Create(const VectorIndexMetadata &index_metadata);

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
     * @param metadata (OUT) Metadata of the vector index
     * @return Result of the info operation
     */
    VectorOpResult Info(const std::string &name, VectorIndexMetadata &metadata);

    /**
     * @brief Search for similar vectors in an index
     *
     * @param name Name of the vector index to search
     * @param query_vector Query vector
     * @param k_count Number of nearest neighbors to return
     * @param thread_id Thread ID to execute the search
     * @param filter_json JSON filter view string for metadata filtering
     * @param vector_result (OUT) Result of the search vectors
     * @return Result of the search operation
     */
    VectorOpResult Search(const std::string &name,
                          const std::vector<float> &query_vector,
                          size_t k_count,
                          size_t thread_id,
                          const std::string_view &filter_json,
                          SearchResult &vector_result);

    /**
     * @brief Add a vector entry to an index
     *
     * @param name Name of the vector index
     * @param id Unique identifier for the vector
     * @param vector Vector data
     * @param metadata JSON string containing metadata for the vector record
     * @return Result of the add operation
     */
    VectorOpResult Add(const std::string &name,
                       uint64_t id,
                       const std::vector<float> &vector,
                       const std::string_view &metadata);

    /**
     * @brief Update a vector entry in an index
     *
     * @param name Name of the vector index
     * @param id Unique identifier for the vector
     * @param vector Vector data
     * @param metadata JSON string containing metadata for the vector
     * record
     * @return Result of the update operation
     */
    VectorOpResult Update(const std::string &name,
                          uint64_t id,
                          const std::vector<float> &vector,
                          const std::string_view &metadata);

    /**
     * @brief Delete a vector entry from an index
     *
     * @param name Name of the vector index
     * @param id Unique identifier for the vector
     * @return Result of the delete operation
     */
    VectorOpResult Delete(const std::string &name, uint64_t id);

    /**
     * @brief Add multiple vectors to an index in batch
     *
     * @param name Name of the vector index
     * @param ids Vector of unique identifiers
     * @param vectors Vector of vectors
     * @param metadata_list JSON strings containing metadata for each
     * vector record
     * @return Result of the batch add operation
     */
    VectorOpResult BatchAdd(const std::string &name,
                            const std::vector<uint64_t> &ids,
                            const std::vector<std::vector<float>> &vectors,
                            const std::vector<std::string_view> &metadata_list);

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
    /**
     * @brief Cache entry holding both index and its metadata
     */
    struct IndexCache
    {
        std::shared_ptr<VectorIndex> index_;
        std::shared_ptr<VectorIndexMetadata> metadata_;
    };

    // Private constructor to prevent instantiation
    VectorHandler(txservice::TxService *tx_service,
                  txservice::TxWorkerPool *vector_index_worker_pool,
                  std::string &vector_index_data_path,
                  const CloudConfig *cloud_config = nullptr)
        : tx_service_(tx_service),
          vector_index_worker_pool_(vector_index_worker_pool),
          vector_index_data_path_(vector_index_data_path)
    {
        initialized_ = true;
        if (cloud_config && !cloud_config->endpoint_.empty())
        {
            cloud_manager_ = std::make_unique<CloudManager>(*cloud_config);
            if (!cloud_manager_->ConnectCloudService())
            {
                cloud_manager_.reset();
                initialized_ = false;
            }
        }
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
     * @param txm Transaction execution context
     * @return Pair of (IndexCache containing index and metadata, result)
     */
    std::pair<IndexCache, VectorOpResult> GetOrCreateIndex(
        const std::string &name,
        const txservice::TxRecord::Uptr &h_record,
        txservice::TransactionExecution *txm);

    /**
     * @brief Create a vector index with given configuration and initialize it
     *
     * @param h_record Record containing encoded metadata
     * @param txm Transaction execution context
     * @return Pair of IndexCache (containing index and metadata) and the result
     */
    std::pair<IndexCache, VectorOpResult> CreateAndInitializeIndex(
        const txservice::TxRecord::Uptr &h_record,
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

    bool initialized_{false};
    // Transaction service
    txservice::TxService *tx_service_{nullptr};
    // Vector index worker pool
    txservice::TxWorkerPool *vector_index_worker_pool_{nullptr};
    // Vector index data path
    std::string vector_index_data_path_{""};
    // Vector index cache with thread safety
    mutable std::shared_mutex vec_indexes_mutex_;
    // Unified cache: map of index name to (index + metadata)
    std::unordered_map<std::string, IndexCache> vec_indexes_;
    std::unordered_set<std::string> pending_persist_indexes_;
    // Cloud service manager
    std::unique_ptr<CloudManager> cloud_manager_{nullptr};
};

}  // namespace EloqVec
