/**
 * @file hnsw_vector_index.h
 * @brief HNSW (Hierarchical Navigable Small World) vector index implementation
 * 
 * This file defines the HNSWVectorIndex class that implements the VectorIndex
 * interface using the usearch library for high-performance approximate nearest
 * neighbor search.
 * 
 * @author EloqData Inc.
 * @date 2025
 */

#pragma once

#include "vector_index.h"
#include <memory>
#include <mutex>
#include <unordered_map>

#include <usearch/index_dense.hpp>
#include <usearch/index_plugins.hpp>
/**
 * @brief HNSWVectorIndex: HNSW-based VectorIndex implementation using usearch.
 *
 * Provides configurable, high-performance approximate nearest neighbor search
 * (HNSW) with support for adding, removing, updating vectors and exporting/importing
 * the underlying index.
 */
/**
 * @brief Construct a new HNSWVectorIndex.
 */
 /**
  * @brief Destroy the HNSWVectorIndex.
  */
/**
 * @brief Initialize the index with the given configuration.
 *
 * @param config Configuration options (dimension, metric, HNSW parameters, etc.).
 * @return true if the index was configured and initialized successfully; false otherwise.
 */
/**
 * @brief Load an index from the specified file path.
 *
 * @param path Filesystem path to the serialized index.
 * @return true on successful load; false on failure.
 */
/**
 * @brief Save the current index state to the specified file path.
 *
 * @param path Filesystem path where the index will be written.
 * @return true on success; false on failure.
 */
/**
 * @brief Search for nearest neighbors of a query vector.
 *
 * Executes an approximate (or exact when requested) k-NN search and populates
 * the provided SearchResult with found IDs and distances.
 *
 * @param query_vector Query vector of the configured dimensionality.
 * @param k Number of nearest neighbors to retrieve.
 * @param result Output parameter that will be filled with the search results.
 * @param exact If true, performs an exact search; otherwise uses the HNSW approximate search.
 * @param filter Optional predicate to exclude candidate IDs from results.
 * @return IndexOpResult indicating success or the specific failure condition.
 */
/**
 * @brief Add a single vector to the index with the associated id.
 *
 * @param vector Vector to insert (must match index dimensionality).
 * @param id Identifier associated with the vector.
 * @return IndexOpResult indicating success or failure.
 */
/**
 * @brief Add multiple vectors to the index in a batch.
 *
 * Vectors and ids must be one-to-one and vectors must match the index dimensionality.
 *
 * @param vectors Collection of vectors to insert.
 * @param ids Corresponding identifiers for each vector.
 * @return IndexOpResult indicating success or failure.
 */
/**
 * @brief Remove the vector associated with the given id from the index.
 *
 * @param id Identifier whose vector should be removed.
 * @return IndexOpResult indicating success or failure.
 */
/**
 * @brief Update the vector associated with an existing id.
 *
 * Replaces the stored vector for the provided id with the new vector (must match dimensionality).
 *
 * @param vector New vector value.
 * @param id Identifier of the vector to update.
 * @return IndexOpResult indicating success or failure.
 */
/**
 * @brief Get the memory usage of the underlying index in bytes.
 *
 * @return Memory usage in bytes.
 */
/**
 * @brief Check whether the index is initialized and ready for operations.
 *
 * @return true if ready; false otherwise.
 */
/**
 * @brief Get the dimensionality of vectors stored in the index.
 *
 * @return Number of dimensions.
 */
/**
 * @brief Get the number of vectors currently stored in the index.
 *
 * @return Number of indexed vectors.
 */
/**
 * @brief Trigger internal optimizations (if supported) to improve query performance or storage.
 *
 * @return true if optimization completed successfully or is a no-op; false on failure.
 */
/**
 * @brief Get a descriptive type name for this index implementation.
 *
 * @return String describing the index type (e.g., "hnsw_usearch").
 */
/**
 * @brief Set search-related parameters.
 *
 * Accepts key/value string parameters that adjust search-time behavior (e.g., ef_search).
 *
 * @param params Map of parameter names to string values.
 * @return true if parameters were accepted and applied; false otherwise.
 */
