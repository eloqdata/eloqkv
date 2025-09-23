/**
 * @file vector_index.h
 * @brief Virtual base class for vector index implementations
 *
 * This file defines the VectorIndex virtual class that serves as the base
 * interface for different vector index types like HNSW (Hierarchical Navigable
 * Small World) and IVF (Inverted File) indices.
 *
 * @author EloqData Inc.
 * @date 2025
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "vector_type.h"

namespace EloqVec
{

/**
 * @brief Virtual base class for vector index implementations
 *
 * This class defines the interface that all vector index implementations
 * must follow. It provides methods for basic operations like initialization,
 * loading, saving, searching, and managing vectors.
 */
class VectorIndex
{
public:
    /**
     * @brief Virtual destructor
     */
    virtual ~VectorIndex() = default;

    /**
     * @brief Initialize the vector index with given configuration
     *
     * @param config Configuration parameters for the index
     * @return true if initialization successful, false otherwise
     */
    virtual bool initialize(const IndexConfig &config) = 0;

    /**
     * @brief Load an existing index from storage. This call blocks until the
     * index is loaded.
     *
     * @param path Path to the index file
     * @return true if loading successful, false otherwise
     */
    virtual bool load(const std::string &path) = 0;

    /**
     * @brief Save the current index to storage. This call blocks until the
     * index is saved.
     *
     * @param path Path where to save the index file
     * @return true if saving successful, false otherwise
     */
    virtual bool save(const std::string &path) = 0;

    /**
     * @brief Search for similar vectors
     *
     * @param query_vector The query vector to search for
     * @param k Number of nearest neighbors to return
     * @param result SearchResult containing IDs, distances, and optionally
     * vectors
     * @param exact Whether to return exact matches
     * @param filter Optional filter function to apply to results
     * @return IndexOpResult
     */
    virtual IndexOpResult search(
        const std::vector<float> &query_vector,
        size_t k,
        SearchResult &result,
        bool exact = false,
        std::optional<std::function<bool(uint64_t)>> filter = std::nullopt) = 0;

    /**
     * @brief Add a vector to the index
     *
     * @param vector The vector to add
     * @param id Unique identifier for the vector
     * @return IndexOpResult
     */
    virtual IndexOpResult add(const std::vector<float> &vector,
                              uint64_t id) = 0;

    /**
     * @brief Add multiple vectors to the index in batch
     *
     * @param vectors Vector of vectors to add
     * @param ids Vector of unique identifiers
     * @return IndexOpResult
     */
    virtual IndexOpResult add_batch(
        const std::vector<std::vector<float>> &vectors,
        const std::vector<uint64_t> &ids) = 0;

    /**
     * @brief Delete a vector from the index
     *
     * @param id Unique identifier of the vector to delete
     * @return IndexOpResult
     */
    virtual IndexOpResult remove(uint64_t id) = 0;

    /**
     * @brief Update an existing vector in the index
     *
     * @param vector The new vector data
     * @param id Unique identifier of the vector to update
     * @return IndexOpResult
     */
    virtual IndexOpResult update(const std::vector<float> &vector,
                                 uint64_t id) = 0;

    /**
     * @brief Get a vector from the index
     *
     * @param id Unique identifier of the vector to get
     * @param vector [OUT] The vector data. If the size of the vector is 0, it
     * means that no such @id exists in the index.
     * @return IndexOpResult
     */
    virtual IndexOpResult get(uint64_t id, std::vector<float> &vector) = 0;

    /**
     * @brief Get the current memory usage of the index
     *
     * @return Memory usage in bytes
     */
    virtual size_t memory_usage() = 0;

    /**
     * @brief Check if the index is initialized and ready for operations
     *
     * @return true if index is ready, false otherwise
     */
    virtual bool is_ready() = 0;

    /**
     * @brief Get the dimension of vectors in this index
     *
     * @return Vector dimension
     */
    virtual size_t get_dimension() = 0;

    /**
     * @brief Get the total number of elements in the index
     *
     * @return Number of elements
     */
    virtual size_t size() = 0;

    /**
     * @brief Optimize the index for better performance
     *
     * This method may rebuild internal structures for optimal performance.
     *
     * @return true if optimization successful, false otherwise
     */
    virtual bool optimize() = 0;

    /**
     * @brief Get the type name of this index implementation
     *
     * @return String identifier for the index type (e.g., "HNSW", "IVF")
     */
    virtual std::string get_type() const = 0;

    /**
     * @brief Get the persist threshold of the index
     *
     * @return Persist threshold, -1 means MANUAL strategy.
     */
    virtual int64_t get_persist_threshold() = 0;

    /**
     * @brief Set search parameters for the index
     *
     * @param params Index type specific parameters
     * @return true if parameter setting successful, false otherwise
     */
    virtual bool set_search_params(
        std::unordered_map<std::string, std::string> params) = 0;

    /**
     * @brief Set update parameters for the index
     *
     * @param params Index type specific parameters
     * @return true if parameter setting successful, false otherwise
     */
    virtual bool set_update_params(
        std::unordered_map<std::string, std::string> params) = 0;
};

/**
 * @brief Factory function type for creating vector index instances
 */
using VectorIndexFactory = std::function<std::unique_ptr<VectorIndex>()>;

/**
 * @brief Register a vector index factory
 *
 * @param type_name Name of the index type
 * @param factory Factory function for creating instances
 */
void register_vector_index_factory(const std::string &type_name,
                                   VectorIndexFactory factory);

/**
 * @brief Create a vector index instance by type name
 *
 * @param type_name Name of the index type ("HNSW", "IVF", etc.)
 * @return Unique pointer to the created index, or nullptr if type not found
 */
std::unique_ptr<VectorIndex> create_vector_index(const std::string &type_name);

}  // namespace EloqVec
