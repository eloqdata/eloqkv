#pragma once

#include <cassert>
#include <string>

#include "tx_service/include/type.h"

#define KV_DATA_STORE_TYPE_CASSANDRA 1
#define KV_DATA_STORE_TYPE_DYNAMODB 2
#define KV_DATA_STORE_TYPE_ROCKSDB 3
#define ROCKSDB_CLOUD_FS_TYPE_S3 1
#define ROCKSDB_CLOUD_FS_TYPE_GCS 2

#define ROCKSDB_CLOUD_FS()                                                     \
    (defined(ROCKSDB_CLOUD_FS_TYPE) &&                                         \
     (ROCKSDB_CLOUD_FS_TYPE == ROCKSDB_CLOUD_FS_TYPE_S3 ||                     \
      ROCKSDB_CLOUD_FS_TYPE == ROCKSDB_CLOUD_FS_TYPE_GCS))

namespace EloqDS
{
template <typename KeyT, typename ValueT>
struct ScanHeapTuple
{
    ScanHeapTuple(uint32_t shard_id) : sid_(shard_id)
    {
    }

    std::unique_ptr<KeyT> key_{nullptr};
    std::unique_ptr<ValueT> rec_{nullptr};
    uint64_t version_ts_;
    bool deleted_;
    // sid_ is the offset in shard_scan_XX vectors.
    uint32_t sid_;
};

template <typename KeyT, typename ValueT>
struct CacheCompare
{
    bool operator()(const ScanHeapTuple<KeyT, ValueT> &lhs,
                    const ScanHeapTuple<KeyT, ValueT> &rhs)
    {
        return !(*lhs.key_ < *rhs.key_);
    }
};

template <typename KeyT, typename ValueT>
struct CacheReverseCompare
{
    bool operator()(const ScanHeapTuple<KeyT, ValueT> &lhs,
                    const ScanHeapTuple<KeyT, ValueT> &rhs)
    {
        return *lhs.key_ < *rhs.key_;
    }
};

static inline std::string SerializeSchemaImage(
    const std::string &frm,
    const std::string &kv_info,
    const std::string &key_schemas_ts)
{
    size_t len = frm.length();
    std::string res;

    res.append(reinterpret_cast<const char *>(&len), sizeof(len));
    res.append(frm.data(), frm.length());
    len = kv_info.length();
    res.append(reinterpret_cast<const char *>(&len), sizeof(len));
    res.append(kv_info.data(), kv_info.length());
    len = key_schemas_ts.length();
    res.append(reinterpret_cast<const char *>(&len), sizeof(len));
    res.append(key_schemas_ts.data(), key_schemas_ts.length());

    return res;
}

static inline void DeserializeSchemaImage(const std::string &image,
                                          std::string &frm,
                                          std::string &kv_info,
                                          std::string &key_schemas_ts)
{
    size_t offset = 0;
    const char *buf = image.data();
    size_t len_val;
    len_val = *(size_t *) (buf + offset);
    offset += sizeof(len_val);
    frm.append(buf + offset, len_val);
    offset += len_val;

    len_val = *(size_t *) (buf + offset);
    offset += sizeof(len_val);
    kv_info.append(buf + offset, len_val);
    offset += len_val;

    len_val = *(size_t *) (buf + offset);
    offset += sizeof(len_val);
    key_schemas_ts.append(buf + offset, len_val);
    offset += len_val;

    assert(offset == image.length());
}

struct TableKeySchemaTs
{
    TableKeySchemaTs() = default;
    TableKeySchemaTs(const std::string &key_schemas_ts_str)
    {
        std::stringstream ts_ss(key_schemas_ts_str);
        std::istream_iterator<std::string> ts_b(ts_ss);
        std::istream_iterator<std::string> ts_e;
        std::vector<std::string> schemas_ts(ts_b, ts_e);
        pk_schema_ts_ = std::stoull(schemas_ts.at(0));
        for (size_t idx = 1; idx < schemas_ts.size(); ++idx)
        {
            txservice::TableType table_type = txservice::TableType::Secondary;
            if (schemas_ts[idx].find(txservice::UNIQUE_INDEX_NAME_PREFIX) !=
                std::string::npos)
            {
                table_type = txservice::TableType::UniqueSecondary;
            }
            else if (schemas_ts[idx].find(txservice::INDEX_NAME_PREFIX) !=
                     std::string::npos)
            {
                table_type = txservice::TableType::Secondary;
            }
            else
            {
                assert(false && "Unknown secondary key type.");
            }
            txservice::TableName table_name(schemas_ts[idx], table_type);
            ++idx;
            sk_schemas_ts_.try_emplace(std::move(table_name),
                                       std::stoull(schemas_ts[idx]));
        }
    }

