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
 * @brief Persist strategy types
 */
enum class PersistStrategy
{
    // Persist every N log items
    EVERY_N,
    // Persist manually
    MANUAL,
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
 * @brief Convert string to persist strategy enum
 *
 * @param sv String view representation of the persist strategy
 * @return Persist strategy enum value
 */
inline PersistStrategy string_to_persist_strategy(const std::string_view &sv)
{
    std::string persist_strategy_str(sv);
    std::transform(persist_strategy_str.begin(),
                   persist_strategy_str.end(),
                   persist_strategy_str.begin(),
                   ::toupper);
    if (persist_strategy_str == "EVERY_N")
    {
        return PersistStrategy::EVERY_N;
    }
    if (persist_strategy_str == "MANUAL")
    {
        return PersistStrategy::MANUAL;
    }
    return PersistStrategy::UNKNOWN;
}

/**
 * @brief Configuration parameters for vector index operations
 */
struct IndexConfig
{
    IndexConfig() = default;
    IndexConfig(size_t dimension,
                Algorithm algorithm,
                DistanceMetric metric,
                std::unordered_map<std::string, std::string> &&alg_params)
        : dimension(dimension),
          algorithm(algorithm),
          distance_metric(metric),
          params(std::move(alg_params))
    {
    }

    size_t dimension = 0;                   ///< Vector dimension
    size_t max_elements = 1000000;          ///< Maximum number of elements
    Algorithm algorithm = Algorithm::HNSW;  ///< Algorithm type
    DistanceMetric distance_metric =
        DistanceMetric::L2SQ;  ///< Distance metric type

    /// Index type specific parameters (e.g., m, ef_construction, ef_search for
    /// HNSW) Marked mutable to allow adding default parameters in
    /// initialize_usearch_index()
    mutable std::unordered_map<std::string, std::string> params;

    /**
     * @brief Encode IndexConfig to binary format
     * @param encoded_str Output string to append encoded data
     */
    void Encode(std::string &encoded_str) const;

    /**
     * @brief Decode IndexConfig from binary format
     * @param buf Buffer containing encoded data
     * @param buff_size Size of the buffer
     * @param offset Current offset in buffer (updated after decoding)
     */
    void Decode(const char *buf, size_t buff_size, size_t &offset);
};

/**
 * @brief Unified metadata for vector index combining configuration and state
 *
 * Design rationale:
 * - name: Index identifier (metadata, not algorithm config)
 * - persist_threshold: Persistence strategy (metadata, not algorithm config)
 * - file_path: Full path to persisted index file (runtime state)
 * - config_: Embedded algorithm configuration (eliminates duplication)
 * - timestamps: Lifecycle tracking (runtime state)
 */
class VectorIndexMetadata
{
public:
    VectorIndexMetadata() = default;

    /**
     * @brief Constructor from configuration and metadata parameters
     *
     * @param name Index name
     * @param config Algorithm configuration
     * @param persist_threshold Persistence threshold (-1 for manual)
     * @param storage_base_path Base directory for index files (used to generate
     * file_path)
     */
    VectorIndexMetadata(const std::string &name,
                        const IndexConfig &config,
                        int64_t persist_threshold,
                        const std::string &storage_base_path);

    /**
     * @brief Encode metadata to binary format
     * @param encoded_str Output string to append encoded data
     */
    void Encode(std::string &encoded_str) const;

    /**
     * @brief Decode metadata from binary format
     * @param buf Buffer containing encoded data
     * @param buff_size Size of the buffer
     * @param offset Current offset in buffer (updated after decoding)
     * @param version Version timestamp for created_ts initialization
     */
    void Decode(const char *buf,
                size_t buff_size,
                size_t &offset,
                uint64_t version);

    // Configuration accessors (immutable)
    const std::string &Name() const
    {
        return name_;
    }
    const IndexConfig &Config() const
    {
        return config_;
    }

    // Metadata accessors
    int64_t PersistThreshold() const
    {
        return persist_threshold_;
    }

    // State accessors (mutable)
    const std::string &FilePath() const
    {
        return file_path_;
    }
    void SetFilePath(const std::string &path)
    {
        file_path_ = path;
    }

    uint64_t CreatedTs() const
    {
        return created_ts_;
    }
    uint64_t LastPersistTs() const
    {
        return last_persist_ts_;
    }
    void SetLastPersistTs(uint64_t ts)
    {
        last_persist_ts_ = ts;
    }

private:
    // Index name
    std::string name_;
    // Embedded algorithm configuration
    IndexConfig config_;
    // Persistence threshold (-1 = manual)
    int64_t persist_threshold_{10000};
    // Full path to persisted index file (with timestamp)
    std::string file_path_;
    // Creation timestamp
    uint64_t created_ts_{0};
    // Last persistence timestamp
    uint64_t last_persist_ts_{0};
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