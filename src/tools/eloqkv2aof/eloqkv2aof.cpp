#include <array>
#include <filesystem>
#include <fstream>
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
#include "redis_string_num.h"  // d2string()
#include "redis_string_object.h"
#include "redis_zset_object.h"
#include "rocksdb/db.h"

#define CRLF "\r\n"  // NOLINT

DEFINE_string(rocksdb_path, "", "The RocksDB data (full) path");
DEFINE_string(output_file_dir,
              "",
              "The file directory path to store the aof data files");
DEFINE_uint32(thread_count,
              1,
              "The number of thread to parse records from rocksdb");

DEFINE_uint32(round_batch_size,
              10000,
              "The number of keys to parse in one round");

DEFINE_uint32(pre_read_ratio,
              2,
              "Pre read batch count for each parsing thread");

namespace EloqKV
{
namespace Tools
{
class RedisReplyUtil
{
public:
    template <typename T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
    void MultiLen(T len, std::string &cmd_str)
    {
        cmd_str.append("*");
        cmd_str.append(std::to_string(len));
        cmd_str.append(CRLF);
    }

    void BulkString(const std::string &data, std::string &cmd_str)
    {
        cmd_str.append("$");
        cmd_str.append(std::to_string(data.length()));
        cmd_str.append(CRLF);
        cmd_str.append(data);
        cmd_str.append(CRLF);
    }

    void ArrayOfBulkStrings(const std::vector<std::string> &elems,
                            std::string &cmd_str)
    {
        MultiLen(elems.size(), cmd_str);
        for (const auto &elem : elems)
        {
            BulkString(elem, cmd_str);
        }
    }

    void ParseSelectDB(int db_idx, std::string &cmd_str)
    {
        ArrayOfBulkStrings({"SELECT", std::to_string(db_idx)}, cmd_str);
    }

    void ParseEloqKV(const EloqKey *key,
                     const RedisEloqObject *obj,
                     std::string &cmd_str)
    {
        // std::string output;
        switch (obj->ObjectType())
        {
        case RedisObjectType::String:
        case RedisObjectType::TTLString:
        {
            const RedisStringObject *typed_obj =
                static_cast<const RedisStringObject *>(obj);
            ArrayOfBulkStrings({"SET",
                                std::string(key->StringView()),
                                std::string(typed_obj->StringView())},
                               cmd_str);
            break;
        }

        case RedisObjectType::List:
        case RedisObjectType::TTLList:
        {
            const RedisListObject *typed_obj =
                static_cast<const RedisListObject *>(obj);

            const std::deque<EloqString> &elements = typed_obj->Elements();
            for (const EloqString &value : elements)
            {
                ArrayOfBulkStrings(
                    {"RPUSH", std::string(key->StringView()), value.String()},
                    cmd_str);
            }

            break;
        }
        case RedisObjectType::Hash:
        case RedisObjectType::TTLHash:
        {
            const RedisHashObject *typed_obj =
                static_cast<const RedisHashObject *>(obj);

            const absl::flat_hash_map<EloqString, EloqString> &elements =
                typed_obj->Elements();
            for (const auto &[sub_key, sub_value] : elements)
            {
                ArrayOfBulkStrings({"HSET",
                                    std::string(key->StringView()),
                                    sub_key.String(),
                                    sub_value.String()},
                                   cmd_str);
            }

            break;
        }
        case RedisObjectType::Set:
        case RedisObjectType::TTLSet:
        {
            const RedisHashSetObject *typed_obj =
                static_cast<const RedisHashSetObject *>(obj);

            const auto &elements = typed_obj->Elements();
            for (const EloqString &sub_key : elements)
            {
                ArrayOfBulkStrings(
                    {"SADD", std::string(key->StringView()), sub_key.String()},
                    cmd_str);
            }

            break;
        }
        case RedisObjectType::Zset:
        case RedisObjectType::TTLZset:
        {
            const RedisZsetObject *typed_obj =
                static_cast<const RedisZsetObject *>(obj);

            const absl::flat_hash_map<std::string_view, double> &elements =
                typed_obj->Elements();

            for (auto &[sub_key_sv, score] : elements)
            {
                ArrayOfBulkStrings({"ZADD",
                                    std::string(key->StringView()),
                                    d2string(score),
                                    std::string(sub_key_sv)},
                                   cmd_str);
            }

            break;
        }
        default:
            assert(false);
            break;
        }

        if (obj->HasTTL())
        {
            ArrayOfBulkStrings({"EXPIREAT",
                                key->ToString(),
                                std::to_string(obj->GetTTL() / 1000)},
                               cmd_str);
        }
    }
};
struct KvEntry
{
    uint16_t db_idx_;
    std::string key_str_;
    std::string payload_str_;
    // std::string cmd_str_;
    bool is_deleted_;
    int64_t version_ts_;
};

using BatchKvEntry = std::vector<KvEntry>;
std::vector<BatchKvEntry> entry_pool;

moodycamel::ConcurrentQueue<BatchKvEntry *> free_tasks;
// std::atomic<uint32_t> free_tasks_size{0};
std::condition_variable free_task_cv;

moodycamel::ConcurrentQueue<BatchKvEntry *> parse_tasks;
// std::atomic<uint32_t> parse_tasks_size{0};
std::condition_variable parse_task_cv;

// count of key read from rocksdb
std::atomic_uint64_t stat_read_key_count;
// count of key written to aof
std::atomic_uint64_t stat_written_key_count;

struct ParseWorker
{
    explicit ParseWorker(const std::string output_aof_path)
        : output_aof_path_(output_aof_path)
    {
        thd_ = std::thread([this] { Run(); });
    }

    void Terminate()
    {
        std::unique_lock<std::mutex> lk(entry_mtx_);
        terminated_ = true;
    }