    std::string Serialize() const

    {
        std::string output_str;
        size_t len_sizeof = sizeof(size_t);

        std::string table_ts(std::to_string(pk_schema_ts_));
        size_t len_val = table_ts.size();
        char *len_ptr = reinterpret_cast<char *>(&len_val);
        output_str.append(len_ptr, len_sizeof);
        output_str.append(table_ts.data(), len_val);

        std::string index_tables_ts;
        if (sk_schemas_ts_.size() != 0)
        {
            for (auto it = sk_schemas_ts_.cbegin(); it != sk_schemas_ts_.cend();
                 ++it)
            {
                index_tables_ts.append(it->first.StringView())
                    .append(" ")
                    .append(std::to_string(it->second))
                    .append(" ");
            }
            index_tables_ts.erase(index_tables_ts.size() - 1);
        }
        else
        {
            index_tables_ts.clear();
        }

        len_val = index_tables_ts.size();
        output_str.append(len_ptr, len_sizeof);
        output_str.append(index_tables_ts.data(), len_val);

        return output_str;
    }

    void Deserialize(const char *buf, size_t &offset)

    {
        if (buf == nullptr || buf[0] == '\0')
        {
            return;
        }
        size_t len_sizeof = sizeof(size_t);
        size_t *len_ptr = (size_t *) (buf + offset);
        size_t len_val = *len_ptr;
        offset += len_sizeof;

        pk_schema_ts_ = std::stoull(std::string(buf + offset, len_val));
        offset += len_val;

        len_ptr = (size_t *) (buf + offset);
        len_val = *len_ptr;
        offset += len_sizeof;
        if (len_val != 0)
        {
            std::string index_tables_ts(buf + offset, len_val);
            std::stringstream sk_ss(index_tables_ts);
            std::istream_iterator<std::string> sk_b(sk_ss);
            std::istream_iterator<std::string> sk_e;
            std::vector<std::string> sk_iter(sk_b, sk_e);
            for (auto it = sk_iter.begin(); it != sk_iter.end(); ++it)
            {
                txservice::TableType table_type;
                if (it->find(txservice::UNIQUE_INDEX_NAME_PREFIX) !=
                    std::string::npos)
                {
                    table_type = txservice::TableType::UniqueSecondary;
                }
                else if (it->find(txservice::INDEX_NAME_PREFIX) !=
                         std::string::npos)
                {
                    table_type = txservice::TableType::Secondary;
                }
                else
                {
                    assert(false && "Unknown secondary key type.");
                }
                txservice::TableName table_name(*it, table_type);
                sk_schemas_ts_.try_emplace(std::move(table_name),
                                           std::stoull(*(++it)));
            }
        }
        else
        {
            sk_schemas_ts_.clear();
        }
        offset += len_val;
    }

    /**
     * @brief Get the key schema ts of the specified [primary/secondary] key.
     *
     * @param table_name key schema name.
     *
     * @return If the key is newly added, return 1. Otherwise, return normal ts.
     */
    uint64_t GetKeySchemaTs(const txservice::TableName &table_name) const
    {
        if (table_name.Type() == txservice::TableType::Primary)
        {
            return pk_schema_ts_;
        }
        else
        {
            auto v_it = sk_schemas_ts_.find(table_name);
            return v_it == sk_schemas_ts_.end() ? 1 : v_it->second;
        }
    }

    uint64_t pk_schema_ts_{1};
    std::unordered_map<txservice::TableName, uint64_t> sk_schemas_ts_;
};

}  // namespace EloqDS
