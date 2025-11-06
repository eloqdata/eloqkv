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

#include "tx_util.h"

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

void IndexConfig::Encode(std::string &encoded_str) const
{
    // dimension (8 bytes)
    encoded_str.append(reinterpret_cast<const char *>(&dimension),
                       sizeof(size_t));

    // max_elements (8 bytes)
    encoded_str.append(reinterpret_cast<const char *>(&max_elements),
                       sizeof(size_t));

    // algorithm (1 byte)
    encoded_str.append(reinterpret_cast<const char *>(&algorithm),
                       sizeof(uint8_t));

    // distance_metric (1 byte)
    encoded_str.append(reinterpret_cast<const char *>(&distance_metric),
                       sizeof(uint8_t));

    // params
    uint32_t param_count = static_cast<uint32_t>(params.size());
    encoded_str.append(reinterpret_cast<const char *>(&param_count),
                       sizeof(uint32_t));
    for (const auto &[key, value] : params)
    {
        uint32_t key_len = static_cast<uint32_t>(key.size());
        encoded_str.append(reinterpret_cast<const char *>(&key_len),
                           sizeof(uint32_t));
        encoded_str.append(key);

        uint32_t value_len = static_cast<uint32_t>(value.size());
        encoded_str.append(reinterpret_cast<const char *>(&value_len),
                           sizeof(uint32_t));
        encoded_str.append(value);
    }
}

void IndexConfig::Decode(const char *buf, size_t buff_size, size_t &offset)
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
// VectorIndexMetadata Implementation
// ============================================================================

VectorIndexMetadata::VectorIndexMetadata(const std::string &name,
                                         const IndexConfig &config,
                                         int64_t persist_threshold,
                                         const std::string &storage_base_path)
    : name_(name),
      config_(config),
      persist_threshold_(persist_threshold),
      created_ts_(0),
      last_persist_ts_(0)
{
    // Construct timestamped file path from base path
    file_path_ = storage_base_path;
    if (!file_path_.empty() && file_path_.back() != '/')
    {
        file_path_.append("/");
    }
    file_path_.append(name_)
        .append("-")
        .append(initial_timestamp_sv)
        .append(".index");
}

void VectorIndexMetadata::Encode(std::string &encoded_str) const
{
    // name (2 bytes length + data)
    uint16_t name_len = static_cast<uint16_t>(name_.size());
    encoded_str.append(reinterpret_cast<const char *>(&name_len),
                       sizeof(uint16_t));
    encoded_str.append(name_);

    // Encode embedded IndexConfig
    config_.Encode(encoded_str);

    // persist_threshold (8 bytes)
    encoded_str.append(reinterpret_cast<const char *>(&persist_threshold_),
                       sizeof(int64_t));

    // file_path (4 bytes length + data)
    uint32_t file_path_len = static_cast<uint32_t>(file_path_.size());
    encoded_str.append(reinterpret_cast<const char *>(&file_path_len),
                       sizeof(uint32_t));
    encoded_str.append(file_path_);

    // timestamps (8 bytes each)
    encoded_str.append(reinterpret_cast<const char *>(&created_ts_),
                       sizeof(uint64_t));
    encoded_str.append(reinterpret_cast<const char *>(&last_persist_ts_),
                       sizeof(uint64_t));
}

void VectorIndexMetadata::Decode(const char *buf,
                                 size_t buff_size,
                                 size_t &offset,
                                 uint64_t version)
{
    // name
    uint16_t name_len = *reinterpret_cast<const uint16_t *>(buf + offset);
    offset += sizeof(uint16_t);
    name_.assign(buf + offset, name_len);
    offset += name_len;

    // Decode embedded IndexConfig
    config_.Decode(buf, buff_size, offset);

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

}  // namespace EloqVec
