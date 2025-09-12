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

#include <string>
#include <unordered_map>

#include "eloq_string_key_record.h"
#include "tx_execution.h"
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
    void Decode(const std::string &encoded_str);

private:
    std::string name_{""};
    size_t dimension_{0};
    Algorithm algorithm_{Algorithm::HNSW};
    DistanceMetric metric_{DistanceMetric::L2SQ};
    std::unordered_map<std::string, std::string> alg_params_;
    size_t buffer_threshold_{10000};
    // Total number of vectors in the index
    size_t size_{0};
    uint64_t created_ts_{0};
    uint64_t last_build_ts_{0};
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
     * @brief Get the singleton instance of VectorHandler
     *
     * @return Reference to the singleton VectorHandler instance
     */
    static VectorHandler &Instance();

    /**
     * @brief Create a new vector index
     *
     * @param idx_spec Configuration for the vector index to create
     * @param txm Transaction execution context
     * @return Result of the create operation
     */
    VectorOpResult Create(const IndexConfig &idx_spec,
                          txservice::TransactionExecution *txm);

    /**
     * @brief Drop (delete) a vector index
     *
     * @param name Name of the vector index to drop
     * @param txm Transaction execution context
     * @return Result of the drop operation
     */
    VectorOpResult Drop(const std::string &name,
                        txservice::TransactionExecution *txm);

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
     * @param search_params Search parameters
     * @param txm Transaction execution context
     * @param vector_result (OUT) Result of the search vectors
     * @return Result of the search operation
     */
    VectorOpResult Search(
        const std::string &name,
        const std::vector<float> &query_vector,
        size_t k_count,
        const std::unordered_map<std::string, std::string> &search_params,
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

private:
    // Private constructor to prevent instantiation
    VectorHandler() = default;
    ~VectorHandler() = default;

    // Delete copy and move constructors/operators
    VectorHandler(const VectorHandler &) = delete;
    VectorHandler(VectorHandler &&) = delete;
    VectorHandler &operator=(const VectorHandler &) = delete;
    VectorHandler &operator=(VectorHandler &&) = delete;
};

}  // namespace EloqVec
