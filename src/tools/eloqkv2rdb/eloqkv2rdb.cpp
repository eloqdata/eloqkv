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
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <ratio>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "bthread/moodycamelqueue.h"
#include "eloqkv_key.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "redis_hash_object.h"
#include "redis_list_object.h"
#include "redis_object.h"
#include "redis_set_object.h"
#include "redis_string_object.h"
#include "redis_zset_object.h"
#include "rocksdb/db.h"

#if defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_S3) ||                       \
    defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_GCS)
#define ROCKSDB_CLOUD_EXPORT 1
#endif

#if ROCKSDB_CLOUD_EXPORT
#if defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_S3)
#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/s3/S3Client.h>
#endif
#include <rocksdb/cloud/cloud_file_system.h>
#include <rocksdb/cloud/cloud_storage_provider.h>
#include <rocksdb/cloud/db_cloud.h>
#include <rocksdb/env.h>
#include <rocksdb/options.h>

#include <algorithm>
#include <sstream>
#endif
extern "C"
{
#include "crcspeed/crc64speed.h"
}

DEFINE_string(rocksdb_path, "", "The RocksDB data (full) path");
DEFINE_string(output_file, "dump.rdb", "The file to store the RDB file");
DEFINE_uint32(thread_count,
              1,
              "The number of thread to parse records from rocksdb");

DEFINE_uint32(round_batch_size,
              10000,
              "The number of keys to parse in one round");

DEFINE_uint32(pre_read_ratio,
              2,
              "Pre read batch count for each parsing thread");

#if ROCKSDB_CLOUD_EXPORT
DEFINE_uint32(shard_num, 1, "Number of shards to export");
DEFINE_string(db_path,
              "/tmp/eloqkv_rdb_export",
              "Local cache directory for RocksDB Cloud");
DECLARE_string(aws_access_key_id);
DECLARE_string(aws_secret_key);
DECLARE_string(eloq_dss_branch_name);
DECLARE_string(rocksdb_cloud_bucket_name);
DECLARE_string(rocksdb_cloud_bucket_prefix);
DECLARE_string(rocksdb_cloud_object_path);
DECLARE_string(rocksdb_cloud_region);
DECLARE_string(rocksdb_cloud_s3_endpoint_url);
DECLARE_string(rocksdb_cloud_sst_file_cache_size);
DECLARE_int32(rocksdb_cloud_sst_file_cache_num_shard_bits);
#endif

// Compression not supported yet
// DEFINE_uint32(compress_threshold,
//               4,
//               "Threshold to compress string. Default for Redis is 4");

namespace EloqKV
{
namespace Tools
{
const static std::regex number_pattern("^-?\\d+$");
class RedisRdbUtil
{
public:
    static void WriteUInt32BE(uint32_t val, std::string &output_buf)
    {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        val = ((val << 24) & 0xFF000000) | ((val << 8) & 0x00FF0000) |
              ((val >> 8) & 0x0000FF00) | ((val >> 24) & 0x000000FF);
#endif
        output_buf.append(reinterpret_cast<char *>(&val), sizeof(val));
    }

    static void OutputString(const std::string &s, std::string &output_buf)
    {
        // Check if string is a number using regex
        if (std::regex_match(s, number_pattern))
        {
            int32_t n = std::stol(s);
            if (n >= -128 && n <= 127)
            {
                OutputLengthEncoding(0, true, output_buf);
                int8_t val = static_cast<int8_t>(n);
                output_buf.push_back(val);
                return;
            }
            else if (n >= -32768 && n <= 32767)
            {
                OutputLengthEncoding(1, true, output_buf);
                output_buf.append(reinterpret_cast<char *>(&n), sizeof(n));
                return;
            }
            else if (n >= -2147483648 && n <= 2147483647)
            {
                OutputLengthEncoding(2, true, output_buf);
                output_buf.append(reinterpret_cast<char *>(&n), sizeof(n));
                return;
            }
        }

        // Not a number or too big
        // if (s.size() > FLAGS_compress_threshold)
        // {
        //     std::string compressed = CompressString(s);
        //     if (compressed.length() < s.length())
        //     {
        //         // Compression saved space
        //         OutputLengthEncoding(3, true, output_buf);
        //         OutputLengthEncoding(compressed.length(), false, output_buf);
        //         OutputLengthEncoding(s.length(), false, output_buf);
        //         output_buf.append(compressed);
        //         return;
        //     }
        // }

        OutputLengthEncoding(s.size(), false, output_buf);
        output_buf.append(s);
    }

    static void OutputLengthEncoding(uint32_t n,
                                     bool special,
                                     std::string &output_buf)
    {
        if (!special)
        {
            if (n <= 0x3F)
            {
                // smaller than 63
                output_buf.push_back(static_cast<char>(n));
            }
            else if (n <= 0x3FFF)
            {
                output_buf.push_back(static_cast<char>(0x40 | (n >> 8)));
                output_buf.push_back(static_cast<char>(n & 0xFF));
            }
            else
            {
                output_buf.push_back(static_cast<char>(0x80));
                WriteUInt32BE(n, output_buf);
            }
        }
        else
        {
            if (n > 0x3F)
            {
                throw std::runtime_error("Cannot encode " + std::to_string(n) +
                                         " using special length encoding");
            }
            output_buf.push_back(static_cast<char>(0xC0 | n));
        }
    }

    static void ParseHeader(std::string &output_buf)
    {
        std::string header = "REDIS0006";
        output_buf.append(header);
    }

    static void ParseEnd(std::string &output_buf)
    {
        output_buf.push_back('\xFF');
    }

    static void ParseSelectDB(int db_idx, std::string &output_buf)
    {
        output_buf.push_back('\xFE');
        OutputLengthEncoding(db_idx, false, output_buf);
    }

    static void OutputTTL(uint64_t ttl, std::string &output_buf)
    {
        // we have no way of knowing if the TTL was set at
        // second level or millisecond level. Treat it as second level ttl.
        output_buf.push_back('\xFD');
        uint32_t expire = ttl / 1000;

        output_buf.append(reinterpret_cast<char *>(&expire), sizeof(expire));
    }

    void ParseEloqKV(const EloqKey *key,
                     const RedisEloqObject *obj,
                     std::string &output_buf)
    {
        if (obj->HasTTL())
        {
            OutputTTL(obj->GetTTL(), output_buf);
        }
        switch (obj->ObjectType())
        {
        case RedisObjectType::String:
        case RedisObjectType::TTLString:
        {
            const RedisStringObject *typed_obj =
                static_cast<const RedisStringObject *>(obj);
            output_buf.push_back(0);
            buf_.assign(key->StringView());
            OutputString(buf_, output_buf);
            buf_.assign(typed_obj->StringView());
            OutputString(buf_, output_buf);
            break;
        }

        case RedisObjectType::List:
        case RedisObjectType::TTLList:
        {
            const RedisListObject *typed_obj =
                static_cast<const RedisListObject *>(obj);

            output_buf.push_back(1);
            const std::deque<EloqString> &elements = typed_obj->Elements();
            buf_.assign(key->StringView());
            OutputString(buf_, output_buf);
            OutputLengthEncoding(elements.size(), false, output_buf);
            for (const EloqString &value : elements)
            {
                OutputString(value.String(), output_buf);
            }

            break;
        }
        case RedisObjectType::Hash:
        case RedisObjectType::TTLHash:
        {
            const RedisHashObject *typed_obj =
                static_cast<const RedisHashObject *>(obj);
            output_buf.push_back(4);
            buf_.assign(key->StringView());
            OutputString(buf_, output_buf);

            const absl::flat_hash_map<EloqString, EloqString> &elements =
                typed_obj->Elements();
            OutputLengthEncoding(elements.size(), false, output_buf);

            for (auto &[sub_key, sub_value] : elements)
            {
                OutputString(sub_key.String(), output_buf);
                OutputString(sub_value.String(), output_buf);
            }

            break;
        }
        case RedisObjectType::Set:
        case RedisObjectType::TTLSet:
        {
            const RedisHashSetObject *typed_obj =
                static_cast<const RedisHashSetObject *>(obj);
            output_buf.push_back(2);
            buf_.assign(key->StringView());
            OutputString(buf_, output_buf);
            const auto &elements = typed_obj->Elements();
            OutputLengthEncoding(elements.size(), false, output_buf);
            for (const EloqString &sub_key : elements)
            {
                OutputString(sub_key.String(), output_buf);
            }

            break;
        }
        case RedisObjectType::Zset:
        case RedisObjectType::TTLZset:
        {
            const RedisZsetObject *typed_obj =
                static_cast<const RedisZsetObject *>(obj);
            output_buf.push_back(3);
            buf_.assign(key->StringView());
            OutputString(buf_, output_buf);
            const absl::flat_hash_map<std::string_view, double> &elements =
                typed_obj->Elements();
            OutputLengthEncoding(elements.size(), false, output_buf);

            for (auto &[sub_key_sv, score] : elements)
            {
                buf_.assign(sub_key_sv);
                OutputString(buf_, output_buf);
                if (std::isnan(score))
                {
                    output_buf.push_back('\xFD');
                }
                else if (std::isinf(score))
                {
                    if (score < 0)
                    {
                        output_buf.push_back('\xFF');
                    }
                    else
                    {
                        output_buf.push_back('\xFE');
                    }
                }
                else
                {
                    OutputString(std::to_string(score), output_buf);
                }
            }

            break;
        }
        default:
            assert(false);
            break;
        }
    }

