#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ds_request.pb.h"
#include "kv_store.h"
#include "redis_object.h"

#if KV_DATA_STORE_TYPE == KV_DATA_STORE_TYPE_ROCKSDB
#include <rocksdb/db.h>

#include "rocksdb/compaction_filter.h"
#endif
#if ROCKSDB_CLOUD_FS()
#include <rocksdb/cloud/cloud_storage_provider.h>
#endif

namespace EloqShare
{
std::string uint32_str_converter(uint32_t val);
std::string uint16_str_converter(uint16_t val);
std::string string_str_converter(const std::string &val);
std::string bool_str_converter(bool val);

uint32_t str_uint32_converter(const std::string &s);
uint16_t str_uint16_converter(const std::string &s);
std::string str_string_converter(const std::string &s);
bool str_bool_converter(const std::string &s);

template <typename T>
std::string SerializeVectorToString(
    const std::vector<T> &vec,
    const std::string &sep,
    std::function<std::string(const T &)> converter)
{
    std::string str;
    for (size_t i = 0; i < vec.size(); ++i)
    {
        str.append(converter(vec[i]));
        if (i < vec.size() - 1)
        {
            str.append(sep);
        }
    }
    return str;
}

template <typename T>
void DeserializeStringToVector(const std::string &str,
                               const std::string &sep,
                               std::vector<T> &vec,
                               std::function<T(const std::string &)> converter)
{
    if (str.empty())
    {
        return;  // Return early if the string is empty
    }

    std::string::size_type start = 0;
    std::string::size_type end = str.find(sep);
    while (end != std::string::npos)
    {
        vec.push_back(converter(str.substr(start, end - start)));
        start = end + sep.size();
        end = str.find(sep, start);
    }

    // Handle the last element or the case where str has only one element
    vec.push_back(converter(str.substr(start)));
}

::EloqDS::remote::UpsertTableOperationType UpsertTableOperationTypeConverter(
    txservice::OperationType op_type);

txservice::OperationType OperationTypeConverter(
    ::EloqDS::remote::UpsertTableOperationType op_type);

void ExtractNodesConfigs(
    const std::unordered_map<uint32_t,
                             std::vector<::EloqDS::remote::ClusterNodeConfig>>
        &ng_configs,
    std::vector<::EloqDS::remote::ClusterNodeConfig> &nodes);

#if KV_DATA_STORE_TYPE == KV_DATA_STORE_TYPE_ROCKSDB

rocksdb::InfoLogLevel StringToInfoLogLevel(const std::string &log_level_str);

void SerializeFlushRecord(const txservice::FlushRecord &flush_rec,
                          std::vector<char> &buf);
void SerializeFlushRecord(const txservice::FlushRecord &flush_rec,
                          std::string &rec_str);
class TTLCompactionFilter : public rocksdb::CompactionFilter
{
public:
    bool Filter(int level,
                const rocksdb::Slice &key,
                const rocksdb::Slice &existing_value,
                std::string *new_value,
                bool *value_changed) const override
    {
        // Ensure the value contains enough bytes to decode object type and the
        // ttl
        assert(existing_value.size() >=
               (1 /*delete*/ + sizeof(int64_t) /*commit_ts*/));

        bool is_deleted = existing_value[0];

        // Check if the 10th byte of the value matches
        // RedisObjectType::TTLString
        if (!existing_value.empty() && !is_deleted &&
            static_cast<EloqKV::RedisObjectType>(existing_value[9]) ==
                EloqKV::RedisObjectType::TTLString)
        {
            assert(existing_value.size() >= (10 + sizeof(uint64_t) /*ttl*/));
            uint64_t ttl;
            std::memcpy(&ttl, existing_value.data() + 10, sizeof(uint64_t));

            // Get the current timestamp in microseconds
            auto current_timestamp = txservice::LocalCcShards::ClockTs();

            // Check if the timestamp is smaller than the current timestamp
            if (ttl < current_timestamp)
            {
                return true;  // Mark the key for deletion
            }
        }

        return false;  // Keep the key
    }

    const char *Name() const override
    {
        return "TTLCompactionFilter";
    }
};

// RocksDBEventListener is used to listen the flush event of RocksDB for
// recording unexpected write slow and stall when flushing
class RocksDBEventListener : public rocksdb::EventListener
{
public:
    void OnCompactionBegin(rocksdb::DB *db,
                           const rocksdb::CompactionJobInfo &ci) override
    {
        DLOG(INFO) << "Compaction begin, job_id: " << ci.job_id
                   << " ,thread: " << ci.thread_id
                   << " ,output_level: " << ci.output_level
                   << " ,input_files_size: " << ci.input_files.size()
                   << " ,compaction_reason: "
                   << static_cast<int>(ci.compaction_reason);
    }

