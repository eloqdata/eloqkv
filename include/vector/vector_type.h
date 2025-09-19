/**
 * @file vector_type.h
 * @brief Vector type definitions for EloqVec
 *
 * This file defines the vector type definitions for EloqVec.
 *
 * @author EloqData Inc.
 * @date 2025
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "type.h"

namespace EloqVec
{

constexpr static std::string_view vector_index_meta_table_name_sv{
    "__vector_index_meta_table"};

inline static txservice::TableName vector_index_meta_table{
    vector_index_meta_table_name_sv,
    txservice::TableType::Primary,
    txservice::TableEngine::InternalHash};

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
    if (str == "L2SQ" || str == "L2")
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
    if (metric_str == "L2SQ" || metric_str == "L2")
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
 * @brief Configuration parameters for vector index operations
 */
struct IndexConfig
{
    IndexConfig() = default;
    IndexConfig(const std::string &name,
                size_t dimension,
                Algorithm algorithm,
                DistanceMetric metric,
                std::string &storage_path,
                std::unordered_map<std::string, std::string> &&alg_params)
        : name(name),
          dimension(dimension),
          algorithm(algorithm),
          distance_metric(metric),
          storage_path(storage_path),
          params(std::move(alg_params))
    {
    }

    std::string name = "";                  ///< Index name
    size_t dimension = 0;                   ///< Vector dimension
    size_t max_elements = 1000000;          ///< Maximum number of elements
    Algorithm algorithm = Algorithm::HNSW;  ///< Algorithm type
    DistanceMetric distance_metric =
        DistanceMetric::L2SQ;       ///< Distance metric type
    std::string storage_path = "";  ///< Path for persistent storage

    std::unordered_map<std::string, std::string>
        params;  ///< Index type specific parameters
};

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

enum class VectorOpResult
{
    SUCCEED,
    INDEX_EXISTED,
    INDEX_NOT_EXIST,
    INDEX_META_OP_FAILED,
    VECTOR_DIMENSION_MISMATCH,
    INDEX_INTERNAL_ERROR,
    INDEX_INIT_FAILED,
    INDEX_LOAD_FAILED,
    INDEX_ADD_FAILED,
    INDEX_UPDATE_FAILED,
    INDEX_DELETE_FAILED,
    INDEX_LOG_OP_FAILED,
    UNKNOWN,
};

struct IndexOpResult
{
    VectorOpResult error = VectorOpResult::SUCCEED;  ///< Error flag
    std::string error_message = "";                  ///< Error message
    IndexOpResult(VectorOpResult error, std::string error_message)
        : error(error), error_message(std::move(error_message))
    {
    }
};

}  // namespace EloqVec