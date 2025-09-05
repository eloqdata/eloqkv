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
#include "eloq_catalog_factory.h"

#include <memory>
#include <vector>

#include "cc/ccm_scanner.h"
#include "cc/object_cc_map.h"
#include "cc/range_cc_map.h"
#include "cc/template_cc_map.h"
#include "eloq_key.h"
#include "kv_store.h"
#include "redis_object.h"
#include "schema.h"
#include "sharder.h"
#include "tx_key.h"
#include "tx_service/include/sequences/sequences.h"
#include "type.h"

#if (defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_S3) ||                      \
     defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_GCS) ||                     \
     defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB) ||                               \
     defined(DATA_STORE_TYPE_ELOQDSS_ELOQSTORE))
#define ELOQDS 1
#endif

#if defined(DATA_STORE_TYPE_DYNAMODB)
#include "store_handler/dynamo_handler.h"
#elif defined(DATA_STORE_TYPE_ROCKSDB)
#include "store_handler/rocksdb_handler.h"
#elif ELOQDS
#include "data_store_service_client.h"
#endif

namespace EloqKV
{
RedisTableSchema::RedisTableSchema(const txservice::TableName &redis_table_name,
                                   const std::string &catalog_image,
                                   uint64_t version)
    : redis_table_name_(redis_table_name),
      schema_image_(catalog_image),
      version_(version)
{
#if defined(DATA_STORE_TYPE_DYNAMODB)
    // TODO(lokax): catalog image format
    kv_info_ = std::make_unique<EloqDS::DynamoCatalogInfo>();
    // Catalog image only stores kv_table_name for now.
    const std::string &kv_table_name = catalog_image;
    // Use table version as key schema version.
    uint64_t key_schema_ts = version;

    kv_info_->kv_table_name_ = kv_table_name;
    key_schema_ = std::make_unique<RedisKeySchema>(key_schema_ts);
    record_schema_ = std::make_unique<RedisRecordSchema>();

#elif defined(DATA_STORE_TYPE_ROCKSDB)
    // TODO(lokax): catalog image format
    kv_info_ = std::make_unique<RocksDBCatalogInfo>();
    // Catalog image only stores kv_table_name for now.
    const std::string &kv_table_name = catalog_image;
    // Use table version as key schema version.
    uint64_t key_schema_ts = version;

    kv_info_->kv_table_name_ = kv_table_name;
    key_schema_ = std::make_unique<RedisKeySchema>(key_schema_ts);
    record_schema_ = std::make_unique<RedisRecordSchema>();
#elif ELOQDS
    // TODO(lokax): catalog image format
    kv_info_ = std::make_unique<txservice::KVCatalogInfo>();
    // Catalog image only stores kv_table_name for now.
    const std::string &kv_table_name = catalog_image;
    // Use table version as key schema version.
    uint64_t key_schema_ts = version;

    kv_info_->kv_table_name_ = kv_table_name;
    key_schema_ = std::make_unique<RedisKeySchema>(key_schema_ts);
    record_schema_ = std::make_unique<RedisRecordSchema>();
#endif
}

txservice::TableSchema::uptr RedisTableSchema::Clone() const
{
    return std::make_unique<RedisTableSchema>(
        redis_table_name_, schema_image_, version_);
}

std::unique_ptr<txservice::TxCommand> RedisTableSchema::CreateTxCommand(
    std::string_view cmd_image) const
{
    std::unique_ptr<txservice::TxCommand> cmd = nullptr;
    // first byte is the command type
    const uint8_t cmd_type =
        *reinterpret_cast<decltype(cmd_type) *>(cmd_image.data());
    RedisCommandType redis_cmd_type = static_cast<RedisCommandType>(cmd_type);
    switch (redis_cmd_type)
    {
    case RedisCommandType::SET:
        cmd = std::make_unique<SetCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::SETBIT:
        cmd = std::make_unique<SetBitCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::GETBIT:
        cmd = std::make_unique<GetBitCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::SETRANGE:
        cmd = std::make_unique<SetRangeCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::GETRANGE:
    case RedisCommandType::SUBSTR:
        cmd = std::make_unique<GetRangeCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::BITCOUNT:
        cmd = std::make_unique<BitCountCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::APPEND:
        cmd = std::make_unique<AppendCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::BITFIELD:
    case RedisCommandType::BITFIELD_RO:
        cmd = std::make_unique<BitFieldCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::BITPOS:
        cmd = std::make_unique<BitPosCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::INCRBYFLOAT:
        cmd = std::make_unique<FloatOpCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::RPUSH:
        cmd = std::make_unique<RPushCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::LPUSH:
        cmd = std::make_unique<LPushCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::LPOP:
        cmd = std::make_unique<LPopCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::RPOP:
        cmd = std::make_unique<RPopCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::HSET:
        cmd = std::make_unique<HSetCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::DEL:
        cmd = std::make_unique<DelCommand>();
        break;
    case RedisCommandType::ZADD:
        cmd = std::make_unique<ZAddCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::ZREM:
        cmd = std::make_unique<ZRemCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::SZSCAN:
        cmd = std::make_unique<SZScanCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::INCR:
    case RedisCommandType::INCRBY:
    case RedisCommandType::DECR:
    case RedisCommandType::DECRBY:
        cmd = std::make_unique<IntOpCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::HDEL:
        cmd = std::make_unique<HDelCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::HINCRBY:
        cmd = std::make_unique<HIncrByCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::HINCRBYFLOAT:
        cmd = std::make_unique<HIncrByFloatCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::HSETNX:
        cmd = std::make_unique<HSetNxCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::HMGET:
        cmd = std::make_unique<HMGetCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::HVALS:
        cmd = std::make_unique<HValsCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::LTRIM:
        cmd = std::make_unique<LTrimCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::LINDEX:
        cmd = std::make_unique<LIndexCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::LINSERT:
        cmd = std::make_unique<LInsertCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::LPOS:
        cmd = std::make_unique<LPosCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::LSET:
        cmd = std::make_unique<LSetCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::LREM:
        cmd = std::make_unique<LRemCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::LMOVEPOP:
        cmd = std::make_unique<LMovePopCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::LMOVEPUSH:
        cmd = std::make_unique<LMovePushCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::LPUSHX:
        cmd = std::make_unique<LPushXCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::RPUSHX:
        cmd = std::make_unique<RPushXCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::SADD:
        cmd = std::make_unique<SAddCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::SREM:
        cmd = std::make_unique<SRemCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::SCARD:
        cmd = std::make_unique<SCardCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::SPOP:
        cmd = std::make_unique<SPopCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::GET:
        cmd = std::make_unique<GetCommand>();
        // readonly command has no content to deserialize.
        break;
    case RedisCommandType::GETDEL:
        cmd = std::make_unique<GetDelCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::DUMP:
        cmd = std::make_unique<DumpCommand>();
        // readonly command has no content to deserialize.
        break;

    case RedisCommandType::RESTORE:
        cmd = std::make_unique<RestoreCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::STORE_LIST:
        cmd = std::make_unique<StoreListCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::LLEN:
        cmd = std::make_unique<LLenCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::LRANGE:
        cmd = std::make_unique<LRangeCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::HGET:
        cmd = std::make_unique<HGetCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::HLEN:
        cmd = std::make_unique<HLenCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::HSTRLEN:
        cmd = std::make_unique<HStrLenCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::HKEYS:
        cmd = std::make_unique<HKeysCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::HGETALL:
        cmd = std::make_unique<HGetAllCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::HEXISTS:
        cmd = std::make_unique<HExistsCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::HRANDFIELD:
        cmd = std::make_unique<HRandFieldCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::HSCAN:
        cmd = std::make_unique<HScanCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::EXISTS:
        cmd = std::make_unique<ExistsCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::ZRANGE:
    case RedisCommandType::ZRANGEBYSCORE:
    case RedisCommandType::ZRANGEBYLEX:
    case RedisCommandType::ZRANGEBYRANK:
    case RedisCommandType::ZREVRANGE:
    case RedisCommandType::ZREVRANGEBYSCORE:
    case RedisCommandType::ZREVRANGEBYLEX:
        cmd = std::make_unique<ZRangeCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::ZREMRANGE:
        cmd = std::make_unique<ZRemRangeCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::ZSCORE:
        cmd = std::make_unique<ZScoreCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::ZLEXCOUNT:
        cmd = std::make_unique<ZLexCountCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::ZPOPMIN:
        cmd = std::make_unique<ZPopCommand>(ZPopCommand::PopType::POPMIN);
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::ZPOPMAX:
        cmd = std::make_unique<ZPopCommand>(ZPopCommand::PopType::POPMAX);
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::ZCARD:
        cmd = std::make_unique<ZCardCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::ZCOUNT:
        cmd = std::make_unique<ZCountCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::ZMSCORE:
        cmd = std::make_unique<ZMScoreCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::SMEMBERS:
        cmd = std::make_unique<SMembersCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::STRLEN:
        cmd = std::make_unique<StrLenCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::WATCH:
        cmd = std::make_unique<WatchCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::ZSCAN:
        cmd = std::make_unique<ZScanCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::SORTABLE_LOAD:
        cmd = std::make_unique<SortableLoadCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::SISMEMBER:
        cmd = std::make_unique<SIsMemberCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::SMISMEMBER:
        cmd = std::make_unique<SMIsMemberCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::SRANDMEMBER:
        cmd = std::make_unique<SRandMemberCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::SSCAN:
        cmd = std::make_unique<SScanCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::ZRANDMEMBER:
        cmd = std::make_unique<ZRandMemberCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::ZRANK:
        cmd = std::make_unique<ZRankCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;

    case RedisCommandType::BLOCKPOP:
        cmd = std::make_unique<BlockLPopCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::BLOCKDISCARD:
        cmd = std::make_unique<BlockDiscardCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::TYPE:
        cmd = std::make_unique<TypeCommand>();
        break;
    case RedisCommandType::MEMORY_USAGE:
        cmd = std::make_unique<RedisMemoryUsageCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::TTL:
        cmd = std::make_unique<TTLCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::EXPIRE:
        cmd = std::make_unique<ExpireCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::PERSIST:
        cmd = std::make_unique<PersistCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::GETEX:
        cmd = std::make_unique<GetExCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    case RedisCommandType::RECOVER:
        cmd = std::make_unique<RecoverObjectCommand>();
        cmd->Deserialize({cmd_image.data() + sizeof(cmd_type),
                          cmd_image.size() - sizeof(cmd_type)});
        break;
    default:
        LOG(ERROR) << "Unimplemented command type "
                   << static_cast<int>(redis_cmd_type);
        assert(false);
        break;
    }

    return cmd;
}

txservice::TableSchema::uptr RedisCatalogFactory::CreateTableSchema(
    const txservice::TableName &table_name,
    const std::string &catalog_image,
    uint64_t version)
{
    return std::make_unique<RedisTableSchema>(
        table_name, catalog_image, version);
}

txservice::CcMap::uptr RedisCatalogFactory::CreatePkCcMap(
    const txservice::TableName &table_name,
    const txservice::TableSchema *table_schema,
    bool ccm_has_full_entries,
    txservice::CcShard *shard,
    txservice::NodeGroupId cc_ng_id)
{
    uint64_t table_version = table_schema->Version();
    assert(table_version == table_schema->KeySchema()->SchemaTs());

    return std::make_unique<txservice::ObjectCcMap<EloqKey, RedisEloqObject>>(
        shard,
        cc_ng_id,
        table_name,
        table_version,
        table_schema,
        ccm_has_full_entries);
}

txservice::CcMap::uptr RedisCatalogFactory::CreateSkCcMap(
    const txservice::TableName &table_name,
    const txservice::TableSchema *table_schema,
    txservice::CcShard *shard,
    txservice::NodeGroupId cc_ng_id)
{
    return nullptr;
}

txservice::CcMap::uptr RedisCatalogFactory::CreateRangeMap(
    const txservice::TableName &range_table_name,
    const txservice::TableSchema *table_schema,
    uint64_t schema_ts,
    txservice::CcShard *shard,
    txservice::NodeGroupId ng_id)
{
    assert(range_table_name.Type() == txservice::TableType::RangePartition);
    return std::make_unique<txservice::RangeCcMap<EloqKey>>(
        range_table_name, table_schema, schema_ts, shard, ng_id);
}

std::unique_ptr<txservice::CcScanner> RedisCatalogFactory::CreatePkCcmScanner(
    txservice::ScanDirection direction, const txservice::KeySchema *key_schema)
{
    return std::make_unique<
        txservice::HashParitionCcScanner<EloqKey, RedisEloqObject>>(
        direction, txservice::ScanIndexType::Primary, key_schema);
}

std::unique_ptr<txservice::CcScanner> RedisCatalogFactory::CreateSkCcmScanner(
    txservice::ScanDirection direction,
    const txservice::KeySchema *compound_key_schema)
{
    return nullptr;
}

std::unique_ptr<txservice::CcScanner>
RedisCatalogFactory::CreateRangeCcmScanner(
    txservice::ScanDirection direction,
    const txservice::KeySchema *key_schema,
    const txservice::TableName &range_table_name)
{
    return nullptr;
}

std::unique_ptr<txservice::Statistics>
RedisCatalogFactory::CreateTableStatistics(
    const txservice::TableSchema *table_schema, txservice::NodeGroupId cc_ng_id)
{
    return nullptr;
}

std::unique_ptr<txservice::Statistics>
RedisCatalogFactory::CreateTableStatistics(
    const txservice::TableSchema *table_schema,
    std::unordered_map<txservice::TableName,
                       std::pair<uint64_t, std::vector<txservice::TxKey>>>
        sample_pool_map,
    txservice::CcShard *ccs,
    txservice::NodeGroupId cc_ng_id)
{
    return nullptr;
}

txservice::TxKey RedisCatalogFactory::NegativeInfKey()
{
    return txservice::TxKey(EloqKey::NegativeInfinity());
}

txservice::TxKey RedisCatalogFactory::PositiveInfKey()
{
    return txservice::TxKey(EloqKey::PositiveInfinity());
}

size_t RedisCatalogFactory::KeyHash(
    const char *buf,
    size_t offset,
    const txservice::KeySchema *key_schema) const
{
    return EloqKey::HashFromSerializedKey(buf, offset);
}

}  // namespace EloqKV
