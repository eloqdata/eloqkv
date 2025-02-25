#include "rocksdb_handler.h"

#include <brpc/controller.h>
#include <brpc/server.h>
#include <brpc/stream.h>
#include <bthread/condition_variable.h>
#include <butil/file_util.h>
#include <butil/iobuf.h>
#include <fcntl.h>
#include <glog/logging.h>
#include <rocksdb/convenience.h>
#include <rocksdb/db.h>
#include <rocksdb/sst_file_reader.h>
#include <rocksdb/trace_reader_writer.h>
#include <rocksdb/utilities/checkpoint.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <ios>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cc_node_service.h"
#include "cc_request.pb.h"
#include "local_cc_shards.h"
#include "redis_hash_object.h"
#include "redis_list_object.h"
#include "redis_object.h"  // RedisEloqObject
#include "redis_set_object.h"
#include "redis_string_object.h"
#include "redis_zset_object.h"
#include "rocksdb/iostats_context.h"
#include "rocksdb/rate_limiter.h"
#include "rocksdb_scanner.h"
#include "store_util.h"
#include "tx_record.h"
#include "tx_service/include/cc/cc_req_pool.h"
#include "tx_service/include/error_messages.h"
#include "tx_service/include/sharder.h"
#include "tx_service/include/util.h"
#include "tx_worker_pool.h"