    void Join()
    {
        thd_.join();
    }

    void Run()
    {
        std::ofstream outfile(output_aof_path_);
        if (!outfile.is_open())
        {
            LOG(INFO) << "Failed to open output file: " << output_aof_path_;
            return;
        }

        uint64_t clock_ts =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();

        size_t written_key_count = 0;
        std::string cmds_str;
        // write 1MB data once
        const uint32_t WriteBlockSize = 1000 * 1000;
        cmds_str.reserve(WriteBlockSize);

        while (true)
        {
            bool res = parse_tasks.try_dequeue(entry_vector_);
            if (!res)
            {
                std::unique_lock<std::mutex> lk(entry_mtx_);
                if (terminated_)
                {
                    break;
                }

                parse_task_cv.wait(lk);
                continue;
            }

            for (size_t idx = 0; idx < entry_vector_->size(); idx++)
            {
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

                if (entry.db_idx_ != last_db_idx_)
                {
                    reply_util_.ParseSelectDB(entry.db_idx_, cmds_str);
                    last_db_idx_ = entry.db_idx_;
                }

                // cmds_str.clear();
                reply_util_.ParseEloqKV(
                    &eloq_key,
                    static_cast<RedisEloqObject *>(eloq_rec.get()),
                    cmds_str);

                written_key_count++;
                if (cmds_str.size() >= WriteBlockSize)
                {
                    outfile << cmds_str;
                    cmds_str.clear();
                    cmds_str.reserve(WriteBlockSize);
                }
            }

            free_tasks.enqueue(entry_vector_);
            free_task_cv.notify_all();

            entry_vector_ = nullptr;
        }

        if (cmds_str.size() > 0)
        {
            outfile << cmds_str;
        }
        outfile.flush();
        outfile.close();

        LOG(INFO) << "Thread terminated, write key count:" << written_key_count;

        stat_written_key_count += written_key_count;
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
    bool terminated_{false};
    std::vector<KvEntry> *entry_vector_{nullptr};
    std::mutex entry_mtx_;
    // std::condition_variable cv_;

    std::string output_aof_path_;
    std::thread thd_;

    int32_t last_db_idx_{-1};
    RedisReplyUtil reply_util_;
};

void Rocksdb2Aof(const std::string &db_path,
                 const std::string &output_dir,
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

    std::vector<std::unique_ptr<ParseWorker>> workers;
    for (uint32_t i = 0; i < threads_cnt; i++)
    {
        const std::string output_fpath =
            output_dir + "/" + std::to_string(i) + ".aof";
        workers.emplace_back(std::make_unique<ParseWorker>(output_fpath));
    }

    entry_pool.resize(threads_cnt * read_parse_ratio);

    for (size_t i = 0; i < entry_pool.size(); i++)
    {
        free_tasks.enqueue(&entry_pool[i]);
    }

    for (size_t db_no = 0; db_no < data_table_cfhs.size(); db_no++)
    {
        LOG(INFO) << "handling keys in db-" << db_no;
        // read db_x
        rocksdb::ColumnFamilyHandle *cfh = data_table_cfhs[db_no];

        auto iter = std::unique_ptr<rocksdb::Iterator>(
            db->NewIterator(read_options, cfh));
        iter->SeekToFirst();

        uint64_t key_count = 0;

        BatchKvEntry *batch_entry;

        std::mutex sleep_mtx;

        while (iter->Valid())
        {
            while (!free_tasks.try_dequeue(batch_entry))
            {
                std::unique_lock lk(sleep_mtx);
                free_task_cv.wait(lk);
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
            parse_task_cv.notify_one();
        }

        LOG(INFO) << "db-" << db_no << " contains keys count:" << key_count;
        stat_read_key_count += key_count;
    }

    for (uint32_t idx = 0; idx < threads_cnt; idx++)
    {
        workers.at(idx)->Terminate();
        parse_task_cv.notify_all();
        workers.at(idx)->Join();
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
    // using namespace EloqKV;
    // google::SetVersionString(VERSION);
    google::ParseCommandLineFlags(&argc, &argv, true);

    FLAGS_logtostdout = true;
    google::InitGoogleLogging(argv[0]);

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

    if (!EloqKV::Tools::CheckPathExists(FLAGS_output_file_dir))
    {
        // Create output file dir
        std::error_code error_code;
        if (!std::filesystem::create_directories(FLAGS_output_file_dir,
                                                 error_code))
        {
            LOG(ERROR) << "Failed to create output directory: "
                       << FLAGS_output_file_dir
                       << ", error code: " << error_code.value()
                       << ", error message: " << error_code.message();
            return -1;
        }
    }

    if (!std::filesystem::is_empty(FLAGS_output_file_dir))
    {
        LOG(ERROR)
            << "Output file directory is not empty, please use one empty "
               "output directory.";
        return -1;
    }

    LOG(INFO) << "RocksDB path:" << FLAGS_rocksdb_path;
    LOG(INFO) << "Output file directory path:" << FLAGS_output_file_dir;
    LOG(INFO) << "Parse thread count:" << FLAGS_thread_count;
    LOG(INFO) << "Round batch size:" << FLAGS_round_batch_size;
    LOG(INFO) << "Pre read batch count:" << FLAGS_pre_read_ratio;
    LOG(INFO) << "====Begin====";

    auto start_time = std::chrono::system_clock::now();

    EloqKV::Tools::Rocksdb2Aof(FLAGS_rocksdb_path,
                               FLAGS_output_file_dir,
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
              << ", write keys to aof: "
              << EloqKV::Tools::stat_written_key_count.load()
              << ", expired keys:"
              << (EloqKV::Tools::stat_read_key_count.load() -
                  EloqKV::Tools::stat_written_key_count.load());
    return 0;
}
