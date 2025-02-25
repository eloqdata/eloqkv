#include "store_util.h"

#include <memory>
#include <string>
#include <vector>

#include "redis_hash_object.h"
#include "redis_list_object.h"
#include "redis_object.h"  // RedisMonoObject
#include "redis_set_object.h"
#include "redis_string_num.h"
#include "redis_string_object.h"
#include "redis_zset_object.h"

namespace EloqShare
{

std::string uint32_str_converter(uint32_t val)
{
    return std::to_string(val);
}

std::string uint16_str_converter(uint16_t val)
{
    return std::to_string(val);
}

std::string string_str_converter(const std::string &val)
{
    return val;
}

std::string bool_str_converter(bool val)
{
    return val ? "1" : "0";
}

uint32_t str_uint32_converter(const std::string &s)
{
    return std::stoul(s);
}

uint16_t str_uint16_converter(const std::string &s)
{
    return std::stoul(s);
}

std::string str_string_converter(const std::string &s)
{
    return s;
}

bool str_bool_converter(const std::string &s)
{
    return s == "1";
}

::EloqDS::remote::UpsertTableOperationType UpsertTableOperationTypeConverter(
    txservice::OperationType op_type)
{
    switch (op_type)
    {
    case txservice::OperationType::CreateTable:
        return ::EloqDS::remote::UpsertTableOperationType::CreateTable;
    case txservice::OperationType::DropTable:
        return ::EloqDS::remote::UpsertTableOperationType::DropTable;
    case txservice::OperationType::TruncateTable:
        return ::EloqDS::remote::UpsertTableOperationType::TruncateTable;
    case txservice::OperationType::AddIndex:
        return ::EloqDS::remote::UpsertTableOperationType::AddIndex;
    case txservice::OperationType::DropIndex:
        return ::EloqDS::remote::UpsertTableOperationType::DropIndex;
    default:
        assert(false);
        return ::EloqDS::remote::UpsertTableOperationType::CreateTable;
    }
}

txservice::OperationType OperationTypeConverter(
    ::EloqDS::remote::UpsertTableOperationType op_type)
{
    switch (op_type)
    {
    case ::EloqDS::remote::UpsertTableOperationType::CreateTable:
        return txservice::OperationType::CreateTable;
    case ::EloqDS::remote::UpsertTableOperationType::DropTable:
        return txservice::OperationType::DropTable;
    case ::EloqDS::remote::UpsertTableOperationType::TruncateTable:
        return txservice::OperationType::TruncateTable;
    case ::EloqDS::remote::UpsertTableOperationType::AddIndex:
        return txservice::OperationType::AddIndex;
    case ::EloqDS::remote::UpsertTableOperationType::DropIndex:
        return txservice::OperationType::DropIndex;
    default:
        assert(false);
        return txservice::OperationType::CreateTable;
    }
}

void ExtractNodesConfigs(
    const std::unordered_map<uint32_t,
                             std::vector<::EloqDS::remote::ClusterNodeConfig>>
        &ng_configs,
    std::vector<::EloqDS::remote::ClusterNodeConfig> &nodes)
{
    std::unordered_set<uint32_t> nid_set;
    for (const auto &[ng_id, ng_members] : ng_configs)
    {
        for (const auto &member : ng_members)
        {
            if (nid_set.find(member.node_id()) == nid_set.end())
            {
                nodes.emplace_back(member);
                nid_set.emplace(member.node_id());
            }
        }
    }
    std::sort(nodes.begin(),
              nodes.end(),
              [](auto &v1, auto &v2) { return v1.node_id() < v2.node_id(); });
}

#if KV_DATA_STORE_TYPE == KV_DATA_STORE_TYPE_ROCKSDB

rocksdb::InfoLogLevel StringToInfoLogLevel(const std::string &log_level_str)
{
    if (log_level_str == "DEBUG")
    {
        return rocksdb::InfoLogLevel::DEBUG_LEVEL;
    }
    else if (log_level_str == "INFO")
    {
        return rocksdb::InfoLogLevel::INFO_LEVEL;
    }
    else if (log_level_str == "WARN")
    {
        return rocksdb::InfoLogLevel::WARN_LEVEL;
    }
    else if (log_level_str == "ERROR")
    {
        return rocksdb::InfoLogLevel::ERROR_LEVEL;
    }
    else
    {
        // If the log level string is not recognized, default to a specific log
        // level, e.g., INFO_LEVEL Alternatively, you could throw an exception
        // or handle the case as you see fit
        return rocksdb::InfoLogLevel::INFO_LEVEL;
    }
}

void SerializeFlushRecord(const txservice::FlushRecord &flush_rec,
                          std::vector<char> &buf)
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

void SerializeFlushRecord(const txservice::FlushRecord &flush_rec,
                          std::string &rec_str)
{
    if (flush_rec.payload_status_ != txservice::RecordStatus::Deleted)
    {
        const txservice::BlobTxRecord *rec =
            dynamic_cast<const txservice::BlobTxRecord *>(flush_rec.Payload());
        assert(rec != nullptr);

        rec_str.resize(sizeof(int8_t) + sizeof(int64_t) + rec->value_.size());
        char *p = rec_str.data();

        int8_t deleted = 0;
        int64_t commit_ts = flush_rec.commit_ts_;

        std::memcpy(p, &deleted, sizeof(int8_t));
        p += sizeof(int8_t);
        std::memcpy(p, &commit_ts, sizeof(int64_t));
        p += sizeof(int64_t);
        std::memcpy(p, rec->value_.c_str(), rec->value_.size());
    }
    else
    {
        rec_str.resize(sizeof(int8_t) + sizeof(int64_t));
        char *p = rec_str.data();

        int8_t deleted = 1;
        int64_t commit_ts = flush_rec.commit_ts_;

        std::memcpy(p, &deleted, sizeof(int8_t));
        p += sizeof(int8_t);
        std::memcpy(p, &commit_ts, sizeof(int64_t));
    }
}

#if ROCKSDB_CLOUD_FS()
std::string MakeCloudManifestCookie(int64_t cc_ng_id, int64_t term)
{
    return std::to_string(cc_ng_id) + "-" + std::to_string(term);
}

std::string MakeCloudManifestFile(const std::string &dbname,
                                  int64_t cc_ng_id,
                                  int64_t term)
{
    if (cc_ng_id < 0 || term < 0)
    {
        return dbname + "/CLOUDMANIFEST";
    }

    assert(cc_ng_id >= 0 && term >= 0);

    return dbname + "/CLOUDMANIFEST-" + std::to_string(cc_ng_id) + "-" +
           std::to_string(term);
}

bool IsCloudManifestFile(const std::string &filename)
{
    return filename.find("CLOUDMANIFEST") != std::string::npos;
}

// Helper function to split a string by a delimiter
inline std::vector<std::string> SplitString(const std::string &str,
                                            char delimiter)
{
    std::vector<std::string> tokens;
    std::stringstream ss(str);
    std::string token;
    while (std::getline(ss, token, delimiter))
    {
        tokens.push_back(token);
    }
    return tokens;
}

bool GetCookieFromCloudManifestFile(const std::string &filename,
                                    int64_t &cc_ng_id,
                                    int64_t &term)
{
    const std::string prefix = "CLOUDMANIFEST";
    auto pos = filename.rfind('/');
    std::string manifest_part =
        (pos != std::string::npos) ? filename.substr(pos + 1) : filename;

    // Check if the filename starts with "CLOUDMANIFEST"
    if (manifest_part.find(prefix) != 0)
    {
        return false;
    }

    // Remove the prefix "CLOUDMANIFEST" to parse the rest
    std::string suffix = manifest_part.substr(prefix.size());

    // If there's no suffix
    if (suffix.empty())
    {
        cc_ng_id = -1;
        term = -1;
        return false;
    }
    else if (suffix[0] == '-')
    {
        // Parse the "-cc_ng_id-term" format
        suffix = suffix.substr(1);  // Remove the leading '-'
        std::vector<std::string> parts = SplitString(suffix, '-');
        if (parts.size() != 2)
        {
            return false;
        }

        bool res =
            EloqKV::string2ll(parts[0].c_str(), parts[0].size(), cc_ng_id);
        if (!res)
        {
            return false;
        }
        res = EloqKV::string2ll(parts[1].c_str(), parts[1].size(), term);
        return res;
    }

    return false;
}

int64_t FindMaxTermFromCloudManifestFiles(
    const std::shared_ptr<ROCKSDB_NAMESPACE::CloudStorageProvider>
        &storage_provider,
    const std::string &bucket_prefix,
    const std::string &bucket_name,
    const int64_t cc_ng_id_in_cookie)
{
    // find the max term cookie from cloud manifest files
    // read only db should be opened with the latest cookie
    std::vector<std::string> cloud_objects;
    auto st = storage_provider->ListCloudObjects(
        bucket_prefix + bucket_name, "rocksdb_cloud", &cloud_objects);
    if (!st.ok())
    {
        LOG(ERROR) << "Failed to list cloud objects, error: " << st.ToString();
        return -1;
    }

    int64_t max_term = -1;
    for (const auto &object : cloud_objects)
    {
        if (IsCloudManifestFile(object))
        {
            int64_t cc_ng_id;
            int64_t term;
            bool res = GetCookieFromCloudManifestFile(object, cc_ng_id, term);
            if (cc_ng_id_in_cookie == cc_ng_id && res)
            {
                if (term > max_term)
                {
                    max_term = term;
                }
            }
        }
    }
    DLOG(INFO) << "FindMaxTermFromCloudManifestFiles, cc_ng_id: "
               << cc_ng_id_in_cookie << " max_term: " << max_term;
    return max_term;
}
#endif

void DeserializeRecord(const char *payload,
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
#endif

}  // namespace EloqShare