DECLARE_bool(bootstrap);
namespace EloqKV
{

RocksDBHandler::RocksDBHandler(const EloqShare::RocksDBConfig &config,
                               bool create_if_missing,
                               bool tx_enable_cache_replacement)
    : enable_stats_(config.enable_stats_),
      stats_dump_period_sec_(config.stats_dump_period_sec_),
      storage_path_(config.storage_path_),
      max_write_buffer_number_(config.max_write_buffer_number_),
      max_background_jobs_(config.max_background_jobs_),
      max_background_flushes_(config.max_background_flush_),
      max_background_compactions_(config.max_background_compaction_),
      target_file_size_base_(config.target_file_size_base_bytes_),
      target_file_size_multiplier_(config.target_file_size_multiplier_),
      write_buff_size_(config.write_buffer_size_bytes_),
      use_direct_io_for_flush_and_compaction_(
          config.use_direct_io_for_flush_and_compaction_),
      use_direct_io_for_read_(config.use_direct_io_for_read_),
      level0_stop_writes_trigger_(config.level0_stop_writes_trigger_),
      level0_slowdown_writes_trigger_(config.level0_slowdown_writes_trigger_),
      level0_file_num_compaction_trigger_(
          config.level0_file_num_compaction_trigger_),
      max_bytes_for_level_base_(config.max_bytes_for_level_base_bytes_),
      max_bytes_for_level_multiplier_(config.max_bytes_for_level_multiplier_),
      compaction_style_(config.compaction_style_),
      soft_pending_compaction_bytes_limit_(
          config.soft_pending_compaction_bytes_limit_bytes_),
      hard_pending_compaction_bytes_limit_(
          config.hard_pending_compaction_bytes_limit_bytes_),
      max_subcompactions_(config.max_subcompactions_),
      write_rate_limit_(config.write_rate_limit_bytes_),
      batch_write_size_(config.batch_write_size_),
      periodic_compaction_seconds_(config.periodic_compaction_seconds_),
      dialy_offpeak_time_utc_(config.dialy_offpeak_time_utc_),
      db_path_(storage_path_ + "/db/"),
      ckpt_path_(storage_path_ + "/rocksdb_snapshot/"),
      backup_path_(storage_path_ + "/backups/"),
      received_snapshot_path_(storage_path_ + "/received_snapshot/"),
      create_db_if_missing_(create_if_missing),
      tx_enable_cache_replacement_(tx_enable_cache_replacement)
{
    info_log_level_ = EloqShare::StringToInfoLogLevel(config.info_log_level_);
    query_worker_pool_ =
        std::make_unique<txservice::TxWorkerPool>(config.query_worker_num_);
    db_manage_worker_ = std::make_unique<txservice::TxWorkerPool>(1);
}

RocksDBHandler::~RocksDBHandler()
{
    query_worker_pool_->Shutdown();
    db_manage_worker_->Shutdown();
}

bool RocksDBHandler::Connect()
{
    // before opening rocksdb, rocksdb_storage_path_ must exist, create it
    // if not exist
    std::error_code error_code;
    bool rocksdb_storage_path_exists =
        std::filesystem::exists(db_path_, error_code);
    if (error_code.value() != 0)
    {
        LOG(ERROR) << "unable to check rocksdb directory: " << db_path_
                   << ", error code: " << error_code.value()
                   << ", error message: " << error_code.message();
        return false;
    }
    if (!rocksdb_storage_path_exists)
    {
        std::filesystem::create_directories(db_path_, error_code);
        if (error_code.value() != 0)
        {
            LOG(ERROR) << "unable to create rocksdb directory: " << db_path_
                       << ", error code: " << error_code.value()
                       << ", error message: " << error_code.message();
            return false;
        }
    }

    // Cleanup previous snapshots
    rocksdb_storage_path_exists =
        std::filesystem::exists(ckpt_path_, error_code);
    if (error_code.value() != 0)
    {
        LOG(ERROR) << "unable to check rocksdb directory: " << ckpt_path_
                   << ", error code: " << error_code.value()
                   << ", error message: " << error_code.message();
        return false;
    }
    if (!rocksdb_storage_path_exists)
    {
        std::filesystem::create_directories(ckpt_path_, error_code);
        if (error_code.value() != 0)
        {
            LOG(ERROR) << "unable to create rocksdb directory: " << ckpt_path_
                       << ", error code: " << error_code.value()
                       << ", error message: " << error_code.message();
            return false;
        }
    }
    else
    {
        // clean up previous snapshots
        for (const auto &entry :
             std::filesystem::directory_iterator(ckpt_path_))
        {
            std::filesystem::remove_all(entry.path());
        }
    }

    rocksdb_storage_path_exists =
        std::filesystem::exists(received_snapshot_path_, error_code);
    if (error_code.value() != 0)
    {
        LOG(ERROR) << "unable to check rocksdb directory: "
                   << received_snapshot_path_
                   << ", error code: " << error_code.value()
                   << ", error message: " << error_code.message();
        return false;
    }
    if (!rocksdb_storage_path_exists)
    {
        std::filesystem::create_directories(received_snapshot_path_,
                                            error_code);
        if (error_code.value() != 0)
        {
            LOG(ERROR) << "unable to create rocksdb directory: "
                       << received_snapshot_path_
                       << ", error code: " << error_code.value()
                       << ", error message: " << error_code.message();
            return false;
        }
    }
    else
    {
        // clean up previous snapshots
        for (const auto &entry :
             std::filesystem::directory_iterator(received_snapshot_path_))
        {
            std::filesystem::remove_all(entry.path());
        }
    }

    rocksdb_storage_path_exists =
        std::filesystem::exists(backup_path_, error_code);
    if (error_code.value() != 0)
    {
        LOG(ERROR) << "unable to check rocksdb directory: " << backup_path_
                   << ", error code: " << error_code.value()
                   << ", error message: " << error_code.message();
        return false;
    }
    if (!rocksdb_storage_path_exists)
    {
        std::filesystem::create_directories(backup_path_, error_code);
        if (error_code.value() != 0)
        {
            LOG(ERROR) << "unable to create rocksdb directory: " << backup_path_
                       << ", error code: " << error_code.value()
                       << ", error message: " << error_code.message();
            return false;
        }
    }

    // For non shared storage, we only need to open db if we're leader or if
    // we've received a snapshot from primary node as a standby node.
    if (create_db_if_missing_)
    {
        return StartDB(true);
    }
    return true;
}

void RocksDBHandler::ScheduleTimerTasks()
{
    LOG(ERROR) << "RocksDBHandler::ScheduleTimerTasks not implemented";
    // Not implemented
    assert(false);
}

bool RocksDBHandler::InitializeClusterConfig(
    const std::unordered_map<uint32_t, std::vector<txservice::NodeConfig>>
        &ng_configs)
{
    uint64_t version = 2;

    std::vector<uint32_t> ng_ids;
    std::vector<std::string> ng_members;
    std::vector<std::string> ng_members_is_candidate;
    std::vector<uint32_t> node_ids;
    std::vector<std::string> ips;
    std::vector<uint16_t> ports;

    std::vector<txservice::NodeConfig> nodes;
    txservice::ExtractNodesConfigs(ng_configs, nodes);
    for (const txservice::NodeConfig &node : nodes)
    {
        node_ids.push_back(node.node_id_);
        ips.push_back(node.host_name_.c_str());
        ports.push_back(node.port_);
    }

    for (auto ng_pair : ng_configs)
    {
        ng_ids.push_back(ng_pair.first);
        std::vector<uint32_t> group_members;
        std::vector<bool> group_members_is_candidate;
        for (auto &node : ng_pair.second)
        {
            group_members.push_back(node.node_id_);
            group_members_is_candidate.push_back(node.is_candidate_);
        }
        ng_members.push_back(EloqShare::SerializeVectorToString<uint32_t>(
            group_members, ",", EloqShare::uint32_str_converter));
        ng_members_is_candidate.push_back(
            EloqShare::SerializeVectorToString<bool>(
                group_members_is_candidate,
                ",",
                EloqShare::bool_str_converter));
    }

    std::string node_ids_str = EloqShare::SerializeVectorToString<uint32_t>(
        node_ids, " ", EloqShare::uint32_str_converter);
    std::string ips_str = EloqShare::SerializeVectorToString<std::string>(
        ips, " ", EloqShare::string_str_converter);
    std::string ports_str = EloqShare::SerializeVectorToString<uint16_t>(
        ports, " ", EloqShare::uint16_str_converter);
    std::string ng_ids_str = EloqShare::SerializeVectorToString<uint32_t>(
        ng_ids, " ", EloqShare::uint32_str_converter);
    std::string ng_members_str =
        EloqShare::SerializeVectorToString<std::string>(
            ng_members, " ", EloqShare::string_str_converter);
    std::string ng_members_is_candidate_str =
        EloqShare::SerializeVectorToString<std::string>(
            ng_members_is_candidate, " ", EloqShare::string_str_converter);
    std::string verstion_str = std::to_string(version);

    // Create a rocksdb::WideColumns object
    rocksdb::WideColumns cluster_config_wc;
    cluster_config_wc.emplace_back("node_ids", node_ids_str);
    cluster_config_wc.emplace_back("ips", ips_str);
    cluster_config_wc.emplace_back("ports", ports_str);
    cluster_config_wc.emplace_back("ng_ids", ng_ids_str);
    cluster_config_wc.emplace_back("ng_members", ng_members_str);
    cluster_config_wc.emplace_back("ng_members_is_candidate",
                                   ng_members_is_candidate_str);
    cluster_config_wc.emplace_back("version", verstion_str);

    // Write the cluster config to the rocksdb
    rocksdb::WriteOptions write_options;
    auto db = GetDBPtr();
    auto status =
        db->PutEntity(write_options,
                      GetColumnFamilyHandler(rocksdb::kDefaultColumnFamilyName),
                      "cluster_config",
                      cluster_config_wc);

    if (!status.ok())
    {
        LOG(ERROR) << "Unable to write to db with error: " << status.ToString();
        return false;
    }

    rocksdb::FlushOptions flush_options;
    flush_options.allow_write_stall = false;
    flush_options.wait = true;
    status = db->Flush(flush_options);
    if (!status.ok())
    {
        LOG(ERROR) << "Unable to write to db with error: " << status.ToString();
        return false;
    }

    return true;
}

bool RocksDBHandler::ReadClusterConfig(
    std::unordered_map<uint32_t, std::vector<txservice::NodeConfig>>
        &ng_configs,
    uint64_t &version,
    bool &uninitialized)
{
    DLOG(INFO) << "RocksDBHandler::ReadClusterConfig started";
    rocksdb::ReadOptions read_options;
    auto db = GetDBPtr();
    if (db == nullptr)
    {
        uninitialized = true;
        return false;
    }
    rocksdb::PinnableWideColumns pinnable_cluster_configs;
    auto status =
        db->GetEntity(read_options,
                      GetColumnFamilyHandler(rocksdb::kDefaultColumnFamilyName),
                      "cluster_config",
                      &pinnable_cluster_configs);
    if (!status.ok())
    {
        uninitialized = true;
        return false;
    }

    const rocksdb::WideColumns &cluster_configs =
        pinnable_cluster_configs.columns();

    if (cluster_configs.empty())
    {
        uninitialized = true;
        return false;
    }

    std::vector<uint32_t> node_ids;
    std::vector<std::string> ips;
    std::vector<uint16_t> ports;
    std::vector<uint32_t> ng_ids;
    std::vector<std::string> ng_members;
    std::vector<std::string> ng_members_is_candidate;

    for (const auto &config : cluster_configs)
    {
        if (config.name() == "node_ids")
        {
            std::string node_ids_str;
            node_ids_str = config.value().ToString();
            EloqShare::DeserializeStringToVector<uint32_t>(
                node_ids_str, " ", node_ids, EloqShare::str_uint32_converter);
        }
        else if (config.name() == "ips")
        {
            std::string ips_str;
            ips_str = config.value().ToString();
            EloqShare::DeserializeStringToVector<std::string>(
                ips_str, " ", ips, EloqShare::str_string_converter);
        }
        else if (config.name() == "ports")
        {
            std::string ports_str;
            ports_str = config.value().ToString();
            EloqShare::DeserializeStringToVector<uint16_t>(
                ports_str, " ", ports, EloqShare::str_uint16_converter);
        }
        else if (config.name() == "ng_ids")
        {
            std::string ng_ids_str;
            ng_ids_str = config.value().ToString();
            EloqShare::DeserializeStringToVector<uint32_t>(
                ng_ids_str, " ", ng_ids, EloqShare::str_uint32_converter);
        }
        else if (config.name() == "ng_members")
        {
            std::string ng_members_str;
            ng_members_str = config.value().ToString();
            EloqShare::DeserializeStringToVector<std::string>(
                ng_members_str,
                " ",
                ng_members,
                EloqShare::str_string_converter);
        }
        else if (config.name() == "ng_members_is_candidate")
        {
            std::string ng_members_candidate_str;
            ng_members_candidate_str = config.value().ToString();
            EloqShare::DeserializeStringToVector<std::string>(
                ng_members_candidate_str,
                " ",
                ng_members_is_candidate,
                EloqShare::str_string_converter);
        }
        else if (config.name() == "version")
        {
            std::string version_str;
            version_str = config.value().ToString();
            version = std::stoul(version_str);
        }
    }

    std::map<uint32_t, std::pair<std::string, uint16_t>> node_ip_map;
    assert(ips.size() == node_ids.size());
    for (size_t node_idx = 0; node_idx < node_ids.size(); ++node_idx)
    {
        node_ip_map.try_emplace(node_ids[node_idx],
                                std::make_pair(ips[node_idx], ports[node_idx]));
    }

    for (size_t group_idx = 0; group_idx < ng_ids.size(); ++group_idx)
    {
        std::vector<txservice::NodeConfig> group_members_config;
        std::vector<uint32_t> group_members;
        EloqShare::DeserializeStringToVector<uint32_t>(
            ng_members[group_idx],
            ",",
            group_members,
            EloqShare::str_uint32_converter);

        std::vector<bool> group_members_is_candidate;
        EloqShare::DeserializeStringToVector<bool>(
            ng_members_is_candidate[group_idx],
            ",",
            group_members_is_candidate,
            EloqShare::str_bool_converter);
        assert(group_members.size() == group_members_is_candidate.size());
        for (size_t idx = 0; idx < group_members.size(); idx++)
        {
            uint32_t nid = group_members[idx];
            group_members_config.emplace_back(nid,
                                              std::get<0>(node_ip_map.at(nid)),
                                              std::get<1>(node_ip_map.at(nid)),
                                              group_members_is_candidate[idx]);
        }
        ng_configs.try_emplace(ng_ids[group_idx],
                               std::move(group_members_config));
    }

    uninitialized = false;

    DLOG(INFO) << "RocksDBHandler::ReadClusterConfig finished";
    return true;
}

void RocksDBHandler::SerializeFlushRecord(
    const txservice::FlushRecord &flush_rec, std::vector<char> &buf)
{
    if (flush_rec.payload_status_ != txservice::RecordStatus::Deleted)
    {
        int64_t commit_ts = flush_rec.commit_ts_;
        int8_t deleted = 0;

        const txservice::BlobTxRecord *rec =
            dynamic_cast<const txservice::BlobTxRecord *>(flush_rec.Payload());
        assert(rec != nullptr);
        buf.resize(sizeof(int8_t) + sizeof(int64_t) + rec->value_.size());
        char *p = buf.data();

        // encode deleted
        std::memcpy(p, &deleted, sizeof(int8_t));
        p += sizeof(int8_t);
        // encode version
        std::memcpy(p, &commit_ts, sizeof(int64_t));
        p += sizeof(int64_t);
        std::memcpy(p, rec->value_.c_str(), rec->value_.size());
    }
    else
    {
        buf.resize(sizeof(int8_t) + sizeof(int64_t));
        char *p = buf.data();
        int64_t commit_ts = flush_rec.commit_ts_;
        int8_t deleted = 1;

        // encode deleted
        std::memcpy(p, &deleted, sizeof(int8_t));
        p += sizeof(int8_t);
        // encode version
        std::memcpy(p, &commit_ts, sizeof(int64_t));
        p += sizeof(int64_t);
    }
}

void RocksDBHandler::DeserializeToTxRecord(const char *payload,
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
        EloqKV::RedisObjectType obj_type =
            static_cast<EloqKV::RedisObjectType>(obj_type_int8);
        switch (obj_type)
        {
        case EloqKV::RedisObjectType::String:
            typed_rec.reset(new EloqKV::RedisStringObject());
            break;
        case EloqKV::RedisObjectType::List:
            typed_rec.reset(new EloqKV::RedisListObject());
            break;
        case EloqKV::RedisObjectType::Hash:
            typed_rec.reset(new EloqKV::RedisHashObject());
            break;
        case EloqKV::RedisObjectType::Zset:
            typed_rec.reset(new EloqKV::RedisZsetObject());
            break;
        case EloqKV::RedisObjectType::Set:
            typed_rec.reset(new EloqKV::RedisHashSetObject());
            break;
        case EloqKV::RedisObjectType::TTLString:
            typed_rec.reset(new EloqKV::RedisStringTTLObject());
            break;
        case EloqKV::RedisObjectType::TTLSet:
            typed_rec.reset(new EloqKV::RedisHashSetTTLObject());
            break;
        case EloqKV::RedisObjectType::TTLHash:
            typed_rec.reset(new EloqKV::RedisHashTTLObject());
            break;
        case EloqKV::RedisObjectType::TTLList:
            typed_rec.reset(new EloqKV::RedisListTTLObject());
            break;
        case EloqKV::RedisObjectType::TTLZset:
            typed_rec.reset(new EloqKV::RedisZsetTTLObject());
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

void RocksDBHandler::DeserializeRecord(const char *payload,
                                       const size_t payload_size,
                                       std::string &rec_str,
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
    if (deleted == 0)
    {
        is_deleted = false;
        rec_str =
            std::string(p, payload_size - sizeof(int8_t) - sizeof(int64_t));
    }
    else
    {
        is_deleted = true;
    }
}

bool RocksDBHandler::PutAll(std::vector<txservice::FlushRecord> &batch,
                            const txservice::TableName &table_name,
                            const txservice::TableSchema *table_schema,
                            uint32_t node_group)
{
    std::thread::id this_id = std::this_thread::get_id();
    if (batch.empty())
    {
        return true;
    }
    std::shared_lock<std::shared_mutex> db_lk(db_mux_);
    auto db = GetDBPtr();
    if (!db)
    {
        return false;
    }
    rocksdb::WriteOptions write_options;
    write_options.disableWAL = true;
    write_options.no_slowdown = false;
    rocksdb::WriteBatch write_batch;
    const std::string &kv_cf_name =
        table_schema->GetKVCatalogInfo()->kv_table_name_;
    rocksdb::ColumnFamilyHandle *cfh = GetColumnFamilyHandler(kv_cf_name);
    assert(cfh != nullptr);
    uint64_t write_batch_size = 0;
    uint64_t now = txservice::LocalCcShards::ClockTsInMillseconds();
    for (auto &flush_rec : batch)
    {
        txservice::TxKey key = flush_rec.Key();
        const EloqKV::EloqKey *redis_key = key.GetKey<EloqKV::EloqKey>();
        if (flush_rec.payload_status_ == txservice::RecordStatus::Normal &&
            flush_rec.Payload()->GetTTL() > now)
        {
            std::vector<char> rec_buf;
            SerializeFlushRecord(flush_rec, rec_buf);
            write_batch_size += redis_key->Length();
            write_batch_size += rec_buf.size();
            write_batch.Put(
                cfh,
                rocksdb::Slice(redis_key->Buf(), redis_key->Length()),
                rocksdb::Slice(rec_buf.data(), rec_buf.size()));
        }
        else
        {
            write_batch_size += redis_key->Length();
            write_batch.Delete(
                cfh, rocksdb::Slice(redis_key->Buf(), redis_key->Length()));
        }

        if (write_batch_size >= batch_write_size_)
        {
            auto status = db->Write(write_options, &write_batch);
            if (!status.ok())
            {
                LOG(ERROR) << "PutAll end failed, table:" << table_name.String()
                           << ", thread id: " << this_id
                           << ", result:" << static_cast<int>(status.ok())
                           << ", batch size:" << batch.size()
                           << ", error: " << status.ToString()
                           << ", error code: " << status.code();
                return false;
            }
            // collect metrics: flush rows
            if (metrics::enable_kv_metrics)
            {
                metrics::kv_meter->Collect(metrics::NAME_KV_FLUSH_ROWS_TOTAL,
                                           write_batch.Count(),
                                           "base");
            }

            write_batch.Clear();
            write_batch_size = 0;
        }
    }

    if (write_batch_size > 0)
    {
        auto status = db->Write(write_options, &write_batch);

        if (!status.ok())
        {
            LOG(ERROR) << "PutAll end failed, table:" << table_name.String()
                       << ", thread id: " << this_id
                       << ", result:" << static_cast<int>(status.ok())
                       << ", batch size:" << batch.size()
                       << ", error: " << status.ToString()
                       << ", error code: " << status.code();
            return false;
        }
        // collect metrics: flush rows
        if (metrics::enable_kv_metrics)
        {
            metrics::kv_meter->Collect(
                metrics::NAME_KV_FLUSH_ROWS_TOTAL, write_batch.Count(), "base");
        }
    }

    return true;
}

bool RocksDBHandler::CkptEnd(const txservice::TableName &table_name,
                             const txservice::TableSchema *schema,
                             uint32_t node_group,
                             uint64_t version)
{
    std::shared_lock<std::shared_mutex> db_lk(db_mux_);
    auto db = GetDBPtr();
    if (!db)
    {
        return false;
    }
    assert(schema != nullptr);
    const std::string kv_cf_name = schema->GetKVCatalogInfo()->kv_table_name_;
    rocksdb::ColumnFamilyHandle *cfh = GetColumnFamilyHandler(kv_cf_name);
    assert(cfh != nullptr);
    rocksdb::FlushOptions flush_options;
    flush_options.allow_write_stall = true;
    flush_options.wait = true;
    auto status = GetDBPtr()->Flush(flush_options, cfh);
    if (!status.ok())
    {
        LOG(ERROR) << "Unable to flush db with error: " << status.ToString();
        return false;
    }
    return true;
};

void RocksDBHandler::UpsertTable(
    const txservice::TableSchema *old_table_schema,
    const txservice::TableSchema *table_schema,
    txservice::OperationType op_type,
    uint64_t write_time,
    txservice::NodeGroupId ng_id,
    int64_t tx_term,
    txservice::CcHandlerResult<txservice::Void> *hd_res,
    const txservice::AlterTableInfo *alter_table_info,
    txservice::CcRequestBase *cc_req,
    txservice::CcShard *ccs,
    txservice::CcErrorCode *err_code)
{
    switch (op_type)
    {
    case txservice::OperationType::CreateTable:
        LOG(ERROR)
            << "RocksDBHandler::UpsertTable::CreateTable not implemented";
        assert(false);
        break;
    case txservice::OperationType::TruncateTable:
        query_worker_pool_->SubmitWork(
            [this,
             old_table_schema,
             table_schema,
             write_time,
             hd_res,
             ng_id,
             tx_term,
             op_type,
             alter_table_info,
             cc_req,
             ccs,
             err_code]()
            {
                int64_t term;
                if (!txservice::IsStandbyTx(tx_term))
                {
                    term = txservice::Sharder::Instance().TryPinNodeGroupData(
                        ng_id);
                }
                else
                {
                    // for standby node
                    term = txservice::Sharder::Instance()
                               .TryPinStandbyNodeGroupData();
                }

                if (term < 0)
                {
                    if (hd_res != nullptr)
                    {
                        hd_res->SetError(
                            txservice::CcErrorCode::TX_NODE_NOT_LEADER);
                    }
                    else
                    {
                        *err_code = txservice::CcErrorCode::NG_TERM_CHANGED;
                        ccs->Enqueue(cc_req);
                    }
                    return;
                }
                std::shared_ptr<void> defer_unpin(
                    nullptr,
                    [ng_id](void *) {
                        txservice::Sharder::Instance().UnpinNodeGroupData(
                            ng_id);
                    });
                if (term != tx_term)
                {
                    if (hd_res != nullptr)
                    {
                        hd_res->SetError(
                            txservice::CcErrorCode::NG_TERM_CHANGED);
                    }
                    else
                    {
                        *err_code = txservice::CcErrorCode::NG_TERM_CHANGED;
                        ccs->Enqueue(cc_req);
                    }
                    return;
                }

                std::shared_lock<std::shared_mutex> db_lk(db_mux_);
                auto db = GetDBPtr();
                if (!db)
                {
                    std::unique_lock<std::mutex> ddl_lk(pending_ddl_mux_);
                    pending_ddl_req_.emplace(old_table_schema,
                                             table_schema,
                                             op_type,
                                             write_time,
                                             ng_id,
                                             tx_term,
                                             alter_table_info,
                                             cc_req,
                                             ccs,
                                             err_code);
                    return;
                }

                std::unique_lock<std::mutex> ddl_lk(ddl_mux_);
                const std::string &table_name_str =
                    table_schema->GetBaseTableName().String();

                // check catalog version
                std::string catalog_cf_name = table_name_str + "_catalog";
                rocksdb::ColumnFamilyHandle *catalog_cfh =
                    GetColumnFamilyHandler(rocksdb::kDefaultColumnFamilyName);
                rocksdb::PinnableWideColumns pinnable_table_catalog;
                auto status1 = db->GetEntity(rocksdb::ReadOptions(),
                                             catalog_cfh,
                                             catalog_cf_name,
                                             &pinnable_table_catalog);
                if (!status1.ok())
                {
                    assert(false);
                    LOG(ERROR) << "Not found table catalog in data store, "
                               << table_name_str;
                    if (hd_res != nullptr)
                    {
                        hd_res->SetError(
                            txservice::CcErrorCode::DATA_STORE_ERR);
                    }
                    else
                    {
                        *err_code = txservice::CcErrorCode::DATA_STORE_ERR;
                        ccs->Enqueue(cc_req);
                    }
                    return;
                }

                uint64_t store_schema_version = 0;
                const rocksdb::WideColumns &table_catalog_wc1 =
                    pinnable_table_catalog.columns();
                for (auto &column : table_catalog_wc1)
                {
                    if (column.name() == "version")
                    {
                        const rocksdb::Slice &val = column.value();
                        assert(val.size() == sizeof(uint64_t));
                        store_schema_version =
                            *reinterpret_cast<const uint64_t *>(val.data());
                    }
                }

                if (store_schema_version >= table_schema->Version())
                {
                    // has updated, skip it.
                    if (hd_res != nullptr)
                    {
                        hd_res->SetFinished();
                    }
                    else
                    {
                        *err_code = txservice::CcErrorCode::NO_ERROR;
                        ccs->Enqueue(cc_req);
                    }
                    return;
                }

                // Create the new column family and drop the old.
                std::string new_cf_name =
                    table_schema->GetKVCatalogInfo()->kv_table_name_;
                rocksdb::ColumnFamilyHandle *new_cfh;
                auto status = db->CreateColumnFamily(
                    rocksdb::ColumnFamilyOptions(), new_cf_name, &new_cfh);
                if (!status.ok())
                {
                    LOG(ERROR)
                        << "Unable to create column family with error: "
                        << status.ToString() << ", cfname: " << new_cf_name;
                    if (hd_res != nullptr)
                    {
                        hd_res->SetError(
                            txservice::CcErrorCode::DATA_STORE_ERR);
                    }
                    else
                    {
                        *err_code = txservice::CcErrorCode::DATA_STORE_ERR;
                        ccs->Enqueue(cc_req);
                    }
                    return;
                }
                std::string old_cf_name =
                    old_table_schema->GetKVCatalogInfo()->kv_table_name_;
                rocksdb::ColumnFamilyHandle *cfh =
                    GetColumnFamilyHandler(old_cf_name);
                status = db->DropColumnFamily(cfh);
                if (!status.ok())
                {
                    if (status.IsInvalidArgument())
                    {
                        LOG(WARNING)
                            << "Unable to drop column family " << old_cf_name
                            << " with error: " << status.ToString()
                            << ", it may has been dropped, ignore it.";
                    }
                    else
                    {
                        LOG(ERROR)
                            << "Unable to drop column family" << old_cf_name
                            << " with error: " << status.ToString();
                        if (hd_res != nullptr)
                        {
                            hd_res->SetError(
                                txservice::CcErrorCode::DATA_STORE_ERR);
                        }
                        else
                        {
                            *err_code = txservice::CcErrorCode::DATA_STORE_ERR;
                            ccs->Enqueue(cc_req);
                        }
                        return;
                    }
                }
                ResetColumnFamilyHandler(old_cf_name, new_cf_name, new_cfh);

                // Update the table catalog
                std::string table_key = table_name_str + "_catalog";
                rocksdb::WideColumns table_catalog_wc;
                table_catalog_wc.emplace_back("kv_cf_name", new_cf_name);
                uint64_t version = table_schema->Version();
                rocksdb::Slice version_value(
                    reinterpret_cast<const char *>(&version), sizeof(uint64_t));
                table_catalog_wc.emplace_back("version", version_value);
                rocksdb::ColumnFamilyHandle *cfh_default =
                    GetColumnFamilyHandler(rocksdb::kDefaultColumnFamilyName);
                DLOG(INFO) << "Update table catalog: " << table_key
                           << ", from old cf: " << old_cf_name
                           << " to new cf: " << new_cf_name
                           << ", version: " << version;
                status = db->PutEntity(rocksdb::WriteOptions(),
                                       cfh_default,
                                       table_key,
                                       table_catalog_wc);
                if (!status.ok())
                {
                    LOG(ERROR) << "Unable to write catalog info to db with "
                                  "error, table_name: "
                               << table_key << " error: " << status.ToString();
                    if (hd_res != nullptr)
                    {
                        hd_res->SetError(
                            txservice::CcErrorCode::DATA_STORE_ERR);
                    }
                    else
                    {
                        *err_code = txservice::CcErrorCode::DATA_STORE_ERR;
                        ccs->Enqueue(cc_req);
                    }
                    return;
                }
                // unlock ddl mutex
                ddl_lk.unlock();

                rocksdb::FlushOptions flush_options;
                flush_options.allow_write_stall = false;
                flush_options.wait = true;
                status = db->Flush(flush_options);
                if (!status.ok())
                {
                    LOG(ERROR) << "Unable to flush db with error: "
                               << status.ToString();
                    if (hd_res != nullptr)
                    {
                        hd_res->SetError(
                            txservice::CcErrorCode::DATA_STORE_ERR);
                    }
                    else
                    {
                        *err_code = txservice::CcErrorCode::DATA_STORE_ERR;
                        ccs->Enqueue(cc_req);
                    }
                    return;
                }

                if (hd_res != nullptr)
                {
                    hd_res->SetFinished();
                }
                else
                {
                    *err_code = txservice::CcErrorCode::NO_ERROR;
                    ccs->Enqueue(cc_req);
                }
            });
        break;
    case txservice::OperationType::DropTable:
    case txservice::OperationType::Update:
    case txservice::OperationType::AddIndex:
    case txservice::OperationType::DropIndex:
        LOG(ERROR) << "RocksDBHandler::UpsertTable::DropTable/Update/AddIndex/"
                      "DropIndex not implemented";
        assert(false);
        break;
    default:
        LOG(ERROR)
            << "Unsupported command for RocksDBHanlder::UpsertTable, op_type: "
            << static_cast<int>(op_type);
        break;
    }
}

void RocksDBHandler::FetchTableCatalog(
    const txservice::TableName &ccm_table_name,
    txservice::FetchCatalogCc *fetch_cc)
{
    query_worker_pool_->SubmitWork(
        [this, ccm_table_name, fetch_cc]()
        {
            std::shared_lock<std::shared_mutex> db_lk(db_mux_);
            auto db = GetDBPtr();
            if (!db)
            {
                fetch_cc->SetFinish(
                    txservice::RecordStatus::Unknown,
                    (int) txservice::CcErrorCode::DATA_STORE_ERR);
                return;
            }
            std::string table_key = ccm_table_name.String() + "_catalog";
            rocksdb::ColumnFamilyHandle *cfh =
                GetColumnFamilyHandler(rocksdb::kDefaultColumnFamilyName);
            rocksdb::PinnableWideColumns pinnable_table_catalog;
            auto status = GetDBPtr()->GetEntity(rocksdb::ReadOptions(),
                                                cfh,
                                                table_key,
                                                &pinnable_table_catalog);
            if (!status.ok())
            {
                fetch_cc->SetFinish(txservice::RecordStatus::Deleted, 0);
                return;
            }
            std::string kv_cf_name;
            const rocksdb::WideColumns &table_catalog_wc =
                pinnable_table_catalog.columns();
            for (auto &column : table_catalog_wc)
            {
                if (column.name() == "version")
                {
                    const rocksdb::Slice &val = column.value();
                    assert(val.size() == sizeof(uint64_t));
                    uint64_t version =
                        *reinterpret_cast<const uint64_t *>(val.data());
                    fetch_cc->SetCommitTs(version);
                }
                if (column.name() == "kv_cf_name")
                {
                    const rocksdb::Slice &val = column.value();
                    kv_cf_name = val.ToString();
                }
            }
            std::string &catalog_image = fetch_cc->CatalogImage();
            // catalog image stores only kv_table_name
            catalog_image.append(kv_cf_name);
            fetch_cc->SetFinish(txservice::RecordStatus::Normal, 0);
        });
}

void RocksDBHandler::FetchCurrentTableStatistics(
    const txservice::TableName &ccm_table_name,
    txservice::FetchTableStatisticsCc *fetch_cc)
{
    LOG(ERROR) << "RocksDBHandler::FetchCurrentTableStatistics not "
                  "implemented";
    // Not implemented
    fetch_cc->SetFinish(0);
}

void RocksDBHandler::FetchTableStatistics(
    const txservice::TableName &ccm_table_name,
    txservice::FetchTableStatisticsCc *fetch_cc)
{
    LOG(ERROR) << "RocksDBHandler::FetchTableStatistics not implemented";
    // Not implemented
    assert(false);
}

bool RocksDBHandler::UpsertTableStatistics(
    const txservice::TableName &ccm_table_name,
    const std::unordered_map<txservice::TableName,
                             std::pair<uint64_t, std::vector<txservice::TxKey>>>
        &sample_pool_map,
    uint64_t version)
{
    LOG(ERROR) << "RocksDBHandler::UpsertTableStatistics not implemented";
    // Not implemented
    assert(false);
    return true;
}

void RocksDBHandler::FetchTableRanges(txservice::FetchTableRangesCc *fetch_cc)
{
    LOG(ERROR) << "RocksDBHandler::FetchTableRanges not implemented";
    // Not implemented
    assert(false);
}

void RocksDBHandler::FetchRangeSlices(txservice::FetchRangeSlicesReq *fetch_cc)
{
    LOG(ERROR) << "RocksDBHandler::FetchRangeSlices not implemented";
    // Not implemented
    assert(false);
}

bool DeleteOutOfRangeDataInternal(std::string delete_from_partition_sql,
                                  int32_t partition_id,
                                  const txservice::TxKey *start_k)
{
    LOG(ERROR) << "RocksDBHandler::DeleteOutOfRangeDataInternal not "
                  "implemented";
    // Not implemented
    assert(false);
    return true;
}

bool RocksDBHandler::DeleteOutOfRangeData(
    const txservice::TableName &table_name,
    int32_t partition_id,
    const txservice::TxKey *start_key,
    const txservice::TableSchema *table_schema)
{
    LOG(ERROR) << "RocksDBHandler::DeleteOutOfRangeData not implemented";
    // Not implemented
    assert(false);
    return true;
}

bool RocksDBHandler::GetNextRangePartitionId(
    const txservice::TableName &tablename,
    uint32_t range_cnt,
    int32_t &out_next_partition_id,
    int retry_count)
{
    LOG(ERROR) << "RocksDBHandler::GetNextRangePartitionId not implemented";
    // Not implemented
    assert(false);
    return true;
}

bool RocksDBHandler::Read(const txservice::TableName &table_name,
                          const txservice::TxKey &key,
                          txservice::TxRecord &rec,
                          bool &found,
                          uint64_t &version_ts,
                          const txservice::TableSchema *table_schema)
{
    LOG(ERROR) << "RocksDBHandler::Read not implemented";
    // Not implemented
    assert(false);
    return true;
}

#ifdef ON_KEY_OBJECT
txservice::store::DataStoreHandler::DataStoreOpStatus
RocksDBHandler::FetchRecord(txservice::FetchRecordCc *fetch_cc)
{
    const EloqKey *redis_key_ptr = fetch_cc->tx_key_.GetKey<EloqKV::EloqKey>();
    EloqKey redis_key_copy(*redis_key_ptr);
    txservice::TableName tbl_name_copy(*fetch_cc->table_name_);
    if (metrics::enable_kv_metrics)
    {
        fetch_cc->start_ = metrics::Clock::now();
    }

    query_worker_pool_->SubmitWork(
        [this,
         fetch_cc,
         redis_key = std::move(redis_key_copy),
         table_name = std::move(tbl_name_copy)]()
        {
            std::shared_lock<std::shared_mutex> db_lk(db_mux_);
            auto db = GetDBPtr();
            if (!db)
            {
                fetch_cc->SetFinish(
                    static_cast<int>(txservice::CcErrorCode::DATA_STORE_ERR));
                return;
            }
            std::string value;
            const std::string &kv_cf_name =
                fetch_cc->table_schema_->GetKVCatalogInfo()->kv_table_name_;
            rocksdb::ColumnFamilyHandle *cfh =
                GetColumnFamilyHandler(kv_cf_name);
            assert(cfh != nullptr);
            rocksdb::Status status =
                db->Get(rocksdb::ReadOptions(),
                        cfh,
                        rocksdb::Slice(redis_key.Buf(), redis_key.Length()),
                        &value);
            if (metrics::enable_kv_metrics)
            {
                metrics::kv_meter->CollectDuration(
                    metrics::NAME_KV_READ_DURATION, fetch_cc->start_);
                metrics::kv_meter->Collect(metrics::NAME_KV_READ_TOTAL, 1);
            }
            if (!status.ok())
            {
                if (status.IsNotFound())
                {
                    fetch_cc->rec_ts_ = 1;
                    fetch_cc->rec_status_ = txservice::RecordStatus::Deleted;
                    fetch_cc->SetFinish(
                        static_cast<int>(txservice::CcErrorCode::NO_ERROR));
                }
                else
                {
                    fetch_cc->SetFinish(static_cast<int>(
                        txservice::CcErrorCode::DATA_STORE_ERR));
                }
                return;
            }
            const char *payload = value.data();
            const size_t payload_size = value.size();
            bool is_deleted = false;
            int64_t version_ts = 0;
            std::string rec_str;
            DeserializeRecord(
                payload, payload_size, rec_str, is_deleted, version_ts);
            if (!is_deleted)
            {
                fetch_cc->rec_status_ = txservice::RecordStatus::Normal;
                fetch_cc->rec_str_ = rec_str;
            }
            else
            {
                fetch_cc->rec_status_ = txservice::RecordStatus::Deleted;
            }
            fetch_cc->rec_ts_ = version_ts;
            fetch_cc->SetFinish(
                static_cast<int>(txservice::CcErrorCode::NO_ERROR));
        });
    return DataStoreOpStatus::Success;
}

rocksdb::ColumnFamilyHandle *RocksDBHandler::GetColumnFamilyHandler(
    const std::string &cf)
{
    auto cfh = column_families_.find(cf);
    if (cfh != column_families_.cend())
    {
        std::unique_ptr<rocksdb::ColumnFamilyHandle> &cfh_ptr = cfh->second;
        return cfh_ptr.get();
    }
    return nullptr;
}

void RocksDBHandler::ResetColumnFamilyHandler(const std::string &old_cf,
                                              const std::string &new_cf,
                                              rocksdb::ColumnFamilyHandle *cfh)
{
    auto cfh_entry = column_families_.find(old_cf);
    assert(cfh_entry != column_families_.cend());
    std::unique_ptr<rocksdb::ColumnFamilyHandle> &cfh_ptr = cfh_entry->second;
    rocksdb::ColumnFamilyHandle *old_cfh = cfh_ptr.release();
    GetDBPtr()->DestroyColumnFamilyHandle(old_cfh);
    column_families_.erase(cfh_entry);
    assert(new_cf == cfh->GetName());
    auto pair = column_families_.emplace(new_cf, cfh);
    assert(pair.second);
}
#endif

std::unique_ptr<txservice::store::DataStoreScanner> RocksDBHandler::ScanForward(
    const txservice::TableName &table_name,
    uint32_t ng_id,
    const txservice::TxKey &start_key,
    bool inclusive,
    uint8_t key_parts,
    const std::vector<txservice::store::DataStoreSearchCond> &search_cond,
    const txservice::KeySchema *key_schema,
    const txservice::RecordSchema *rec_schema,
    const txservice::KVCatalogInfo *kv_info,
    bool scan_foward)
{
    const std::string &kv_cf_name = kv_info->kv_table_name_;
    rocksdb::ColumnFamilyHandle *cfh = GetColumnFamilyHandler(kv_cf_name);
    std::unique_ptr<RocksDBScanner> scanner =
        std::make_unique<RocksDBScanner>(GetDBPtr(),
                                         cfh,
                                         key_schema,
                                         rec_schema,
                                         table_name,
                                         kv_info,
                                         start_key.GetKey<EloqKV::EloqKey>(),
                                         inclusive,
                                         search_cond,
                                         scan_foward);
    scanner->Init();
    return scanner;
}

txservice::store::DataStoreHandler::DataStoreOpStatus
RocksDBHandler::LoadRangeSlice(const txservice::TableName &table_name,
                               const txservice::KVCatalogInfo *kv_info,
                               uint32_t range_partition_id,
                               txservice::LoadRangeSliceRequest *load_slice_req)
{
    LOG(ERROR) << "RocksDBHandler::LoadRangeSlice not implemented";
    // Not implemented
    assert(false);
    return txservice::store::DataStoreHandler::DataStoreOpStatus::Error;
}

bool RocksDBHandler::UpdateRangeSlices(
    const txservice::TableName &table_name,
    uint64_t version,
    txservice::TxKey range_start_key,
    std::vector<const txservice::StoreSlice *> slices,
    int32_t partition_id,
    uint64_t range_version)
{
    LOG(ERROR) << "RocksDBHandler::UpdateRangeSlices not implemented";
    // Not implemented
    assert(false);
    return true;
}

bool RocksDBHandler::UpsertRanges(
    const txservice::TableName &table_name,
    std::vector<txservice::SplitRangeInfo> range_info,
    uint64_t version)
{
    LOG(ERROR) << "RocksDBHandler::UpsertRanges not implemented";
    // Not implemented
    assert(false);
    return true;
}

bool RocksDBHandler::FetchTable(const txservice::TableName &table_name,
                                std::string &schema_image,
                                bool &found,
                                uint64_t &version_ts) const
{
    LOG(ERROR) << "RocksDBHandler::FetchTable not implemented";
    // Not implemented
    assert(false);
    return true;
}

bool RocksDBHandler::DiscoverAllTableNames(
    std::vector<std::string> &norm_name_vec,
    const std::function<void()> *yield_fptr,
    const std::function<void()> *resume_fptr) const
{
    LOG(ERROR) << "RocksDBHandler::DiscoverAllTableNames not implemented";
    // Not implemented
    assert(false);
    return true;
}

//-- database
bool RocksDBHandler::UpsertDatabase(std::string_view db,
                                    std::string_view definition) const
{
    DLOG(ERROR) << "RocksDBHandler::UpsertDatabase not implemented";
    // Not implemented
    assert(false);
    DLOG(INFO) << "RocksDBHandler::UpsertDatabase finished";
    return true;
}

bool RocksDBHandler::DropDatabase(std::string_view db) const
{
    LOG(ERROR) << "RocksDBHandler::DropDatabase not implemented";
    // Not implemented
    assert(false);
    return true;
}

bool RocksDBHandler::FetchDatabase(
    std::string_view db,
    std::string &definition,
    bool &found,
    const std::function<void()> *yield_fptr,
    const std::function<void()> *resume_fptr) const
{
    LOG(ERROR) << "RocksDBHandler::FetchDatabase not implemented";
    // Not implemented
    assert(false);
    return true;
}

bool RocksDBHandler::FetchAllDatabase(
    std::vector<std::string> &dbnames,
    const std::function<void()> *yield_fptr,
    const std::function<void()> *resume_fptr) const
{
    LOG(ERROR) << "RocksDBHandler::FetchAllDatabase not implemented";
    // Not implemented
    assert(false);
    return true;
}

bool RocksDBHandler::DropKvTable(const std::string &kv_table_name) const
{
    LOG(ERROR) << "RocksDBHandler::DropKvTable not implemented";
    // Not implemented
    assert(false);
    DLOG(INFO) << "RocksDBHandler::DropKvTable finished";
    return true;
}

void RocksDBHandler::DropKvTableAsync(const std::string &kv_table_name) const
{
    LOG(ERROR) << "RocksDBHandler::DropKvTableAsync not implemented";
    // Not implemented
    assert(false);
    return;
}

void RocksDBHandler::SetTxService(txservice::TxService *tx_service)
{
    tx_service_ = tx_service;
}

std::string RocksDBHandler::CreateKVCatalogInfo(
    const txservice::TableSchema *table_schema) const
{
    LOG(ERROR) << "RocksDBHandler::CreateKVCatalogInfo not implemented";
    // Not implemented
    assert(false);
    return "";
}

txservice::KVCatalogInfo::uptr RocksDBHandler::DeserializeKVCatalogInfo(
    const std::string &kv_info_str, size_t &offset) const
{
    LOG(ERROR) << "RocksDBHandler::DeserializeKVCatalogInfo not implemented";
    // Not implemented
    assert(false);
    return nullptr;
}

std::string RocksDBHandler::CreateNewKVCatalogInfo(
    const txservice::TableName &table_name,
    const txservice::TableSchema *current_table_schema,
    txservice::AlterTableInfo &alter_table_info)
{
    LOG(ERROR) << "RocksDBHandler::CreateNewKVCatalogInfo not implemented";
    // Not implemented
    assert(false);
    return "";
}

/**
 * @brief Write batch historical versions into DataStore.
 *
 */
bool RocksDBHandler::PutArchivesAll(uint32_t node_group,
                                    const txservice::TableName &table_name,
                                    const txservice::KVCatalogInfo *kv_info,
                                    std::vector<txservice::FlushRecord> &batch)
{
    LOG(ERROR) << "RocksDBHandler::PutArchivesAll not implemented";
    // Not implemented
    assert(false);
    return true;
}
/**
 * @brief Copy record from base/sk table to mvcc_archives.
 */
bool RocksDBHandler::CopyBaseToArchive(
    std::vector<txservice::TxKey> &batch,
    uint32_t node_group,
    const txservice::TableName &table_name,
    const txservice::TableSchema *table_schema)
{
    LOG(ERROR) << "RocksDBHandler::CopyBaseToArchive not implemented";
    // Not implemented
    assert(false);
    return true;
}

/**
 * @brief  Get the latest visible(commit_ts <= upper_bound_ts) historical
 * version.
 */
bool RocksDBHandler::FetchVisibleArchive(
    const txservice::TableName &table_name,
    const txservice::KVCatalogInfo *kv_info,
    const txservice::TxKey &key,
    const uint64_t upper_bound_ts,
    txservice::TxRecord &rec,
    txservice::RecordStatus &rec_status,
    uint64_t &commit_ts)
{
    LOG(ERROR) << "RocksDBHandler::FetchVisibleArchive not implemented";
    // Not implemented
    assert(false);
    return true;
}

/**
 * @brief  Fetch all archives whose commit_ts >= from_ts.
 */
bool RocksDBHandler::FetchArchives(
    const txservice::TableName &table_name,
    const txservice::KVCatalogInfo *kv_info,
    const txservice::TxKey &key,
    std::vector<txservice::VersionTxRecord> &archives,
    uint64_t from_ts)
{
    LOG(ERROR) << "RocksDBHandler::FetchArchives not implemented";
    // Not implemented
    assert(false);
    return true;
}

bool RocksDBHandler::UpdateClusterConfig(
    const std::unordered_map<uint32_t, std::vector<txservice::NodeConfig>>
        &new_cnf,
    uint64_t version)
{
    std::vector<uint32_t> ng_ids;
    std::vector<std::string> ng_members;
    std::vector<std::string> ng_members_is_candidate;
    std::vector<uint32_t> node_ids;
    std::vector<std::string> ips;
    std::vector<uint16_t> ports;

    std::vector<txservice::NodeConfig> nodes;
    txservice::ExtractNodesConfigs(new_cnf, nodes);
    for (const txservice::NodeConfig &node : nodes)
    {
        node_ids.push_back(node.node_id_);
        ips.push_back(node.host_name_.c_str());
        ports.push_back(node.port_);
    }

    for (auto ng_pair : new_cnf)
    {
        ng_ids.push_back(ng_pair.first);
        std::vector<uint32_t> group_members;
        std::vector<bool> group_members_is_candidate;
        for (auto &node : ng_pair.second)
        {
            group_members.push_back(node.node_id_);
            group_members_is_candidate.push_back(node.is_candidate_);
        }
        ng_members.push_back(EloqShare::SerializeVectorToString<uint32_t>(
            group_members, ",", EloqShare::uint32_str_converter));
        ng_members_is_candidate.push_back(
            EloqShare::SerializeVectorToString<bool>(
                group_members_is_candidate,
                ",",
                EloqShare::bool_str_converter));
    }

    std::string node_ids_str = EloqShare::SerializeVectorToString<uint32_t>(
        node_ids, " ", EloqShare::uint32_str_converter);
    std::string ips_str = EloqShare::SerializeVectorToString<std::string>(
        ips, " ", EloqShare::string_str_converter);
    std::string ports_str = EloqShare::SerializeVectorToString<uint16_t>(
        ports, " ", EloqShare::uint16_str_converter);
    std::string ng_ids_str = EloqShare::SerializeVectorToString<uint32_t>(
        ng_ids, " ", EloqShare::uint32_str_converter);
    std::string ng_members_str =
        EloqShare::SerializeVectorToString<std::string>(
            ng_members, " ", EloqShare::string_str_converter);
    std::string ng_members_is_candidate_str =
        EloqShare::SerializeVectorToString<std::string>(
            ng_members_is_candidate, " ", EloqShare::string_str_converter);
    std::string verstion_str = std::to_string(version);

    // Create a rocksdb::WideColumns object
    rocksdb::WideColumns cluster_config_wc;
    cluster_config_wc.emplace_back("node_ids", node_ids_str);
    cluster_config_wc.emplace_back("ips", ips_str);
    cluster_config_wc.emplace_back("ports", ports_str);
    cluster_config_wc.emplace_back("ng_ids", ng_ids_str);
    cluster_config_wc.emplace_back("ng_members", ng_members_str);
    cluster_config_wc.emplace_back("ng_members_is_candidate",
                                   ng_members_is_candidate_str);
    cluster_config_wc.emplace_back("version", verstion_str);

    // Write the cluster config to the rocksdb
    rocksdb::WriteOptions write_options;
    auto db = GetDBPtr();
    auto status =
        db->PutEntity(write_options,
                      GetColumnFamilyHandler(rocksdb::kDefaultColumnFamilyName),
                      "cluster_config",
                      cluster_config_wc);

    if (!status.ok())
    {
        LOG(ERROR) << "Unable to write to db with error: " << status.ToString();
        return false;
    }

    rocksdb::FlushOptions flush_options;
    flush_options.allow_write_stall = false;
    flush_options.wait = true;
    status = db->Flush(flush_options);
    if (!status.ok())
    {
        LOG(ERROR) << "Unable to write to db with error: " << status.ToString();
        return false;
    }
    return true;
}

bool RocksDBHandler::NeedCopyRange() const
{
    return true;
}

void RocksDBHandler::ParallelIterateTable(
    txservice::NodeGroupId cc_ng_id,
    uint64_t cc_ng_term,
    std::shared_ptr<std::queue<std::pair<std::string, std::string>>>
        table_names,
    uint16_t core_cnt,
    std::shared_ptr<bthread::Mutex> task_mutex,
    const size_t batch_size,
    const size_t concurrent_cc_count,
    std::shared_ptr<std::atomic<txservice::CcErrorCode>>
        cancel_data_loading_on_error,
    std::shared_ptr<std::atomic<uint16_t>> on_flying_count)
{
    // Initialize a list of RestoreCcMapCc
    txservice::CcRequestPool<txservice::RestoreCcMapCc> cc_pool(
        concurrent_cc_count);
    txservice::LocalCcShards *local_cc_shards =
        txservice::Sharder::Instance().GetLocalCcShards();

    // keep table_name to prevent invalid pointer
    // std::unordered_set<txservice::TableName> table_name_holder;
    rocksdb::Options options;
    rocksdb::ReadOptions read_options;
    read_options.async_io = true;

    std::unique_lock<bthread::Mutex> lk(*task_mutex);
    while (!table_names->empty())
    {
        std::string kv_cf_name = table_names->front().second;
        txservice::TableName table_name(table_names->front().first,
                                        txservice::TableType::Primary);
        table_names->pop();
        lk.unlock();
        rocksdb::ColumnFamilyHandle *cfh = GetColumnFamilyHandler(kv_cf_name);
        std::unique_ptr<rocksdb::Iterator> it =
            std::unique_ptr<rocksdb::Iterator>(
                GetDBPtr()->NewIterator(read_options, cfh));
        if (!it->status().ok())
        {
            LOG(ERROR) << "Create iterator failed: " << it->status().ToString();
            txservice::CcErrorCode expected = txservice::CcErrorCode::NO_ERROR;
            cancel_data_loading_on_error->compare_exchange_strong(
                expected, txservice::CcErrorCode::DATA_STORE_ERR);
            return;
        }

        auto start_time = std::chrono::steady_clock::now();

        txservice::RestoreCcMapCc *cc = cc_pool.NextRequest();
        while (cc == nullptr)
        {
            //  sleep 100u if on fly cc count reachs limit
            bthread_usleep(100);
            cc = cc_pool.NextRequest();
        }
        cc->Reset(&table_name,
                  cc_ng_id,
                  cc_ng_term,
                  core_cnt,
                  cancel_data_loading_on_error.get());

        size_t cnt = 0;
        for (it->SeekToFirst(); it->Valid(); it->Next())
        {
            rocksdb::Slice key = it->key();
            std::string key_str = std::string(key.data(), key.size());

            rocksdb::Slice val = it->value();
            std::string val_str = std::string(val.data(), val.size());
            size_t hash = EloqKey::Hash(key_str.data(), key_str.size());
            // Uses the lower 10 bits of the hash code to shard the key across
            // CPU cores at this node.
            uint16_t core_code = hash & 0x3FF;
            uint16_t core_id = core_code % core_cnt;
            int64_t version_ts;
            bool is_deleted;
            std::string rec_str;
            DeserializeRecord(val_str.data(),
                              val_str.size(),
                              rec_str,
                              is_deleted,
                              version_ts);
            if (!is_deleted)
            {
                cc->AddDataItem(core_id,
                                std::move(key_str),
                                std::move(rec_str),
                                version_ts,
                                is_deleted);
                cnt++;
            }

            if (cnt % batch_size == 0)
            {
                if (cancel_data_loading_on_error->load(
                        std::memory_order_acquire) !=
                    txservice::CcErrorCode::NO_ERROR)
                {
                    cc->Free();
                    break;
                }
                for (uint16_t core = 0; core < core_cnt; core++)
                {
                    local_cc_shards->EnqueueToCcShard(core, cc);
                }
                cc = cc_pool.NextRequest();
                while (cc == nullptr)
                {
                    //  sleep 100u if on fly cc count reachs limit
                    bthread_usleep(100);
                    cc = cc_pool.NextRequest();
                }
                cc->Reset(&table_name,
                          cc_ng_id,
                          cc_ng_term,
                          core_cnt,
                          cancel_data_loading_on_error.get());
            }
        }

        // submit remaining data item for processing
        if (cnt % batch_size != 0)
        {
            if (cancel_data_loading_on_error->load(std::memory_order_acquire) ==
                txservice::CcErrorCode::NO_ERROR)
            {
                for (uint16_t core = 0; core < core_cnt; core++)
                {
                    local_cc_shards->EnqueueToCcShard(core, cc);
                }
            }
            else
            {
                cc->Free();
            }
        }
        else
        {
            cc->Free();
        }

        auto end_time = std::chrono::steady_clock::now();
        auto elapsed_seconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(end_time -
                                                                  start_time)
                .count();
        DLOG(INFO) << "Iteratoring table " << table_name.String()
                   << " with key counts of " << cnt << " ,costs "
                   << elapsed_seconds << " milliseconds."
                   << "(thread: " << std::this_thread::get_id() << ")"
                   << std::endl;
        // wait for all cc finish
        while (!cc_pool.IsAllFree())
        {
            bthread_usleep(100);
        }
        if (cancel_data_loading_on_error->load(std::memory_order_acquire) !=
            txservice::CcErrorCode::NO_ERROR)
        {
            DLOG(ERROR) << "break on error: "
                        << static_cast<uint8_t>(
                               cancel_data_loading_on_error->load(
                                   std::memory_order_acquire));
            break;
        }
        // set ccm has full entries if all data restored
        // 1) all data retored for a table
        // 2) table has no data at all
        txservice::WaitableCc ccm_has_full_entries_cc;
        ccm_has_full_entries_cc.Reset(
            [&table_name, cc_ng_id, cc_ng_term](txservice::CcShard &ccs)
            {
                txservice::CcMap *ccm = ccs.GetCcm(table_name, cc_ng_id);
                if (ccm == nullptr)
                {
                    const txservice::CatalogEntry *catalog_entry =
                        ccs.InitCcm(table_name, cc_ng_id, cc_ng_term, nullptr);
                    if (catalog_entry == nullptr)
                    {
                        return false;
                    }
                    ccm = ccs.GetCcm(table_name, cc_ng_id);
                }
                ccm->ccm_has_full_entries_ = true;
                return true;
            },
            core_cnt);

        for (uint16_t core = 0; core < core_cnt; core++)
        {
            local_cc_shards->EnqueueToCcShard(core, &ccm_has_full_entries_cc);
        }
        ccm_has_full_entries_cc.Wait();
        lk.lock();
    }

    uint16_t old_flying_cnt =
        on_flying_count->fetch_sub(1, std::memory_order_seq_cst);
    DLOG(INFO) << "parallel rocksdb iterate thread done! ("
               << std::this_thread::get_id() << ")"
               << " flying_cnt: " << old_flying_cnt;
};

void RocksDBHandler::RestoreTxCache(txservice::NodeGroupId cc_ng_id,
                                    int64_t cc_ng_term)
{
    txservice::LocalCcShards *local_cc_shards =
        txservice::Sharder::Instance().GetLocalCcShards();
    uint16_t core_cnt = local_cc_shards->Count();
    uint16_t thread_num = core_cnt < 2 ? 1 : (core_cnt / 2);

    query_worker_pool_->SubmitWork(
        [this, cc_ng_id, cc_ng_term, core_cnt, thread_num]
        {
            LOG(INFO) << "Start restore Tx service cache from KV store when KV "
                         "is enabled and cache replacement is disabled.";

            txservice::LocalCcShards *local_cc_shards =
                txservice::Sharder::Instance().GetLocalCcShards();

            std::vector<txservice::TableName> restore_table_names;
            for (auto &pre_built_table : pre_built_tables_)
            {
                txservice::TableName restore_table_name(
                    pre_built_table.first, txservice::TableType::Primary);
                restore_table_names.push_back(std::move(restore_table_name));
            }

            // Check if data has been restored by last term
            // and the CcMap is survivied.
            std::atomic<bool> all_ccm_has_full_entries = true;
            txservice::WaitableCc check_ccm_has_full_entries_cc;
            check_ccm_has_full_entries_cc.Reset(
                [&restore_table_names, cc_ng_id, &all_ccm_has_full_entries](
                    txservice::CcShard &ccs)
                {
                    for (auto &table_name : restore_table_names)
                    {
                        if (!all_ccm_has_full_entries.load(
                                std::memory_order_acquire))
                        {
                            break;
                        }
                        txservice::CcMap *ccm =
                            ccs.GetCcm(table_name, cc_ng_id);
                        if (ccm == nullptr || !ccm->ccm_has_full_entries_)
                        {
                            bool expected = true;
                            all_ccm_has_full_entries.compare_exchange_strong(
                                expected, false);
                            break;
                        }
                    }
                    return true;
                },
                core_cnt);

            for (uint16_t core = 0; core < core_cnt; core++)
            {
                local_cc_shards->EnqueueToCcShard(
                    core, &check_ccm_has_full_entries_cc);
            }
            check_ccm_has_full_entries_cc.Wait();
            // Skip TX cache restore if all ccm has full entries
            if (all_ccm_has_full_entries.load(std::memory_order_acquire))
            {
                LOG(WARNING) << "Previous restore TX service cache completed; "
                                "skipping current operation";
                return;
            }

            auto cancel_data_loading_on_error =
                std::make_shared<std::atomic<txservice::CcErrorCode>>(
                    txservice::CcErrorCode::NO_ERROR);
            auto on_flying_count =
                std::make_shared<std::atomic<uint16_t>>(thread_num);
            std::shared_ptr<std::queue<std::pair<std::string, std::string>>>
                kv_table_names = std::make_shared<
                    std::queue<std::pair<std::string, std::string>>>();
            std::shared_ptr<bthread::Mutex> task_mux =
                std::make_shared<bthread::Mutex>();
            rocksdb::ColumnFamilyHandle *default_cfh =
                GetColumnFamilyHandler(rocksdb::kDefaultColumnFamilyName);

            for (const auto &table_name : restore_table_names)
            {
                std::string table_key = table_name.String() + "_catalog";
                rocksdb::PinnableWideColumns pinnable_table_catalog;
                GetDBPtr()->GetEntity(rocksdb::ReadOptions(),
                                      default_cfh,
                                      table_key,
                                      &pinnable_table_catalog);
                const rocksdb::WideColumns &table_catalog_wc =
                    pinnable_table_catalog.columns();
                for (auto &column : table_catalog_wc)
                {
                    if (column.name() == "kv_cf_name")
                    {
                        const rocksdb::Slice &val = column.value();
                        kv_table_names->emplace(table_name.String(),
                                                val.ToString());
                    }
                }
            }

            for (uint16_t thd_id = 0; thd_id < thread_num; thd_id++)
            {
                query_worker_pool_->SubmitWork(
                    [this,
                     cc_ng_id,
                     cc_ng_term,
                     kv_table_names,
                     task_mux,
                     core_cnt,
                     cancel_data_loading_on_error,
                     on_flying_count]
                    {
                        ParallelIterateTable(cc_ng_id,
                                             cc_ng_term,
                                             kv_table_names,
                                             core_cnt,
                                             task_mux,
                                             640,
                                             500,
                                             cancel_data_loading_on_error,
                                             on_flying_count);
                    });
            }

            // wait until all flying task finish
            while (on_flying_count->load(std::memory_order_relaxed) > 0)
            {
                bthread_usleep(100);
            }

            txservice::CcErrorCode error_code =
                cancel_data_loading_on_error->load(std::memory_order_acquire);
            if (error_code == txservice::CcErrorCode::NO_ERROR)
            {
                LOG(INFO) << "Restore Tx service cache finish";
            }
            else if (error_code == txservice::CcErrorCode::NG_TERM_CHANGED)
            {
                LOG(WARNING)
                    << "Restore Tx service cache abort due to term change";
            }
            else if (error_code == txservice::CcErrorCode::OUT_OF_MEMORY)
            {
                LOG(ERROR) << "Restore Tx service cache failed due to run out "
                              "of memory, please "
                              "shutdown";
                std::cerr << "Restore Tx service cache failed due to run out "
                             "of memory, please "
                             "shutdown "
                             "server using ctrl+c!"
                          << std::endl;
            }
            else
            {
                LOG(ERROR) << "Restore Tx service cache failed due to error "
                           << static_cast<uint8_t>(error_code)
                           << ", please shutdown server";
                std::cerr << "Restore Tx service cache failed due to error "
                          << static_cast<uint8_t>(error_code)
                          << ", please shutdown"
                             "server using ctrl+c!"
                          << std::endl;
            }
        });
}

bool RocksDBHandler::OnLeaderStart(uint32_t *next_leader_node)
{
    bthread::Mutex mux;
    bthread::ConditionVariable cv;
    bool succ = false;
    bool finished = false;

    db_manage_worker_->SubmitWork(
        [this, &succ, &mux, &cv, &finished, next_leader_node]
        {
            bool res = StartDB(true, next_leader_node);
            std::unique_lock<bthread::Mutex> lk(mux);
            succ = res;
            DLOG_IF(INFO, !res) << "OnLeaderStart, failed to start rocksdb";
            finished = true;
            cv.notify_one();
        });
    std::unique_lock<bthread::Mutex> lk(mux);
    while (!finished)
    {
        cv.wait(lk);
    }

    return succ;
}

void RocksDBHandler::OnStartFollowing()
{
    // shutdown previous opened db
    bthread::Mutex mux;
    bthread::ConditionVariable cv;
    bool finished = false;

    db_manage_worker_->SubmitWork(
        [this, &mux, &cv, &finished]
        {
            Shutdown();
            // remove outdated db snapshot
            std::unique_lock<bthread::Mutex> lk(mux);
            finished = true;
            cv.notify_one();
        });
    std::unique_lock<bthread::Mutex> lk(mux);
    while (!finished)
    {
        cv.wait(lk);
    }
}

void RocksDBHandler::OnShutdown()
{
    // Save current node gorup leader if this node is a standby
    if (txservice::Sharder::Instance().StandbyNodeTerm() > 0)
    {
        uint32_t followed_leader = txservice::Sharder::Instance().LeaderNodeId(
            txservice::Sharder::Instance().NativeNodeGroup());
        std::shared_lock<std::shared_mutex> db_lk(db_mux_);
        auto db = GetDBPtr();
        if (!db)
        {
            return;
        }
        auto status =
            db->Put(rocksdb::WriteOptions(),
                    GetColumnFamilyHandler(rocksdb::kDefaultColumnFamilyName),
                    rocksdb::Slice("followed_leader"),
                    rocksdb::Slice(std::to_string(followed_leader)));
        if (!status.ok())
        {
            LOG(ERROR)
                << "Unable to write current ng leader on shutdown with error: "
                << status.ToString();
            return;
        }

        rocksdb::FlushOptions flush_options;
        flush_options.allow_write_stall = false;
        flush_options.wait = true;
        status = db->Flush(flush_options);
        if (!status.ok())
        {
            LOG(ERROR)
                << "Unable to write current ng leader on shutdown with error: "
                << status.ToString();
            return;
        }
    }
    else if (txservice::Sharder::Instance().LeaderTerm(
                 txservice::Sharder::Instance().NativeNodeGroup()) > 0)
    {
        std::shared_lock<std::shared_mutex> db_lk(db_mux_);
        auto db = GetDBPtr();
        auto status = db->Delete(
            rocksdb::WriteOptions(),
            GetColumnFamilyHandler(rocksdb::kDefaultColumnFamilyName),
            rocksdb::Slice("followed_leader"));
        if (!status.ok())
        {
            LOG(ERROR)
                << "Unable to write current ng leader on shutdown with error: "
                << status.ToString();
            return;
        }
        rocksdb::FlushOptions flush_options;
        flush_options.allow_write_stall = false;
        flush_options.wait = true;
        status = db->Flush(flush_options);
        if (!status.ok())
        {
            LOG(ERROR)
                << "Unable to write current ng leader on shutdown with error: "
                << status.ToString();
            return;
        }
    }
}

#if ROCKSDB_CLOUD_FS()
RocksDBCloudHandlerImpl::RocksDBCloudHandlerImpl(
    const EloqShare::RocksDBCloudConfig &cloud_config,
    const EloqShare::RocksDBConfig &config,
    bool create_if_missing,
    bool tx_enable_cache_replacement)
    : RocksDBHandler(config, create_if_missing, tx_enable_cache_replacement),
      cloud_config_(cloud_config),
      cloud_fs_(),
      cloud_env_(nullptr),
      db_(nullptr),
      ttl_compaction_filter_(nullptr)
{
}

RocksDBCloudHandlerImpl::~RocksDBCloudHandlerImpl()
{
    query_worker_pool_->Shutdown();
    db_manage_worker_->Shutdown();
    Shutdown();
}

void RocksDBCloudHandlerImpl::Shutdown()
{
    std::unique_lock<std::shared_mutex> db_lk(db_mux_);
    {
        std::unique_lock<std::mutex> lk(pending_ddl_mux_);
        while (!pending_ddl_req_.empty())
        {
            *pending_ddl_req_.front().err_code_ =
                txservice::CcErrorCode::NG_TERM_CHANGED;
            pending_ddl_req_.front().ccs_->Enqueue(
                pending_ddl_req_.front().cc_req_);
            pending_ddl_req_.pop();
        }
    }
    if (db_ != nullptr)
    {
        for (auto &cfh : column_families_)
        {
            rocksdb::ColumnFamilyHandle *cfh_ptr = cfh.second.release();
            db_->DestroyColumnFamilyHandle(cfh_ptr);
        }
        column_families_.clear();
        db_->Close();
        db_->PauseBackgroundWork();
        delete db_;
        db_ = nullptr;
        ttl_compaction_filter_ = nullptr;
        cloud_env_ = nullptr;
        cloud_fs_ = nullptr;
    }
}

bool RocksDBCloudHandlerImpl::CreateSnapshot(
    const std::string &snapshot_path, std::vector<std::string> &snapshot_files)
{
    return true;
}

bool RocksDBCloudHandlerImpl::CreateSnapshotForStandby(
    std::vector<std::string> &snapshot_files)
{
    return CreateSnapshot(ckpt_path_, snapshot_files);
}

bool RocksDBCloudHandlerImpl::CreateSnapshotForBackup(
    const std::string &backup_name, std::vector<std::string> &snapshot_files)
{
    // local path to store snapshot temporarily
    const std::string snapshot_path = backup_path_ + backup_name + "/";
    return CreateSnapshot(snapshot_path, snapshot_files);
}

bool RocksDBCloudHandlerImpl::RemoveBackupSnapshot(
    const std::string &backup_name)
{
    return true;
}

bool RocksDBCloudHandlerImpl::SendSnapshotToRemote(
    uint32_t ng_id,
    int64_t ng_term,
    std::vector<std::string> &snapshot_files,
    const std::string &remote_dest)
{
    return true;
}

bool RocksDBCloudHandlerImpl::OnSnapshotReceived(
    const txservice::remote::OnSnapshotSyncedRequest *req)
{
    return true;
}

std::string RocksDBCloudHandlerImpl::SnapshotSyncDestPath() const
{
    return "";
}

bool RocksDBCloudHandlerImpl::StartDB(bool is_ng_leader,
                                      uint32_t *next_leader_node)
{
    if (db_)
    {
        // db is already started, no op
        return true;
    }
    // setup cloud fd config
    rocksdb::Status status;
    rocksdb::CloudFileSystemOptions cfs_options;

#if ROCKSDB_CLOUD_FS_TYPE == ROCKSDB_CLOUD_FS_TYPE_S3
    if (cloud_config_.aws_access_key_id_.length() == 0 ||
        cloud_config_.aws_secret_key_.length() == 0)
    {
        LOG(INFO) << "No AWS_ACCESS_KEY_ID/AWS_SECRET_ACCESS_KEY "
                     "provided, use default credential provider";
        cfs_options.credentials.type = rocksdb::AwsAccessType::kUndefined;
    }
    else
    {
        cfs_options.credentials.InitializeSimple(
            cloud_config_.aws_access_key_id_, cloud_config_.aws_secret_key_);
    }

    status = cfs_options.credentials.HasValid();
    if (!status.ok())
    {
        LOG(ERROR) << "Valid AWS_ACCESS_KEY_ID/AWS_SECRET_ACCESS_KEY "
                      "is required, error: "
                   << status.ToString();
        return false;
    }
#endif

    cfs_options.src_bucket.SetBucketName(cloud_config_.bucket_name_,
                                         cloud_config_.bucket_prefix_);
    cfs_options.src_bucket.SetRegion(cloud_config_.region_);
    cfs_options.src_bucket.SetObjectPath("rocksdb_cloud");
    cfs_options.dest_bucket.SetBucketName(cloud_config_.bucket_name_,
                                          cloud_config_.bucket_prefix_);
    cfs_options.dest_bucket.SetRegion(cloud_config_.region_);
    cfs_options.dest_bucket.SetObjectPath("rocksdb_cloud");
    // Add sst_file_cache for accerlating random access on sst files
    cfs_options.sst_file_cache =
        rocksdb::NewLRUCache(cloud_config_.sst_file_cache_size_);
    // delay cloud file deletion for 1 hour
    cfs_options.cloud_file_deletion_delay =
        std::chrono::seconds(cloud_config_.db_file_deletion_delay_);
    // sync cloudmanifest and manifest files when open db
    cfs_options.resync_on_open = true;

    DLOG(INFO) << "DBCloudContainer Open";
    rocksdb::CloudFileSystem *cfs;
    status = EloqShare::NewCloudFileSystem(cfs_options, &cfs);

    if (!status.ok())
    {
        LOG(ERROR) << "Unable to create cloud storage filesystem, cloud type: "
#if ROCKSDB_CLOUD_FS_TYPE == ROCKSDB_CLOUD_FS_TYPE_S3
                   << "Aws"
#elif ROCKSDB_CLOUD_FS_TYPE == ROCKSDB_CLOUD_FS_TYPE_GCS
                   << "Gcp"
#endif
                   << ", at path rocksdb_cloud with bucket "
                   << cfs_options.src_bucket.GetBucketName()
                   << ", with error: " << status.ToString();

        std::abort();
    }

    cloud_fs_.reset(cfs);
    // Create options and use the AWS file system that we created
    // earlier
    cloud_env_ = rocksdb::NewCompositeEnv(cloud_fs_);

    return OpenCloudDB(cfs_options, is_ng_leader, next_leader_node);
}

bool RocksDBCloudHandlerImpl::OpenCloudDB(
    const rocksdb::CloudFileSystemOptions &cfs_options,
    bool is_ng_leader,
    uint32_t *next_leader_node)
{
    rocksdb::Options options;
    options.env = cloud_env_.get();
    options.create_if_missing = true;
    options.create_missing_column_families = true;
    // boost write performance by enabling unordered write
    options.unordered_write = true;

    // print db statistics every 60 seconds
    if (enable_stats_)
    {
        options.statistics = rocksdb::CreateDBStatistics();
        options.stats_dump_period_sec = stats_dump_period_sec_;
    }

    // Max background jobs number, rocksdb will auto turn max flush(1/4 of
    // max_background_jobs) and compaction jobs(3/4 of max_background_jobs)
    if (max_background_jobs_ > 0)
    {
        options.max_background_jobs = max_background_jobs_;
    }

    if (max_background_flushes_ > 0)
    {
        options.max_background_flushes = max_background_flushes_;
    }

    if (max_background_compactions_ > 0)
    {
        options.max_background_compactions = max_background_compactions_;
    }

    options.use_direct_io_for_flush_and_compaction =
        use_direct_io_for_flush_and_compaction_;
    options.use_direct_reads = use_direct_io_for_read_;

    // Set compation style
    if (compaction_style_ == "universal")
    {
        LOG(WARNING)
            << "Universal compaction has a size limitation. Please be careful "
               "when your DB (or column family) size is over 100GB";
        options.compaction_style = rocksdb::kCompactionStyleUniversal;
    }
    else if (compaction_style_ == "level")
    {
        options.compaction_style = rocksdb::kCompactionStyleLevel;
    }
    else if (compaction_style_ == "fifo")
    {
        LOG(ERROR) << "FIFO compaction style should not be used";
        std::abort();
    }
    else
    {
        LOG(ERROR) << "Invalid compaction style: " << compaction_style_;
        std::abort();
    }

    // set the max subcompactions
    if (max_subcompactions_ > 0)
    {
        options.max_subcompactions = max_subcompactions_;
    }

    // set the write rate limit
    if (write_rate_limit_ > 0)
    {
        options.rate_limiter.reset(
            rocksdb::NewGenericRateLimiter(write_rate_limit_));
    }

    options.info_log_level = info_log_level_;
    options.best_efforts_recovery = false;
    options.skip_checking_sst_file_sizes_on_db_open = true;
    options.skip_stats_update_on_db_open = true;
    // Important! keep atomic_flush true, since we disabled WAL
    options.atomic_flush = true;
    auto db_event_listener =
        std::make_shared<EloqShare::RocksDBEventListener>();
    options.listeners.emplace_back(db_event_listener);

    // The following two configuration items are setup for purpose of removing
    // expired kv data items according to their ttl Rocksdb will compact all sst
    // files which are older than periodic_compaction_seconds_ at
    // dialy_offpeak_time_utc_ Then all kv data items in the sst files will go
    // through the TTLCompactionFilter which is configurated for column family
    options.periodic_compaction_seconds = periodic_compaction_seconds_;
    options.daily_offpeak_time_utc = dialy_offpeak_time_utc_;

    // The max_open_files default value is -1, it cause DB open all files on
    // DB::Open() This behavior causes 2 effects,
    // 1. DB::Open() will be slow
    // 2. During DB::Open, some of the opened sst files keep in LRUCache
    // will be deleted due to LRU policy, which causes DB::Open failed
    options.max_open_files = 0;

    std::vector<rocksdb::ColumnFamilyDescriptor> cfds;

    rocksdb::ColumnFamilyOptions cf_options;

    if (target_file_size_base_ > 0)
    {
        cf_options.target_file_size_base = target_file_size_base_;
    }

    if (target_file_size_multiplier_ > 0)
    {
        cf_options.target_file_size_multiplier = target_file_size_multiplier_;
    }

    // mem table size
    if (write_buff_size_ > 0)
    {
        cf_options.write_buffer_size = write_buff_size_;
    }
    // Max write buffer number
    if (max_write_buffer_number_ > 0)
    {
        cf_options.max_write_buffer_number = max_write_buffer_number_;
    }

    if (level0_slowdown_writes_trigger_ > 0)
    {
        cf_options.level0_slowdown_writes_trigger =
            level0_slowdown_writes_trigger_;
    }

    if (level0_stop_writes_trigger_ > 0)
    {
        cf_options.level0_stop_writes_trigger = level0_stop_writes_trigger_;
    }

    if (level0_file_num_compaction_trigger_ > 0)
    {
        cf_options.level0_file_num_compaction_trigger =
            level0_file_num_compaction_trigger_;
    }

    if (soft_pending_compaction_bytes_limit_ > 0)
    {
        cf_options.soft_pending_compaction_bytes_limit =
            soft_pending_compaction_bytes_limit_;
    }

    if (hard_pending_compaction_bytes_limit_ > 0)
    {
        cf_options.hard_pending_compaction_bytes_limit =
            hard_pending_compaction_bytes_limit_;
    }

    if (max_bytes_for_level_base_ > 0)
    {
        cf_options.max_bytes_for_level_base = max_bytes_for_level_base_;
    }

    if (max_bytes_for_level_multiplier_ > 0)
    {
        cf_options.max_bytes_for_level_multiplier =
            max_bytes_for_level_multiplier_;
    }

    // Disable auto compaction if cache replacement is disabled,
    // the auto compaction will be enabled after RestoreTxCache finished
    if (!txservice::txservice_skip_kv &&
        !txservice::txservice_enable_cache_replacement)
    {
        cf_options.disable_auto_compactions = true;
    }

    std::unique_lock<std::shared_mutex> db_lk(db_mux_);
    // set ttl compaction filter
    assert(ttl_compaction_filter_ == nullptr);
    ttl_compaction_filter_ = std::make_unique<EloqShare::TTLCompactionFilter>();

    cf_options.compaction_filter =
        static_cast<rocksdb::CompactionFilter *>(ttl_compaction_filter_.get());

    bool need_init_prebuilt_schema = false;
    // list all column families and open them
    std::vector<std::string> column_families;
    rocksdb::Status status = rocksdb::DBCloud::ListColumnFamilies(
        options, db_path_, &column_families);
    if (status.ok())
    {
        for (const auto &cf : column_families)
        {
            cfds.emplace_back(cf, cf_options);
        }
    }
    else
    {
        LOG(ERROR) << "Failed to list column families: " << status.ToString()
                   << ", use default prebuilt tables.";
        need_init_prebuilt_schema = true;
        cfds.emplace_back(rocksdb::kDefaultColumnFamilyName,
                          rocksdb::ColumnFamilyOptions());

        for (const auto &pre_built_table : pre_built_tables_)
        {
            std::string cf = pre_built_table.first;
            cfds.emplace_back(cf, cf_options);
        }
    }

    std::vector<rocksdb::ColumnFamilyHandle *> cfhs;
    status =
        rocksdb::DBCloud::Open(options, db_path_, cfds, "", 0, &cfhs, &db_);

    if (!status.ok())
    {
        if (create_db_if_missing_)
        {
            LOG(ERROR) << "Unable to open db at path " << db_path_
                       << " with error: " << status.ToString();
            std::abort();
        }
        assert(cfhs.empty());

        LOG(ERROR) << "Unable to open db at path " << storage_path_
                   << " with bucket " << cfs_options.src_bucket.GetBucketName()
                   << " with error: " << status.ToString();
        ttl_compaction_filter_ = nullptr;

        // db does not exist. This node cannot escalate to be the ng leader.
        return false;
    }

    if (cloud_config_.warm_up_thread_num_ != 0)
    {
        db_->WarmUp(cloud_config_.warm_up_thread_num_);
    }

    // Reset max_open_files to default value of -1 after DB::Open
    db_->SetDBOptions({{"max_open_files", "-1"}});

    // set the column family handlers
    for (auto cfh : cfhs)
    {
        column_families_.emplace(cfh->GetName(), cfh);
    }

    if (is_ng_leader)
    {
        std::string value;
        status = db_->Get(
            rocksdb::ReadOptions(), rocksdb::Slice("followed_leader"), &value);
        uint32_t followed_leader;

        if (status.ok())
        {
            followed_leader = std::stoul(value);
        }

        if (status.ok() &&
            followed_leader != txservice::Sharder::Instance().NodeId())
        {
            // key is found. This node was a follower before shutdown, tranfer
            // leader to previous leader.
            if (next_leader_node)
            {
                DLOG(INFO) << "Not leader before shutdown. Transfering leader "
                              "to node "
                           << followed_leader;
                *next_leader_node = followed_leader;
            }
            // remove this key. If the cluster is shutdown in a chaos status and
            // no other node is eligible to become the leader, escalate to
            // leader if the leadership is transferred back to this node.
            status = db_->Delete(
                rocksdb::WriteOptions(),
                GetColumnFamilyHandler(rocksdb::kDefaultColumnFamilyName),
                rocksdb::Slice("followed_leader"));

            rocksdb::FlushOptions flush_options;
            flush_options.allow_write_stall = false;
            flush_options.wait = true;
            status = db_->Flush(flush_options);

            for (auto &cfh : column_families_)
            {
                rocksdb::ColumnFamilyHandle *cfh_ptr = cfh.second.release();
                db_->DestroyColumnFamilyHandle(cfh_ptr);
            }
            column_families_.clear();
            db_->Close();
            db_->PauseBackgroundWork();
            delete db_;
            db_ = nullptr;
            ttl_compaction_filter_ = nullptr;
            return false;
        }
        else if (status.IsNotFound() ||
                 (status.ok() &&
                  followed_leader == txservice::Sharder::Instance().NodeId()))
        {
            // Cannot find follower key. This node was leader before shutdown.
            // Continue with the startup.
        }
        else
        {
            LOG(ERROR)
                << "Failed to get previous ng leader cache from rocksdb: "
                << status.ToString();
            for (auto &cfh : column_families_)
            {
                rocksdb::ColumnFamilyHandle *cfh_ptr = cfh.second.release();
                db_->DestroyColumnFamilyHandle(cfh_ptr);
            }
            column_families_.clear();
            db_->Close();
            db_->PauseBackgroundWork();
            delete db_;
            db_ = nullptr;
            ttl_compaction_filter_ = nullptr;
            return false;
        }
    }

    auto cf_pair = column_families_.find(rocksdb::kDefaultColumnFamilyName);
    assert(cf_pair != column_families_.end());
    rocksdb::ColumnFamilyHandle *cfh_default = cf_pair->second.get();
    assert(cfh_default != nullptr);

    if (need_init_prebuilt_schema)
    {
        // create redis table catalog info
        for (auto cfh : cfhs)
        {
            // Create and update prebuilt tables info
            std::string table_name = cfh->GetName();
            if (cfh->GetName() == rocksdb::kDefaultColumnFamilyName)
            {
                continue;
            }
            // find the table schema
            std::string table_key = table_name + "_catalog";
            rocksdb::PinnableWideColumns pinnable_table_catalog;
            auto status = db_->GetEntity(rocksdb::ReadOptions(),
                                         cfh_default,
                                         table_key,
                                         &pinnable_table_catalog);
            if (!status.ok())
            {
                if (status.IsNotFound())
                {
                    // if the table schema is not found, then we need to create
                    // it
                    auto pre_built_table = pre_built_tables_.find(table_name);
                    if (pre_built_table != pre_built_tables_.end())
                    {
                        rocksdb::WideColumns table_catalog_wc;
                        table_catalog_wc.emplace_back("kv_cf_name", table_name);
                        uint64_t version = 100U;
                        rocksdb::Slice commit_ts_value(
                            reinterpret_cast<const char *>(&version),
                            sizeof(uint64_t));
                        table_catalog_wc.emplace_back("version",
                                                      commit_ts_value);
                        status = db_->PutEntity(rocksdb::WriteOptions(),
                                                cfh_default,
                                                table_key,
                                                table_catalog_wc);
                        if (!status.ok())
                        {
                            LOG(ERROR) << "Unable to write table schema to db "
                                          "with error: "
                                       << status.ToString();
                            return false;
                        }
                    }
                    else
                    {
                        LOG(ERROR) << "Unable to get table schema for table: "
                                   << table_name << " in pre_built_tables_";
                        return false;
                    }
                }
                else
                {
                    LOG(ERROR)
                        << "Unable to get table schema for table: "
                        << table_name << " with error: " << status.ToString();
                    return false;
                }
            }
        }
    }

    {
        std::unique_lock<std::mutex> ddl_lk(pending_ddl_mux_);
        while (!pending_ddl_req_.empty())
        {
            UpsertTableReq ddl_req = std::move(pending_ddl_req_.front());
            pending_ddl_req_.pop();
            ddl_lk.unlock();
            UpsertTable(ddl_req.old_table_schema_,
                        ddl_req.table_schema_,
                        ddl_req.op_type_,
                        ddl_req.write_time_,
                        ddl_req.ng_id_,
                        ddl_req.tx_term_,
                        nullptr,
                        ddl_req.alter_table_info_,
                        ddl_req.cc_req_,
                        ddl_req.ccs_,
                        ddl_req.err_code_);
            ddl_lk.lock();
        }
    }

    LOG(INFO) << "RocksDB Cloud started";
    return true;
}

rocksdb::DBCloud *RocksDBCloudHandlerImpl::GetDBPtr()
{
    return db_;
}

#else

RocksDBHandlerImpl::RocksDBHandlerImpl(const EloqShare::RocksDBConfig &config,
                                       bool create_if_missing,
                                       bool tx_enable_cache_replacement)
    : RocksDBHandler(config, create_if_missing, tx_enable_cache_replacement),
      db_(nullptr),
      ttl_compaction_filter_(nullptr)
{
    rsync_task_pool_ = std::make_unique<txservice::TxWorkerPool>(
        config.snapshot_sync_worker_num_);
}

RocksDBHandlerImpl::~RocksDBHandlerImpl()
{
    if (rsync_task_pool_)
    {
        // shutdown before stanbdy sync worker
        rsync_task_pool_->Shutdown();
    }

    query_worker_pool_->Shutdown();
    db_manage_worker_->Shutdown();
    Shutdown();
}

bool RocksDBHandlerImpl::StartDB(bool is_ng_leader, uint32_t *next_leader_node)
{
    if (db_)
    {
        // db is already started, no op
        return true;
    }
    rocksdb::Options options;
    options.create_if_missing = create_db_if_missing_;
    options.create_missing_column_families = true;
    // boost write performance by enabling unordered write
    options.unordered_write = true;

    // print db statistics every 60 seconds
    if (enable_stats_)
    {
        options.statistics = rocksdb::CreateDBStatistics();
        options.stats_dump_period_sec = stats_dump_period_sec_;
    }

    // Max background jobs number, rocksdb will auto turn max flush(1/4 of
    // max_background_jobs) and compaction jobs(3/4 of max_background_jobs)
    if (max_background_jobs_ > 0)
    {
        options.max_background_jobs = max_background_jobs_;
    }

    if (max_background_flushes_ > 0)
    {
        options.max_background_flushes = max_background_flushes_;
    }

    if (max_background_compactions_ > 0)
    {
        options.max_background_compactions = max_background_compactions_;
    }

    options.use_direct_io_for_flush_and_compaction =
        use_direct_io_for_flush_and_compaction_;
    options.use_direct_reads = use_direct_io_for_read_;

    // Set compation style
    if (compaction_style_ == "universal")
    {
        LOG(WARNING)
            << "Universal compaction has a size limitation. Please be careful "
               "when your DB (or column family) size is over 100GB";
        options.compaction_style = rocksdb::kCompactionStyleUniversal;
    }
    else if (compaction_style_ == "level")
    {
        options.compaction_style = rocksdb::kCompactionStyleLevel;
    }
    else if (compaction_style_ == "fifo")
    {
        LOG(ERROR) << "FIFO compaction style should not be used";
        return false;
    }
    else
    {
        LOG(ERROR) << "Invalid compaction style: " << compaction_style_;
        return false;
    }

    // set the max subcompactions
    if (max_subcompactions_ > 0)
    {
        options.max_subcompactions = max_subcompactions_;
    }

    // set the write rate limit
    if (write_rate_limit_ > 0)
    {
        options.rate_limiter.reset(
            rocksdb::NewGenericRateLimiter(write_rate_limit_));
    }

    options.info_log_level = info_log_level_;
    // Important! keep atomic_flush true, since we disabled WAL
    options.atomic_flush = true;
    auto db_event_listener =
        std::make_shared<EloqShare::RocksDBEventListener>();
    options.listeners.emplace_back(db_event_listener);

    // The following two configuration items are setup for purpose of removing
    // expired kv data items according to their ttl Rocksdb will compact all sst
    // files which are older than periodic_compaction_seconds_ at
    // dialy_offpeak_time_utc_ Then all kv data items in the sst files will go
    // through the TTLCompactionFilter which is configurated for column family
    options.periodic_compaction_seconds = periodic_compaction_seconds_;
    options.daily_offpeak_time_utc = dialy_offpeak_time_utc_;

    std::vector<rocksdb::ColumnFamilyDescriptor> cfds;

    rocksdb::ColumnFamilyOptions cf_options;

    if (target_file_size_base_ > 0)
    {
        cf_options.target_file_size_base = target_file_size_base_;
    }

    if (target_file_size_multiplier_ > 0)
    {
        cf_options.target_file_size_multiplier = target_file_size_multiplier_;
    }

    // mem table size
    if (write_buff_size_ > 0)
    {
        cf_options.write_buffer_size = write_buff_size_;
    }

    // Max write buffer number
    if (max_write_buffer_number_ > 0)
    {
        cf_options.max_write_buffer_number = max_write_buffer_number_;
    }

    if (level0_slowdown_writes_trigger_ > 0)
    {
        cf_options.level0_slowdown_writes_trigger =
            level0_slowdown_writes_trigger_;
    }

    if (level0_stop_writes_trigger_ > 0)
    {
        cf_options.level0_stop_writes_trigger = level0_stop_writes_trigger_;
    }

    if (level0_file_num_compaction_trigger_ > 0)
    {
        cf_options.level0_file_num_compaction_trigger =
            level0_file_num_compaction_trigger_;
    }

    if (soft_pending_compaction_bytes_limit_ > 0)
    {
        cf_options.soft_pending_compaction_bytes_limit =
            soft_pending_compaction_bytes_limit_;
    }

    if (hard_pending_compaction_bytes_limit_ > 0)
    {
        cf_options.hard_pending_compaction_bytes_limit =
            hard_pending_compaction_bytes_limit_;
    }

    if (max_bytes_for_level_base_ > 0)
    {
        cf_options.max_bytes_for_level_base = max_bytes_for_level_base_;
    }

    if (max_bytes_for_level_multiplier_ > 0)
    {
        cf_options.max_bytes_for_level_multiplier =
            max_bytes_for_level_multiplier_;
    }

    std::unique_lock<std::shared_mutex> db_lk(db_mux_);
    // set ttl compaction filter
    assert(ttl_compaction_filter_ == nullptr);
    ttl_compaction_filter_ = std::make_unique<EloqShare::TTLCompactionFilter>();

    cf_options.compaction_filter =
        static_cast<rocksdb::CompactionFilter *>(ttl_compaction_filter_.get());

    bool need_init_prebuilt_schema = false;
    // list all column families and open them
    std::vector<std::string> column_families;
    rocksdb::Status status =
        rocksdb::DB::ListColumnFamilies(options, db_path_, &column_families);

    cfds.emplace_back(rocksdb::kDefaultColumnFamilyName,
                      rocksdb::ColumnFamilyOptions());
    if (status.ok())
    {
        for (const auto &cf : column_families)
        {
            if (cf != rocksdb::kDefaultColumnFamilyName)
            {
                assert(cf.find("data_table_") == 0);
                cfds.emplace_back(cf, cf_options);
            }
        }
    }
    else
    {
        LOG(ERROR) << "Failed to list column families: " << status.ToString()
                   << ", use default prebuilt tables.";
        need_init_prebuilt_schema = true;

        for (const auto &pre_built_table : pre_built_tables_)
        {
            std::string cf = pre_built_table.first;
            cfds.emplace_back(cf, cf_options);
        }
    }

    std::vector<rocksdb::ColumnFamilyHandle *> cfhs;
    status = rocksdb::DB::Open(options, db_path_, cfds, &cfhs, &db_);
    if (!status.ok())
    {
        if (create_db_if_missing_)
        {
            LOG(ERROR) << "Unable to open db at path " << db_path_
                       << " with error: " << status.ToString();
            std::abort();
        }
        assert(cfhs.empty());

        LOG(ERROR) << "Unable to open db at path " << db_path_
                   << " with error: " << status.ToString();
        ttl_compaction_filter_ = nullptr;

        // db does not exist. This node cannot escalate to be the ng leader.
        return false;
    }

    // set the column family handlers
    for (auto cfh : cfhs)
    {
        column_families_.emplace(cfh->GetName(), cfh);
    }

    if (is_ng_leader)
    {
        std::string value;
        status = db_->Get(
            rocksdb::ReadOptions(), rocksdb::Slice("followed_leader"), &value);
        uint32_t followed_leader;

        if (status.ok())
        {
            followed_leader = std::stoul(value);
        }

        if (status.ok() &&
            followed_leader != txservice::Sharder::Instance().NodeId())
        {
            // key is found. This node was a follower before shutdown, tranfer
            // leader to previous leader.
            if (next_leader_node)
            {
                DLOG(INFO) << "Not leader before shutdown. Transfering leader "
                              "to node "
                           << followed_leader;
                *next_leader_node = followed_leader;
            }
            // remove this key. If the cluster is shutdown in a chaos status and
            // no other node is eligible to become the leader, escalate to
            // leader if the leadership is transferred back to this node.
            status = db_->Delete(
                rocksdb::WriteOptions(),
                GetColumnFamilyHandler(rocksdb::kDefaultColumnFamilyName),
                rocksdb::Slice("followed_leader"));

            rocksdb::FlushOptions flush_options;
            flush_options.allow_write_stall = false;
            flush_options.wait = true;
            status = db_->Flush(flush_options);

            for (auto &cfh : column_families_)
            {
                rocksdb::ColumnFamilyHandle *cfh_ptr = cfh.second.release();
                db_->DestroyColumnFamilyHandle(cfh_ptr);
            }
            column_families_.clear();
            db_->Close();
            db_->PauseBackgroundWork();
            delete db_;
            db_ = nullptr;
            ttl_compaction_filter_ = nullptr;
            return false;
        }
        else if (status.IsNotFound() ||
                 (status.ok() &&
                  followed_leader == txservice::Sharder::Instance().NodeId()))
        {
            // Cannot find follower key. This node was leader before shutdown.
            // Continue with the startup.
        }
        else
        {
            LOG(ERROR)
                << "Failed to get previous ng leader cache from rocksdb: "
                << status.ToString();
            for (auto &cfh : column_families_)
            {
                rocksdb::ColumnFamilyHandle *cfh_ptr = cfh.second.release();
                db_->DestroyColumnFamilyHandle(cfh_ptr);
            }
            column_families_.clear();
            db_->Close();
            db_->PauseBackgroundWork();
            delete db_;
            db_ = nullptr;
            ttl_compaction_filter_ = nullptr;
            return false;
        }
    }

    if (need_init_prebuilt_schema)
    {
        // create redis table catalog info
        for (auto cfh : cfhs)
        {
            // Create and update prebuilt tables info
            std::string table_name = cfh->GetName();
            if (cfh->GetName() == rocksdb::kDefaultColumnFamilyName)
            {
                continue;
            }
            // create table schema if it does not exist
            rocksdb::ColumnFamilyHandle *cfh_default =
                GetColumnFamilyHandler(rocksdb::kDefaultColumnFamilyName);
            assert(cfh_default != nullptr);
            std::string table_key = table_name + "_catalog";
            rocksdb::PinnableWideColumns pinnable_table_catalog;
            auto status = db_->GetEntity(rocksdb::ReadOptions(),
                                         cfh_default,
                                         table_key,
                                         &pinnable_table_catalog);
            if (!status.ok())
            {
                if (status.IsNotFound())
                {
                    // if the table schema is not found, then we need to create
                    // it, init the table schema with version 100
                    auto pre_built_table = pre_built_tables_.find(table_name);
                    if (pre_built_table != pre_built_tables_.end())
                    {
                        // create the table schema
                        rocksdb::WideColumns table_catalog_wc;
                        table_catalog_wc.emplace_back("kv_cf_name", table_name);
                        uint64_t version = 100U;
                        rocksdb::Slice commit_ts_value(
                            reinterpret_cast<const char *>(&version),
                            sizeof(uint64_t));
                        table_catalog_wc.emplace_back("version",
                                                      commit_ts_value);
                        status = db_->PutEntity(rocksdb::WriteOptions(),
                                                cfh_default,
                                                table_key,
                                                table_catalog_wc);
                        if (!status.ok())
                        {
                            LOG(ERROR) << "Unable to write table schema to db "
                                          "with error: "
                                       << status.ToString();
                            return false;
                        }
                    }
                    else
                    {
                        LOG(ERROR) << "Unable to get table schema for table: "
                                   << table_name << " in pre_built_tables_";
                        return false;
                    }
                }
                else
                {
                    LOG(ERROR)
                        << "Unable to get table schema for table: "
                        << table_name << " with error: " << status.ToString();
                    return false;
                }
            }
        }
    }

    {
        std::unique_lock<std::mutex> ddl_lk(pending_ddl_mux_);
        while (!pending_ddl_req_.empty())
        {
            UpsertTableReq ddl_req = std::move(pending_ddl_req_.front());
            pending_ddl_req_.pop();
            ddl_lk.unlock();
            UpsertTable(ddl_req.old_table_schema_,
                        ddl_req.table_schema_,
                        ddl_req.op_type_,
                        ddl_req.write_time_,
                        ddl_req.ng_id_,
                        ddl_req.tx_term_,
                        nullptr,
                        ddl_req.alter_table_info_,
                        ddl_req.cc_req_,
                        ddl_req.ccs_,
                        ddl_req.err_code_);
            ddl_lk.lock();
        }
    }

    LOG(INFO) << "RocksDB started";

    return true;
}

rocksdb::DB *RocksDBHandlerImpl::GetDBPtr() const
{
    return db_;
}

bool RocksDBHandlerImpl::OnSnapshotReceived(
    const txservice::remote::OnSnapshotSyncedRequest *req)
{
    bthread::Mutex mux;
    bthread::ConditionVariable cv;
    bool succ = false;
    bool finished = false;

    db_manage_worker_->SubmitWork(
        [this, &succ, &mux, &cv, &finished, req]
        {
            bool res = true;
            if (txservice::Sharder::Instance().CandidateStandbyNodeTerm() !=
                req->standby_node_term())
            {
                res = false;
            }

            if (res)
            {
                res = OverrideDB(req->snapshot_path());
            }
            std::unique_lock<bthread::Mutex> lk(mux);
            succ = res;
            finished = true;
            cv.notify_one();
        });
    std::unique_lock<bthread::Mutex> lk(mux);
    while (!finished)
    {
        cv.wait(lk);
    }

    return succ;
}

void RocksDBHandlerImpl::Shutdown()
{
    std::unique_lock<std::shared_mutex> db_lk(db_mux_);
    {
        std::unique_lock<std::mutex> lk(pending_ddl_mux_);
        while (!pending_ddl_req_.empty())
        {
            *pending_ddl_req_.front().err_code_ =
                txservice::CcErrorCode::NG_TERM_CHANGED;
            pending_ddl_req_.front().ccs_->Enqueue(
                pending_ddl_req_.front().cc_req_);
            pending_ddl_req_.pop();
        }
    }
    if (db_ != nullptr)
    {
        for (auto &cfh : column_families_)
        {
            rocksdb::ColumnFamilyHandle *cfh_ptr = cfh.second.release();
            db_->DestroyColumnFamilyHandle(cfh_ptr);
        }
        column_families_.clear();
        db_->Close();
        db_->PauseBackgroundWork();
        delete db_;
        db_ = nullptr;
        ttl_compaction_filter_ = nullptr;
    }
}

bool RocksDBHandlerImpl::OverrideDB(const std::string &new_snapshot_path)
{
    if (db_ != nullptr)
    {
        // no need to acquire mutex since this thread is the only thread that
        // would modify db_.
        return false;
    }

    assert(GetDBPtr() == nullptr);
    // First remove current db dir.
    std::filesystem::remove_all(db_path_);
    // atomically switch snapshot to current db dir
    std::filesystem::rename(new_snapshot_path, db_path_);

    std::filesystem::path new_snapshot_fs_path(new_snapshot_path);
    uint64_t new_snapshot_ts =
        std::stoull(new_snapshot_fs_path.parent_path().filename().string());
    // remove expired snapshot dirs if there is any
    for (const auto &entry :
         std::filesystem::directory_iterator(received_snapshot_path_))
    {
        uint64_t entry_snapshot_ts =
            std::stoull(entry.path().filename().string());
        if (entry_snapshot_ts < new_snapshot_ts)
        {
            std::filesystem::remove_all(entry);
        }
    }

    return StartDB(false);
}

std::string RocksDBHandlerImpl::SnapshotSyncDestPath() const
{
    uint64_t ts = std::chrono::system_clock().now().time_since_epoch().count();
    auto dest_path = std::filesystem::path(received_snapshot_path_ +
                                           std::to_string(ts) + "/");
    if (dest_path.is_absolute())
    {
        return dest_path;
    }
    else
    {
        std::filesystem::path abs_path = std::filesystem::current_path();
        abs_path += "/";
        abs_path += dest_path;
        return abs_path;
    }
}

bool RocksDBHandlerImpl::CreateSnapshot(
    const std::string &snapshot_path, std::vector<std::string> &snapshot_files)
{
    // do rocksdb checkpoint, add checkpoint files to snapshot
    rocksdb::Checkpoint *checkpoint;
    auto status = rocksdb::Checkpoint::Create(db_, &checkpoint);
    if (!status.ok())
    {
        LOG(ERROR) << "Create checkpoint object failed: " << status.ToString();
        return false;
    }
    assert(status.ok());

    // ckpt_path must not exist before CreateCheckpoint
    std::error_code error_code;
    std::filesystem::remove_all(snapshot_path, error_code);
    if (error_code.value() != 0)
    {
        LOG(ERROR) << "Failed to clean up old snapshot directory: "
                   << snapshot_path << " error code: " << error_code.message();
        return false;
    }
    status = checkpoint->CreateCheckpoint(snapshot_path);
    if (!status.ok())
    {
        LOG(ERROR) << "Create checkpoint failed: " << status.ToString();
        return false;
    }
    assert(status.ok());
    std::filesystem::directory_iterator dir_ite(snapshot_path, error_code);
    if (error_code.value() != 0)
    {
        LOG(ERROR) << "Failed to scan snapshot directry.";
        return false;
    }
    else
    {
        for (const auto &entry :
             std::filesystem::directory_iterator(snapshot_path))
        {
            // add relative path
            std::string path(snapshot_path);
            snapshot_files.emplace_back(path.append(entry.path().filename()));
        }
        return true;
    }
}

bool RocksDBHandlerImpl::CreateSnapshotForStandby(
    std::vector<std::string> &snapshot_files)
{
    return CreateSnapshot(ckpt_path_, snapshot_files);
}
bool RocksDBHandlerImpl::CreateSnapshotForBackup(
    const std::string &backup_name, std::vector<std::string> &snapshot_files)
{
    // local path to store snapshot temporarily
    const std::string snapshot_path = backup_path_ + backup_name + "/";
    return CreateSnapshot(snapshot_path, snapshot_files);
}

bool RocksDBHandlerImpl::RemoveBackupSnapshot(const std::string &backup_name)
{
    const std::string snapshot_path = backup_path_ + backup_name + "/";

    // remove local temporary store directory of backup files.
    std::error_code error_code;
    std::filesystem::remove_all(snapshot_path, error_code);
    if (error_code.value() != 0)
    {
        LOG(ERROR) << "Failed to remove local temporary store directory of "
                      "backup files , backup name:"
                   << backup_name << ", error code: " << error_code.message();
        return false;
    }
    return true;
}

bool RocksDBHandlerImpl::SendSnapshotToRemote(
    uint32_t ng_id,
    int64_t ng_term,
    std::vector<std::string> &snapshot_files,
    const std::string &remote_dest)
{
    using namespace txservice;

    if (snapshot_files.empty())
    {
        return true;
    }
    else
    {
        std::atomic<bool> succ{true};

        // Send the first snapshot file to remote node in order to create
        // non-existent directory. Otherwise, multiple rsync processes
        // creating a non-existent directory at the same time will result in
        // an error.
        bool send_result = SendFileToRemoteNode(snapshot_files[0], remote_dest);
        if (!send_result)
        {
            LOG(ERROR) << "Failed to send snapshot to remote"
                       << ", filename: " << snapshot_files[0]
                       << ", remote dest: " << remote_dest;
            succ.store(false, std::memory_order_relaxed);
            return false;
        }

        assert(snapshot_files.size() >= 1);
        size_t unfinished_rsync_tasks_ = snapshot_files.size() - 1;
        std::mutex rsync_task_mux_;
        std::condition_variable rsync_task_cond_;

        for (size_t file_idx = 1; file_idx < snapshot_files.size(); ++file_idx)
        {
            rsync_task_pool_->SubmitWork(
                [ng_id,
                 ng_term,
                 &snapshot_files,
                 file_idx,
                 &remote_dest,
                 &succ,
                 &rsync_task_mux_,
                 &rsync_task_cond_,
                 &unfinished_rsync_tasks_,
                 this]()
                {
                    bool prev_succ = succ.load(std::memory_order_acquire);
                    if (!Sharder::Instance().CheckLeaderTerm(ng_id, ng_term) ||
                        !prev_succ)
                    {
                        std::lock_guard<std::mutex> rsync_task_lk(
                            rsync_task_mux_);
                        succ.store(false, std::memory_order_relaxed);
                        if (--unfinished_rsync_tasks_ == 0)
                        {
                            rsync_task_cond_.notify_one();
                        }
                        return;
                    }

                    bool send_result = SendFileToRemoteNode(
                        snapshot_files[file_idx], remote_dest);
                    std::lock_guard<std::mutex> rsync_task_lk(rsync_task_mux_);
                    if (!send_result)
                    {
                        succ.store(false, std::memory_order_relaxed);
                        LOG(ERROR) << "Failed to send snapshot to remote"
                                   << ", filename: " << snapshot_files[0]
                                   << ", remote dest: " << remote_dest;
                    }

                    if (--unfinished_rsync_tasks_ == 0)
                    {
                        rsync_task_cond_.notify_one();
                    }
                });
        }

        {
            std::unique_lock<std::mutex> rsync_task_lk(rsync_task_mux_);
            rsync_task_cond_.wait(
                rsync_task_lk, [&]() { return unfinished_rsync_tasks_ == 0; });
        }

        return succ.load(std::memory_order_relaxed);
    }
}

bool RocksDBHandlerImpl::SendFileToRemoteNode(const std::string &snapshot_path,
                                              const std::string &remote_path)
{
    // -rlptgoD

    std::string rsyncCommand =
        "rsync -av -e 'ssh -o "
        "UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no' " +
        snapshot_path + " " + remote_path;

    int res = std::system(rsyncCommand.c_str());
    return res == 0;
}

#endif

}  // namespace EloqKV
