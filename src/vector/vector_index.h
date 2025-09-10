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

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <optional>
#include <functional>

namespace EloqVec {

/**
 * @brief Distance metric types for vector similarity calculations
 */
enum class DistanceMetric {
    L2,         ///< Euclidean distance (L2 norm)
    IP,         ///< Inner product (dot product)
    COSINE     ///< Cosine similarity
};

/**
 * @brief Convert distance metric enum to string representation
 * 
 * @param metric The distance metric enum value
 * @return String representation of the metric
 */
inline std::string distance_metric_to_string(DistanceMetric metric) {
    switch (metric) {
        case DistanceMetric::L2: return "L2";
        case DistanceMetric::IP: return "IP";
        case DistanceMetric::COSINE: return "COSINE";
        default: return "UNKNOWN";
    }
}

/**
 * @brief Convert string to distance metric enum
 * 
 * @param str String representation of the metric
 * @return Distance metric enum value, defaults to L2 if string is invalid
 */
inline DistanceMetric string_to_distance_metric(const std::string& str) {
    if (str == "L2") return DistanceMetric::L2;
    if (str == "IP") return DistanceMetric::IP;
    if (str == "COSINE") return DistanceMetric::COSINE;
    return DistanceMetric::L2; // Default fallback
}

/**
 * @brief Result structure for vector search operations
 */
struct SearchResult {
    std::vector<uint64_t> ids;           ///< Vector IDs of the search results
    std::vector<float> distances;        ///< Distances to the query vector
    std::vector<std::vector<float>> vectors; ///< Optional: actual vectors (if requested)
    
    SearchResult() = default;
    SearchResult(std::vector<uint64_t> ids, std::vector<float> distances)
        : ids(std::move(ids)), distances(std::move(distances)) {}
};

/**
 * @brief Configuration parameters for vector index operations
 */
struct IndexConfig {
    size_t dimension = 0;                ///< Vector dimension
    size_t max_elements = 1000000;       ///< Maximum number of elements
    DistanceMetric distance_metric = DistanceMetric::L2;  ///< Distance metric type
    std::string storage_path = "";       ///< Path for persistent storage
    
    std::unordered_map<std::string, std::string> params; ///< Index type specific parameters
    IndexConfig() = default;
};

/**
 * @brief Statistics about the vector index
 */
struct IndexStats {
    size_t total_elements = 0;           ///< Total number of elements in index
    size_t memory_usage_bytes = 0;       ///< Memory usage in bytes
    uint64_t last_save_time = 0;         ///< Time of last save operation
    bool is_loaded = false;              ///< Whether index is currently loaded
    std::string index_type = "";         ///< Type of index implementation
};

/**
 * @brief Virtual base class for vector index implementations
 * 
 * This class defines the interface that all vector index implementations
 * must follow. It provides methods for basic operations like initialization,
 * loading, saving, searching, and managing vectors.
 */
class VectorIndex {
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
    virtual bool initialize(const IndexConfig& config) = 0;

    /**
     * @brief Load an existing index from storage
     * 
     * @param path Path to the index file
     * @return true if loading successful, false otherwise
     */
    virtual bool load(const std::string& path) = 0;

    /**
     * @brief Save the current index to storage
     * 
     * @param path Path where to save the index file
     * @return true if saving successful, false otherwise
     */
    virtual bool save(const std::string& path) = 0;

    /**
     * @brief Search for similar vectors
     * 
     * @param query_vector The query vector to search for
     * @param k Number of nearest neighbors to return
     * @param exact Whether to return exact matches
     * @param filter Optional filter function to apply to results
     * @return SearchResult containing IDs, distances, and optionally vectors
     */
    virtual SearchResult search(
        const std::vector<float>& query_vector,
        size_t k,
        bool exact = false,
        std::optional<std::function<bool(uint64_t)>> filter = std::nullopt
    ) const = 0;

    /**
     * @brief Add a vector to the index
     * 
     * @param vector The vector to add
     * @param id Unique identifier for the vector
     * @return true if addition successful, false otherwise
     */
    virtual bool add(const std::vector<float>& vector, uint64_t id) = 0;

    /**
     * @brief Add multiple vectors to the index in batch
     * 
     * @param vectors Vector of vectors to add
     * @param ids Vector of unique identifiers
     * @return true if batch addition successful, false otherwise
     */
    virtual bool add_batch(
        const std::vector<std::vector<float>>& vectors,
        const std::vector<uint64_t>& ids
    ) = 0;

    /**
     * @brief Delete a vector from the index
     * 
     * @param id Unique identifier of the vector to delete
     * @return true if deletion successful, false otherwise
     */
    virtual bool remove(uint64_t id) = 0;

    /**
     * @brief Update an existing vector in the index
     * 
     * @param vector The new vector data
     * @param id Unique identifier of the vector to update
     * @return true if update successful, false otherwise
     */
    virtual bool update(const std::vector<float>& vector, uint64_t id) = 0;

    /**
     * @brief Get the current memory usage of the index
     * 
     * @return Memory usage in bytes
     */
    virtual size_t memory_usage() const = 0;

    /**
     * @brief Get statistics about the index
     * 
     * @return IndexStats containing various statistics
     */
    virtual IndexStats get_stats() const = 0;

    /**
     * @brief Check if the index is initialized and ready for operations
     * 
     * @return true if index is ready, false otherwise
     */
    virtual bool is_ready() const = 0;

    /**
     * @brief Get the dimension of vectors in this index
     * 
     * @return Vector dimension
     */
    virtual size_t get_dimension() const = 0;

    /**
     * @brief Get the total number of elements in the index
     * 
     * @return Number of elements
     */
    virtual size_t size() const = 0;

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
     * @brief Set search parameters for the index
     * 
     * @param params Index type specific parameters
     * @return true if parameter setting successful, false otherwise
     */
    virtual bool set_search_params(std::unordered_map<std::string, std::string> params) = 0;
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
void register_vector_index_factory(const std::string& type_name, VectorIndexFactory factory);

/**
 * @brief Create a vector index instance by type name
 * 
 * @param type_name Name of the index type ("HNSW", "IVF", etc.)
 * @return Unique pointer to the created index, or nullptr if type not found
 */
std::unique_ptr<VectorIndex> create_vector_index(const std::string& type_name);

} // namespace EloqVec