    std::string buf_;
};
struct KvEntry
{
    uint16_t db_idx_;
    std::string key_str_;
    std::string payload_str_;
    bool is_deleted_;
    int64_t version_ts_;
};

const uint32_t WriteBlockSize = 1000 * 1000;

using BatchKvEntry = std::vector<KvEntry>;
std::vector<BatchKvEntry> entry_pool;
std::vector<std::string> write_buf_pool;

std::mutex rocksdb_reader_mux;
std::condition_variable rocksdb_reader_cv;

std::mutex writer_mux;
std::condition_variable writer_cv;

std::mutex parser_mux;
std::condition_variable parser_cv;

moodycamel::ConcurrentQueue<BatchKvEntry *> free_parse_tasks;
moodycamel::ConcurrentQueue<BatchKvEntry *> parse_tasks;
moodycamel::ConcurrentQueue<std::string *> flush_tasks;
moodycamel::ConcurrentQueue<std::string *> free_flush_tasks;

// mutex to write file and update crc checksum
std::mutex write_mux;
uint64_t crc_val = 0;
std::ofstream outfile;

// count of key read from rocksdb
std::atomic_uint64_t stat_read_key_count;
// count of key written to rdb
std::atomic_uint64_t stat_written_key_count;

inline bool FindFlagInfo(const std::vector<google::CommandLineFlagInfo> &flags,
                         const std::string &name,
                         google::CommandLineFlagInfo &info)
{
    for (const auto &flag : flags)
    {
        if (flag.name == name)
        {
            info = flag;
            return true;
        }
    }
    return false;
}

inline bool IsFlagDefault(const std::string &name)
{
    google::CommandLineFlagInfo info;
    return !google::GetCommandLineFlagInfo(name.c_str(), &info) ||
           info.is_default;
}

inline void PrintSelectedFlag(
    const std::vector<google::CommandLineFlagInfo> &flags,
    const std::string &name,
    const char *description_override = nullptr,
    bool required = false,
    bool show_default = true)
{
    google::CommandLineFlagInfo flag;
    if (!FindFlagInfo(flags, name, flag))
    {
        return;
    }

    const std::string description = description_override != nullptr
                                        ? description_override
                                        : flag.description;
    std::cout << "    --" << flag.name << " (" << description
              << ") type: " << flag.type;
    if (required)
    {
        std::cout << " required";
    }
    else if (show_default)
    {
        std::cout << " default: ";
        if (flag.type == "string")
        {
            std::cout << "\"" << flag.default_value << "\"";
        }
        else
        {
            std::cout << flag.default_value;
        }
    }
    std::cout << std::endl;
}

inline void PrintToolHelp(const char *argv0)
{
    std::vector<google::CommandLineFlagInfo> flags;
    google::GetAllFlags(&flags);

    std::cout << "Usage: " << argv0 << " [options]" << std::endl << std::endl;

#if ROCKSDB_CLOUD_EXPORT
    std::cout << "RocksDB Cloud export flags:" << std::endl;
    PrintSelectedFlag(flags, "aws_access_key_id");
    PrintSelectedFlag(flags, "aws_secret_key");
    PrintSelectedFlag(flags, "rocksdb_cloud_region");
    PrintSelectedFlag(
        flags,
        "rocksdb_cloud_s3_endpoint_url",
        "Optional for AWS S3. Set it for S3-compatible object stores such as "
        "MinIO, for example http://127.0.0.1:9900");
    PrintSelectedFlag(
        flags, "rocksdb_cloud_bucket_prefix", nullptr, true, false);
    PrintSelectedFlag(flags, "rocksdb_cloud_bucket_name", nullptr, true, false);
    PrintSelectedFlag(flags, "rocksdb_cloud_object_path", nullptr, true, false);
    PrintSelectedFlag(flags, "rocksdb_cloud_sst_file_cache_size");
    PrintSelectedFlag(
        flags,
        "rocksdb_cloud_sst_file_cache_num_shard_bits",
        "SST file cache shard bits. The cache is split into 2^N shards to "
        "reduce lock contention; 5 means 32 shards. Usually no need to change");
    PrintSelectedFlag(flags, "eloq_dss_branch_name");
    PrintSelectedFlag(flags, "shard_num");
    PrintSelectedFlag(flags, "db_path");
    PrintSelectedFlag(flags, "output_file");
#else
    std::cout << "RocksDB export flags:" << std::endl;
    const std::vector<std::string> flag_names = {
        "rocksdb_path",
        "output_file",
        "thread_count",
        "round_batch_size",
        "pre_read_ratio",
    };

    for (const auto &name : flag_names)
    {
        PrintSelectedFlag(flags, name);
    }
#endif

    std::cout << std::endl
              << "Use --helpfull to show all linked gflags." << std::endl;
}

inline bool IsToolHelpArg(const char *arg)
{
    const std::string value(arg);
    return value == "--help" || value == "-help" || value == "--help=true" ||
           value == "-help=true" || value == "--help=1" || value == "-help=1";
}

inline bool MaybeHandleToolHelp(int argc, char *argv[])
{
    for (int idx = 1; idx < argc; idx++)
    {
        if (IsToolHelpArg(argv[idx]))
        {
            PrintToolHelp(argv[0]);
            return true;
        }
    }
    return false;
}

#if ROCKSDB_CLOUD_EXPORT
inline bool ValidateRequiredCloudFlag(const std::string &name,
                                      const std::string &value)
{
    if (IsFlagDefault(name) || value.empty())
    {
        std::cerr << "Please specify non-empty --" << name << std::endl;
        return false;
    }
    return true;
}

inline bool ValidateRequiredCloudFlags()
{
    return ValidateRequiredCloudFlag("rocksdb_cloud_bucket_prefix",
                                     FLAGS_rocksdb_cloud_bucket_prefix) &&
           ValidateRequiredCloudFlag("rocksdb_cloud_bucket_name",
                                     FLAGS_rocksdb_cloud_bucket_name) &&
           ValidateRequiredCloudFlag("rocksdb_cloud_object_path",
                                     FLAGS_rocksdb_cloud_object_path);
}
#endif

struct WriteWorker
{
    explicit WriteWorker()
    {
        thd_ = std::thread([this] { Run(); });
    }

    void Terminate()
    {
        terminated_.store(true, std::memory_order_relaxed);
    }

    void Join()
    {
        thd_.join();
    }
    void Run()
    {
        std::string *write_bufs[100];

        // acquire write lock to write to file and update crc val.
        std::unique_lock<std::mutex> write_lk(write_mux);
        while (true)
        {
            size_t task_cnt;
            while ((task_cnt = flush_tasks.try_dequeue_bulk(write_bufs, 100)) ==
                   0)
            {
                if (terminated_.load(std::memory_order_relaxed))
                {
                    // Flush worker is only terminated after all parse worker is
                    // joined. So if the flush task queue is empty, then we've
                    // finished all tasks.
                    return;
                }

                std::unique_lock<std::mutex> lk(writer_mux);
                writer_cv.wait_for(lk, std::chrono::milliseconds(100));
            }

            size_t buf_idx = 0;
            while (buf_idx < task_cnt)
            {
                outfile << *write_bufs[buf_idx];
                crc_val = crc64speed(crc_val,
                                     write_bufs[buf_idx]->data(),
                                     write_bufs[buf_idx]->size());
                write_bufs[buf_idx]->clear();
                free_flush_tasks.enqueue(write_bufs[buf_idx]);
                buf_idx++;
                parser_cv.notify_all();
            }
        }
    }

