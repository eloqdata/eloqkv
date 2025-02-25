#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <ratio>
#include <regex>
#include <string>
#include <vector>

#include "bthread/moodycamelqueue.h"
#include "eloq_key.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "redis_hash_object.h"
#include "redis_list_object.h"
#include "redis_object.h"
#include "redis_set_object.h"
#include "redis_string_object.h"
#include "redis_zset_object.h"
#include "rocksdb/db.h"
extern "C"
{
#include "crcspeed/crc64speed.h"
}

DEFINE_string(rocksdb_path, "", "The RocksDB data (full) path");
DEFINE_string(output_file, "", "The file to store the RDB file");
DEFINE_uint32(thread_count,
              1,
              "The number of thread to parse records from rocksdb");

DEFINE_uint32(round_batch_size,
              10000,
              "The number of keys to parse in one round");

DEFINE_uint32(pre_read_ratio,
              2,
              "Pre read batch count for each parsing thread");

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
    google::ParseCommandLineFlags(&argc, &argv, true);

    FLAGS_logtostdout = true;
    google::InitGoogleLogging(argv[0]);
    crc64speed_init();

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

    LOG(INFO) << "====Finished====";

    LOG(INFO) << "Used time: " << time_str;
    LOG(INFO) << "Read keys from rocksdb: "
              << EloqKV::Tools::stat_read_key_count.load()
              << ", write keys to RDB: "
              << EloqKV::Tools::stat_written_key_count.load()
              << ", expired keys:"
              << (EloqKV::Tools::stat_read_key_count.load() -
                  EloqKV::Tools::stat_written_key_count.load());
    return 0;
}