/**
 * @brief Set update-related parameters.
 *
 * Accepts key/value string parameters that influence add/update/remove behavior.
 *
 * @param params Map of parameter names to string values.
 * @return true if parameters were accepted and applied; false otherwise.
 */
/**
 * @brief Convert an EloqVec DistanceMetric to the corresponding usearch metric type.
 *
 * @param metric EloqVec distance metric enum value.
 * @return Corresponding unum::usearch::metric_kind_t value.
 */
/**
 * @brief Validate an IndexConfig for compatibility with this index implementation.
 *
 * Checks required fields such as dimensionality and supported metrics.
 *
 * @param config Configuration to validate.
 * @return true if the configuration is valid for initialization; false otherwise.
 */
/**
 * @brief Initialize the underlying usearch index according to the provided configuration.
 *
 * Allocates and configures the usearch index structures based on IndexConfig.
 *
 * @param config Configuration parameters used to create the usearch index.
 * @return true on successful initialization; false on failure.
 */
/**
 * @brief Create and return a new HNSWVectorIndex instance.
 *
 * @return std::unique_ptr<VectorIndex> Unique pointer to the created HNSWVectorIndex.
 */
namespace EloqVec {

/**
 * @brief HNSW vector index implementation using usearch library
 * 
 * This class provides an HNSW-based implementation of the VectorIndex interface,
 * offering high-performance approximate nearest neighbor search with configurable
 * accuracy and speed trade-offs.
 */
class HNSWVectorIndex : public VectorIndex {
public:
    /**
     * @brief Constructor
     */
    HNSWVectorIndex();

    /**
     * @brief Destructor
     */
    virtual ~HNSWVectorIndex();

    // VectorIndex interface implementation
    bool initialize(const IndexConfig& config) override;
    bool load(const std::string& path) override;
    bool save(const std::string& path) override;
    
    IndexOpResult search(
        const std::vector<float>& query_vector,
        size_t k,
        SearchResult &result,
        bool exact = false,
        std::optional<std::function<bool(uint64_t)>> filter = std::nullopt
    ) override;
    
    IndexOpResult add(const std::vector<float>& vector, uint64_t id) override;
    IndexOpResult add_batch(
        const std::vector<std::vector<float>>& vectors,
        const std::vector<uint64_t>& ids
    ) override;
    IndexOpResult remove(uint64_t id) override;
    IndexOpResult update(const std::vector<float>& vector, uint64_t id) override;
    size_t memory_usage() override;
    bool is_ready() override;
    size_t get_dimension() override;
    size_t size() override;
    bool optimize() override;
    std::string get_type() const override;
    bool set_search_params(std::unordered_map<std::string, std::string> params) override;
    bool set_update_params(std::unordered_map<std::string, std::string> params) override;

private:
    /**
     * @brief Convert DistanceMetric enum to usearch metric type
     * 
     * @param metric EloqVec distance metric
     * @return usearch metric type
     */
    unum::usearch::metric_kind_t convert_metric(DistanceMetric metric) const;

    /**
     * @brief Validate configuration parameters
     * 
     * @param config Configuration to validate
     * @return true if valid, false otherwise
     */
    bool validate_config(const IndexConfig& config) const;

    /**
     * @brief Initialize usearch index with configuration
     * 
     * @param config Configuration parameters
     * @return true if successful, false otherwise
     */
    bool initialize_usearch_index(const IndexConfig& config);

private:
    // usearch index instance
    unum::usearch::index_dense_t usearch_index_;
    
    // Configuration
    IndexConfig config_;
    
    // Thread safety
    std::shared_mutex index_mutex_;
    
    // Internal state
    bool initialized_;
};

/**
 * @brief Factory function for creating HNSWVectorIndex instances
 * 
 * @return Unique pointer to HNSWVectorIndex instance
 */
std::unique_ptr<VectorIndex> create_hnsw_vector_index();

} // namespace EloqVec