    std::atomic_bool terminated_{false};
    std::thread thd_;
};

struct ParseWorker
{
    explicit ParseWorker()
    {
        thd_ = std::thread([this] { Run(); });
    }

    void Terminate()
    {
        terminated_.store(true, std::memory_order_relaxed);
    }

    void Join()
    {
        thd_.join();
    }

    void Run()
    {
        uint64_t clock_ts =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();

        size_t written_key_count = 0;
        std::string *output_buf = nullptr;

        while (true)
        {
            while (!parse_tasks.try_dequeue(entry_vector_))
            {
                if (terminated_.load(std::memory_order_relaxed))
                {
                    if (output_buf && !output_buf->empty())
                    {
                        flush_tasks.enqueue(output_buf);
                        writer_cv.notify_one();
                        output_buf = nullptr;
                    }

                    stat_written_key_count.fetch_add(written_key_count);
                    return;
                }

                std::unique_lock<std::mutex> lk(parser_mux);
                parser_cv.wait_for(lk, std::chrono::milliseconds(100));
            }
            for (size_t idx = 0; entry_vector_ && idx < entry_vector_->size();
                 idx++)
            {
                if (output_buf == nullptr)
                {
                    while (!free_flush_tasks.try_dequeue(output_buf))
                    {
                        std::unique_lock<std::mutex> lk(parser_mux);
                        parser_cv.wait_for(lk, std::chrono::milliseconds(100));
                    }
                }
                KvEntry &entry = entry_vector_->at(idx);
                EloqKey eloq_key(entry.key_str_.data(), entry.key_str_.size());
                txservice::TxRecord::Uptr eloq_rec;

                DeserializeToTxRecord(entry.payload_str_.data(),
                                      entry.payload_str_.size(),
                                      eloq_rec,
                                      entry.is_deleted_,
                                      entry.version_ts_);

                if (entry.is_deleted_ ||
                    IsRecordTTLExpired(eloq_rec.get(), clock_ts))
                {
                    entry.is_deleted_ = true;
                    continue;
                }

                util_.ParseEloqKV(
                    &eloq_key,
                    static_cast<RedisEloqObject *>(eloq_rec.get()),
                    *output_buf);

                written_key_count++;
                if (output_buf->size() >= WriteBlockSize)
                {
                    flush_tasks.enqueue(output_buf);
                    writer_cv.notify_one();
                    output_buf = nullptr;
                }
            }

            if (output_buf && !output_buf->empty())
            {
                flush_tasks.enqueue(output_buf);
                writer_cv.notify_one();
                output_buf = nullptr;
            }

            free_parse_tasks.enqueue(entry_vector_);
            rocksdb_reader_cv.notify_one();

            entry_vector_ = nullptr;
        }
    }

    void DeserializeToTxRecord(const char *payload,
                               const size_t payload_size,
                               txservice::TxRecord::Uptr &typed_rec,
                               bool &is_deleted,
                               int64_t &version_ts)
    {
        assert(payload_size >= (sizeof(int8_t) + sizeof(int64_t)));
        const char *p = payload;
        int8_t deleted = 0;
        std::memcpy(&deleted, p, sizeof(int8_t));
        p += sizeof(int8_t);
        int64_t version = 0;
        std::memcpy(&version, p, sizeof(int64_t));
        p += sizeof(int64_t);
        version_ts = version;
        size_t offset = 0;
        if (deleted == 0)
        {
            is_deleted = false;
            int8_t obj_type_int8 = static_cast<int8_t>(*p);
            RedisObjectType obj_type =
                static_cast<RedisObjectType>(obj_type_int8);
            switch (obj_type)
            {
            case RedisObjectType::String:
                typed_rec.reset(new RedisStringObject());
                break;
            case RedisObjectType::List:
                typed_rec.reset(new RedisListObject());
                break;
            case RedisObjectType::Hash:
                typed_rec.reset(new RedisHashObject());
                break;
            case RedisObjectType::Zset:
                typed_rec.reset(new RedisZsetObject());
                break;
            case RedisObjectType::Set:
                typed_rec.reset(new RedisHashSetObject());
                break;
            case RedisObjectType::TTLString:
                typed_rec.reset(new RedisStringTTLObject());
                break;
            case RedisObjectType::TTLSet:
                typed_rec.reset(new RedisHashSetTTLObject());
                break;
            case RedisObjectType::TTLHash:
                typed_rec.reset(new RedisHashTTLObject());
                break;
            case RedisObjectType::TTLList:
                typed_rec.reset(new RedisListTTLObject());
                break;
            case RedisObjectType::TTLZset:
                typed_rec.reset(new RedisZsetTTLObject());
                break;
            default:
                assert(false);
            }
            typed_rec->Deserialize(p, offset);
        }
        else
        {
            is_deleted = true;
        }
    }

    bool IsRecordTTLExpired(const txservice::TxRecord *rec, uint64_t ts_base)
    {
        if (rec != nullptr && rec->HasTTL())
        {
            if (rec->GetTTL() < (ts_base / 1000))
            {
                return true;
            }
        }

        return false;
    }

private:
    std::atomic_bool terminated_{false};
    std::vector<KvEntry> *entry_vector_{nullptr};

    RedisRdbUtil util_;

