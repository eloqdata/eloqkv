/**
 *    Copyright (C) 2025 EloqData Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under either of the following two licenses:
 *    1. GNU Affero General Public License, version 3, as published by the Free
 *    Software Foundation.
 *    2. GNU General Public License as published by the Free Software
 *    Foundation; version 2 of the License.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License or GNU General Public License for more
 *    details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    and GNU General Public License V2 along with this program.  If not, see
 *    <http://www.gnu.org/licenses/>.
 *
 */
#pragma once
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "INIReader.h"
#include "type.h"

namespace EloqVec
{

constexpr static std::string_view vector_index_meta_table_name_sv{
    "__vector_index_meta_table"};

inline static txservice::TableName vector_index_meta_table{
    vector_index_meta_table_name_sv,
    txservice::TableType::Primary,
    txservice::TableEngine::InternalHash};

constexpr static std::string_view initial_timestamp_sv{"0000000000000000"};

/**
 * @brief Metadata field types for vector record metadata
 */
enum class MetadataFieldType : uint8_t
{
    Int32 = 0,
    Int64 = 1,
    Double = 2,
    Bool = 3,
    String = 4,
    Unknown = 255
};

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
 * @brief Convert string to metadata field type enum
 *
 * @param sv String view representation of the metadata field type
 * @return Metadata field type enum value
 */
inline MetadataFieldType string_to_field_type(const std::string_view &sv)
{
    std::string field_type_str(sv);
    std::transform(field_type_str.begin(),
                   field_type_str.end(),
                   field_type_str.begin(),
                   ::toupper);
    if (field_type_str == "INT32")
    {
        return MetadataFieldType::Int32;
    }
    if (field_type_str == "INT64")
    {
        return MetadataFieldType::Int64;
    }
    if (field_type_str == "DOUBLE")
    {
        return MetadataFieldType::Double;
    }
    if (field_type_str == "BOOL")
    {
        return MetadataFieldType::Bool;
    }
    if (field_type_str == "STRING")
    {
        return MetadataFieldType::String;
    }
    return MetadataFieldType::Unknown;
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

    IndexConfig(const IndexConfig &) = delete;
    IndexConfig(IndexConfig &&) = default;
    IndexConfig &operator=(const IndexConfig &) = delete;
    IndexConfig &operator=(IndexConfig &&) = default;
    ~IndexConfig() = default;

    /**
     * @brief Serialize IndexConfig to binary format
     * @param str Output string to append encoded data
     */
    void Serialize(std::string &str) const;

    /**
     * @brief Deserialize IndexConfig from binary format
     * @param buf Buffer containing encoded data
     * @param buff_size Size of the buffer
     * @param offset Current offset in buffer (updated after decoding)
     */
    void Deserialize(const char *buf, size_t buff_size, size_t &offset);

    // Vector dimension
    size_t dimension = 0;
    // Maximum number of elements
    size_t max_elements = 1000000;
    // Algorithm type
    Algorithm algorithm = Algorithm::HNSW;
    // Distance metric type
    DistanceMetric distance_metric = DistanceMetric::L2SQ;
    // Index type specific parameters (e.g., m, ef_construction, ef_search for
    // HNSW) Marked mutable to allow adding default parameters in initialize()
    mutable std::unordered_map<std::string, std::string> params;
};

/**
 * @brief Vector record metadata schema definition
 *
 * This class represents the schema definition of vector record metadata,
 * storing an ordered list of field names and their corresponding types.
 */
class VectorRecordMetadata
{
public:
    VectorRecordMetadata() = default;
    VectorRecordMetadata(std::vector<std::string> &&field_names,
                         std::vector<MetadataFieldType> &&field_types)
        : field_names_(std::move(field_names)),
          field_types_(std::move(field_types))
    {
    }

    VectorRecordMetadata(const VectorRecordMetadata &) = delete;
    VectorRecordMetadata(VectorRecordMetadata &&) = default;
    VectorRecordMetadata &operator=(const VectorRecordMetadata &) = delete;
    VectorRecordMetadata &operator=(VectorRecordMetadata &&) = default;
    ~VectorRecordMetadata() = default;

    // Serialization methods
    void Serialize(std::string &str) const;
    void Deserialize(const char *buf, size_t buff_size, size_t &offset);

    /**
     * @brief Encode metadata values to binary format
     * @param metadata_json JSON string view representation of the metadata
     * @param buf Output buffer to append encoded data
     * @return True if encoding succeeded, false otherwise
     */
    bool Encode(const std::string_view &metadata_json,
                std::vector<char> &buf) const;

    /**
     * @brief Decode metadata values from binary format
     * @param buf Buffer containing encoded data
     * @param index Index of the field to decode
     * @param value Output value to decode
     */
    template <typename FieldT>
    void Decode(const std::vector<char> &buf, size_t index, FieldT &value) const
    {
        assert(index < field_types_.size() && "Index out of range");
        size_t offset = 0;
        for (size_t i = 0; i < index; ++i)
        {
            offset += GetFieldLength(i);
            if (field_types_[i] == MetadataFieldType::String)
            {
                size_t str_len =
                    *reinterpret_cast<const size_t *>(buf.data() + offset);
                offset += str_len;
            }
        }
        value = *reinterpret_cast<const FieldT *>(buf.data() + offset);
    }