    void OnCompactionCompleted(rocksdb::DB *db,
                               const rocksdb::CompactionJobInfo &ci) override
    {
        DLOG(INFO) << "Compaction end, job_id: " << ci.job_id
                   << " ,thread: " << ci.thread_id
                   << " ,output_level: " << ci.output_level
                   << " ,input_files_size: " << ci.input_files.size()
                   << " ,compaction_reason: "
                   << static_cast<int>(ci.compaction_reason);
    }

    void OnFlushBegin(rocksdb::DB *db,
                      const rocksdb::FlushJobInfo &flush_job_info) override
    {
        if (flush_job_info.triggered_writes_slowdown ||
            flush_job_info.triggered_writes_stop)
        {
            LOG(INFO) << "Flush begin, file: " << flush_job_info.file_path
                      << " ,job_id: " << flush_job_info.job_id
                      << " ,thread: " << flush_job_info.thread_id
                      << " ,file_number: " << flush_job_info.file_number
                      << " ,triggered_writes_slowdown: "
                      << flush_job_info.triggered_writes_slowdown
                      << " ,triggered_writes_stop: "
                      << flush_job_info.triggered_writes_stop
                      << " ,smallest_seqno: " << flush_job_info.smallest_seqno
                      << " ,largest_seqno: " << flush_job_info.largest_seqno
                      << " ,flush_reason: "
                      << GetFlushReason(flush_job_info.flush_reason);
        }
    }

    void OnFlushCompleted(rocksdb::DB *db,
                          const rocksdb::FlushJobInfo &flush_job_info) override
    {
        if (flush_job_info.triggered_writes_slowdown ||
            flush_job_info.triggered_writes_stop)
        {
            LOG(INFO) << "Flush end, file: " << flush_job_info.file_path
                      << " ,job_id: " << flush_job_info.job_id
                      << " ,thread: " << flush_job_info.thread_id
                      << " ,file_number: " << flush_job_info.file_number
                      << " ,triggered_writes_slowdown: "
                      << flush_job_info.triggered_writes_slowdown
                      << " ,triggered_writes_stop: "
                      << flush_job_info.triggered_writes_stop
                      << " ,smallest_seqno: " << flush_job_info.smallest_seqno
                      << " ,largest_seqno: " << flush_job_info.largest_seqno
                      << " ,flush_reason: "
                      << GetFlushReason(flush_job_info.flush_reason);
        }
    }

    std::string GetFlushReason(rocksdb::FlushReason flush_reason)
    {
        switch (flush_reason)
        {
        case rocksdb::FlushReason::kOthers:
            return "kOthers";
        case rocksdb::FlushReason::kGetLiveFiles:
            return "kGetLiveFiles";
        case rocksdb::FlushReason::kShutDown:
            return "kShutDown";
        case rocksdb::FlushReason::kExternalFileIngestion:
            return "kExternalFileIngestion";
        case rocksdb::FlushReason::kManualCompaction:
            return "kManualCompaction";
        case rocksdb::FlushReason::kWriteBufferManager:
            return "kWriteBufferManager";
        case rocksdb::FlushReason::kWriteBufferFull:
            return "kWriteBufferFull";
        case rocksdb::FlushReason::kTest:
            return "kTest";
        case rocksdb::FlushReason::kDeleteFiles:
            return "kDeleteFiles";
        case rocksdb::FlushReason::kAutoCompaction:
            return "kAutoCompaction";
        case rocksdb::FlushReason::kManualFlush:
            return "kManualFlush";
        case rocksdb::FlushReason::kErrorRecovery:
            return "kErrorRecovery";
        case rocksdb::FlushReason::kErrorRecoveryRetryFlush:
            return "kErrorRecoveryRetryFlush";
        case rocksdb::FlushReason::kWalFull:
            return "kWalFull";
        default:
            return "unknown";
        }
    }
};

#if ROCKSDB_CLOUD_FS()
std::string MakeCloudManifestCookie(int64_t cc_ng_id, int64_t term);

std::string MakeCloudManifestFile(const std::string &dbname,
                                  int64_t cc_ng_id,
                                  int64_t term);

bool IsCloudManifestFile(const std::string &filename);

// Helper function to split a string by a delimiter
std::vector<std::string> SplitString(const std::string &str, char delimiter);

bool IsCloudManifestFile(const std::string &filename);

bool GetCookieFromCloudManifestFile(const std::string &filename,
                                    int64_t &cc_ng_id,
                                    int64_t &term);

int64_t FindMaxTermFromCloudManifestFiles(
    const std::shared_ptr<ROCKSDB_NAMESPACE::CloudStorageProvider>
        &storage_provider,
    const std::string &bucket_prefix,
    const std::string &bucket_name,
    const int64_t cc_ng_id_in_cookie);

void DeserializeRecord(const char *payload,
                       const size_t payload_size,
                       std::string &rec_str,
                       bool &is_deleted,
                       int64_t &version_ts);

void DeserializeToTxRecord(const char *payload,
                           const size_t payload_size,
                           txservice::TxRecord::Uptr &typed_rec,
                           bool &is_deleted,
                           int64_t &version_ts);
#endif

#endif

}  // namespace EloqShare
