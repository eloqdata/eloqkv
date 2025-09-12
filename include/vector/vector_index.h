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

namespace EloqVec
{

/**
 * @brief Vector index algorithm types
 */
enum class Algorithm
{
    // Hierarchical Navigable Small World
    HNSW,
    UNKNOWN
};

/**
 * @brief Distance metric types for vector similarity calculations
 */
enum class DistanceMetric
{
    L2SQ,    ///< Squared Euclidean
    IP,      ///< Inner product (dot product)
    COSINE,  ///< Cosine similarity
    UNKNOWN
};

/**
 * @brief Convert distance metric enum to string representation
 *
 * @param metric The distance metric enum value
 * @return String representation of the metric
 */
inline std::string distance_metric_to_string(DistanceMetric metric)
{
    switch (metric)
    {
    case DistanceMetric::L2SQ:
        return "L2SQ";
    case DistanceMetric::IP:
        return "IP";
    case DistanceMetric::COSINE:
        return "COSINE";
    default:
        return "UNKNOWN";
    }
}

/**
 * @brief Convert string to distance metric enum
 *
 * @param str String representation of the metric
 * @return Distance metric enum value, defaults to L2 if string is invalid
 */
inline DistanceMetric string_to_distance_metric(const std::string &str)
{
    if (str == "L2SQ")
        return DistanceMetric::L2SQ;
    if (str == "IP")
        return DistanceMetric::IP;
    if (str == "COSINE")
        return DistanceMetric::COSINE;
    return DistanceMetric::UNKNOWN;
}

/**
 * @brief Convert string to distance metric enum
 *
 * @param sv String view representation of the metric
 * @return Distance metric enum value, defaults to L2 if string is invalid
 */
inline DistanceMetric string_to_distance_metric(const std::string_view &sv)
{
    std::string metric_str(sv);
    std::transform(
        metric_str.begin(), metric_str.end(), metric_str.begin(), ::toupper);
    if (metric_str == "L2SQ")
        return DistanceMetric::L2SQ;
    if (metric_str == "IP")
        return DistanceMetric::IP;
    if (metric_str == "COSINE")
        return DistanceMetric::COSINE;
    return DistanceMetric::UNKNOWN;
}

/**
 * @brief Convert algorithm enum to string representation
 *
 * @param algorithm The algorithm enum value
 * @return String representation of the algorithm
 */
inline std::string algorithm_to_string(Algorithm algorithm)
{
    switch (algorithm)
    {
    case Algorithm::HNSW:
        return "HNSW";
    default:
        return "UNKNOWN";
    }
}

/**
 * @brief Convert string to algorithm enum
 *
 * @param sv String view representation of the algorithm
 * @return Algorithm enum value, defaults to HNSW if string is invalid
 */
inline Algorithm string_to_algorithm(const std::string_view &sv)
{
    std::string alg_str(sv);
    std::transform(alg_str.begin(), alg_str.end(), alg_str.begin(), ::toupper);
    if (alg_str == "HNSW")
        return Algorithm::HNSW;
    return Algorithm::UNKNOWN;
}

/**
 * @brief Result structure for vector search operations
 */
struct SearchResult
{
    std::vector<uint64_t> ids;     ///< Vector IDs of the search results
    std::vector<float> distances;  ///< Distances to the query vector
    std::vector<std::vector<float>>
        vectors;  ///< Optional: actual vectors (if requested)

    SearchResult() = default;
    SearchResult(std::vector<uint64_t> ids, std::vector<float> distances)
        : ids(std::move(ids)), distances(std::move(distances))
    {
    }
};

/**
 * @brief Configuration parameters for vector index operations
 */
struct IndexConfig
{
    size_t dimension = 0;           ///< Vector dimension
    size_t max_elements = 1000000;  ///< Maximum number of elements
    DistanceMetric distance_metric =
        DistanceMetric::L2SQ;       ///< Distance metric type
    std::string storage_path = "";  ///< Path for persistent storage

    std::unordered_map<std::string, std::string>
        params;  ///< Index type specific parameters
    IndexConfig() = default;
};

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
     * @param exact Whether to return exact matches
     * @param filter Optional filter function to apply to results
     * @return SearchResult containing IDs, distances, and optionally vectors
     */
    virtual SearchResult search(
        const std::vector<float> &query_vector,
        size_t k,
        bool exact = false,
        std::optional<std::function<bool(uint64_t)>> filter = std::nullopt) = 0;

    /**
     * @brief Add a vector to the index
     *
     * @param vector The vector to add
     * @param id Unique identifier for the vector
     * @return true if addition successful, false otherwise
     */
    virtual bool add(const std::vector<float> &vector, uint64_t id) = 0;

    /**
     * @brief Add multiple vectors to the index in batch
     *
     * @param vectors Vector of vectors to add
     * @param ids Vector of unique identifiers
     * @return true if batch addition successful, false otherwise
     */
    virtual bool add_batch(const std::vector<std::vector<float>> &vectors,
                           const std::vector<uint64_t> &ids) = 0;

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
    virtual bool update(const std::vector<float> &vector, uint64_t id) = 0;

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