    /**
     * @brief Decode metadata values to offset of each field
     * @param buf Buffer containing encoded data
     * @param offsets Output offsets of each field
     */
    void Decode(const std::vector<char> &buf,
                std::vector<size_t> &offsets) const;

    // Schema operation methods
    const std::vector<std::string> &FieldNames() const
    {
        return field_names_;
    }

    const std::vector<MetadataFieldType> &FieldTypes() const
    {
        return field_types_;
    }

    void AddMetadataField(const std::string &field_name,
                          MetadataFieldType field_type);
    bool HasMetadataField(const std::string &field_name) const
    {
        return std::find(field_names_.begin(),
                         field_names_.end(),
                         field_name) != field_names_.end();
    }
    bool CheckMetadataField(const std::string &field_name, size_t index) const
    {
        return index < field_names_.size() &&
               field_names_[index].compare(field_name) == 0;
    }

    MetadataFieldType GetFieldType(const std::string &field_name) const;
    MetadataFieldType GetFieldType(size_t index) const
    {
        assert(index < field_types_.size());
        return field_types_[index];
    }
    size_t GetFieldIndex(const std::string &field_name) const;
    size_t GetFieldLength(size_t index) const;
    size_t Size() const
    {
        return field_names_.size();
    }

    bool Empty() const
    {
        return field_names_.empty();
    }

private:
    std::vector<std::string> field_names_;
    std::vector<MetadataFieldType> field_types_;
};

/**
 * @brief Unified metadata for vector index combining configuration and state
 *
 * This class encapsulates all metadata about a vector index, including
 * the index configuration and vector record metadata schema.
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
     * @param metadata Vector record metadata schema
     * @param persist_threshold Persistence threshold (-1 for manual)
     * @param storage_base_path Base directory for index files (used to generate
     * file_path)
     */
    VectorIndexMetadata(std::string &&name,
                        IndexConfig &&config,
                        VectorRecordMetadata &&metadata,
                        int64_t persist_threshold,
                        const std::string &storage_base_path);
    ~VectorIndexMetadata() = default;

    VectorIndexMetadata(const VectorIndexMetadata &) = delete;
    VectorIndexMetadata(VectorIndexMetadata &&) = default;
    VectorIndexMetadata &operator=(const VectorIndexMetadata &) = delete;
    VectorIndexMetadata &operator=(VectorIndexMetadata &&) = default;

    /**
     * @brief Serialize metadata to binary format
     * @param str Output string to append encoded data
     */
    void Serialize(std::string &str) const;

    /**
     * @brief Deserialize metadata from binary format
     * @param buf Buffer containing encoded data
     * @param buff_size Size of the buffer
     * @param offset Current offset in buffer (updated after decoding)
     * @param version Version timestamp for created_ts initialization
     */
    void Deserialize(const char *buf,
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
    const VectorRecordMetadata &Metadata() const
    {
        return metadata_;
    }

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
    std::string name_{""};
    // Embedded algorithm configuration
    IndexConfig config_;
    // Vector record metadata
    VectorRecordMetadata metadata_;
    // Persistence threshold (-1 = manual)
    int64_t persist_threshold_{10000};
    // Full path to persisted index file (with timestamp)
    std::string file_path_{""};
    // Creation timestamp
    uint64_t created_ts_{0};
    // Last persistence timestamp
    uint64_t last_persist_ts_{0};
};

class VectorId
{
public:
    VectorId() = default;
    explicit VectorId(uint64_t id) : id_(id)
    {
    }
    VectorId(uint64_t id, std::vector<char> &&metadata)
        : id_(id), metadata_(std::move(metadata))
    {
    }
    ~VectorId() = default;

    VectorId(const VectorId &rhs) = default;
    VectorId(VectorId &&rhs) = default;
    VectorId &operator=(const VectorId &rhs) = default;
    VectorId &operator=(VectorId &&rhs) = default;

    bool operator==(const VectorId &other) const
    {
        return id_ == other.id_;
    }

    bool operator!=(const VectorId &other) const
    {
        return !(*this == other);
    }

    void Serialize(std::string &str) const;
    void Deserialize(const char *buf, size_t &offset);

    uint64_t id_{0};
    // Encoded meta data.
    std::vector<char> metadata_;
};

/**
 * @brief Result structure for vector search operations
 */
struct SearchResult
{
    std::vector<VectorId> ids;     ///< Vector IDs of the search results
    std::vector<float> distances;  ///< Distances to the query vector
    std::vector<std::vector<float>>
        vectors;  ///< Optional: actual vectors (if requested)

    SearchResult() = default;
    SearchResult(std::vector<VectorId> &&ids, std::vector<float> &&distances)
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
    METADATA_OP_FAILED,
    METADATA_FIELD_TYPE_MISMATCH,
    METADATA_FIELD_NOT_IN_SCHEMA,
    METADATA_FIELD_ALREADY_EXISTS,
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

struct CloudConfig
{
    explicit CloudConfig(const INIReader &config_reader);

    std::string endpoint_;
    std::string base_path_;
};

}  // namespace EloqVec