    std::thread thd_;
};

#if ROCKSDB_CLOUD_EXPORT

struct S3UrlComponents
{
    std::string protocol;
    std::string bucket_name;
    std::string object_path;
    std::string endpoint_url;
    bool is_valid{false};
    std::string error_message;
};

inline std::string LowerString(std::string s)
{
    std::transform(s.begin(),
                   s.end(),
                   s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

inline S3UrlComponents ParseS3Url(const std::string &s3_url)
{
    S3UrlComponents result;

    if (s3_url.empty())
    {
        result.error_message = "Object store URL is empty";
        return result;
    }

    size_t protocol_end = s3_url.find("://");
    if (protocol_end == std::string::npos)
    {
        result.error_message = "Invalid URL format: missing '://' separator";
        return result;
    }

    result.protocol = LowerString(s3_url.substr(0, protocol_end));

    if (result.protocol != "s3" && result.protocol != "gs" &&
        result.protocol != "http" && result.protocol != "https")
    {
        result.error_message = "Invalid protocol '" + result.protocol +
                               "'. Must be one of: s3, gs, http, https";
        return result;
    }

    size_t path_start = protocol_end + 3;
    if (path_start >= s3_url.length())
    {
        result.error_message = "Invalid URL format: no content after protocol";
        return result;
    }

    std::string remaining = s3_url.substr(path_start);

    if (result.protocol == "http" || result.protocol == "https")
    {
        size_t first_slash = remaining.find('/');
        if (first_slash == std::string::npos)
        {
            result.error_message =
                "Invalid URL: missing bucket and object path";
            return result;
        }
        result.endpoint_url =
            result.protocol + "://" + remaining.substr(0, first_slash);
        remaining = remaining.substr(first_slash + 1);
    }

    size_t first_slash = remaining.find('/');
    if (first_slash == std::string::npos)
    {
        result.error_message = "Invalid URL: missing object path";
        return result;
    }

    result.bucket_name = remaining.substr(0, first_slash);
    result.object_path = remaining.substr(first_slash + 1);

    if (result.bucket_name.empty() || result.object_path.empty())
    {
        result.error_message = "Bucket name or object path cannot be empty";
        return result;
    }

    result.is_valid = true;
    return result;
}

inline std::string BuildShardObjectPath(const std::string &object_path,
                                        uint32_t shard_id)
{
    std::string path = object_path;
    while (!path.empty() && path.back() == '/')
    {
        path.pop_back();
    }

    const std::string shard_suffix = "/ds_" + std::to_string(shard_id);
    if (path.size() >= shard_suffix.size() &&
        path.compare(path.size() - shard_suffix.size(),
                     shard_suffix.size(),
                     shard_suffix) == 0)
    {
        return path;
    }

    return path + shard_suffix;
}

inline std::string TrimTrailingSlashes(std::string value)
{
    while (!value.empty() && value.back() == '/')
    {
        value.pop_back();
    }
    return value;
}

inline std::string TrimLeadingSlashes(std::string value)
{
    while (!value.empty() && value.front() == '/')
    {
        value.erase(value.begin());
    }
    return value;
}

inline std::string BuildObjectStoreServiceUrl()
{
    std::string bucket_prefix = FLAGS_rocksdb_cloud_bucket_prefix;
    std::string bucket_name = FLAGS_rocksdb_cloud_bucket_name;
    std::string bucket = bucket_prefix + bucket_name;
    std::string object_path =
        TrimLeadingSlashes(FLAGS_rocksdb_cloud_object_path);
    if (bucket_prefix.empty() || bucket_name.empty() || object_path.empty())
    {
        return "";
    }

    if (!FLAGS_rocksdb_cloud_s3_endpoint_url.empty())
    {
        return TrimTrailingSlashes(FLAGS_rocksdb_cloud_s3_endpoint_url) + "/" +
               bucket + "/" + object_path;
    }

#if defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_GCS)
    return "gs://" + bucket + "/" + object_path;
#else
    return "s3://" + bucket + "/" + object_path;
#endif
}

inline std::string FormatElapsed(uint64_t elapsed_secs)
{
    const uint64_t hours = elapsed_secs / 3600;
    const uint64_t minutes = (elapsed_secs % 3600) / 60;
    const uint64_t seconds = elapsed_secs % 60;

    std::ostringstream ss;
    ss << std::setfill('0') << std::setw(2) << hours << ":" << std::setw(2)
       << minutes << ":" << std::setw(2) << seconds;
    return ss.str();
}

inline std::string FormatKeyRate(double rate)
{
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(rate >= 1000 ? 1 : 0) << rate;
    return ss.str();
}

class ShardProgressPrinter
{
public:
    explicit ShardProgressPrinter(uint32_t shard_id)
        : shard_id_(shard_id),
          start_time_(Clock::now()),
          last_print_time_(start_time_)
    {
    }

    ~ShardProgressPrinter()
    {
        StopThread();
    }

    void Start(const char *phase)
    {
        SetPhase(phase);
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true))
        {
            return;
        }
        thd_ = std::thread([this] { Run(); });
    }

    void SetPhase(const char *phase)
    {
        phase_.store(phase, std::memory_order_relaxed);
    }

    void SetCounts(uint64_t read_key_count, uint64_t exported_key_count)
    {
        read_key_count_.store(read_key_count, std::memory_order_relaxed);
        exported_key_count_.store(exported_key_count,
                                  std::memory_order_relaxed);
    }

    void Finish(const char *final_status = "done")
    {
        StopThread();
        Print(Clock::now(), final_status);
        std::cout << std::endl;
    }

private:
    using Clock = std::chrono::steady_clock;

    void Run()
    {
        Print(Clock::now(), nullptr);

        std::unique_lock<std::mutex> lk(refresh_mux_);
        while (running_.load(std::memory_order_relaxed))
        {
            bool stopped = refresh_cv_.wait_for(
                lk,
                std::chrono::seconds(1),
                [this] { return !running_.load(std::memory_order_relaxed); });
            if (stopped)
            {
                break;
            }

            lk.unlock();
            Print(Clock::now(), nullptr);
            lk.lock();
        }
    }

    void StopThread()
    {
        bool expected = true;
        if (running_.compare_exchange_strong(expected, false))
        {
            refresh_cv_.notify_all();
        }
        if (thd_.joinable())
        {
            thd_.join();
        }
    }

    void Print(Clock::time_point now, const char *final_status)
    {
        double interval_secs =
            std::chrono::duration<double>(now - last_print_time_).count();
        double elapsed_secs =
            std::chrono::duration<double>(now - start_time_).count();
        uint64_t read_key_count =
            read_key_count_.load(std::memory_order_relaxed);
        uint64_t exported_key_count =
            exported_key_count_.load(std::memory_order_relaxed);
        double rate = 0;
        if (interval_secs > 0)
        {
            rate =
                (exported_key_count - last_exported_key_count_) / interval_secs;
        }
        double avg_rate = 0;
        if (elapsed_secs > 0)
        {
            avg_rate = exported_key_count / elapsed_secs;
        }

        std::ostringstream ss;
        ss << "shard " << shard_id_;
        if (final_status != nullptr)
        {
            ss << " " << final_status;
        }
        else
        {
            ss << " | " << phase_.load(std::memory_order_relaxed);
        }
        ss << " | elapsed "
           << FormatElapsed(static_cast<uint64_t>(elapsed_secs))
           << " | exported " << exported_key_count;
        if (final_status != nullptr)
        {
            ss << " | avg " << FormatKeyRate(avg_rate) << " keys/s";
        }
        else
        {
            ss << " | rate " << FormatKeyRate(rate) << " keys/s";
        }
        ss << " | read " << read_key_count;

        std::string line = ss.str();
        std::cout << '\r' << line;
        if (last_line_size_ > line.size())
        {
            std::cout << std::string(last_line_size_ - line.size(), ' ');
        }
        std::cout.flush();

        last_line_size_ = line.size();
        last_print_time_ = now;
        last_exported_key_count_ = exported_key_count;
    }

    uint32_t shard_id_;
    Clock::time_point start_time_;
    Clock::time_point last_print_time_;
    std::atomic<const char *> phase_{"starting"};
    std::atomic_bool running_{false};
    std::atomic_uint64_t read_key_count_{0};
    std::atomic_uint64_t exported_key_count_{0};
    std::mutex refresh_mux_;
    std::condition_variable refresh_cv_;
    std::thread thd_;
    uint64_t last_exported_key_count_{0};
    size_t last_line_size_{0};
};

inline std::string MakeCloudManifestCookie(const std::string &branch_name,
                                           int64_t dss_shard_id,
                                           int64_t term)
{
    if (branch_name.empty())
    {
        return std::to_string(dss_shard_id) + "-" + std::to_string(term);
    }

    return branch_name + "-" + std::to_string(dss_shard_id) + "-" +
           std::to_string(term);
}

inline bool ParseCloudManifestFileName(const std::string &filename,
                                       std::string &branch_name,
                                       int64_t &dss_shard_id,
                                       int64_t &term)
{
    const std::string prefix = "CLOUDMANIFEST";
    const size_t pos = filename.rfind('/');
    std::string manifest_part =
        (pos == std::string::npos) ? filename : filename.substr(pos + 1);

    if (manifest_part.rfind(prefix, 0) != 0)
    {
        return false;
    }

    std::string suffix = manifest_part.substr(prefix.size());
    if (suffix.empty())
    {
        branch_name.clear();
        dss_shard_id = -1;
        term = -1;
        return true;
    }

    if (suffix[0] != '-' || suffix.size() == 1)
    {
        return false;
    }
    suffix.erase(0, 1);

    const size_t last_hyphen = suffix.rfind('-');
    if (last_hyphen == std::string::npos)
    {
        branch_name = suffix;
        dss_shard_id = -1;
        term = -1;
        return true;
    }

    const size_t second_last_hyphen = suffix.rfind('-', last_hyphen - 1);
    try
    {
        if (second_last_hyphen != std::string::npos)
        {
            branch_name = suffix.substr(0, second_last_hyphen);
            dss_shard_id = std::stoll(suffix.substr(
                second_last_hyphen + 1, last_hyphen - second_last_hyphen - 1));
        }
        else
        {
            branch_name.clear();
            dss_shard_id = std::stoll(suffix.substr(0, last_hyphen));
        }
        term = std::stoll(suffix.substr(last_hyphen + 1));
    }
    catch (...)
    {
        return false;
    }

    return true;
}

inline bool FindLatestCloudManifest(
    const std::shared_ptr<rocksdb::CloudStorageProvider> &storage_provider,
    const std::string &bucket_name,
    const std::string &object_path,
    const std::string &branch_name,
    std::string &cookie_on_open,
    std::string &error_message)
{
    constexpr int64_t kDssShardIdInCookie = 0;
    std::vector<std::string> cloud_objects;
    const std::string manifest_prefix = "CLOUDMANIFEST-" + branch_name;
    auto list_status = storage_provider->ListCloudObjectsWithPrefix(
        bucket_name, object_path, manifest_prefix, &cloud_objects);
    if (!list_status.ok())
    {
        error_message = "Failed to list cloud manifest files from bucket " +
                        bucket_name + ", object_path: " + object_path +
                        ", prefix: " + manifest_prefix +
                        ", error: " + list_status.ToString();
        return false;
    }

    int64_t max_term = -1;
    std::string matched_branch;
    for (const auto &object : cloud_objects)
    {
        std::string object_branch;
        int64_t dss_shard_id = -1;
        int64_t term = -1;
        if (!ParseCloudManifestFileName(
                object, object_branch, dss_shard_id, term))
        {
            continue;
        }

        if (object_branch == branch_name &&
            dss_shard_id == kDssShardIdInCookie && term > max_term)
        {
            matched_branch = object_branch;
            max_term = term;
        }
    }

    if (max_term < 0)
    {
        error_message = "No matching cloud manifest found in bucket " +
                        bucket_name + ", object_path: " + object_path +
                        ", branch: " + branch_name +
                        ", expected cookie shard id: " +
                        std::to_string(kDssShardIdInCookie);
        return false;
    }

    cookie_on_open =
        MakeCloudManifestCookie(matched_branch, kDssShardIdInCookie, max_term);
    return true;
}

inline bool ParseSizeBytes(const std::string &size_str, uint64_t &bytes)
{
    if (size_str.size() <= 2)
    {
        return false;
    }

    std::string number_part = size_str.substr(0, size_str.size() - 2);
    std::string unit_part = size_str.substr(size_str.size() - 2);
    std::transform(unit_part.begin(),
                   unit_part.end(),
                   unit_part.begin(),
                   [](unsigned char c) { return std::toupper(c); });

    uint64_t multiplier = 0;
    if (unit_part == "MB")
    {
        multiplier = 1024ULL * 1024ULL;
    }
    else if (unit_part == "GB")
    {
        multiplier = 1024ULL * 1024ULL * 1024ULL;
    }
    else if (unit_part == "TB")
    {
        multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
    }
    else
    {
        return false;
    }

    if (number_part.empty() ||
        !std::all_of(number_part.begin(),
                     number_part.end(),
                     [](unsigned char c) { return std::isdigit(c); }))
    {
        return false;
    }

    try
    {
        bytes = std::stoull(number_part) * multiplier;
    }
    catch (...)
    {
        return false;
    }

    return bytes > 0;
}

// DSS value format constants and helpers.
// Value layout: [version_ts(8B, MSB=has_ttl)][ttl(8B optional)][record_data]
constexpr uint64_t kTtlMsb = 1ULL << 63;
constexpr uint64_t kTtlMsbMask = ~kTtlMsb;

inline void DecodeHasTTLFromTs(uint64_t &ts, bool &has_ttl)
{
    if (ts & kTtlMsb)
    {
        has_ttl = true;
        ts &= kTtlMsbMask;
    }
    else
    {
        has_ttl = false;
    }
}

inline void DeserializeDssValue(const char *data,
                                size_t size,
                                std::string &record,
                                uint64_t &ts,
                                uint64_t &ttl)
{
    assert(size >= sizeof(uint64_t));
    ts = *reinterpret_cast<const uint64_t *>(data);
    size_t offset = sizeof(uint64_t);
    bool has_ttl = false;
    DecodeHasTTLFromTs(ts, has_ttl);
    if (has_ttl)
    {
        assert(size >= sizeof(uint64_t) * 2);
        ttl = *reinterpret_cast<const uint64_t *>(data + offset);
        offset += sizeof(uint64_t);
    }
    else
    {
        ttl = 0;
    }
    record.assign(data + offset, size - offset);
}

// Parse DSS key: {kv_table_name}/{partition_id}/{actual_key}
// Extracts kv_table_name and actual_key from the full RocksDB key.
inline bool ParseDssKey(const rocksdb::Slice &full_key,
                        std::string &table_name,
                        std::string_view &actual_key)
{
    std::string key_str = full_key.ToString();
    size_t first_sep = key_str.find('/');
    if (first_sep == std::string::npos)
    {
        return false;
    }
    table_name = key_str.substr(0, first_sep);

    size_t second_sep = key_str.find('/', first_sep + 1);
    if (second_sep == std::string::npos)
    {
        return false;
    }

    actual_key = std::string_view(full_key.data() + second_sep + 1,
                                  full_key.size() - second_sep - 1);
    return true;
}

// Deserialize DSS record_data into a RedisEloqObject.
// record_data format: [obj_type(int8)][serialized_object]
inline txservice::TxRecord::Uptr DeserializeRecordData(const char *data,
                                                       size_t size)
{
    if (size < 1)
    {
        return nullptr;
    }

    int8_t obj_type_int8 = static_cast<int8_t>(data[0]);
    RedisObjectType obj_type = static_cast<RedisObjectType>(obj_type_int8);
    txservice::TxRecord::Uptr typed_rec;

    switch (obj_type)
    {
    case RedisObjectType::String:
        typed_rec.reset(new RedisStringObject());
        break;
    case RedisObjectType::List:
        typed_rec.reset(new RedisListObject());
        break;
    case RedisObjectType::Hash:
        typed_rec.reset(new RedisHashObject());
        break;
    case RedisObjectType::Zset:
        typed_rec.reset(new RedisZsetObject());
        break;
    case RedisObjectType::Set:
        typed_rec.reset(new RedisHashSetObject());
        break;
    case RedisObjectType::TTLString:
        typed_rec.reset(new RedisStringTTLObject());
        break;
    case RedisObjectType::TTLSet:
        typed_rec.reset(new RedisHashSetTTLObject());
        break;
    case RedisObjectType::TTLHash:
        typed_rec.reset(new RedisHashTTLObject());
        break;
    case RedisObjectType::TTLList:
        typed_rec.reset(new RedisListTTLObject());
        break;
    case RedisObjectType::TTLZset:
        typed_rec.reset(new RedisZsetTTLObject());
        break;
    default:
        return nullptr;
    }

    size_t offset = 0;
    typed_rec->Deserialize(data, offset);
    return typed_rec;
}

// Extract DB number from catalog key. Catalog keys have format:
//   table_catalogs/0/eloqkv_data_table_N
// Returns -1 if the key does not match.
inline int ExtractDbNumberFromCatalogKey(const std::string &table_name)
{
    const std::string prefix("eloqkv_data_table_");
    if (table_name.compare(0, prefix.size(), prefix) != 0)
    {
        return -1;
    }
    // The table name is like "eloqkv_data_table_0" or
    // "eloqkv_data_table_0_2026_06_03..." (after FLUSHDB)
    std::string suffix = table_name.substr(prefix.size());
    // Extract the leading number before any '_'
    size_t underscore = suffix.find('_');
    std::string num_str = (underscore == std::string::npos)
                              ? suffix
                              : suffix.substr(0, underscore);
    try
    {
        return std::stoi(num_str);
    }
    catch (...)
    {
        return -1;
    }
}

void RocksdbCloud2RDB(const std::string &base_url,
                      const std::string &output_fpath,
                      const std::string &aws_key_id,
                      const std::string &aws_secret,
                      const std::string &region,
                      const std::string &local_db_path_base,
                      uint32_t shard_num,
                      uint32_t threads_cnt,
                      uint32_t round_batch_size,
                      uint32_t read_parse_ratio)
{
    std::cout << "Starting RocksDB Cloud export" << std::endl;

#if defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_S3)
    Aws::SDKOptions aws_options;
    aws_options.httpOptions.installSigPipeHandler = true;
    aws_options.loggingOptions.logLevel = Aws::Utils::Logging::LogLevel::Info;
    Aws::InitAPI(aws_options);
#endif

    if (base_url.empty())
    {
        LOG(ERROR) << "No object store URL provided";
        return;
    }

    outfile.open(output_fpath, std::ios::binary);
    if (!outfile.is_open())
    {
        LOG(ERROR) << "Failed to open output file: " << output_fpath;
        return;
    }

    // Write RDB header
    {
        std::unique_lock<std::mutex> write_lk(write_mux);
        std::string buf;
        RedisRdbUtil::ParseHeader(buf);
        outfile << buf;
        crc_val = crc64speed(crc_val, buf.data(), buf.size());
    }

    // Parse base URL to get common cloud config (matching DSS pattern)
    std::string base = base_url;
    while (!base.empty() && base.back() == '/')
    {
        base.pop_back();
    }

    S3UrlComponents url_parts = ParseS3Url(base);
    if (!url_parts.is_valid)
    {
        LOG(ERROR) << "Failed to parse URL: " << base_url
                   << ", error: " << url_parts.error_message;
        return;
    }

    // Set up base CloudFileSystemOptions. Each shard gets its own object path,
    // matching RocksDBCloudDataStoreFactory's /ds_{shard_id} suffix.
    rocksdb::CloudFileSystemOptions cfs_options;

#if defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_S3)
    if (!aws_key_id.empty() && !aws_secret.empty())
    {
        cfs_options.credentials.InitializeSimple(aws_key_id, aws_secret);
    }
    else
    {
        cfs_options.credentials.type = rocksdb::AwsAccessType::kUndefined;
    }
    rocksdb::Status cred_status = cfs_options.credentials.HasValid();
    if (!cred_status.ok())
    {
        LOG(ERROR) << "Invalid AWS credentials: " << cred_status.ToString();
        return;
    }
#endif

    cfs_options.src_bucket.SetBucketName(url_parts.bucket_name);
    cfs_options.src_bucket.SetBucketPrefix("");
    cfs_options.src_bucket.SetRegion(region);
    cfs_options.src_bucket.SetObjectPath(url_parts.object_path);
    cfs_options.dest_bucket.SetBucketName(url_parts.bucket_name);
    cfs_options.dest_bucket.SetBucketPrefix("");
    cfs_options.dest_bucket.SetRegion(region);
    cfs_options.dest_bucket.SetObjectPath(url_parts.object_path);

#if defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_S3)
    if (!url_parts.endpoint_url.empty())
    {
        std::string ep_lower = url_parts.endpoint_url;
        std::transform(ep_lower.begin(),
                       ep_lower.end(),
                       ep_lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        bool use_https = (ep_lower.rfind("https://", 0) == 0);
        std::string host;
        if (ep_lower.rfind("http://", 0) == 0)
        {
            host = url_parts.endpoint_url.substr(7);
        }
        else if (use_https)
        {
            host = url_parts.endpoint_url.substr(8);
        }
        else
        {
            host = url_parts.endpoint_url;
        }
        cfs_options.s3_client_factory =
            [host = std::move(host), use_https](
                const std::shared_ptr<Aws::Auth::AWSCredentialsProvider>
                    &cred_provider,
                const Aws::Client::ClientConfiguration &base_config)
            -> std::shared_ptr<Aws::S3::S3Client>
        {
            Aws::Client::ClientConfiguration config = base_config;
            config.endpointOverride = host;
            config.scheme =
                use_https ? Aws::Http::Scheme::HTTPS : Aws::Http::Scheme::HTTP;
            config.verifySSL = false;
            if (cred_provider)
            {
                return std::make_shared<Aws::S3::S3Client>(
                    cred_provider,
                    config,
                    Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
                    false /* path-style for MinIO */);
            }
            return std::make_shared<Aws::S3::S3Client>(
                config,
                Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
                false /* path-style for MinIO */);
        };
    }
#endif

    cfs_options.constant_sst_file_size_in_sst_file_manager = 64 * 1024 * 1024L;
    cfs_options.skip_cloud_files_in_getchildren = true;
    uint64_t sst_file_cache_size = 0;
    if (!ParseSizeBytes(FLAGS_rocksdb_cloud_sst_file_cache_size,
                        sst_file_cache_size))
    {
        LOG(ERROR) << "Invalid --rocksdb_cloud_sst_file_cache_size: "
                   << FLAGS_rocksdb_cloud_sst_file_cache_size
                   << ", expected a positive size ending with MB, GB, or TB";
        return;
    }
    cfs_options.sst_file_cache = rocksdb::NewLRUCache(
        sst_file_cache_size, FLAGS_rocksdb_cloud_sst_file_cache_num_shard_bits);
    std::cout << "SST file cache size: "
              << FLAGS_rocksdb_cloud_sst_file_cache_size << " ("
              << sst_file_cache_size << " bytes), shard bits: "
              << FLAGS_rocksdb_cloud_sst_file_cache_num_shard_bits << std::endl;
    cfs_options.resync_on_open = true;
    cfs_options.disable_cloud_file_deletion = true;
    cfs_options.delete_cloud_invisible_files_on_open = false;
#if defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_S3)
    cfs_options.use_aws_transfer_manager = url_parts.endpoint_url.empty();
#endif

    uint64_t clock_ts = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();

    for (uint32_t shard_id = 0; shard_id < shard_num; shard_id++)
    {
        std::cout << "Processing shard " << shard_id << std::endl;
        ShardProgressPrinter progress(shard_id);
        progress.Start("creating-fs");

        const std::string shard_object_path =
            BuildShardObjectPath(url_parts.object_path, shard_id);
        rocksdb::CloudFileSystemOptions shard_cfs_options = cfs_options;
        shard_cfs_options.src_bucket.SetObjectPath(shard_object_path);
        shard_cfs_options.dest_bucket.SetObjectPath(shard_object_path);

        rocksdb::CloudFileSystem *cfs = nullptr;
#if defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_S3)
        rocksdb::Status fs_status =
            rocksdb::CloudFileSystemEnv::NewAwsFileSystem(
                rocksdb::FileSystem::Default(),
                shard_cfs_options,
                nullptr,
                &cfs);
#elif defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_GCS)
        rocksdb::Status fs_status =
            rocksdb::CloudFileSystemEnv::NewGcpFileSystem(
                rocksdb::FileSystem::Default(),
                shard_cfs_options,
                nullptr,
                &cfs);
#endif
        if (!fs_status.ok())
        {
            progress.Finish("failed");
            std::cerr << "Failed to create cloud file system for shard "
                      << shard_id << ": " << fs_status.ToString() << std::endl;
            continue;
        }

        std::shared_ptr<rocksdb::FileSystem> cloud_fs(cfs);
        auto storage_provider = cfs->GetStorageProvider();

        std::string cookie_on_open;
        std::string manifest_error;
        progress.SetPhase("discovering");
        if (!FindLatestCloudManifest(storage_provider,
                                     url_parts.bucket_name,
                                     shard_object_path,
                                     FLAGS_eloq_dss_branch_name,
                                     cookie_on_open,
                                     manifest_error))
        {
            progress.Finish("skipped");
            std::cerr << manifest_error << std::endl;
            continue;
        }

        auto &mutable_cfs_options = cfs->GetMutableCloudFileSystemOptions();
        mutable_cfs_options.cookie_on_open = cookie_on_open;
        mutable_cfs_options.new_cookie_on_open.clear();

        std::unique_ptr<rocksdb::Env> cloud_env =
            rocksdb::NewCompositeEnv(cloud_fs);

        rocksdb::Options options;
        options.env = cloud_env.get();
        options.create_if_missing = false;
        options.info_log_level = rocksdb::InfoLogLevel::INFO_LEVEL;
        options.paranoid_checks = false;
        options.max_open_files = 0;
        options.disable_auto_compactions = true;
        options.best_efforts_recovery = false;
        options.skip_checking_sst_file_sizes_on_db_open = true;
        options.skip_stats_update_on_db_open = true;
        options.atomic_flush = true;

        std::string db_subpath =
            local_db_path_base + "/ds_" + std::to_string(shard_id) + "/db/";
        std::error_code create_ec;
        std::filesystem::create_directories(db_subpath, create_ec);
        if (create_ec.value() != 0)
        {
            progress.Finish("failed");
            std::cerr << "Failed to create local cache dir " << db_subpath
                      << ": " << create_ec.message() << std::endl;
            continue;
        }

        rocksdb::DBCloud *db = nullptr;
        progress.SetPhase("opening");
        rocksdb::Status open_status =
            rocksdb::DBCloud::Open(options, db_subpath, "", 0, &db, true);
        if (!open_status.ok())
        {
            progress.Finish("failed");
            std::cerr << "Failed to open DBCloud for shard " << shard_id << ": "
                      << open_status.ToString() << std::endl;
            continue;
        }

        // Restore skip_cloud_files_in_getchildren
        cfs->GetMutableCloudFileSystemOptions()
            .skip_cloud_files_in_getchildren = false;
        rocksdb::Status set_open_files_status =
            db->SetDBOptions({{"max_open_files", "-1"}});
        if (!set_open_files_status.ok())
        {
            std::cerr << "Failed to restore max_open_files for shard "
                      << shard_id << ": " << set_open_files_status.ToString()
                      << std::endl;
        }

        // Direct scan
        progress.SetPhase("scanning");
        rocksdb::ReadOptions ro;
        std::unique_ptr<rocksdb::Iterator> it(
            db->NewIterator(ro, db->DefaultColumnFamily()));

        int current_db = -1;
        uint64_t read_key_count = 0;
        uint64_t written_key_count = 0;
        std::string output_buf;
        RedisRdbUtil util;
        progress.SetCounts(read_key_count, written_key_count);

        for (it->SeekToFirst(); it->Valid(); it->Next())
        {
            std::string table_name;
            std::string_view actual_key;
            if (!ParseDssKey(it->key(), table_name, actual_key))
            {
                continue;
            }

            int db_no = ExtractDbNumberFromCatalogKey(table_name);
            if (db_no < 0)
            {
                continue;
            }
            read_key_count++;
            progress.SetCounts(read_key_count, written_key_count);

            std::string record;
            uint64_t ts = 0;
            uint64_t ttl = 0;
            DeserializeDssValue(
                it->value().data(), it->value().size(), record, ts, ttl);

            const bool has_ttl = ttl > 0;
            if (has_ttl && ttl < (clock_ts / 1000))
            {
                continue;
            }

            txservice::TxRecord::Uptr rec =
                DeserializeRecordData(record.data(), record.size());
            if (!rec)
            {
                continue;
            }

            if (has_ttl)
            {
                rec->SetTTL(ttl);
            }

            if (db_no != current_db)
            {
                if (!output_buf.empty())
                {
                    std::unique_lock<std::mutex> write_lk(write_mux);
                    outfile << output_buf;
                    crc_val = crc64speed(
                        crc_val, output_buf.data(), output_buf.size());
                    output_buf.clear();
                }
                std::unique_lock<std::mutex> write_lk(write_mux);
                std::string buf;
                RedisRdbUtil::ParseSelectDB(db_no, buf);
                outfile << buf;
                crc_val = crc64speed(crc_val, buf.data(), buf.size());
                current_db = db_no;
            }

            EloqKey eloq_key(actual_key.data(), actual_key.size());

            util.ParseEloqKV(&eloq_key,
                             static_cast<RedisEloqObject *>(rec.get()),
                             output_buf);

            written_key_count++;
            progress.SetCounts(read_key_count, written_key_count);

            if (output_buf.size() >= WriteBlockSize)
            {
                std::unique_lock<std::mutex> write_lk(write_mux);
                outfile << output_buf;
                crc_val =
                    crc64speed(crc_val, output_buf.data(), output_buf.size());
                output_buf.clear();
            }
        }

        if (!output_buf.empty())
        {
            std::unique_lock<std::mutex> write_lk(write_mux);
            outfile << output_buf;
            crc_val = crc64speed(crc_val, output_buf.data(), output_buf.size());
            output_buf.clear();
        }

        stat_read_key_count.fetch_add(read_key_count);
        stat_written_key_count.fetch_add(written_key_count);
        progress.SetPhase("closing");

        it.reset();
        rocksdb::Status close_status = db->Close();
        delete db;
        progress.Finish();
        if (!close_status.ok())
        {
            std::cerr << "Failed to close DBCloud for shard " << shard_id
                      << ": " << close_status.ToString() << std::endl;
        }
    }

    // Write RDB footer with CRC64
    {
        std::unique_lock<std::mutex> write_lk(write_mux);
        std::string buf;
        RedisRdbUtil::ParseEnd(buf);
        crc_val = crc64speed(crc_val, buf.data(), buf.size());
        buf.append(reinterpret_cast<char *>(&crc_val), 8);
        outfile << buf;
        outfile.flush();
        outfile.close();
    }

#if defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_S3)
    Aws::ShutdownAPI(aws_options);
#endif
}

#endif  // ROCKSDB_CLOUD_EXPORT

void Rocksdb2RDB(const std::string &db_path,
                 const std::string &output_fpath,
                 uint32_t threads_cnt = 1,
                 uint32_t round_batch_size = 10000,
                 uint32_t read_parse_ratio = 3)
{
    rocksdb::Options options;
    options.create_if_missing = false;

    rocksdb::ColumnFamilyOptions cf_options;
    std::vector<rocksdb::ColumnFamilyDescriptor> cfds;

    std::vector<std::string> column_families;
    rocksdb::Status status =
        rocksdb::DB::ListColumnFamilies(options, db_path, &column_families);
    if (!status.ok())
    {
        LOG(ERROR) << "Failed to list column families: " << status.ToString()
                   << ", please check db path: " << db_path;
        return;
    }

    for (const auto &cf : column_families)
    {
        cfds.emplace_back(cf, cf_options);
    }

    outfile.open(output_fpath);
    if (!outfile.is_open())
    {
        LOG(INFO) << "Failed to open output file: " << output_fpath;
        return;
    }

    rocksdb::DB *db;
    std::vector<rocksdb::ColumnFamilyHandle *> cfhs;
    // The ordered data table column family handlers.
    std::vector<rocksdb::ColumnFamilyHandle *> data_table_cfhs;

    status = rocksdb::DB::Open(options, db_path, cfds, &cfhs, &db);
    if (!status.ok())
    {
        LOG(ERROR) << "Failed to open rocksdb: " << status.ToString()
                   << ", please check db path: " << db_path;
        return;
    }

    rocksdb::ColumnFamilyHandle *default_cfh = nullptr;
    for (auto cfh : cfhs)
    {
        if (cfh->GetName() == rocksdb::kDefaultColumnFamilyName)
        {
            default_cfh = cfh;
            break;
        }
    }
    CHECK(default_cfh != nullptr);

    // Traverse the table catalogs and store the corresponding column family
    // handlers in order.
    std::vector<std::string> table_names;
    const int databases = 16;
    table_names.resize(databases);
    for (int i = 0; i < databases; i++)
    {
        table_names[i].append("data_table_");
        table_names[i].append(std::to_string(i));
    }

    rocksdb::ReadOptions read_options;
    for (const auto &table_name : table_names)
    {
        std::string table_key = table_name + "_catalog";
        rocksdb::PinnableWideColumns pinnable_table_catalog;
        auto status = db->GetEntity(rocksdb::ReadOptions(),
                                    default_cfh,
                                    table_key,
                                    &pinnable_table_catalog);
        CHECK(status.ok());
        const rocksdb::WideColumns &table_catalog_wc =
            pinnable_table_catalog.columns();
        rocksdb::ColumnFamilyHandle *target_cfh = nullptr;
        for (auto &column : table_catalog_wc)
        {
            if (column.name() == "kv_cf_name")
            {
                const rocksdb::Slice &val = column.value();
                for (const auto cfh : cfhs)
                {
                    if (cfh->GetName() == val)
                    {
                        target_cfh = cfh;
                        break;
                    }
                }
            }
        }
        CHECK(target_cfh != nullptr);
        data_table_cfhs.emplace_back(target_cfh);
    }

    const size_t BATCH_PER_THREAD = round_batch_size;

    entry_pool.resize(threads_cnt * read_parse_ratio);
    uint32_t write_buf_cnt = threads_cnt > 50 ? threads_cnt * 2 : 100;
    write_buf_pool.resize(write_buf_cnt);

    for (size_t i = 0; i < entry_pool.size(); i++)
    {
        free_parse_tasks.enqueue(&entry_pool[i]);
    }

    for (size_t i = 0; i < write_buf_pool.size(); i++)
    {
        // reserve 2* write block size to avoid resize
        write_buf_pool[i].reserve(WriteBlockSize * 2);
        free_flush_tasks.enqueue(&write_buf_pool[i]);
    }

    {
        std::unique_lock<std::mutex> write_lk(write_mux);
        std::string buf;
        RedisRdbUtil::ParseHeader(buf);
        outfile << buf;
        crc_val = crc64speed(crc_val, buf.data(), buf.size());
    }

    for (size_t db_no = 0; db_no < data_table_cfhs.size(); db_no++)
    {
        LOG(INFO) << "handling keys in db-" << db_no;
        // read db_x
        rocksdb::ColumnFamilyHandle *cfh = data_table_cfhs[db_no];

        auto iter = std::unique_ptr<rocksdb::Iterator>(
            db->NewIterator(read_options, cfh));
        iter->SeekToFirst();

        if (!iter->Valid())
        {
            LOG(INFO) << "db-" << db_no << " is empty";
            continue;
        }
        // write db selector
        {
            std::unique_lock<std::mutex> write_lk(write_mux);
            std::string buf;
            RedisRdbUtil::ParseSelectDB(db_no, buf);
            outfile << buf;
            crc_val = crc64speed(crc_val, buf.data(), buf.size());
        }

        std::vector<std::unique_ptr<ParseWorker>> parse_workers;
        for (uint32_t i = 0; i < threads_cnt; i++)
        {
            parse_workers.emplace_back(std::make_unique<ParseWorker>());
        }
        WriteWorker write_worker;

        uint64_t key_count = 0;

        BatchKvEntry *batch_entry;

        while (iter->Valid())
        {
            while (!free_parse_tasks.try_dequeue(batch_entry))
            {
                std::unique_lock<std::mutex> lk(rocksdb_reader_mux);
                // Wait for free tasks with timeout of 100ms
                rocksdb_reader_cv.wait_for(lk, std::chrono::milliseconds(100));
            }

            auto &entry_vec = *batch_entry;
            entry_vec.clear();
            entry_vec.reserve(BATCH_PER_THREAD);

            for (size_t entry_idx = 0;
                 entry_idx < BATCH_PER_THREAD && iter->Valid();
                 entry_idx++)
            {
                rocksdb::Slice key_slice = iter->key();
                rocksdb::Slice value_slice = iter->value();
                entry_vec.emplace_back();
                entry_vec[entry_idx].db_idx_ = db_no;
                entry_vec[entry_idx].key_str_ = key_slice.ToString();
                entry_vec[entry_idx].payload_str_ = value_slice.ToString();

                key_count++;
                iter->Next();
            }

            parse_tasks.enqueue(batch_entry);
            parser_cv.notify_all();
        }

        LOG(INFO) << "db-" << db_no << " contains keys count:" << key_count;
        stat_read_key_count += key_count;

        // wait for all pending task to finish
        for (uint32_t idx = 0; idx < threads_cnt; idx++)
        {
            parse_workers.at(idx)->Terminate();
        }

        parser_cv.notify_all();

        for (uint32_t idx = 0; idx < threads_cnt; idx++)
        {
            parse_workers.at(idx)->Join();
        }

        write_worker.Terminate();
        writer_cv.notify_one();
        write_worker.Join();
    }

    {
        std::unique_lock<std::mutex> write_lk(write_mux);
        std::string buf;
        RedisRdbUtil::ParseEnd(buf);
        crc_val = crc64speed(crc_val, buf.data(), buf.size());

        // write crc checksum
        buf.append(reinterpret_cast<char *>(&crc_val), 8);
        outfile << buf;
        outfile.flush();
        outfile.close();
    }
    // close rocksdb
    for (const auto cfh : cfhs)
    {
        delete cfh;
    }
    db->Close();
    delete db;
}

bool CheckPathExists(const std::string &db_path)
{
    std::error_code error_code;
    bool path_exists = std::filesystem::exists(db_path, error_code);
    if (error_code.value() != 0)
    {
        LOG(ERROR) << "unable to check rocksdb directory: " << db_path
                   << ", error code: " << error_code.value()
                   << ", error message: " << error_code.message();
        return false;
    }

    return path_exists;
}

}  // namespace Tools

}  // namespace EloqKV

int main(int argc, char *argv[])
{
    google::SetUsageMessage("Export EloqKV data to Redis RDB format.");
    if (EloqKV::Tools::MaybeHandleToolHelp(argc, argv))
    {
        return 0;
    }

    google::ParseCommandLineFlags(&argc, &argv, true);

    FLAGS_logtostdout = true;
    google::InitGoogleLogging(argv[0]);
    crc64speed_init();

#if ROCKSDB_CLOUD_EXPORT
    if (!EloqKV::Tools::ValidateRequiredCloudFlags())
    {
        return -1;
    }

    const std::string object_store_url =
        EloqKV::Tools::BuildObjectStoreServiceUrl();
    if (object_store_url.empty())
    {
        std::cerr << "Please specify non-empty --rocksdb_cloud_bucket_prefix, "
                     "--rocksdb_cloud_bucket_name and "
                     "--rocksdb_cloud_object_path"
                  << std::endl;
        return -1;
    }

    std::string db_path = FLAGS_db_path;
    if (db_path.empty())
    {
        db_path = "/tmp/eloqkv_rdb_export";
    }

    std::cout << "Object store URL: " << object_store_url << std::endl;
    std::cout << "Shard count: " << FLAGS_shard_num << std::endl;
    std::cout << "Output file: " << FLAGS_output_file << std::endl;
    std::cout << "Local cache dir: " << db_path << std::endl;
    std::cout << "Region: " << FLAGS_rocksdb_cloud_region << std::endl;
    std::cout << "==== Begin ====" << std::endl;

    auto start_time = std::chrono::system_clock::now();

    EloqKV::Tools::RocksdbCloud2RDB(object_store_url,
                                    FLAGS_output_file,
                                    FLAGS_aws_access_key_id,
                                    FLAGS_aws_secret_key,
                                    FLAGS_rocksdb_cloud_region,
                                    db_path,
                                    FLAGS_shard_num,
                                    FLAGS_thread_count,
                                    FLAGS_round_batch_size,
                                    FLAGS_pre_read_ratio);
#else
    if (FLAGS_rocksdb_path.empty())
    {
        LOG(ERROR) << "Please specify rocksdb data path: --rocksdb_path";
        return -1;
    }

    if (!EloqKV::Tools::CheckPathExists(FLAGS_rocksdb_path))
    {
        LOG(ERROR) << "Rocksdb data path is not exists: " << FLAGS_rocksdb_path;
        return -1;
    }

    std::filesystem::path output_file_dir =
        std::filesystem::path(FLAGS_output_file).parent_path();
    std::error_code error_code;
    if (!std::filesystem::exists(output_file_dir, error_code))
    {
        if (error_code.value() != 0)
        {
            LOG(ERROR) << "Unable to check output file directory: "
                       << output_file_dir
                       << ", error code: " << error_code.value()
                       << ", error message: " << error_code.message();
            return -1;
        }
        // Create output file dir
        if (!std::filesystem::create_directories(output_file_dir, error_code))
        {
            LOG(ERROR) << "Failed to create output directory: "
                       << output_file_dir
                       << ", error code: " << error_code.value()
                       << ", error message: " << error_code.message();
            return -1;
        }
    }

    LOG(INFO) << "RocksDB path:" << FLAGS_rocksdb_path;
    LOG(INFO) << "Output file path:" << FLAGS_output_file;
    LOG(INFO) << "Parse thread count:" << FLAGS_thread_count;
    LOG(INFO) << "Round batch size:" << FLAGS_round_batch_size;
    LOG(INFO) << "Pre read batch count:" << FLAGS_pre_read_ratio;
    LOG(INFO) << "====Begin====";

    auto start_time = std::chrono::system_clock::now();

    EloqKV::Tools::Rocksdb2RDB(FLAGS_rocksdb_path,
                               FLAGS_output_file,
                               FLAGS_thread_count,
                               FLAGS_round_batch_size,
                               FLAGS_pre_read_ratio);
#endif

    auto end_time = std::chrono::system_clock::now();
    auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time)
            .count();
    int hours = seconds / 3600;
    int minutes = (seconds % 3600) / 60;
    int remainingSeconds = seconds % 60;

    std::string time_str;
    if (hours > 0)
    {
        time_str = std::to_string(hours) + "hour";
    }
    if (minutes > 0)
    {
        time_str.append(std::to_string(minutes) + "min");
    }
    time_str.append(std::to_string(remainingSeconds) + "sec");

    std::cout << "==== Finished ====" << std::endl;
    std::cout << "Used time: " << time_str << std::endl;
    std::cout << "Read keys from rocksdb: "
              << EloqKV::Tools::stat_read_key_count.load()
              << ", write keys to RDB: "
              << EloqKV::Tools::stat_written_key_count.load()
              << ", expired keys:"
              << (EloqKV::Tools::stat_read_key_count.load() -
                  EloqKV::Tools::stat_written_key_count.load())
              << std::endl;
    return 0;
}
