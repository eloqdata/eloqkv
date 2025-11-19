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
#include "vector_type.h"

#include <gflags/gflags.h>

#include <algorithm>
#include <stdexcept>

#include "vector_util.h"

DEFINE_string(vector_cloud_endpoint, "", "vector cloud server endpoint");
DEFINE_string(vector_cloud_base_path,
              "vector-bucket",
              "vector cloud base path");
namespace EloqVec
{
inline bool CheckCommandLineFlagIsDefault(const char *name)
{
    gflags::CommandLineFlagInfo flag_info;

    bool flag_found = gflags::GetCommandLineFlagInfo(name, &flag_info);
    // Make sure the flag is declared.
    assert(flag_found);
    (void) flag_found;

    // Return `true` if the flag has the default value and has not been set
    // explicitly from the cmdline or via SetCommandLineOption
    return flag_info.is_default;
}
// ============================================================================
// IndexConfig Implementation
// ============================================================================

/**
 * Serialization format:
 * dimension (8 bytes)
 * | max_elements (8 bytes)
 * | algorithm (1 byte)
 * | distance_metric (1 byte)
 * | params_count (4 bytes) | [key_len (4 bytes) | key | value_len (4 bytes)
 * | value]*
 */
void IndexConfig::Serialize(std::string &str) const
{
    // dimension (8 bytes)
    str.append(reinterpret_cast<const char *>(&dimension), sizeof(size_t));

    // max_elements (8 bytes)
    str.append(reinterpret_cast<const char *>(&max_elements), sizeof(size_t));

    // algorithm (1 byte)
    str.append(reinterpret_cast<const char *>(&algorithm), sizeof(uint8_t));

    // distance_metric (1 byte)
    str.append(reinterpret_cast<const char *>(&distance_metric),
               sizeof(uint8_t));

    // params
    uint32_t param_count = static_cast<uint32_t>(params.size());
    str.append(reinterpret_cast<const char *>(&param_count), sizeof(uint32_t));
    for (const auto &[key, value] : params)
    {
        uint32_t key_len = static_cast<uint32_t>(key.size());
        str.append(reinterpret_cast<const char *>(&key_len), sizeof(uint32_t));
        str.append(key);

        uint32_t value_len = static_cast<uint32_t>(value.size());
        str.append(reinterpret_cast<const char *>(&value_len),
                   sizeof(uint32_t));
        str.append(value);
    }
}

void IndexConfig::Deserialize(const char *buf, size_t buff_size, size_t &offset)
{
    // dimension
    dimension = *reinterpret_cast<const size_t *>(buf + offset);
    offset += sizeof(size_t);

    // max_elements
    max_elements = *reinterpret_cast<const size_t *>(buf + offset);
    offset += sizeof(size_t);

    // algorithm
    algorithm = static_cast<Algorithm>(
        *reinterpret_cast<const uint8_t *>(buf + offset));
    offset += sizeof(uint8_t);

    // distance_metric
    distance_metric = static_cast<DistanceMetric>(
        *reinterpret_cast<const uint8_t *>(buf + offset));
    offset += sizeof(uint8_t);

    // params
    uint32_t param_count = *reinterpret_cast<const uint32_t *>(buf + offset);
    offset += sizeof(uint32_t);

    params.clear();
    for (uint32_t i = 0; i < param_count; ++i)
    {
        uint32_t key_len = *reinterpret_cast<const uint32_t *>(buf + offset);
        offset += sizeof(uint32_t);
        std::string key(buf + offset, key_len);
        offset += key_len;

        uint32_t value_len = *reinterpret_cast<const uint32_t *>(buf + offset);
        offset += sizeof(uint32_t);
        std::string value(buf + offset, value_len);
        offset += value_len;

        params.emplace(std::move(key), std::move(value));
    }
}

// ============================================================================
// VectorRecordMetadata Implementation
// ============================================================================

/**
 * Serialization format:
 * field_count (4 bytes)
 * | [field_name_len (4 bytes) | field_name | field_type (1 byte)]*
 */
void VectorRecordMetadata::Serialize(std::string &str) const
{
    // 1. Serialize field count
    uint32_t field_count = static_cast<uint32_t>(field_names_.size());
    str.append(reinterpret_cast<const char *>(&field_count), sizeof(uint32_t));

    // 2. Serialize each field (name + type)
    for (size_t i = 0; i < field_names_.size(); ++i)
    {
        // Serialize field name
        uint32_t name_len = static_cast<uint32_t>(field_names_[i].size());
        str.append(reinterpret_cast<const char *>(&name_len), sizeof(uint32_t));
        str.append(field_names_[i].data(), name_len);

        // Serialize field type
        uint8_t field_type = static_cast<uint8_t>(field_types_[i]);
        str.append(reinterpret_cast<const char *>(&field_type),
                   sizeof(uint8_t));
    }
}

void VectorRecordMetadata::Deserialize(const char *buf,
                                       size_t buff_size,
                                       size_t &offset)
{
    // 1. Deserialize field count
    uint32_t field_count = *reinterpret_cast<const uint32_t *>(buf + offset);
    offset += sizeof(uint32_t);

    // 2. Clear existing data and reserve space
    field_names_.clear();
    field_types_.clear();
    field_names_.reserve(field_count);
    field_types_.reserve(field_count);

    // 3. Deserialize each field (name + type)
    for (uint32_t i = 0; i < field_count; ++i)
    {
        // Deserialize field name
        uint32_t name_len = *reinterpret_cast<const uint32_t *>(buf + offset);
        offset += sizeof(uint32_t);
        std::string field_name(buf + offset, name_len);
        offset += name_len;

        // Deserialize field type
        MetadataFieldType field_type = static_cast<MetadataFieldType>(
            *reinterpret_cast<const uint8_t *>(buf + offset));
        offset += sizeof(uint8_t);

        // Add to vectors
        field_names_.push_back(std::move(field_name));
        field_types_.push_back(field_type);
    }
}

bool VectorRecordMetadata::Encode(const std::string_view &metadata_json,
                                  std::vector<char> &buf) const
{
    buf.clear();
    // Parse JSON
    nlohmann::ordered_json metadata_obj;
    try
    {
        metadata_obj = nlohmann::ordered_json::parse(metadata_json);
    }
    catch (const nlohmann::ordered_json::parse_error &e)
    {
        // Invalid JSON
        return false;
    }

    if (!metadata_obj.is_array() || metadata_obj.size() != field_names_.size())
    {
        // Metadata JSON must be an array and size must match the schema
        return false;
    }

    // Convert each field to binary using schema
    for (size_t idx = 0; idx < metadata_obj.size(); ++idx)
    {
        MetadataFieldType field_type = GetFieldType(idx);
        const nlohmann::json &json_value = metadata_obj[idx];

        // Convert JSON value to binary based on schema type
        if (!ParseJSONFieldValue(json_value, field_type, buf))
        {
            return false;
        }
    }
    return true;
}

template <>
void VectorRecordMetadata::Decode<std::string>(const std::vector<char> &buf,
                                               size_t index,
                                               std::string &value) const
{
    assert(index < field_types_.size() && "Index out of range");
    assert(field_types_[index] == MetadataFieldType::String &&
           "Field type must be string");
    value.clear();
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
    size_t str_len = *reinterpret_cast<const size_t *>(buf.data() + offset);
    std::copy(buf.data() + offset,
              buf.data() + offset + str_len,
              std::back_inserter(value));
}

void VectorRecordMetadata::Decode(const std::vector<char> &buf,
                                  std::vector<size_t> &offsets) const
{
    offsets.clear();
    offsets.reserve(field_types_.size());
    size_t offset = 0;
    for (size_t i = 0; i < field_types_.size(); ++i)
    {
        size_t field_length = GetFieldLength(i);
        if (field_types_[i] == MetadataFieldType::String)
        {
            field_length +=
                *reinterpret_cast<const size_t *>(buf.data() + offset);
        }
        offsets.push_back(offset);
        offset += field_length;
    }
    assert(offset == buf.size());
}

void VectorRecordMetadata::AddMetadataField(const std::string &field_name,
                                            MetadataFieldType field_type)
{
    // Check if field already exists
    if (HasMetadataField(field_name))
    {
        // Field already exists, don't add duplicate
        return;
    }

    field_names_.push_back(field_name);
    field_types_.push_back(field_type);
}

MetadataFieldType VectorRecordMetadata::GetFieldType(
    const std::string &field_name) const
{
    for (size_t i = 0; i < field_names_.size(); ++i)
    {
        if (field_names_[i] == field_name)
        {
            return field_types_[i];
        }
    }

    // Field not found, this should not happen if caller checks HasMetadataField
    // first
    throw std::runtime_error("Field not found: " + field_name);
}

size_t VectorRecordMetadata::GetFieldIndex(const std::string &field_name) const
{
    auto it = std::find(field_names_.begin(), field_names_.end(), field_name);
    if (it == field_names_.end())
    {
        return UINT64_MAX;  // Not found
    }
    return std::distance(field_names_.begin(), it);
}

size_t VectorRecordMetadata::GetFieldLength(size_t index) const
{
    assert(index < field_types_.size() && "Index out of range");
    switch (field_types_[index])
    {
    case MetadataFieldType::Int64:
    {
        return sizeof(int64_t);
    }
    case MetadataFieldType::Double:
    {
        return sizeof(double);
    }
    case MetadataFieldType::Bool:
    {
        return sizeof(uint8_t);
    }
    case MetadataFieldType::Int32:
    {
        return sizeof(int32_t);
    }
    case MetadataFieldType::String:
    {
        return sizeof(size_t);
    }
    default:
    {
        assert(false && "Unsupported field type");
        return 0;
    }
    }
}

// ============================================================================
// VectorIndexMetadata Implementation
// ============================================================================

VectorIndexMetadata::VectorIndexMetadata(std::string &&name,
                                         IndexConfig &&config,
                                         VectorRecordMetadata &&metadata,
                                         int64_t persist_threshold,
                                         const std::string &storage_base_path)
    : name_(std::move(name)),
      config_(std::move(config)),
      metadata_(std::move(metadata)),
      persist_threshold_(persist_threshold),
      created_ts_(0),
      last_persist_ts_(0)
{
    // Construct timestamped file path from base path
    file_path_.assign(storage_base_path);
    if (!file_path_.empty() && file_path_.back() != '/')
    {
        file_path_.append("/");
    }
    file_path_.append(name_)
        .append("-")
        .append(initial_timestamp_sv)
        .append(".index");
}

/**
 * Serialization format:
 * name_len (2 bytes) | name
 * | IndexConfig::Encode()
 * | VectorRecordMetadata::Encode()
 * | persist_threshold (8 bytes)
 * | file_path_len (4 bytes) | file_path
 * | created_ts (8 bytes)
 * | last_persist_ts (8 bytes)
 */
void VectorIndexMetadata::Serialize(std::string &str) const
{
    // name (2 bytes length + data)
    uint16_t name_len = static_cast<uint16_t>(name_.size());
    str.append(reinterpret_cast<const char *>(&name_len), sizeof(uint16_t));
    str.append(name_);

    // Encode embedded IndexConfig
    config_.Serialize(str);

    // Encode embedded VectorRecordMetadata
    metadata_.Serialize(str);

    // persist_threshold (8 bytes)
    str.append(reinterpret_cast<const char *>(&persist_threshold_),
               sizeof(int64_t));

    // file_path (4 bytes length + data)
    uint32_t file_path_len = static_cast<uint32_t>(file_path_.size());
    str.append(reinterpret_cast<const char *>(&file_path_len),
               sizeof(uint32_t));
    str.append(file_path_);

    // timestamps (8 bytes each)
    str.append(reinterpret_cast<const char *>(&created_ts_), sizeof(uint64_t));

    // last_persist_ts (8 bytes)
    str.append(reinterpret_cast<const char *>(&last_persist_ts_),
               sizeof(uint64_t));
}

void VectorIndexMetadata::Deserialize(const char *buf,
                                      size_t buff_size,
                                      size_t &offset,
                                      uint64_t version)
{
    // name
    uint16_t name_len = *reinterpret_cast<const uint16_t *>(buf + offset);
    offset += sizeof(uint16_t);
    name_.assign(buf + offset, name_len);
    offset += name_len;

    // Deserialize embedded IndexConfig
    config_.Deserialize(buf, buff_size, offset);

    // Deserialize embedded VectorRecordMetadata
    metadata_.Deserialize(buf, buff_size, offset);

    // persist_threshold
    persist_threshold_ = *reinterpret_cast<const int64_t *>(buf + offset);
    offset += sizeof(int64_t);

    // file_path
    uint32_t file_path_len = *reinterpret_cast<const uint32_t *>(buf + offset);
    offset += sizeof(uint32_t);
    file_path_.assign(buf + offset, file_path_len);
    offset += file_path_len;

    // timestamps
    created_ts_ = *reinterpret_cast<const uint64_t *>(buf + offset);
    created_ts_ = (created_ts_ == 0) ? version : created_ts_;
    offset += sizeof(uint64_t);

    // last_persist_ts
    last_persist_ts_ = *reinterpret_cast<const uint64_t *>(buf + offset);
    offset += sizeof(uint64_t);
}

// ============================================================================
// CloudConfig Implementation
// ============================================================================

CloudConfig::CloudConfig(const INIReader &config_reader)
{
    endpoint_ =
        !CheckCommandLineFlagIsDefault("vector_cloud_endpoint")
            ? FLAGS_vector_cloud_endpoint
            : config_reader.GetString("store", "vector_cloud_endpoint", "");
    base_path_ =
        !CheckCommandLineFlagIsDefault("vector_cloud_base_path")
            ? FLAGS_vector_cloud_base_path
            : config_reader.GetString("store", "vector_cloud_base_path", "");
}

// ============================================================================
// VectorId Implementation
// ============================================================================
/**
 * Serialization format:
 * id (8 bytes)
 * | metadata_len (4 bytes) | metadata
 */
void VectorId::Serialize(std::string &str) const
{
    // id (8 bytes)
    str.append(reinterpret_cast<const char *>(&id_), sizeof(uint64_t));

    // metadata_len (4 bytes)
    uint32_t metadata_len = static_cast<uint32_t>(metadata_.size());
    str.append(reinterpret_cast<const char *>(&metadata_len), sizeof(uint32_t));
    str.append(metadata_.data(), metadata_len);
}

void VectorId::Deserialize(const char *buf, size_t &offset)
{
    // id
    id_ = *reinterpret_cast<const uint64_t *>(buf + offset);
    offset += sizeof(uint64_t);

    // metadata_len (4 bytes)
    metadata_.clear();
    uint32_t metadata_len = *reinterpret_cast<const uint32_t *>(buf + offset);
    offset += sizeof(uint32_t);
    std::copy(buf + offset,
              buf + offset + metadata_len,
              std::back_inserter(metadata_));
    offset += metadata_len;
}
}  // namespace EloqVec

namespace std
{
template <>
struct hash<EloqVec::VectorId>
{
    std::size_t operator()(const EloqVec::VectorId &v) const noexcept
    {
        return std::hash<uint64_t>{}(v.id_);
    }
};
}  // namespace std
