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
#include "redis_command.h"

#include <brpc/acceptor.h>
#include <butil/endpoint.h>
#include <butil/logging.h>
#include <strings.h>
#include <sys/times.h>
#include <unistd.h>

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "cc/cc_entry.h"
#include "cc/cc_shard.h"
#include "eloq_catalog_factory.h"
#include "eloq_key.h"
#include "eloq_string.h"
#include "local_cc_shards.h"
#include "output_handler.h"
#include "redis/commands.h"
#include "redis/server.h"
#include "redis_connection_context.h"
#include "redis_errors.h"
#include "redis_hash_object.h"
#include "redis_list_object.h"
#include "redis_object.h"
#include "redis_service.h"
#include "redis_set_object.h"
#include "redis_string_match.h"
#include "redis_string_num.h"
#include "redis_string_object.h"
#include "redis_zset_object.h"
#include "remote/remote_type.h"
#include "sharder.h"
#include "tx_command.h"
#include "tx_request.h"
#include "tx_service.h"
#include "type.h"

extern "C"
{
#include "crcspeed/crc64speed.h"
}

extern struct redisCommand redisCommandTable[];

namespace EloqKV
{
// Global variable defined in redis_service.cpp
extern int databases;
// Global variable defined in redis_service.cpp
extern std::string requirepass;
// Global variable defined in redis_service.cpp
extern brpc::Acceptor *server_acceptor;

thread_local CcRequestPool<DbSizeCc> dbsize_pool_;

const std::vector<std::pair<const char *, RedisCommandType>> command_types{{
    {"echo", RedisCommandType::ECHO},
    {"info", RedisCommandType::INFO},
    {"cluster", RedisCommandType::CLUSTER},
    {"dbsize", RedisCommandType::DBSIZE},
    {"time", RedisCommandType::TIME},
    {"slowlog", RedisCommandType::SLOWLOG},
    {"select", RedisCommandType::SELECT},
    {"config", RedisCommandType::CONFIG},
    {"ping", RedisCommandType::PING},
    {"eval", RedisCommandType::EVAL},
    {"dump", RedisCommandType::DUMP},
    {"restore", RedisCommandType::RESTORE},
    {"get", RedisCommandType::GET},
    {"set", RedisCommandType::SET},
    {"getset", RedisCommandType::GETSET},
    {"strlen", RedisCommandType::STRLEN},
    {"setnx", RedisCommandType::SETNX},
    {"psetex", RedisCommandType::PSETEX},
    {"getrange", RedisCommandType::GETRANGE},
    {"substr", RedisCommandType::SUBSTR},
    {"incrbyfloat", RedisCommandType::INCRBYFLOAT},
    {"setrange", RedisCommandType::SETRANGE},
    {"append", RedisCommandType::APPEND},
    {"setex", RedisCommandType::SETEX},
    {"getbit", RedisCommandType::GETBIT},
    {"setbit", RedisCommandType::SETBIT},
    {"bitcount", RedisCommandType::BITCOUNT},
    {"bitfield", RedisCommandType::BITFIELD},
    {"bitfield_ro", RedisCommandType::BITFIELD_RO},
    {"bitpos", RedisCommandType::BITPOS},
    {"bitop", RedisCommandType::BITOP},
    {"rpush", RedisCommandType::RPUSH},
    {"lrange", RedisCommandType::LRANGE},
    {"lpush", RedisCommandType::LPUSH},
    {"lpop", RedisCommandType::LPOP},
    {"rpop", RedisCommandType::RPOP},
    {"lmpop", RedisCommandType::LMPOP},
    {"lmpop", RedisCommandType::LMPOP},
    {"blmove", RedisCommandType::BLMOVE},
    {"blmpop", RedisCommandType::BLMPOP},
    {"blpop", RedisCommandType::BLPOP},
    {"brpop", RedisCommandType::BRPOP},
    {"brpoplpush", RedisCommandType::BRPOPLPUSH},
    {"hset", RedisCommandType::HSET},
    {"hget", RedisCommandType::HGET},
    {"incr", RedisCommandType::INCR},
    {"decr", RedisCommandType::DECR},
    {"incrby", RedisCommandType::INCRBY},
    {"decrby", RedisCommandType::DECRBY},
    {"type", RedisCommandType::TYPE},
    {"del", RedisCommandType::DEL},
    {"exists", RedisCommandType::EXISTS},
    {"zadd", RedisCommandType::ZADD},
    {"zcount", RedisCommandType::ZCOUNT},
    {"zcard", RedisCommandType::ZCARD},
    {"zrange", RedisCommandType::ZRANGE},
    {"zrem", RedisCommandType::ZREM},
    {"zrangebyscore", RedisCommandType::ZRANGEBYSCORE},
    {"zrangebyrank", RedisCommandType::ZRANGEBYRANK},
    {"zrangebylex", RedisCommandType::ZRANGEBYLEX},
    {"zremrangebyscore", RedisCommandType::ZREMRANGE},
    {"zremrangebylex", RedisCommandType::ZREMRANGE},
    {"zremrangebyrank", RedisCommandType::ZREMRANGE},
    {"zlexcount", RedisCommandType::ZLEXCOUNT},
    {"zpopmin", RedisCommandType::ZPOPMIN},
    {"zpopmax", RedisCommandType::ZPOPMAX},
    {"zrandmember", RedisCommandType::ZRANDMEMBER},
    {"zrank", RedisCommandType::ZRANK},
    {"zscore", RedisCommandType::ZSCORE},
    {"zrevrange", RedisCommandType::ZREVRANGE},
    {"zrevrangebylex", RedisCommandType::ZREVRANGEBYLEX},
    {"zrevrangebyscore", RedisCommandType::ZREVRANGEBYSCORE},
    {"zrevrange", RedisCommandType::ZREVRANGE},
    {"zrevrank", RedisCommandType::ZREVRANK},
    {"zmscore", RedisCommandType::ZMSCORE},
    {"zmpop", RedisCommandType::ZMPOP},
    {"zunion", RedisCommandType::ZUNION},
    {"zunionstore", RedisCommandType::ZUNIONSTORE},
    {"zinter", RedisCommandType::ZINTER},
    {"zinterstore", RedisCommandType::ZINTERSTORE},
    {"zintercard", RedisCommandType::ZINTERCARD},
    {"zdiff", RedisCommandType::ZDIFF},
    {"zdiffstore", RedisCommandType::ZDIFFSTORE},
    {"zrangestore", RedisCommandType::ZRANGESTORE},
    {"zincrby", RedisCommandType::ZINCRBY},
    {"zscan", RedisCommandType::ZSCAN},
    {"mget", RedisCommandType::MGET},
    {"mset", RedisCommandType::MSET},
    {"msetnx", RedisCommandType::MSETNX},
    {"hdel", RedisCommandType::HDEL},
    {"hlen", RedisCommandType::HLEN},
    {"hstrlen", RedisCommandType::HSTRLEN},
    {"hexists", RedisCommandType::HEXISTS},
    {"hgetall", RedisCommandType::HGETALL},
    {"hincrby", RedisCommandType::HINCRBY},
    {"hincrbyfloat", RedisCommandType::HINCRBYFLOAT},
    {"hmset", RedisCommandType::HSET},
    {"hmget", RedisCommandType::HMGET},
    {"hkeys", RedisCommandType::HKEYS},
    {"hvals", RedisCommandType::HVALS},
    {"hsetnx", RedisCommandType::HSETNX},
    {"hrandfield", RedisCommandType::HRANDFIELD},
    {"hscan", RedisCommandType::HSCAN},
    {"llen", RedisCommandType::LLEN},
    {"ltrim", RedisCommandType::LTRIM},
    {"lindex", RedisCommandType::LINDEX},
    {"linsert", RedisCommandType::LINSERT},
    {"lpos", RedisCommandType::LPOS},
    {"lset", RedisCommandType::LSET},
    {"lmove", RedisCommandType::LMOVE},
    {"rpoplpush", RedisCommandType::RPOPLPUSH},
    {"lrem", RedisCommandType::LREM},
    {"lpushx", RedisCommandType::LPUSHX},
    {"rpushx", RedisCommandType::RPUSHX},
    {"sadd", RedisCommandType::SADD},
    {"smembers", RedisCommandType::SMEMBERS},
    {"srem", RedisCommandType::SREM},
    {"flushdb", RedisCommandType::FLUSHDB},
    {"flushall", RedisCommandType::FLUSHALL},
    {"scard", RedisCommandType::SCARD},
    {"sdiff", RedisCommandType::SDIFF},
    {"sdiffstore", RedisCommandType::SDIFFSTORE},
    {"sinter", RedisCommandType::SINTER},
    {"sintercard", RedisCommandType::SINTERCARD},
    {"sinterstore", RedisCommandType::SINTERSTORE},
    {"sismember", RedisCommandType::SISMEMBER},
    {"smismember", RedisCommandType::SMISMEMBER},
    {"smove", RedisCommandType::SMOVE},
    {"spop", RedisCommandType::SPOP},
    {"srandmember", RedisCommandType::SRANDMEMBER},
    {"sscan", RedisCommandType::SSCAN},
    {"sunion", RedisCommandType::SUNION},
    {"sunionstore", RedisCommandType::SUNIONSTORE},
    {"sort", RedisCommandType::SORT},
    {"sort_ro", RedisCommandType::SORT},
    {"scan", RedisCommandType::SCAN},
    {"keys", RedisCommandType::KEYS},
#ifdef WITH_FAULT_INJECT
    {"fault_inject", RedisCommandType::FAULT_INJECT},
#endif
    {"expire", RedisCommandType::EXPIRE},
    {"ttl", RedisCommandType::TTL},
    {"persist", RedisCommandType::PERSIST},
    {"pexpire", RedisCommandType::PEXPIRE},
    {"pttl", RedisCommandType::PTTL},
    {"expiretime", RedisCommandType::EXPIRETIME},
    {"pexpiretime", RedisCommandType::PEXPIRETIME},
    {"setex", RedisCommandType::SETEX},
    {"psetex", RedisCommandType::PSETEX},
    {"expireat", RedisCommandType::EXPIREAT},
    {"pexpireat", RedisCommandType::PEXPIREAT},
    {"getex", RedisCommandType::GETEX},
    {"begin", RedisCommandType::BEGIN},
    {"rollback", RedisCommandType::ROLLBACK},
    {"commit", RedisCommandType::COMMIT},
    {"publish", RedisCommandType::PUBLISH},
    {"subscribe", RedisCommandType::SUBSCRIBE},
    {"unsubscribe", RedisCommandType::UNSUBSCRIBE},
    {"psubscribe", RedisCommandType::PSUBSCRIBE},
    {"punsubscribe", RedisCommandType::PUNSUBSCRIBE},
    {"eloqvec.create", RedisCommandType::ELOQVEC_CREATE},
    {"eloqvec.info", RedisCommandType::ELOQVEC_INFO},
}};

/*
 * Copyright (c) 2009-2016, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
const std::array<std::pair<unsigned long long, const char *>, 25> command_flags{
    {{CMD_WRITE, "write"},
     {CMD_READONLY, "readonly"},
     {CMD_DENYOOM, "denyoom"},
     {CMD_MODULE, "module"},
     {CMD_ADMIN, "admin"},
     {CMD_PUBSUB, "pubsub"},
     {CMD_NOSCRIPT, "noscript"},
     {CMD_BLOCKING, "blocking"},
     {CMD_LOADING, "loading"},
     {CMD_STALE, "stale"},
     {CMD_SKIP_MONITOR, "skip_monitor"},
     {CMD_SKIP_SLOWLOG, "skip_slowlog"},
     {CMD_ASKING, "asking"},
     {CMD_FAST, "fast"},
     {CMD_NO_AUTH, "no_auth"},
     /* {CMD_MAY_REPLICATE,     "may_replicate"},, Hidden on purpose */
     /* {CMD_SENTINEL,          "sentinel"}, Hidden on purpose */
     /* {CMD_ONLY_SENTINEL,     "only_sentinel"}, Hidden on purpose */
     {CMD_NO_MANDATORY_KEYS, "no_mandatory_keys"},
     /* {CMD_PROTECTED,         "protected"}, Hidden on purpose */
     {CMD_NO_ASYNC_LOADING, "no_async_loading"},
     {CMD_NO_MULTI, "no_multi"},
     {CMD_MOVABLE_KEYS, "movablekeys"},
     {CMD_ALLOW_BUSY, "allow_busy"},
     /* {CMD_TOUCHES_ARBITRARY_KEYS,  "TOUCHES_ARBITRARY_KEYS"}, Hidden on
        purpose */
     {0, NULL}}};

const std::array<std::pair<unsigned long long, const char *>, 22>
    acl_categories{{{ACL_CATEGORY_KEYSPACE, "keyspace"},
                    {ACL_CATEGORY_READ, "read"},
                    {ACL_CATEGORY_WRITE, "write"},
                    {ACL_CATEGORY_SET, "set"},
                    {ACL_CATEGORY_SORTEDSET, "sortedset"},
                    {ACL_CATEGORY_LIST, "list"},
                    {ACL_CATEGORY_HASH, "hash"},
                    {ACL_CATEGORY_STRING, "string"},
                    {ACL_CATEGORY_BITMAP, "bitmap"},
                    {ACL_CATEGORY_HYPERLOGLOG, "hyperloglog"},
                    {ACL_CATEGORY_GEO, "geo"},
                    {ACL_CATEGORY_STREAM, "stream"},
                    {ACL_CATEGORY_PUBSUB, "pubsub"},
                    {ACL_CATEGORY_ADMIN, "admin"},
                    {ACL_CATEGORY_FAST, "fast"},
                    {ACL_CATEGORY_SLOW, "slow"},
                    {ACL_CATEGORY_BLOCKING, "blocking"},
                    {ACL_CATEGORY_DANGEROUS, "dangerous"},
                    {ACL_CATEGORY_CONNECTION, "connection"},
                    {ACL_CATEGORY_TRANSACTION, "transaction"},
                    {ACL_CATEGORY_SCRIPTING, "scripting"},
                    {0, NULL}}};

const std::array<std::pair<unsigned long long, const char *>, 12>
    doc_flag_names{{{CMD_KEY_RO, "RO"},
                    {CMD_KEY_RW, "RW"},
                    {CMD_KEY_OW, "OW"},
                    {CMD_KEY_RM, "RM"},
                    {CMD_KEY_ACCESS, "access"},
                    {CMD_KEY_UPDATE, "update"},
                    {CMD_KEY_INSERT, "insert"},
                    {CMD_KEY_DELETE, "delete"},
                    {CMD_KEY_NOT_KEY, "not_key"},
                    {CMD_KEY_INCOMPLETE, "incomplete"},
                    {CMD_KEY_VARIABLE_FLAGS, "variable_flags"},
                    {0, NULL}}};

inline bool IsOdd(unsigned long num)
{
    return num & 1;
}

inline bool IsEven(unsigned long num)
{
    return !(num & 1);
}

RedisCommandType CommandType(std::string_view cmd)
{
    // cmd are transformed to lower case by brpc redis command parser and lua
    // interpreter
    for (const auto &pair : command_types)
    {
        if (pair.first != nullptr && pair.first == cmd)
        {
            return pair.second;
        }
    }
    return RedisCommandType::UNKNOWN;
}

std::vector<const char *> CommandFlag(unsigned long long flag)
{
    std::vector<const char *> flags;
    for (const auto &pair : command_flags)
    {
        if (pair.first & flag)
        {
            flags.emplace_back(pair.second);
        }
    }
    return flags;
}

std::vector<const char *> DocFlagName(unsigned long long flag)
{
    std::vector<const char *> flags;
    for (const auto &pair : doc_flag_names)
    {
        if (pair.first & flag)
        {
            flags.emplace_back(pair.second);
        }
    }
    return flags;
}

std::vector<const char *> AclCategory(unsigned long long category,
                                      unsigned long long flag)
{
    if (flag & CMD_WRITE)
    {
        category |= ACL_CATEGORY_WRITE;
    }
    if (flag & CMD_READONLY && !(flag & ACL_CATEGORY_SCRIPTING))
    {
        category |= ACL_CATEGORY_READ;
    }
    if (flag & CMD_ADMIN)
    {
        category |= ACL_CATEGORY_ADMIN | ACL_CATEGORY_DANGEROUS;
    }
    if (flag & CMD_PUBSUB)
    {
        category |= ACL_CATEGORY_PUBSUB;
    }
    if (flag & CMD_FAST)
    {
        category |= ACL_CATEGORY_FAST;
    }
    if (flag & CMD_BLOCKING)
    {
        category |= ACL_CATEGORY_BLOCKING;
    }

    if (!(category & ACL_CATEGORY_FAST))
    {
        category |= ACL_CATEGORY_SLOW;
    }

    std::vector<const char *> categories;
    for (const auto &pair : acl_categories)
    {
        if (pair.first & category)
        {
            categories.emplace_back(pair.second);
        }
    }

    return categories;
}

void RedisCommandResult::Serialize(std::string &buf) const
{
    buf.append(reinterpret_cast<const char *>(&err_code_), sizeof(int32_t));
}

void RedisCommandResult::Deserialize(const char *buf, size_t &offset)
{
    err_code_ = *(reinterpret_cast<const int32_t *>(buf + offset));
    offset += sizeof(int32_t);
}

// Serialize first append RedisResultType.RedisResultType will be identified in
// txservice and then be parsed according to the virtual function. Refer to the
// parsing method of redis_command.
void RedisStringResult::Serialize(std::string &buf) const
{
    uint8_t redis_result_type =
        static_cast<uint8_t>(RedisResultType::StringResult);
    buf.append(reinterpret_cast<const char *>(&redis_result_type),
               sizeof(uint8_t));

    // Serialize "RedisCommandResult::err_code_"
    RedisCommandResult::Serialize(buf);

    auto *ret_ptr = reinterpret_cast<const char *>(&int_ret_);
    buf.append(ret_ptr, sizeof(int64_t));
    uint32_t sz = static_cast<uint32_t>(str_.size());
    buf.append(reinterpret_cast<const char *>(&sz), sizeof(uint32_t));
    buf.append(str_);

    sz = static_cast<uint32_t>(vct_int_.size());
    buf.append(reinterpret_cast<const char *>(&sz), sizeof(uint32_t));
    for (const auto &val : vct_int_)
    {
        buf.append(1, val.first ? 0 : 1);
        buf.append(reinterpret_cast<const char *>(&val.second),
                   sizeof(int64_t));
    }
}

void RedisStringResult::Deserialize(const char *buf, size_t &offset)
{
    // Do not need RedisResultType, skip one byte.
    offset += sizeof(uint8_t);

    // Deserialize "RedisCommandResult::err_code_"
    RedisCommandResult::Deserialize(buf, offset);

    int_ret_ = *reinterpret_cast<const int64_t *>(buf + offset);
    offset += sizeof(int64_t);

    uint32_t sz = *reinterpret_cast<const uint32_t *>(buf + offset);
    offset += sizeof(uint32_t);

    str_.clear();
    str_.append(buf + offset, sz);
    offset += sz;

    sz = *reinterpret_cast<const uint32_t *>(buf + offset);
    offset += sizeof(uint32_t);
    vct_int_.reserve(sz);
    for (uint32_t i = 0; i < sz; i++)
    {
        bool not_nil = (*(buf + offset) == 1 ? false : true);
        offset++;
        int64_t vs = *reinterpret_cast<const int64_t *>(buf + offset);
        offset += sizeof(int64_t);
        // pair<Value is not NIL, the integer value if not NIL>
        vct_int_.push_back(std::make_pair(not_nil, vs));
    }
}

void RedisIntResult::Serialize(std::string &buf) const
{
    uint8_t redis_result_type =
        static_cast<uint8_t>(RedisResultType::IntResult);
    buf.append(reinterpret_cast<const char *>(&redis_result_type),
               sizeof(uint8_t));

    // Serialize "RedisCommandResult::err_code_"
    RedisCommandResult::Serialize(buf);

    auto *int_val_ptr = reinterpret_cast<const char *>(&int_val_);
    buf.append(int_val_ptr, sizeof(int64_t));
}

void RedisIntResult::Deserialize(const char *buf, size_t &offset)
{
    // Do not need RedisResultType, skip one byte.
    offset += sizeof(uint8_t);

    // Deserialize "RedisCommandResult::err_code_"
    RedisCommandResult::Deserialize(buf, offset);

    int_val_ = *reinterpret_cast<const int64_t *>(buf + offset);
    offset += sizeof(int64_t);
}

void RedisListResult::Serialize(std::string &buf) const
{
    uint8_t redis_result_type =
        static_cast<uint8_t>(RedisResultType::ListResult);
    buf.append(reinterpret_cast<const char *>(&redis_result_type),
               sizeof(uint8_t));

    // Serialize "RedisCommandResult::err_code_"
    RedisCommandResult::Serialize(buf);
    buf.append(reinterpret_cast<const char *>(&ret_), sizeof(int64_t));

    uint8_t result_type;
    if (std::holds_alternative<std::vector<int64_t>>(result_))
    {
        result_type = static_cast<uint8_t>(ResultType::Int64Vector);
        buf.append(reinterpret_cast<const char *>(&result_type),
                   sizeof(uint8_t));
        const auto &vector_ans = std::get<std::vector<int64_t>>(result_);

        uint32_t cnt = vector_ans.size();
        buf.append(reinterpret_cast<const char *>(&cnt), sizeof(uint32_t));

        for (const auto &ans : vector_ans)
        {
            buf.append(reinterpret_cast<const char *>(&ans), sizeof(int64_t));
        }
    }
    else if (std::holds_alternative<std::vector<std::string>>(result_))
    {
        result_type = static_cast<uint8_t>(ResultType::StringVector);
        buf.append(reinterpret_cast<const char *>(&result_type),
                   sizeof(uint8_t));
        const auto &vector_ans = std::get<std::vector<std::string>>(result_);

        uint32_t cnt = vector_ans.size();
        buf.append(reinterpret_cast<const char *>(&cnt), sizeof(uint32_t));

        for (const auto &ans : vector_ans)
        {
            uint32_t size = ans.size();

            buf.append(reinterpret_cast<const char *>(&size), sizeof(uint32_t));
            buf.append(ans.data(), size);
        }
    }
    else if (std::holds_alternative<EloqString>(result_))
    {
        result_type = static_cast<uint8_t>(ResultType::EloqStringType);
        buf.append(reinterpret_cast<const char *>(&result_type),
                   sizeof(uint8_t));
        auto &string_ans = std::get<EloqString>(result_);
        uint32_t size = string_ans.Length();

        buf.append(reinterpret_cast<const char *>(&size), sizeof(uint32_t));
        buf.append(string_ans.StringView());
    }
    else
    {
        assert(false);
    }
}

void RedisListResult::Deserialize(const char *buf, size_t &offset)
{
    // Do not need RedisResultType, skip one byte.
    offset += sizeof(uint8_t);

    RedisCommandResult::Deserialize(buf, offset);
    ret_ = *reinterpret_cast<const int64_t *>(buf + offset);
    offset += sizeof(int64_t);
    const uint8_t result_type =
        *reinterpret_cast<const uint8_t *>(buf + offset);
    offset += sizeof(uint8_t);

    switch (static_cast<ResultType>(result_type))
    {
    case ResultType::Int64Vector:
    {
        const uint32_t cnt = *reinterpret_cast<const uint32_t *>(buf + offset);
        offset += sizeof(uint32_t);
        std::vector<int64_t> result_vector;
        result_vector.reserve(cnt);

        for (size_t i = 0; i < cnt; i++)
        {
            const int64_t ans =
                *reinterpret_cast<const int64_t *>(buf + offset);
            result_vector.emplace_back(ans);
            offset += sizeof(int64_t);
        }
        result_ = std::move(result_vector);
        break;
    }
    case ResultType::StringVector:
    {
        const uint32_t cnt = *reinterpret_cast<const uint32_t *>(buf + offset);
        offset += sizeof(uint32_t);
        std::vector<std::string> result_vector;
        result_vector.reserve(cnt);

        for (size_t i = 0; i < cnt; i++)
        {
            const uint32_t size =
                *reinterpret_cast<const uint32_t *>(buf + offset);
            offset += sizeof(uint32_t);

            result_vector.emplace_back(buf + offset, size);
            offset += size;
        }
        result_ = std::move(result_vector);
        break;
    }
    case ResultType::EloqStringType:
    {
        const uint32_t size = *reinterpret_cast<const uint32_t *>(buf + offset);
        offset += sizeof(uint32_t);

        const EloqString ans(buf + offset, size);
        offset += size;
        result_ = std::move(ans);
        break;
    }
    default:
    {
        assert(false);
    }
    }
}

void RedisSetScanResult::Serialize(std::string &buf) const
{
    uint8_t redis_result_type =
        static_cast<uint8_t>(RedisResultType::SetScanResult);
    buf.append(reinterpret_cast<const char *>(&redis_result_type),
               sizeof(uint8_t));

    RedisCommandResult::Serialize(buf);

    uint8_t result_type;

    if (std::holds_alternative<std::vector<EloqString>>(result_))
    {
        result_type = static_cast<uint8_t>(ResultType::StringVector);
        buf.append(reinterpret_cast<const char *>(&result_type),
                   sizeof(uint8_t));
        const auto &result = std::get<std::vector<EloqString>>(result_);
        uint32_t cnt = result.size();
        buf.append(reinterpret_cast<const char *>(&cnt), sizeof(uint32_t));

        for (const auto &field : result)
        {
            uint32_t field_str_len = field.Length();
            buf.append(reinterpret_cast<const char *>(&field_str_len),
                       sizeof(uint32_t));
            buf.append(field.StringView());
        }
    }
    else if (std::holds_alternative<std::vector<std::pair<EloqString, double>>>(
                 result_))
    {
        result_type = static_cast<uint8_t>(ResultType::StringDoubleVector);
        buf.append(reinterpret_cast<const char *>(&result_type),
                   sizeof(uint8_t));
        const auto &result =
            std::get<std::vector<std::pair<EloqString, double>>>(result_);
        uint32_t cnt = result.size();
        buf.append(reinterpret_cast<const char *>(&cnt), sizeof(uint32_t));

        for (const auto &[field, score] : result)
        {
            uint32_t field_str_len = field.Length();
            buf.append(reinterpret_cast<const char *>(&field_str_len),
                       sizeof(uint32_t));
            buf.append(field.StringView());

            buf.append(reinterpret_cast<const char *>(&score), sizeof(double));
        }
    }

    else
    {
        assert(false && "Dead branch");
    }
}

void RedisSetScanResult::Deserialize(const char *buf, size_t &offset)
{
    offset += sizeof(uint8_t);

    RedisCommandResult::Deserialize(buf, offset);

    uint8_t result_type = *(reinterpret_cast<const uint8_t *>(buf + offset));
    offset += sizeof(uint8_t);

    switch (static_cast<ResultType>(result_type))
    {
    case ResultType::StringVector:
    {
        const uint32_t cnt = *reinterpret_cast<const uint32_t *>(buf + offset);
        offset += sizeof(uint32_t);
        std::vector<EloqString> result_vector;
        result_vector.reserve(cnt);

        for (size_t i = 0; i < cnt; i++)
        {
            const uint32_t size =
                *reinterpret_cast<const uint32_t *>(buf + offset);
            offset += sizeof(uint32_t);
            result_vector.emplace_back(buf + offset, size);
            offset += size;
        }

        result_ = std::move(result_vector);
        break;
    }
    case ResultType::StringDoubleVector:
    {
        const uint32_t cnt = *reinterpret_cast<const uint32_t *>(buf + offset);
        offset += sizeof(uint32_t);
        std::vector<std::pair<EloqString, double>> result_vector;
        result_vector.reserve(cnt);

        for (size_t i = 0; i < cnt; i++)
        {
            const uint32_t size =
                *reinterpret_cast<const uint32_t *>(buf + offset);
            offset += sizeof(uint32_t);

            EloqString field(buf + offset, size);
            offset += size;
            double score = *reinterpret_cast<const double *>(buf + offset);
            offset += sizeof(double);

            result_vector.push_back(std::pair(std::move(field), score));
        }

        result_ = std::move(result_vector);

        break;
    }
    default:
    {
        assert(false && "Dead branch");
        break;
    }
    }
}

void RedisZsetResult::Serialize(std::string &buf) const
{
    uint8_t redis_result_type =
        static_cast<uint8_t>(RedisResultType::ZsetResult);
    buf.append(reinterpret_cast<const char *>(&redis_result_type),
               sizeof(uint8_t));

    RedisCommandResult::Serialize(buf);

    uint8_t result_type = 0;
    if (std::holds_alternative<int>(result_))
    {
        result_type = static_cast<uint8_t>(ResultType::Int);
        buf.append(reinterpret_cast<const char *>(&result_type),
                   sizeof(uint8_t));
        int32_t ans = std::get<int>(result_);
        buf.append(reinterpret_cast<const char *>(&ans), sizeof(int32_t));
    }
    else if (std::holds_alternative<double>(result_))
    {
        result_type = static_cast<uint8_t>(ResultType::Double);
        buf.append(reinterpret_cast<const char *>(&result_type),
                   sizeof(uint8_t));
        double result = std::get<double>(result_);
        buf.append(reinterpret_cast<const char *>(&result), sizeof(double));
    }
    else if (std::holds_alternative<EloqString>(result_))
    {
        result_type = static_cast<uint8_t>(ResultType::String);
        buf.append(reinterpret_cast<const char *>(&result_type),
                   sizeof(uint8_t));
        auto &string_ans = std::get<EloqString>(result_);
        uint32_t size = string_ans.Length();

        buf.append(reinterpret_cast<const char *>(&size), sizeof(uint32_t));
        buf.append(string_ans.StringView());
    }
    else if (std::holds_alternative<std::vector<EloqString>>(result_))
    {
        result_type = static_cast<uint8_t>(ResultType::StringVector);
        buf.append(reinterpret_cast<const char *>(&result_type),
                   sizeof(uint8_t));
        auto &vector_ans = std::get<std::vector<EloqString>>(result_);

        uint32_t cnt = vector_ans.size();
        buf.append(reinterpret_cast<const char *>(&cnt), sizeof(uint32_t));

        for (auto &ans : vector_ans)
        {
            uint32_t size = ans.Length();
            buf.append(reinterpret_cast<const char *>(&size), sizeof(uint32_t));
            buf.append(ans.StringView());
        }
    }
    else if (std::holds_alternative<std::pair<int32_t, std::string>>(result_))
    {
        result_type = static_cast<uint8_t>(ResultType::PairInt32Str);
        buf.append(reinterpret_cast<const char *>(&result_type),
                   sizeof(uint8_t));
        auto &pr = std::get<std::pair<int32_t, std::string>>(result_);

        buf.append(reinterpret_cast<const char *>(&pr.first), sizeof(uint32_t));
        uint32_t size = static_cast<uint32_t>(pr.second.size());
        buf.append(reinterpret_cast<const char *>(&size), sizeof(uint32_t));
        buf.append(pr.second);
    }
    else
    {
        assert(false && "Dead branch");
    }
}

void RedisZsetResult::Deserialize(const char *buf, size_t &offset)
{
    // Do not need RedisResultType, skip one byte.
    offset += sizeof(uint8_t);

    RedisCommandResult::Deserialize(buf, offset);

    uint8_t result_type = *(reinterpret_cast<const uint8_t *>(buf + offset));
    offset += sizeof(uint8_t);

    switch (static_cast<ResultType>(result_type))
    {
    case ResultType::Int:
    {
        const int32_t ans = *reinterpret_cast<const int32_t *>(buf + offset);
        offset += sizeof(int32_t);
        result_ = (int) ans;
        break;
    }
    case ResultType::Double:
    {
        const double ans = *reinterpret_cast<const double *>(buf + offset);
        offset += sizeof(double);
        result_ = ans;
        break;
    }
    case ResultType::String:
    {
        const uint32_t size = *reinterpret_cast<const uint32_t *>(buf + offset);
        offset += sizeof(uint32_t);

        const EloqString ans(buf + offset, size);
        offset += size;
        result_ = std::move(ans);
        break;
    }
    case ResultType::StringVector:
    {
        const uint32_t cnt = *reinterpret_cast<const uint32_t *>(buf + offset);
        offset += sizeof(uint32_t);
        std::vector<EloqString> result_vector;
        result_vector.reserve(cnt);

        for (size_t i = 0; i < cnt; i++)
        {
            const uint32_t size =
                *reinterpret_cast<const uint32_t *>(buf + offset);
            offset += sizeof(uint32_t);

            result_vector.emplace_back(buf + offset, size);
            offset += size;
        }
        result_ = std::move(result_vector);
        break;
    }
    case ResultType::PairInt32Str:
    {
        std::pair<int32_t, std::string> pr;
        pr.first = *reinterpret_cast<const uint32_t *>(buf + offset);
        offset += sizeof(uint32_t);
        int32_t sz = *reinterpret_cast<const uint32_t *>(buf + offset);
        offset += sizeof(uint32_t);
        pr.second = std::string(buf + offset, sz);
        result_ = std::move(pr);
        offset += sz;
        break;
    }
    default:
    {
        assert(false);
    }
    }
}

void RedisHashResult::Serialize(std::string &buf) const
{
    uint8_t redis_result_type =
        static_cast<uint8_t>(RedisResultType::HashResult);
    buf.append(reinterpret_cast<const char *>(&redis_result_type),
               sizeof(uint8_t));

    RedisCommandResult::Serialize(buf);

    uint8_t result_type;
    if (std::holds_alternative<int64_t>(result_))
    {
        result_type = static_cast<uint8_t>(ResultType::Int64_t);
        buf.append(reinterpret_cast<const char *>(&result_type),
                   sizeof(uint8_t));
        int64_t ans = std::get<int64_t>(result_);
        buf.append(reinterpret_cast<const char *>(&ans), sizeof(int64_t));
    }
    else if (std::holds_alternative<std::string>(result_))
    {
        result_type = static_cast<uint8_t>(ResultType::String);
        buf.append(reinterpret_cast<const char *>(&result_type),
                   sizeof(uint8_t));
        auto &string_ans = std::get<std::string>(result_);
        uint32_t size = string_ans.size();

        buf.append(reinterpret_cast<const char *>(&size), sizeof(uint32_t));
        buf.append(string_ans.data(), size);
    }
    else if (std::holds_alternative<std::vector<std::string>>(result_))
    {
        result_type = static_cast<uint8_t>(ResultType::StringVector);
        buf.append(reinterpret_cast<const char *>(&result_type),
                   sizeof(uint8_t));
        const auto &vector_ans = std::get<std::vector<std::string>>(result_);

        uint32_t cnt = vector_ans.size();
        buf.append(reinterpret_cast<const char *>(&cnt), sizeof(uint32_t));

        for (const auto &ans : vector_ans)
        {
            uint32_t size = ans.size();

            buf.append(reinterpret_cast<const char *>(&size), sizeof(uint32_t));
            buf.append(ans.data(), size);
        }
    }
    else if (std::holds_alternative<std::vector<std::optional<std::string>>>(
                 result_))
    {
        result_type = static_cast<uint8_t>(ResultType::NilableStringVector);
        buf.append(reinterpret_cast<const char *>(&result_type),
                   sizeof(uint8_t));
        const auto &vector_ans =
            std::get<std::vector<std::optional<std::string>>>(result_);
        uint32_t cnt = vector_ans.size();
        buf.append(reinterpret_cast<const char *>(&cnt), sizeof(uint32_t));
        for (const auto &ans : vector_ans)
        {
            if (ans.has_value())
            {
                uint32_t size = ans->size();
                assert(size < UINT32_MAX);
                buf.append(reinterpret_cast<const char *>(&size),
                           sizeof(uint32_t));
                buf.append(ans->data(), size);
            }
            else
            {
                // Nil is encoded as size UINT32_MAX, while empty-string is
                // encoded as size zero.
                uint32_t size = UINT32_MAX;
                std::string nil;
                buf.append(reinterpret_cast<const char *>(&size),
                           sizeof(uint32_t));
                buf.append(nil.data(), nil.size());
            }
        }
    }
}

void RedisHashResult::Deserialize(const char *buf, size_t &offset)
{
    // Do not need RedisResultType, skip one byte.
    offset += sizeof(uint8_t);

    RedisCommandResult::Deserialize(buf, offset);

    const uint8_t result_type =
        *reinterpret_cast<const uint8_t *>(buf + offset);
    offset += sizeof(uint8_t);

    switch (static_cast<ResultType>(result_type))
    {
    case ResultType::Int64_t:
    {
        const int64_t ans = *reinterpret_cast<const int64_t *>(buf + offset);
        offset += sizeof(int64_t);
        result_ = ans;
        break;
    }
    case ResultType::String:
    {
        const uint32_t size = *reinterpret_cast<const uint32_t *>(buf + offset);
        offset += sizeof(uint32_t);

        const std::string ans(buf + offset, size);
        offset += size;
        result_ = std::move(ans);
        break;
    }
    case ResultType::StringVector:
    {
        const uint32_t cnt = *reinterpret_cast<const uint32_t *>(buf + offset);
        offset += sizeof(uint32_t);
        std::vector<std::string> result_vector;
        result_vector.reserve(cnt);
        for (size_t i = 0; i < cnt; i++)
        {
            const uint32_t size =
                *reinterpret_cast<const uint32_t *>(buf + offset);
            offset += sizeof(uint32_t);

            result_vector.emplace_back(buf + offset, size);
            offset += size;
        }
        result_ = std::move(result_vector);
        break;
    }
    case ResultType::NilableStringVector:
    {
        const uint32_t cnt = *reinterpret_cast<const uint32_t *>(buf + offset);
        offset += sizeof(cnt);
        std::vector<std::optional<std::string>> result_vector;
        result_vector.reserve(cnt);
        for (size_t i = 0; i < cnt; i++)
        {
            const uint32_t size =
                *reinterpret_cast<const uint32_t *>(buf + offset);
            offset += sizeof(uint32_t);

            if (size != UINT32_MAX)
            {
                result_vector.emplace_back(
                    std::make_optional<std::string>(buf + offset, size));
                offset += size;
            }
            else
            {
                result_vector.emplace_back(std::nullopt);
                offset += 0;
            }
        }
        result_ = std::move(result_vector);
        break;
    }
    default:
        assert(false);
        break;
    }
}

void RedisHashSetResult::Serialize(std::string &buf) const
{
    uint8_t redis_result_type =
        static_cast<uint8_t>(RedisResultType::HashSetResult);
    buf.append(reinterpret_cast<const char *>(&redis_result_type),
               sizeof(uint8_t));

    // Serialize "RedisCommandResult::err_code_"
    RedisCommandResult::Serialize(buf);

    auto *ret_ptr = reinterpret_cast<const char *>(&ret_);
    buf.append(ret_ptr, sizeof(int32_t));

    uint32_t cnt = string_list_.size();
    buf.append(reinterpret_cast<const char *>(&cnt), sizeof(uint32_t));

    for (const auto &str : string_list_)
    {
        uint32_t str_size = str.Length();
        buf.append(reinterpret_cast<const char *>(&str_size), sizeof(uint32_t));
        buf.append(str.StringView());
    }
}

void RedisHashSetResult::Deserialize(const char *buf, size_t &offset)
{
    // Do not need RedisResultType, skip one byte.
    offset += sizeof(uint8_t);

    // Deserialize "RedisCommandResult::err_code_"
    RedisCommandResult::Deserialize(buf, offset);

    ret_ = *reinterpret_cast<const int32_t *>(buf + offset);
    offset += sizeof(int32_t);

    uint32_t cnt = *reinterpret_cast<const uint32_t *>(buf + offset);
    offset += sizeof(uint32_t);

    string_list_.reserve(cnt);
    for (uint32_t i = 0; i < cnt; i++)
    {
        uint32_t size = *reinterpret_cast<const uint32_t *>(buf + offset);
        offset += sizeof(uint32_t);
        string_list_.emplace_back(buf + offset, size);
        offset += size;
    }
}

void RedisSortableLoadResult::Serialize(std::string &buf) const
{
    uint8_t redis_result_type =
        static_cast<uint8_t>(RedisResultType::SortableLoadResult);
    buf.append(reinterpret_cast<const char *>(&redis_result_type),
               sizeof(uint8_t));

    // Serialize "RedisCommandResult::err_code_"
    RedisCommandResult::Serialize(buf);

    uint8_t obj_type = static_cast<uint8_t>(obj_type_);
    buf.append(reinterpret_cast<const char *>(&obj_type), sizeof(uint8_t));

    uint32_t cnt = elems_.size();
    buf.append(reinterpret_cast<const char *>(&cnt), sizeof(uint32_t));

    for (const std::string &elem : elems_)
    {
        uint32_t size = elem.size();
        buf.append(reinterpret_cast<const char *>(&size), sizeof(uint32_t));
        buf.append(elem.data(), size);
    }
}

void RedisSortableLoadResult::Deserialize(const char *buf, size_t &offset)
{
    offset += sizeof(uint8_t);

    RedisCommandResult::Deserialize(buf, offset);

    uint8_t obj_type = *reinterpret_cast<const uint8_t *>(buf + offset);
    obj_type_ = static_cast<RedisObjectType>(obj_type);
    offset += sizeof(uint8_t);

    uint32_t cnt = *reinterpret_cast<const uint32_t *>(buf + offset);
    offset += sizeof(uint32_t);
    elems_.reserve(cnt);
    assert(elems_.empty());

    for (uint32_t i = 0; i < cnt; i++)
    {
        uint32_t size = *reinterpret_cast<const uint32_t *>(buf + offset);
        offset += sizeof(uint32_t);
        elems_.emplace_back(buf + offset, size);
        offset += size;
    }
}

void RedisSortResult::Serialize(std::string &buf) const
{
    uint8_t redis_result_type =
        static_cast<uint8_t>(RedisResultType::SortResult);
    buf.append(reinterpret_cast<const char *>(&redis_result_type),
               sizeof(uint8_t));

    RedisCommandResult::Serialize(buf);

    uint32_t cnt = result_.size();
    buf.append(reinterpret_cast<const char *>(&cnt), sizeof(uint32_t));

    for (const std::optional<std::string> &optional_string : result_)
    {
        if (optional_string.has_value())
        {
            uint32_t size = optional_string->size();
            assert(size < UINT32_MAX);
            buf.append(reinterpret_cast<const char *>(&size), sizeof(uint32_t));
            buf.append(optional_string->data(), size);
        }
        else
        {
            uint32_t size = UINT32_MAX;
            std::string nil;
            buf.append(reinterpret_cast<const char *>(&size), sizeof(uint32_t));
            buf.append(nil.data(), nil.size());
        }
    }
}

void RedisSortResult::Deserialize(const char *buf, size_t &offset)
{
    // Do not need RedisResultType, skip one byte.
    offset += sizeof(uint8_t);

    RedisCommandResult::Deserialize(buf, offset);

    const uint32_t cnt = *reinterpret_cast<const uint32_t *>(buf + offset);
    offset += sizeof(cnt);

    std::vector<std::optional<std::string>> result_vector;
    result_vector.reserve(cnt);
    for (size_t i = 0; i < cnt; i++)
    {
        const uint32_t size = *reinterpret_cast<const uint32_t *>(buf + offset);
        offset += sizeof(size);

        if (size != UINT32_MAX)
        {
            result_vector.emplace_back(
                std::make_optional<std::string>(buf + offset, size));
            offset += size;
        }
        else
        {
            result_vector.emplace_back(std::nullopt);
            offset += 0;
        }
    }
    result_ = std::move(result_vector);
}

std::unique_ptr<TxCommand> RedisCommand::RetireExpiredTTLObjectCommand() const
{
    return std::make_unique<DelCommand>();
}

std::unique_ptr<txservice::TxRecord> StringCommand::CreateObject(
    const std::string *image) const
{
    auto tmp_obj = std::make_unique<RedisStringObject>();
    if (image != nullptr)
    {
        size_t offset = 0;
        tmp_obj->Deserialize(image->data(), offset);
        assert(offset == image->size());
    }
    return tmp_obj;
}

bool StringCommand::CheckTypeMatch(const txservice::TxRecord &obj)
{
    const auto &redis_obj = static_cast<const RedisEloqObject &>(obj);
    return redis_obj.ObjectType() == RedisObjectType::String;
}

SetCommand::SetCommand(const EloqKV::SetCommand &rhs)
    : StringCommand(rhs),
      value_(rhs.value_.Clone()),
      flag_(rhs.flag_),
      obj_expire_ts_(rhs.obj_expire_ts_)
{
}

txservice::ExecResult SetCommand::ExecuteOn(const txservice::TxObject &object)
{
    // Set command overrides the object regardless of the object's type
    RedisStringResult &str_result = result_;
    if (flag_ & OBJ_GET_SET)
    {
        if (!CheckTypeMatch(object))
        {
            str_result.err_code_ = RD_ERR_WRONG_TYPE;
            return txservice::ExecResult::Fail;
        }

        const RedisStringObject &str_obj =
            static_cast<const RedisStringObject &>(object);
        std::string_view sv = str_obj.StringView();

        if (sv.data() == nullptr)
        {
            str_result.err_code_ = RD_NIL;
            return flag_ & OBJ_SET_XX ? txservice::ExecResult::Read
                                      : txservice::ExecResult::Write;
        }
        else
        {
            str_result.str_ = sv;
            str_result.err_code_ = RD_OK;
            if (flag_ & OBJ_SET_NX)
            {
                return txservice::ExecResult::Read;
            }

            if (!str_obj.CheckSerializedLength(value_.Length()))
            {
                str_result.err_code_ = RD_ERR_OBJECT_TOO_BIG;
                return txservice::ExecResult::Read;
            }
            return txservice::ExecResult::Write;
        }
    }
    else
    {
        const RedisStringObject &str_obj =
            static_cast<const RedisStringObject &>(object);
        if (!str_obj.CheckSerializedLength(value_.Length()))
        {
            str_result.err_code_ = RD_ERR_OBJECT_TOO_BIG;
            return txservice::ExecResult::Fail;
        }
        else
        {
            str_result.err_code_ = RD_OK;
            result_.int_ret_ = 1;
        }
        return txservice::ExecResult::Write;
    }
}

txservice::TxObject *SetCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    auto *str_obj_ptr = dynamic_cast<RedisStringObject *>(obj_ptr);
    if (str_obj_ptr != nullptr)
    {
        txservice::TxObject *obj = obj_ptr;
        // the object is string type
        str_obj_ptr->CommitSet(value_);
        // this set command carrying ttl information
        if (obj_expire_ts_ != UINT64_MAX)
        {
            // str_obj_ptr is already ttl_str_obj
            if (str_obj_ptr->HasTTL())
            {
                str_obj_ptr->SetTTL(obj_expire_ts_);
            }
            // otherwise convert to ttl_str_obj
            else
            {
                obj = static_cast<txservice::TxObject *>(
                    str_obj_ptr->AddTTL(obj_expire_ts_).release());
            }
        }
        // if the str obj has ttl but this string set command has no ttl, then
        // we need to remove ttl
        else if (obj_expire_ts_ == UINT64_MAX && str_obj_ptr->HasTTL())
        {
            // if keep ttl is not given
            if (!(flag_ & OBJ_SET_KEEPTTL))
            {
                auto *str_ttl_obj_ptr =
                    static_cast<RedisStringTTLObject *>(obj_ptr);
                obj = static_cast<txservice::TxObject *>(
                    str_ttl_obj_ptr->RemoveTTL().release());
            }
        }
        return obj;
    }
    else
    {
        // the object is not string type, create a new string object
        std::unique_ptr<txservice::TxRecord> new_obj_uptr = nullptr;
        // this set command carrying ttl information
        if (obj_expire_ts_ != UINT64_MAX)
        {
            new_obj_uptr = std::make_unique<RedisStringTTLObject>();
            auto &str_obj = static_cast<RedisStringTTLObject &>(*new_obj_uptr);
            str_obj.CommitSet(value_);
            new_obj_uptr->SetTTL(obj_expire_ts_);
        }
        else
        {
            new_obj_uptr = StringCommand::CreateObject(nullptr);
            auto &str_obj = static_cast<RedisStringObject &>(*new_obj_uptr);
            str_obj.CommitSet(value_);
        }
        auto &str_obj = static_cast<RedisStringObject &>(*new_obj_uptr);
        str_obj.CommitSet(value_);
        return static_cast<txservice::TxObject *>(new_obj_uptr.release());
    }
}

void SetCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::SET);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));

    auto *flag_ptr = reinterpret_cast<const char *>(&flag_);
    str.append(flag_ptr, sizeof(int32_t));
    std::string_view value_view = value_.StringView();
    uint32_t len = value_view.size();
    str.append(reinterpret_cast<const char *>(&len), sizeof(uint32_t));
    str.append(value_view.data(), len);
    str.append(reinterpret_cast<const char *>(&obj_expire_ts_),
               sizeof(uint64_t));
}

void SetCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    flag_ = *reinterpret_cast<const int32_t *>(ptr);
    ptr += sizeof(int32_t);
    const uint32_t str_len = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);
    value_ = EloqString(ptr, str_len);
    ptr += str_len;
    obj_expire_ts_ = *reinterpret_cast<const uint64_t *>(ptr);
}

void DirectRequest::Execute(RedisServiceImpl *redis_impl)
{
    cmd_->Execute(redis_impl, ctx_);
}

void DirectRequest::OutputResult(OutputHandler *reply) const
{
    cmd_->OutputResult(reply);
}

bool CustomCommandRequest::Execute(RedisServiceImpl *redis_impl,
                                   RedisConnectionContext *ctx,
                                   txservice::TransactionExecution *txm,
                                   OutputHandler *output,
                                   bool auto_commit)
{
    return cmd_->Execute(redis_impl, ctx, table_, txm, output, auto_commit);
}

void EchoCommand::Execute(RedisServiceImpl *redis_impl,
                          RedisConnectionContext *ctx)
{
}

void PingCommand::Execute(RedisServiceImpl *redis_impl,
                          RedisConnectionContext *ctx)
{
}

void AuthCommand::Execute(RedisServiceImpl *redis_impl,
                          RedisConnectionContext *ctx)
{
    if (password_ == requirepass)
    {
        result_.err_code_ = RD_OK;
        ctx->authenticated = true;
    }
    else
    {
        result_.err_code_ = RD_ERR_WRONG_PASS;
    }
}

void AuthCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.err_code_ == RD_OK)
    {
        reply->OnStatus(redis_get_error_messages(RD_OK));
    }
    else
    {
        assert(result_.err_code_ == RD_ERR_WRONG_PASS);
        reply->OnError(redis_get_error_messages(RD_ERR_WRONG_PASS));
    }
}

void SelectCommand::Execute(RedisServiceImpl *redis_impl,
                            RedisConnectionContext *ctx)
{
    if (db_id_ >= 0 && db_id_ < databases)
    {
        result_.err_code_ = RD_OK;
        ctx->db_id = db_id_;
    }
    else
    {
        result_.err_code_ = RD_ERR_SELECT_OUT_OF_RANGE;
    }
}

void SelectCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.err_code_ == RD_OK)
    {
        reply->OnStatus(redis_get_error_messages(RD_OK));
    }
    else
    {
        assert(result_.err_code_ == RD_ERR_SELECT_OUT_OF_RANGE);
        reply->OnError(redis_get_error_messages(RD_ERR_SELECT_OUT_OF_RANGE));
    }
}

void InfoCommand::Execute(RedisServiceImpl *redis_impl,
                          RedisConnectionContext *ctx)
{
    config_file_ = redis_impl->GetConfigFile();
    uptime_in_secs_ = butil::cpuwide_time_s() - redis_impl->GetStartSecond();
    node_id_ = Sharder::Instance().NodeId();
    node_count_ = Sharder::Instance().GetNodeCount();
    tcp_port_ = redis_impl->GetRedisPort();
    core_num_ = redis_impl->GetCoreNum();
    enable_data_store_ = redis_impl->GetEnableDataStore();
    enable_wal_ = redis_impl->GetEnableWal();
    node_memory_limit_mb_ = redis_impl->GetNodeMemoryLimitMB();
    node_log_limit_mb_ = redis_impl->GetNodeLogLimitMB();

    if (set_section_.size() == 0 ||
        set_section_.find("memory") != set_section_.end())
    {
        auto &local_shards = redis_impl->GetTxService()->CcShards();
        size_t cnt = local_shards.Count();
        CkptTsCc ckpt_req(cnt, node_id_);
        for (size_t i = 0; i < cnt; i++)
        {
            local_shards.EnqueueCcRequest(i, &ckpt_req);
        }
        ckpt_req.Wait();
        data_memory_allocated_ = ckpt_req.GetMemUsage();
        data_memory_committed_ = ckpt_req.GetMemCommited();
        last_ckpt_ts_ = ckpt_req.GetCkptTs();
    }
    else
    {
        data_memory_allocated_ = -1;
        data_memory_committed_ = -1;
        last_ckpt_ts_ = -1;
    }

    event_dispatcher_num_ = redis_impl->GetEventDispatcherNum();
    version_ = redis_impl->GetVersion();
    max_connection_count_ = redis_impl->MaxConnectionCount();

    if (redis_impl->IsEnableRedisStats())
    {
        conn_received_count_ = RedisStats::GetConnReceivedCount();
        conn_rejected_count_ = RedisStats::GetConnRejectedCount();
        connecting_count_ = RedisStats::GetConnectingCount();
        blocked_clients_count_ = RedisStats::GetBlockedClientsCount();

        cmd_read_count_ = RedisStats::GetReadCommandsCount();
        cmd_write_count_ = RedisStats::GetWriteCommandsCount();
        multi_cmd_count_ = RedisStats::GetMultiObjectCommandsCount();
        //  cmds_per_sec_ = RedisStats::GetCommandsPerSecond();
    }

    if (set_section_.size() == 0 ||
        set_section_.find("keyspace") != set_section_.end())
    {
        std::vector<TableName> table_names;
        size_t total_redis_table_cnt = redis_impl->GetRedisTableCount();
        for (size_t db_id = 0; db_id < total_redis_table_cnt; ++db_id)
        {
            const TableName *tbn = redis_impl->RedisTableName(db_id);
            table_names.emplace_back(
                tbn->StringView(), tbn->Type(), TableEngine::EloqKv);
        }

        dbsizes_ = DBSizeCommand::FetchDBSize(std::move(table_names));
    }
}

void ClusterCommand::Execute(RedisServiceImpl *redis_impl,
                             RedisConnectionContext *ctx)
{
}

void CommandCommand::Execute(RedisServiceImpl *redis_impl,
                             RedisConnectionContext *ctx)
{
}

void CommandInfoCommand::Execute(RedisServiceImpl *redis_impl,
                                 RedisConnectionContext *ctx)
{
}

void CommandCountCommand::Execute(RedisServiceImpl *redis_impl,
                                  RedisConnectionContext *ctx)
{
}

void CommandListCommand::Execute(RedisServiceImpl *redis_impl,
                                 RedisConnectionContext *ctx)
{
}

void ClusterInfoCommand::Execute(RedisServiceImpl *redis_impl,
                                 RedisConnectionContext *ctx)
{
    // TODO(lzx): return the real information of our system instead of Redis.
    std::string cluster_state = "ok";
    uint32_t cluster_size = redis_impl->RedisClusterSize();
    uint32_t nodes_count = redis_impl->RedisClusterNodesCount();

    result_.push_back("cluster_state:" + cluster_state);
    result_.push_back("cluster_slots_assigned:16384");
    result_.push_back("cluster_slots_ok:16384");
    result_.push_back("cluster_slots_pfail:0");
    result_.push_back("cluster_slots_fail:0");
    result_.push_back("cluster_known_nodes:" + std::to_string(nodes_count));
    result_.push_back("cluster_size:" + std::to_string(cluster_size));
    result_.push_back("cluster_current_epoch:1");
    result_.push_back("cluster_my_epoch:1");
    result_.push_back("cluster_stats_messages_ping_sent:0");
    result_.push_back("cluster_stats_messages_pong_sent:0");
    result_.push_back("cluster_stats_messages_meet_sent:0");
    result_.push_back("cluster_stats_messages_sent:0");
    result_.push_back("cluster_stats_messages_ping_received:0");
    result_.push_back("cluster_stats_messages_pong_received:0");
    result_.push_back("cluster_stats_messages_received:0");
    result_.push_back("total_cluster_links_buffer_limit_exceeded:0");
}

void ClusterNodesCommand::Execute(RedisServiceImpl *redis_impl,
                                  RedisConnectionContext *ctx)
{
    // space-separated CSV string
    // Each line fields:
    //    <id> <ip:port@cport[,hostname]> <flags> <master> <ping-sent>
    //    <pong-recv> <config-epoch> <link-state> <slot> <slot> ... <slot>
    result_.clear();
    redis_impl->RedisClusterNodes(result_);
}

void ClusterSlotsCommand::Execute(RedisServiceImpl *redis_impl,
                                  RedisConnectionContext *ctx)
{
    // space-separated CSV string
    // Each line fields:
    //    <id> <ip:port@cport[,hostname]> <flags> <master> <ping-sent>
    //    <pong-recv> <config-epoch> <link-state> <slot> <slot> ... <slot>
    result_.clear();
    redis_impl->RedisClusterSlots(result_);
}

void ClusterKeySlotCommand::Execute(RedisServiceImpl *redis_impl,
                                    RedisConnectionContext *ctx)
{
    int32_t slot_id = key_.Hash() & 0x3fff;
    result_ = slot_id % total_range_buckets;
}

void FailoverCommand::Execute(RedisServiceImpl *redis_impl,
                              RedisConnectionContext *ctx)
{
    failover_succeed_ = txservice::Sharder::Instance().Failover(
        target_host_, target_port_, error_message_);
}

void ClientCommand::Execute(RedisServiceImpl *redis_impl,
                            RedisConnectionContext *ctx)
{
}

ClientCommand::ClientInfo::ClientInfo(const brpc::Socket *socket,
                                      const RedisConnectionContext *ctx)
    : id_(static_cast<int64_t>(socket->id())),
      addr_(socket->remote_side()),
      laddr_(socket->local_side()),
      fd_(socket->fd()),
      name_(ctx->connection_name),
      db_(ctx->db_id),
      sub_(ctx->SubscriptionsCount()),
      psub_(ctx->subscribed_patterns.size()),
      lib_name_(ctx->lib_name),
      lib_ver_(ctx->lib_ver)
{
    int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
    age_ = std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::microseconds(
                   now > ctx->connect_time_us ? now - ctx->connect_time_us : 0))
               .count();
    idle_ =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::microseconds(now > socket->last_active_time_us()
                                          ? now - socket->last_active_time_us()
                                          : 0))
            .count();

    if (ctx->in_multi_transaction)
    {
        multi_ = ctx->multi_transaction_handler->PendingRequestNum();
    }

    // The following are dummy fields and users should not rely on those.
    // redis-py needs to access some of the missing fields when parsing the
    // client info result.
    qbuf_ = 0;
    qbuf_free_ = 0;
    obl_ = 0;
    argv_mem_ = 0;
    oll_ = 0;
    omem_ = 0;
    tot_mem_ = 0;
}

std::string ClientCommand::ClientInfo::ToString() const
{
    std::stringstream ss;
    ss << *this;
    return ss.str();
}

std::ostream &operator<<(std::ostream &out,
                         const ClientCommand::ClientInfo &client)
{
    out << "id=" << client.id_;
    out << " addr=" << client.addr_;
    out << " laddr=" << client.laddr_;
    out << " fd=" << client.fd_;
    out << " name=" << client.name_;
    out << " age=" << client.age_;
    out << " idle=" << client.idle_;
    out << " db=" << client.db_;
    out << " sub=" << client.sub_;
    out << " psub=" << client.psub_;
    out << " multi=" << client.multi_;
    out << " qbuf=" << client.qbuf_;
    out << " qbuf-free=" << client.qbuf_free_;
    out << " argv-mem=" << client.argv_mem_;
    out << " obl=" << client.obl_;
    out << " oll=" << client.oll_;
    out << " omem=" << client.omem_;
    out << " tot-mem=" << client.tot_mem_;
    out << " user=" << client.user_;
    out << " lib-name=" << client.lib_name_;
    out << " lib-ver=" << client.lib_ver_;
    return out;
}

void ClientSetNameCommand::Execute(RedisServiceImpl *redis_impl,
                                   RedisConnectionContext *ctx)
{
    ctx->connection_name = connection_name_;
}

void ClientSetNameCommand::OutputResult(OutputHandler *reply) const
{
    reply->OnStatus(redis_get_error_messages(RD_OK));
}

void ClientGetNameCommand::Execute(RedisServiceImpl *redis_impl,
                                   RedisConnectionContext *ctx)
{
    connection_name_ = ctx->connection_name;
}

void ClientGetNameCommand::OutputResult(OutputHandler *reply) const
{
    if (connection_name_.empty())
    {
        reply->OnNil();
    }
    else
    {
        reply->OnString(connection_name_);
    }
}

void ClientSetInfoCommand::Execute(RedisServiceImpl *redis_impl,
                                   RedisConnectionContext *ctx)
{
    switch (attribute_)
    {
    case ClientSetInfoCommand::Attribute::LIB_NAME:
        ctx->lib_name = value_;
        break;
    case ClientSetInfoCommand::Attribute::LIB_VER:
        ctx->lib_ver = value_;
        break;
    default:
        assert(false);
        break;
    }
}

void ClientSetInfoCommand::OutputResult(OutputHandler *reply) const
{
    reply->OnStatus(redis_get_error_messages(RD_OK));
}

void ClientIdCommand::Execute(RedisServiceImpl *redis_impl,
                              RedisConnectionContext *ctx)
{
    client_id_ = static_cast<int64_t>(ctx->socket->id());
}

void ClientIdCommand::OutputResult(OutputHandler *reply) const
{
    reply->OnInt(client_id_);
}

void ClientInfoCommand::Execute(RedisServiceImpl *redis_impl,
                                RedisConnectionContext *ctx)
{
    client_.emplace(ctx->socket, ctx);
}

void ClientInfoCommand::OutputResult(OutputHandler *reply) const
{
    std::string str = client_->ToString();
    str.push_back('\n');
    reply->OnString(str);
}

void ClientListCommand::Execute(RedisServiceImpl *redis_impl,
                                RedisConnectionContext *ctx)
{
    if (type_.has_value())
    {
        // Unsupport client type.
    }
    else if (client_id_vec_.has_value())
    {
        for (int64_t client_id : client_id_vec_.value())
        {
            brpc::SocketId socket_id = static_cast<brpc::SocketId>(client_id);
            brpc::SocketUniquePtr socket_ptr;
            if (brpc::Socket::Address(socket_id, &socket_ptr) == 0)
            {
                socket_ptr->IsAvailable();
                client_info_vec_.emplace_back(socket_ptr.get(), ctx);
            }
        }
    }
    else
    {
        std::vector<brpc::SocketId> conn_list;
        server_acceptor->ListConnections(&conn_list);
        client_info_vec_.reserve(conn_list.size());
        for (brpc::SocketId socket_id : conn_list)
        {
            brpc::SocketUniquePtr socket_ptr;
            if (brpc::Socket::Address(socket_id, &socket_ptr) == 0)
            {
                client_info_vec_.emplace_back(socket_ptr.get(), ctx);
            }
        }
    }
}

void ClientListCommand::OutputResult(OutputHandler *reply) const
{
    std::stringstream ss;
    for (const ClientInfo &client : client_info_vec_)
    {
        ss << client << std::endl;
    }
    reply->OnString(ss.str());
}

bool ClientListCommand::GetTypeByName(const char *name, Type *type)
{
    if (!strcasecmp(name, "normal"))
    {
        *type = Type::NORMAL;
        return true;
    }
    else if (!strcasecmp(name, "slave") || !strcasecmp(name, "replica"))
    {
        *type = Type::SLAVE;
        return true;
    }
    else if (!strcasecmp(name, "pubsub"))
    {
        *type = Type::PUBSUB;
        return true;
    }
    else if (!strcasecmp(name, "master"))
    {
        *type = Type::MASTER;
        return true;
    }
    else
    {
        return false;
    }
}

void ClientKillCommand::Execute(RedisServiceImpl *redis_impl,
                                RedisConnectionContext *ctx)
{
    std::vector<brpc::SocketId> conn_list;
    server_acceptor->ListConnections(&conn_list);
    for (brpc::SocketId socket_id : conn_list)
    {
        brpc::SocketUniquePtr socket_ptr;
        if (brpc::Socket::Address(socket_id, &socket_ptr) == 0)
        {
            butil::EndPoint remote = socket_ptr->remote_side();
            if (addr_.has_value() && addr_.value() == remote)
            {
                socket_ptr->ReleaseAdditionalReference();
                killed_ += 1;
            }
        }
    }
}

void ClientKillCommand::OutputResult(OutputHandler *reply) const
{
    if (old_style_syntax_)
    {
        if (killed_ >= 1)
        {
            reply->OnStatus(redis_get_error_messages(RD_OK));
        }
        else
        {
            reply->OnError("ERR No such client");
        }
    }
    else
    {
        reply->OnInt(killed_);
    }
}

void DBSizeCommand::Execute(RedisServiceImpl *redis_impl,
                            RedisConnectionContext *ctx)
{
    std::vector<TableName> table_names;
    const TableName *tbn = redis_impl->RedisTableName(ctx->db_id);
    table_names.emplace_back(
        tbn->StringView(), tbn->Type(), TableEngine::EloqKv);
    auto result = FetchDBSize(std::move(table_names));
    total_db_size_ = result[0];
}

std::vector<int64_t> DBSizeCommand::FetchDBSize(
    std::vector<TableName> table_names)
{
    DbSizeCc *dbcc = dbsize_pool_.NextRequest();

    auto all_node_groups = txservice::Sharder::Instance().AllNodeGroups();

    uint32_t node_id = Sharder::Instance().NodeId();
    bool has_local = false;

    std::unordered_map<NodeGroupId, NodeId> leader_node_ids;
    size_t remote_ref_cnt = 0;
    for (uint32_t ng_id : *all_node_groups)
    {
        NodeId leader_node_id = Sharder::Instance().LeaderNodeId(ng_id);
        leader_node_ids.emplace(ng_id, leader_node_id);
        if (leader_node_id == node_id)
        {
            has_local = true;
        }
        else
        {
            remote_ref_cnt++;
        }
    }

    assert(leader_node_ids.size() == all_node_groups->size());

    if (has_local)
    {
        dbcc->Reset(&table_names,
                    Sharder::Instance().GetLocalCcShardsCount(),
                    remote_ref_cnt);
    }
    else
    {
        dbcc->Reset(&table_names, 0, remote_ref_cnt);
    }

    for (size_t ng_id : *all_node_groups)
    {
        NodeId n_id = leader_node_ids.at(ng_id);
        if (n_id == node_id)
        {
            dbcc->AddLocalNodeGroupId(ng_id);
        }
        else
        {
            txservice::remote::CcMessage send_msg;

            send_msg.set_type(txservice::remote::CcMessage::MessageType::
                                  CcMessage_MessageType_DBSizeRequest);
            send_msg.set_handler_addr(reinterpret_cast<uint64_t>(dbcc));

            txservice::remote::DBSizeRequest *req =
                send_msg.mutable_dbsize_req();
            req->set_src_node_id(node_id);
            for (const auto &tbn : table_names)
            {
                req->add_table_name_str(tbn.String());
                req->add_table_type(
                    txservice::remote::ToRemoteType::ConvertTableType(
                        tbn.Type()));
                req->add_table_engine(
                    txservice::remote::ToRemoteType::ConvertTableEngine(
                        TableEngine::EloqKv));
            }

            req->set_node_group_id(ng_id);
            req->set_dbsize_term(dbcc->GetTerm());
            Sharder::Instance().GetCcStreamSender()->SendMessageToNg(
                ng_id, send_msg, nullptr);
        }
    }

    if (has_local)
    {
        LocalCcShards *local_shards = Sharder::Instance().GetLocalCcShards();
        size_t cnt = local_shards->Count();
        for (size_t i = 0; i < cnt; i++)
        {
            local_shards->EnqueueCcRequest(i, dbcc);
        }
    }

    dbcc->Wait();

    auto total_db_size = dbcc->GetTotalObjSizes();
    dbcc->IncTerm();
    dbcc->Free();

    return total_db_size;
}

void ConfigCommand::Execute(RedisServiceImpl *redis_impl,
                            RedisConnectionContext *ctx)
{
    if (flag_ & CONFIG_GET)
    {
        redis_impl->ExecuteGetConfig(this);
    }
    else if (flag_ & CONFIG_SET)
    {
        redis_impl->ExecuteSetConfig(this);
    }
    else
    {
        assert(false);
    }
}

void EchoCommand::OutputResult(OutputHandler *reply) const
{
    reply->OnString(value_.StringView());
}

void PingCommand::OutputResult(OutputHandler *reply) const
{
    if (empty_)
    {
        reply->OnStatus("PONG");
        return;
    }
    reply->OnString(reply_value_.StringView());
}

std::string ExecCommand(const std::string &cmd)
{
    char line[1024];
    FILE *fp;
    const char *sysCommand = cmd.data();
    if ((fp = popen(sysCommand, "r")) == NULL)
    {
        return "Failed to execute command '" + cmd + "'!";
    }

    std::string rst;
    while (fgets(line, sizeof(line) - 1, fp) != NULL)
    {
        if (rst.size() > 0 && *rst.rbegin() != '\n')
        {
            rst += "\n";
        }

        rst += line;
    }
    pclose(fp);

    if (*rst.rbegin() == '\n')
    {
        return rst.substr(0, rst.size() - 1);
    }
    else
    {
        return rst;
    }
}

void InfoCommand::OutputResult(OutputHandler *reply) const
{
    std::string result;
    std::string pid = std::to_string(getpid());

    if (set_section_.size() == 0 ||
        set_section_.find("server") != set_section_.end())
    {
        result += "# Server";
        result += "\r\neloqkv_version:";
        result += version_;
        result += "\r\neloqkv_mode:";
        result += (node_count_ == 1 ? "standalone" : "cluster");
        result += "\r\nmultiplexing_api:epoll";
        result += "\r\nos:" + ExecCommand("uname -srm");
        result += "\r\nprocess_id:" + pid;
        result += "\r\ntcp_port:" + std::to_string(tcp_port_);
        result += "\r\nuptime_in_seconds:" + std::to_string(uptime_in_secs_);
        result += "\r\nuptime_in_days:" +
                  std::to_string(uptime_in_secs_ / (3600 * 24));
        result += "\r\nexecutable:" + ExecCommand("ls -l /proc/" + pid +
                                                  "/exe | awk '{print $11}'");
        result += "\r\nconfig_file:" + config_file_;
        result += "\r\nbrpc_io_threads_active:" +
                  std::to_string(event_dispatcher_num_);
        result += "\r\nworker_thread_num:" + std::to_string(core_num_);
        result += "\r\nnode_id:" + std::to_string(node_id_);
        result += "\r\nnode_count:" + std::to_string(node_count_);
    }

    if (set_section_.size() == 0 ||
        set_section_.find("clients") != set_section_.end())
    {
        if (result.size() > 0)
        {
            result += "\r\n";
        }

        result += "# Clients";
        result += "\r\nmaxclients:" + std::to_string(max_connection_count_);
        result += "\r\nconnected_clients:" + std::to_string(connecting_count_);
        result +=
            "\r\nblocked_clients:" + std::to_string(blocked_clients_count_);
    }

    if (set_section_.size() == 0 ||
        set_section_.find("memory") != set_section_.end())
    {
        if (result.size() > 0)
        {
            result += "\r\n";
        }

        result += "# Memory";
        result += "\r\nused_memory_rss:" +
                  ExecCommand("cat /proc/" + pid +
                              "/status | grep VmRSS | awk '{print $2}'") +
                  " kb";
        result += "\r\ntotal_system_memory:" +
                  ExecCommand("free  | grep Mem | awk '{print $2}'") + " kb";
        result += "\r\ndata_memory_allocated:" +
                  std::to_string(data_memory_allocated_) + " kb";
        result += "\r\ndata_memory_committed:" +
                  std::to_string(data_memory_committed_) + " kb";
        // result += "data_memory_frag_ratio: " + ;  // TODO
        result +=
            "\r\nnode_memory_limit:" + std::to_string(node_memory_limit_mb_) +
            " mb";
        result +=
            "\r\nnode_log_limit:" + std::to_string(node_log_limit_mb_) + " mb ";
    }

    if (set_section_.size() == 0 ||
        set_section_.find("persistence") != set_section_.end())
    {
        if (result.size() > 0)
        {
            result += "\r\n";
        }

        result += "# Persistence";

        result += "\r\nlast_success_ckpt_ts:" + std::to_string(last_ckpt_ts_);
        result += "\r\nenable_data_store:" + enable_data_store_;
        result += "\r\nenable_wal:" + enable_wal_;
    }

    if (set_section_.size() == 0 ||
        set_section_.find("stats") != set_section_.end())
    {
        if (result.size() > 0)
        {
            result += "\r\n";
        }
        result += "# Stats";
        result += "\r\ntotal_connections_received:" +
                  std::to_string(conn_received_count_);
        result += "\r\ntotal_rejected_connections:" +
                  std::to_string(conn_rejected_count_);

        result += "\r\ntotal_commands_processed:" +
                  std::to_string(cmd_read_count_ + cmd_write_count_ +
                                 multi_cmd_count_);
        // result +=
        //     "\r\nread_commands_processed:" +
        //     std::to_string(cmd_read_count_);
        // result +=
        //     "\r\nwrite_commands_processed:" +
        //     std::to_string(cmd_write_count_);
        // result += "\r\nmulti_object_commands_processed:" +
        //           std::to_string(multi_cmd_count_);
        // result +=
        //    "\r\ninstantaneous_ops_per_sec:" + std::to_string(cmds_per_sec_);
    }

    if (set_section_.size() == 0 ||
        set_section_.find("cpu") != set_section_.end())
    {
        if (result.size() > 0)
        {
            result += "\r\n";
        }
        result += "# CPU";
        struct tms st_tms;
        if (times(&st_tms) != -1)
        {
            auto clocks_per_sec = sysconf(_SC_CLK_TCK);
            double ts =
                static_cast<double>(st_tms.tms_stime) / (clocks_per_sec);
            result += "\r\nused_cpu_sys:" + std::to_string(ts);
            ts = static_cast<double>(st_tms.tms_utime) / (clocks_per_sec);
            result += "\r\nused_cpu_user:" + std::to_string(ts);
        }
        else
        {
            result += "\r\nused_cpu_sys:-1";
            result += "\r\nused_cpu_user:-1";
        }
    }

    if (set_section_.size() == 0 ||
        set_section_.find("cluster") != set_section_.end())
    {
        if (result.size() > 0)
        {
            result += "\r\n";
        }

        result += "# Cluster";
        result += "\r\ncluster_enabled:";
        result += (node_count_ > 1 ? "1" : "0");
        if (node_count_ > 1)
        {
            result += "\r\ncluster_known_nodes:" + std::to_string(node_count_);
        }
    }

    if (set_section_.size() == 0 ||
        set_section_.find("keyspace") != set_section_.end())
    {
        if (result.size() > 0)
        {
            result += "\r\n";
        }

        result += "# Keyspace";
        for (size_t idx = 0; idx < dbsizes_.size(); ++idx)
        {
            if (dbsizes_[idx] > 0)
            {
                result += "\r\ndb" + std::to_string(idx) +
                          ":keys=" + std::to_string(dbsizes_[idx]);
            }
        }
    }

    result += "\r\n";
    reply->OnString(result);
}

void ClusterCommand::OutputResult(OutputHandler *reply) const
{
}

void ClusterInfoCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.empty())
    {
        reply->OnNil();
        return;
    }
    std::string result_str = "";
    for (auto &result : result_)
    {
        result_str.append(result);
        result_str.append("\n");
    }
    reply->OnString(result_str);
}

void ClusterNodesCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.empty())
    {
        reply->OnNil();
        return;
    }
    std::string result_str = "";
    for (auto &result : result_)
    {
        result_str.append(result);
        result_str.append("\n");
    }
    reply->OnString(result_str);
}

void ClusterSlotsCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.empty())
    {
        reply->OnArrayStart(0);
        reply->OnArrayEnd();
        return;
    }
    reply->OnArrayStart(result_.size());
    for (auto &result : result_)
    {
        assert(result.hosts.size() > 0);

        reply->OnArrayStart(2 + result.hosts.size());
        reply->OnInt(result.start_slot_range);
        reply->OnInt(result.end_slot_range);

        for (auto &host : result.hosts)
        {
            reply->OnArrayStart(4);
            reply->OnString(host.ip);
            reply->OnInt(host.port);
            reply->OnString(host.host_id());
            if (host.host_names.empty())
            {
                reply->OnArrayStart(0);
                reply->OnArrayEnd();
            }
            else
            {
                reply->OnArrayStart(1 + host.host_names.size());
                reply->OnString("hostname");
                for (auto &name : host.host_names)
                {
                    reply->OnString(name);
                }
                reply->OnArrayEnd();
            }

            reply->OnArrayEnd();
        }

        reply->OnArrayEnd();
    }
    reply->OnArrayEnd();
}

void ClusterKeySlotCommand::OutputResult(OutputHandler *reply) const
{
    reply->OnInt(result_);
}

void FailoverCommand::OutputResult(OutputHandler *reply) const
{
    if (failover_succeed_)
    {
        reply->OnStatus(redis_get_error_messages(RD_OK));
    }
    else
    {
        reply->OnError(error_message_);
    }
}

void ClientCommand::OutputResult(OutputHandler *reply) const
{
}

void ConfigCommand::OutputResult(OutputHandler *reply) const
{
    if (flag_ == CONFIG_SET)
    {
        reply->OnStatus("OK");
        return;
    }
    if (results_.empty())
    {
        reply->OnArrayStart(0);
        reply->OnArrayEnd();
        return;
    }
    reply->OnArrayStart(results_.size());
    for (auto &result : results_)
    {
        reply->OnString(result);
    }
    reply->OnArrayEnd();
}

void DBSizeCommand::OutputResult(OutputHandler *reply) const
{
    reply->OnInt(total_db_size_);
}

void ReadOnlyCommand::Execute(RedisServiceImpl *redis_impl,
                              RedisConnectionContext *ctx)
{
}

void ReadOnlyCommand::OutputResult(OutputHandler *reply) const
{
    if (Sharder::Instance().GetNodeCount() == 1)
    {
        reply->OnError("ERR This instance has cluster support disabled");
    }
    else
    {
        reply->OnStatus(redis_get_error_messages(RD_OK));
    }
}

void CommandList(redisCommand *cmd,
                 std::vector<std::string> &command_list,
                 std::string prefix = "")
{
    const auto &command_name =
        prefix.empty() ? cmd->declared_name : prefix + "|" + cmd->declared_name;
    command_list.emplace_back(command_name);

    if (!cmd->subcommands)
    {
        return;
    }
    size_t sub_command_count = 0;
    for (; cmd->subcommands[sub_command_count].declared_name != nullptr;
         sub_command_count++)
    {
        CommandList(
            &cmd->subcommands[sub_command_count], command_list, command_name);
    }
}

void CommandReply(redisCommand *cmd,
                  OutputHandler *reply,
                  std::string prefix = "")
{
    auto command_name =
        prefix.empty() ? cmd->declared_name : prefix + "|" + cmd->declared_name;

    reply->OnArrayStart(10);

    reply->OnString(command_name);

    reply->OnInt(cmd->arity);

    auto flags = CommandFlag(cmd->flags);
    reply->OnArrayStart(flags.size());
    for (auto &flag : flags)
    {
        reply->OnString(flag);
    }
    reply->OnArrayEnd();

    int first_key = 0, last_key = 0, key_step = 0;
    if (cmd->key_specs && cmd->key_specs->begin_search_type != KSPEC_BS_INVALID)
    {
        first_key = cmd->key_specs->bs.index.pos;
        last_key = cmd->key_specs->fk.range.lastkey;
        if (last_key >= 0)
        {
            last_key += first_key;
        }
        key_step = cmd->key_specs->fk.range.keystep;
    }
    reply->OnInt(first_key);

    reply->OnInt(last_key);

    reply->OnInt(key_step);

    auto categories = AclCategory(cmd->acl_categories, cmd->flags);
    reply->OnArrayStart(categories.size());
    for (auto &category : categories)
    {
        std::string categorymsg = "@";
        categorymsg += category;
        reply->OnString(categorymsg);
    }
    reply->OnArrayEnd();

    reply->OnArrayStart(cmd->num_tips);
    for (int i = 0; i < cmd->num_tips; ++i)
    {
        reply->OnString(cmd->tips[i]);
    }
    reply->OnArrayEnd();

    reply->OnArrayStart(cmd->key_specs_num);
    for (int i = 0; i < cmd->key_specs_num; ++i)
    {
        auto key_spec = &cmd->key_specs[i];
        int maplen = 6;
        if (key_spec->notes)
        {
            maplen += 2;
        }

        reply->OnArrayStart(maplen);
        if (key_spec->notes)
        {
            reply->OnString("notes");
            reply->OnString(key_spec->notes);
        }

        reply->OnString("flags");
        auto doc_flag_names = DocFlagName(key_spec->flags);
        reply->OnArrayStart(doc_flag_names.size());
        for (auto &flag : doc_flag_names)
        {
            reply->OnString(flag);
        }
        reply->OnArrayEnd();

        reply->OnString("begin_search");
        switch (key_spec->begin_search_type)
        {
        case KSPEC_BS_UNKNOWN:
            reply->OnArrayStart(4);
            reply->OnString("type");
            reply->OnString("unknown");
            reply->OnString("spec");
            reply->OnArrayStart(0);
            reply->OnArrayEnd();
            reply->OnArrayEnd();
            break;
        case KSPEC_BS_INDEX:
            reply->OnArrayStart(4);
            reply->OnString("type");
            reply->OnString("index");
            reply->OnString("spec");
            reply->OnArrayStart(2);
            reply->OnString("index");
            reply->OnInt(key_spec->bs.index.pos);
            reply->OnArrayEnd();
            reply->OnArrayEnd();
            break;
        case KSPEC_BS_KEYWORD:
            reply->OnArrayStart(4);
            reply->OnString("type");
            reply->OnString("keyword");
            reply->OnString("spec");
            reply->OnArrayStart(4);
            reply->OnString("keyword");
            reply->OnString(key_spec->bs.keyword.keyword);
            reply->OnString("startfrom");
            reply->OnInt(key_spec->bs.keyword.startfrom);
            reply->OnArrayEnd();
            reply->OnArrayEnd();
            break;
        default:
            assert(false);
            break;
        }

        reply->OnString("find_keys");
        switch (key_spec->find_keys_type)
        {
        case KSPEC_FK_UNKNOWN:
            reply->OnArrayStart(4);
            reply->OnString("type");
            reply->OnString("unknown");
            reply->OnString("spec");
            reply->OnArrayStart(0);
            reply->OnArrayEnd();
            reply->OnArrayEnd();
            break;
        case KSPEC_FK_RANGE:
            reply->OnArrayStart(4);
            reply->OnString("type");
            reply->OnString("range");
            reply->OnString("spec");
            reply->OnArrayStart(6);
            reply->OnString("lastkey");
            reply->OnInt(key_spec->fk.range.lastkey);
            reply->OnString("keystep");
            reply->OnInt(key_spec->fk.range.keystep);
            reply->OnString("limit");
            reply->OnInt(key_spec->fk.range.limit);
            reply->OnArrayEnd();
            reply->OnArrayEnd();
            break;
        case KSPEC_FK_KEYNUM:
            reply->OnArrayStart(4);
            reply->OnString("type");
            reply->OnString("keynum");
            reply->OnString("spec");
            reply->OnArrayStart(6);
            reply->OnString("keynumidx");
            reply->OnInt(key_spec->fk.keynum.keynumidx);
            reply->OnString("firstkey");
            reply->OnInt(key_spec->fk.keynum.firstkey);
            reply->OnString("keystep");
            reply->OnInt(key_spec->fk.keynum.keystep);
            reply->OnArrayEnd();
            reply->OnArrayEnd();
            break;
        default:
            assert(false);
            break;
        }

        reply->OnArrayEnd();
    }
    reply->OnArrayEnd();

    if (!cmd->subcommands)
    {
        reply->OnArrayStart(0);
        reply->OnArrayEnd();
    }
    else
    {
        size_t sub_command_count = 0;
        for (; cmd->subcommands[sub_command_count].declared_name != nullptr;
             sub_command_count++)
            ;
        reply->OnArrayStart(sub_command_count);
        for (size_t i = 0; i < sub_command_count; i++)
        {
            CommandReply(&cmd->subcommands[i], reply, command_name);
        }
        reply->OnArrayEnd();
    }

    reply->OnArrayEnd();
}

void CommandCommand::OutputResult(OutputHandler *reply) const
{
    size_t command_count = 0;
    for (; redisCommandTable[command_count].declared_name != nullptr;
         command_count++)
        ;
    reply->OnArrayStart(command_count);
    for (size_t i = 0; i < command_count; i++)
    {
        CommandReply(&redisCommandTable[i], reply);
    }
    reply->OnArrayEnd();
}

void CommandInfoCommand::OutputResult(OutputHandler *reply) const
{
    size_t command_count = 0;
    for (; redisCommandTable[command_count].declared_name != nullptr;
         command_count++)
        ;
    if (command_names_.empty())
    {
        reply->OnArrayStart(command_count);
        for (size_t i = 0; i < command_count; i++)
        {
            CommandReply(&redisCommandTable[i], reply);
        }
        reply->OnArrayEnd();
        return;
    }
    reply->OnArrayStart(command_names_.size());
    for (auto &command_name : command_names_)
    {
        bool found = false;
        for (size_t i = 0; i < command_count; i++)
        {
            if (stringcomp(command_name, redisCommandTable[i].declared_name, 1))
            {
                CommandReply(&redisCommandTable[i], reply);
                found = true;
                break;
            }
        }
        if (!found)
        {
            reply->OnNil();
        }
    }
    reply->OnArrayEnd();
}

void CommandCountCommand::OutputResult(OutputHandler *reply) const
{
    size_t command_count = 0;
    for (; redisCommandTable[command_count].declared_name != nullptr;
         command_count++)
        ;
    reply->OnInt(command_count);
}

void CommandListCommand::OutputResult(OutputHandler *reply) const
{
    size_t command_count = 0;
    std::vector<std::string> command_list;
    for (; redisCommandTable[command_count].declared_name != nullptr;
         command_count++)
    {
        CommandList(&redisCommandTable[command_count], command_list);
    }
    reply->OnArrayStart(command_list.size());
    for (auto &command : command_list)
    {
        reply->OnString(command);
    }
    reply->OnArrayEnd();
}

void SetCommand::OutputResult(OutputHandler *reply) const
{
    if (flag_ & OBJ_SET_SETNX)
    {
        reply->OnInt(result_.int_ret_);
        return;
    }

    if (flag_ & OBJ_GET_SET)
    {
        if (result_.err_code_ == RD_OK)
        {
            reply->OnString(result_.str_);
        }
        else if (result_.err_code_ == RD_NIL)
        {
            reply->OnNil();
        }
        else
        {
            reply->OnError(redis_get_error_messages(result_.err_code_));
        }
        return;
    }
    else if (result_.err_code_ == RD_OK)
    {
        reply->OnStatus(redis_get_error_messages(RD_OK));
    }
    else if (result_.err_code_ == RD_NIL)
    {
        reply->OnNil();
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

txservice::ExecResult GetCommand::ExecuteOn(const txservice::TxObject &object)
{
    const auto &str_obj = static_cast<const RedisStringObject &>(object);
    RedisStringResult &str_result = result_;

    if (!CheckTypeMatch(object))
    {
        str_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    str_obj.Execute(*this);
    return txservice::ExecResult::Read;
}

void GetCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.err_code_ == RD_OK)
    {
        const auto &str_result = result_;
        reply->OnString(str_result.str_);
    }
    else if (result_.err_code_ == RD_NIL)
    {
        reply->OnNil();
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

txservice::ExecResult GetDelCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    const auto &str_obj = static_cast<const RedisStringObject &>(object);
    RedisStringResult &str_result = result_;

    if (!CheckTypeMatch(object))
    {
        str_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    str_obj.Execute(*this);
    return txservice::ExecResult::Write;
}

txservice::TxObject *GetDelCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    return nullptr;
}

void GetDelCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.err_code_ == RD_OK)
    {
        const auto &str_result = result_;
        reply->OnString(str_result.str_);
    }
    else if (result_.err_code_ == RD_NIL)
    {
        reply->OnNil();
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

txservice::ExecResult StrLenCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    const auto &str_obj = static_cast<const RedisStringObject &>(object);
    RedisStringResult &str_result = result_;

    if (!CheckTypeMatch(object))
    {
        str_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    str_obj.Execute(*this);
    return txservice::ExecResult::Read;
}

void StrLenCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.err_code_ == RD_OK)
    {
        const auto &str_result = result_;
        reply->OnInt(str_result.int_ret_);
    }
    else if (result_.err_code_ == RD_NIL)
    {
        reply->OnInt(0);
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

txservice::ExecResult GetBitCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    const auto &str_obj = static_cast<const RedisStringObject &>(object);
    RedisStringResult &str_result = result_;

    if (!CheckTypeMatch(object))
    {
        str_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    str_obj.Execute(*this);
    return txservice::ExecResult::Read;
}

void GetBitCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.err_code_ == RD_OK)
    {
        const auto &str_result = result_;
        reply->OnInt(str_result.int_ret_);
    }
    else if (result_.err_code_ == RD_NIL)
    {
        reply->OnInt(0);
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

void GetBitCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::GETBIT);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
    str.append(reinterpret_cast<const char *>(&offset_), sizeof(int64_t));
}

void GetBitCommand::Deserialize(std::string_view cmd_img)
{
    const char *ptr = cmd_img.data();
    offset_ = *reinterpret_cast<const int64_t *>(ptr);
}

txservice::ExecResult GetRangeCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    const auto &str_obj = static_cast<const RedisStringObject &>(object);
    RedisStringResult &str_result = result_;

    if (!CheckTypeMatch(object))
    {
        str_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    str_obj.Execute(*this);
    return txservice::ExecResult::Read;
}

void GetRangeCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.err_code_ == RD_OK || result_.err_code_ == RD_NIL)
    {
        const auto &str_result = result_;
        reply->OnString(str_result.str_);
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

txservice::ExecResult BitCountCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    if (!CheckTypeMatch(object))
    {
        result_.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const auto &str_obj = static_cast<const RedisStringObject &>(object);
    str_obj.Execute(*this);
    return txservice::ExecResult::Read;
}

void BitCountCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.err_code_ == RD_OK || result_.err_code_ == RD_NIL)
    {
        reply->OnInt(result_.int_ret_);
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

void BitCountCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::BITCOUNT);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));

    str.append(reinterpret_cast<const char *>(&start_), sizeof(int64_t));
    str.append(reinterpret_cast<const char *>(&end_), sizeof(int64_t));

    uint8_t offset_type = static_cast<uint8_t>(type_);
    str.append(reinterpret_cast<const char *>(&offset_type), sizeof(uint8_t));
}

void BitCountCommand::Deserialize(std::string_view cmd_img)
{
    const char *ptr = cmd_img.data();
    start_ = *reinterpret_cast<const int64_t *>(ptr);
    ptr += sizeof(int64_t);
    end_ = *reinterpret_cast<const int64_t *>(ptr);
    ptr += sizeof(int64_t);
    type_ = static_cast<OffsetType>(*reinterpret_cast<const uint8_t *>(ptr));
    ptr += sizeof(uint8_t);
}

void GetRangeCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::GETRANGE);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
    str.append(reinterpret_cast<const char *>(&start_), sizeof(int64_t));
    str.append(reinterpret_cast<const char *>(&end_), sizeof(int64_t));
}

void GetRangeCommand::Deserialize(std::string_view cmd_img)
{
    const char *ptr = cmd_img.data();
    start_ = *reinterpret_cast<const int64_t *>(ptr);
    ptr += sizeof(int64_t);
    end_ = *reinterpret_cast<const int64_t *>(ptr);
}

SetBitCommand::SetBitCommand(const EloqKV::SetBitCommand &rhs)
    : StringCommand(rhs), offset_(rhs.offset_), value_(rhs.value_)
{
}

txservice::ExecResult SetBitCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    assert(static_cast<uint64_t>(offset_) < MAX_OBJECT_SIZE * 8);

    const auto &str_obj = static_cast<const RedisStringObject &>(object);
    RedisStringResult &str_result = result_;

    if (!CheckTypeMatch(object))
    {
        str_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    bool success = str_obj.Execute(*this);
    return success ? txservice::ExecResult::Write : txservice::ExecResult::Fail;
}

txservice::TxObject *SetBitCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    auto *str_obj_ptr = static_cast<RedisStringObject *>(obj_ptr);
    // the object is string type
    str_obj_ptr->CommitSetBit(offset_, value_);
    return obj_ptr;
}

void SetBitCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::SETBIT);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));

    str.append(reinterpret_cast<const char *>(&offset_), sizeof(int64_t));
    str.append(reinterpret_cast<const char *>(&value_), sizeof(int8_t));
}

void SetBitCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    offset_ = *reinterpret_cast<const int64_t *>(ptr);
    ptr += sizeof(int64_t);
    value_ = *reinterpret_cast<const int8_t *>(ptr);
}

void SetBitCommand::OutputResult(OutputHandler *reply) const
{
    const auto &str_result = result_;
    if (str_result.err_code_ == RD_OK)
    {
        reply->OnInt(str_result.int_ret_);
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

SetRangeCommand::SetRangeCommand(const EloqKV::SetRangeCommand &rhs)
    : StringCommand(rhs), offset_(rhs.offset_), value_(rhs.value_.Clone())
{
}

txservice::ExecResult SetRangeCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    assert(static_cast<uint64_t>(offset_) + value_.Length() <= MAX_OBJECT_SIZE);
    const auto &str_obj = static_cast<const RedisStringObject &>(object);
    RedisStringResult &str_result = result_;

    if (!CheckTypeMatch(object))
    {
        str_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    bool success = str_obj.Execute(*this);
    return success ? txservice::ExecResult::Write : txservice::ExecResult::Fail;
}

txservice::TxObject *SetRangeCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    auto *str_obj_ptr = static_cast<RedisStringObject *>(obj_ptr);
    // the object is string type
    str_obj_ptr->CommitSetRange(offset_, value_);
    if (str_obj_ptr->Length() == 0)
    {
        return nullptr;
    }
    else
    {
        return obj_ptr;
    }
}

void SetRangeCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::SETRANGE);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));

    str.append(reinterpret_cast<const char *>(&offset_), sizeof(int64_t));
    uint32_t len = value_.Length();
    str.append(reinterpret_cast<const char *>(&len), sizeof(uint32_t));
    str.append(value_.Data(), len);
}

void SetRangeCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    offset_ = *reinterpret_cast<const int64_t *>(ptr);
    ptr += sizeof(int64_t);
    uint32_t len = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);
    value_ = EloqString(ptr, len);
}

void SetRangeCommand::OutputResult(OutputHandler *reply) const
{
    const auto &str_result = result_;
    if (str_result.err_code_ == RD_OK)
    {
        reply->OnInt(str_result.int_ret_);
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

AppendCommand::AppendCommand(const EloqKV::AppendCommand &rhs)
    : StringCommand(rhs), value_(rhs.value_.Clone())
{
}

txservice::ExecResult AppendCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    if (!CheckTypeMatch(object))
    {
        result_.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const auto &str_obj = static_cast<const RedisStringObject &>(object);
    bool success = str_obj.Execute(*this);
    return success ? txservice::ExecResult::Write : txservice::ExecResult::Fail;
}

txservice::TxObject *AppendCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    auto *str_obj_ptr = static_cast<RedisStringObject *>(obj_ptr);
    // the object is string type
    str_obj_ptr->CommitAppend(value_);
    return obj_ptr;
}

void AppendCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::APPEND);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(cmd_type));

    uint32_t len = value_.Length();
    str.append(reinterpret_cast<const char *>(&len), sizeof(len));
    str.append(value_.Data(), len);
}

void AppendCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    uint32_t len = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(len);
    value_ = EloqString(ptr, len);
    ptr += len;
}

void AppendCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.err_code_ == RD_OK)
    {
        reply->OnInt(result_.int_ret_);
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

void SlowLogCommand::Execute(RedisServiceImpl *redis_impl,
                             RedisConnectionContext *ctx)
{
    if (flag_ & SLOWLOG_GET)
    {
        redis_impl->GetSlowLog(results_, count_);
    }
    else if (flag_ & SLOWLOG_RESET)
    {
        redis_impl->ResetSlowLog();
    }
    else if (flag_ & SLOWLOG_LEN)
    {
        len_ = redis_impl->GetSlowLogLen();
    }
}

void SlowLogCommand::OutputResult(OutputHandler *reply) const
{
    if (flag_ & SLOWLOG_GET)
    {
        reply->OnArrayStart(results_.size());
        // Output format is [id, timestamp, execution_time, command,
        // client_addr, client_name]
        for (const auto &entry : results_)
        {
            reply->OnArrayStart(6);
            reply->OnInt(entry.id_);
            reply->OnInt(entry.timestamp_);
            reply->OnInt(entry.execution_time_);
            reply->OnArrayStart(entry.cmd_.size());
            for (const auto &cmd : entry.cmd_)
            {
                reply->OnString(cmd);
            }
            reply->OnArrayEnd();
            reply->OnString(
                std::string(butil::ip2str(entry.client_addr_.ip).c_str()) +
                ":" + std::to_string(entry.client_addr_.port));
            reply->OnString(entry.client_name_);
            reply->OnArrayEnd();
        }
        reply->OnArrayEnd();
    }
    else if (flag_ & SLOWLOG_RESET)
    {
        reply->OnString("OK");
    }
    else if (flag_ & SLOWLOG_LEN)
    {
        reply->OnInt(len_);
    }
}

void PublishCommand::Execute(RedisServiceImpl *redis_impl,
                             RedisConnectionContext *ctx)
{
    received_ = redis_impl->Publish(chan_, message_);
}

void PublishCommand::OutputResult(OutputHandler *reply) const
{
    reply->OnInt(received_);
}

BitFieldCommand::BitFieldCommand(const EloqKV::BitFieldCommand &rhs)
    : StringCommand(rhs)
{
    for (const SubCommand &cmd : rhs.vct_sub_cmd_)
    {
        vct_sub_cmd_.push_back(cmd);
    }
}

txservice::ExecResult BitFieldCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    if (!CheckTypeMatch(object))
    {
        result_.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const auto &str_obj = static_cast<const RedisStringObject &>(object);
    return str_obj.Execute(*this) ? txservice::ExecResult::Write
                                  : txservice::ExecResult::Read;
}

txservice::TxObject *BitFieldCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    auto *str_obj_ptr = static_cast<RedisStringObject *>(obj_ptr);
    // the object is string type
    str_obj_ptr->CommitBitField(vct_sub_cmd_);

    if (str_obj_ptr->Length() == 0)
    {
        return nullptr;
    }
    else
    {
        return obj_ptr;
    }
}

void BitFieldCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::BITFIELD);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));

    uint32_t len = static_cast<uint32_t>(vct_sub_cmd_.size());
    str.append(reinterpret_cast<const char *>(&len), sizeof(len));
    for (const SubCommand &cmd : vct_sub_cmd_)
    {
        str.append(reinterpret_cast<const char *>(&cmd), sizeof(SubCommand));
    }
}

void BitFieldCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    uint32_t len = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);
    vct_sub_cmd_.clear();
    vct_sub_cmd_.reserve(len);

    for (uint32_t i = 0; i < len; i++)
    {
        vct_sub_cmd_.push_back(*reinterpret_cast<const SubCommand *>(ptr));
        ptr += sizeof(SubCommand);
    }
}

void BitFieldCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.err_code_ == RD_OK)
    {
        reply->OnArrayStart(result_.vct_int_.size());
        for (const auto &val : result_.vct_int_)
        {
            if (val.first)
            {
                reply->OnInt(val.second);
            }
            else
            {
                reply->OnNil();
            }
        }
        reply->OnArrayEnd();
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

txservice::ExecResult BitPosCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    if (!CheckTypeMatch(object))
    {
        result_.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    result_.err_code_ = RD_OK;
    const auto &str_obj = static_cast<const RedisStringObject &>(object);
    str_obj.Execute(*this);
    return txservice::ExecResult::Read;
}

void BitPosCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::BITPOS);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));

    str.append(reinterpret_cast<const char *>(&bit_val_), sizeof(uint8_t));
    str.append(reinterpret_cast<const char *>(&start_), sizeof(int64_t));
    str.append(reinterpret_cast<const char *>(&end_), sizeof(int64_t));
}

void BitPosCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    bit_val_ = *reinterpret_cast<const uint8_t *>(ptr);
    ptr += sizeof(uint8_t);
    start_ = *reinterpret_cast<const int64_t *>(ptr);
    ptr += sizeof(int64_t);
    end_ = *reinterpret_cast<const int64_t *>(ptr);
}

void BitPosCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.err_code_ == RD_OK)
    {
        reply->OnInt(result_.int_ret_);
    }
    else if (result_.err_code_ == RD_NIL)
    {
        if (bit_val_ == 0)
        {
            reply->OnInt(0);
        }
        else
        {
            reply->OnInt(-1);
        }
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

std::unique_ptr<txservice::TxRecord> ListCommand::CreateObject(
    const std::string *image) const
{
    auto tmp_obj = std::make_unique<RedisListObject>();
    if (image != nullptr)
    {
        size_t offset = 0;
        tmp_obj->Deserialize(image->data(), offset);
        assert(offset == image->size());
    }
    return tmp_obj;
}

bool ListCommand::CheckTypeMatch(const txservice::TxRecord &obj)
{
    const auto &redis_obj = static_cast<const RedisEloqObject &>(obj);
    return redis_obj.ObjectType() == RedisObjectType::List;
}

LPushCommand::LPushCommand(const LPushCommand &rhs) : ListCommand(rhs)
{
    elements_.reserve(rhs.elements_.size());
    for (const auto &eloq_str : rhs.elements_)
    {
        elements_.emplace_back(eloq_str.Clone());
    }
}

txservice::ExecResult LPushCommand::ExecuteOn(const txservice::TxObject &object)
{
    RedisListResult &list_result = result_;

    if (!CheckTypeMatch(object))
    {
        list_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const auto &list_obj = static_cast<const RedisListObject &>(object);
    bool success = list_obj.Execute(*this);
    return success ? txservice::ExecResult::Write : txservice::ExecResult::Fail;
}

txservice::TxObject *LPushCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    auto &list_obj = static_cast<RedisListObject &>(*obj_ptr);
    list_obj.CommitLPush(elements_);
    return obj_ptr;
}

void LPushCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::LPUSH);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));

    uint32_t elements_cnt = elements_.size();
    str.append(reinterpret_cast<const char *>(&elements_cnt), sizeof(uint32_t));
    for (const auto &eloq_str : elements_)
    {
        std::string_view sv = eloq_str.StringView();
        uint32_t len = sv.size();
        str.append(reinterpret_cast<const char *>(&len), sizeof(uint32_t));
        str.append(sv.data(), len);
    }
}

void LPushCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    const uint32_t elements_cnt = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);

    for (uint32_t i = 0; i < elements_cnt; i++)
    {
        const uint32_t len = *reinterpret_cast<const uint32_t *>(ptr);
        ptr += sizeof(uint32_t);
        elements_.emplace_back(ptr, len);
        ptr += len;
    }
}

void LPushCommand::OutputResult(OutputHandler *reply) const
{
    const auto &list_result = result_;
    if (list_result.err_code_ == RD_OK)
    {
        reply->OnInt(list_result.ret_);
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

txservice::ExecResult LPopCommand::ExecuteOn(const txservice::TxObject &object)
{
    RedisListResult &list_result = result_;

    if (!CheckTypeMatch(object))
    {
        list_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const auto &list_obj = static_cast<const RedisListObject &>(object);
    list_obj.Execute(*this);
    return txservice::ExecResult::Write;
}

txservice::TxObject *LPopCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    auto &list_obj = static_cast<RedisListObject &>(*obj_ptr);
    int64_t count = count_ == -1 ? 1 : count_;
    bool empty_after_removal = list_obj.CommitLPop(count);
    return empty_after_removal ? nullptr : obj_ptr;
}

void LPopCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::LPOP);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
    str.append(reinterpret_cast<const char *>(&count_), sizeof(int64_t));
}

void LPopCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    count_ = *reinterpret_cast<const int64_t *>(ptr);
}

void LPopCommand::OutputResult(OutputHandler *reply) const
{
    const auto &list_result = result_;
    if (list_result.err_code_ == RD_OK)
    {
        assert(list_result.ret_ >= 0);
        if (count_ == -1)
        {
            assert(std::get<std::vector<std::string>>(list_result.result_)
                       .size() == 1);
            reply->OnString(
                std::get<std::vector<std::string>>(list_result.result_)[0]);
        }
        else
        {
            reply->OnArrayStart(
                std::get<std::vector<std::string>>(list_result.result_).size());
            for (const auto &str :
                 std::get<std::vector<std::string>>(list_result.result_))
            {
                reply->OnString(str);
            }
            reply->OnArrayEnd();
        }
    }
    else if (list_result.err_code_ == RD_NIL)
    {
        reply->OnNil();
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

RPushCommand::RPushCommand(const RPushCommand &rhs) : ListCommand(rhs)
{
    elements_.reserve(rhs.elements_.size());
    for (const auto &eloq_str : rhs.elements_)
    {
        elements_.emplace_back(eloq_str.Clone());
    }
}

txservice::ExecResult RPushCommand::ExecuteOn(const txservice::TxObject &object)
{
    RedisListResult &list_result = result_;

    if (!CheckTypeMatch(object))
    {
        list_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const auto &list_obj = static_cast<const RedisListObject &>(object);
    bool success = list_obj.Execute(*this);
    return success ? txservice::ExecResult::Write : txservice::ExecResult::Fail;
}

txservice::TxObject *RPushCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    auto &list_obj = static_cast<RedisListObject &>(*obj_ptr);
    list_obj.CommitRPush(elements_);
    return obj_ptr;
}

void RPushCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::RPUSH);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));

    uint32_t elements_cnt = elements_.size();
    str.append(reinterpret_cast<const char *>(&elements_cnt), sizeof(uint32_t));
    for (const auto &eloq_str : elements_)
    {
        std::string_view sv = eloq_str.StringView();
        uint32_t len = sv.size();
        str.append(reinterpret_cast<const char *>(&len), sizeof(uint32_t));
        str.append(sv.data(), len);
    }
}

void RPushCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    const uint32_t elements_cnt = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);

    for (uint32_t i = 0; i < elements_cnt; i++)
    {
        const uint32_t len = *reinterpret_cast<const uint32_t *>(ptr);
        ptr += sizeof(uint32_t);
        elements_.emplace_back(ptr, len);
        ptr += len;
    }
}

void RPushCommand::OutputResult(OutputHandler *reply) const
{
    const auto &list_result = result_;
    if (list_result.err_code_ == RD_OK)
    {
        reply->OnInt(list_result.ret_);
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

txservice::ExecResult RPopCommand::ExecuteOn(const txservice::TxObject &object)
{
    RedisListResult &list_result = result_;

    if (!CheckTypeMatch(object))
    {
        list_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const auto &list_obj = static_cast<const RedisListObject &>(object);
    list_obj.Execute(*this);
    return txservice::ExecResult::Write;
}

txservice::TxObject *RPopCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    auto &list_obj = static_cast<RedisListObject &>(*obj_ptr);
    int64_t count = count_ == -1 ? 1 : count_;
    bool empty_after_removal = list_obj.CommitRPop(count);
    return empty_after_removal ? nullptr : obj_ptr;
}

void RPopCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::RPOP);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
    str.append(reinterpret_cast<const char *>(&count_), sizeof(int64_t));
}

void RPopCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    count_ = *reinterpret_cast<const int64_t *>(ptr);
}

void RPopCommand::OutputResult(OutputHandler *reply) const
{
    const auto &list_result = result_;
    if (list_result.err_code_ == RD_OK)
    {
        assert(list_result.ret_ >= 0);
        if (count_ == -1)
        {
            reply->OnString(
                std::get<std::vector<std::string>>(list_result.result_)[0]);
        }
        else
        {
            reply->OnArrayStart(
                std::get<std::vector<std::string>>(list_result.result_).size());
            for (const auto &str :
                 std::get<std::vector<std::string>>(list_result.result_))
            {
                reply->OnString(str);
            }
            reply->OnArrayEnd();
        }
    }
    else if (list_result.err_code_ == RD_NIL)
    {
        reply->OnNil();
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

LMPopCommand::LMPopCommand(std::vector<EloqKey> &&keys,
                           bool from_left,
                           int64_t count)
    : keys_(keys)
{
    assert(count > 0);
    if (from_left)
    {
        pop_cmd_ = std::make_unique<LPopCommand>(static_cast<int32_t>(count));
    }
    else
    {
        pop_cmd_ = std::make_unique<RPopCommand>(static_cast<int32_t>(count));
    }

    vct_key_ptrs_.reserve(keys_.size());
    vct_cmd_ptrs_.reserve(keys_.size());
    for (const auto &key : keys_)
    {
        std::vector<txservice::TxKey> vct;
        vct.emplace_back(&key);
        vct_key_ptrs_.emplace_back(std::move(vct));
        std::vector<txservice::TxCommand *> vct2;
        vct2.emplace_back(pop_cmd_.get());
        vct_cmd_ptrs_.emplace_back(std::move(vct2));
    }
}

void LMPopCommand::OutputResult(OutputHandler *reply) const
{
    const auto &list_result = pop_cmd_->result_;
    if (list_result.err_code_ == RD_OK)
    {
        if (list_result.ret_ == 0)
        {
            reply->OnNil();
        }
        else
        {
            assert(curr_step_ >= 0 && curr_step_ < keys_.size());
            reply->OnArrayStart(2);
            reply->OnString(keys_[curr_step_].StringView());
            reply->OnArrayStart(
                std::get<std::vector<std::string>>(list_result.result_).size());
            for (const auto &str :
                 std::get<std::vector<std::string>>(list_result.result_))
            {
                reply->OnString(str);
            }
            reply->OnArrayEnd();
            reply->OnArrayEnd();
        }
    }
    else if (list_result.err_code_ == RD_NIL)
    {
        reply->OnNil();
    }
    else
    {
        reply->OnError(redis_get_error_messages(list_result.err_code_));
    }
}

LMovePopCommand::LMovePopCommand(const LMovePopCommand &rhs)
    : ListCommand(rhs), is_left_(rhs.is_left_)
{
}

txservice::ExecResult LMovePopCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    RedisListResult &list_result = result_;

    if (!CheckTypeMatch(object))
    {
        list_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const RedisListObject &list_obj =
        static_cast<const RedisListObject &>(object);
    // Do not write log and proeed if source list is empty.
    bool succeed = list_obj.Execute(*this);
    return succeed ? ExecResult::Write : ExecResult::Read;
}

txservice::TxObject *LMovePopCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    auto &list_obj = static_cast<RedisListObject &>(*obj_ptr);
    bool empty_after_removal = list_obj.CommitLMovePop(is_left_);
    return empty_after_removal ? nullptr : obj_ptr;
}

void LMovePopCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::LMOVEPOP);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
    str.append(reinterpret_cast<const char *>(&is_left_), sizeof(bool));
}

void LMovePopCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    is_left_ = *reinterpret_cast<const bool *>(ptr);
}

void LMovePopCommand::OutputResult(OutputHandler *reply) const
{
    const auto &list_result = result_;
    if (list_result.err_code_ == RD_OK)
    {
        reply->OnString(std::get<std::string>(list_result.result_));
    }
    else if (result_.err_code_ == RD_NIL)
    {
        reply->OnNil();
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

LMovePushCommand::LMovePushCommand(const LMovePushCommand &rhs)
    : ListCommand(rhs),
      is_left_(rhs.is_left_),
      element_(rhs.element_.Clone()),
      is_in_the_middle_stage_(rhs.is_in_the_middle_stage_)
{
}

txservice::ExecResult LMovePushCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    RedisListResult &list_result = result_;

    if (!CheckTypeMatch(object))
    {
        list_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const RedisListObject &list_obj =
        static_cast<const RedisListObject &>(object);
    bool success = list_obj.Execute(*this);
    return success ? txservice::ExecResult::Write : txservice::ExecResult::Fail;
}

txservice::TxObject *LMovePushCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    auto &list_obj = static_cast<RedisListObject &>(*obj_ptr);
    list_obj.CommitLMovePush(is_left_, element_, is_in_the_middle_stage_);
    return obj_ptr;
}

void LMovePushCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::LMOVEPUSH);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
    str.append(reinterpret_cast<const char *>(&is_left_), sizeof(bool));

    uint32_t element_size = element_.Length();
    str.append(reinterpret_cast<const char *>(&element_size), sizeof(uint32_t));
    str.append(reinterpret_cast<const char *>(element_.Data()), element_size);
}

void LMovePushCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    is_left_ = *reinterpret_cast<const bool *>(ptr);
    ptr += sizeof(bool);

    uint32_t element_size = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);
    element_ = EloqString(ptr, element_size);
}

void LMovePushCommand::OutputResult(OutputHandler *reply) const
{
    const auto &list_result = result_;
    if (list_result.err_code_ == RD_OK)
    {
        reply->OnString(std::get<EloqString>(list_result.result_).StringView());
    }
    else if (result_.err_code_ == RD_NIL)
    {
        reply->OnNil();
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

txservice::ExecResult LRangeCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    RedisListResult &list_result = result_;

    if (!CheckTypeMatch(object))
    {
        list_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const auto &list_obj = static_cast<const RedisListObject &>(object);
    list_obj.Execute(*this);
    return txservice::ExecResult::Read;
}

void LRangeCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::LRANGE);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
    str.append(reinterpret_cast<const char *>(&start_), sizeof(int32_t));
    str.append(reinterpret_cast<const char *>(&end_), sizeof(int32_t));
}

void LRangeCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    start_ = *reinterpret_cast<const int32_t *>(ptr);
    ptr += sizeof(int32_t);
    end_ = *reinterpret_cast<const int32_t *>(ptr);
}

void LRangeCommand::OutputResult(OutputHandler *reply) const
{
    const auto &list_result = result_;
    if (list_result.err_code_ == RD_OK)
    {
        reply->OnArrayStart(
            std::get<std::vector<std::string>>(list_result.result_).size());
        for (const auto &str :
             std::get<std::vector<std::string>>(list_result.result_))
        {
            reply->OnString(str);
        }
        reply->OnArrayEnd();
    }
    else if (list_result.err_code_ == RD_NIL)
    {
        reply->OnArrayStart(0);
        reply->OnArrayEnd();
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

txservice::ExecResult LLenCommand::ExecuteOn(const txservice::TxObject &object)
{
    RedisListResult &list_result = result_;

    if (!CheckTypeMatch(object))
    {
        list_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    list_result.err_code_ = RD_OK;
    const RedisListObject &list_obj =
        static_cast<const RedisListObject &>(object);
    list_obj.Execute(*this);
    return txservice::ExecResult::Read;
}

void LLenCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::LLEN);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
}

void LLenCommand::Deserialize(std::string_view cmd_image)
{
}

void LLenCommand::OutputResult(OutputHandler *reply) const
{
    const auto &list_result = result_;

    if (list_result.err_code_ == RD_OK)
    {
        reply->OnInt(list_result.ret_);
    }
    else if (list_result.err_code_ == RD_NIL)
    {
        reply->OnInt(0);
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

txservice::ExecResult LTrimCommand::ExecuteOn(const txservice::TxObject &object)
{
    RedisListResult &list_result = result_;

    if (!CheckTypeMatch(object))
    {
        list_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    list_result.err_code_ = RD_OK;
    const RedisListObject &list_obj =
        static_cast<const RedisListObject &>(object);
    list_obj.Execute(*this);
    return txservice::ExecResult::Write;
}

txservice::TxObject *LTrimCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    RedisListObject &list_obj = static_cast<RedisListObject &>(*obj_ptr);
    bool empty_after_removal = list_obj.CommitLTrim(start_, end_);
    return empty_after_removal ? nullptr : obj_ptr;
}

void LTrimCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::LTRIM);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
    str.append(reinterpret_cast<const char *>(&start_), sizeof(int64_t));
    str.append(reinterpret_cast<const char *>(&end_), sizeof(int64_t));
}

void LTrimCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    start_ = *reinterpret_cast<const int64_t *>(ptr);
    ptr += sizeof(int64_t);
    end_ = *reinterpret_cast<const int64_t *>(ptr);
}

void LTrimCommand::OutputResult(OutputHandler *reply) const
{
    const auto &list_result = result_;
    if (list_result.err_code_ == RD_OK || list_result.err_code_ == RD_NIL)
    {
        reply->OnStatus(redis_get_error_messages(RD_OK));
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

txservice::ExecResult LIndexCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    RedisListResult &list_result = result_;

    if (!CheckTypeMatch(object))
    {
        list_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    list_result.err_code_ = RD_OK;
    const RedisListObject &list_obj =
        static_cast<const RedisListObject &>(object);
    list_obj.Execute(*this);
    return txservice::ExecResult::Read;
}

void LIndexCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::LINDEX);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
    str.append(reinterpret_cast<const char *>(&index_), sizeof(int64_t));
}

void LIndexCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    index_ = *reinterpret_cast<const int64_t *>(ptr);
}

void LIndexCommand::OutputResult(OutputHandler *reply) const
{
    const auto &list_result = result_;
    if (list_result.err_code_ == RD_OK)
    {
        reply->OnString(std::string_view(
            std::get<std::vector<std::string>>(list_result.result_).at(0)));
    }
    else if (list_result.err_code_ == RD_NIL)
    {
        reply->OnNil();
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

LInsertCommand::LInsertCommand(const LInsertCommand &rhs)
    : ListCommand(rhs),
      is_before_(rhs.is_before_),
      pivot_(rhs.pivot_.Clone()),
      element_(rhs.element_.Clone())
{
}

txservice::ExecResult LInsertCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    RedisListResult &list_result = result_;

    if (!CheckTypeMatch(object))
    {
        list_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    list_result.err_code_ = RD_OK;
    const RedisListObject &list_obj =
        static_cast<const RedisListObject &>(object);
    bool cmd_success = list_obj.Execute(*this);
    return cmd_success ? txservice::ExecResult::Write
                       : txservice::ExecResult::Fail;
}

txservice::TxObject *LInsertCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    RedisListObject &list_obj = static_cast<RedisListObject &>(*obj_ptr);
    list_obj.CommitLInsert(is_before_, pivot_, element_);
    return obj_ptr;
}

void LInsertCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::LINSERT);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));

    str.append(reinterpret_cast<const char *>(&is_before_), sizeof(bool));

    std::string_view pivot_sv = pivot_.StringView();
    uint32_t pivot_len = pivot_sv.size();
    str.append(reinterpret_cast<const char *>(&pivot_len), sizeof(uint32_t));
    str.append(pivot_sv.data(), pivot_len);

    std::string_view element_sv = element_.StringView();
    uint32_t element_len = element_sv.size();
    str.append(reinterpret_cast<const char *>(&element_len), sizeof(uint32_t));
    str.append(element_sv.data(), element_len);
}

void LInsertCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();

    is_before_ = *reinterpret_cast<const bool *>(ptr);
    ptr += sizeof(bool);

    const uint32_t pivot_len = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);
    pivot_ = EloqString(ptr, pivot_len);
    ptr += pivot_len;

    const uint32_t element_len = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);
    element_ = EloqString(ptr, element_len);
}

void LInsertCommand::OutputResult(OutputHandler *reply) const
{
    const auto &list_result = result_;
    if (list_result.err_code_ == RD_OK || list_result.err_code_ == RD_NIL)
    {
        reply->OnInt(list_result.ret_);
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

txservice::ExecResult LPosCommand::ExecuteOn(const txservice::TxObject &object)
{
    RedisListResult &list_result = result_;

    if (!CheckTypeMatch(object))
    {
        list_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    list_result.err_code_ = RD_OK;
    const RedisListObject &list_obj =
        static_cast<const RedisListObject &>(object);
    list_obj.Execute(*this);
    return txservice::ExecResult::Read;
}

void LPosCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::LPOS);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));

    std::string_view sv = element_.StringView();
    uint32_t len = sv.size();
    str.append(reinterpret_cast<const char *>(&len), sizeof(uint32_t));
    str.append(sv.data(), len);

    str.append(reinterpret_cast<const char *>(&rank_), sizeof(int64_t));
    str.append(reinterpret_cast<const char *>(&count_), sizeof(uint64_t));
    str.append(reinterpret_cast<const char *>(&len_), sizeof(uint64_t));
}

void LPosCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    const uint32_t len = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);
    element_ = EloqString(ptr, len);
    ptr += len;

    rank_ = *reinterpret_cast<const int64_t *>(ptr);
    ptr += sizeof(int64_t);
    count_ = *reinterpret_cast<const uint64_t *>(ptr);
    ptr += sizeof(uint64_t);
    len_ = *reinterpret_cast<const uint64_t *>(ptr);
}

void LPosCommand::OutputResult(OutputHandler *reply) const
{
    const auto &list_result = result_;
    if (list_result.err_code_ == RD_OK)
    {
        if (count_ != -1)
        {
            reply->OnArrayStart(
                std::get<std::vector<int64_t>>(list_result.result_).size());
            for (const auto &index :
                 std::get<std::vector<int64_t>>(list_result.result_))
            {
                reply->OnInt(index);
            }
            reply->OnArrayEnd();
        }
        else
        {
            assert(std::get<std::vector<int64_t>>(list_result.result_).size() ==
                   1);
            reply->OnInt(
                std::get<std::vector<int64_t>>(list_result.result_)[0]);
        }
    }
    else if (list_result.err_code_ == RD_NIL)
    {
        if (count_ != -1)
        {
            reply->OnArrayStart(0);
            reply->OnArrayEnd();
        }
        else
        {
            reply->OnNil();
        }
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

LSetCommand::LSetCommand(const LSetCommand &rhs)
    : ListCommand(rhs), index_(rhs.index_), element_(rhs.element_.Clone())
{
}

txservice::ExecResult LSetCommand::ExecuteOn(const txservice::TxObject &object)
{
    RedisListResult &list_result = result_;

    if (!CheckTypeMatch(object))
    {
        list_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    list_result.err_code_ = RD_OK;
    const RedisListObject &list_obj =
        static_cast<const RedisListObject &>(object);
    bool cmd_success = list_obj.Execute(*this);
    return cmd_success ? txservice::ExecResult::Write
                       : txservice::ExecResult::Fail;
}

txservice::TxObject *LSetCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    RedisListObject &list_obj = static_cast<RedisListObject &>(*obj_ptr);
    list_obj.CommitLSet(index_, element_);
    return obj_ptr;
}

void LSetCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::LSET);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));

    str.append(reinterpret_cast<const char *>(&index_), sizeof(int64_t));

    std::string_view sv = element_.StringView();
    uint32_t len = sv.size();
    str.append(reinterpret_cast<const char *>(&len), sizeof(uint32_t));
    str.append(sv.data(), len);
}

void LSetCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    index_ = *reinterpret_cast<const int64_t *>(ptr);
    ptr += sizeof(int64_t);

    const uint32_t len = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);
    element_ = EloqString(ptr, len);
    ptr += len;
}

void LSetCommand::OutputResult(OutputHandler *reply) const
{
    const auto &list_result = result_;
    if (list_result.err_code_ == RD_OK)
    {
        reply->OnStatus(redis_get_error_messages(RD_OK));
    }
    else if (list_result.err_code_ == RD_NIL)
    {
        // key not found
        reply->OnError(redis_get_error_messages(RD_ERR_NO_SUCH_KEY));
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

LRemCommand::LRemCommand(const LRemCommand &rhs)
    : ListCommand(rhs), count_(rhs.count_), element_(rhs.element_.Clone())
{
}

txservice::ExecResult LRemCommand::ExecuteOn(const txservice::TxObject &object)
{
    RedisListResult &list_result = result_;

    if (!CheckTypeMatch(object))
    {
        list_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    list_result.err_code_ = RD_OK;
    const RedisListObject &list_obj =
        static_cast<const RedisListObject &>(object);
    bool cmd_success = list_obj.Execute(*this);
    return cmd_success ? txservice::ExecResult::Write
                       : txservice::ExecResult::Fail;
}

txservice::TxObject *LRemCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    RedisListObject &list_obj = static_cast<RedisListObject &>(*obj_ptr);
    bool empty_after_removal = list_obj.CommitLRem(count_, element_);
    return empty_after_removal ? nullptr : obj_ptr;
}

void LRemCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::LREM);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));

    std::string_view sv = element_.StringView();
    uint32_t len = sv.size();
    str.append(reinterpret_cast<const char *>(&len), sizeof(uint32_t));
    str.append(sv.data(), len);

    str.append(reinterpret_cast<const char *>(&count_), sizeof(uint64_t));
}

void LRemCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    const uint32_t len = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);
    element_ = EloqString(ptr, len);
    ptr += len;

    count_ = *reinterpret_cast<const uint64_t *>(ptr);
}

void LRemCommand::OutputResult(OutputHandler *reply) const
{
    const auto &list_result = result_;
    if (list_result.err_code_ == RD_OK)
    {
        reply->OnInt(list_result.ret_);
    }
    else if (list_result.err_code_ == RD_NIL)
    {
        reply->OnInt(0);
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

LPushXCommand::LPushXCommand(const LPushXCommand &rhs) : ListCommand(rhs)
{
    elements_.reserve(rhs.elements_.size());
    for (const auto &eloq_str : rhs.elements_)
    {
        elements_.emplace_back(eloq_str.Clone());
    }
}

txservice::ExecResult LPushXCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    RedisListResult &list_result = result_;

    if (!CheckTypeMatch(object))
    {
        list_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    list_result.err_code_ = RD_OK;
    const RedisListObject &list_obj =
        static_cast<const RedisListObject &>(object);
    bool success = list_obj.Execute(*this);
    return success ? txservice::ExecResult::Write : txservice::ExecResult::Fail;
}

txservice::TxObject *LPushXCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    RedisListObject &list_obj = static_cast<RedisListObject &>(*obj_ptr);
    list_obj.CommitLPush(elements_);
    return obj_ptr;
}

void LPushXCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::LPUSHX);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));

    uint32_t elements_cnt = elements_.size();
    str.append(reinterpret_cast<const char *>(&elements_cnt), sizeof(uint32_t));
    for (const auto &eloq_str : elements_)
    {
        std::string_view sv = eloq_str.StringView();
        uint32_t len = sv.size();
        str.append(reinterpret_cast<const char *>(&len), sizeof(uint32_t));
        str.append(sv.data(), len);
    }
}

void LPushXCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    const uint32_t elements_cnt = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);

    for (uint32_t i = 0; i < elements_cnt; i++)
    {
        const uint32_t len = *reinterpret_cast<const uint32_t *>(ptr);
        ptr += sizeof(uint32_t);
        elements_.emplace_back(ptr, len);
        ptr += len;
    }
}

void LPushXCommand::OutputResult(OutputHandler *reply) const
{
    const auto &list_result = result_;
    if (list_result.err_code_ == RD_OK)
    {
        reply->OnInt(list_result.ret_);
    }
    else if (list_result.err_code_ == RD_NIL)
    {
        reply->OnInt(0);
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

RPushXCommand::RPushXCommand(const RPushXCommand &rhs) : ListCommand(rhs)
{
    elements_.reserve(rhs.elements_.size());
    for (const auto &eloq_str : rhs.elements_)
    {
        elements_.emplace_back(eloq_str.Clone());
    }
}

txservice::ExecResult RPushXCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    RedisListResult &list_result = result_;

    if (!CheckTypeMatch(object))
    {
        list_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    list_result.err_code_ = RD_OK;
    const RedisListObject &list_obj =
        static_cast<const RedisListObject &>(object);
    bool success = list_obj.Execute(*this);
    return success ? txservice::ExecResult::Write : txservice::ExecResult::Fail;
}

txservice::TxObject *RPushXCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    RedisListObject &list_obj = static_cast<RedisListObject &>(*obj_ptr);
    list_obj.CommitRPush(elements_);
    return obj_ptr;
}

void RPushXCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::RPUSHX);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));

    uint32_t elements_cnt = elements_.size();
    str.append(reinterpret_cast<const char *>(&elements_cnt), sizeof(uint32_t));
    for (const auto &eloq_str : elements_)
    {
        std::string_view sv = eloq_str.StringView();
        uint32_t len = sv.size();
        str.append(reinterpret_cast<const char *>(&len), sizeof(uint32_t));
        str.append(sv.data(), len);
    }
}

void RPushXCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    const uint32_t elements_cnt = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);

    for (uint32_t i = 0; i < elements_cnt; i++)
    {
        const uint32_t len = *reinterpret_cast<const uint32_t *>(ptr);
        ptr += sizeof(uint32_t);
        elements_.emplace_back(ptr, len);
        ptr += len;
    }
}

void RPushXCommand::OutputResult(OutputHandler *reply) const
{
    const auto &list_result = result_;
    if (list_result.err_code_ == RD_OK)
    {
        reply->OnInt(list_result.ret_);
    }
    else if (list_result.err_code_ == RD_NIL)
    {
        reply->OnInt(0);
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

txservice::ExecResult BlockLPopCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    RedisListResult &list_result = result_;

    if (!CheckTypeMatch(object))
    {
        list_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    list_result.err_code_ = RD_OK;
    const RedisListObject &list_obj =
        static_cast<const RedisListObject &>(object);
    bool b = false;
    if (op_type_ == txservice::BlockOperation::BlockLock)
    {
        list_result.ret_ = list_obj.Size();
        b = list_result.ret_ > 0;
    }
    else
    {
        b = list_obj.Execute(*this);
    }

    switch (op_type_)
    {
    case txservice::BlockOperation::NoBlock:
        return b ? txservice::ExecResult::Write : txservice::ExecResult::Read;
    case txservice::BlockOperation::PopBlock:
        return b ? txservice::ExecResult::Write : txservice::ExecResult::Block;
    case txservice::BlockOperation::PopNoBlock:
        return b ? txservice::ExecResult::Write : txservice::ExecResult::Unlock;
    case txservice::BlockOperation::BlockLock:
        return b ? txservice::ExecResult::Read : txservice::ExecResult::Block;
    case txservice::BlockOperation::PopElement:
        assert(b);
        return txservice::ExecResult::Write;
    default:
        assert(false);
        return txservice::ExecResult::Fail;
    }
}

txservice::TxObject *BlockLPopCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    RedisListObject &list_obj = static_cast<RedisListObject &>(*obj_ptr);
    bool not_empty = list_obj.CommitBlockPop(is_left_, count_);
    return not_empty ? obj_ptr : nullptr;
}

void BlockLPopCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::BLOCKPOP);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));

    uint8_t op_type = static_cast<uint8_t>(op_type_);
    str.append(reinterpret_cast<const char *>(&op_type), sizeof(uint8_t));
    str.append(sizeof(uint8_t), is_left_ ? 1 : 0);
    str.append(reinterpret_cast<const char *>(&count_), sizeof(uint32_t));
}

void BlockLPopCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();

    uint8_t op_type = *reinterpret_cast<const uint8_t *>(ptr);
    op_type_ = static_cast<BlockOperation>(op_type);
    ptr += sizeof(uint8_t);
    is_left_ = (*ptr != 0);
    ptr += sizeof(uint8_t);
    count_ = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);
}

void BlockLPopCommand::OutputResult(OutputHandler *reply) const
{
    // Now BlockLPopCommand only be as child command of BLMOVE, BLMPOP etc. It
    // does not need output result.
}

bool BlockLPopCommand::AblePopBlockRequest(txservice::TxObject *object) const
{
    if (object == nullptr ||
        static_cast<const RedisEloqObject &>(*object).ObjectType() !=
            RedisObjectType::List)
    {
        return false;
    }

    const RedisListObject &list_obj =
        static_cast<const RedisListObject &>(*object);
    return !list_obj.Empty();
}

txservice::ExecResult BlockDiscardCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    assert(false);
    return txservice::ExecResult::Fail;
}

void BlockDiscardCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::BLOCKDISCARD);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
}

void BlockDiscardCommand::Deserialize(std::string_view cmd_image)
{
    // NOT need to do anything
}

void BlockDiscardCommand::OutputResult(OutputHandler *reply) const
{
    // Only as child command, NOT need to output anything.
}

BLMoveCommand::BLMoveCommand(EloqKey &&skey,
                             BlockLPopCommand &&sour_cmd,
                             EloqKey &&dkey,
                             LMovePushCommand &&dest_cmd,
                             uint64_t ts_expired)
    : sour_key_(std::make_unique<EloqKey>(std::move(skey))),
      sour_cmd_(std::make_unique<BlockLPopCommand>(std::move(sour_cmd))),
      dest_key_(std::make_unique<EloqKey>(std::move(dkey))),
      dest_cmd_(std::make_unique<LMovePushCommand>(std::move(dest_cmd))),
      ts_expired_(ts_expired)
{
    discard_cmd_ = std::make_unique<BlockDiscardCommand>();
    vct_key_ptrs_.reserve(4);
    vct_cmd_ptrs_.reserve(4);

    // The first step to pop an element
    std::vector<txservice::TxKey> vct_key;
    std::vector<txservice::TxCommand *> vct_cmd;
    vct_key.emplace_back(txservice::TxKey(sour_key_.get()));
    vct_key_ptrs_.push_back(std::move(vct_key));
    vct_cmd.emplace_back(sour_cmd_.get());
    vct_cmd_ptrs_.push_back(std::move(vct_cmd));

    // All commands will be blocked in related ccentry, until one of them insert
    // elements or expired. If insert, add lock to the object and goto next
    // step.
    assert(vct_key.empty());
    assert(vct_cmd.empty());
    vct_key.emplace_back(txservice::TxKey(sour_key_.get()));
    vct_key_ptrs_.push_back(std::move(vct_key));
    vct_cmd.emplace_back(sour_cmd_.get());
    vct_cmd_ptrs_.push_back(std::move(vct_cmd));

    assert(vct_key.empty());
    assert(vct_cmd.empty());
    vct_key.emplace_back(txservice::TxKey(sour_key_.get()));
    vct_key_ptrs_.push_back(std::move(vct_key));
    vct_cmd.emplace_back(sour_cmd_.get());
    vct_cmd_ptrs_.push_back(std::move(vct_cmd));

    assert(vct_key.empty());
    assert(vct_cmd.empty());
    // The second step to push the element
    vct_key.emplace_back(txservice::TxKey(dest_key_.get()));
    vct_key_ptrs_.push_back(std::move(vct_key));
    vct_cmd.emplace_back(dest_cmd_.get());
    vct_cmd_ptrs_.push_back(std::move(vct_cmd));
}

void BLMoveCommand::OutputResult(OutputHandler *reply) const
{
    if (curr_step_ == 1 && has_forwarded_ &&
        vct_cmd_ptrs_[curr_step_][0] == discard_cmd_.get())
    {
        // expired
        reply->OnNil();
        return;
    }

    if ((curr_step_ >= 0 && curr_step_ <= 2) &&
        sour_cmd_->result_.err_code_ != RD_OK &&
        sour_cmd_->result_.err_code_ != RD_NIL)
    {
        reply->OnError(redis_get_error_messages(sour_cmd_->result_.err_code_));
    }
    else if (curr_step_ == 3 && dest_cmd_->result_.err_code_ != RD_OK)
    {
        reply->OnError(redis_get_error_messages(dest_cmd_->result_.err_code_));
    }
    else if (curr_step_ == 3 && dest_cmd_->result_.ret_ != 0)
    {
        std::vector<std::string> &elements =
            std::get<std::vector<std::string>>(sour_cmd_->result_.result_);
        assert(elements.size() == 1);
        reply->OnString(elements[0]);
    }
    else
    {
        reply->OnNil();
    }
}

bool BLMoveCommand::ForwardResult()
{
    assert(curr_step_ == 1);
    has_forwarded_ = true;

    if (sour_cmd_->result_.err_code_ == RD_ERR_WRONG_TYPE)
    {
        return false;
    }

    if (sour_cmd_->result_.err_code_ == RD_NIL || sour_cmd_->result_.ret_ == 0)
    {
        vct_cmd_ptrs_[curr_step_][0] = discard_cmd_.get();
        return true;
    }
    else
    {
        return false;
    }
}

bool BLMoveCommand::HandleMiddleResult()
{
    // Step 0: Get result from non-empty list. This step is non-blocking, return
    // immediately if list object is empty. we should return the result if list
    // object isn't empty even if the timeout is very short.
    if (curr_step_ == 0)
    {
        if (sour_cmd_->result_.err_code_ == RD_OK)
        {
            if (sour_cmd_->result_.ret_ > 0)
            {
                // skip step 2 and 3. push element into dest object.
                IncrSteps();
                IncrSteps();
                assert(curr_step_ == 2);

                // The element has poped and goto next step to push it into
                // destination object.
                std::vector<std::string> &elements =
                    std::get<std::vector<std::string>>(
                        sour_cmd_->result_.result_);
                assert(elements.size() == 1);
                dest_cmd_->element_ =
                    EloqString(elements[0].c_str(), elements[0].size());
                // process next step
                return true;
            }
            else
            {
                assert(sour_cmd_->result_.ret_ == 0);
                // multi
                if (ts_expired_ == 0)
                {
                    // expired.
                    return false;
                }

                // empty list.
                // we need to enter blocking phase(step 1).
                sour_cmd_->result_.err_code_ = RD_NIL;
                sour_cmd_->op_type_ = txservice::BlockOperation::BlockLock;
                return true;
            }
        }
        else
        {
            // error.
            return false;
        }
    }
    else if (curr_step_ == 1)
    {
        // Step 1: This step is blocking operation. Block on the object until
        // the object has at least one element and only return list object size.
        // Commands executed in this step will not be written to the log.
        if (has_forwarded_ &&
            vct_cmd_ptrs_[curr_step_][0] == discard_cmd_.get())
        {
            // expired.
            return false;
        }

        if (sour_cmd_->result_.err_code_ == RD_OK)
        {
            assert(sour_cmd_->result_.ret_ > 0);
            if (sour_cmd_->result_.ret_ > 0)
            {
                sour_cmd_->result_.err_code_ = RD_NIL;
                sour_cmd_->op_type_ = txservice::BlockOperation::PopElement;
                return true;
            }
        }

        return false;
    }
    else if (curr_step_ == 2)
    {
        // step 2: Get the result element from list object. Write log.
        // This step should not be blocked, because we have made sure there is
        // data in the list in the previous step
        if (sour_cmd_->result_.err_code_ == RD_OK)
        {
            assert(sour_cmd_->result_.ret_ > 0);
            if (sour_cmd_->result_.ret_ > 0)
            {
                // The element has poped and goto next step to push it into
                // destination object.
                std::vector<std::string> &elements =
                    std::get<std::vector<std::string>>(
                        sour_cmd_->result_.result_);
                assert(elements.size() == 1);
                dest_cmd_->element_ =
                    EloqString(elements[0].c_str(), elements[0].size());
                return true;
            }
        }

        return false;
    }
    else
    {
        // Not need next step, return false
        return false;
    }
}

BLMPopCommand::BLMPopCommand(std::vector<EloqKey> &&v_key,
                             std::vector<BlockLPopCommand> &&v_cmd,
                             uint64_t ts_expired,
                             bool is_mpop)
    : vct_key_(std::move(v_key)),
      vct_cmd_(std::move(v_cmd)),
      ts_expired_(ts_expired),
      is_mpop_(is_mpop)
{
    discard_cmd_ = std::make_unique<BlockDiscardCommand>();
    assert(vct_key_.size() == vct_cmd_.size());
    vct_key_ptrs_.reserve(vct_key_.size() + 2);
    vct_cmd_ptrs_.reserve(vct_key_.size() + 2);

    std::vector<txservice::TxKey> vct_key;
    std::vector<txservice::TxCommand *> vct_cmd;

    // To visit the objects one by one. If one of them have elements, pop the
    // elements, then break or go to next step to BlockLock all commands until
    // one of related objects insert elements.
    for (size_t i = 0; i < vct_key_.size(); i++)
    {
        vct_key.emplace_back(txservice::TxKey(&vct_key_[i]));
        vct_key_ptrs_.push_back(std::move(vct_key));
        vct_cmd.emplace_back(&vct_cmd_[i]);
        vct_cmd_ptrs_.push_back(std::move(vct_cmd));
    }

    // All commands will be blocked in related ccentry, until one of them insert
    // elements or expired. If insert, add lock to the object and goto next
    // step.
    for (size_t i = 0; i < vct_key_.size(); i++)
    {
        vct_key.emplace_back(txservice::TxKey(&vct_key_[i]));
        vct_cmd.emplace_back(&vct_cmd_[i]);
    }
    vct_key_ptrs_.push_back(std::move(vct_key));
    vct_cmd_ptrs_.push_back(std::move(vct_cmd));

    // Here add empty key and command vector, the related key and command will
    // be added when the previous step finish and at least one element insert
    // into the related object.
    vct_key_ptrs_.push_back(std::move(vct_key));
    vct_cmd_ptrs_.push_back(std::move(vct_cmd));
}

void BLMPopCommand::OutputResult(OutputHandler *reply) const
{
    if (pos_cmd_ >= 0)
    {
        auto &cmd = vct_cmd_[pos_cmd_];
        if (cmd.result_.err_code_ != RD_OK)
        {
            reply->OnError(redis_get_error_messages(cmd.result_.err_code_));
        }
        else
        {
            reply->OnArrayStart(2);
            reply->OnString(vct_key_[pos_cmd_].ToString());

            const auto &vstr =
                std::get<std::vector<std::string>>(cmd.result_.result_);
            if (!is_mpop_)
            {
                /*

                redis> BLMPOP 0 1 mylist LEFT 1
                1) "mylist"
                2) 1) "five"

                redis> BLPOP mylist 0
                1) "mylist"
                2) "five"

                */
                assert(vstr.size() == 1);
                reply->OnString(vstr[0]);
            }
            else
            {
                reply->OnArrayStart(vstr.size());
                for (const auto &str : vstr)
                {
                    reply->OnString(str);
                }
                reply->OnArrayEnd();
            }
            reply->OnArrayEnd();
        }
    }
    else
    {
        reply->OnNil();
    }
}

bool BLMPopCommand::ForwardResult()
{
    assert(curr_step_ == vct_key_.size());
    bool discard = false;
    size_t idx = vct_key_.size();

    for (size_t i = 0; i < vct_cmd_.size(); i++)
    {
        if (vct_cmd_[i].result_.err_code_ == RD_ERR_WRONG_TYPE ||
            (vct_cmd_[i].result_.err_code_ == RD_OK &&
             vct_cmd_[i].result_.ret_ > 0))
        {
            if (pos_cmd_ < 0)
            {
                pos_cmd_ = static_cast<int>(i);
                continue;
            }
        }

        vct_cmd_ptrs_[idx][i] = discard_cmd_.get();
        discard = true;
    }

    has_forwarded_ = true;
    return discard;
}

bool BLMPopCommand::HandleMiddleResult()
{
    if (curr_step_ < vct_key_.size())
    {
        if (vct_cmd_[curr_step_].result_.err_code_ != RD_OK ||
            vct_cmd_[curr_step_].result_.ret_ > 0)
        {
            pos_cmd_ = curr_step_;
            return false;
        }

        if (curr_step_ == vct_key_.size() - 1)
        {
            if (ts_expired_ == 0)
            {
                return false;
            }
            for (size_t i = 0; i < vct_cmd_.size(); i++)
            {
                vct_cmd_[i].result_.err_code_ = RD_NIL;
                vct_cmd_[i].op_type_ = txservice::BlockOperation::BlockLock;
            }
        }
        return true;
    }
    else if (curr_step_ == vct_key_.size())
    {
        if (pos_cmd_ >= 0)
        {
            vct_key_ptrs_.rbegin()->emplace_back(
                txservice::TxKey(&vct_key_[pos_cmd_]));
            vct_cmd_[pos_cmd_].op_type_ = txservice::BlockOperation::PopElement;
            vct_cmd_ptrs_.rbegin()->emplace_back(&vct_cmd_[pos_cmd_]);
            return true;
        }
        else
        {
            return false;
        }
    }
    else
    {
        // Not need next step, return false
        return false;
    }
}

bool HashCommand::CheckTypeMatch(const txservice::TxRecord &obj)
{
    const auto &redis_obj = static_cast<const RedisEloqObject &>(obj);
    return redis_obj.ObjectType() == RedisObjectType::Hash;
}

std::unique_ptr<txservice::TxRecord> HashCommand::CreateObject(
    const std::string *image) const
{
    auto tmp_obj = std::make_unique<RedisHashObject>();
    if (image != nullptr)
    {
        size_t offset = 0;
        tmp_obj->Deserialize(image->data(), offset);
        assert(offset == image->size());
    }
    return tmp_obj;
}

HSetCommand::HSetCommand(const HSetCommand &other)
{
    field_value_pairs_.reserve(other.field_value_pairs_.size());
    for (const auto &[field, value] : other.field_value_pairs_)
    {
        field_value_pairs_.emplace_back(field.Clone(), value.Clone());
    }
}

txservice::ExecResult HSetCommand::ExecuteOn(const txservice::TxObject &object)
{
    RedisHashResult &hash_result = result_;
    if (!CheckTypeMatch(object))
    {
        hash_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const auto &hash_obj = static_cast<const RedisHashObject &>(object);
    bool pass = hash_obj.Execute(*this);
    return pass ? txservice::ExecResult::Write : txservice::ExecResult::Fail;
}

txservice::TxObject *HSetCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    auto &hash_obj = static_cast<RedisHashObject &>(*obj_ptr);
    hash_obj.CommitHset(field_value_pairs_);
    return obj_ptr;
}

void HSetCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::HSET);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));

    uint32_t elements_cnt = field_value_pairs_.size();
    str.append(reinterpret_cast<const char *>(&elements_cnt), sizeof(uint32_t));
    for (const auto &[field, value] : field_value_pairs_)
    {
        uint32_t field_size = field.Length();
        uint32_t value_size = value.Length();
        str.append(reinterpret_cast<const char *>(&field_size),
                   sizeof(uint32_t));

        str.append(reinterpret_cast<const char *>(&value_size),
                   sizeof(uint32_t));

        str.append(reinterpret_cast<const char *>(field.Data()), field_size);
        str.append(reinterpret_cast<const char *>(value.Data()), value_size);
    }
}

void HSetCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    const uint32_t elements_cnt = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);
    field_value_pairs_.reserve(elements_cnt);
    for (uint32_t i = 0; i < elements_cnt; i++)
    {
        uint32_t field_size = *reinterpret_cast<const uint32_t *>(ptr);
        ptr += sizeof(uint32_t);
        uint32_t value_size = *reinterpret_cast<const uint32_t *>(ptr);
        ptr += sizeof(uint32_t);
        field_value_pairs_.emplace_back(
            EloqString(ptr, field_size),
            EloqString(ptr + field_size, value_size));
        ptr += field_size + value_size;
    }
}

void HSetCommand::OutputResult(OutputHandler *reply) const
{
    const auto &hash_result = result_;
    if (hash_result.err_code_ == RD_OK)
    {
        // hmset is similar like hset, except that hmset response with 'OK',
        // while hset response with inserts.
        if (sub_type_ == SubType::HSET)
        {
            reply->OnInt(std::get<int64_t>(hash_result.result_));
        }
        else
        {
            assert(sub_type_ == SubType::HMSET);
            reply->OnStatus(redis_get_error_messages(RD_OK));
        }
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

txservice::ExecResult HLenCommand::ExecuteOn(const txservice::TxObject &object)
{
    RedisHashResult &hash_result = result_;
    if (!CheckTypeMatch(object))
    {
        hash_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const RedisHashObject &hash_obj =
        static_cast<const RedisHashObject &>(object);
    hash_obj.Execute(*this);
    return txservice::ExecResult::Read;
}

void HLenCommand::OutputResult(OutputHandler *reply) const
{
    const auto &hash_result = result_;

    if (hash_result.err_code_ == RD_OK)
    {
        reply->OnInt(std::get<int64_t>(hash_result.result_));
    }
    else if (hash_result.err_code_ == RD_NIL)
    {
        reply->OnInt(0);
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

txservice::ExecResult HStrLenCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    RedisHashResult &hash_result = result_;
    if (!CheckTypeMatch(object))
    {
        hash_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const auto &hash_obj = static_cast<const RedisHashObject &>(object);
    hash_obj.Execute(*this);
    return txservice::ExecResult::Read;
}

void HStrLenCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::HSTRLEN);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
    uint32_t field_size = field_.Length();
    str.append(reinterpret_cast<const char *>(&field_size), sizeof(uint32_t));
    str.append(field_.Data(), field_size);
}

void HStrLenCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    const uint32_t field_size = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);
    field_ = EloqString(ptr, field_size);
}

void HStrLenCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.err_code_ == RD_OK)
    {
        reply->OnInt(std::get<int64_t>(result_.result_));
    }
    else if (result_.err_code_ == RD_NIL)
    {
        reply->OnInt(0);
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

txservice::ExecResult HGetCommand::ExecuteOn(const txservice::TxObject &object)
{
    RedisHashResult &hash_result = result_;
    if (!CheckTypeMatch(object))
    {
        hash_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const RedisHashObject &hash_obj =
        static_cast<const RedisHashObject &>(object);
    hash_obj.Execute(*this);
    return txservice::ExecResult::Read;
}

void HGetCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::HGET);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));

    std::string_view sv = field_.StringView();
    uint32_t len = sv.size();
    str.append(reinterpret_cast<const char *>(&len), sizeof(uint32_t));
    str.append(reinterpret_cast<const char *>(sv.data()), len);
}

void HGetCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    const uint32_t len = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);
    field_ = EloqString(ptr, len);
}

void HGetCommand::OutputResult(OutputHandler *reply) const
{
    const auto &hash_result = result_;
    if (hash_result.err_code_ == RD_OK)
    {
        reply->OnString(std::get<std::string>(hash_result.result_));
    }
    else if (hash_result.err_code_ == RD_NIL)
    {
        reply->OnNil();
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

bool IntOpCommand::CheckTypeMatch(const txservice::TxRecord &obj)
{
    const auto &redis_obj = static_cast<const RedisEloqObject &>(obj);
    return redis_obj.ObjectType() == RedisObjectType::String;
}

std::unique_ptr<txservice::TxRecord> IntOpCommand::CreateObject(
    const std::string *image) const
{
    auto tmp_obj = std::make_unique<RedisStringObject>();
    if (image != nullptr)
    {
        size_t offset = 0;
        tmp_obj->Deserialize(image->data(), offset);
        assert(offset == image->size());
    }
    return tmp_obj;
}

txservice::ExecResult IntOpCommand::ExecuteOn(const txservice::TxObject &object)
{
    const auto &str_obj = static_cast<const RedisStringObject &>(object);
    if (!CheckTypeMatch(object))
    {
        result_.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }
    return str_obj.Execute(*this) ? txservice::ExecResult::Write
                                  : txservice::ExecResult::Fail;
}

txservice::TxObject *IntOpCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    auto &str_obj = static_cast<RedisStringObject &>(*obj_ptr);
    str_obj.CommitIncrDecr(incr_);
    return obj_ptr;
}

void IntOpCommand::Serialize(std::string &str) const
{
    auto cmd_type = static_cast<uint8_t>(RedisCommandType::INCRBY);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
    str.append(reinterpret_cast<const char *>(&incr_), sizeof(int64_t));
}

void IntOpCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    incr_ = *reinterpret_cast<const int64_t *>(ptr);
}

void IntOpCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.err_code_ == RD_OK)
    {
        reply->OnInt(result_.int_val_);
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

bool FloatOpCommand::CheckTypeMatch(const txservice::TxRecord &obj)
{
    const auto &redis_obj = static_cast<const RedisEloqObject &>(obj);
    return redis_obj.ObjectType() == RedisObjectType::String;
}

std::unique_ptr<txservice::TxRecord> FloatOpCommand::CreateObject(
    const std::string *image) const
{
    auto tmp_obj = std::make_unique<RedisStringObject>();
    if (image != nullptr)
    {
        size_t offset = 0;
        tmp_obj->Deserialize(image->data(), offset);
        assert(offset == image->size());
    }
    return tmp_obj;
}

txservice::ExecResult FloatOpCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    const auto &str_obj = static_cast<const RedisStringObject &>(object);
    if (!CheckTypeMatch(object))
    {
        result_.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }
    return str_obj.Execute(*this) ? txservice::ExecResult::Write
                                  : txservice::ExecResult::Fail;
}

txservice::TxObject *FloatOpCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    auto &str_obj = static_cast<RedisStringObject &>(*obj_ptr);
    str_obj.CommitFloatIncr(incr_);
    return obj_ptr;
}

void FloatOpCommand::Serialize(std::string &str) const
{
    auto cmd_type = static_cast<uint8_t>(RedisCommandType::INCRBYFLOAT);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
    str.append(reinterpret_cast<const char *>(&incr_), sizeof(long double));
}

void FloatOpCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    incr_ = *reinterpret_cast<const long double *>(ptr);
}

void FloatOpCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.err_code_ == RD_OK)
    {
        reply->OnString(result_.str_);
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

txservice::ExecResult TypeCommand::ExecuteOn(const txservice::TxObject &object)
{
    const RedisEloqObject &eloq_obj =
        dynamic_cast<const RedisEloqObject &>(object);
    switch (eloq_obj.ObjectType())
    {
    case RedisObjectType::String:
        result_.str_ = "string";
        break;
    case RedisObjectType::List:
        result_.str_ = "list";
        break;
    case RedisObjectType::Hash:
        result_.str_ = "hash";
        break;
    case RedisObjectType::Zset:
        result_.str_ = "zset";
        break;
    case RedisObjectType::Set:
        result_.str_ = "set";
        break;
    default:
        assert(false && "Unknown redis object type");
        break;
    }
    result_.err_code_ = RD_OK;
    return txservice::ExecResult::Read;
}

void TypeCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::TYPE);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
}

void TypeCommand::Deserialize(std::string_view cmd_image)
{
}

void TypeCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.err_code_ == RD_OK)
    {
        reply->OnStatus(result_.str_);
    }
    else if (result_.err_code_ == RD_NIL)
    {
        reply->OnStatus("none");
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

txservice::ExecResult DelCommand::ExecuteOn(const txservice::TxObject &object)
{
    RedisIntResult &int_result = result_;
    int_result.int_val_ = 1;
    int_result.err_code_ = RD_OK;
    return txservice::ExecResult::Write;
}

txservice::TxObject *DelCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    return nullptr;
}

void DelCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::DEL);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
}

void DelCommand::Deserialize(std::string_view cmd_image)
{
}

void DelCommand::OutputResult(OutputHandler *reply) const
{
    assert(false);
}

txservice::ExecResult ExistsCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    RedisIntResult &int_result = result_;
    int_result.int_val_ = cnt_;
    int_result.err_code_ = RD_OK;
    return txservice::ExecResult::Read;
}

txservice::TxObject *ExistsCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    return nullptr;
}

void ExistsCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::EXISTS);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
    str.append(reinterpret_cast<const char *>(&cnt_), sizeof(int32_t));
}

void ExistsCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    cnt_ = *reinterpret_cast<const int32_t *>(ptr);
}

void ExistsCommand::OutputResult(OutputHandler *reply) const
{
    assert(false);
}

std::unique_ptr<txservice::TxRecord> HashSetCommand::CreateObject(
    const std::string *image) const
{
    auto tmp_obj = std::make_unique<RedisHashSetObject>();
    if (image != nullptr)
    {
        size_t offset = 0;
        tmp_obj->Deserialize(image->data(), offset);
        assert(offset == image->size());
    }
    return tmp_obj;
}

bool HashSetCommand::CheckTypeMatch(const txservice::TxRecord &obj)
{
    const auto &redis_obj = static_cast<const RedisEloqObject &>(obj);
    return redis_obj.ObjectType() == RedisObjectType::Set;
}

SAddCommand::SAddCommand(const SAddCommand &rhs) : HashSetCommand(rhs)
{
    vct_paras_.reserve(rhs.vct_paras_.size());
    for (const auto &eloq_str : rhs.vct_paras_)
    {
        vct_paras_.emplace_back(eloq_str.Clone());
    }

    force_remove_add_ = rhs.force_remove_add_;
}

txservice::ExecResult SAddCommand::ExecuteOn(const txservice::TxObject &object)
{
    RedisHashSetResult &set_result = result_;
    if (!force_remove_add_ && !CheckTypeMatch(object))
    {
        set_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    if (force_remove_add_)
    {
        uint64_t len = 0;
        for (const auto &str : vct_paras_)
        {
            len += (sizeof(size_t) + str.Length());
        }

        if (len > MAX_OBJECT_SIZE)
        {
            set_result.err_code_ = RD_ERR_OBJECT_TOO_BIG;
            return txservice::ExecResult::Fail;
        }

        set_result.err_code_ = RD_OK;
        set_result.ret_ = vct_paras_.size();
        return txservice::ExecResult::Write;
    }

    const RedisHashSetObject *set_obj =
        static_cast<const RedisHashSetObject *>(&object);
    return set_obj->Execute(*this) ? txservice::ExecResult::Write
                                   : txservice::ExecResult::Fail;
}

txservice::TxObject *SAddCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    if (force_remove_add_)
    {
        if (vct_paras_.size() == 0)
        {
            return nullptr;
        }

        auto new_obj_uptr = CreateObject(nullptr);
        RedisHashSetObject *set_obj =
            static_cast<RedisHashSetObject *>(new_obj_uptr.get());
        bool empty = set_obj->CommitSAdd(vct_paras_, is_in_the_middle_stage_);
        if (empty)
        {
            return nullptr;
        }
        return static_cast<txservice::TxObject *>(new_obj_uptr.release());
    }
    else
    {
        RedisHashSetObject *set_obj =
            static_cast<RedisHashSetObject *>(obj_ptr);
        bool empty = set_obj->CommitSAdd(vct_paras_, is_in_the_middle_stage_);
        if (empty)
        {
            return nullptr;
        }
        return obj_ptr;
    }
}

void SAddCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::SADD);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
    char ignore_check_type = (force_remove_add_ ? 1 : 0);
    str.append(&ignore_check_type, sizeof(char));

    uint32_t elements_cnt = vct_paras_.size();
    str.append(reinterpret_cast<const char *>(&elements_cnt), sizeof(uint32_t));
    for (const auto &value : vct_paras_)
    {
        uint32_t value_size = value.Length();
        str.append(reinterpret_cast<const char *>(&value_size),
                   sizeof(uint32_t));
        str.append(reinterpret_cast<const char *>(value.Data()), value_size);
    }
}

void SAddCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    force_remove_add_ = (*ptr == 1 ? true : false);
    ptr++;
    const uint32_t elements_cnt = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);
    vct_paras_.reserve(elements_cnt);
    for (uint32_t i = 0; i < elements_cnt; i++)
    {
        uint32_t value_size = *reinterpret_cast<const uint32_t *>(ptr);
        ptr += sizeof(uint32_t);
        vct_paras_.emplace_back(ptr, value_size);
        ptr += value_size;
    }
}

void SAddCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.err_code_ == RD_OK)
    {
        reply->OnInt(result_.ret_);
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

txservice::ExecResult SMembersCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    RedisHashSetResult &set_result = result_;
    if (!CheckTypeMatch(object))
    {
        set_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const RedisHashSetObject &set_obj =
        static_cast<const RedisHashSetObject &>(object);
    set_obj.Execute(*this);
    return txservice::ExecResult::Read;
}

void SMembersCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::SMEMBERS);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
}

void SMembersCommand::Deserialize(std::string_view cmd_image)
{
}

void SMembersCommand::OutputResult(OutputHandler *reply) const
{
    const auto &set_result = result_;

    if (set_result.err_code_ == RD_OK || set_result.err_code_ == RD_NIL)
    {
        reply->OnArrayStart(set_result.string_list_.size());
        for (const auto &str : set_result.string_list_)
        {
            reply->OnString(str.StringView());
        }
        reply->OnArrayEnd();
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

SRemCommand::SRemCommand(const SRemCommand &rhs) : HashSetCommand(rhs)
{
    vct_paras_.reserve(rhs.vct_paras_.size());
    for (const auto &eloq_str : rhs.vct_paras_)
    {
        vct_paras_.emplace_back(eloq_str.Clone());
    }
}

txservice::ExecResult SRemCommand::ExecuteOn(const txservice::TxObject &object)
{
    RedisHashSetResult &set_result = result_;
    if (!CheckTypeMatch(object))
    {
        set_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const RedisHashSetObject &set_obj =
        static_cast<const RedisHashSetObject &>(object);
    return set_obj.Execute(*this) ? txservice::ExecResult::Write
                                  : txservice::ExecResult::Fail;
}

txservice::TxObject *SRemCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    RedisHashSetObject &set_obj = static_cast<RedisHashSetObject &>(*obj_ptr);
    bool empty_after_removal = set_obj.CommitSRem(vct_paras_);
    return empty_after_removal ? nullptr : obj_ptr;
}

void SRemCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::SREM);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));

    uint32_t elements_cnt = vct_paras_.size();
    str.append(reinterpret_cast<const char *>(&elements_cnt), sizeof(uint32_t));
    for (const auto &value : vct_paras_)
    {
        uint32_t value_size = value.Length();
        str.append(reinterpret_cast<const char *>(&value_size),
                   sizeof(uint32_t));
        str.append(reinterpret_cast<const char *>(value.Data()), value_size);
    }
}

void SRemCommand::Deserialize(std::string_view cmd_image)
{
    char *ptr = const_cast<char *>(cmd_image.data());
    const uint32_t elements_cnt = *reinterpret_cast<uint32_t *>(ptr);
    ptr += sizeof(uint32_t);
    vct_paras_.reserve(elements_cnt);
    for (uint32_t i = 0; i < elements_cnt; i++)
    {
        uint32_t value_size = *reinterpret_cast<uint32_t *>(ptr);
        ptr += sizeof(uint32_t);
        vct_paras_.emplace_back(ptr, value_size);
        ptr += value_size;
    }
}

txservice::ExecResult SCardCommand::ExecuteOn(const txservice::TxObject &object)
{
    RedisHashSetResult &set_result = result_;
    if (!CheckTypeMatch(object))
    {
        set_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const RedisHashSetObject &set_obj =
        static_cast<const RedisHashSetObject &>(object);
    set_obj.Execute(*this);
    return txservice::ExecResult::Read;
}

void SCardCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::SCARD);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
}

void SCardCommand::Deserialize(std::string_view cmd_image)
{
}

void SCardCommand::OutputResult(OutputHandler *reply) const
{
    const auto &set_result = result_;

    if (set_result.err_code_ == RD_OK || set_result.err_code_ == RD_NIL)
    {
        reply->OnInt(set_result.ret_);
    }
    else
    {
        reply->OnError(redis_get_error_messages(set_result.err_code_));
    }
}

void SDiffCommand::OutputResult(OutputHandler *reply) const
{
    if (cmds_[0].result_.err_code_ != RD_OK &&
        cmds_[0].result_.err_code_ != RD_NIL)
    {
        reply->OnError(redis_get_error_messages(cmds_[0].result_.err_code_));
        return;
    }

    for (size_t i = 1; i < cmds_.size(); i++)
    {
        if (cmds_[i].result_.err_code_ != RD_OK &&
            cmds_[i].result_.err_code_ != RD_NIL)
        {
            reply->OnError(
                redis_get_error_messages(cmds_[i].result_.err_code_));
            return;
        }
    }

    // This is the target vector to compare with
    const std::vector<EloqString> &target_vec = cmds_[0].result_.string_list_;

    // Create a set of all unique strings from other_vectors
    std::unordered_set<std::string_view> unique_strings_to_compare;
    for (size_t i = 1; i < cmds_.size(); i++)
    {
        for (const EloqString &str : cmds_[i].result_.string_list_)
        {
            unique_strings_to_compare.insert(str.StringView());
        }
    }

    // Find elements only in target_vec (not present in any other vector)
    std::vector<std::string_view> unique_elements;
    for (const EloqString &str : target_vec)
    {
        if (unique_strings_to_compare.count(str.StringView()) == 0)
        {
            unique_elements.push_back(str.StringView());
        }
    }

    reply->OnArrayStart(unique_elements.size());
    for (const auto &str : unique_elements)
    {
        reply->OnString(str);
    }
    reply->OnArrayEnd();
}

bool ZRangeStoreCommand::HandleMiddleResult()
{
    if (cmd_search_->zset_result_.err_code_ != RD_OK &&
        cmd_search_->zset_result_.err_code_ != RD_NIL)
    {
        return false;
    }
    if (cmd_search_->zset_result_.err_code_ == RD_NIL ||
        !std::holds_alternative<std::vector<EloqString>>(
            cmd_search_->zset_result_.result_))
    {
        return true;
    }
    const auto &target_vec =
        std::get<std::vector<EloqString>>(cmd_search_->zset_result_.result_);

    if (target_vec.empty())
    {
        return true;
    }
    assert(target_vec.size() % 2 == 0);
    std::vector<std::pair<double, EloqString>> vp;
    vp.reserve(target_vec.size() / 2);

    for (size_t i = 0; i < target_vec.size(); i += 2)
    {
        double dbl;
        bool b = string2double(target_vec[i + 1].StringView().data(),
                               target_vec[i + 1].StringView().size(),
                               dbl);
        assert(b);
        (void) b;
        vp.push_back(std::pair(dbl, std::move(target_vec[i])));
    }

    cmd_store_->elements_ = std::move(vp);
    cmd_store_->type_ = ZAddCommand::ElementType::vector;

    return true;
}

void ZRangeStoreCommand::OutputResult(OutputHandler *reply) const
{
    if (curr_step_ == 0)
    {
        if (cmd_search_->zset_result_.err_code_ == RD_NIL)
        {
            reply->OnInt(0);
            return;
        }
        reply->OnError(redis_get_error_messages(RD_ERR_WRONG_TYPE));
        return;
    }
    reply->OnInt(std::get<int>(cmd_store_->zset_result_.result_));
}

bool SDiffStoreCommand::HandleMiddleResult()
{
    if (cmds_[0].result_.err_code_ != RD_OK &&
        cmds_[0].result_.err_code_ != RD_NIL)
    {
        return false;
    }

    for (size_t i = 1; i < cmds_.size(); i++)
    {
        if (cmds_[i].result_.err_code_ != RD_OK &&
            cmds_[i].result_.err_code_ != RD_NIL)
        {
            return false;
        }
    }

    // This is the target vector to compare with
    std::vector<EloqString> &target_vec = cmds_[0].result_.string_list_;

    // Create a set of all unique strings from other_vectors
    std::unordered_set<std::string_view> unique_strings_to_compare;
    for (size_t i = 1; i < cmds_.size(); i++)
    {
        for (const EloqString &str : cmds_[i].result_.string_list_)
        {
            unique_strings_to_compare.insert(str.StringView());
        }
    }

    // Find elements only in target_vec (not present in any other vector)
    for (EloqString &str : target_vec)
    {
        if (unique_strings_to_compare.count(str.StringView()) == 0)
        {
            cmd_store_->vct_paras_.push_back(std::move(str));
        }
    }

    return true;
}

void SDiffStoreCommand::OutputResult(OutputHandler *reply) const
{
    if (curr_step_ != 0)
    {
        reply->OnInt(cmd_store_->vct_paras_.size());
    }
    else
    {
        reply->OnError(redis_get_error_messages(RD_ERR_WRONG_TYPE));
    }
}

std::vector<const EloqString *> FindSetResultInter(
    const std::vector<SMembersCommand> &cmds, size_t limit = UINT64_MAX)
{
    // Find the smallest vector (potentially more efficient for lookups)
    size_t smallest_index = 0;
    for (size_t i = 1; i < cmds.size(); ++i)
    {
        if (cmds[i].result_.string_list_.size() <
            cmds[smallest_index].result_.string_list_.size())
        {
            smallest_index = i;
        }
    }

    // Use the smallest vector as the base set and convert to unordered_set
    const std::vector<EloqString> &target_vec =
        cmds[smallest_index].result_.string_list_;
    std::vector<const EloqString *> base_set;
    for (const auto &str : target_vec)
    {
        base_set.push_back(&str);
    }

    // Iterate through remaining vectors (excluding the smallest)
    for (size_t i = 0; i < cmds.size(); ++i)
    {
        if (i == smallest_index)
        {
            continue;  // Skip the smallest vector (already used as base)
        }

        const std::vector<EloqString> &other_vec = cmds[i].result_.string_list_;

        // Build a hash table for quick lookup
        std::unordered_set<std::string_view> other_set;
        for (const auto &str : other_vec)
        {
            other_set.insert(str.StringView());
        }

        // Iterate through base_set and remove elements not found in
        // other_set
        for (auto it = base_set.begin(); it != base_set.end();)
        {
            if (other_set.count((*it)->StringView()) == 0)
            {
                // Not present in all vectors, remove
                it = base_set.erase(it);
                // Early termination if intersection becomes empty
                if (base_set.empty())
                {
                    break;
                }
            }
            else
            {
                ++it;
            }
        }
    }

    if (base_set.size() > limit)
    {
        base_set.erase(base_set.begin() + limit, base_set.end());
    }

    return base_set;
}

std::vector<EloqString *> FindSetResultInter(std::vector<SMembersCommand> &cmds,
                                             size_t limit = UINT64_MAX)
{
    // Find the smallest vector (potentially more efficient for lookups)
    size_t smallest_index = 0;
    for (size_t i = 1; i < cmds.size(); ++i)
    {
        if (cmds[i].result_.string_list_.size() <
            cmds[smallest_index].result_.string_list_.size())
        {
            smallest_index = i;
        }
    }

    // Use the smallest vector as the base set and convert to unordered_set
    std::vector<EloqString> &target_vec =
        cmds[smallest_index].result_.string_list_;
    std::vector<EloqString *> base_set;
    for (auto &str : target_vec)
    {
        base_set.push_back(&str);
    }

    // Iterate through remaining vectors (excluding the smallest)
    for (size_t i = 0; i < cmds.size(); ++i)
    {
        if (i == smallest_index)
        {
            continue;  // Skip the smallest vector (already used as base)
        }

        std::vector<EloqString> &other_vec = cmds[i].result_.string_list_;

        // Build a hash table for quick lookup
        std::unordered_set<std::string_view> other_set;
        for (auto &str : other_vec)
        {
            other_set.insert(str.StringView());
        }

        // Iterate through base_set and remove elements not found in
        // other_set
        for (auto it = base_set.begin(); it != base_set.end();)
        {
            if (other_set.count((*it)->StringView()) == 0)
            {
                // Not present in all vectors, remove
                it = base_set.erase(it);
                // Early termination if intersection becomes empty
                if (base_set.empty())
                {
                    break;
                }
            }
            else
            {
                ++it;
            }
        }
    }

    if (base_set.size() > limit)
    {
        base_set.erase(base_set.begin() + limit, base_set.end());
    }

    return base_set;
}

void SInterCommand::OutputResult(OutputHandler *reply) const
{
    if (cmds_[0].result_.err_code_ != RD_OK &&
        cmds_[0].result_.err_code_ != RD_NIL)
    {
        reply->OnError(redis_get_error_messages(cmds_[0].result_.err_code_));
        return;
    }

    for (size_t i = 1; i < cmds_.size(); i++)
    {
        if (cmds_[i].result_.err_code_ != RD_OK &&
            cmds_[i].result_.err_code_ != RD_NIL)
        {
            reply->OnError(
                redis_get_error_messages(cmds_[i].result_.err_code_));
            return;
        }
    }

    std::vector<const EloqString *> vct = FindSetResultInter(cmds_);

    reply->OnArrayStart(vct.size());
    for (const EloqString *str : vct)
    {
        reply->OnString(str->StringView());
    }
    reply->OnArrayEnd();
}

void SInterCardCommand::OutputResult(OutputHandler *reply) const
{
    if (cmds_[0].result_.err_code_ != RD_OK &&
        cmds_[0].result_.err_code_ != RD_NIL)
    {
        reply->OnError(redis_get_error_messages(cmds_[0].result_.err_code_));
        return;
    }

    for (size_t i = 1; i < cmds_.size(); i++)
    {
        if (cmds_[i].result_.err_code_ != RD_OK &&
            cmds_[i].result_.err_code_ != RD_NIL)
        {
            reply->OnError(
                redis_get_error_messages(cmds_[i].result_.err_code_));
            return;
        }
    }

    std::vector<const EloqString *> vct = FindSetResultInter(cmds_, limit_);

    reply->OnInt(vct.size());
}

bool SInterStoreCommand::HandleMiddleResult()
{
    if (cmds_[0].result_.err_code_ != RD_OK &&
        cmds_[0].result_.err_code_ != RD_NIL)
    {
        return false;
    }

    for (size_t i = 1; i < cmds_.size(); i++)
    {
        if (cmds_[i].result_.err_code_ != RD_OK &&
            cmds_[i].result_.err_code_ != RD_NIL)
        {
            return false;
        }
    }

    std::vector<EloqString *> vct = FindSetResultInter(cmds_);
    cmd_store_->vct_paras_.reserve(vct.size());

    for (EloqString *str : vct)
    {
        cmd_store_->vct_paras_.push_back(std::move(*str));
    }
    return true;
}

void SInterStoreCommand::OutputResult(OutputHandler *reply) const
{
    if (curr_step_ != 0)
    {
        reply->OnInt(cmd_store_->vct_paras_.size());
    }
    else
    {
        reply->OnError(redis_get_error_messages(RD_ERR_WRONG_TYPE));
    }
}

txservice::ExecResult SIsMemberCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    RedisHashSetResult &set_result = result_;
    if (!CheckTypeMatch(object))
    {
        set_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const RedisHashSetObject &set_obj =
        static_cast<const RedisHashSetObject &>(object);
    set_obj.Execute(*this);
    return txservice::ExecResult::Read;
}

void SIsMemberCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::SISMEMBER);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
    uint32_t size = str_member_.Length();
    str.append(reinterpret_cast<const char *>(&size), sizeof(uint32_t));
    str.append(reinterpret_cast<const char *>(str_member_.Data()), size);
}

void SIsMemberCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    uint32_t size = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);
    str_member_ = EloqString(ptr, size);
}

void SIsMemberCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.err_code_ == RD_OK || result_.err_code_ == RD_NIL)
    {
        reply->OnInt(result_.ret_);
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

txservice::ExecResult SMIsMemberCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    RedisHashSetResult &set_result = result_;
    if (!CheckTypeMatch(object))
    {
        set_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const RedisHashSetObject &set_obj =
        static_cast<const RedisHashSetObject &>(object);
    set_obj.Execute(*this);
    return txservice::ExecResult::Read;
}

void SMIsMemberCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::SMISMEMBER);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));

    uint32_t size = members_.size();
    str.append(reinterpret_cast<const char *>(&size), sizeof(uint32_t));

    for (const auto &member : members_)
    {
        uint32_t val_size = member.Length();
        str.append(reinterpret_cast<const char *>(&val_size), sizeof(uint32_t));
        str.append(reinterpret_cast<const char *>(member.Data()), val_size);
    }
}

void SMIsMemberCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    uint32_t size = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);

    for (uint32_t i = 0; i < size; i++)
    {
        uint32_t val_size = *reinterpret_cast<const uint32_t *>(ptr);
        ptr += sizeof(uint32_t);
        members_.emplace_back(ptr, val_size);
        ptr += val_size;
    }
}

void SMIsMemberCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.err_code_ == RD_OK)
    {
        reply->OnArrayStart(result_.string_list_.size());
        for (const auto &str : result_.string_list_)
        {
            reply->OnInt(str.StringView()[0] == '1' ? 1 : 0);
        }
        reply->OnArrayEnd();
    }
    else if (result_.err_code_ == RD_NIL)
    {
        reply->OnArrayStart(members_.size());
        for (size_t i = 0; i < members_.size(); i++)
        {
            reply->OnInt(0);
        }
        reply->OnArrayEnd();
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

bool MSetNxCommand::HandleMiddleResult()
{
    for (const auto &cmd : exist_cmds_)
    {
        assert(cmd.result_.err_code_ == RD_OK ||
               cmd.result_.err_code_ == RD_NIL);

        // Some of key have already exist. We don't need to execute set command.
        if (cmd.result_.int_val_ != 0)
        {
            return false;
        }
    }

    return true;
}

void MSetNxCommand::OutputResult(OutputHandler *reply) const
{
    if (curr_step_ == 0)
    {
        reply->OnInt(0);
    }
    else
    {
        reply->OnInt(1);
    }
}

bool SMoveCommand::HandleMiddleResult()
{
    if (cmd_src_->result_.err_code_ == RD_ERR_WRONG_TYPE)
    {
        return false;
    }

    if (cmd_src_->result_.ret_ == 1)
    {
        // the EloqString in cmd_src_->vct_paras_ is of string_view type
        // pointing to the string in SMOVE command, so the string_view will
        // always be valid until the end of transaction.
        cmd_dst_->vct_paras_.emplace_back(cmd_src_->vct_paras_[0].StringView());
    }

    return true;
}

void SMoveCommand::OutputResult(OutputHandler *reply) const
{
    if (curr_step_ != 0)
    {
        if (cmd_dst_->result_.err_code_ == RD_OK)
        {
            reply->OnInt(cmd_dst_->result_.ret_);
        }
        else
        {
            reply->OnError(
                redis_get_error_messages(cmd_dst_->result_.err_code_));
        }
    }
    else
    {
        assert(cmd_src_->result_.err_code_ != RD_OK);
        reply->OnError(redis_get_error_messages(cmd_src_->result_.err_code_));
    }
}

bool LMoveCommand::HandleMiddleResult()
{
    if (cmd_src_->result_.err_code_ == RD_ERR_WRONG_TYPE)
    {
        return false;
    }

    if (cmd_src_->result_.ret_ == 1)
    {
        cmd_dst_->element_ =
            std::move(std::get<EloqString>(cmd_src_->result_.result_));
        return true;
    }
    else
    {
        // src list does not exist(list size == 0)
        assert(cmd_src_->result_.ret_ == 0);
        return false;
    }
}

void LMoveCommand::OutputResult(OutputHandler *reply) const
{
    if (curr_step_ == 0)
    {
        assert(cmd_src_->result_.err_code_ != RD_OK);
        if (cmd_src_->result_.err_code_ == RD_NIL)
        {
            reply->OnNil();
        }
        else
        {
            reply->OnError(
                redis_get_error_messages(cmd_src_->result_.err_code_));
        }
    }
    else
    {
        if (cmd_dst_->result_.err_code_ == RD_OK)
        {
            assert(cmd_dst_->result_.ret_ == 1);
            reply->OnString(
                std::get<EloqString>(cmd_dst_->result_.result_).StringView());
        }
        else
        {
            reply->OnError(
                redis_get_error_messages(cmd_dst_->result_.err_code_));
        }
    }
}

void SUnionCommand::OutputResult(OutputHandler *reply) const
{
    std::unordered_set<std::string_view> set;
    set.reserve(cmds_.size());

    for (const SMembersCommand &cmd : cmds_)
    {
        if (cmd.result_.err_code_ == RD_ERR_WRONG_TYPE)
        {
            reply->OnError(redis_get_error_messages(cmd.result_.err_code_));
            return;
        }
        if (cmd.result_.ret_ == 0)
        {
            continue;
        }

        auto &slist = cmd.result_.string_list_;
        for (const EloqString &str : slist)
        {
            set.insert(str.StringView());
        }
    }

    reply->OnArrayStart(set.size());
    for (auto &str : set)
    {
        reply->OnString(str);
    }
    reply->OnArrayEnd();
}

bool SUnionStoreCommand::HandleMiddleResult()
{
    std::unordered_set<EloqString> union_set;
    for (SMembersCommand &cmd : cmds_)
    {
        if (cmd.result_.err_code_ == RD_ERR_WRONG_TYPE)
        {
            return false;
        }

        if (cmd.result_.ret_ == 0)
        {
            continue;
        }

        auto &slist = cmd.result_.string_list_;
        for (EloqString &str : slist)
        {
            union_set.insert(std::move(str));
        }
    }

    cmd_store_->vct_paras_.reserve(union_set.size());
    for (auto &str : union_set)
    {
        cmd_store_->vct_paras_.push_back(std::move(str));
    }

    return true;
}

void SUnionStoreCommand::OutputResult(OutputHandler *reply) const
{
    if (curr_step_ != 0)
    {
        reply->OnInt(cmd_store_->vct_paras_.size());
    }
    else
    {
        reply->OnError(redis_get_error_messages(RD_ERR_WRONG_TYPE));
    }
}

txservice::ExecResult SRandMemberCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    RedisHashSetResult &set_result = result_;
    if (!CheckTypeMatch(object))
    {
        set_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const RedisHashSetObject &set_obj =
        static_cast<const RedisHashSetObject &>(object);
    set_obj.Execute(*this);
    return txservice::ExecResult::Read;
}

void SRandMemberCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::SRANDMEMBER);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
    str.append(reinterpret_cast<const char *>(&count_), sizeof(int64_t));
}

void SRandMemberCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    count_ = *reinterpret_cast<const int64_t *>(ptr);
}

void SRandMemberCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.err_code_ == RD_OK)
    {
        if (count_provided_)
        {
            reply->OnArrayStart(result_.string_list_.size());
            for (const auto &str : result_.string_list_)
            {
                reply->OnString(str.StringView());
            }
            reply->OnArrayEnd();
        }
        else
        {
            reply->OnString(result_.string_list_[0].StringView());
        }
    }
    else if (result_.err_code_ == RD_NIL)
    {
        if (count_provided_)
        {
            reply->OnArrayStart(0);
            reply->OnArrayEnd();
        }
        else
        {
            reply->OnNil();
        }
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

txservice::ExecResult SPopCommand::ExecuteOn(const txservice::TxObject &object)
{
    RedisHashSetResult &set_result = result_;
    if (!CheckTypeMatch(object))
    {
        set_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const RedisHashSetObject *set_obj =
        static_cast<const RedisHashSetObject *>(&object);

    return set_obj->Execute(*this) ? txservice::ExecResult::Write
                                   : txservice::ExecResult::Fail;
}

txservice::TxObject *SPopCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    RedisHashSetObject *set_obj = static_cast<RedisHashSetObject *>(obj_ptr);
    bool empty_after_removal = set_obj->CommitSPop(result_.string_list_);
    return empty_after_removal ? nullptr : obj_ptr;
}

void SPopCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::SPOP);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
    str.append(reinterpret_cast<const char *>(&count_), sizeof(int64_t));

    // If result_.string_list_ is empty, it is used for remote request, else it
    // is used for log request. For idempotence, spop command should bookkeep
    // popped members in log.
    uint32_t pops = result_.string_list_.size();
    str.append(reinterpret_cast<const char *>(&pops), sizeof(pops));
    for (const EloqString &member : result_.string_list_)
    {
        uint32_t member_len = member.Length();
        str.append(reinterpret_cast<const char *>(&member_len),
                   sizeof(member_len));
        str.append(member.StringView());
    }
}

void SPopCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    count_ = *reinterpret_cast<const int64_t *>(ptr);
    ptr += sizeof(count_);

    uint32_t pops = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(pops);
    result_.string_list_.reserve(pops);
    for (size_t i = 0; i < pops; i++)
    {
        uint32_t member_len = *reinterpret_cast<const uint32_t *>(ptr);
        ptr += sizeof(member_len);
        result_.string_list_.emplace_back(ptr, member_len);
        ptr += member_len;
    }
}

void SPopCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.err_code_ == RD_OK)
    {
        if (count_ != -1)
        {
            reply->OnArrayStart(result_.string_list_.size());
            for (const auto &str : result_.string_list_)
            {
                reply->OnString(str.StringView());
            }
            reply->OnArrayEnd();
        }
        else
        {
            reply->OnString(result_.string_list_[0].StringView());
        }
    }
    else if (result_.err_code_ == RD_NIL)
    {
        if (count_ != -1)
        {
            reply->OnArrayStart(0);
            reply->OnArrayEnd();
        }
        else
        {
            reply->OnNil();
        }
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

txservice::ExecResult SZScanCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    RedisSetScanResult &set_scan_result = set_scan_result_;
    const auto &redis_obj = static_cast<const RedisEloqObject &>(object);

    if (redis_obj.ObjectType() == RedisObjectType::Set)
    {
        const RedisHashSetObject &set_obj =
            static_cast<const RedisHashSetObject &>(object);
        set_obj.Execute(*this);
        return txservice::ExecResult::Read;
    }
    else if (redis_obj.ObjectType() == RedisObjectType::Zset ||
             redis_obj.ObjectType() == RedisObjectType::TTLZset)
    {
        const RedisZsetObject &zset_obj =
            static_cast<const RedisZsetObject &>(object);
        zset_obj.Execute(*this);
        return txservice::ExecResult::Read;
    }
    else
    {
        set_scan_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }
}

void SZScanCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::SZSCAN);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
    str.append(reinterpret_cast<const char *>(&withscores_), sizeof(bool));
}

void SZScanCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    withscores_ = *reinterpret_cast<const bool *>(ptr);
    ptr += sizeof(bool);
}

void SZScanCommand::OutputResult(OutputHandler *reply) const
{
    assert(false && "Dead branch");
    reply->OnNil();
}

txservice::ExecResult SScanCommand::ExecuteOn(const txservice::TxObject &object)
{
    RedisHashSetResult &set_result = result_;
    if (!CheckTypeMatch(object))
    {
        set_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const RedisHashSetObject &set_obj =
        static_cast<const RedisHashSetObject &>(object);
    set_obj.Execute(*this);
    return txservice::ExecResult::Read;
}

void SScanCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::SSCAN);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));

    str.append(reinterpret_cast<const char *>(&cursor_), sizeof(int64_t));
    str.append(reinterpret_cast<const char *>(&count_), sizeof(uint64_t));

    str.append(reinterpret_cast<const char *>(&match_), sizeof(bool));
    uint32_t sz = static_cast<uint32_t>(pattern_.Length());
    str.append(reinterpret_cast<const char *>(&sz), sizeof(uint32_t));
    str.append(pattern_.Data(), sz);
}

void SScanCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    cursor_ = *reinterpret_cast<const int64_t *>(ptr);
    ptr += sizeof(int64_t);
    count_ = *reinterpret_cast<const uint64_t *>(ptr);
    ptr += sizeof(uint64_t);

    match_ = *reinterpret_cast<const bool *>(ptr);
    ptr += sizeof(bool);
    uint32_t sz = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);
    pattern_ = EloqString(ptr, sz);
}

void SScanCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.err_code_ == RD_OK)
    {
        reply->OnArrayStart(2);
        auto it = result_.string_list_.begin();
        reply->OnString(it->StringView());  // cursor
        reply->OnArrayStart(result_.string_list_.size() - 1);
        for (it++; it != result_.string_list_.end(); it++)
        {
            reply->OnString(it->StringView());
        }
        reply->OnArrayEnd();

        reply->OnArrayEnd();
    }
    else if (result_.err_code_ == RD_NIL)
    {
        reply->OnArrayStart(2);

        reply->OnString("0");

        reply->OnArrayStart(0);
        reply->OnArrayEnd();

        reply->OnArrayEnd();
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

BitOpCommand::BitOpCommand(std::vector<EloqKey> &&s_keys,
                           std::vector<GetCommand> &&s_cmds,
                           std::unique_ptr<EloqKey> &&dst_key,
                           std::unique_ptr<SetCommand> &&set_cmd,
                           std::unique_ptr<DelCommand> &&del_cmd,
                           OpType op_type)
    : s_keys_(std::move(s_keys)),
      s_cmds_(std::move(s_cmds)),
      dst_key_(std::move(dst_key)),
      set_cmd_(std::move(set_cmd)),
      del_cmd_(std::move(del_cmd)),
      op_type_(op_type)
{
    assert(s_keys_.size() == s_cmds_.size());
    vct_key_ptrs_.resize(2);
    vct_key_ptrs_[0].reserve(s_keys_.size());
    vct_cmd_ptrs_.resize(2);
    vct_cmd_ptrs_[0].reserve(s_keys_.size());

    for (const auto &key : s_keys_)
    {
        vct_key_ptrs_[0].emplace_back(&key);
    }
    for (auto &cmd : s_cmds_)
    {
        vct_cmd_ptrs_[0].push_back(&cmd);
    }

    vct_key_ptrs_[1].emplace_back(dst_key_.get());
    // vct_cmd_ptrs_[1].emplace_back(d_cmd_.get());
}

void BitOpCommand::OutputResult(OutputHandler *reply) const
{
    if (curr_step_ == 0)
    {
        assert(err_pos_ >= 0);
        reply->OnError(
            redis_get_error_messages(s_cmds_[err_pos_].result_.err_code_));
        return;
    }
    else
    {
        if (to_del_)
        {
            auto &result = del_cmd_->result_;
            if (result.err_code_ == RD_OK)
            {
                reply->OnInt(str_dest_.size());
            }
            else if (result.err_code_ == RD_NIL)
            {
                reply->OnInt(0);
            }
            else
            {
                reply->OnError(redis_get_error_messages(result.err_code_));
            }
        }
        else
        {
            auto &result = set_cmd_->result_;
            if (result.err_code_ == RD_OK)
            {
                reply->OnInt(str_dest_.size());
            }
            else
            {
                reply->OnError(redis_get_error_messages(result.err_code_));
            }
        }
    }
}

bool BitOpCommand::HandleMiddleResult()
{
    assert(curr_step_ == 0);

    size_t len = 0;
    for (size_t i = 0; i < s_cmds_.size(); i++)
    {
        GetCommand &cmd = s_cmds_[i];
        if (cmd.result_.err_code_ == RD_NIL)
        {
            continue;
        }
        else if (cmd.result_.err_code_ != RD_OK)
        {
            err_pos_ = i;
            return false;
        }

        if (len < cmd.result_.str_.size())
        {
            len = cmd.result_.str_.size();
        }
    }

    str_dest_.resize(len);
    char *pv = str_dest_.data();
    if (op_type_ == OpType::Not)
    {
        const auto &str = s_cmds_[0].result_.str_;
        const char *ps = str.c_str();
        for (size_t i = 0; i < str.size(); i++)
        {
            pv[i] = ~ps[i];
        }
    }
    else
    {
        const auto &str = s_cmds_[0].result_.str_;
        memcpy(pv, str.c_str(), str.size());
        memset(pv + str.size(), 0, len - str.size());
    }

    for (size_t i = 1; i < s_cmds_.size(); i++)
    {
        const auto &str = s_cmds_[i].result_.str_;
        for (size_t j = 0; j < str.size(); j++)
        {
            switch (op_type_)
            {
            case OpType::And:
                pv[j] = pv[j] & str[j];
                break;
            case OpType::Or:
                pv[j] = pv[j] | str[j];
                break;
            case OpType::Xor:
                pv[j] = pv[j] ^ str[j];
                break;
            default:
                assert(false);
            }
        }

        for (size_t j = str.size(); j < len; j++)
        {
            switch (op_type_)
            {
            case OpType::And:
                pv[j] = 0;
                break;
            case OpType::Or:
                break;
            case OpType::Xor:
                pv[j] = pv[j] ^ 0;
                break;
            default:
                assert(false);
            }
        }
    }

    if (str_dest_.empty())
    {
        to_del_ = true;
        vct_cmd_ptrs_[1].emplace_back(del_cmd_.get());
    }
    else
    {
        vct_cmd_ptrs_[1].emplace_back(set_cmd_.get());
        set_cmd_->value_ = EloqString(std::string_view(str_dest_));
    }
    return true;
}

bool BitOpCommand::IsPassed() const
{
    if (curr_step_ == 0)
    {
        return err_pos_ >= 0;
    }
    else
    {
        return (to_del_ ? del_cmd_->result_.err_code_
                        : set_cmd_->result_.err_code_) == RD_OK;
    }
}

std::unique_ptr<txservice::TxRecord> RecoverObjectCommand::CreateObject(
    const std::string *image) const
{
    const char *ptr = result_.data();
    const RedisObjectType obj_type = static_cast<const RedisObjectType>(*ptr);

    std::unique_ptr<txservice::TxRecord> obj = nullptr;
    switch (obj_type)
    {
    case RedisObjectType::Unknown:
    case RedisObjectType::String:
    case RedisObjectType::List:
    case RedisObjectType::Hash:
    case RedisObjectType::Del:
    case RedisObjectType::Zset:
    case RedisObjectType::Set:
        assert(false);
        break;
    case RedisObjectType::TTLString:
    {
        obj = std::make_unique<RedisStringTTLObject>();
        break;
    }
    case RedisObjectType::TTLList:
    {
        obj = std::make_unique<RedisListTTLObject>();
        break;
    }
    case RedisObjectType::TTLHash:
    {
        obj = std::make_unique<RedisHashTTLObject>();
        break;
    }
    case RedisObjectType::TTLZset:
    {
        obj = std::make_unique<RedisZsetTTLObject>();
        break;
    }
    case RedisObjectType::TTLSet:
    {
        obj = std::make_unique<RedisHashSetTTLObject>();
        break;
    }
    default:
    {
        assert(false);
        break;
    }
    }

    return obj;
}

txservice::TxObject *RecoverObjectCommand::CommitOn(
    txservice::TxObject *obj_ptr)
{
    RedisEloqObject *obj = static_cast<RedisEloqObject *>(obj_ptr);
    RedisObjectType obj_type = obj->ObjectType();
    size_t offset = 0;

    txservice::TxObject *result_ttl_obj = obj_ptr;

    switch (obj_type)
    {
    case RedisObjectType::Unknown:
    {
        assert(false);
        break;
    }
    case RedisObjectType::String:
    {
        RedisStringTTLObject *str_ttl_obj =
            dynamic_cast<RedisStringTTLObject *>(obj);

        if (str_ttl_obj == nullptr)
        {
            // Create Object directly since the old object here could be of any
            // type(list, zset, string without ttl, ...)
            obj_ptr = static_cast<TxObject *>(CreateObject(nullptr).release());
            str_ttl_obj = static_cast<RedisStringTTLObject *>(obj_ptr);
        }

        assert(str_ttl_obj->HasTTL());
        str_ttl_obj->Deserialize(result_.data(), offset);
        result_ttl_obj = str_ttl_obj;
        DLOG(INFO) << "RecoverObjectCommand::CommitOn ttl string";
        break;
    }
    case RedisObjectType::List:
    {
        RedisListTTLObject *list_obj = static_cast<RedisListTTLObject *>(obj);
        assert(list_obj->HasTTL());
        list_obj->Deserialize(result_.data(), offset);
        result_ttl_obj = list_obj;
        DLOG(INFO) << "RecoverObjectCommand::CommitOn ttl list";
        break;
    }
    case RedisObjectType::Hash:
    {
        RedisHashTTLObject *hash_obj = static_cast<RedisHashTTLObject *>(obj);
        assert(hash_obj->HasTTL());
        hash_obj->Deserialize(result_.data(), offset);
        result_ttl_obj = hash_obj;
        DLOG(INFO) << "RecoverObjectCommand::CommitOn ttl hash";
        break;
    }
    case RedisObjectType::Del:
    {
        assert(false);
        break;
    }
    case RedisObjectType::Zset:
    {
        RedisZsetTTLObject *zset_obj = static_cast<RedisZsetTTLObject *>(obj);
        assert(zset_obj->HasTTL());
        zset_obj->Deserialize(result_.data(), offset);
        result_ttl_obj = zset_obj;
        DLOG(INFO) << "RecoverObjectCommand::CommitOn ttl zset";
        break;
    }
    case RedisObjectType::Set:
    {
        RedisHashSetTTLObject *hashset_obj =
            static_cast<RedisHashSetTTLObject *>(obj);
        assert(hashset_obj->HasTTL());
        hashset_obj->Deserialize(result_.data(), offset);
        result_ttl_obj = hashset_obj;
        DLOG(INFO) << "RecoverObjectCommand::CommitOn ttl set";
        break;
    }
    case RedisObjectType::TTLString:
    case RedisObjectType::TTLList:
    case RedisObjectType::TTLHash:
    case RedisObjectType::TTLZset:
    case RedisObjectType::TTLSet:
    default:
    {
        assert(false);
        break;
    }
    }

    return result_ttl_obj;
}

std::string stringToHex(const std::string &str)
{
    std::stringstream ss;

    for (unsigned char c : str)
    {
        ss << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<int>(c) << " ";
    }
    return ss.str();
}

void RecoverObjectCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::RECOVER);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
    uint32_t len = result_.size();
    auto *len_ptr = reinterpret_cast<const char *>(&len);
    str.append(len_ptr, sizeof(uint32_t));
    str.append(result_.data(), len);
    DLOG(INFO) << "RecoverObjectCommand::Serialize result len: " << len
               << " result: " << stringToHex(result_);
}

void RecoverObjectCommand::Deserialize(std::string_view cmd_img)
{
    const char *ptr = cmd_img.data();
    uint32_t len = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);
    result_ = std::string(ptr, len);
    DLOG(INFO) << "RecoverObjectCommand::Deserialize result len: " << len
               << " size: " << result_.size()
               << " result: " << stringToHex(result_);
}

std::unique_ptr<TxCommand> GetExCommand::Clone()
{
    return std::make_unique<GetExCommand>(expire_ts_, is_persist_);
}

txservice::ExecResult GetExCommand::ExecuteOn(const txservice::TxObject &object)
{
    const auto &str_obj = static_cast<const RedisStringObject &>(object);
    RedisStringResult &str_result = result_;

    if (!CheckTypeMatch(object))
    {
        str_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    str_obj.Execute(*this);

    // if expire ts and persist are not given or trying to persist a
    // obj without ttl
    if ((expire_ts_ == UINT64_MAX && !is_persist_) ||
        (is_persist_ && !str_obj.HasTTL()))
    {
        return txservice::ExecResult::Read;
    }

    recover_ttl_obj_cmd_ = std::make_unique<RecoverObjectCommand>();
    str_obj.Serialize(recover_ttl_obj_cmd_->result_);
    result_.err_code_ = RD_OK;
    return txservice::ExecResult::Write;
}

txservice::TxObject *GetExCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    assert(obj_ptr != nullptr);
    auto *str_obj_ptr = static_cast<RedisStringObject *>(obj_ptr);
    txservice::TxObject *obj = obj_ptr;

    if (expire_ts_ < UINT64_MAX)
    {
        if (str_obj_ptr->HasTTL())
        {
            str_obj_ptr->SetTTL(expire_ts_);
        }
        else
        {
            obj = static_cast<txservice::TxObject *>(
                str_obj_ptr->AddTTL(expire_ts_).release());
        }
    }
    else if (is_persist_)
    {
        auto *str_ttl_obj = static_cast<RedisStringTTLObject *>(obj_ptr);
        obj = static_cast<txservice::TxObject *>(
            str_ttl_obj->RemoveTTL().release());
    }
    else
    {
        assert(false);
    }

    return obj;
}

void GetExCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::GETEX);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));

    auto *expire_ts_ptr = reinterpret_cast<const char *>(&expire_ts_);
    str.append(expire_ts_ptr, sizeof(uint64_t));

    auto *is_persist_ptr = reinterpret_cast<const char *>(&is_persist_);
    str.append(is_persist_ptr, sizeof(bool));
}

void GetExCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();

    expire_ts_ = *reinterpret_cast<const uint64_t *>(ptr);
    ptr += sizeof(uint64_t);

    is_persist_ = *reinterpret_cast<const bool *>(ptr);
    ptr += sizeof(bool);
}

std::unique_ptr<TxCommand> ExpireCommand::Clone()
{
    return std::make_unique<ExpireCommand>(expire_ts_, flags_);
}

txservice::ExecResult ExpireCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    // If this command will reset the ttl, keep the object in log through the
    // RecoverObjectCommand
    const RedisEloqObject &obj = static_cast<const RedisEloqObject &>(object);
    bool will_set_ttl = false;

    if (flags_ != 0)
    {
        if (obj.HasTTL())
        {
            uint64_t old_expire_ts = obj.GetTTL();
            // if not expired
            if (old_expire_ts >
                txservice::LocalCcShards::ClockTsInMillseconds())
            {
                if ((flags_ & EXPIRE_XX) != 0)
                {
                    if ((flags_ & EXPIRE_LT) != 0)
                    {
                        if (expire_ts_ < old_expire_ts)
                        {
                            will_set_ttl = true;
                        }
                        else
                        {
                            will_set_ttl = false;
                        }
                    }
                    else
                    {
                        will_set_ttl = true;
                    }
                }
                else if ((flags_ & EXPIRE_GT) != 0 &&
                         expire_ts_ > old_expire_ts)
                {
                    will_set_ttl = true;
                }
                else if ((flags_ & EXPIRE_LT) != 0 &&
                         expire_ts_ < old_expire_ts)
                {
                    will_set_ttl = true;
                }
            }
        }
        else
        {
            if ((flags_ & EXPIRE_NX) != 0)
            {
                will_set_ttl = true;
            }
            else if ((flags_ & EXPIRE_LT) != 0)
            {
                if (!((flags_ & EXPIRE_XX) != 0))
                {
                    will_set_ttl = true;
                }
            }
        }
    }
    else
    {
        will_set_ttl = true;
    }

    if (!will_set_ttl)
    {
        result_.err_code_ = RD_NIL;
        return txservice::ExecResult::Read;
    }

    RedisObjectType obj_type = obj.ObjectType();

    bool ttl_reset = false;
    switch (obj_type)
    {
    case RedisObjectType::Unknown:
        assert(false);
        break;
    case RedisObjectType::String:
    case RedisObjectType::List:
    case RedisObjectType::Hash:
    {
        if (obj.HasTTL())
        {
            ttl_reset = true;
        }
        break;
    }
    case RedisObjectType::Del:
        assert(false);
        break;
    case RedisObjectType::Zset:
    case RedisObjectType::Set:
    {
        if (obj.HasTTL())
        {
            ttl_reset = true;
        }
        break;
    }
    case RedisObjectType::TTLString:
    case RedisObjectType::TTLList:
    case RedisObjectType::TTLHash:
    case RedisObjectType::TTLZset:
    case RedisObjectType::TTLSet:
        // never exposr ttl type from obj->ObjectType()
    default:
    {
        assert(false);
        break;
    }
    }

    if (ttl_reset == true)
    {
        recover_ttl_obj_cmd_ = std::make_unique<RecoverObjectCommand>();
        obj.Serialize(recover_ttl_obj_cmd_->result_);
    }

    result_.err_code_ = RD_OK;
    return txservice::ExecResult::Write;
}

txservice::TxObject *ExpireCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    assert(obj_ptr != nullptr);
    RedisEloqObject *obj = static_cast<RedisEloqObject *>(obj_ptr);
    RedisObjectType obj_type = obj->ObjectType();
    txservice::TxObject *result_ttl_obj = obj_ptr;

    switch (obj_type)
    {
    case RedisObjectType::Unknown:
    {
        assert(false);
        break;
    }
    case RedisObjectType::String:
    {
        RedisStringObject *str_obj = static_cast<RedisStringObject *>(obj);
        result_ttl_obj = static_cast<txservice::TxObject *>(
            str_obj->AddTTL(expire_ts_).release());
        break;
    }
    case RedisObjectType::List:
    {
        RedisListObject *list_obj = static_cast<RedisListObject *>(obj);
        result_ttl_obj = static_cast<txservice::TxObject *>(
            list_obj->AddTTL(expire_ts_).release());
        break;
    }
    case RedisObjectType::Hash:
    {
        RedisHashObject *hash_obj = static_cast<RedisHashObject *>(obj);
        result_ttl_obj = static_cast<txservice::TxObject *>(
            hash_obj->AddTTL(expire_ts_).release());
        break;
    }
    case RedisObjectType::Del:
        assert(false);
        break;
    case RedisObjectType::Zset:
    {
        RedisZsetObject *zset_obj = static_cast<RedisZsetObject *>(obj);
        result_ttl_obj = static_cast<txservice::TxObject *>(
            zset_obj->AddTTL(expire_ts_).release());
        break;
    }
    case RedisObjectType::Set:
    {
        RedisHashSetObject *hashset_obj =
            static_cast<RedisHashSetObject *>(obj);
        result_ttl_obj = static_cast<txservice::TxObject *>(
            hashset_obj->AddTTL(expire_ts_).release());
        break;
    }
    case RedisObjectType::TTLString:
    {
        RedisStringTTLObject *str_obj =
            static_cast<RedisStringTTLObject *>(obj);
        str_obj->SetTTL(expire_ts_);
        result_ttl_obj = str_obj;
        break;
    }
    case RedisObjectType::TTLList:
    {
        RedisListTTLObject *list_obj = static_cast<RedisListTTLObject *>(obj);
        list_obj->SetTTL(expire_ts_);
        result_ttl_obj = list_obj;
        break;
    }
    case RedisObjectType::TTLHash:
    {
        RedisHashTTLObject *hash_obj = static_cast<RedisHashTTLObject *>(obj);
        hash_obj->SetTTL(expire_ts_);
        result_ttl_obj = hash_obj;
        break;
    }
    case RedisObjectType::TTLZset:
    {
        RedisZsetTTLObject *zset_obj = static_cast<RedisZsetTTLObject *>(obj);
        zset_obj->SetTTL(expire_ts_);
        result_ttl_obj = zset_obj;
        break;
    }
    case RedisObjectType::TTLSet:
    {
        RedisHashSetTTLObject *hashset_obj =
            static_cast<RedisHashSetTTLObject *>(obj);
        hashset_obj->SetTTL(expire_ts_);
        result_ttl_obj = hashset_obj;
        break;
    }
    default:
    {
        assert(false);
        break;
    }
    }

    return result_ttl_obj;
}

void ExpireCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::EXPIRE);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));

    auto *expire_ts_ptr = reinterpret_cast<const char *>(&expire_ts_);
    str.append(expire_ts_ptr, sizeof(uint64_t));

    auto *flags_ptr = reinterpret_cast<const char *>(&flags_);
    str.append(flags_ptr, sizeof(uint8_t));
}

void ExpireCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();

    expire_ts_ = *reinterpret_cast<const uint64_t *>(ptr);
    ptr += sizeof(uint64_t);

    flags_ = *reinterpret_cast<const int8_t *>(ptr);
    ptr += sizeof(int8_t);
}

void ExpireCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.err_code_ == RD_OK)
    {
        reply->OnInt(1);
    }
    else if (result_.err_code_ == RD_NIL)
    {
        reply->OnInt(0);
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

std::unique_ptr<TxCommand> TTLCommand::Clone()
{
    return std::make_unique<TTLCommand>(
        is_pttl_, is_expire_time_, is_pexpire_time_);
}

txservice::ExecResult TTLCommand::ExecuteOn(const txservice::TxObject &object)
{
    if (object.HasTTL())
    {
        uint64_t expire_ts = object.GetTTL();
        // ttl in unit of millisecond
        uint64_t ttl =
            expire_ts - txservice::LocalCcShards::ClockTsInMillseconds();
        if (is_pttl_)
        {
            // ttl is ttl in millseconds
        }
        else if (is_expire_time_)
        {
            double expire_ts_secs = static_cast<double>(expire_ts) / 1000;
            ttl = static_cast<int64_t>(std::round(expire_ts_secs));
        }
        else if (is_pexpire_time_)
        {
            ttl = expire_ts;
        }
        else  // is ttl
        {
            double ttl_secs = static_cast<double>(ttl) / 1000;
            ttl = static_cast<int64_t>(std::round(ttl_secs));
        }
        result_.int_val_ = ttl;
        result_.err_code_ = RD_OK;
    }
    else
    {
        result_.int_val_ = UINT64_MAX;
        result_.err_code_ = RD_ERR_TTL_NOT_EXIST_ON_KEY;
    }

    return txservice::ExecResult::Read;
}

void TTLCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::TTL);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
    str.append(reinterpret_cast<const char *>(&is_pttl_), sizeof(bool));
    str.append(reinterpret_cast<const char *>(&is_expire_time_), sizeof(bool));
    str.append(reinterpret_cast<const char *>(&is_pexpire_time_), sizeof(bool));
}

void TTLCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    is_pttl_ = *reinterpret_cast<const bool *>(ptr);
    ptr += sizeof(bool);
    is_expire_time_ = *reinterpret_cast<const bool *>(ptr);
    ptr += sizeof(bool);
    is_pexpire_time_ = *reinterpret_cast<const bool *>(ptr);
    ptr += sizeof(bool);
}

void TTLCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.err_code_ == RD_OK)
    {
        reply->OnInt(result_.int_val_);
    }
    else if (result_.err_code_ == RD_ERR_TTL_NOT_EXIST_ON_KEY)
    {
        reply->OnInt(-1);
    }
    else if (result_.err_code_ == RD_NIL)
    {
        reply->OnInt(-2);
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

std::unique_ptr<TxCommand> PersistCommand::Clone()
{
    return std::make_unique<PersistCommand>();
}

txservice::ExecResult PersistCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    // If this command will reset the ttl, keep the object in log through the
    // RecoverObjectCommand
    const RedisEloqObject &obj = static_cast<const RedisEloqObject &>(object);
    if (!obj.HasTTL())
    {
        result_.err_code_ = RD_NIL;
        return txservice::ExecResult::Read;
    }

    RedisObjectType obj_type = obj.ObjectType();

    bool ttl_reset = false;
    switch (obj_type)
    {
    case RedisObjectType::Unknown:
        assert(false);
        break;
    case RedisObjectType::String:
    case RedisObjectType::List:
    case RedisObjectType::Hash:
    {
        if (obj.HasTTL())
        {
            ttl_reset = true;
        }
        break;
    }
    case RedisObjectType::Del:
        assert(false);
        break;
    case RedisObjectType::Zset:
    case RedisObjectType::Set:
    {
        if (obj.HasTTL())
        {
            ttl_reset = true;
        }
        break;
    }
    case RedisObjectType::TTLString:
    case RedisObjectType::TTLList:
    case RedisObjectType::TTLHash:
    case RedisObjectType::TTLZset:
    case RedisObjectType::TTLSet:
    {
        // never exposr ttl type from obj->ObjectType()
        assert(false);
        break;
    }
    default:
    {
        assert(false);
        break;
    }
    }

    if (ttl_reset == true)
    {
        recover_ttl_obj_cmd_ = std::make_unique<RecoverObjectCommand>();
        obj.Serialize(recover_ttl_obj_cmd_->result_);
    }

    result_.err_code_ = RD_OK;
    return txservice::ExecResult::Write;
}

txservice::TxObject *PersistCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    assert(obj_ptr != nullptr);
    txservice::TxObject *result_ptr = obj_ptr;
    RedisEloqObject *obj = static_cast<RedisEloqObject *>(obj_ptr);
    RedisObjectType obj_type = obj->ObjectType();

    switch (obj_type)
    {
    case RedisObjectType::Unknown:
        assert(false);
        break;
    case RedisObjectType::String:
    {
        if (obj->HasTTL())
        {
            auto *str_ttl_obj_ptr =
                static_cast<RedisStringTTLObject *>(obj_ptr);
            result_ptr = static_cast<txservice::TxObject *>(
                str_ttl_obj_ptr->RemoveTTL().release());
        }
        break;
    }
    case RedisObjectType::List:
    {
        if (obj->HasTTL())
        {
            auto *list_ttl_obj_ptr = static_cast<RedisListTTLObject *>(obj_ptr);
            result_ptr = static_cast<txservice::TxObject *>(
                list_ttl_obj_ptr->RemoveTTL().release());
        }
        break;
    }
    case RedisObjectType::Hash:
    {
        if (obj->HasTTL())
        {
            auto hash_ttl_obj_ptr = static_cast<RedisHashTTLObject *>(obj_ptr);
            result_ptr = static_cast<txservice::TxObject *>(
                hash_ttl_obj_ptr->RemoveTTL().release());
        }
        break;
    }
    case RedisObjectType::Del:
        assert(false);
        break;
    case RedisObjectType::Zset:
    {
        if (obj->HasTTL())
        {
            auto zset_ttl_obj_ptr = static_cast<RedisZsetTTLObject *>(obj_ptr);
            result_ptr = static_cast<txservice::TxObject *>(
                zset_ttl_obj_ptr->RemoveTTL().release());
        }
        break;
    }
    case RedisObjectType::Set:
    {
        if (obj->HasTTL())
        {
            auto hset_ttl_obj_ptr =
                static_cast<RedisHashSetTTLObject *>(obj_ptr);
            result_ptr = static_cast<txservice::TxObject *>(
                hset_ttl_obj_ptr->RemoveTTL().release());
        }
        break;
    }
    case RedisObjectType::TTLString:
    case RedisObjectType::TTLList:
    case RedisObjectType::TTLHash:
    case RedisObjectType::TTLZset:
    case RedisObjectType::TTLSet:
    {
        // never exposr ttl type from obj->ObjectType()
        assert(false);
        break;
    }
    default:
    {
        assert(false);
        break;
    }
    }

    return result_ptr;
}

void PersistCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::PERSIST);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
}

void PersistCommand::Deserialize(std::string_view cmd_image)
{
    // nothing to recover
}

void PersistCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.err_code_ == RD_OK)
    {
        reply->OnInt(1);
    }
    else if (result_.err_code_ == RD_NIL)
    {
        reply->OnInt(0);
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

using txservice::MultiObjectCommandTxRequest;
using txservice::ObjectCommandTxRequest;
std::pair<bool,
          std::variant<DirectRequest,
                       txservice::ObjectCommandTxRequest,
                       txservice::MultiObjectCommandTxRequest,
                       CustomCommandRequest>>
ParseMultiCommand(RedisServiceImpl *redis_impl,
                  RedisConnectionContext *ctx,
                  const std::vector<std::string> &cmd_args,
                  OutputHandler *output,
                  txservice::TransactionExecution *txm)
{
    assert(!cmd_args.empty());
    std::vector<std::string_view> args;
    args.reserve(cmd_args.size());
    for (const auto &str : cmd_args)
    {
        args.emplace_back(str);
    }

    switch (CommandType(std::string_view(args[0].data(), args[0].size())))
    {
    case RedisCommandType::WATCH:
    {
        break;
    }
    case RedisCommandType::UNWATCH:
    {
        break;
    }
    case RedisCommandType::MULTI:
    {
        break;
    }
    case RedisCommandType::EXEC:
    {
        break;
    }
    case RedisCommandType::ECHO:
    {
        auto [success, cmd] = ParseEchoCommand(args, output);
        if (!success)
        {
            return {false, DirectRequest{}};
        }
        return {
            success,
            DirectRequest(ctx, std::make_unique<EchoCommand>(std::move(cmd)))};
    }
    case RedisCommandType::PING:
    {
        auto [success, cmd] = ParsePingCommand(args, output);
        if (!success)
        {
            return {false, DirectRequest{}};
        }
        return {
            success,
            DirectRequest(ctx, std::make_unique<PingCommand>(std::move(cmd)))};
    }
    case RedisCommandType::SELECT:
    {
        auto [success, cmd] = ParseSelectCommand(args, output);
        if (!success)
        {
            return {false, DirectRequest{}};
        }

        cmd.Execute(redis_impl, ctx);  // Execute select right away.
        return {success,
                DirectRequest(ctx,
                              std::make_unique<SelectCommand>(std::move(cmd)))};
    }
    case RedisCommandType::CONFIG:
    {
        auto [success, cmd] = ParseConfigCommand(args, output);
        if (!success)
        {
            return {false, DirectRequest{}};
        }
        return {success,
                DirectRequest(ctx,
                              std::make_unique<ConfigCommand>(std::move(cmd)))};
    }
    case RedisCommandType::INFO:
    {
        auto [success, cmd] = ParseInfoCommand(args, output);
        if (!success)
        {
            return {false, DirectRequest{}};
        }
        return {
            success,
            DirectRequest(ctx, std::make_unique<InfoCommand>(std::move(cmd)))};
    }
    case RedisCommandType::CLUSTER:
    {
        auto [success, cmd] = ParseClusterCommand(args, output);
        if (!success)
        {
            return {false, DirectRequest{}};
        }
        return {success, DirectRequest(ctx, std::move(cmd))};
    }
    case RedisCommandType::CLIENT:
    {
        auto [success, cmd] = ParseClientCommand(args, output);
        if (!success)
        {
            return {false, DirectRequest{}};
        }
        return {success, DirectRequest(ctx, std::move(cmd))};
        break;
    }
    case RedisCommandType::GET:
    {
        auto [success, key, cmd] = ParseGetCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {
            success,
            ObjectCommandTxRequest(redis_impl->RedisTableName(ctx->db_id),
                                   std::make_unique<EloqKey>(std::move(key)),
                                   std::make_unique<GetCommand>(std::move(cmd)),
                                   false,
                                   true,
                                   txm)};
    }
    case RedisCommandType::GETDEL:
    {
        auto [success, key, cmd] = ParseGetDelCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<GetDelCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::STRLEN:
    {
        auto [success, key, cmd] = ParseStrLenCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<StrLenCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::GETBIT:
    {
        auto [success, key, cmd] = ParseGetBitCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<GetBitCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::BITCOUNT:
    {
        auto [success, key, cmd] = ParseBitCountCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<BitCountCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::SET:
    case RedisCommandType::SETNX:
    case RedisCommandType::GETSET:
    case RedisCommandType::SETEX:
    case RedisCommandType::PSETEX:
    {
        auto [success, key, cmd] = ParseSetCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {
            success,
            ObjectCommandTxRequest(redis_impl->RedisTableName(ctx->db_id),
                                   std::make_unique<EloqKey>(std::move(key)),
                                   std::make_unique<SetCommand>(std::move(cmd)),
                                   false,
                                   true,
                                   txm)};
    }
    case RedisCommandType::SETBIT:
    {
        auto [success, key, cmd] = ParseSetBitCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<SetBitCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::SETRANGE:
    {
        auto [success, key, cmd] = ParseSetRangeCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<SetRangeCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::APPEND:
    {
        auto [success, key, cmd] = ParseAppendCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<AppendCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::BITFIELD:
    {
        auto [success, key, cmd] = ParseBitFieldCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<BitFieldCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::BITFIELD_RO:
    {
        auto [success, key, cmd] = ParseBitFieldRoCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<BitFieldCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::BITPOS:
    {
        auto [success, key, cmd] = ParseBitPosCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<BitPosCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::LRANGE:
    {
        auto [success, key, cmd] = ParseLRangeCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<LRangeCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::RPUSH:
    {
        auto [success, key, cmd] = ParseRPushCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<RPushCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::LPOP:
    {
        auto [success, key, cmd] = ParseLPopCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<LPopCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::RPOP:
    {
        auto [success, key, cmd] = ParseRPopCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<RPopCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::LMPOP:
    {
        auto [success, cmd] = ParseLMPopCommand(args, output);
        if (!success)
        {
            return {false, MultiObjectCommandTxRequest{}};
        }
        return {success,
                MultiObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<LMPopCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
        break;
    }
    case RedisCommandType::LMOVE:
    {
        auto [success, cmd] = ParseLMoveCommand(args, output);
        if (!success)
        {
            return {false, MultiObjectCommandTxRequest{}};
        }
        return {success,
                MultiObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<LMoveCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
        break;
    }
    case RedisCommandType::RPOPLPUSH:
    {
        auto [success, cmd] = ParseRPopLPushCommand(args, output);
        if (!success)
        {
            return {false, MultiObjectCommandTxRequest{}};
        }
        return {success,
                MultiObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<RPopLPushCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
        break;
    }
    case RedisCommandType::HSET:
    {
        auto [success, key, cmd] = ParseHSetCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<HSetCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::HSETNX:
    {
        auto [success, key, cmd] = ParseHSetNxCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<HSetNxCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::HGET:
    {
        auto [success, key, cmd] = ParseHGetCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<HGetCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::HLEN:
    {
        auto [success, key, cmd] = ParseHLenCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<HLenCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::HSTRLEN:
    {
        auto [success, key, cmd] = ParseHStrLenCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<HStrLenCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::HDEL:
    {
        auto [success, key, cmd] = ParseHDelCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<HDelCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::HINCRBY:
    {
        auto [success, key, cmd] = ParseHIncrByCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<HIncrByCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::HINCRBYFLOAT:
    {
        auto [success, key, cmd] = ParseHIncrByFloatCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<HIncrByFloatCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::HMGET:
    {
        auto [success, key, cmd] = ParseHMGetCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<HMGetCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::HKEYS:
    {
        auto [success, key, cmd] = ParseHKeysCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<HKeysCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::HVALS:
    {
        auto [success, key, cmd] = ParseHValsCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<HValsCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::HGETALL:
    {
        auto [success, key, cmd] = ParseHGetAllCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<HGetAllCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::HEXISTS:
    {
        auto [success, key, cmd] = ParseHExistsCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<HExistsCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::HRANDFIELD:
    {
        auto [success, key, cmd] = ParseHRandFieldCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<HRandFieldCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::HSCAN:
    {
        auto [success, key, cmd] = ParseHScanCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<HScanCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::INCR:
    {
        auto [success, key, cmd] = ParseIncrCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<IntOpCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::DECR:
    {
        auto [success, key, cmd] = ParseDecrCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<IntOpCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::INCRBY:
    {
        auto [success, key, cmd] = ParseIncrByCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<IntOpCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::DECRBY:
    {
        auto [success, key, cmd] = ParseDecrByCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<IntOpCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::TYPE:
    {
        auto [success, key, cmd] = ParseTypeCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<TypeCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::DEL:
    {
        auto [success, cmd] = ParseDelCommand(args, output);
        if (!success)
        {
            return {false, MultiObjectCommandTxRequest{}};
        }
        return {success,
                MultiObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<MDelCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::EXISTS:
    {
        auto [success, cmd] = ParseExistsCommand(args, output);
        if (!success)
        {
            return {false, MultiObjectCommandTxRequest{}};
        }
        return {success,
                MultiObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<MExistsCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::MGET:
    {
        auto [success, cmd] = ParseMGetCommand(args, output);
        if (!success)
        {
            return {false, MultiObjectCommandTxRequest{}};
        }
        return {success,
                MultiObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<MGetCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::MSET:
    {
        auto [success, cmd] = ParseMSetCommand(args, output);
        if (!success)
        {
            return {false, MultiObjectCommandTxRequest{}};
        }
        return {success,
                MultiObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<MSetCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::MSETNX:
    {
        auto [success, cmd] = ParseMSetNxCommand(args, output);
        if (!success)
        {
            return {false, MultiObjectCommandTxRequest{}};
        }

        return {success,
                MultiObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<MSetNxCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::ZADD:
    {
        auto [success, key, cmd] = ParseZAddCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<ZAddCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::ZREM:
    {
        auto [success, key, cmd] = ParseZRemCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<ZRemCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::ZCARD:
    {
        auto [success, key, cmd] = ParseZCardCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<ZCardCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::ZREVRANGE:
    {
        auto [success, key, cmd] = ParseZRevRangeCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<ZRangeCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::ZREVRANGEBYSCORE:
    {
        auto [success, key, cmd] = ParseZRevRangeByScoreCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<ZRangeCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::ZREVRANGEBYLEX:
    {
        auto [success, key, cmd] = ParseZRevRangeByLexCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<ZRangeCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::ZRANDMEMBER:
    {
        auto [success, key, cmd] = ParseZRandMemberCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<ZRandMemberCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::ZSCORE:
    {
        auto [success, key, cmd] = ParseZScoreCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<ZScoreCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::ZRANK:
    {
        auto [success, key, cmd] = ParseZRankCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<ZRankCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::ZREVRANK:
    {
        auto [success, key, cmd] = ParseZRevRankCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<ZRankCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::ZCOUNT:
    {
        auto [success, key, cmd] = ParseZCountCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<ZCountCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::ZRANGEBYSCORE:
    {
        auto [success, key, cmd] = ParseZRangeByScoreCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<ZRangeCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::ZRANGEBYRANK:
    {
        auto [success, key, cmd] = ParseZRangeByRankCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<ZRangeCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::ZRANGEBYLEX:
    {
        auto [success, key, cmd] = ParseZRangeByLexCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<ZRangeCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::ZREMRANGE:
    {
        auto [success, key, cmd] = ParseZRemRangeCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<ZRemRangeCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::ZSCAN:
    {
        auto [success, key, cmd] = ParseZScanCommand(ctx, args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                CustomCommandRequest(redis_impl->RedisTableName(ctx->db_id),
                                     std::make_unique<ZScanWrapper>(
                                         std::move(key), std::move(cmd)))};
    }
    case RedisCommandType::ZPOPMIN:
    {
        auto [success, key, cmd] = ParseZPopMinCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<ZPopCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::ZPOPMAX:
    {
        auto [success, key, cmd] = ParseZPopMaxCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }

        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<ZPopCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::ZLEXCOUNT:
    {
        auto [success, key, cmd] = ParseZLexCountCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }

        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<ZLexCountCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::ZUNION:
    {
        auto [success, cmd] = ParseZUnionCommand(args, output);
        if (!success)
        {
            return {false, MultiObjectCommandTxRequest{}};
        }

        return {success,
                MultiObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<ZUnionCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::ZUNIONSTORE:
    {
        auto [success, cmd] = ParseZUnionStoreCommand(args, output);
        if (!success)
        {
            return {false, MultiObjectCommandTxRequest{}};
        }

        return {success,
                MultiObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<ZUnionStoreCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::ZINTER:
    {
        auto [success, cmd] = ParseZInterCommand(args, output);
        if (!success)
        {
            return {false, MultiObjectCommandTxRequest{}};
        }

        return {success,
                MultiObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<ZInterCommand>(std::move(cmd)),
                    false)};
    }
    case RedisCommandType::ZINTERCARD:
    {
        auto [success, cmd] = ParseZInterCardCommand(args, output);
        if (!success)
        {
            return {false, MultiObjectCommandTxRequest{}};
        }

        return {success,
                MultiObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<ZInterCardCommand>(std::move(cmd)),
                    false)};
    }
    case RedisCommandType::ZINTERSTORE:
    {
        auto [success, cmd] = ParseZInterStoreCommand(args, output);
        if (!success)
        {
            return {false, MultiObjectCommandTxRequest{}};
        }

        return {success,
                MultiObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<ZInterStoreCommand>(std::move(cmd)),
                    false)};
    }
    case RedisCommandType::ZMSCORE:
    {
        auto [success, key, cmd] = ParseZMScoreCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<ZMScoreCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
        break;
    }
    case RedisCommandType::ZMPOP:
    {
        auto [success, cmd] = ParseZMPopCommand(args, output);
        if (!success)
        {
            return {false, MultiObjectCommandTxRequest{}};
        }

        return {success,
                MultiObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<ZMPopCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};

        break;
    }
    case RedisCommandType::ZDIFF:
    {
        auto [success, cmd] = ParseZDiffCommand(args, output);
        if (!success)
        {
            return {false, MultiObjectCommandTxRequest{}};
        }

        return {success,
                MultiObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<ZDiffCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};

        break;
    }
    case RedisCommandType::ZDIFFSTORE:
    {
        auto [success, cmd] = ParseZDiffStoreCommand(args, output);
        if (!success)
        {
            return {false, MultiObjectCommandTxRequest{}};
        }

        return {success,
                MultiObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<ZDiffStoreCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};

        break;
    }
    case RedisCommandType::ZRANGESTORE:
    {
        auto [success, cmd] = ParseZRangeStoreCommand(args, output);
        if (!success)
        {
            return {false, MultiObjectCommandTxRequest{}};
        }

        return {success,
                MultiObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<ZRangeStoreCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
        break;
    }
    case RedisCommandType::ZINCRBY:
    {
        auto [success, key, cmd] = ParseZIncrByCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<ZAddCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
        break;
    }
    case RedisCommandType::ZRANGE:
    {
        auto [success, key, cmd] = ParseZRangeCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<ZRangeCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
        break;
    }
    case RedisCommandType::SADD:
    {
        auto [success, key, cmd] = ParseSAddCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<SAddCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::SMEMBERS:
    {
        auto [success, key, cmd] = ParseSMembersCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<SMembersCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::SREM:
    {
        auto [success, key, cmd] = ParseSRemCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<SRemCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::SPOP:
    {
        auto [success, key, cmd] = ParseSPopCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<SPopCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::SCARD:
    {
        auto [success, key, cmd] = ParseSCardCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<SCardCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::SINTER:
    {
        auto [success, cmd] = ParseSInterCommand(args, output);
        if (!success)
        {
            return {false, MultiObjectCommandTxRequest{}};
        }
        return {success,
                MultiObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<SInterCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::SDIFF:
    {
        auto [success, cmd] = ParseSDiffCommand(args, output);
        if (!success)
        {
            return {false, MultiObjectCommandTxRequest{}};
        }
        return {success,
                MultiObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<SDiffCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::SDIFFSTORE:
    {
        auto [success, cmd] = ParseSDiffStoreCommand(args, output);
        if (!success)
        {
            return {false, MultiObjectCommandTxRequest{}};
        }
        return {success,
                MultiObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<SDiffStoreCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::SMOVE:
    {
        auto [success, cmd] = ParseSMoveCommand(args, output);
        if (!success)
        {
            return {false, MultiObjectCommandTxRequest{}};
        }
        return {success,
                MultiObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<SMoveCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::RPUSHX:
    {
        auto [success, key, cmd] = ParseRPushXCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<RPushXCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::LINDEX:
    {
        auto [success, key, cmd] = ParseLIndexCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<LIndexCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::LINSERT:
    {
        auto [success, key, cmd] = ParseLInsertCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<LInsertCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::LPUSH:
    {
        auto [success, key, cmd] = ParseLPushCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<LPushCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::LPUSHX:
    {
        auto [success, key, cmd] = ParseLPushXCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<LPushXCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::LREM:
    {
        auto [success, key, cmd] = ParseLRemCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<LRemCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::LTRIM:
    {
        auto [success, key, cmd] = ParseLTrimCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<LTrimCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::LLEN:
    {
        auto [success, key, cmd] = ParseLLenCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<LLenCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::LPOS:
    {
        auto [success, key, cmd] = ParseLPosCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<LPosCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::LSET:
    {
        auto [success, key, cmd] = ParseLSetCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<LSetCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::DUMP:
    {
        auto [success, key, cmd] = ParseDumpCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<DumpCommand>(std::move(cmd)),
                    false)};
    }
    case RedisCommandType::RESTORE:
    {
        auto [success, key, cmd] = ParseRestoreCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<RestoreCommand>(std::move(cmd)),
                    false)};
    }
    case RedisCommandType::SCAN:
    {
        auto [success, cmd] = ParseScanCommand(ctx, args, output);
        if (!success)
        {
            return {false, CustomCommandRequest{}};
        }
        return {success,
                CustomCommandRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<ScanCommand>(std::move(cmd)))};
        break;
    }
    case RedisCommandType::KEYS:
    {
        auto [success, cmd] = ParseKeysCommand(args, output);
        if (!success)
        {
            return {false, CustomCommandRequest{}};
        }
        return {success,
                CustomCommandRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<ScanCommand>(std::move(cmd)))};
        break;
    }
    case RedisCommandType::BLMOVE:
    {
        auto [success, cmd] = ParseBLMoveCommand(args, output, true);
        if (!success)
        {
            return {false, MultiObjectCommandTxRequest{}};
        }

        return {success,
                MultiObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<BLMoveCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::BLMPOP:
    {
        auto [success, cmd] = ParseBLMPopCommand(args, output, true);
        if (!success)
        {
            return {false, MultiObjectCommandTxRequest{}};
        }

        return {success,
                MultiObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<BLMPopCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::BLPOP:
    {
        auto [success, cmd] = ParseBLPopCommand(args, output, true);
        if (!success)
        {
            return {false, MultiObjectCommandTxRequest{}};
        }

        return {success,
                MultiObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<BLMPopCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
        break;
    }
    case RedisCommandType::BRPOP:
    {
        auto [success, cmd] = ParseBRPopCommand(args, output, true);
        if (!success)
        {
            return {false, MultiObjectCommandTxRequest{}};
        }

        return {success,
                MultiObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<BLMPopCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
        break;
    }
    case RedisCommandType::BRPOPLPUSH:
    {
        auto [success, cmd] = ParseBRPopLPushCommand(args, output, true);
        if (!success)
        {
            return {false, MultiObjectCommandTxRequest{}};
        }

        return {success,
                MultiObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<BLMoveCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::GETRANGE:
    case RedisCommandType::SUBSTR:
    {
        auto [success, key, cmd] = ParseGetRangeCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<GetRangeCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::INCRBYFLOAT:
    {
        auto [success, key, cmd] = ParseIncrByFloatCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<FloatOpCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::BITOP:
    {
        auto [success, cmd] = ParseBitOpCommand(args, output);
        if (!success)
        {
            return {false, MultiObjectCommandTxRequest{}};
        }
        return {success,
                MultiObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<BitOpCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::SINTERCARD:
    {
        auto [success, cmd] = ParseSInterCardCommand(args, output);
        if (!success)
        {
            return {false, MultiObjectCommandTxRequest{}};
        }
        return {success,
                MultiObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<SInterCardCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::SINTERSTORE:
    {
        auto [success, cmd] = ParseSInterStoreCommand(args, output);
        if (!success)
        {
            return {false, MultiObjectCommandTxRequest{}};
        }
        return {success,
                MultiObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<SInterStoreCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::SISMEMBER:
    {
        auto [success, key, cmd] = ParseSIsMemberCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<SIsMemberCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::SMISMEMBER:
    {
        auto [success, key, cmd] = ParseSMIsMemberCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<SMIsMemberCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::SRANDMEMBER:
    {
        auto [success, key, cmd] = ParseSRandMemberCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<SRandMemberCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::SUNION:
    {
        auto [success, cmd] = ParseSUnionCommand(args, output);
        if (!success)
        {
            return {false, MultiObjectCommandTxRequest{}};
        }
        return {success,
                MultiObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<SUnionCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
    case RedisCommandType::SUNIONSTORE:
    {
        auto [success, cmd] = ParseSUnionStoreCommand(args, output);
        if (!success)
        {
            return {false, MultiObjectCommandTxRequest{}};
        }
        return {success,
                MultiObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<SUnionStoreCommand>(std::move(cmd)),
                    false,
                    true,
                    txm)};
    }
#ifdef WITH_FAULT_INJECT
    case RedisCommandType::FAULT_INJECT:
    {
        auto [success, cmd] = ParseFaultInjectCommand(args, output);
        if (!success)
        {
            return {false, CustomCommandRequest{}};
        }
        return {success,
                CustomCommandRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<RedisFaultInjectCommand>(std::move(cmd)))};
    }
#endif
    case RedisCommandType::MEMORY_USAGE:
    {
        auto [success, key, cmd] = ParseMemoryUsageCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<RedisMemoryUsageCommand>(std::move(cmd)),
                    false)};
        break;
    }
    case RedisCommandType::EXPIRE:
    {
        auto [success, key, cmd] =
            ParseExpireCommand(args, output, false, false);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<ExpireCommand>(std::move(cmd)),
                    false)};
        break;
    }
    case RedisCommandType::PEXPIRE:
    {
        auto [success, key, cmd] =
            ParseExpireCommand(args, output, true, false);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<ExpireCommand>(std::move(cmd)),
                    false)};
        break;
    }
    case RedisCommandType::EXPIREAT:
    {
        auto [success, key, cmd] =
            ParseExpireCommand(args, output, false, true);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<ExpireCommand>(std::move(cmd)),
                    false)};
        break;
    }
    case RedisCommandType::PEXPIREAT:
    {
        auto [success, key, cmd] = ParseExpireCommand(args, output, true, true);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<ExpireCommand>(std::move(cmd)),
                    false)};
        break;
    }
    case RedisCommandType::TTL:
    {
        auto [success, key, cmd] =
            ParseTTLCommand(args, output, false, false, false);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {
            success,
            ObjectCommandTxRequest(redis_impl->RedisTableName(ctx->db_id),
                                   std::make_unique<EloqKey>(std::move(key)),
                                   std::make_unique<TTLCommand>(std::move(cmd)),
                                   false)};
        break;
    }
    case RedisCommandType::PTTL:
    {
        auto [success, key, cmd] =
            ParseTTLCommand(args, output, true, false, false);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {
            success,
            ObjectCommandTxRequest(redis_impl->RedisTableName(ctx->db_id),
                                   std::make_unique<EloqKey>(std::move(key)),
                                   std::make_unique<TTLCommand>(std::move(cmd)),
                                   false)};
        break;
    }
    case RedisCommandType::EXPIRETIME:
    {
        auto [success, key, cmd] =
            ParseTTLCommand(args, output, false, true, false);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {
            success,
            ObjectCommandTxRequest(redis_impl->RedisTableName(ctx->db_id),
                                   std::make_unique<EloqKey>(std::move(key)),
                                   std::make_unique<TTLCommand>(std::move(cmd)),
                                   false)};
        break;
    }
    case RedisCommandType::PEXPIRETIME:
    {
        auto [success, key, cmd] =
            ParseTTLCommand(args, output, false, false, true);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {
            success,
            ObjectCommandTxRequest(redis_impl->RedisTableName(ctx->db_id),
                                   std::make_unique<EloqKey>(std::move(key)),
                                   std::make_unique<TTLCommand>(std::move(cmd)),
                                   false)};
        break;
    }
    case RedisCommandType::PERSIST:
    {
        auto [success, key, cmd] = ParsePersistCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<PersistCommand>(std::move(cmd)),
                    false)};
        break;
    }
    case RedisCommandType::GETEX:
    {
        auto [success, key, cmd] = ParseGetExCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }
        return {success,
                ObjectCommandTxRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<EloqKey>(std::move(key)),
                    std::make_unique<GetExCommand>(std::move(cmd)),
                    false)};
        break;
    }
    case RedisCommandType::TIME:
    {
        auto [success, cmd] = ParseTimeCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest{}};
        }

        return {
            success,
            DirectRequest(ctx, std::make_unique<TimeCommand>(std::move(cmd)))};
    }
    case RedisCommandType::SLOWLOG:
    {
        auto [success, cmd] = ParseSlowLogCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest()};
        }
        return {success,
                DirectRequest(
                    ctx, std::make_unique<SlowLogCommand>(std::move(cmd)))};
    }
    case RedisCommandType::PUBLISH:
    {
        auto [success, cmd] = ParsePublishCommand(args, output);
        if (!success)
        {
            return {false, ObjectCommandTxRequest()};
        }
        return {success,
                DirectRequest(
                    ctx, std::make_unique<PublishCommand>(std::move(cmd)))};
    }
    case RedisCommandType::ELOQVEC_CREATE:
    {
        auto [success, cmd] = ParseCreateVecIndexCommand(args, output);
        if (!success)
        {
            return {false, CustomCommandRequest{}};
        }
        return {success,
                CustomCommandRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<CreateVecIndexCommand>(std::move(cmd)))};
    }
    case RedisCommandType::ELOQVEC_INFO:
    {
        auto [success, cmd] = ParseInfoVecIndexCommand(args, output);
        if (!success)
        {
            return {false, CustomCommandRequest{}};
        }
        return {success,
                CustomCommandRequest(
                    redis_impl->RedisTableName(ctx->db_id),
                    std::make_unique<InfoVecIndexCommand>(std::move(cmd)))};
    }
    default:
        LOG(WARNING) << "Unsupported command in MULTI " << args[0];
        output->OnError("Unsupported command in MULTI");
        return {false, ObjectCommandTxRequest()};
    }
    output->OnError("Command not allowed inside a transaction");
    return {false, ObjectCommandTxRequest()};
}

std::tuple<bool, EchoCommand> ParseEchoCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "echo");
    if (args.size() != 2)
    {
        output->OnError("ERR wrong number of arguments for 'echo' command");
        return {false, EchoCommand()};
    }
    return {true, EchoCommand(args[1])};
}

std::tuple<bool, PingCommand> ParsePingCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "ping");
    switch (args.size())
    {
    case 1:
    {
        return {true, PingCommand(true, "PONG")};
    }
    case 2:
    {
        return {true, PingCommand(false, args[1])};
    }
    default:
    {
        output->OnError("ERR wrong number of arguments for 'ping' command");
        return {false, PingCommand()};
    }
    }
}

std::tuple<bool, AuthCommand> ParseAuthCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    if (args.size() == 1)
    {
        output->OnError("ERR wrong number of arguments for 'auth' command");
        return {false, AuthCommand()};
    }
    else if (args.size() > 3)
    {
        output->OnError("ERR syntax error");
        return {false, AuthCommand()};
    }
    else
    {
        assert(args.size() == 2 || args.size() == 3);
        if (args.size() == 2 && requirepass.empty())
        {
            // Mimic the old behavior of giving an error for the two
            // argument form if no password is configured.
            output->OnError(
                "AUTH <password> called without any password "
                "configured for the default user. Are you sure "
                "your configuration is correct?");
            return {false, AuthCommand()};
        }

        using namespace std::literals;
        const std::string_view &username =
            args.size() == 3 ? args[1] : "default"sv;
        const std::string_view &password = args.back();
        return {true, AuthCommand(username, password)};
    }
}

std::tuple<bool, SelectCommand> ParseSelectCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    if (args.size() == 2)
    {
        try
        {
            int db_id = std::stoi(args[1].data());
            return {true, SelectCommand(db_id)};
        }
        catch (std::exception const &ex)
        {
            output->OnError(redis_get_error_messages(RD_ERR_DIGITAL_INVALID));
            return {false, SelectCommand()};
        }
    }
    else
    {
        output->OnError("ERR wrong number of arguments for 'select' command");
        return {false, SelectCommand()};
    }
}

std::tuple<bool, DBSizeCommand> ParseDBSizeCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    if (args.size() > 1)
    {
        output->OnError("ERR wrong number of arguments for 'dbsize' command");
        return {false, DBSizeCommand()};
    }
    return {true, DBSizeCommand()};
}

std::tuple<bool, SlowLogCommand> ParseSlowLogCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "slowlog");
    if (args.size() == 1)
    {
        output->OnError("ERR wrong number of arguments for 'slowlog' command");
        return {false, SlowLogCommand()};
    }

    if (0 == strcasecmp(args[1].data(), "get"))
    {
        if (args.size() == 2)
        {
            return {true, SlowLogCommand(SLOWLOG_GET, -1)};
        }
        else if (args.size() == 3)
        {
            try
            {
                int len = std::stoi(args[2].data());
                if (len < -1)
                {
                    output->OnError(
                        "ERR count should be greater than or equal to -1");
                    return {false, SlowLogCommand()};
                }
                return {true, SlowLogCommand(SLOWLOG_GET, len)};
            }
            catch (std::exception const &ex)
            {
                output->OnError(
                    redis_get_error_messages(RD_ERR_DIGITAL_INVALID));
                return {false, SlowLogCommand()};
            }
        }
        else
        {
            output->OnError(
                "ERR wrong number of arguments for 'slowlog' command");
            return {false, SlowLogCommand()};
        }
    }
    else if (0 == strcasecmp(args[1].data(), "reset"))
    {
        return {true, SlowLogCommand(SLOWLOG_RESET, -1)};
    }
    else if (0 == strcasecmp(args[1].data(), "len"))
    {
        return {true, SlowLogCommand(SLOWLOG_LEN, -1)};
    }
    else
    {
        output->OnError("ERR unknown subcommand '" + std::string(args[1]) +
                        "'.");
        return {false, SlowLogCommand()};
    }
}

std::tuple<bool, PublishCommand> ParsePublishCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "publish");
    if (args.size() != 3)
    {
        output->OnError("ERR wrong number of arguments for 'publish' command");
        return {false, PublishCommand()};
    }
    return {true, PublishCommand(args[1], args[2])};
}

std::tuple<bool, ReadOnlyCommand> ParseReadOnlyCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    if (args.size() > 1)
    {
        output->OnError("ERR wrong number of arguments for 'readonly command");
        return {false, ReadOnlyCommand()};
    }

    return {true, ReadOnlyCommand()};
}

std::tuple<bool, InfoCommand> ParseInfoCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    std::unordered_set<std::string> set_sct;
    set_sct.reserve(args.size() - 1);
    for (size_t i = 1; i < args.size(); i++)
    {
        const std::string_view &sv = args[i];
        std::string str(sv.size(), 0);
        std::transform(sv.begin(),
                       sv.end(),
                       str.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        set_sct.insert(str);
    }

    if (set_sct.find("all") != set_sct.end())
    {
        set_sct.clear();
    }

    return {true, InfoCommand(std::move(set_sct))};
}

std::tuple<bool, std::unique_ptr<CommandCommand>> ParseCommandCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    if (args.size() == 1)
    {
        return {true, std::make_unique<CommandCommand>()};
    }
    else
    {
        if (args[1].size() == 4 && stringcomp(args[1], "info", 1))
        {
            auto command = std::make_unique<CommandInfoCommand>();
            command->command_names_.reserve(args.size() - 2);
            for (size_t i = 2; i < args.size(); ++i)
            {
                command->command_names_.emplace_back(args[i]);
            }
            return {true, std::move(command)};
        }
        if (args[1].size() == 5 && stringcomp(args[1], "count", 1))
        {
            return {true, std::make_unique<CommandCountCommand>()};
        }
        if (args[1].size() == 4 && stringcomp(args[1], "list", 1))
        {
            if (args.size() == 2)
            {
                return {true, std::make_unique<CommandListCommand>()};
            }
            output->OnError("ERR syntax error");
            return {false, nullptr};
        }
        output->OnError("ERR unknown subcommand '" + std::string(args[1]) +
                        "'.");
        return {false, nullptr};
        // TODO(lzx): Complete subcommands of this command
    }

    output->OnError("ERR unknown subcommand '" + std::string(args[1]) + "'.");
    return {false, nullptr};
}

std::tuple<bool, std::unique_ptr<ClusterCommand>> ParseClusterCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(0 == strcasecmp(args[0].data(), "cluster"));
    if (args.size() == 1)
    {
        output->OnError("ERR wrong number of arguments for 'cluster' command");
        return {false, nullptr};
    }
    if (0 == strcasecmp(args[1].data(), "info"))
    {
        if (args.size() == 2)
        {
            return {true, std::make_unique<ClusterInfoCommand>()};
        }
        output->OnError("ERR wrong number of arguments for 'cluster|" +
                        std::string(args[1]) + "' command");
        return {false, nullptr};
    }
    else if (0 == strcasecmp(args[1].data(), "nodes"))
    {
        if (args.size() == 2)
        {
            return {true, std::make_unique<ClusterNodesCommand>()};
        }
        output->OnError("ERR wrong number of arguments for 'cluster|" +
                        std::string(args[1]) + "' command");
        return {false, nullptr};
    }
    else if (0 == strcasecmp(args[1].data(), "slots"))
    {
        if (args.size() == 2)
        {
            return {true, std::make_unique<ClusterSlotsCommand>()};
        }
        output->OnError("ERR wrong number of arguments for 'cluster|" +
                        std::string(args[1]) + "' command");
        return {false, nullptr};
    }
    else if (0 == strcasecmp(args[1].data(), "keyslot"))
    {
        if (args.size() == 3)
        {
            return {true, std::make_unique<ClusterKeySlotCommand>(args[2])};
        }
        output->OnError("ERR wrong number of arguments for 'cluster|" +
                        std::string(args[1]) + "' command");
        return {false, nullptr};
    }

    output->OnError("ERR unknown subcommand '" + std::string(args[1]) + "'.");
    return {false, nullptr};
}

std::tuple<bool, std::unique_ptr<FailoverCommand>> ParseFailoverCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    // syntax: failover to target_host:target_port
    assert(0 == strcasecmp(args[0].data(), "failover"));

    // Check command format and parse arguments
    if (args.size() != 3)
    {
        output->OnError("ERR wrong number of arguments for 'failover' command");
        return {false, nullptr};
    }

    // Validate the TO keyword
    if (0 != strcasecmp(args[1].data(), "to"))
    {
        output->OnError("ERR syntax error: FAILOVER TO <host:port>");
        return {false, nullptr};
    }

    // Parse target host:port from args
    const int arg_index = 2;  // host:port is always at index 2
    std::string target_str(args[arg_index].data(), args[arg_index].size());
    size_t colon_pos = target_str.find(':');
    if (colon_pos == std::string::npos)
    {
        output->OnError("ERR invalid format, expected host:port");
        return {false, nullptr};
    }

    // Extract host and port
    std::string target_host = target_str.substr(0, colon_pos);
    uint16_t target_port = 0;
    try
    {
        target_port = std::stoul(target_str.substr(colon_pos + 1));
    }
    catch (const std::exception &e)
    {
        output->OnError("ERR invalid port number");
        return {false, nullptr};
    }

    return {true, std::make_unique<FailoverCommand>(target_host, target_port)};
}

std::tuple<bool, std::unique_ptr<ClientCommand>> ParseClientCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(0 == strcasecmp(args[0].data(), "client"));
    if (args.size() == 1)
    {
        output->OnError("ERR wrong number of arguments for 'client' command");
        return {false, nullptr};
    }
    else if (0 == strcasecmp(args[1].data(), "id") && args.size() == 2)
    {
        return {true, std::make_unique<ClientIdCommand>()};
    }
    else if (0 == strcasecmp(args[1].data(), "info") && args.size() == 2)
    {
        return {true, std::make_unique<ClientInfoCommand>()};
    }
    else if (0 == strcasecmp(args[1].data(), "setinfo"))
    {
        if (args.size() == 4)
        {
            ClientSetInfoCommand::Attribute attr;
            if (0 == strcasecmp(args[2].data(), "lib-name"))
            {
                attr = ClientSetInfoCommand::Attribute::LIB_NAME;
            }
            else if (0 == strcasecmp(args[2].data(), "lib-ver"))
            {
                attr = ClientSetInfoCommand::Attribute::LIB_VER;
            }
            else
            {
                output->OnError("ERR Unrecognized option" +
                                std::string(args[2]));
                return {false, nullptr};
            }

            return {true,
                    std::make_unique<ClientSetInfoCommand>(attr, args[3])};
        }
        output->OnError("ERR wrong number of arguments for 'client|" +
                        std::string(args[1]) + "'command");
        return {false, nullptr};
    }
    else if (0 == strcasecmp(args[1].data(), "list"))
    {
        if (args.size() == 2)
        {
            return {true, std::make_unique<ClientListCommand>()};
        }
        else if (args.size() == 4 && 0 == strcasecmp(args[2].data(), "type"))
        {
            ClientListCommand::Type type;
            bool success =
                ClientListCommand::GetTypeByName(args[3].data(), &type);
            if (success)
            {
                return {true, std::make_unique<ClientListCommand>(type)};
            }
            else
            {
                output->OnError("Unknown client type '" + std::string(args[3]) +
                                "'");
                return {false, nullptr};
            }
        }
        else if (args.size() > 3 && 0 == strcasecmp(args[2].data(), "id"))
        {
            std::vector<int64_t> client_id_vec;
            client_id_vec.reserve(args.size() - 3);
            for (size_t j = 3; j < args.size(); j++)
            {
                int64_t client_id;
                if (string2ll(args[j].data(), args[j].size(), client_id))
                {
                    if (client_id >= 0)
                    {
                        client_id_vec.push_back(client_id);
                    }
                }
                else
                {
                    output->OnError("Invalid client ID");
                    return {false, nullptr};
                }
            }
            return {
                true,
                std::make_unique<ClientListCommand>(std::move(client_id_vec))};
        }
        else
        {
            output->OnError("ERR wrong number of arguments for 'client|" +
                            std::string(args[1]) + "'command");
            return {false, nullptr};
        }
    }
    else if (0 == strcasecmp(args[1].data(), "kill"))
    {
        if (args.size() == 3)
        {
            // old style syntax
            butil::EndPoint addr;
            if (butil::str2endpoint(args[2].data(), &addr) == 0)
            {
                return {true,
                        std::make_unique<ClientKillCommand>(std::move(addr))};
            }
            else
            {
                return {false, nullptr};
            }
        }
        else
        {
        }
    }
    else if (0 == strcasecmp(args[1].data(), "setname") && args.size() == 3)
    {
        return {true,
                std::make_unique<ClientSetNameCommand>(
                    std::string_view(args[2].data(), args[2].size()))};
    }
    else if (0 == strcasecmp(args[1].data(), "getname") && args.size() == 2)
    {
        return {true, std::make_unique<ClientGetNameCommand>()};
    }

    output->OnFormatError(
        "ERR unknown subcommand '%.128s' or wrong number of arguments.",
        args[1].data(),
        args[0].data());
    return {false, nullptr};
}

std::tuple<bool, ConfigCommand> ParseConfigCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    if (args.size() == 1)
    {
        output->OnError("ERR wrong number of arguments for 'config' command");
        return {false, ConfigCommand()};
    }
    if (stringcomp(args[1], "set", 1))
    {
        if (args.size() < 4)
        {
            output->OnError(
                "ERR wrong number of arguments for 'config|set' command");
            return {false, ConfigCommand()};
        }
        if (args.size() % 2 != 0)
        {
            output->OnError("syntax error");
            return {false, ConfigCommand()};
        }
        std::vector<std::string_view> keys;
        std::vector<std::string_view> values;
        for (size_t i = 2; i < args.size(); i += 2)
        {
            if (redis_config_keys.find(std::string(args[i])) ==
                redis_config_keys.end())
            {
                std::string message =
                    "Unknown option or number of arguments for CONFIG SET - '";
                message.append(args[i]);
                message.append("'");
                output->OnError(message);
                return {false, ConfigCommand()};
            }
            keys.emplace_back(args[i]);
            values.emplace_back(args[i + 1]);
        }
        return {true, ConfigCommand(keys, values)};
    }
    if (stringcomp(args[1], "get", 1))
    {
        std::vector<std::string_view> keys;
        for (size_t i = 2; i < args.size(); ++i)
        {
            keys.emplace_back(args[i]);
        }
        return {true, ConfigCommand(keys)};
    }
    output->OnError("ERR unknown subcommand '" + std::string(args[1]) + "'.");
    return {false, ConfigCommand()};
}

std::tuple<bool, EloqKey, GetCommand> ParseGetCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "get");
    if (args.size() != 2ul)
    {
        output->OnError("ERR wrong number of args for 'get' command");
        return {false, EloqKey(), GetCommand()};
    }
    return {true, EloqKey(args[1]), GetCommand()};
}

std::tuple<bool, EloqKey, GetDelCommand> ParseGetDelCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "getdel");
    if (args.size() != 2ul)
    {
        output->OnError("ERR wrong number of args for 'getdel' command");
        return {false, EloqKey(), GetDelCommand()};
    }
    return {true, EloqKey(args[1]), GetDelCommand()};
}

std::tuple<bool, EloqKey, SetCommand> ParseSetCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    int flag = 0;
    assert(args[0] == "set" || args[0] == "setnx" || args[0] == "getset" ||
           args[0] == "setex" || args[0] == "psetex");
    if (args[0] == "setnx")
    {
        if (args.size() != 3ul)
        {
            output->OnError(
                "ERR wrong number of arguments for 'setnx' command");
            return {false, EloqKey(), SetCommand()};
        }
        return {true,
                EloqKey(args[1]),
                SetCommand(args[2], flag | OBJ_SET_SETNX | OBJ_SET_NX)};
    }
    else if (args[0] == "getset")
    {
        if (args.size() != 3ul)
        {
            output->OnError(
                "ERR wrong number of arguments for 'getset' command");
            return {false, EloqKey(), SetCommand()};
        }
        return {true, EloqKey(args[1]), SetCommand(args[2], OBJ_GET_SET)};
    }
    else if (args[0] == "setex")
    {
        if (args.size() != 4ul)
        {
            output->OnError(
                "ERR wrong number of arguments for 'setex' command");
            return {false, EloqKey(), SetCommand()};
        }

        int64_t expire_ts_tmp;
        if (!string2ll(args[2].data(), args[2].size(), expire_ts_tmp))
        {
            output->OnError("ERR invalid expire time in 'setex' command");
            return {false, EloqKey(), SetCommand()};
        }

        if (expire_ts_tmp < 0)
        {
            output->OnError("invalid expire");
            return {false, EloqKey(), SetCommand()};
        }

        uint64_t current_ts = txservice::LocalCcShards::ClockTsInMillseconds();

        if (EloqKV::will_multiplication_overflow(expire_ts_tmp, 1000ll))
        {
            output->OnError("ERR invalid expire time in 'setex' command");
            return {false, EloqKey(), SetCommand()};
        }
        expire_ts_tmp = expire_ts_tmp * 1000;

        if (EloqKV::will_addition_overflow(static_cast<int64_t>(current_ts),
                                           expire_ts_tmp))
        {
            output->OnError("ERR invalid expire time in 'setex' command");
            return {false, EloqKey(), SetCommand()};
        }

        uint64_t expire_ts =
            (current_ts + expire_ts_tmp) < 0 ? 0 : (current_ts + expire_ts_tmp);
        return {
            true, EloqKey(args[1]), SetCommand(args[3], OBJ_SET_EX, expire_ts)};
    }
    else if (args[0] == "psetex")
    {
        if (args.size() != 4ul)
        {
            output->OnError(
                "ERR wrong number of arguments for 'psetex' command");
            return {false, EloqKey(), SetCommand()};
        }

        int64_t expire_ts_tmp;
        if (!string2ll(args[2].data(), args[2].size(), expire_ts_tmp))
        {
            output->OnError(redis_get_error_messages(RD_ERR_DIGITAL_INVALID));
            return {false, EloqKey(), SetCommand()};
        }

        uint64_t current_ts = txservice::LocalCcShards::ClockTsInMillseconds();

        if (EloqKV::will_addition_overflow(static_cast<int64_t>(current_ts),
                                           expire_ts_tmp))
        {
            output->OnError("ERR invalid expire time in 'psetex' command");
            return {false, EloqKey(), SetCommand()};
        }

        uint64_t expire_ts =
            (current_ts + expire_ts_tmp) < 0 ? 0 : (current_ts + expire_ts_tmp);
        return {
            true, EloqKey(args[1]), SetCommand(args[3], OBJ_SET_EX, expire_ts)};
    }

    if (args.size() < 3ul)
    {
        output->OnError("ERR wrong number of args for 'set' command");
        return {false, EloqKey(), SetCommand()};
    }
    if (args.size() == 3ul)
    {
        return {true, EloqKey(args[1]), SetCommand(args[2])};
    }

    uint64_t expire_ts = UINT64_MAX;

    for (size_t i = 3; i < args.size(); ++i)
    {
        if (args[i].length() == 2 && (args[i][0] == 'N' || args[i][0] == 'n') &&
            (args[i][1] == 'X' || args[i][1] == 'x') && !(flag & OBJ_SET_XX))
        {
            flag |= OBJ_SET_NX;
        }
        else if (args[i].length() == 2 &&
                 (args[i][0] == 'X' || args[i][0] == 'x') &&
                 (args[i][1] == 'X' || args[i][1] == 'x') &&
                 !(flag & OBJ_SET_NX))
        {
            flag |= OBJ_SET_XX;
        }
        else if (args[i].length() == 3 && stringcomp("get", args[i], 1))
        {
            flag |= OBJ_GET_SET;
        }
        else if (args[i].length() == 2 && stringcomp("px", args[i], 1))
        {
            flag |= OBJ_SET_EX;
            int64_t expire_ts_tmp;
            if (!string2ll(
                    args[i + 1].data(), args[i + 1].size(), expire_ts_tmp))
            {
                output->OnError(
                    redis_get_error_messages(RD_ERR_DIGITAL_INVALID));
                return {false, EloqKey(), SetCommand()};
            }

            uint64_t current_ts =
                txservice::LocalCcShards::ClockTsInMillseconds();

            if (EloqKV::will_addition_overflow(static_cast<int64_t>(current_ts),
                                               expire_ts_tmp))
            {
                output->OnError("ERR invalid expire time in 'psetex' command");
                return {false, EloqKey(), SetCommand()};
            }

            expire_ts = (current_ts + expire_ts_tmp) < 0
                            ? 0
                            : (current_ts + expire_ts_tmp);
            i++;
        }
        else if (args[i].length() == 2 && stringcomp("ex", args[i], 1))
        {
            flag |= OBJ_SET_EX;
            int64_t expire_ts_tmp;
            if (!string2ll(
                    args[i + 1].data(), args[i + 1].size(), expire_ts_tmp))
            {
                output->OnError(" ERR invalid expire time in 'set' command");
                return {false, EloqKey(), SetCommand()};
            }

            uint64_t current_ts =
                txservice::LocalCcShards::ClockTsInMillseconds();

            if (EloqKV::will_multiplication_overflow(expire_ts_tmp, 1000ll))
            {
                output->OnError("ERR invalid expire time in 'set' command");
                return {false, EloqKey(), SetCommand()};
            }
            expire_ts_tmp = expire_ts_tmp * 1000;

            if (EloqKV::will_addition_overflow(static_cast<int64_t>(current_ts),
                                               expire_ts_tmp))
            {
                output->OnError("ERR invalid expire time in 'set' command");
                return {false, EloqKey(), SetCommand()};
            }

            expire_ts = (current_ts + expire_ts_tmp) < 0
                            ? 0
                            : (current_ts + expire_ts_tmp);
            i++;
        }
        else if (args[i].length() == 4 && stringcomp("exat", args[i], 1))
        {
            flag |= OBJ_SET_EX;
            int64_t expire_ts_tmp;
            if (!string2ll(
                    args[i + 1].data(), args[i + 1].size(), expire_ts_tmp))
            {
                output->OnError(" ERR invalid expire time in 'set' command");
                return {false, EloqKey(), SetCommand()};
            }

            if (EloqKV::will_multiplication_overflow(expire_ts_tmp, 1000ll))
            {
                output->OnError("ERR invalid expire time in 'set' command");
                return {false, EloqKey(), SetCommand()};
            }
            expire_ts_tmp = expire_ts_tmp * 1000;

            // expire immediately if expire ts is negative
            expire_ts = expire_ts_tmp < 0 ? 0 : expire_ts_tmp;

            i++;
        }
        else if (args[i].length() == 4 && stringcomp("pxat", args[i], 1))
        {
            flag |= OBJ_SET_EX;
            int64_t expire_ts_tmp;
            if (!string2ll(
                    args[i + 1].data(), args[i + 1].size(), expire_ts_tmp))
            {
                output->OnError(" ERR invalid expire time in 'set' command");
                return {false, EloqKey(), SetCommand()};
            }

            // expire immediately if expire ts is negative
            expire_ts = expire_ts_tmp < 0 ? 0 : expire_ts_tmp;

            i++;
        }
        else if (args[i].length() == 7 && stringcomp("keepttl", args[i], 1))
        {
            flag |= OBJ_SET_KEEPTTL;
            i++;
        }
        else
        {
            output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
            return {false, EloqKey(), SetCommand()};
        }
    }
    return {true, EloqKey(args[1]), SetCommand(args[2], flag, expire_ts)};
}

std::tuple<bool, EloqKey, StrLenCommand> ParseStrLenCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "strlen");
    if (args.size() != 2ul)
    {
        output->OnError("ERR wrong number of arguments for 'strlen' command");
        return {false, EloqKey(), StrLenCommand()};
    }
    return {true, EloqKey(args[1]), StrLenCommand()};
}

std::tuple<bool, EloqKey, GetBitCommand> ParseGetBitCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "getbit");
    if (args.size() != 3ul)
    {
        output->OnError("ERR wrong number of arguments for 'getbit' command");
        return {false, EloqKey(), GetBitCommand()};
    }

    int64_t offset;
    if (!string2ll(args[2].data(), args[2].size(), offset) || offset < 0 ||
        offset >= 4 * 1024 * 1024 * 1024LL)
    {
        output->OnError("ERR bit offset is not an integer or out of range");
        return {false, EloqKey(), GetBitCommand()};
    }

    return {true, EloqKey(args[1]), GetBitCommand(offset)};
}

std::tuple<bool, EloqKey, GetRangeCommand> ParseGetRangeCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "getrange");
    if (args.size() != 4ul)
    {
        output->OnError("ERR wrong number of arguments for 'getrange' command");
        return {false, EloqKey(), GetRangeCommand()};
    }

    int64_t start, end;
    if (!string2ll(args[2].data(), args[2].size(), start) ||
        !string2ll(args[3].data(), args[3].size(), end))
    {
        output->OnError(redis_get_error_messages(RD_ERR_DIGITAL_INVALID));
        return {false, EloqKey(), GetRangeCommand()};
    }

    return {true, EloqKey(args[1]), GetRangeCommand(start, end)};
}

std::tuple<bool, EloqKey, SetBitCommand> ParseSetBitCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "setbit");
    if (args.size() != 4ul)
    {
        output->OnError("ERR wrong number of arguments for 'setbit' command");
        return {false, EloqKey(), SetBitCommand()};
    }

    int64_t offset;
    if (!string2ll(args[2].data(), args[2].size(), offset) || offset < 0 ||
        static_cast<uint64_t>(offset) >= MAX_OBJECT_SIZE * 8)
    {
        output->OnError("ERR bit offset is not an integer or out of range");
        return {false, EloqKey(), SetBitCommand()};
    }

    int8_t val = -1;
    if (args[3].size() == 1)
    {
        if (args[3][0] == '1')
        {
            val = 1;
        }
        else if (args[3][0] == '0')
        {
            val = 0;
        }
    }
    if (val < 0)
    {
        output->OnError("ERR bit offset is not an integer or out of range");
        return {false, EloqKey(), SetBitCommand()};
    }

    return {true, EloqKey(args[1]), SetBitCommand(offset, val)};
}

std::tuple<bool, EloqKey, SetRangeCommand> ParseSetRangeCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "setrange");
    if (args.size() != 4ul)
    {
        output->OnError("ERR wrong number of arguments for 'setrange' command");
        return {false, EloqKey(), SetRangeCommand()};
    }

    int64_t offset;
    if (!string2ll(args[2].data(), args[2].size(), offset))
    {
        output->OnError(redis_get_error_messages(RD_ERR_DIGITAL_INVALID));
        return {false, EloqKey(), SetRangeCommand()};
    }

    if (offset < 0)
    {
        output->OnError("ERR offset is out of range");
        return {false, EloqKey(), SetRangeCommand()};
    }

    if (offset + args[3].size() > MAX_OBJECT_SIZE)
    {
        output->OnError(
            "ERR string exceeds maximum allowed size (MAX_OBJECT_SIZE)");
        return {false, EloqKey(), SetRangeCommand()};
    }

    return {true, EloqKey(args[1]), SetRangeCommand(offset, args[3])};
}

std::tuple<bool, EloqKey, BitCountCommand> ParseBitCountCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "bitcount");
    if (args.size() < 2)
    {
        output->OnError("ERR wrong number of arguments for 'bitcount' command");
        return {false, EloqKey(), BitCountCommand()};
    }

    if (args.size() != 2)
    {
        if (args.size() < 4 || args.size() > 5)
        {
            output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
            return {false, EloqKey(), BitCountCommand()};
        }

        assert(args.size() >= 4);
        int64_t start_offset = 0;
        if (!string2ll(args[2].data(), args[2].size(), start_offset))
        {
            output->OnError(redis_get_error_messages(RD_ERR_DIGITAL_INVALID));
            return {false, EloqKey(), BitCountCommand()};
        }

        int64_t end_offset = 0;
        if (!string2ll(args[3].data(), args[3].size(), end_offset))
        {
            output->OnError(redis_get_error_messages(RD_ERR_DIGITAL_INVALID));
            return {false, EloqKey(), BitCountCommand()};
        }

        BitCountCommand::OffsetType type = BitCountCommand::OffsetType::BYTE;
        if (args.size() == 5)
        {
            if (stringcomp(args[4], "bit", 1))
            {
                type = BitCountCommand::OffsetType::BIT;
            }
            else if (stringcomp(args[4], "byte", 1))
            {
                type = BitCountCommand::OffsetType::BYTE;
            }
            else
            {
                output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
                return {false, EloqKey(), BitCountCommand()};
            }
        }

        return {true,
                EloqKey(args[1]),
                BitCountCommand(start_offset, end_offset, type)};
    }

    return {true,
            EloqKey(args[1]),
            BitCountCommand(0, INT64_MAX, BitCountCommand::OffsetType::BYTE)};
}

std::tuple<bool, EloqKey, AppendCommand> ParseAppendCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "append");
    if (args.size() != 3)
    {
        output->OnError("ERR wrong number of arguments for 'append' command");
        return {false, EloqKey(), AppendCommand()};
    }

    return {true, EloqKey(args[1]), AppendCommand(args[2])};
}

std::tuple<bool, EloqKey, BitFieldCommand> ParseBitFieldCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "bitfield");

    std::vector<BitFieldCommand::SubCommand> vct_cmd;
    BitFieldCommand::Overflow ovf = BitFieldCommand::Overflow::WRAP;
    for (size_t i = 2; i < args.size();)
    {
        BitFieldCommand::SubCommand scmd;
        bool bovf = false;

        if (stringcomp(args[i], "overflow", 1))
        {
            if (i + 2 >= args.size())
            {
                output->OnError("ERR syntax error");
                return {false, EloqKey(), BitFieldCommand()};
            }

            i++;
            if (stringcomp(args[i], "wrap", 1))
            {
                ovf = BitFieldCommand::Overflow::WRAP;
            }
            else if (stringcomp(args[i], "sat", 1))
            {
                ovf = BitFieldCommand::Overflow::SAT;
            }
            else if (stringcomp(args[i], "fail", 1))
            {
                ovf = BitFieldCommand::Overflow::FAIL;
            }
            else
            {
                output->OnError("ERR Invalid OVERFLOW type specified");
                return {false, EloqKey(), BitFieldCommand()};
            }
            i++;
            if (i >= args.size())
            {
                break;
            }
            bovf = true;
        }
        scmd.ovf_ = ovf;

        if (stringcomp(args[i], "get", 1))
        {
            if (bovf)
            {
                output->OnError("ERR syntax error");
                return {false, EloqKey(), BitFieldCommand()};
            }

            scmd.op_type_ = BitFieldCommand::OpType::GET;
        }
        else if (stringcomp(args[i], "set", 1))
        {
            scmd.op_type_ = BitFieldCommand::OpType::SET;
        }
        else if (stringcomp(args[i], "incrby", 1))
        {
            scmd.op_type_ = BitFieldCommand::OpType::INCR;
        }
        else
        {
            output->OnError("ERR syntax error");
            return {false, EloqKey(), BitFieldCommand()};
        }

        if ((scmd.op_type_ == BitFieldCommand::OpType::GET &&
             i + 3 > args.size()) ||
            (scmd.op_type_ != BitFieldCommand::OpType::GET &&
             i + 4 > args.size()))
        {
            output->OnError("ERR syntax error");
            return {false, EloqKey(), BitFieldCommand()};
        }

        i++;
        char c = args[i][0];
        if (c == 'U' || c == 'u')
        {
            scmd.encoding_i_ = false;
        }
        else if (c == 'I' || c == 'i')
        {
            scmd.encoding_i_ = true;
        }
        else
        {
            output->OnError(
                "ERR Invalid bitfield type. Use something like i16 u8. "
                "Note "
                "that u64 is not supported but i64 is.");
            return {false, EloqKey(), BitFieldCommand()};
        }

        int64_t num;
        if (!string2ll(args[i].data() + 1, args[i].size() - 1, num) ||
            num <= 0 || num > 64 || (num >= 64 && !scmd.encoding_i_))
        {
            output->OnError(
                "ERR Invalid bitfield type. Use something like i16 u8. "
                "Note "
                "that u64 is not supported but i64 is.");
            return {false, EloqKey(), BitFieldCommand()};
        }
        scmd.encoding_bits_ = static_cast<int8_t>(num);

        i++;
        if (args[i][0] == '#')
        {
            if (!string2ll(args[i].data() + 1, args[i].size() - 1, num) ||
                num >= (UINT32_MAX >> 3))
            {
                output->OnError(
                    "ERR bit offset is not an integer or out of range");
                return {false, EloqKey(), BitFieldCommand()};
            }

            scmd.offset_ = num * scmd.encoding_bits_;
        }
        else
        {
            if (!string2ll(args[i].data(), args[i].size(), num) ||
                num >= UINT32_MAX)
            {
                output->OnError(
                    "ERR bit offset is not an integer or out of range");
                return {false, EloqKey(), BitFieldCommand()};
            }

            scmd.offset_ = num;
        }

        i++;
        if (scmd.op_type_ == BitFieldCommand::OpType::GET)
        {
            vct_cmd.push_back(scmd);
            continue;
        }

        if (!string2ll(args[i].data(), args[i].size(), num))
        {
            output->OnError("ERR bit offset is not an integer or out of range");
            return {false, EloqKey(), BitFieldCommand()};
        }
        scmd.value_ = num;

        vct_cmd.push_back(scmd);
        i++;
    }

    return {true, EloqKey(args[1]), BitFieldCommand(std::move(vct_cmd))};
}

std::tuple<bool, EloqKey, BitFieldCommand> ParseBitFieldRoCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "bitfield_ro");

    std::vector<BitFieldCommand::SubCommand> vct_cmd;
    for (size_t i = 2; i < args.size();)
    {
        BitFieldCommand::SubCommand scmd;

        if (stringcomp(args[i], "overflow", 1))
        {
            if (i + 2 >= args.size())
            {
                output->OnError("ERR syntax error");
                return {false, EloqKey(), BitFieldCommand()};
            }

            i++;
            if (stringcomp(args[i], "wrap", 1))
            {
                scmd.ovf_ = BitFieldCommand::Overflow::WRAP;
            }
            else if (stringcomp(args[i], "sat", 1))
            {
                scmd.ovf_ = BitFieldCommand::Overflow::SAT;
            }
            else if (stringcomp(args[i], "fail", 1))
            {
                scmd.ovf_ = BitFieldCommand::Overflow::FAIL;
            }
            else
            {
                output->OnError("ERR Invalid OVERFLOW type specified");
                return {false, EloqKey(), BitFieldCommand()};
            }

            i++;
            if (i >= args.size())
            {
                break;
            }
        }

        if (stringcomp(args[i], "get", 1))
        {
        }
        else if (stringcomp(args[i], "set", 1) ||
                 stringcomp(args[i], "incrby", 1))
        {
            output->OnError("ERR BITFIELD_RO only supports the GET subcommand");
            return {false, EloqKey(), BitFieldCommand()};
        }
        else
        {
            output->OnError("ERR syntax error");
            return {false, EloqKey(), BitFieldCommand()};
        }
        scmd.op_type_ = BitFieldCommand::OpType::GET;

        if ((scmd.op_type_ == BitFieldCommand::OpType::GET &&
             i + 3 > args.size()) ||
            (scmd.op_type_ != BitFieldCommand::OpType::GET &&
             i + 4 > args.size()))
        {
            output->OnError("ERR syntax error");
            return {false, EloqKey(), BitFieldCommand()};
        }

        i++;
        char c = args[i][0];
        if (c == 'U' || c == 'u')
        {
            scmd.encoding_i_ = false;
        }
        else if (c == 'I' || c == 'i')
        {
            scmd.encoding_i_ = true;
        }
        else
        {
            output->OnError(
                "ERR Invalid bitfield type. Use something like i16 u8. "
                "Note "
                "that u64 is not supported but i64 is.");
            return {false, EloqKey(), BitFieldCommand()};
        }

        int64_t num;
        if (!string2ll(args[i].data() + 1, args[i].size() - 1, num) ||
            num <= 0 || num > 64 || (num >= 64 && !scmd.encoding_i_))
        {
            output->OnError(
                "ERR Invalid bitfield type. Use something like i16 u8. "
                "Note "
                "that u64 is not supported but i64 is.");
            return {false, EloqKey(), BitFieldCommand()};
        }
        scmd.encoding_bits_ = static_cast<int8_t>(num);

        i++;
        if (args[i][0] == '#')
        {
            if (!string2ll(args[i].data() + 1, args[i].size() - 1, num) ||
                num >= (UINT32_MAX >> 3))
            {
                output->OnError(
                    "ERR bit offset is not an integer or out of range");
                return {false, EloqKey(), BitFieldCommand()};
            }

            scmd.offset_ = num * scmd.encoding_bits_;
        }
        else
        {
            if (!string2ll(args[i].data(), args[i].size(), num) ||
                num >= UINT32_MAX)
            {
                output->OnError(
                    "ERR bit offset is not an integer or out of range");
                return {false, EloqKey(), BitFieldCommand()};
            }

            scmd.offset_ = num;
        }

        vct_cmd.push_back(scmd);
        i++;
    }

    return {true, EloqKey(args[1]), BitFieldCommand(std::move(vct_cmd))};
}

std::tuple<bool, EloqKey, BitPosCommand> ParseBitPosCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "bitpos");

    if (args.size() < 3)
    {
        output->OnError("ERR wrong number of arguments for 'bitpos' command");
        return {false, EloqKey(), BitPosCommand()};
    }
    if (args.size() > 6)
    {
        output->OnError("ERR syntax error");
        return {false, EloqKey(), BitPosCommand()};
    }
    int64_t bit_val;
    int64_t start = 0;
    int64_t end = INT64_MAX;
    if (!string2ll(args[2].data(), args[2].size(), bit_val))
    {
        output->OnError("ERR value is not an integer or out of range");
        return {false, EloqKey(), BitPosCommand()};
    }
    if (bit_val != 0 && bit_val != 1)
    {
        output->OnError("ERR The bit argument must be 1 or 0.");
        return {false, EloqKey(), BitPosCommand()};
    }

    if (args.size() >= 4 && !string2ll(args[3].data(), args[3].size(), start))
    {
        output->OnError("ERR value is not an integer or out of range");
        return {false, EloqKey(), BitPosCommand()};
    }

    if (args.size() >= 5 && !string2ll(args[4].data(), args[4].size(), end))
    {
        output->OnError("ERR value is not an integer or out of range");
        return {false, EloqKey(), BitPosCommand()};
    }

    bool is_byte = true;
    if (args.size() == 6)
    {
        if (stringcomp(args[5], "bit", 1))
        {
            is_byte = false;
        }
        else if (!stringcomp(args[5], "byte", 1))
        {
            output->OnError("ERR syntax error");
            return {false, EloqKey(), BitPosCommand()};
        }
    }

    if (is_byte)
    {
        start = start << 3;

        if (end != INT64_MAX)
        {
            end = end << 3;
            end += 7;
        }
    }

    return {true,
            EloqKey(args[1]),
            BitPosCommand(static_cast<uint8_t>(bit_val), start, end)};
}

std::tuple<bool, BitOpCommand> ParseBitOpCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "bitop");
    if (args.size() < 4)
    {
        output->OnError("ERR wrong number of arguments for 'bitop' command");
        return {false, BitOpCommand()};
    }

    BitOpCommand::OpType op_type;
    if (stringcomp(args[1], "and", 1))
    {
        op_type = BitOpCommand::OpType::And;
    }
    else if (stringcomp(args[1], "or", 1))
    {
        op_type = BitOpCommand::OpType::Or;
    }
    else if (stringcomp(args[1], "xor", 1))
    {
        op_type = BitOpCommand::OpType::Xor;
    }
    else if (stringcomp(args[1], "not", 1))
    {
        op_type = BitOpCommand::OpType::Not;
        if (args.size() != 4)
        {
            output->OnError(
                "ERR BITOP NOT must be called with a single source key.");
            return {false, BitOpCommand()};
        }
    }
    else
    {
        output->OnError("ERR syntax error");
        return {false, BitOpCommand()};
    }

    std::vector<EloqKey> s_keys;
    std::vector<GetCommand> s_cmds;
    s_keys.reserve(args.size() - 3);
    s_cmds.reserve(args.size() - 3);

    for (size_t i = 3; i < args.size(); ++i)
    {
        s_keys.emplace_back(args[i]);
        s_cmds.emplace_back();
    }

    return {true,
            BitOpCommand(std::move(s_keys),
                         std::move(s_cmds),
                         std::make_unique<EloqKey>(args[2]),
                         std::make_unique<SetCommand>(),
                         std::make_unique<DelCommand>(),
                         op_type)};
}

std::tuple<bool, EloqKey, GetRangeCommand> ParseSubStrCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "substr");
    if (args.size() != 4)
    {
        output->OnError("ERR wrong number of arguments for 'substr' command");
        return {false, EloqKey(), GetRangeCommand()};
    }

    int64_t start = 0;
    int64_t end = 0;
    if (!string2ll(args[2].data(), args[2].size(), start) ||
        !string2ll(args[3].data(), args[3].size(), end))
    {
        output->OnError(redis_get_error_messages(RD_ERR_DIGITAL_INVALID));
        return {false, EloqKey(), GetRangeCommand()};
    }

    return {true, EloqKey(args[1]), GetRangeCommand(start, end)};
}

std::tuple<bool, EloqKey, FloatOpCommand> ParseIncrByFloatCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "incrbyfloat");
    if (args.size() != 3ul)
    {
        output->OnError(
            "ERR wrong number of arguments for 'incrbyfloat' command");
        return {false, EloqKey(), FloatOpCommand()};
    }

    long double val;
    if (!string2ld(args[2].data(), args[2].size(), val))
    {
        output->OnError(redis_get_error_messages(RD_ERR_FLOAT_VALUE));
        return {false, EloqKey(), FloatOpCommand()};
    }

    return {true, EloqKey(args[1]), FloatOpCommand(val)};
}

std::tuple<bool, EloqKey, LRangeCommand> ParseLRangeCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "lrange");
    if (args.size() != 4ul)
    {
        output->OnError("ERR wrong number of arguments for 'lrange' command");
        return {false, EloqKey(), LRangeCommand()};
    }

    int64_t indexes[2];
    if (!string2ll(args[2].data(), args[2].size(), indexes[0]) ||
        !string2ll(args[3].data(), args[3].size(), indexes[1]))
    {
        output->OnError(redis_get_error_messages(RD_ERR_DIGITAL_INVALID));
        return {false, EloqKey(), LRangeCommand()};
    }

    return {true, args[1], LRangeCommand(indexes[0], indexes[1])};
}

std::tuple<bool, EloqKey, RPushCommand> ParseRPushCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "rpush");
    if (args.size() < 3)
    {
        output->OnError("ERR wrong number of arguments for 'rpush' command");
        return {false, EloqKey(), RPushCommand()};
    }

    std::vector<EloqString> elements;
    elements.reserve(args.size() - 2);
    for (auto arg_it = args.begin() + 2; arg_it != args.end(); ++arg_it)
    {
        elements.emplace_back(*arg_it);
    }

    return {true, args[1], RPushCommand(std::move(elements))};
}

std::tuple<bool, EloqKey, HSetCommand> ParseHSetCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "hset" || args[0] == "hmset");
    if (args.size() < 4 || IsOdd(args.size()))
    {
        const char fmt[] = "ERR wrong number of arguments for '%s' command";
        char buffer[sizeof(fmt) + 5] = {0};
        snprintf(buffer, sizeof(buffer), fmt, args[0].data());
        output->OnError(std::string_view(buffer));
        return {false, EloqKey(), HSetCommand()};
    }

    HSetCommand::SubType sub_type = args[0] == "hset"
                                        ? HSetCommand::SubType::HSET
                                        : HSetCommand::SubType::HMSET;

    std::vector<std::pair<EloqString, EloqString>> hset_command_elements;
    std::unordered_map<std::string_view, size_t> dup_map;

    size_t element_cnt = (args.size() - 2) / 2;
    hset_command_elements.reserve(element_cnt);
    dup_map.reserve(element_cnt);

    for (size_t i = 2; i < args.size(); i += 2)
    {
        auto iter = dup_map.find(args[i]);
        if (iter != dup_map.end())
        {
            hset_command_elements[iter->second].second =
                EloqString(args[i + 1]);

            continue;
        }

        dup_map.emplace(args[i], hset_command_elements.size());
        hset_command_elements.emplace_back(args[i], args[i + 1]);
    }

    return {true,
            EloqKey(args[1]),
            HSetCommand(sub_type, std::move(hset_command_elements))};
}

std::tuple<bool, EloqKey, HGetCommand> ParseHGetCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "hget");
    if (args.size() != 3)
    {
        output->OnError("ERR wrong number of arguments for 'hget' command");
        return {false, EloqKey(), HGetCommand()};
    }
    return {true, EloqKey(args[1]), HGetCommand(args[2])};
}

std::tuple<bool, EloqKey, LPushCommand> ParseLPushCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "lpush");
    if (args.size() < 3)
    {
        output->OnError("ERR wrong number of arguments for 'lpush' command");
        return {false, EloqKey(), LPushCommand()};
    }

    std::vector<EloqString> elements;
    elements.reserve(args.size() - 2);
    for (auto arg_it = args.begin() + 2; arg_it != args.end(); ++arg_it)
    {
        elements.emplace_back(*arg_it);
    }

    return {true, args[1], LPushCommand(std::move(elements))};
}

std::tuple<bool, EloqKey, LPopCommand> ParseLPopCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "lpop");
    if (args.size() < 2 || args.size() > 3)
    {
        output->OnError("ERR wrong number of arguments for 'lpop' command");
        return {false, EloqKey(), LPopCommand()};
    }

    int64_t count = -1;
    if (args.size() == 2)
    {
        return {true, args[1], LPopCommand(count)};
    }

    if (!string2ll(args[2].data(), args[2].size(), count) || count < 0)
    {
        output->OnError(redis_get_error_messages(RD_ERR_POSITIVE_INVALID));
        return {false, EloqKey(), LPopCommand()};
    }

    return {true, args[1], LPopCommand(count)};
}

std::tuple<bool, EloqKey, RPopCommand> ParseRPopCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "rpop");
    if (args.size() < 2 || args.size() > 3)
    {
        output->OnError("ERR wrong number of arguments for 'rpop' command");
        return {false, EloqKey(), RPopCommand()};
    }

    int64_t count = -1;
    if (args.size() == 2)
    {
        return {true, args[1], RPopCommand(count)};
    }

    if (!string2ll(args[2].data(), args[2].size(), count) || count < 0)
    {
        output->OnError(redis_get_error_messages(RD_ERR_POSITIVE_INVALID));
        return {false, EloqKey(), RPopCommand()};
    }

    return {true, args[1], RPopCommand(count)};
}

std::tuple<bool, LMPopCommand> ParseLMPopCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "lmpop");
    if (args.size() < 4)
    {
        output->OnError("ERR wrong number of arguments for 'lmpop' command");
        return {false, LMPopCommand()};
    }

    // parse numkeys
    uint64_t numkeys = 0;
    if (!string2ull(args[1].data(), args[1].size(), numkeys) || numkeys <= 0)
    {
        output->OnError(redis_get_error_messages(RD_ERR_NUMBER_GREATER_ZERO));
        return {false, LMPopCommand()};
    }

    // parse direction
    if (args.size() < numkeys + 3)
    {
        output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
        return {false, LMPopCommand()};
    }
    bool from_left = true;
    if (stringcomp("left", args[2 + numkeys], 1))
    {
        from_left = true;
    }
    else if (stringcomp("right", args[2 + numkeys], 1))
    {
        from_left = false;
    }
    else
    {
        output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
        return {false, LMPopCommand()};
    }

    // parse count
    int64_t count = 1;
    if (args.size() > numkeys + 3)
    {
        if (args.size() != numkeys + 5 ||
            !stringcomp("count", args[3 + numkeys], 1))
        {
            output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
            return {false, LMPopCommand()};
        }
        if (!string2ll(
                args[4 + numkeys].data(), args[4 + numkeys].size(), count) ||
            count <= 0)
        {
            output->OnError(
                redis_get_error_messages(RD_ERR_COUNT_GREATER_ZERO));
            return {false, LMPopCommand()};
        }
    }

    // parse keys
    std::vector<EloqKey> vct_key;
    vct_key.reserve(numkeys);
    for (uint64_t i = 2; i < 2 + numkeys; i++)
    {
        vct_key.emplace_back(args[i]);
    }

    return {true, LMPopCommand(std::move(vct_key), from_left, count)};
}

std::tuple<bool, EloqKey, IntOpCommand> ParseIncrCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "incr");
    if (args.size() != 2ul)
    {
        output->OnError("ERR wrong number of arguments for 'incr' command");
        return {false, EloqKey(), IntOpCommand()};
    }

    return {true, EloqKey(args[1]), IntOpCommand(1)};
}

std::tuple<bool, EloqKey, IntOpCommand> ParseDecrCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "decr");
    if (args.size() != 2ul)
    {
        output->OnError("ERR wrong number of arguments for 'decr' command");
        return {false, EloqKey(), IntOpCommand()};
    }
    return {true, EloqKey(args[1]), IntOpCommand(-1)};
}

std::tuple<bool, EloqKey, IntOpCommand> ParseIncrByCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "incrby");
    if (args.size() != 3ul)
    {
        output->OnError("ERR wrong number of arguments for 'incrby' command");
        return {false, EloqKey(), IntOpCommand()};
    }

    int64_t incr;
    if (!string2ll(args[2].data(), args[2].size(), incr))
    {
        output->OnError(redis_get_error_messages(RD_ERR_DIGITAL_INVALID));
        return {false, EloqKey(), IntOpCommand()};
    }

    return {true, EloqKey(args[1]), IntOpCommand(incr)};
}

std::tuple<bool, EloqKey, IntOpCommand> ParseDecrByCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "decrby");
    if (args.size() != 3ul)
    {
        output->OnError("ERR wrong number of arguments for 'decrby' command");
        return {false, EloqKey(), IntOpCommand()};
    }

    int64_t decr;
    if (!string2ll(args[2].data(), args[2].size(), decr))
    {
        output->OnError(redis_get_error_messages(RD_ERR_DIGITAL_INVALID));
        return {false, EloqKey(), IntOpCommand()};
    }

    return {true, EloqKey(args[1]), IntOpCommand(-decr)};
}

// Zset
std::unique_ptr<txservice::TxRecord> ZsetCommand::CreateObject(
    const std::string *image) const
{
    auto tmp_obj = std::make_unique<RedisZsetObject>();
    if (image != nullptr)
    {
        size_t offset = 0;
        tmp_obj->Deserialize(image->data(), offset);
        assert(offset == image->size());
    }
    return tmp_obj;
}

bool ZsetCommand::CheckTypeMatch(const txservice::TxRecord &obj)
{
    const auto &redis_obj = dynamic_cast<const RedisEloqObject &>(obj);
    return redis_obj.ObjectType() == RedisObjectType::Zset;
}

std::tuple<bool, EloqKey, ZAddCommand> ParseZAddCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "zadd");
    if (args.size() < 4)
    {
        output->OnError("ERR wrong number of arguments for 'zadd' command");
        return {false, EloqKey(), ZAddCommand()};
    }
    ZParams zparams;
    size_t i = 2;

    // All parameters will be parsed here, the first one don't match here
    // will be recognized as double, and if isn't double.It will break and
    // return error
    for (; i < args.size() - 1; ++i)
    {
        if (stringcomp(args[i], "xx", 1))
        {
            zparams.flags |= ZADD_IN_XX;  // update only
        }
        else if (stringcomp(args[i], "nx", 1))
        {
            zparams.flags |= ZADD_IN_NX;  // add new only.
        }
        else if (stringcomp(args[i], "gt", 1))
        {
            zparams.flags |= ZADD_IN_GT;
        }
        else if (stringcomp(args[i], "lt", 1))
        {
            zparams.flags |= ZADD_IN_LT;
        }
        else if (stringcomp(args[i], "ch", 1))
        {
            zparams.flags |= ZADD_OUT_CH;
        }
        else if (stringcomp(args[i], "incr", 1))
        {
            zparams.flags |= ZADD_IN_INCR;
        }
        else
        {
            break;
        }
    }

    if (zparams.XX() && zparams.NX())
    {
        output->OnError(redis_get_error_messages(RD_ERR_XX_NX));
        return {false, EloqKey(), ZAddCommand()};
    }

    if (zparams.NX() && (zparams.GT() || zparams.LT()))
    {
        output->OnError(redis_get_error_messages(RD_ERR_LT_GT_NX));
        return {false, EloqKey(), ZAddCommand()};
    }

    if (zparams.GT() && zparams.LT())
    {
        output->OnError(redis_get_error_messages(RD_ERR_LT_GT_NX));
        return {false, EloqKey(), ZAddCommand()};
    }

    if (IsOdd(args.size() - i))
    {
        output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
        return {false, EloqKey(), ZAddCommand()};
    }
    std::vector<std::pair<double, EloqString>> elements;
    elements.reserve((args.size() - i) / 2);

    // parse number of zadd
    for (; i < args.size(); i += 2)
    {
        double num;
        if (args[i] == "+inf" || args[i] == "inf")
        {
            num = std::numeric_limits<double>::infinity();
        }
        else if (args[i] == "-inf")
        {
            num = std::numeric_limits<double>::infinity() * -1;
        }
        else
        {
            if (!string2double(args[i].begin(), args[i].size(), num))
            {
                output->OnError(redis_get_error_messages(RD_ERR_FLOAT_VALUE));
                return {false, EloqKey(), ZAddCommand()};
            }
        }
        elements.emplace_back(num, args[i + 1]);
    }

    if (elements.empty())
    {
        output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
        return {false, EloqKey(), ZAddCommand()};
    }
    if (zparams.INCR())
    {
        if (elements.size() > 1 || elements.empty())
        {
            output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
            return {false, EloqKey(), ZAddCommand()};
        }
        else
        {
            return {true,
                    EloqKey(args[1]),
                    ZAddCommand(std::move(*elements.begin()),
                                zparams,
                                ZAddCommand::ElementType::pair)};
        }
    }
    else
    {
        return {true,
                EloqKey(args[1]),
                ZAddCommand(std::move(elements),
                            zparams,
                            ZAddCommand::ElementType::vector)};
    }
}

void ZPopCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type;
    if (pop_type_ == PopType::POPMIN)
    {
        cmd_type = static_cast<uint8_t>(RedisCommandType::ZPOPMIN);
    }
    else
    {
        assert(pop_type_ == PopType::POPMAX);
        cmd_type = static_cast<uint8_t>(RedisCommandType::ZPOPMAX);
    }

    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
    str.append(reinterpret_cast<const char *>(&count_), sizeof(int64_t));
}

void ZPopCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    count_ = *reinterpret_cast<const int64_t *>(ptr);
    ptr += sizeof(int64_t);
}

void ZPopCommand::OutputResult(OutputHandler *reply) const
{
    if (zset_result_.err_code_ == RD_OK)
    {
        auto &result_vector =
            std::get<std::vector<EloqString>>(zset_result_.result_);
        reply->OnArrayStart(result_vector.size());

        for (const auto &result : result_vector)
        {
            reply->OnString(result.StringView());
        }

        reply->OnArrayEnd();
    }
    else if (zset_result_.err_code_ == RD_NIL)
    {
        reply->OnArrayStart(0);
        reply->OnArrayEnd();
    }
    else
    {
        reply->OnError(redis_get_error_messages(zset_result_.err_code_));
    }
}

txservice::ExecResult ZPopCommand::ExecuteOn(const txservice::TxObject &object)
{
    if (!CheckTypeMatch(object))
    {
        zset_result_.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const RedisZsetObject &zset_obj =
        static_cast<const RedisZsetObject &>(object);
    return zset_obj.Execute(*this) ? txservice::ExecResult::Write
                                   : txservice::ExecResult::Fail;
}

txservice::TxObject *ZPopCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    auto &zset_obj = static_cast<RedisZsetObject &>(*obj_ptr);
    bool empty_after_removal = zset_obj.CommitZPop(pop_type_, count_);
    return empty_after_removal ? nullptr : obj_ptr;
}

txservice::ExecResult ZLexCountCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    if (!CheckTypeMatch(object))
    {
        zset_result_.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const RedisZsetObject &zset_obj =
        static_cast<const RedisZsetObject &>(object);
    zset_obj.Execute(*this);
    return txservice::ExecResult::Read;
}

void ZLexCountCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::ZLEXCOUNT);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(cmd_type));

    str.append(reinterpret_cast<const char *>(&spec_.null_), sizeof(bool));
    str.append(reinterpret_cast<const char *>(&spec_.minex_), sizeof(bool));
    str.append(reinterpret_cast<const char *>(&spec_.maxex_), sizeof(bool));
    str.append(reinterpret_cast<const char *>(&spec_.negative_), sizeof(bool));
    str.append(reinterpret_cast<const char *>(&spec_.positive_), sizeof(bool));

    if (std::holds_alternative<EloqString>(spec_.opt_start_))
    {
        EloqString sval = std::get<EloqString>(spec_.opt_start_);
        uint32_t len = sval.Length();
        str.append(reinterpret_cast<const char *>(&len), sizeof(uint32_t));
        str.append(sval.Data(), len);
    }
    else if (!spec_.null_)
    {
        assert(false && " not null and min or max not valid string range item");
    }

    if (std::holds_alternative<EloqString>(spec_.opt_end_))
    {
        EloqString sval = std::get<EloqString>(spec_.opt_end_);
        uint32_t len = sval.Length();
        str.append(reinterpret_cast<const char *>(&len), sizeof(uint32_t));
        str.append(sval.Data(), len);
    }
    else if (!spec_.null_)
    {
        assert(false && " not null min or max not valid string range item");
    }
}

void ZLexCountCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();

    spec_.null_ = *reinterpret_cast<const bool *>(ptr);
    ptr += sizeof(bool);

    if (spec_.null_)
    {
        return;
    }

    spec_.minex_ = *reinterpret_cast<const bool *>(ptr);
    ptr += sizeof(bool);

    spec_.maxex_ = *reinterpret_cast<const bool *>(ptr);
    ptr += sizeof(bool);

    spec_.negative_ = *reinterpret_cast<const bool *>(ptr);
    ptr += sizeof(bool);

    spec_.positive_ = *reinterpret_cast<const bool *>(ptr);
    ptr += sizeof(bool);

    uint32_t len = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);

    spec_.opt_start_ = EloqString(ptr, len);
    ptr += len;

    len = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);

    spec_.opt_end_ = EloqString(ptr, len);
    ptr += len;
}

void ZLexCountCommand::OutputResult(OutputHandler *reply) const
{
    if (zset_result_.err_code_ == RD_OK)
    {
        reply->OnInt(std::get<int>(zset_result_.result_));
    }
    else if (zset_result_.err_code_ == RD_NIL)
    {
        reply->OnInt(0);
    }
    else
    {
        reply->OnError(redis_get_error_messages(zset_result_.err_code_));
    }
}

txservice::ExecResult ZAddCommand::ExecuteOn(const txservice::TxObject &object)
{
    if (!params_.ForceClear() && !CheckTypeMatch(object))
    {
        zset_result_.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    if (params_.ForceClear())
    {
        if (type_ == ElementType::vector)
        {
            auto &vct =
                std::get<std::vector<std::pair<double, EloqString>>>(elements_);
            zset_result_.result_ = static_cast<int>(vct.size());
        }
        else
        {
            zset_result_.result_ = 0;
        }
        zset_result_.err_code_ = RD_OK;
        return txservice::ExecResult::Write;
    }
    const RedisZsetObject &zset_obj =
        static_cast<const RedisZsetObject &>(object);
    return zset_obj.Execute(*this) ? txservice::ExecResult::Write
                                   : txservice::ExecResult::Fail;
}

txservice::TxObject *ZAddCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    if (params_.ForceClear())
    {
        if (type_ == ElementType::monostate)
        {
            return nullptr;
        }

        auto new_obj_uptr = CreateObject(nullptr);
        RedisZsetObject *zset_obj =
            static_cast<RedisZsetObject *>(new_obj_uptr.get());
        zset_obj->CommitZAdd(elements_, params_, is_in_the_middle_stage_);
        return static_cast<txservice::TxObject *>(new_obj_uptr.release());
    }
    auto &zset_obj = static_cast<RedisZsetObject &>(*obj_ptr);
    zset_obj.CommitZAdd(elements_, params_, is_in_the_middle_stage_);
    return obj_ptr;
}

void ZAddCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::ZADD);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));

    str.append(reinterpret_cast<const char *>(&params_.flags), sizeof(uint8_t));

    auto SerializeElement = [&str](double num, const EloqString &field)
    {
        str.append(reinterpret_cast<const char *>(&num), sizeof(double));
        std::string_view sv = field.StringView();
        uint32_t size = sv.size();
        str.append(reinterpret_cast<const char *>(&size), sizeof(uint32_t));
        str.append(sv.data(), size);
    };

    uint8_t element_type = (uint8_t) type_;
    str.append(reinterpret_cast<const char *>(&element_type), sizeof(uint8_t));

    if (type_ == ElementType::vector)
    {
        auto &tmp =
            std::get<std::vector<std::pair<double, EloqString>>>(elements_);

        uint32_t ele_cnt = tmp.size();
        str.append(reinterpret_cast<const char *>(&ele_cnt), sizeof(uint32_t));
        for (auto &[num, field] : tmp)
        {
            SerializeElement(num, field);
        }
    }
    else if (type_ == ElementType::pair)
    {
        auto &tmp = std::get<std::pair<double, EloqString>>(elements_);
        SerializeElement(tmp.first, tmp.second);
    }
    else if (type_ == ElementType::monostate)
    {
        // no elementes to serialize
    }
}

void ZAddCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();

    params_.flags = *reinterpret_cast<const uint8_t *>(ptr);
    ptr += sizeof(uint8_t);

    uint8_t element_type = *reinterpret_cast<const uint8_t *>(ptr);
    ptr += sizeof(uint8_t);

    type_ = static_cast<ElementType>(element_type);

    if (type_ == ElementType::vector)
    {
        uint32_t ele_cnt = *reinterpret_cast<const uint32_t *>(ptr);
        ptr += sizeof(uint32_t);

        std::vector<std::pair<double, EloqString>> deserialize_elements;
        deserialize_elements.reserve(ele_cnt);

        for (uint32_t i = 0; i < ele_cnt; i++)
        {
            const double num = *reinterpret_cast<const double *>(ptr);
            ptr += sizeof(double);

            uint32_t size = *reinterpret_cast<const uint32_t *>(ptr);
            ptr += sizeof(uint32_t);
            deserialize_elements.emplace_back(num, EloqString(ptr, size));
            ptr += size;
        }

        elements_ = std::move(deserialize_elements);
    }
    else if (type_ == ElementType::pair)
    {
        const double num = *reinterpret_cast<const double *>(ptr);
        ptr += sizeof(double);

        uint32_t size = *reinterpret_cast<const uint32_t *>(ptr);
        ptr += sizeof(uint32_t);
        std::pair<double, EloqString> deserialize_element(
            num, EloqString(ptr, size));
        ptr += size;

        elements_ = std::move(deserialize_element);
    }
    else
    {
        elements_ = std::monostate{};
    }
}

void ZAddCommand::OutputResult(OutputHandler *reply) const
{
    if (zset_result_.err_code_ == RD_OK)
    {
        const auto &zset_result = zset_result_;
        std::visit(
            [&reply, &zset_result](auto &&arg)
            {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, int>)
                {
                    reply->OnInt(std::get<int>(zset_result.result_));
                }
                else if constexpr (std::is_same_v<T, EloqString>)
                {
                    reply->OnString(
                        std::get<EloqString>(zset_result.result_).StringView());
                }
                else
                {
                    assert(false);
                }
            },
            zset_result_.result_);
    }
    else if (zset_result_.err_code_ == RD_NIL)
    {
        if (params_.INCR())
        {
            reply->OnNil();
        }
        else
        {
            reply->OnInt(0);
        }
    }
    else
    {
        reply->OnError(redis_get_error_messages(zset_result_.err_code_));
    }
}

ZAddCommand::ZAddCommand(const ZAddCommand &rhs)
    : ZsetCommand(rhs),
      type_(rhs.type_),
      params_(rhs.params_),
      is_in_the_middle_stage_(rhs.is_in_the_middle_stage_)
{
    if (type_ == ElementType::vector)
    {
        auto &rhs_elements =
            std::get<std::vector<std::pair<double, EloqString>>>(rhs.elements_);

        std::vector<std::pair<double, EloqString>> elements;
        elements.reserve(rhs_elements.size());

        for (auto &[score, member] : rhs_elements)
        {
            elements.emplace_back(score, member.Clone());
        }

        elements_ = std::move(elements);
    }
    else if (type_ == ElementType::pair)
    {
        auto &[score, member] =
            std::get<std::pair<double, EloqString>>(rhs.elements_);

        elements_ = std::pair<double, EloqString>(score, member.Clone());
    }
    else
    {
        // monostate
        elements_ = std::monostate{};
    }
}

// first return can it successfully change
// second return whether it is excluded
std::tuple<bool, bool, int> GetIntFromSv(std::string_view sv)
{
    int64_t num;

    static_assert(std::numeric_limits<decltype(num)>::has_infinity == false);

    if (sv == "inf" || sv == "+inf" || sv == "-inf")
    {
        return {false, false, {}};
    }

    if (sv[0] == '(')
    {
        if (!string2ll(sv.data() + 1, sv.size() - 1, num))
        {
            return {false, true, {}};
        }
        else
        {
            return {true, true, num};
        }
    }
    else
    {
        if (!string2ll(sv.data(), sv.size(), num))
        {
            return {false, false, {}};
        }
        else
        {
            return {true, false, num};
        }
    }
}

// first return can it successfully change
// second return whether it is excluded
std::tuple<bool, bool, double> GetDoubleFromSv(std::string_view sv)
{
    double num;

    static_assert(std::numeric_limits<decltype(num)>::has_infinity);

    if (sv[0] == '(')
    {
        if (sv == "inf" || sv == "+inf")
        {
            return {true, true, std::numeric_limits<double>::infinity()};
        }

        if (sv == "-inf")
        {
            return {true, true, std::numeric_limits<double>::infinity() * -1};
        }

        if (!string2double(sv.data() + 1, sv.size() - 1, num))
        {
            return {false, true, {}};
        }
        else
        {
            return {true, true, num};
        }
    }
    else
    {
        if (sv == "inf" || sv == "+inf")
        {
            return {true, false, std::numeric_limits<double>::infinity()};
        }

        if (sv == "-inf")
        {
            return {true, false, std::numeric_limits<double>::infinity() * -1};
        }

        if (!string2double(sv.data(), sv.size(), num))
        {
            return {false, false, {}};
        }
        else
        {
            return {true, false, num};
        }
    }
}

// first return can it successfully change
// second return whether it is excluded
std::tuple<bool, bool, EloqString, bool> GetEloqStringFromSv(
    std::string_view sv)
{
    if (sv == "-" || sv == "+")
    {
        return std::make_tuple(true, true, EloqString(), true);
    }

    if (sv[0] == '(')
    {
        return std::make_tuple(
            true, true, EloqString(sv.substr(1, sv.size() - 1)), false);
    }
    else if (sv[0] == '[')
    {
        return std::make_tuple(
            true, false, EloqString(sv.substr(1, sv.size() - 1)), false);
    }
    else
    {
        return std::make_tuple(false, false, EloqString(), false);
    }
}

std::pair<bool, int> ParseZrangeElement(
    const std::vector<std::string_view> &args,
    ZRangeConstraint &constraint,
    ZRangeSpec &spec)
{
    int minidx = 2;
    int maxidx = 3;

    // parse zrange parameters(withscores/limit/rev/byscore/byrank/bylex)
    for (size_t i = 4; i < args.size(); i++)
    {
        if (stringcomp(args[i].data(), "withscores", 1))
        {
            if (args[0] == "zrangestore")
            {
                // zrangestore does not support withscores
                return {false, RD_ERR_SYNTAX};
            }
            constraint.opt_withscores_ = true;
        }
        else if (stringcomp(args[i].data(), "limit", 1) && i + 2 < args.size())
        {
            auto [ec_offset, bool_offset, num_offset] =
                GetIntFromSv(args[i + 1]);
            auto [ec_limit, bool_limit, num_limit] = GetIntFromSv(args[i + 2]);
            if (!ec_offset || !ec_limit)
            {
                return {false, RD_ERR_DIGITAL_INVALID};
            }
            else
            {
                constraint.opt_offset_ = num_offset;
                constraint.opt_limit_ = num_limit;
            }
            i += 2;
        }
        else if (constraint.direction_ ==
                     ZRangeDirection::ZRANGE_DIRECTION_AUTO &&
                 stringcomp(args[i].data(), "rev", 1))
        {
            if (args[0] == "zrangebyscore")
            {
                // zrangebyscore does not support rev
                return {false, RD_ERR_SYNTAX};
            }
            constraint.direction_ = ZRangeDirection::ZRANGE_DIRECTION_REVERSE;
        }
        else if (constraint.rangetype_ == ZRangeType::ZRANGE_AUTO &&
                 stringcomp(args[i].data(), "bylex", 1))
        {
            constraint.rangetype_ = ZRangeType::ZRANGE_LEX;
        }
        else if (constraint.rangetype_ == ZRangeType::ZRANGE_AUTO &&
                 stringcomp(args[i].data(), "byscore", 1))
        {
            constraint.rangetype_ = ZRangeType::ZRANGE_SCORE;
        }
        else
        {
            return {false, RD_ERR_SYNTAX};
        }
    }

    // check for conflicting arguments.
    if (constraint.direction_ == ZRangeDirection::ZRANGE_DIRECTION_AUTO)
    {
        constraint.direction_ = ZRangeDirection::ZRANGE_DIRECTION_FORWARD;
    }

    if (constraint.rangetype_ == ZRangeType::ZRANGE_AUTO)
    {
        constraint.rangetype_ = ZRangeType::ZRANGE_RANK;
    }

    if (constraint.opt_limit_ != -1 &&
        constraint.rangetype_ == ZRangeType::ZRANGE_RANK)
    {
        return {false, RD_ERR_LIMIT};
    }

    if (constraint.opt_withscores_ &&
        constraint.rangetype_ == ZRangeType::ZRANGE_LEX)
    {
        return {false, RD_ERR_WITHSCORE_BYLEX};
    }

    if (constraint.direction_ == ZRangeDirection::ZRANGE_DIRECTION_REVERSE &&
        (constraint.rangetype_ == ZRangeType::ZRANGE_SCORE ||
         constraint.rangetype_ == ZRangeType::ZRANGE_LEX))
    {
        std::swap(maxidx, minidx);
    }

    // parse the range
    switch (constraint.rangetype_)
    {
    case ZRangeType::ZRANGE_AUTO:
    case ZRangeType::ZRANGE_RANK:
    {
        auto [ec_rank_start, bool_rank_start, rank_start] =
            GetIntFromSv(args[minidx]);
        auto [ec_rank_end, bool_rank_end, rank_end] =
            GetIntFromSv(args[maxidx]);
        if (!ec_rank_start || !ec_rank_end)
        {
            return {false, RD_ERR_DIGITAL_INVALID};
        }
        spec.opt_start_ = rank_start;
        spec.opt_end_ = rank_end;
        spec.minex_ = bool_rank_start;
        spec.maxex_ = bool_rank_end;
        break;
    }

    case ZRangeType::ZRANGE_SCORE:
    {
        auto [ec_score_start, bool_score_start, score_start] =
            GetDoubleFromSv(args[minidx]);
        auto [ec_score_end, bool_score_end, score_end] =
            GetDoubleFromSv(args[maxidx]);
        if (!ec_score_start || !ec_score_end)
        {
            return {false, RD_ERR_MIN_MAX_NOT_FLOAT};
        }
        spec.opt_start_ = score_start;
        spec.opt_end_ = score_end;
        spec.minex_ = bool_score_start;
        spec.maxex_ = bool_score_end;
        break;
    }

    case ZRangeType::ZRANGE_LEX:
    {
        if (args[minidx] == "+" || args[maxidx] == "-")
        {
            spec.null_ = true;
            break;
        }
        auto [ec_lex_start, bool_lex_start, lex_start, negative] =
            GetEloqStringFromSv(args[minidx]);
        auto [ec_lex_end, bool_lex_end, lex_end, positive] =
            GetEloqStringFromSv(args[maxidx]);
        if (!ec_lex_start || !ec_lex_end)
        {
            return {false, RD_ERR_MIN_MAX_NOT_STRING};
        }

        spec.negative_ = negative;
        spec.positive_ = positive;
        spec.opt_start_.emplace<EloqString>(std::move(lex_start));
        spec.opt_end_.emplace<EloqString>(std::move(lex_end));
        spec.minex_ = bool_lex_start;
        spec.maxex_ = bool_lex_end;
        break;
    }
    default:
    {
        assert(false && "Unknown range type");
        break;
    }
    }
    return {true, {RD_OK}};
}

std::tuple<bool, EloqKey, ZRangeCommand> ParseZRangeCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "zrange");
    if (args.size() < 4)
    {
        output->OnError("ERR wrong number of arguments for 'zrange' command");
        return {false, EloqKey(), ZRangeCommand()};
    }

    ZRangeSpec spec;
    ZRangeConstraint constraint;

    auto [success, err_code] = ParseZrangeElement(args, constraint, spec);
    if (!success)
    {
        output->OnError(redis_get_error_messages(err_code));
        return {false, EloqKey(), ZRangeCommand()};
    }

    return {true, EloqKey(args[1]), ZRangeCommand(constraint, std::move(spec))};
}

txservice::ExecResult ZRangeCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    const auto &redis_obj = dynamic_cast<const RedisEloqObject &>(object);
    RedisObjectType type = redis_obj.ObjectType();

    if (type == RedisObjectType::Zset || type == RedisObjectType::TTLZset)
    {
        const RedisZsetObject &zset_obj =
            static_cast<const RedisZsetObject &>(object);
        zset_obj.Execute(*this);
    }
    else if (multi_type_range_ &&
             (type == RedisObjectType::Set || type == RedisObjectType::TTLSet))
    {
        const RedisHashSetObject &set_obj =
            static_cast<const RedisHashSetObject &>(object);
        set_obj.Execute(*this);
    }
    else
    {
        zset_result_.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    return txservice::ExecResult::Read;
}

void ZRangeCommand::Serialize(std::string &str) const
{
    uint8_t type = static_cast<uint8_t>(RedisCommandType::ZRANGE);
    str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));

    str.append(reinterpret_cast<const char *>(&constraint_.opt_withscores_),
               sizeof(bool));
    str.append(reinterpret_cast<const char *>(&constraint_.opt_offset_),
               sizeof(int));
    str.append(reinterpret_cast<const char *>(&constraint_.opt_limit_),
               sizeof(int));

    type = static_cast<uint8_t>(constraint_.rangetype_);
    str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));

    type = static_cast<uint8_t>(constraint_.direction_);
    str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));

    str.append(reinterpret_cast<const char *>(&spec_.minex_), sizeof(bool));
    str.append(reinterpret_cast<const char *>(&spec_.maxex_), sizeof(bool));
    str.append(reinterpret_cast<const char *>(&spec_.negative_), sizeof(bool));
    str.append(reinterpret_cast<const char *>(&spec_.positive_), sizeof(bool));

    if (std::holds_alternative<int>(spec_.opt_start_))
    {
        type = static_cast<uint8_t>(ResultType::Int);
        str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));
        int ival = std::get<int>(spec_.opt_start_);
        str.append(reinterpret_cast<const char *>(&ival), sizeof(int));
    }
    else if (std::holds_alternative<double>(spec_.opt_start_))
    {
        type = static_cast<uint8_t>(ResultType::Double);
        str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));
        double dval = std::get<double>(spec_.opt_start_);
        str.append(reinterpret_cast<const char *>(&dval), sizeof(double));
    }
    else if (std::holds_alternative<EloqString>(spec_.opt_start_))
    {
        type = static_cast<uint8_t>(ResultType::String);
        str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));
        EloqString sval = std::get<EloqString>(spec_.opt_start_);
        uint32_t len = sval.Length();
        str.append(reinterpret_cast<const char *>(&len), sizeof(uint32_t));
        str.append(sval.Data(), len);
    }
    else
    {
        assert(false);
    }

    if (std::holds_alternative<int>(spec_.opt_end_))
    {
        type = static_cast<uint8_t>(ResultType::Int);
        str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));
        int ival = std::get<int>(spec_.opt_end_);
        str.append(reinterpret_cast<const char *>(&ival), sizeof(int));
    }
    else if (std::holds_alternative<double>(spec_.opt_end_))
    {
        type = static_cast<uint8_t>(ResultType::Double);
        str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));
        double dval = std::get<double>(spec_.opt_end_);
        str.append(reinterpret_cast<const char *>(&dval), sizeof(double));
    }
    else if (std::holds_alternative<EloqString>(spec_.opt_end_))
    {
        type = static_cast<uint8_t>(ResultType::String);
        str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));
        EloqString sval = std::get<EloqString>(spec_.opt_end_);
        uint32_t len = sval.Length();
        str.append(reinterpret_cast<const char *>(&len), sizeof(uint32_t));
        str.append(sval.Data(), len);
    }
    else
    {
        assert(false);
    }

    str.append(reinterpret_cast<const char *>(&multi_type_range_),
               sizeof(bool));
}

void ZRangeCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();

    constraint_.opt_withscores_ = *reinterpret_cast<const bool *>(ptr);
    ptr += sizeof(bool);

    constraint_.opt_offset_ = *reinterpret_cast<const int *>(ptr);
    ptr += sizeof(int);

    constraint_.opt_limit_ = *reinterpret_cast<const int *>(ptr);
    ptr += sizeof(int);

    constraint_.rangetype_ =
        static_cast<ZRangeType>(*reinterpret_cast<const uint8_t *>(ptr));
    ptr += sizeof(uint8_t);

    constraint_.direction_ =
        static_cast<ZRangeDirection>(*reinterpret_cast<const uint8_t *>(ptr));
    ptr += sizeof(uint8_t);

    spec_.minex_ = *reinterpret_cast<const bool *>(ptr);
    ptr += sizeof(bool);
    spec_.maxex_ = *reinterpret_cast<const bool *>(ptr);
    ptr += sizeof(bool);
    spec_.negative_ = *reinterpret_cast<const bool *>(ptr);
    ptr += sizeof(bool);
    spec_.positive_ = *reinterpret_cast<const bool *>(ptr);
    ptr += sizeof(bool);

    ResultType type =
        static_cast<ResultType>(*reinterpret_cast<const uint8_t *>(ptr));
    ptr += sizeof(uint8_t);

    switch (type)
    {
    case ResultType::Int:
    {
        spec_.opt_start_ = *reinterpret_cast<const int *>(ptr);
        ptr += sizeof(int);
        break;
    }
    case ResultType::Double:
    {
        spec_.opt_start_ = *reinterpret_cast<const double *>(ptr);
        ptr += sizeof(double);
        break;
    }
    case ResultType::String:
    {
        uint32_t len = *reinterpret_cast<const uint32_t *>(ptr);
        ptr += sizeof(uint32_t);
        spec_.opt_start_ = EloqString(ptr, len);
        ptr += len;
        break;
    }
    default:
        assert(false);
    }

    type = static_cast<ResultType>(*reinterpret_cast<const uint8_t *>(ptr));
    ptr += sizeof(uint8_t);

    switch (type)
    {
    case ResultType::Int:
    {
        spec_.opt_end_ = *reinterpret_cast<const int *>(ptr);
        ptr += sizeof(int);
        break;
    }
    case ResultType::Double:
    {
        spec_.opt_end_ = *reinterpret_cast<const double *>(ptr);
        ptr += sizeof(double);
        break;
    }
    case ResultType::String:
    {
        uint32_t len = *reinterpret_cast<const uint32_t *>(ptr);
        ptr += sizeof(uint32_t);
        spec_.opt_end_ = EloqString(ptr, len);
        ptr += len;
        break;
    }
    default:
        assert(false);
    }

    multi_type_range_ = *reinterpret_cast<const bool *>(ptr);
    ptr += sizeof(bool);
}

void ZRangeCommand::OutputResult(OutputHandler *reply) const
{
    const auto &zset_result = zset_result_;
    if (zset_result.err_code_ == RD_OK)
    {
        reply->OnArrayStart(
            std::get<std::vector<EloqString>>(zset_result.result_).size());

        for (auto &it : std::get<std::vector<EloqString>>(zset_result.result_))
        {
            reply->OnString(it.StringView());
        }

        reply->OnArrayEnd();
    }
    else if (zset_result.err_code_ == RD_NIL)
    {
        reply->OnArrayStart(0);
        reply->OnArrayEnd();
    }
    else
    {
        reply->OnError(redis_get_error_messages(zset_result.err_code_));
    }
}

txservice::ExecResult ZRemRangeCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    if (!CheckTypeMatch(object))
    {
        zset_result_.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const RedisZsetObject &zset_obj =
        static_cast<const RedisZsetObject &>(object);
    return zset_obj.Execute(*this) ? txservice::ExecResult::Write
                                   : txservice::ExecResult::Fail;
}

txservice::TxObject *ZRemRangeCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    RedisZsetObject &zset_obj = static_cast<RedisZsetObject &>(*obj_ptr);
    bool empty_after_removal = zset_obj.CommitZRemRange(range_type_, spec_);
    return empty_after_removal ? nullptr : obj_ptr;
}

void ZRemRangeCommand::Serialize(std::string &str) const
{
    uint8_t type = static_cast<uint8_t>(RedisCommandType::ZREMRANGE);
    str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));

    type = static_cast<uint8_t>(range_type_);
    str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));

    str.append(reinterpret_cast<const char *>(&spec_.minex_), sizeof(bool));
    str.append(reinterpret_cast<const char *>(&spec_.maxex_), sizeof(bool));
    str.append(reinterpret_cast<const char *>(&spec_.negative_), sizeof(bool));
    str.append(reinterpret_cast<const char *>(&spec_.positive_), sizeof(bool));

    if (std::holds_alternative<int>(spec_.opt_start_))
    {
        type = static_cast<uint8_t>(ResultType::Int);
        str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));
        int ival = std::get<int>(spec_.opt_start_);
        str.append(reinterpret_cast<const char *>(&ival), sizeof(int));
    }
    else if (std::holds_alternative<double>(spec_.opt_start_))
    {
        type = static_cast<uint8_t>(ResultType::Double);
        str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));
        double dval = std::get<double>(spec_.opt_start_);
        str.append(reinterpret_cast<const char *>(&dval), sizeof(double));
    }
    else if (std::holds_alternative<EloqString>(spec_.opt_start_))
    {
        type = static_cast<uint8_t>(ResultType::String);
        str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));
        EloqString sval = std::get<EloqString>(spec_.opt_start_);
        uint32_t len = sval.Length();
        str.append(reinterpret_cast<const char *>(&len), sizeof(uint32_t));
        str.append(sval.Data(), len);
    }
    else
    {
        assert(false);
    }

    if (std::holds_alternative<int>(spec_.opt_end_))
    {
        type = static_cast<uint8_t>(ResultType::Int);
        str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));
        int ival = std::get<int>(spec_.opt_end_);
        str.append(reinterpret_cast<const char *>(&ival), sizeof(int));
    }
    else if (std::holds_alternative<double>(spec_.opt_end_))
    {
        type = static_cast<uint8_t>(ResultType::Double);
        str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));
        double dval = std::get<double>(spec_.opt_end_);
        str.append(reinterpret_cast<const char *>(&dval), sizeof(double));
    }
    else if (std::holds_alternative<EloqString>(spec_.opt_end_))
    {
        type = static_cast<uint8_t>(ResultType::String);
        str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));
        EloqString sval = std::get<EloqString>(spec_.opt_end_);
        uint32_t len = sval.Length();
        str.append(reinterpret_cast<const char *>(&len), sizeof(uint32_t));
        str.append(sval.Data(), len);
    }
    else
    {
        assert(false);
    }
}

void ZRemRangeCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();

    range_type_ =
        static_cast<ZRangeType>(*reinterpret_cast<const uint8_t *>(ptr));
    ptr += sizeof(uint8_t);

    spec_.minex_ = *reinterpret_cast<const bool *>(ptr);
    ptr += sizeof(bool);
    spec_.maxex_ = *reinterpret_cast<const bool *>(ptr);
    ptr += sizeof(bool);
    spec_.negative_ = *reinterpret_cast<const bool *>(ptr);
    ptr += sizeof(bool);
    spec_.positive_ = *reinterpret_cast<const bool *>(ptr);
    ptr += sizeof(bool);

    ResultType type =
        static_cast<ResultType>(*reinterpret_cast<const uint8_t *>(ptr));
    ptr += sizeof(uint8_t);

    switch (type)
    {
    case ResultType::Int:
    {
        spec_.opt_start_ = *reinterpret_cast<const int *>(ptr);
        ptr += sizeof(int);
        break;
    }
    case ResultType::Double:
    {
        spec_.opt_start_ = *reinterpret_cast<const double *>(ptr);
        ptr += sizeof(double);
        break;
    }
    case ResultType::String:
    {
        uint32_t len = *reinterpret_cast<const uint32_t *>(ptr);
        ptr += sizeof(uint32_t);
        spec_.opt_start_ = EloqString(ptr, len);
        ptr += len;
        break;
    }
    default:
        assert(false);
    }

    type = static_cast<ResultType>(*reinterpret_cast<const uint8_t *>(ptr));
    ptr += sizeof(uint8_t);

    switch (type)
    {
    case ResultType::Int:
    {
        spec_.opt_end_ = *reinterpret_cast<const int *>(ptr);
        ptr += sizeof(int);
        break;
    }
    case ResultType::Double:
    {
        spec_.opt_end_ = *reinterpret_cast<const double *>(ptr);
        ptr += sizeof(double);
        break;
    }
    case ResultType::String:
    {
        uint32_t len = *reinterpret_cast<const uint32_t *>(ptr);
        ptr += sizeof(uint32_t);
        spec_.opt_end_ = EloqString(ptr, len);
        ptr += len;
        break;
    }
    default:
        assert(false);
    }
}

void ZRemRangeCommand::OutputResult(OutputHandler *reply) const
{
    const auto &zset_result = zset_result_;
    if (zset_result.err_code_ == RD_OK)
    {
        reply->OnInt(std::get<int>(zset_result.result_));
    }
    else if (zset_result.err_code_ == RD_NIL)
    {
        reply->OnInt(0);
    }
    else
    {
        reply->OnError(redis_get_error_messages(zset_result.err_code_));
    }
}

std::tuple<bool, EloqKey, ZRemCommand> ParseZRemCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "zrem");
    if (args.size() < 3)
    {
        output->OnError("ERR wrong number of arguments for 'zrem' command");
        return {false, EloqKey(), ZRemCommand()};
    }
    std::vector<EloqString> elements;
    elements.reserve(args.size() - 2);
    for (size_t i = 2; i < args.size(); i++)
    {
        elements.emplace_back(args[i].data());
    }

    return {true, EloqKey(args[1]), ZRemCommand(std::move(elements))};
}

ZRemCommand::ZRemCommand(const ZRemCommand &rhs) : ZsetCommand(rhs)
{
    elements_.reserve(rhs.elements_.size());
    for (const auto &element : rhs.elements_)
    {
        elements_.emplace_back(element.Clone());
    }
}

txservice::ExecResult ZRemCommand::ExecuteOn(const txservice::TxObject &object)
{
    if (!CheckTypeMatch(object))
    {
        zset_result_.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }
    const RedisZsetObject &zset_obj =
        static_cast<const RedisZsetObject &>(object);
    return zset_obj.Execute(*this) ? txservice::ExecResult::Write
                                   : txservice::ExecResult::Fail;
}

txservice::TxObject *ZRemCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    RedisZsetObject &zset_obj = static_cast<RedisZsetObject &>(*obj_ptr);
    bool empty_after_removal = zset_obj.CommitZRem(elements_);
    return empty_after_removal ? nullptr : obj_ptr;
}

void ZRemCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::ZREM);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));

    uint32_t ele_cnt = elements_.size();
    str.append(reinterpret_cast<const char *>(&ele_cnt), sizeof(uint32_t));

    for (auto &field : elements_)
    {
        std::string_view sv = field.StringView();
        uint32_t size = sv.size();
        str.append(reinterpret_cast<const char *>(&size), sizeof(uint32_t));
        str.append(sv.data(), size);
    }
}

void ZRemCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    uint32_t ele_cnt = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);
    elements_.reserve(ele_cnt);
    for (uint32_t i = 0; i < ele_cnt; i++)
    {
        uint32_t size = *reinterpret_cast<const uint32_t *>(ptr);
        ptr += sizeof(uint32_t);
        elements_.emplace_back(EloqString(ptr, size));
        ptr += size;
    }
}

void ZRemCommand::OutputResult(OutputHandler *reply) const
{
    const auto &zset_result = zset_result_;
    if (zset_result.err_code_ == RD_OK)
    {
        reply->OnInt(std::get<int>(zset_result.result_));
    }
    else if (zset_result.err_code_ == RD_NIL)
    {
        reply->OnInt(0);
    }
    else
    {
        reply->OnError(redis_get_error_messages(zset_result.err_code_));
    }
}

std::tuple<bool, EloqKey, ZScoreCommand> ParseZScoreCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "zscore");
    if (args.size() != 3)
    {
        output->OnError("ERR wrong number of arguments for 'zscore' command");
        return {false, EloqKey(), ZScoreCommand()};
    }

    return {true, EloqKey(args[1]), ZScoreCommand(args[2])};
}

txservice::ExecResult ZScoreCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    if (!CheckTypeMatch(object))
    {
        zset_result_.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const RedisZsetObject &zset_obj =
        static_cast<const RedisZsetObject &>(object);
    zset_obj.Execute(*this);
    return txservice::ExecResult::Read;
}

void ZScoreCommand::Serialize(std::string &str) const
{
    uint8_t type = static_cast<uint8_t>(RedisCommandType::ZSCORE);
    str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));

    uint32_t value_size = element_.Length();
    str.append(reinterpret_cast<const char *>(&value_size), sizeof(uint32_t));
    str.append(element_.Data(), value_size);
}

void ZScoreCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = reinterpret_cast<const char *>(cmd_image.data());

    uint32_t value_size = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);
    element_ = EloqString(ptr, value_size);
    ptr += value_size;
}

void ZScoreCommand::OutputResult(OutputHandler *reply) const
{
    const auto &zset_result = zset_result_;
    if (zset_result.err_code_ == RD_OK)
    {
        const auto &result = std::get<EloqString>(zset_result.result_);
        reply->OnString(result.StringView());
    }
    else if (zset_result.err_code_ == RD_NIL)
    {
        reply->OnNil();
    }
    else
    {
        reply->OnError(redis_get_error_messages(zset_result_.err_code_));
    }
}

std::pair<bool, std::string> ParseZcountElement(
    const std::vector<std::string_view> &args, ZRangeSpec &spec)
{
    int minidx = 2;
    int maxidx = 3;

    // parse the range
    auto [ec_start, bool_start, start] = GetDoubleFromSv(args[minidx]);
    auto [ec_end, bool_end, end] = GetDoubleFromSv(args[maxidx]);
    if (!ec_start || !ec_end)
    {
        return {false, redis_get_error_messages(RD_ERR_MIN_MAX_NOT_FLOAT)};
    }
    spec.opt_start_ = start;
    spec.opt_end_ = end;
    spec.minex_ = bool_start;
    spec.maxex_ = bool_end;

    return {true, {}};
}

std::tuple<bool, EloqKey, ZCountCommand> ParseZCountCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "zcount");
    if (args.size() != 4)
    {
        output->OnError("ERR wrong number of arguments for 'zcount' command");
        return {false, EloqKey(), ZCountCommand()};
    }

    ZRangeSpec spec;

    auto [success, err_code] = ParseZcountElement(args, spec);
    if (!success)
    {
        output->OnError(err_code);
        return {false, EloqKey(), ZCountCommand()};
    }

    return {true, EloqKey(args[1]), ZCountCommand(std::move(spec))};
}

txservice::ExecResult ZCountCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    if (!CheckTypeMatch(object))
    {
        zset_result_.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const RedisZsetObject &zset_obj =
        static_cast<const RedisZsetObject &>(object);
    zset_obj.Execute(*this);
    return txservice::ExecResult::Read;
}

void ZCountCommand::Serialize(std::string &str) const
{
    uint8_t type = static_cast<uint8_t>(RedisCommandType::ZCOUNT);
    str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));

    str.append(reinterpret_cast<const char *>(&spec_.minex_), sizeof(bool));
    str.append(reinterpret_cast<const char *>(&spec_.maxex_), sizeof(bool));

    if (std::holds_alternative<int>(spec_.opt_start_))
    {
        type = static_cast<uint8_t>(ResultType::Int);
        str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));
        int ival = std::get<int>(spec_.opt_start_);
        str.append(reinterpret_cast<const char *>(&ival), sizeof(int));
    }
    else if (std::holds_alternative<double>(spec_.opt_start_))
    {
        type = static_cast<uint8_t>(ResultType::Double);
        str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));
        double dval = std::get<double>(spec_.opt_start_);
        str.append(reinterpret_cast<const char *>(&dval), sizeof(double));
    }
    else if (std::holds_alternative<EloqString>(spec_.opt_start_))
    {
        type = static_cast<uint8_t>(ResultType::String);
        str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));
        EloqString sval = std::get<EloqString>(spec_.opt_start_);
        uint32_t len = sval.Length();
        str.append(reinterpret_cast<const char *>(&len), sizeof(uint32_t));
        str.append(sval.Data(), len);
    }
    else
    {
        assert(false);
    }

    if (std::holds_alternative<int>(spec_.opt_end_))
    {
        type = static_cast<uint8_t>(ResultType::Int);
        str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));
        int ival = std::get<int>(spec_.opt_end_);
        str.append(reinterpret_cast<const char *>(&ival), sizeof(int));
    }
    else if (std::holds_alternative<double>(spec_.opt_end_))
    {
        type = static_cast<uint8_t>(ResultType::Double);
        str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));
        double dval = std::get<double>(spec_.opt_end_);
        str.append(reinterpret_cast<const char *>(&dval), sizeof(double));
    }
    else if (std::holds_alternative<EloqString>(spec_.opt_end_))
    {
        type = static_cast<uint8_t>(ResultType::String);
        str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));
        EloqString sval = std::get<EloqString>(spec_.opt_end_);
        uint32_t len = sval.Length();
        str.append(reinterpret_cast<const char *>(&len), sizeof(uint32_t));
        str.append(sval.Data(), len);
    }
    else
    {
        assert(false);
    }
}

void ZCountCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();

    spec_.minex_ = *reinterpret_cast<const bool *>(ptr);
    ptr += sizeof(bool);
    spec_.maxex_ = *reinterpret_cast<const bool *>(ptr);
    ptr += sizeof(bool);

    ResultType type =
        static_cast<ResultType>(*reinterpret_cast<const uint8_t *>(ptr));
    ptr += sizeof(uint8_t);

    switch (type)
    {
    case ResultType::Int:
    {
        spec_.opt_start_ = *reinterpret_cast<const int *>(ptr);
        ptr += sizeof(int);
        break;
    }
    case ResultType::Double:
    {
        spec_.opt_start_ = *reinterpret_cast<const double *>(ptr);
        ptr += sizeof(double);
        break;
    }
    case ResultType::String:
    {
        uint32_t len = *reinterpret_cast<const uint32_t *>(ptr);
        ptr += sizeof(uint32_t);
        spec_.opt_start_ = EloqString(ptr, len);
        ptr += len;
        break;
    }
    default:
        assert(false);
    }

    type = static_cast<ResultType>(*reinterpret_cast<const uint8_t *>(ptr));
    ptr += sizeof(uint8_t);

    switch (type)
    {
    case ResultType::Int:
    {
        spec_.opt_end_ = *reinterpret_cast<const int *>(ptr);
        ptr += sizeof(int);
        break;
    }
    case ResultType::Double:
    {
        spec_.opt_end_ = *reinterpret_cast<const double *>(ptr);
        ptr += sizeof(double);
        break;
    }
    case ResultType::String:
    {
        uint32_t len = *reinterpret_cast<const uint32_t *>(ptr);
        ptr += sizeof(uint32_t);
        spec_.opt_end_ = EloqString(ptr, len);
        ptr += len;
        break;
    }
    default:
        assert(false);
    }
}

void ZCountCommand::OutputResult(OutputHandler *reply) const
{
    const auto &zset_result = zset_result_;
    if (zset_result.err_code_ == RD_OK)
    {
        reply->OnInt(std::get<int>(zset_result.result_));
    }
    else if (zset_result.err_code_ == RD_NIL)
    {
        reply->OnInt(0);
    }
    else
    {
        reply->OnError(redis_get_error_messages(zset_result.err_code_));
    }
}

std::tuple<bool, EloqKey, ZCardCommand> ParseZCardCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "zcard");
    if (args.size() != 2)
    {
        output->OnError("ERR wrong number of arguments for 'zcount' command");
        return {false, EloqKey(), ZCardCommand()};
    }

    return {true, EloqKey(args[1]), ZCardCommand()};
}

txservice::ExecResult ZCardCommand::ExecuteOn(const txservice::TxObject &object)
{
    if (!CheckTypeMatch(object))
    {
        zset_result_.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const RedisZsetObject &zset_obj =
        static_cast<const RedisZsetObject &>(object);
    zset_obj.Execute(*this);
    return txservice::ExecResult::Read;
}

void ZCardCommand::Serialize(std::string &str) const
{
    uint8_t type = static_cast<uint8_t>(RedisCommandType::ZCARD);
    str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));
}

void ZCardCommand::Deserialize(std::string_view cmd_image)
{
    // nothing to do
}

void ZCardCommand::OutputResult(OutputHandler *reply) const
{
    const auto &zset_result = zset_result_;
    if (zset_result.err_code_ == RD_OK)
    {
        reply->OnInt(std::get<int>(zset_result.result_));
    }
    else if (zset_result.err_code_ == RD_NIL)
    {
        reply->OnInt(0);
    }
    else
    {
        reply->OnError(redis_get_error_messages(zset_result.err_code_));
    }
}

txservice::ExecResult ZScanCommand::ExecuteOn(const txservice::TxObject &object)
{
    RedisZsetResult &z_result = zset_result_;
    if (!CheckTypeMatch(object))
    {
        z_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const RedisZsetObject &z_obj = static_cast<const RedisZsetObject &>(object);
    z_obj.Execute(*this);
    return txservice::ExecResult::Read;
}

void ZScanCommand::Serialize(std::string &str) const
{
    uint8_t type = static_cast<uint8_t>(RedisCommandType::ZSCAN);
    str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));

    str.append(reinterpret_cast<const char *>(&match_), sizeof(match_));
    uint32_t key_size = pattern_.Length();
    str.append(reinterpret_cast<const char *>(&key_size), sizeof(uint32_t));
    str.append(pattern_.Data(), key_size);

    str.append(reinterpret_cast<const char *>(&count_), sizeof(uint64_t));

    str.append(reinterpret_cast<const char *>(&with_cursor_), sizeof(bool));
    uint32_t cursor_field_size = cursor_field_.Length();
    str.append(reinterpret_cast<const char *>(&cursor_field_size),
               sizeof(uint32_t));
    str.append(cursor_field_.Data(), cursor_field_size);
    str.append(reinterpret_cast<const char *>(&cursor_score_), sizeof(double));
}

void ZScanCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    match_ = *reinterpret_cast<const bool *>(ptr);
    ptr += sizeof(bool);

    uint32_t key_size = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);

    pattern_ = EloqString(ptr, key_size);
    ptr += key_size;

    count_ = *reinterpret_cast<const uint64_t *>(ptr);
    ptr += sizeof(uint64_t);

    with_cursor_ = *reinterpret_cast<const bool *>(ptr);
    ptr += sizeof(bool);

    uint32_t cursor_field_size = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);

    cursor_field_ = EloqString(ptr, cursor_field_size);
    ptr += cursor_field_size;
    cursor_score_ = *reinterpret_cast<const double *>(ptr);
    ptr += sizeof(double);
}

void ZScanCommand::OutputResult(OutputHandler *reply) const
{
    assert(false);
}

void ZScanCommand::OutputResult(OutputHandler *reply,
                                RedisConnectionContext *ctx) const
{
    assert(reply != nullptr);

    const auto &z_result = zset_result_;
    if (z_result.err_code_ == RD_OK)
    {
        auto &ans_list = std::get<std::vector<EloqString>>(z_result.result_);
        reply->OnArrayStart(2);

        if (ans_list.front().StringView() != "0")
        {
            // cache cursor content to context and output the returned
            // cursor_id.
            uint64_t cursor_id =
                ctx->CacheScanCursor(ans_list.front().StringView());
            reply->OnString(std::to_string(cursor_id));
        }
        else
        {
            reply->OnString("0");
        }

        reply->OnArrayStart(ans_list.size() - 1);
        for (auto hscan_it = ans_list.begin() + 1; hscan_it != ans_list.end();
             hscan_it++)
        {
            reply->OnString(hscan_it->StringView());
        }
        reply->OnArrayEnd();

        reply->OnArrayEnd();
    }
    else if (z_result.err_code_ == RD_NIL)
    {
        reply->OnArrayStart(2);

        reply->OnString("0");

        reply->OnArrayStart(0);
        reply->OnArrayEnd();

        reply->OnArrayEnd();
    }
    else
    {
        reply->OnError(redis_get_error_messages(z_result.err_code_));
    }
}

bool ZScanWrapper::Execute(RedisServiceImpl *redis_impl,
                           RedisConnectionContext *ctx,
                           const txservice::TableName *table,
                           txservice::TransactionExecution *txm,
                           OutputHandler *output,
                           bool auto_commit)
{
    return redis_impl->ExecuteCommand(
        ctx, txm, table, key_, &cmd_, output, auto_commit);
}

bool ZUnionStoreCommand::HandleMiddleResult()
{
    bool all_empty = true;
    for (const auto &cmd : cmds_)
    {
        if (cmd.set_scan_result_.err_code_ == RD_ERR_WRONG_TYPE)
        {
            return false;
        }
        else if (cmd.set_scan_result_.err_code_ != RD_NIL)
        {
            auto &res = std::get<std::vector<std::pair<EloqString, double>>>(
                cmd.set_scan_result_.result_);
            if (!res.empty())
            {
                all_empty = false;
            }
        }
    }

    if (all_empty)
    {
        return false;
    }

    std::unordered_map<std::string_view, std::pair<const EloqString *, double>>
        union_vals;
    auto update_value_func = AggregateUtil::GetUpdateFunction(aggregate_type_);
    for (size_t cmd_idx = 0; cmd_idx < cmds_.size(); ++cmd_idx)
    {
        if (cmds_[cmd_idx].set_scan_result_.err_code_ == RD_NIL)
        {
            continue;
        }

        const auto &vals = std::get<std::vector<std::pair<EloqString, double>>>(
            cmds_[cmd_idx].set_scan_result_.result_);

        for (size_t idx = 0; idx < vals.size(); idx += 1)
        {
            double new_score = AggregateUtil::CaculateNewScore(
                vals[idx].second, weights_[cmd_idx]);

            auto iter = union_vals.find(vals[idx].first.StringView());
            if (iter == union_vals.end())
            {
                union_vals.emplace(vals[idx].first.StringView(),
                                   std::pair(&vals[idx].first, new_score));
            }
            else
            {
                update_value_func(iter->second.second, new_score);
            }
        }
    }

    std::vector<std::pair<double, EloqString>> elements;
    elements.reserve(union_vals.size());
    for (auto &val : union_vals)
    {
        elements.push_back(
            std::pair(val.second.second, std::move(*val.second.first)));
    }

    assert(cmd_store_->is_in_the_middle_stage_ == true);

    cmd_store_->elements_ = std::move(elements);
    cmd_store_->type_ = ZAddCommand::ElementType::vector;
    cmd_store_->params_.flags = ZADD_FORCE_CLEAR;
    return true;
}

void ZUnionStoreCommand::OutputResult(OutputHandler *reply) const
{
    if (curr_step_ != 0)
    {
        if (cmd_store_->zset_result_.err_code_ == RD_OK)
        {
            auto &elements =
                std::get<std::vector<std::pair<double, EloqString>>>(
                    cmd_store_->elements_);
            reply->OnInt(elements.size());
        }
        else
        {
            reply->OnError(
                redis_get_error_messages(cmd_store_->zset_result_.err_code_));
        }
    }
    else
    {
        for (const auto &cmd : cmds_)
        {
            if (cmd.set_scan_result_.err_code_ == RD_ERR_WRONG_TYPE)
            {
                reply->OnError(redis_get_error_messages(RD_ERR_WRONG_TYPE));
                return;
            }
        }

        reply->OnInt(0);
    }
}

void ZUnionCommand::OutputResult(OutputHandler *reply) const
{
    for (const auto &cmd : cmds_)
    {
        if (cmd.set_scan_result_.err_code_ == RD_ERR_WRONG_TYPE)
        {
            reply->OnError(
                redis_get_error_messages(cmd.set_scan_result_.err_code_));
            return;
        }
    }

    std::unordered_map<std::string_view, double> union_vals;
    auto update_value_func = AggregateUtil::GetUpdateFunction(aggregate_type_);

    for (size_t cmd_idx = 0; cmd_idx < cmds_.size(); ++cmd_idx)
    {
        if (cmds_[cmd_idx].set_scan_result_.err_code_ == RD_NIL)
        {
            continue;
        }

        const auto &vals = std::get<std::vector<std::pair<EloqString, double>>>(
            cmds_[cmd_idx].set_scan_result_.result_);

        for (size_t idx = 0; idx < vals.size(); idx += 1)
        {
            double new_score = AggregateUtil::CaculateNewScore(
                vals[idx].second, weights_[cmd_idx]);
            auto iter = union_vals.find(vals[idx].first.StringView());
            if (iter == union_vals.end())
            {
                union_vals.emplace(vals[idx].first.StringView(), new_score);
            }
            else
            {
                update_value_func(iter->second, new_score);
            }
        }
    }

    std::vector<std::pair<std::string_view, double>> union_vals_list;
    union_vals_list.reserve(union_vals.size());
    for (const auto &val : union_vals)
    {
        union_vals_list.push_back({val.first, val.second});
    }
    std::sort(union_vals_list.begin(),
              union_vals_list.end(),
              [](auto &v1, auto &v2)
              {
                  return v1.second != v2.second ? v1.second < v2.second
                                                : v1.first < v2.first;
              });

    reply->OnArrayStart(opt_withscores_ ? union_vals.size() * 2
                                        : union_vals.size());
    for (const auto &val : union_vals_list)
    {
        reply->OnString(val.first);

        if (opt_withscores_)
        {
            reply->OnString(d2string(val.second));
        }
    }
    reply->OnArrayEnd();
}

txservice::ExecResult ZRandMemberCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    if (!CheckTypeMatch(object))
    {
        zset_result_.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const RedisZsetObject &zset_obj =
        static_cast<const RedisZsetObject &>(object);
    zset_obj.Execute(*this);
    return txservice::ExecResult::Read;
}

void ZRandMemberCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::ZRANDMEMBER);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
    str.append(reinterpret_cast<const char *>(&count_), sizeof(int64_t));
    str.append(reinterpret_cast<const char *>(&with_scores_), sizeof(bool));
}

void ZRandMemberCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    count_ = *reinterpret_cast<const int64_t *>(ptr);
    ptr += sizeof(int64_t);
    with_scores_ = *reinterpret_cast<const bool *>(ptr);
}

void ZRandMemberCommand::OutputResult(OutputHandler *reply) const
{
    if (zset_result_.err_code_ == RD_OK)
    {
        auto &keys = std::get<std::vector<EloqString>>(zset_result_.result_);
        if (count_provided_)
        {
            reply->OnArrayStart(keys.size());

            for (const auto &key : keys)
            {
                reply->OnString(key.StringView());
            }

            reply->OnArrayEnd();
        }
        else
        {
            assert(keys.size() == 1);
            reply->OnString(keys[0].StringView());
        }
    }
    else if (zset_result_.err_code_ == RD_NIL)
    {
        if (count_provided_)
        {
            reply->OnArrayStart(0);
            reply->OnArrayEnd();
        }
        else
        {
            reply->OnNil();
        }
    }
    else
    {
        reply->OnError(redis_get_error_messages(zset_result_.err_code_));
    }
}

txservice::ExecResult ZRankCommand::ExecuteOn(const txservice::TxObject &object)
{
    if (!CheckTypeMatch(object))
    {
        zset_result_.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const RedisZsetObject &zset_obj =
        static_cast<const RedisZsetObject &>(object);
    zset_obj.Execute(*this);
    return txservice::ExecResult::Read;
}

void ZRankCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::ZRANK);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));

    uint32_t value_size = member_.Length();
    str.append(reinterpret_cast<const char *>(&value_size), sizeof(uint32_t));
    str.append(member_.Data(), value_size);

    str.append(reinterpret_cast<const char *>(&with_score_), sizeof(bool));
    str.append(reinterpret_cast<const char *>(&is_rev_), sizeof(bool));
}

void ZRankCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();

    uint32_t value_size = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);
    member_ = EloqString(ptr, value_size);
    ptr += value_size;

    with_score_ = *reinterpret_cast<const bool *>(ptr);
    ptr += sizeof(bool);
    is_rev_ = *reinterpret_cast<const bool *>(ptr);
}

void ZRankCommand::OutputResult(OutputHandler *reply) const
{
    if (zset_result_.err_code_ == RD_OK)
    {
        if (std::holds_alternative<int>(zset_result_.result_))
        {
            auto &ans = std::get<int>(zset_result_.result_);
            reply->OnInt(ans);
        }
        else
        {
            auto &pr =
                std::get<std::pair<int32_t, std::string>>(zset_result_.result_);
            reply->OnArrayStart(2);
            reply->OnInt(pr.first);
            reply->OnString(pr.second);
            reply->OnArrayEnd();
        }
    }
    else if (zset_result_.err_code_ == RD_NIL)
    {
        reply->OnNil();
    }
    else
    {
        reply->OnError(redis_get_error_messages(zset_result_.err_code_));
    }
}

void ZInterCommand::OutputResult(OutputHandler *reply) const
{
    size_t smallest_set_idx = 0;
    size_t smallest_set_size = SIZE_MAX;
    for (size_t idx = 0; idx < cmds_.size(); ++idx)
    {
        auto &cmd = cmds_[idx];
        if (cmd.set_scan_result_.err_code_ == RD_OK)
        {
            size_t set_size = 0;
            const auto &vals =
                std::get<std::vector<std::pair<EloqString, double>>>(
                    cmd.set_scan_result_.result_);
            set_size = vals.size();

            if (set_size < smallest_set_size)
            {
                smallest_set_idx = idx;
                smallest_set_size = set_size;
            }
        }
        else if (cmd.set_scan_result_.err_code_ == RD_NIL)
        {
            smallest_set_size = 0;
            smallest_set_idx = idx;
        }
        else
        {
            reply->OnError(
                redis_get_error_messages(cmd.set_scan_result_.err_code_));
            return;
        }
    }

    if (smallest_set_size == 0)
    {
        reply->OnArrayStart(0);
        reply->OnArrayEnd();
        return;
    }

    assert(!cmds_.empty());

    std::unordered_map<std::string_view, std::pair<double, size_t>> result_map;
    auto update_value_func = AggregateUtil::GetUpdateFunction(aggregate_type_);

    assert(cmds_[smallest_set_idx].set_scan_result_.err_code_ == RD_OK);

    const auto &vals = std::get<std::vector<std::pair<EloqString, double>>>(
        cmds_[smallest_set_idx].set_scan_result_.result_);
    for (size_t idx = 0; idx < vals.size(); idx += 1)
    {
        result_map.emplace(
            vals[idx].first.StringView(),
            std::pair<double, size_t>(
                AggregateUtil::CaculateNewScore(vals[idx].second,
                                                weights_[smallest_set_idx]),
                1));
    }

    size_t processed_cmd_cnt = 1;
    std::vector<std::pair<std::string, double>> results;
    for (size_t cmd_idx = 0; cmd_idx < cmds_.size(); ++cmd_idx)
    {
        assert(cmds_[cmd_idx].set_scan_result_.err_code_ == RD_OK);

        if (cmd_idx == smallest_set_idx)
        {
            continue;
        }

        auto &vals = std::get<std::vector<std::pair<EloqString, double>>>(
            cmds_[cmd_idx].set_scan_result_.result_);
        size_t key_inter_size = 0;
        for (size_t val_idx = 0; val_idx < vals.size(); val_idx += 1)
        {
            auto iter = result_map.find(vals[val_idx].first.StringView());
            if (iter == result_map.end() ||
                iter->second.second != processed_cmd_cnt)
            {
                continue;
            }

            iter->second.second += 1;
            key_inter_size += 1;

            update_value_func(iter->second.first,
                              AggregateUtil::CaculateNewScore(
                                  vals[val_idx].second, weights_[cmd_idx]));

            if (iter->second.second == cmds_.size())
            {
                results.emplace_back(iter->first, iter->second.first);
            }
        }

        processed_cmd_cnt += 1;
    }

    // sort
    std::sort(results.begin(),
              results.end(),
              [](auto &v1, auto &v2)
              {
                  return v1.second != v2.second ? v1.second < v2.second
                                                : v1.first < v2.first;
              });

    if (opt_withscores_)
    {
        reply->OnArrayStart(results.size() * 2);
        for (const auto &result : results)
        {
            reply->OnString(result.first);
            reply->OnString(d2string(result.second));
        }
        reply->OnArrayEnd();
    }
    else
    {
        reply->OnArrayStart(results.size());
        for (const auto &result : results)
        {
            reply->OnString(result.first);
        }
        reply->OnArrayEnd();
    }
}

bool ZInterStoreCommand::HandleMiddleResult()
{
    size_t smallest_set_idx = 0;
    size_t smallest_set_size = SIZE_MAX;
    for (size_t idx = 0; idx < cmds_.size(); ++idx)
    {
        auto &cmd = cmds_[idx];
        if (cmd.set_scan_result_.err_code_ == RD_OK)
        {
            const auto &vals =
                std::get<std::vector<std::pair<EloqString, double>>>(
                    cmd.set_scan_result_.result_);

            if (vals.size() < smallest_set_size)
            {
                smallest_set_idx = idx;
                smallest_set_size = vals.size();
            }
        }
        else if (cmd.set_scan_result_.err_code_ == RD_NIL)
        {
            smallest_set_size = 0;
            smallest_set_idx = idx;
            return false;
        }
        else
        {
            return false;
        }
    }

    if (smallest_set_size == 0)
    {
        // We don't need to execute `ZAddCommand`
        return false;
    }

    assert(cmds_[smallest_set_idx].set_scan_result_.err_code_ == RD_OK);

    // size_t is used as a flag to denote how many zsets have this entry
    std::unordered_map<std::string_view,
                       std::tuple<double, size_t, EloqString *>>
        result_map;
    std::vector<std::pair<double, EloqString>> results;
    auto update_value_func = AggregateUtil::GetUpdateFunction(aggregate_type_);

    auto &vals = std::get<std::vector<std::pair<EloqString, double>>>(
        cmds_[smallest_set_idx].set_scan_result_.result_);

    if (cmds_.size() == 1)
    {
        auto weight = weights_[0];
        for (auto &[key, score] : vals)
        {
            results.emplace_back(AggregateUtil::CaculateNewScore(score, weight),
                                 key);
        }
        assert(!results.empty());
        assert(cmd_store_->is_in_the_middle_stage_ == true);
        cmd_store_->elements_ = std::move(results);
        cmd_store_->type_ = ZAddCommand::ElementType::vector;
        cmd_store_->params_.flags = ZADD_FORCE_CLEAR;
        return true;
    }
    for (size_t idx = 0; idx < vals.size(); idx += 1)
    {
        result_map.emplace(
            vals[idx].first.StringView(),
            std::tuple<double, size_t, EloqString *>(
                AggregateUtil::CaculateNewScore(vals[idx].second,
                                                weights_[smallest_set_idx]),
                1,
                &vals[idx].first));
    }

    size_t processed_cmd_cnt = 1;
    for (size_t cmd_idx = 0; cmd_idx < cmds_.size(); ++cmd_idx)
    {
        assert(cmds_[cmd_idx].set_scan_result_.err_code_ == RD_OK);

        if (cmd_idx == smallest_set_idx)
        {
            continue;
        }

        const auto &cmd_vals =
            std::get<std::vector<std::pair<EloqString, double>>>(
                cmds_[cmd_idx].set_scan_result_.result_);
        for (size_t val_idx = 0; val_idx < cmd_vals.size(); val_idx += 1)
        {
            auto iter = result_map.find(cmd_vals[val_idx].first.StringView());
            if (iter == result_map.end() ||
                std::get<1>(iter->second) != processed_cmd_cnt)
            {
                continue;
            }

            std::get<1>(iter->second) += 1;

            update_value_func(std::get<0>(iter->second),
                              AggregateUtil::CaculateNewScore(
                                  cmd_vals[val_idx].second, weights_[cmd_idx]));

            if (std::get<1>(iter->second) == cmds_.size())
            {
                // all zsets contain this entry
                results.emplace_back(std::get<0>(iter->second),
                                     std::move(*std::get<2>(iter->second)));
            }
        }

        processed_cmd_cnt += 1;
    }

    assert(cmd_store_->is_in_the_middle_stage_ == true);

    cmd_store_->elements_ = std::move(results);
    cmd_store_->type_ = ZAddCommand::ElementType::vector;
    cmd_store_->params_.flags = ZADD_FORCE_CLEAR;
    return true;
}

void ZInterStoreCommand::OutputResult(OutputHandler *reply) const
{
    if (curr_step_ == 1)
    {
        if (cmd_store_->zset_result_.err_code_ == RD_OK)
        {
            auto &elements =
                std::get<std::vector<std::pair<double, EloqString>>>(
                    cmd_store_->elements_);
            reply->OnInt(elements.size());
        }
        else
        {
            reply->OnError(
                redis_get_error_messages(cmd_store_->zset_result_.err_code_));
        }
    }
    else
    {
        for (const auto &cmd : cmds_)
        {
            if (cmd.set_scan_result_.err_code_ == RD_ERR_WRONG_TYPE)
            {
                reply->OnError(redis_get_error_messages(RD_ERR_WRONG_TYPE));
                return;
            }
        }

        reply->OnInt(0);
    }
}

std::vector<std::string_view> FindZsetResultInter(
    const std::vector<SZScanCommand> &cmds, size_t limit = UINT64_MAX)
{
    // Find the smallest vector (potentially more efficient for lookups)
    size_t smallest_index = 0;
    for (size_t i = 1; i < cmds.size(); ++i)
    {
        const auto &iter =
            std::get<std::vector<EloqString>>(cmds[i].set_scan_result_.result_);
        const auto &smallest = std::get<std::vector<EloqString>>(
            cmds[smallest_index].set_scan_result_.result_);
        if (iter.size() < smallest.size())
        {
            smallest_index = i;
        }
    }

    // Use the smallest vector as the base set and convert to unordered_set
    const auto &target_vec = std::get<std::vector<EloqString>>(
        cmds[smallest_index].set_scan_result_.result_);
    std::vector<std::string_view> base_set;
    for (const auto &str : target_vec)
    {
        // Implicit conversion from std::string to std::string_view
        base_set.push_back(str.StringView());
    }

    // Iterate through remaining vectors (excluding the smallest)
    for (size_t i = 0; i < cmds.size(); ++i)
    {
        if (i == smallest_index)
        {
            continue;  // Skip the smallest vector (already used as base)
        }

        const auto &other_vec =
            std::get<std::vector<EloqString>>(cmds[i].set_scan_result_.result_);

        // Build a hash table for quick lookup
        std::unordered_set<std::string_view> other_set;
        for (const auto &str : other_vec)
        {
            // Implicit conversion from std::string to std::string_view
            other_set.insert(str.StringView());
        }

        // Iterate through base_set and remove elements not found in
        // other_set
        for (auto it = base_set.begin(); it != base_set.end();)
        {
            if (other_set.count(*it) == 0)
            {
                // Not present in all vectors, remove
                it = base_set.erase(it);
                // Early termination if intersection becomes empty or
                // reaches limit
                if (base_set.empty())
                {
                    break;
                }
            }
            else
            {
                ++it;
            }
        }
    }

    if (base_set.size() > limit)
    {
        base_set.erase(base_set.begin() + limit, base_set.end());
    }

    return base_set;
}

void ZInterCardCommand::OutputResult(OutputHandler *reply) const
{
    bool has_empty_set = false;
    for (size_t idx = 0; idx < cmds_.size(); ++idx)
    {
        auto &cmd = cmds_[idx];
        if (cmd.set_scan_result_.err_code_ == RD_OK)
        {
            auto &vals =
                std::get<std::vector<EloqString>>(cmd.set_scan_result_.result_);
            if (vals.empty())
            {
                has_empty_set = true;
                break;
            }
        }
        else if (cmd.set_scan_result_.err_code_ == RD_NIL)
        {
            has_empty_set = true;
            break;
        }
        else
        {
            reply->OnError(
                redis_get_error_messages(cmd.set_scan_result_.err_code_));
            return;
        }
    }

    if (has_empty_set)
    {
        reply->OnInt(0);
        return;
    }

    std::vector<std::string_view> vct_key = FindZsetResultInter(cmds_, limit_);
    reply->OnInt(vct_key.size());
}

txservice::ExecResult ZMScoreCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    if (!CheckTypeMatch(object))
    {
        zset_result_.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const RedisZsetObject &zset_obj =
        static_cast<const RedisZsetObject &>(object);
    zset_obj.Execute(*this);
    return txservice::ExecResult::Read;
}

void ZMScoreCommand::Serialize(std::string &str) const
{
    uint8_t type = static_cast<uint8_t>(RedisCommandType::ZMSCORE);
    str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));

    uint32_t elm_size = static_cast<uint32_t>(vct_elm_.size());
    str.append(reinterpret_cast<const char *>(&elm_size), sizeof(uint32_t));

    for (const EloqString &elm : vct_elm_)
    {
        uint32_t value_size = elm.Length();
        str.append(reinterpret_cast<const char *>(&value_size),
                   sizeof(uint32_t));
        str.append(elm.Data(), value_size);
    }
}

void ZMScoreCommand::Deserialize(std::string_view cmd_image)
{
    assert(vct_elm_.size() == 0);
    const char *ptr = reinterpret_cast<const char *>(cmd_image.data());
    uint32_t elm_size = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);

    for (uint32_t i = 0; i < elm_size; i++)
    {
        uint32_t value_size = *reinterpret_cast<const uint32_t *>(ptr);
        ptr += sizeof(uint32_t);
        vct_elm_.emplace_back(ptr, value_size);
        ptr += value_size;
    }
}

void ZMScoreCommand::OutputResult(OutputHandler *reply) const
{
    const auto &zset_result = zset_result_;
    if (zset_result.err_code_ == RD_OK)
    {
        auto vct_res = std::get<std::vector<EloqString>>(zset_result.result_);
        reply->OnArrayStart(vct_res.size());
        for (const auto &elm : vct_res)
        {
            if (elm.Length() == 0)
            {
                reply->OnNil();
            }
            else
            {
                reply->OnString(elm.StringView());
            }
        }
        reply->OnArrayEnd();
    }
    else if (zset_result.err_code_ == RD_NIL)
    {
        size_t sz = vct_elm_.size();
        reply->OnArrayStart(sz);
        for (size_t i = 0; i < sz; i++)
        {
            reply->OnNil();
        }
        reply->OnArrayEnd();
    }
    else
    {
        reply->OnError(redis_get_error_messages(zset_result.err_code_));
    }
}

std::vector<const EloqString *> FindZDiff(
    const std::vector<ZRangeCommand> &cmds)
{
    std::vector<const EloqString *> unique_elements;
    if (!std::holds_alternative<std::vector<EloqString>>(
            cmds[0].zset_result_.result_))
    {
        return unique_elements;
    }

    // This is the target vector to compare with
    const auto &target_vec =
        std::get<std::vector<EloqString>>(cmds[0].zset_result_.result_);

    // Create a map of all unique strings from other_vectors
    std::unordered_set<std::string_view> unique_strings_to_compare;
    for (size_t i = 1; i < cmds.size(); i++)
    {
        if (!std::holds_alternative<std::vector<EloqString>>(
                cmds[i].zset_result_.result_))
        {
            continue;
        }

        auto &other_vec =
            std::get<std::vector<EloqString>>(cmds[i].zset_result_.result_);
        for (auto member_it = other_vec.begin(); member_it != other_vec.end();
             member_it++)
        {
            unique_strings_to_compare.insert(member_it->StringView());
        }
    }

    // Find elements only in target_vec (not present in any other vector)
    if (cmds[0].constraint_.opt_withscores_)
    {
        for (auto it = target_vec.begin(); it != target_vec.end(); it++)
        {
            if (unique_strings_to_compare.count(it->StringView()) == 0)
            {
                unique_elements.push_back(&(*it));
                it++;
                unique_elements.push_back(&(*it));
            }
            else
            {
                it++;
            }
        }
    }
    else
    {
        for (auto it = target_vec.begin(); it != target_vec.end(); it++)
        {
            if (unique_strings_to_compare.count(it->StringView()) == 0)
            {
                unique_elements.push_back(&(*it));
            }
        }
    }
    return unique_elements;
}

void ZDiffCommand::OutputResult(OutputHandler *reply) const
{
    for (size_t i = 0; i < cmds_.size(); i++)
    {
        if (cmds_[i].zset_result_.err_code_ != RD_OK &&
            cmds_[i].zset_result_.err_code_ != RD_NIL)
        {
            reply->OnError(
                redis_get_error_messages(cmds_[i].zset_result_.err_code_));
            return;
        }
    }

    std::vector<const EloqString *> vct = FindZDiff(cmds_);

    reply->OnArrayStart(vct.size());
    for (const EloqString *str : vct)
    {
        reply->OnString(str->StringView());
    }
    reply->OnArrayEnd();
}

bool ZDiffStoreCommand::HandleMiddleResult()
{
    for (size_t i = 0; i < cmds_.size(); i++)
    {
        if (cmds_[i].zset_result_.err_code_ != RD_OK &&
            cmds_[i].zset_result_.err_code_ != RD_NIL)
        {
            return false;
        }
    }

    std::vector<const EloqString *> vct = FindZDiff(cmds_);

    std::vector<std::pair<double, EloqString>> vp;
    vp.reserve(vct.size());

    for (size_t i = 0; i < vct.size(); i += 2)
    {
        double dbl;
        bool b = string2double(vct[i + 1]->StringView().data(),
                               vct[i + 1]->StringView().size(),
                               dbl);
        assert(b);
        (void) b;
        vp.push_back(std::pair(dbl, std::move(*vct[i])));
    }

    assert(cmd_store_->is_in_the_middle_stage_ == true);

    if (vp.size() > 0)
    {
        cmd_store_->elements_ = std::move(vp);
        cmd_store_->type_ = ZAddCommand::ElementType::vector;
    }
    else
    {
        cmd_store_->type_ = ZAddCommand::ElementType::monostate;
    }
    return true;
}

void ZDiffStoreCommand::OutputResult(OutputHandler *reply) const
{
    if (curr_step_ != 0)
    {
        reply->OnInt(std::get<int>(cmd_store_->zset_result_.result_));
    }
    else
    {
        reply->OnError(redis_get_error_messages(RD_ERR_WRONG_TYPE));
    }
}

HDelCommand::HDelCommand(const HDelCommand &rhs) : HashCommand(rhs)
{
    del_list_.reserve(rhs.del_list_.size());

    for (auto &del_key : rhs.del_list_)
    {
        del_list_.emplace_back(del_key.Clone());
    }
}

txservice::ExecResult HDelCommand::ExecuteOn(const txservice::TxObject &object)
{
    RedisHashResult &hash_result = result_;
    if (!CheckTypeMatch(object))
    {
        hash_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const RedisHashObject &hash_obj =
        static_cast<const RedisHashObject &>(object);
    return hash_obj.Execute(*this) ? txservice::ExecResult::Write
                                   : txservice::ExecResult::Fail;
}

txservice::TxObject *HDelCommand::CommitOn(txservice::TxObject *const obj_ptr)
{
    auto &hash_obj = static_cast<RedisHashObject &>(*obj_ptr);
    bool empty_after_removal = hash_obj.CommitHdel(del_list_);
    return empty_after_removal ? nullptr : obj_ptr;
}

void HDelCommand::Serialize(std::string &str) const
{
    uint8_t type = static_cast<uint8_t>(RedisCommandType::HDEL);
    str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));

    uint32_t cnt = del_list_.size();
    str.append(reinterpret_cast<const char *>(&cnt), sizeof(uint32_t));
    for (auto &del_element : del_list_)
    {
        std::string_view sv = del_element.StringView();
        uint32_t size = sv.size();
        str.append(reinterpret_cast<const char *>(&size), sizeof(uint32_t));
        str.append(sv.data(), size);
    }
}

void HDelCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    const uint32_t cnt = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);

    del_list_.clear();
    del_list_.reserve(cnt);
    for (uint32_t i = 0; i < cnt; i++)
    {
        const uint32_t field_size = *reinterpret_cast<const uint32_t *>(ptr);
        ptr += sizeof(uint32_t);
        del_list_.emplace_back(EloqString(ptr, field_size));
        ptr += field_size;
    }
}

void HDelCommand::OutputResult(OutputHandler *reply) const
{
    const auto &hash_result = result_;
    if (result_.err_code_ == RD_NIL)
    {
        reply->OnInt(0);
    }
    else if (result_.err_code_ == RD_OK)
    {
        reply->OnInt(std::get<int64_t>(hash_result.result_));
    }
    else
    {
        reply->OnError(redis_get_error_messages(hash_result.err_code_));
    }
}

txservice::ExecResult HExistsCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    RedisHashResult &hash_result = result_;
    if (!CheckTypeMatch(object))
    {
        hash_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const RedisHashObject &hash_obj =
        static_cast<const RedisHashObject &>(object);
    hash_obj.Execute(*this);
    return txservice::ExecResult::Read;
}

void HExistsCommand::Serialize(std::string &str) const
{
    uint8_t type = static_cast<uint8_t>(RedisCommandType::HEXISTS);
    str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));

    uint32_t size = key_.Length();
    str.append(reinterpret_cast<const char *>(&size), sizeof(uint32_t));
    str.append(key_.Data(), size);
}

void HExistsCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();

    const uint32_t size = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);
    key_ = EloqString(ptr, size);
}

void HExistsCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.err_code_ == RD_NIL)
    {
        reply->OnInt(0);
    }
    else if (result_.err_code_ == RD_OK)
    {
        reply->OnInt(1);
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

txservice::ExecResult HGetAllCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    RedisHashResult &hash_result = result_;
    if (!CheckTypeMatch(object))
    {
        hash_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const RedisHashObject &hash_obj =
        static_cast<const RedisHashObject &>(object);
    hash_obj.Execute(*this);
    return txservice::ExecResult::Read;
}

void HGetAllCommand::OutputResult(OutputHandler *reply) const
{
    const auto &hash_result = result_;
    if (result_.err_code_ == RD_NIL)
    {
        reply->OnArrayStart(0);
        reply->OnArrayEnd();
    }
    else if (result_.err_code_ == RD_OK)
    {
        auto &list = std::get<std::vector<std::string>>(hash_result.result_);
        reply->OnArrayStart(list.size());
        for (auto &ans_string : list)
        {
            reply->OnString(ans_string);
        }
        reply->OnArrayEnd();
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

HIncrByCommand::HIncrByCommand(const HIncrByCommand &rhs) : HashCommand(rhs)
{
    field_ = rhs.field_.Clone();
    score_ = rhs.score_;
}

txservice::ExecResult HIncrByCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    RedisHashResult &hash_result = result_;
    if (!CheckTypeMatch(object))
    {
        hash_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const RedisHashObject &hash_obj =
        static_cast<const RedisHashObject &>(object);
    bool cmd_success = hash_obj.Execute(*this);
    return cmd_success ? txservice::ExecResult::Write
                       : txservice::ExecResult::Fail;
}

txservice::TxObject *HIncrByCommand::CommitOn(
    txservice::TxObject *const obj_ptr)
{
    auto &hash_obj = static_cast<RedisHashObject &>(*obj_ptr);
    hash_obj.CommitHincrby(field_, score_);
    return obj_ptr;
}

void HIncrByCommand::Serialize(std::string &str) const
{
    uint8_t type = static_cast<uint8_t>(RedisCommandType::HINCRBY);
    str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));

    uint32_t field_size = field_.Length();

    str.append(reinterpret_cast<const char *>(&field_size), sizeof(uint32_t));
    str.append(reinterpret_cast<const char *>(field_.Data()), field_size);
    str.append(reinterpret_cast<const char *>(&score_), sizeof(int64_t));
}

void HIncrByCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();

    const uint32_t field_size = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);
    field_ = EloqString(ptr, field_size);
    ptr += field_size;

    score_ = *reinterpret_cast<const int64_t *>(ptr);
    ptr += sizeof(int64_t);
}

void HIncrByCommand::OutputResult(OutputHandler *reply) const
{
    const auto &hash_result = result_;
    if (hash_result.err_code_ == RD_OK)
    {
        reply->OnInt(std::get<int64_t>(result_.result_));
    }
    else
    {
        reply->OnError(redis_get_error_messages(hash_result.err_code_));
    }
}

HIncrByFloatCommand::HIncrByFloatCommand(const HIncrByFloatCommand &rhs)
    : HashCommand(rhs)
{
    field_ = rhs.field_.Clone();
    incr_ = rhs.incr_;
}

txservice::ExecResult HIncrByFloatCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    RedisHashResult &hash_result = result_;
    if (!CheckTypeMatch(object))
    {
        hash_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const RedisHashObject &hash_obj =
        static_cast<const RedisHashObject &>(object);
    bool cmd_success = hash_obj.Execute(*this);
    return cmd_success ? txservice::ExecResult::Write
                       : txservice::ExecResult::Fail;
}

txservice::TxObject *HIncrByFloatCommand::CommitOn(
    txservice::TxObject *const obj_ptr)
{
    auto &hash_obj = static_cast<RedisHashObject &>(*obj_ptr);
    hash_obj.CommitHIncrByFloat(field_, incr_);
    return obj_ptr;
}

void HIncrByFloatCommand::Serialize(std::string &str) const
{
    uint8_t type = static_cast<uint8_t>(RedisCommandType::HINCRBYFLOAT);
    str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));

    uint32_t field_size = field_.Length();

    str.append(reinterpret_cast<const char *>(&field_size), sizeof(uint32_t));
    str.append(reinterpret_cast<const char *>(field_.Data()), field_size);
    str.append(reinterpret_cast<const char *>(&incr_), sizeof(long double));
}

void HIncrByFloatCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();

    const uint32_t field_size = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);
    field_ = EloqString(ptr, field_size);
    ptr += field_size;

    incr_ = *reinterpret_cast<const long double *>(ptr);
    ptr += sizeof(long double);
}

void HIncrByFloatCommand::OutputResult(OutputHandler *reply) const
{
    const auto &hash_result = result_;
    if (hash_result.err_code_ == RD_OK)
    {
        reply->OnString(std::get<std::string>(result_.result_));
    }
    else
    {
        reply->OnError(redis_get_error_messages(hash_result.err_code_));
    }
}

txservice::ExecResult HMGetCommand::ExecuteOn(const txservice::TxObject &object)
{
    RedisHashResult &hash_result = result_;
    if (!CheckTypeMatch(object))
    {
        hash_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const RedisHashObject &hash_obj =
        static_cast<const RedisHashObject &>(object);
    hash_obj.Execute(*this);
    return txservice::ExecResult::Read;
}

void HMGetCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::HMGET);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
    uint32_t field_cnt = fields_.size();
    str.append(reinterpret_cast<const char *>(&field_cnt), sizeof(uint32_t));
    for (const EloqString &field : fields_)
    {
        uint32_t field_size = field.Length();
        str.append(reinterpret_cast<const char *>(&field_size),
                   sizeof(uint32_t));
        str.append(field.Data(), field_size);
    }
}

void HMGetCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();

    uint32_t field_cnt = *reinterpret_cast<const uint32_t *>(ptr);
    fields_.reserve(field_cnt);
    ptr += sizeof(uint32_t);

    for (uint32_t i = 0; i < field_cnt; i++)
    {
        uint32_t field_size = *reinterpret_cast<const uint32_t *>(ptr);
        ptr += sizeof(uint32_t);
        fields_.emplace_back(ptr, field_size);
        ptr += field_size;
    }
}

/**
 * The hmget command outputs values order by input field. If input field
 * doesn't exists, hmget outputs nil for that field.
 */
void HMGetCommand::OutputResult(OutputHandler *reply) const
{
    const RedisHashResult &hash_result = result_;
    if (hash_result.err_code_ == RD_OK)
    {
        const auto &values = std::get<std::vector<std::optional<std::string>>>(
            hash_result.result_);

        assert(fields_.size() == values.size());
        reply->OnArrayStart(values.size());

        for (const auto &value : values)
        {
            if (value.has_value())
            {
                reply->OnString(value.value());
            }
            else
            {
                reply->OnNil();
            }
        }
        reply->OnArrayEnd();
    }
    else if (hash_result.err_code_ == RD_NIL)
    {
        reply->OnArrayStart(fields_.size());
        for (const EloqString &field : fields_)
        {
            (void) field;
            reply->OnNil();
        }
        reply->OnArrayEnd();
    }
    else
    {
        reply->OnError(redis_get_error_messages(hash_result.err_code_));
    }
}

txservice::ExecResult HKeysCommand::ExecuteOn(const txservice::TxObject &object)
{
    RedisHashResult &hash_result = result_;
    if (!CheckTypeMatch(object))
    {
        hash_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const RedisHashObject &hash_obj =
        static_cast<const RedisHashObject &>(object);
    hash_obj.Execute(*this);
    return txservice::ExecResult::Read;
}

void HKeysCommand::OutputResult(OutputHandler *reply) const
{
    const auto &hash_result = result_;
    if (hash_result.err_code_ == RD_OK)
    {
        const auto &list =
            std::get<std::vector<std::string>>(hash_result.result_);
        reply->OnArrayStart(list.size());
        for (const auto &key : list)
        {
            reply->OnString(key);
        }
        reply->OnArrayEnd();
    }
    else if (hash_result.err_code_ == RD_NIL)
    {
        reply->OnArrayStart(0);
        reply->OnArrayEnd();
    }
    else
    {
        reply->OnError(redis_get_error_messages(hash_result.err_code_));
    }
}

txservice::ExecResult HValsCommand::ExecuteOn(const txservice::TxObject &object)
{
    RedisHashResult &hash_result = result_;
    if (!CheckTypeMatch(object))
    {
        hash_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const RedisHashObject &hash_obj =
        static_cast<const RedisHashObject &>(object);
    hash_obj.Execute(*this);
    return txservice::ExecResult::Read;
}

void HValsCommand::OutputResult(OutputHandler *reply) const
{
    const auto &hash_result = result_;
    if (hash_result.err_code_ == RD_OK)
    {
        const auto &list =
            std::get<std::vector<std::string>>(hash_result.result_);
        reply->OnArrayStart(list.size());
        for (const auto &val : list)
        {
            reply->OnString(val);
        }
        reply->OnArrayEnd();
    }
    else if (hash_result.err_code_ == RD_NIL)
    {
        reply->OnArrayStart(0);
        reply->OnArrayEnd();
    }
    else
    {
        reply->OnError(redis_get_error_messages(hash_result.err_code_));
    }
}

HSetNxCommand::HSetNxCommand(const HSetNxCommand &rhs)
    : HashCommand(rhs), key_(rhs.key_.Clone()), value_(rhs.value_.Clone())
{
}

txservice::ExecResult HSetNxCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    RedisHashResult &hash_result = result_;
    if (!CheckTypeMatch(object))
    {
        hash_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const RedisHashObject &hash_obj =
        static_cast<const RedisHashObject &>(object);
    bool success = hash_obj.Execute(*this);
    return success ? txservice::ExecResult::Write : txservice::ExecResult::Fail;
}

txservice::TxObject *HSetNxCommand::CommitOn(txservice::TxObject *const obj_ptr)
{
    auto &hash_obj = static_cast<RedisHashObject &>(*obj_ptr);
    hash_obj.CommitHSetNx(key_, value_);
    return obj_ptr;
}

void HSetNxCommand::Serialize(std::string &str) const
{
    uint8_t type = static_cast<uint8_t>(RedisCommandType::HSETNX);
    str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));

    uint32_t key_size = key_.Length();
    str.append(reinterpret_cast<const char *>(&key_size), sizeof(uint32_t));
    str.append(key_.Data(), key_size);

    uint32_t value_size = value_.Length();
    str.append(reinterpret_cast<const char *>(&value_size), sizeof(uint32_t));
    str.append(value_.Data(), value_size);
}

void HSetNxCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();

    uint32_t key_size = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);
    key_ = EloqString(ptr, key_size);
    ptr += key_size;

    uint32_t value_size = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);
    value_ = EloqString(ptr, value_size);
    ptr += value_size;
}

void HSetNxCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.err_code_ == RD_OK)
    {
        reply->OnInt(1);
    }
    else if (result_.err_code_ == RD_NIL)
    {
        reply->OnInt(0);
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

txservice::ExecResult HRandFieldCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    RedisHashResult &hash_result = result_;
    if (!CheckTypeMatch(object))
    {
        hash_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const RedisHashObject &hash_obj =
        static_cast<const RedisHashObject &>(object);
    hash_obj.Execute(*this);
    return txservice::ExecResult::Read;
}

void HRandFieldCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.err_code_ == RD_OK)
    {
        const auto &hash_result =
            std::get<std::vector<std::string>>(result_.result_);
        if (with_count_)
        {
            reply->OnArrayStart(hash_result.size());
            for (const std::string &s : hash_result)
            {
                reply->OnString(s);
            }
            reply->OnArrayEnd();
        }
        else
        {
            assert(hash_result.size() <= 1);
            if (hash_result.size() == 0)
            {
                reply->OnNil();
            }
            else
            {
                reply->OnString(hash_result.front());
            }
        }
    }
    else if (result_.err_code_ == RD_NIL)
    {
        if (with_count_)
        {
            reply->OnArrayStart(0);
            reply->OnArrayEnd();
        }
        else
        {
            reply->OnNil();
        }
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

void HRandFieldCommand::Serialize(std::string &str) const
{
    uint8_t type = static_cast<uint8_t>(RedisCommandType::HRANDFIELD);
    str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));

    str.append(reinterpret_cast<const char *>(&count_), sizeof(int64_t));
    str.append(reinterpret_cast<const char *>(&with_count_), sizeof(bool));
    str.append(reinterpret_cast<const char *>(&with_values_), sizeof(bool));
}

void HRandFieldCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    count_ = *reinterpret_cast<const int64_t *>(ptr);
    ptr += sizeof(int64_t);
    with_count_ = *reinterpret_cast<const bool *>(ptr);
    ptr += sizeof(bool);
    with_values_ = *reinterpret_cast<const bool *>(ptr);
}

txservice::ExecResult HScanCommand::ExecuteOn(const txservice::TxObject &object)
{
    RedisHashResult &hash_result = result_;
    if (!CheckTypeMatch(object))
    {
        hash_result.err_code_ = RD_ERR_WRONG_TYPE;
        return txservice::ExecResult::Fail;
    }

    const RedisHashObject &hash_obj =
        static_cast<const RedisHashObject &>(object);
    hash_obj.Execute(*this);
    return txservice::ExecResult::Read;
}

void HScanCommand::Serialize(std::string &str) const
{
    uint8_t type = static_cast<uint8_t>(RedisCommandType::HSCAN);
    str.append(reinterpret_cast<const char *>(&type), sizeof(uint8_t));

    str.append(reinterpret_cast<const char *>(&novalues_), sizeof(bool));
    str.append(reinterpret_cast<const char *>(&match_), sizeof(bool));
    uint32_t patten_size = pattern_.Length();
    str.append(reinterpret_cast<const char *>(&patten_size), sizeof(uint32_t));
    str.append(pattern_.Data(), patten_size);

    str.append(reinterpret_cast<const char *>(&count_), sizeof(uint64_t));
    str.append(reinterpret_cast<const char *>(&cursor_), sizeof(int64_t));
}

void HScanCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    novalues_ = *reinterpret_cast<const bool *>(ptr);
    ptr += sizeof(bool);
    match_ = *reinterpret_cast<const bool *>(ptr);
    ptr += sizeof(bool);

    uint32_t patten_size = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);

    pattern_ = EloqString(ptr, patten_size);
    ptr += patten_size;

    count_ = *reinterpret_cast<const uint64_t *>(ptr);
    ptr += sizeof(uint64_t);
    cursor_ = *reinterpret_cast<const int64_t *>(ptr);
    ptr += sizeof(int64_t);
}

void HScanCommand::OutputResult(OutputHandler *reply) const
{
    const auto &hash_result = result_;
    if (hash_result.err_code_ == RD_OK)
    {
        auto &ans_list =
            std::get<std::vector<std::string>>(hash_result.result_);
        reply->OnArrayStart(2);

        reply->OnString(*ans_list.begin());

        reply->OnArrayStart(ans_list.size() - 1);
        for (auto hscan_it = ans_list.begin() + 1; hscan_it != ans_list.end();
             hscan_it++)
        {
            reply->OnString(*hscan_it);
        }
        reply->OnArrayEnd();

        reply->OnArrayEnd();
    }
    else if (hash_result.err_code_ == RD_NIL)
    {
        reply->OnArrayStart(2);

        reply->OnString("0");

        reply->OnArrayStart(0);
        reply->OnArrayEnd();

        reply->OnArrayEnd();
    }
    else
    {
        reply->OnError(redis_get_error_messages(hash_result.err_code_));
    }
}

ScanCommand::ScanCommand(std::string_view pattern)
    : scan_cursor_(nullptr),
      pattern_(pattern),
      count_(-1),
      obj_type_(RedisObjectType::Unknown)
{
}

ScanCommand::ScanCommand(BucketScanCursor *scan_cursor,
                         std::string_view pattern,
                         int64_t count,
                         RedisObjectType obj_type)
    : scan_cursor_(scan_cursor),
      pattern_(pattern),
      count_(count),
      obj_type_(obj_type)
{
    assert(count_ > 0);
}

bool ScanCommand::Execute(RedisServiceImpl *redis_impl,
                          RedisConnectionContext *ctx,
                          const txservice::TableName *table,
                          txservice::TransactionExecution *txm,
                          OutputHandler *output,
                          bool auto_commit)
{
    return redis_impl->ExecuteCommand(
        ctx, txm, table, this, output, auto_commit);
}

void ScanCommand::OutputResult(OutputHandler *reply,
                               RedisConnectionContext *ctx) const
{
    if (result_.err_code_ == RD_OK || result_.err_code_ == RD_NIL)
    {
        if (!IsKeysCmd())
        {
            reply->OnArrayStart(2);
            reply->OnString(std::to_string(result_.cursor_id_));
        }

        reply->OnArrayStart(result_.vct_key_.size());
        for (const auto &key : result_.vct_key_)
        {
            reply->OnString(key);
        }
        reply->OnArrayEnd();

        if (!IsKeysCmd())
        {
            reply->OnArrayEnd();
        }
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

/**
 * +-----------+--------------------------+-------------------+------------+
 * |1 byte type|Serialized RedisEloqObject|2 byte MONO version|8 byte CRC64|
 * +-----------+--------------------------+-------------------+------------+
 *
 * MONO version and CRC are both in big endian.
 */
txservice::ExecResult DumpCommand::ExecuteOn(const txservice::TxObject &object)
{
    const RedisEloqObject &eloq_obj =
        dynamic_cast<const RedisEloqObject &>(object);

    RedisObjectType obj_type = eloq_obj.ObjectType();
    assert(obj_type != RedisObjectType::Unknown &&
           obj_type != RedisObjectType::Del);

    uint8_t obj_type_u8 = static_cast<uint8_t>(obj_type);
    result_.str_.append(reinterpret_cast<const char *>(&obj_type_u8),
                        sizeof(uint8_t));

    eloq_obj.Serialize(result_.str_);

    result_.str_.append(reinterpret_cast<const char *>(&dump_version_),
                        sizeof(dump_version_));

    uint64_t crc64 =
        crc64speed_big(0, result_.str_.data(), result_.str_.size());
    result_.str_.append(reinterpret_cast<const char *>(&crc64), sizeof(crc64));

    result_.err_code_ = RD_OK;

    return txservice::ExecResult::Read;
}

void DumpCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.err_code_ == RD_OK)
    {
        reply->OnString(result_.str_);
    }
    else if (result_.err_code_ == RD_NIL)
    {
        reply->OnNil();
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

std::unique_ptr<txservice::TxRecord> RestoreCommand::CreateObject(
    const std::string *image) const
{
    // Construct a dummy object. CommitOn will create a real object.
    return std::make_unique<RedisStringObject>();
}

txservice::ExecResult RestoreCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    result_.err_code_ = RD_OK;
    // If the RESTORE command does not modify the object, it won't proceed to
    // ExecuteOn.
    return ExecResult::Write;
}

txservice::TxObject *RestoreCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    RedisEloqObject *robj = nullptr;

    const uint8_t *obj_type_ptr =
        reinterpret_cast<const uint8_t *>(value_.data());
    RedisObjectType obj_type = static_cast<RedisObjectType>(*obj_type_ptr);
    size_t offset = 1;

    switch (obj_type)
    {
    case RedisObjectType::String:
        robj = new RedisStringObject();
        robj->Deserialize(value_.data(), offset);
        break;
    case RedisObjectType::List:
        robj = new RedisListObject();
        robj->Deserialize(value_.data(), offset);
        break;
    case RedisObjectType::Hash:
        robj = new RedisHashObject();
        robj->Deserialize(value_.data(), offset);
        break;
    case RedisObjectType::Zset:
        robj = new RedisZsetObject();
        robj->Deserialize(value_.data(), offset);
        break;
    case RedisObjectType::Set:
        robj = new RedisHashSetObject();
        robj->Deserialize(value_.data(), offset);
        break;
    default:
        assert(false && "Unknown redis object type");
        break;
    }

    return robj;
}

void RestoreCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::RESTORE);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));

    uint32_t len = value_.size();
    str.append(reinterpret_cast<const char *>(&len), sizeof(uint32_t));
    str.append(value_.data(), value_.size());

    str.append(reinterpret_cast<const char *>(&expire_when_), sizeof(uint64_t));

    uint8_t replace = static_cast<uint8_t>(replace_);
    str.append(reinterpret_cast<const char *>(&replace), sizeof(uint8_t));

    str.append(reinterpret_cast<const char *>(&idle_time_sec_),
               sizeof(uint64_t));
    str.append(reinterpret_cast<const char *>(&frequency_), sizeof(uint64_t));
}

void RestoreCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();
    const uint32_t str_len = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(uint32_t);
    value_ = std::string(ptr, str_len);
    ptr += str_len;

    expire_when_ = *reinterpret_cast<const uint64_t *>(ptr);
    ptr += sizeof(uint64_t);

    replace_ = *reinterpret_cast<const uint8_t *>(ptr);
    ptr += sizeof(uint8_t);

    idle_time_sec_ = *reinterpret_cast<const uint64_t *>(ptr);
    ptr += sizeof(uint64_t);

    frequency_ = *reinterpret_cast<const uint64_t *>(ptr);
    ptr += sizeof(uint64_t);
}

void RestoreCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.err_code_ == RD_OK)
    {
        reply->OnStatus(redis_get_error_messages(RD_OK));
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

bool RestoreCommand::VerifyDumpPayload(std::string_view dump_payload)
{
    if (dump_payload.size() < 10)
    {
        return false;
    }

    uint16_t dump_version = *(reinterpret_cast<const uint16_t *>(
        dump_payload.data() + dump_payload.size() - 10));
    if (dump_version > DumpCommand::dump_version_)
    {
        return false;
    }

    uint64_t crc64_val = *(reinterpret_cast<const uint64_t *>(
        dump_payload.data() + dump_payload.size() - 8));

    return crc64_val ==
           crc64speed_big(0, dump_payload.data(), dump_payload.size() - 8);
}

#ifdef WITH_FAULT_INJECT
RedisFaultInjectCommand::RedisFaultInjectCommand(std::string_view fault_name,
                                                 std::string_view fault_paras,
                                                 int vct_node_id)
    : fault_name_(fault_name), fault_paras_(fault_paras)
{
    vct_node_id_.push_back(vct_node_id);
}

bool RedisFaultInjectCommand::Execute(RedisServiceImpl *redis_impl,
                                      RedisConnectionContext *ctx,
                                      const txservice::TableName *table,
                                      txservice::TransactionExecution *txm,
                                      OutputHandler *output,
                                      bool auto_commit)
{
    return redis_impl->ExecuteCommand(ctx, txm, this, output, auto_commit);
}

void RedisFaultInjectCommand::OutputResult(OutputHandler *reply,
                                           RedisConnectionContext *ctx) const
{
    reply->OnStatus(redis_get_error_messages(RD_OK));
}
#endif

std::unique_ptr<txservice::TxRecord> RedisMemoryUsageCommand::CreateObject(
    const std::string *image) const
{
    return nullptr;
}

txservice::ExecResult RedisMemoryUsageCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    const auto &obj = static_cast<const RedisEloqObject &>(object);
    result_.int_val_ = obj.SerializedLength();
    result_.err_code_ = RD_OK;
    return txservice::ExecResult::Read;
}

void RedisMemoryUsageCommand::Serialize(std::string &str) const
{
    auto cmd_type = static_cast<uint8_t>(RedisCommandType::MEMORY_USAGE);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(uint8_t));
}

void RedisMemoryUsageCommand::Deserialize(std::string_view cmd_image)
{
}

void RedisMemoryUsageCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.err_code_ == RD_OK)
    {
        reply->OnInt(result_.int_val_ + key_len_);
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

std::tuple<bool, EloqKey, ZRangeCommand> ParseZRangeByRankCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "zrangebyrank");
    if (args.size() < 4)
    {
        output->OnError(
            "ERR wrong number of arguments for 'zrangebyrank' command");
        return {false, EloqKey(), ZRangeCommand()};
    }

    ZRangeConstraint constraint;
    ZRangeSpec spec;
    constraint.rangetype_ = ZRangeType::ZRANGE_RANK;

    auto [success, err_code] = ParseZrangeElement(args, constraint, spec);
    if (!success)
    {
        output->OnError(redis_get_error_messages(err_code));
        return {false, EloqKey(), ZRangeCommand()};
    }

    return {true, EloqKey(args[1]), ZRangeCommand(constraint, std::move(spec))};
}

std::tuple<bool, EloqKey, ZRangeCommand> ParseZRangeByScoreCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "zrangebyscore");
    if (args.size() < 4)
    {
        output->OnError(
            "ERR wrong number of arguments for 'zrangebyscore' command");
        return {false, EloqKey(), ZRangeCommand()};
    }

    ZRangeConstraint constraint;
    ZRangeSpec spec;
    constraint.rangetype_ = ZRangeType::ZRANGE_SCORE;

    auto [success, err_code] = ParseZrangeElement(args, constraint, spec);
    if (!success)
    {
        output->OnError(redis_get_error_messages(err_code));
        return {false, EloqKey(), ZRangeCommand()};
    }

    return {true, EloqKey(args[1]), ZRangeCommand(constraint, std::move(spec))};
}

std::tuple<bool, EloqKey, ZRangeCommand> ParseZRangeByLexCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "zrangebylex");
    if (args.size() < 4)
    {
        output->OnError(
            "ERR wrong number of arguments for 'zrangebylex' command");
        return {false, EloqKey(), ZRangeCommand()};
    }

    ZRangeConstraint constraint;
    ZRangeSpec spec;
    constraint.rangetype_ = ZRangeType::ZRANGE_LEX;

    auto [success, err_code] = ParseZrangeElement(args, constraint, spec);
    if (!success)
    {
        output->OnError(redis_get_error_messages(err_code));
        return {false, EloqKey(), ZRangeCommand()};
    }

    return {true, EloqKey(args[1]), ZRangeCommand(constraint, std::move(spec))};
}

std::tuple<bool, EloqKey, ZRangeCommand> ParseZRevRangeCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "zrevrange");
    if (args.size() < 4 || args.size() > 5)
    {
        output->OnError(
            "ERR wrong number of arguments for 'zrevrange' command");
        return {false, EloqKey(), ZRangeCommand()};
    }

    ZRangeConstraint constraint;
    ZRangeSpec spec;
    constraint.rangetype_ = ZRangeType::ZRANGE_RANK;
    constraint.direction_ = ZRangeDirection::ZRANGE_DIRECTION_REVERSE;

    auto [success, err_code] = ParseZrangeElement(args, constraint, spec);
    if (!success)
    {
        output->OnError(redis_get_error_messages(err_code));
        return {false, EloqKey(), ZRangeCommand()};
    }

    return {true, EloqKey(args[1]), ZRangeCommand(constraint, std::move(spec))};
}

std::tuple<bool, EloqKey, ZRangeCommand> ParseZRevRangeByScoreCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "zrevrangebyscore");
    if (args.size() < 4)
    {
        output->OnError(
            "ERR wrong number of arguments for 'zrevrangebyscore' command");
        return {false, EloqKey(), ZRangeCommand()};
    }

    ZRangeConstraint constraint;
    ZRangeSpec spec;
    constraint.rangetype_ = ZRangeType::ZRANGE_SCORE;
    constraint.direction_ = ZRangeDirection::ZRANGE_DIRECTION_REVERSE;

    auto [success, err_code] = ParseZrangeElement(args, constraint, spec);
    if (!success)
    {
        output->OnError(redis_get_error_messages(err_code));
        return {false, EloqKey(), ZRangeCommand()};
    }

    return {true, EloqKey(args[1]), ZRangeCommand(constraint, std::move(spec))};
}

std::tuple<bool, EloqKey, ZRangeCommand> ParseZRevRangeByLexCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "zrevrangebylex");
    if (args.size() < 4)
    {
        output->OnError(
            "ERR wrong number of arguments for 'zrevrangebylex' command");
        return {false, EloqKey(), ZRangeCommand()};
    }

    ZRangeConstraint constraint;
    ZRangeSpec spec;
    constraint.rangetype_ = ZRangeType::ZRANGE_LEX;
    constraint.direction_ = ZRangeDirection::ZRANGE_DIRECTION_REVERSE;

    auto [success, err_code] = ParseZrangeElement(args, constraint, spec);
    if (!success)
    {
        output->OnError(redis_get_error_messages(err_code));
        return {false, EloqKey(), ZRangeCommand()};
    }

    return {true, EloqKey(args[1]), ZRangeCommand(constraint, std::move(spec))};
}

std::pair<bool, std::string> ParseZRemRangeElement(
    const std::vector<std::string_view> &args,
    ZRangeType &range_type,
    ZRangeSpec &spec)
{
    // parse the range
    switch (range_type)
    {
    case ZRangeType::ZRANGE_SCORE:
    {
        // ZREMRANGEBYSCORE key min max

        auto [get_min_succeed, min_excluded, min] = GetDoubleFromSv(args[2]);
        auto [get_max_succeed, max_excluded, max] = GetDoubleFromSv(args[3]);
        if (!get_min_succeed || !get_max_succeed)
        {
            return {false, redis_get_error_messages(RD_ERR_MIN_MAX_NOT_FLOAT)};
        }
        spec.opt_start_ = min;
        spec.opt_end_ = max;
        spec.minex_ = min_excluded;
        spec.maxex_ = max_excluded;
        break;
    }
    case ZRangeType::ZRANGE_LEX:
    {
        // ZREMRANGEBYLEX key min max
        if (args[2] == "+" || args[3] == "-")
        {
            spec.null_ = true;
            break;
        }

        auto [get_min_succeed, min_excluded, min, is_negative_sign] =
            GetEloqStringFromSv(args[2]);
        auto [get_max_succeed, max_excluded, max, is_positive_sign] =
            GetEloqStringFromSv(args[3]);
        if (!get_min_succeed || !get_max_succeed)
        {
            return {false, redis_get_error_messages(RD_ERR_MIN_MAX_NOT_STRING)};
        }

        spec.negative_ = is_negative_sign;
        spec.positive_ = is_positive_sign;
        spec.opt_start_.emplace<EloqString>(std::move(min));
        spec.opt_end_.emplace<EloqString>(std::move(max));
        assert(std::get<EloqString>(spec.opt_start_).Type() ==
               EloqString::StorageType::View);
        assert(std::get<EloqString>(spec.opt_end_).Type() ==
               EloqString::StorageType::View);
        spec.minex_ = min_excluded;
        spec.maxex_ = max_excluded;
        break;
    }
    case ZRangeType::ZRANGE_RANK:
    {
        // ZREMRANGEBYRANK key start stop

        auto [get_start_succeed, start_excluded, start] = GetIntFromSv(args[2]);
        auto [get_end_succeed, end_excluded, end] = GetIntFromSv(args[3]);
        if (!get_start_succeed || !get_end_succeed)
        {
            return {false, redis_get_error_messages(RD_ERR_DIGITAL_INVALID)};
        }
        spec.opt_start_ = start;
        spec.opt_end_ = end;
        spec.minex_ = start_excluded;
        spec.maxex_ = end_excluded;
        break;
    }
    default:
    {
        assert(false && "Unknown range type");
        break;
    }
    }
    return {true, ""};
}

std::tuple<bool, EloqKey, ZRemRangeCommand> ParseZRemRangeCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    if (args.size() < 4)
    {
        output->OnError(
            "ERR wrong number of arguments for 'zremrangeby...' command");
        return {false, EloqKey(), ZRemRangeCommand()};
    }

    ZRangeType range_type;
    ZRangeSpec spec;

    if (args[0] == "zremrangebyscore")
    {
        range_type = ZRangeType::ZRANGE_SCORE;
    }
    else if (args[0] == "zremrangebylex")
    {
        range_type = ZRangeType::ZRANGE_LEX;
    }
    else if (args[0] == "zremrangebyrank")
    {
        range_type = ZRangeType::ZRANGE_RANK;
    }
    else
    {
        assert(false && "wrong command");
    }

    auto [success, err_code] = ParseZRemRangeElement(args, range_type, spec);
    if (!success)
    {
        output->OnError(err_code);
        return {false, EloqKey(), ZRemRangeCommand()};
    }

    return {
        true, EloqKey(args[1]), ZRemRangeCommand(range_type, std::move(spec))};
}

std::tuple<bool, EloqKey, ZLexCountCommand> ParseZLexCountCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "zlexcount");
    if (args.size() != 4)
    {
        output->OnError(
            "ERR wrong number of arguments for 'zlexcount' command");
        return {false, EloqKey(), ZLexCountCommand()};
    }

    if (args[2] == "+" || args[3] == "-")
    {
        ZRangeSpec spec;
        spec.null_ = true;
        return {true, EloqKey(args[1]), ZLexCountCommand(std::move(spec))};
    }

    auto [ec_lex_start, bool_lex_start, lex_start, negative] =
        GetEloqStringFromSv(args[2]);
    auto [ec_lex_end, bool_lex_end, lex_end, positive] =
        GetEloqStringFromSv(args[3]);

    if (!ec_lex_start || !ec_lex_end)
    {
        output->OnError(redis_get_error_messages(RD_ERR_MIN_MAX_NOT_STRING));
        return {false, EloqKey(), ZLexCountCommand()};
    }

    ZRangeSpec spec;
    spec.negative_ = negative;
    spec.positive_ = positive;
    spec.opt_start_.emplace<EloqString>(std::move(lex_start));
    spec.opt_end_.emplace<EloqString>(std::move(lex_end));
    spec.minex_ = bool_lex_start;
    spec.maxex_ = bool_lex_end;

    return {true, EloqKey(args[1]), ZLexCountCommand(std::move(spec))};
}

std::tuple<bool, EloqKey, ZPopCommand> ParseZPopMinCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "zpopmin");
    if (args.size() < 2 || args.size() > 3)
    {
        output->OnError("ERR wrong number of arguments for 'zpopmin' command");
        return {false, EloqKey(), ZPopCommand()};
    }

    if (args.size() == 2)
    {
        return {true,
                EloqKey(args[1]),
                ZPopCommand(ZPopCommand::PopType::POPMIN, 1)};
    }

    int64_t count = 0;
    if (!string2ll(args[2].data(), args[2].size(), count) || count < 0)
    {
        output->OnError(redis_get_error_messages(RD_ERR_POSITIVE_INVALID));
        return {false, EloqKey(), ZPopCommand()};
    }

    return {true,
            EloqKey(args[1]),
            ZPopCommand(ZPopCommand::PopType::POPMIN, count)};
}

std::tuple<bool, EloqKey, ZPopCommand> ParseZPopMaxCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "zpopmax");
    if (args.size() < 2 || args.size() > 3)
    {
        output->OnError("ERR wrong number of arguments for 'zpopmax' command");
        return {false, EloqKey(), ZPopCommand()};
    }

    if (args.size() == 2)
    {
        return {true,
                EloqKey(args[1]),
                ZPopCommand(ZPopCommand::PopType::POPMAX, 1)};
    }

    int64_t count = 0;
    if (!string2ll(args[2].data(), args[2].size(), count) || count < 0)
    {
        output->OnError(redis_get_error_messages(RD_ERR_POSITIVE_INVALID));
        return {false, EloqKey(), ZPopCommand()};
    }

    return {true,
            EloqKey(args[1]),
            ZPopCommand(ZPopCommand::PopType::POPMAX, count)};
}

std::tuple<bool, EloqKey, ZScanCommand> ParseZScanCommand(
    RedisConnectionContext *ctx,
    const std::vector<std::string_view> &args,
    OutputHandler *output)
{
    assert(args[0] == "zscan");
    // ZSCAN key cursor [MATCH pattern] [COUNT count]
    if (args.size() < 3)
    {
        output->OnError("ERR wrong number of arguments for 'zscan' command");
        return {false, EloqKey(), ZScanCommand()};
    }

    uint64_t cursor;
    if (!string2ull(args[2].data(), args[2].size(), cursor))
    {
        output->OnError(redis_get_error_messages(RD_ERR_INVALID_CURSOR));
        return {false, EloqKey(), ZScanCommand()};
    }

    // parse cursor
    bool with_cursor = false;
    EloqString cursor_field;
    double cursor_score = 0.0;
    if (cursor > 0)
    {
        auto tmp_res = ctx->FindScanCursor(cursor);
        if (tmp_res.first)
        {
            ZScanCommand::DeserializeCursorZNode(
                *tmp_res.second, cursor_field, cursor_score);
            with_cursor = true;
        }
    }

    size_t args_index = 3;

    int64_t count = 0;
    bool match = false;
    EloqString pattern;

    if (IsOdd(args.size() - args_index))
    {
        output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
        return {false, EloqKey(), ZScanCommand()};
    }

    // According to the redis command implementation, the query is
    // determined by the last time input parameter
    while (args_index < args.size())
    {
        if (stringcomp(args[args_index], "match", 1))
        {
            match = true;
            pattern = EloqString(args[args_index + 1]);
        }
        else if (stringcomp(args[args_index], "count", 1))
        {
            if (!string2ll(args[args_index + 1].data(),
                           args[args_index + 1].size(),
                           count))
            {
                output->OnError(
                    redis_get_error_messages(RD_ERR_DIGITAL_INVALID));
                return {false, EloqKey(), ZScanCommand()};
            }
            if (count < 1)
            {
                output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
                return {false, EloqKey(), ZScanCommand()};
            }
        }
        else
        {
            output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
            return {false, EloqKey(), ZScanCommand()};
        }
        args_index += 2;
    }

    return {true,
            EloqKey(args[1]),
            ZScanCommand(with_cursor,
                         std::move(cursor_field),
                         cursor_score,
                         match,
                         std::move(pattern),
                         count)};
}

std::tuple<bool, EloqKey, ZRandMemberCommand> ParseZRandMemberCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "zrandmember");
    if (args.size() < 2 || args.size() > 4)
    {
        output->OnError(
            "ERR wrong number of arguments for 'zrandmember' command");
        return {false, EloqKey(), ZRandMemberCommand()};
    }

    bool count_provided = false;
    int64_t count = 1;
    bool with_scores = false;

    bool parse_count_succeed = false;
    if (args.size() > 2)
    {
        parse_count_succeed = string2ll(args[2].data(), args[2].size(), count);
        if (parse_count_succeed)
        {
            count_provided = true;
        }
    }

    bool parse_withscores_succeed;
    if (args.size() == 4)
    {
        parse_withscores_succeed = stringcomp(args[3], "withscores", 1);
    }

    if (args.size() == 3)
    {
        if (!parse_count_succeed)
        {
            output->OnError(redis_get_error_messages(RD_ERR_DIGITAL_INVALID));
            return {false, EloqKey(), ZRandMemberCommand()};
        }
        else if (count <= LLONG_MIN || count >= LLONG_MAX)
        {
            output->OnError(
                redis_get_error_messages(RD_ERR_VALUE_OUT_OF_RANGE));
            return {false, EloqKey(), ZRandMemberCommand()};
        }
    }

    if (args.size() == 4)
    {
        if (!parse_count_succeed)
        {
            output->OnError(redis_get_error_messages(RD_ERR_DIGITAL_INVALID));
            return {false, EloqKey(), ZRandMemberCommand()};
        }
        else if (!parse_withscores_succeed)
        {
            output->OnError("ERR syntax error");
            return {false, EloqKey(), ZRandMemberCommand()};
        }
        else if (count <= LLONG_MIN / 2 || count > (LLONG_MAX - 1) / 2)
        {
            output->OnError(redis_get_error_messages(RD_ERR_POSITIVE_INVALID));
            return {false, EloqKey(), ZRandMemberCommand()};
        }
        else
        {
            with_scores = true;
        }
    }

    return {true,
            EloqKey(args[1]),
            ZRandMemberCommand(count, with_scores, count_provided)};
}

std::tuple<bool, EloqKey, ZRankCommand> ParseZRankCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    // ZRANK key member [WITHSCORE]
    assert(args[0] == "zrank");

    if (args.size() < 2 || args.size() > 4)
    {
        output->OnError("ERR wrong number of arguments for 'zrank' command");
        return {false, EloqKey(), ZRankCommand()};
    }

    bool with_score = false;

    if (args.size() == 4)
    {
        if (stringcomp(args[3], "withscore", 1))
        {
            with_score = true;
        }
        else
        {
            output->OnError("ERR syntax error");
            return {false, EloqKey(), ZRankCommand()};
        }
    }

    return {true, EloqKey(args[1]), ZRankCommand(args[2], with_score, false)};
}

std::tuple<bool, EloqKey, ZRankCommand> ParseZRevRankCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    // ZREVRANK key member [WITHSCORE]
    assert(args[0] == "zrevrank");

    if (args.size() < 2 || args.size() > 4)
    {
        output->OnError("ERR wrong number of arguments for 'zrevrank' command");
        return {false, EloqKey(), ZRankCommand()};
    }

    bool with_score = false;

    if (args.size() == 4)
    {
        if (stringcomp(args[3], "withscore", 1))
        {
            with_score = true;
        }
        else
        {
            output->OnError("ERR syntax error");
            return {false, EloqKey(), ZRankCommand()};
        }
    }

    return {true, EloqKey(args[1]), ZRankCommand(args[2], with_score, true)};
}

std::tuple<bool, EloqKey, ZMScoreCommand> ParseZMScoreCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "zmscore");
    if (args.size() < 3)
    {
        output->OnError("ERR wrong number of arguments for 'zmscore' command");
        return {false, EloqKey(), ZMScoreCommand()};
    }

    std::vector<EloqString> vct;
    for (size_t i = 2; i < args.size(); i++)
    {
        vct.emplace_back(args[i]);
    }

    return {true, EloqKey(args[1]), ZMScoreCommand(std::move(vct))};
}

std::pair<bool, ZMPopCommand> ParseZMPopCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "zmpop");
    if (args.size() < 4)
    {
        output->OnError("ERR wrong number of arguments for 'zmpop' command");
        return {false, ZMPopCommand()};
    }

    uint64_t num;
    if (!string2ull(args[1].data(), args[1].size(), num) || num <= 0)
    {
        output->OnError("ERR numkeys should be greater than 0");
        return {false, ZMPopCommand()};
    }
    if (num + 2 >= args.size())
    {
        output->OnError("ERR syntax error");
        return {false, ZMPopCommand()};
    }

    ZPopCommand::PopType ptype;
    if (stringcomp(args[num + 2], "MIN", 1))
    {
        ptype = ZPopCommand::PopType::POPMIN;
    }
    else if (stringcomp(args[num + 2], "MAX", 1))
    {
        ptype = ZPopCommand::PopType::POPMAX;
    }
    else
    {
        output->OnError("ERR syntax error");
        return {false, ZMPopCommand()};
    }

    uint64_t count = 1;
    if (num + 3 < args.size())
    {
        if (num + 5 != args.size() || !stringcomp(args[num + 3], "COUNT", 1))
        {
            output->OnError("ERR syntax error");
            return {false, ZMPopCommand()};
        }

        if (!string2ull(args[num + 4].data(), args[num + 4].size(), count) ||
            count <= 0)
        {
            output->OnError("ERR count should be greater than 0");
            return {false, ZMPopCommand()};
        }
    }

    std::vector<EloqKey> vct_key;
    std::vector<ZPopCommand> vct_cmd;
    vct_key.reserve(num);
    vct_cmd.reserve(num);
    for (size_t i = 0; i < num; i++)
    {
        vct_key.emplace_back(args[i + 2]);
        vct_cmd.emplace_back(ptype, count);
    }

    return {true, ZMPopCommand(std::move(vct_key), std::move(vct_cmd))};
}

std::tuple<bool, EloqKey, TypeCommand> ParseTypeCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "type");
    if (args.size() != 2)
    {
        output->OnError("ERR wrong number of arguments for 'type' command");
        return {false, EloqKey(), TypeCommand()};
    }

    return {true, EloqKey(args[1]), TypeCommand()};
}

static bool ParseSetOpOptionArgs(const std::vector<std::string_view> &args,
                                 OutputHandler *output,
                                 size_t num_key,
                                 bool is_store_cmd,
                                 std::vector<double> &weights,
                                 AggregateType &aggregate_type,
                                 bool &opt_withscores)
{
    size_t idx = 2 + num_key;
    if (is_store_cmd)
    {
        // dest key
        idx = 3 + num_key;
    }

    for (; idx < args.size(); ++idx)
    {
        if (stringcomp(args[idx], "withscores", 1))
        {
            if (is_store_cmd)
            {
                // ZUNIONSTORE/ZINTERSTORE doesn't support `withscores`
                // option.
                output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
                return false;
            }

            opt_withscores = true;
        }
        else if (stringcomp(args[idx], "weights", 1))
        {
            if (idx + num_key + 1 > args.size())
            {
                output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
                return false;
            }

            for (size_t j = idx + 1; j < idx + num_key + 1; ++j)
            {
                double weight{};
                if (!string2double(args[j].data(), args[j].size(), weight))
                {
                    output->OnError(
                        redis_get_error_messages(RD_ERR_WEIGHT_FLOAT_VALUE));
                    return false;
                }

                weights.emplace_back(weight);
            }

            idx = idx + num_key;
        }
        else if (stringcomp(args[idx], "aggregate", 1))
        {
            if (idx + 1 >= args.size())
            {
                output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
                return false;
            }

            if (stringcomp(args[idx + 1], "sum", 1))
            {
                aggregate_type = AggregateType::SUM;
            }
            else if (stringcomp(args[idx + 1], "min", 1))
            {
                aggregate_type = AggregateType::MIN;
            }
            else if (stringcomp(args[idx + 1], "max", 1))
            {
                aggregate_type = AggregateType::MAX;
            }
            else
            {
                output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
                return false;
            }
            idx += 1;
        }
        else
        {
            output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
            return false;
        }
    }

    if (weights.empty())
    {
        weights.resize(num_key, 1);
    }

    return true;
}

std::tuple<bool, ZUnionCommand> ParseZUnionCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "zunion");
    if (args.size() < 3)
    {
        output->OnError("ERR wrong number of arguments for 'zunion' command");
        return {false, ZUnionCommand()};
    }

    int64_t num_key;
    if (!string2ll(args[1].data(), args[1].size(), num_key) || num_key <= 0)
    {
        output->OnError(
            "ERR at least 1 input key is needed for 'zunion' command");
        return {false, ZUnionCommand()};
    }

    if (args.size() < 2 + (size_t) num_key)
    {
        output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
        return {false, ZUnionCommand()};
    }

    std::vector<double> weights;
    bool opt_withscores = false;
    AggregateType aggregate_type = AggregateType::SUM;

    if (!ParseSetOpOptionArgs(args,
                              output,
                              num_key,
                              false,
                              weights,
                              aggregate_type,
                              opt_withscores))
    {
        return {false, ZUnionCommand()};
    }

    std::vector<EloqKey> vct_key;
    std::vector<SZScanCommand> vct_cmd;
    vct_key.reserve(num_key);
    vct_cmd.reserve(num_key);

    for (size_t idx = 2; idx < 2 + (size_t) num_key; ++idx)
    {
        vct_key.emplace_back(args[idx]);
    }

    for (size_t idx = 0; idx < vct_key.size(); ++idx)
    {
        vct_cmd.emplace_back(true);
    }

    return {true,
            ZUnionCommand(std::move(vct_key),
                          std::move(vct_cmd),
                          aggregate_type,
                          opt_withscores,
                          std::move(weights))};
}

std::tuple<bool, ZUnionStoreCommand> ParseZUnionStoreCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "zunionstore");
    if (args.size() < 4)
    {
        output->OnError(
            "ERR wrong number of arguments for 'zunionstore' command");
        return {false, ZUnionStoreCommand()};
    }

    int64_t num_key;
    if (!string2ll(args[2].data(), args[2].size(), num_key) || num_key <= 0)
    {
        output->OnError(
            "ERR at least 1 input key is needed for 'zunionstore' command");
        return {false, ZUnionStoreCommand()};
    }

    if (args.size() < 3 + (size_t) num_key)
    {
        output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
        return {false, ZUnionStoreCommand()};
    }

    bool opt_withscores = true;
    std::vector<double> weights;
    AggregateType aggregate_type = AggregateType::SUM;

    if (!ParseSetOpOptionArgs(args,
                              output,
                              num_key,
                              true,
                              weights,
                              aggregate_type,
                              opt_withscores))
    {
        return {false, ZUnionStoreCommand()};
    }

    std::vector<EloqKey> vct_key;
    std::vector<SZScanCommand> vct_cmd;
    vct_key.reserve(num_key);
    vct_cmd.reserve(num_key);

    for (size_t idx = 3; idx < 3 + (size_t) num_key; ++idx)
    {
        vct_key.emplace_back(args[idx]);
    }

    for (size_t idx = 0; idx < vct_key.size(); ++idx)
    {
        vct_cmd.emplace_back(true);
    }

    return {true,
            ZUnionStoreCommand(std::move(vct_key),
                               std::move(vct_cmd),
                               aggregate_type,
                               std::move(weights),
                               args[1])};
}

std::tuple<bool, ZInterCommand> ParseZInterCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "zinter");
    if (args.size() < 3)
    {
        output->OnError("ERR wrong number of arguments for 'zinter' command");
        return {false, ZInterCommand()};
    }

    int64_t num_key;
    if (!string2ll(args[1].data(), args[1].size(), num_key) || num_key <= 0)
    {
        output->OnError(
            "ERR at least 1 input key is needed for 'zinter' command");
        return {false, ZInterCommand()};
    }

    if (args.size() < 2 + (size_t) num_key)
    {
        output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
        return {false, ZInterCommand()};
    }

    std::vector<double> weights;
    bool opt_withscores = false;
    AggregateType aggregate_type = AggregateType::SUM;

    if (!ParseSetOpOptionArgs(args,
                              output,
                              num_key,
                              false,
                              weights,
                              aggregate_type,
                              opt_withscores))
    {
        return {false, ZInterCommand()};
    }

    std::vector<EloqKey> vct_key;
    std::vector<SZScanCommand> vct_cmd;
    vct_key.reserve(num_key);
    vct_cmd.reserve(num_key);

    for (size_t idx = 2; idx < 2 + (size_t) num_key; ++idx)
    {
        vct_key.emplace_back(args[idx]);
    }

    for (size_t idx = 0; idx < vct_key.size(); ++idx)
    {
        vct_cmd.emplace_back(true);
    }

    return {true,
            ZInterCommand(std::move(vct_key),
                          std::move(vct_cmd),
                          aggregate_type,
                          opt_withscores,
                          std::move(weights))};
}

std::tuple<bool, ZInterStoreCommand> ParseZInterStoreCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "zinterstore");
    if (args.size() < 4)
    {
        output->OnError(
            "ERR wrong number of arguments for 'zinterstore' command");
        return {false, ZInterStoreCommand()};
    }

    int64_t num_key;
    if (!string2ll(args[2].data(), args[2].size(), num_key) || num_key <= 0)
    {
        output->OnError(
            "ERR at least 1 input key is needed for 'zinterstore' command");
        return {false, ZInterStoreCommand()};
    }

    if (args.size() < 3 + (size_t) num_key)
    {
        output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
        return {false, ZInterStoreCommand()};
    }

    bool opt_withscores = true;
    AggregateType aggregate_type = AggregateType::SUM;
    std::vector<double> weights;

    if (!ParseSetOpOptionArgs(args,
                              output,
                              num_key,
                              true,
                              weights,
                              aggregate_type,
                              opt_withscores))
    {
        return {false, ZInterStoreCommand()};
    }

    std::vector<EloqKey> vct_key;
    std::vector<SZScanCommand> vct_cmd;
    vct_key.reserve(num_key);
    vct_cmd.reserve(num_key);

    for (size_t idx = 3; idx < 3 + (size_t) num_key; ++idx)
    {
        vct_key.emplace_back(args[idx]);
    }

    for (size_t idx = 0; idx < vct_key.size(); ++idx)
    {
        vct_cmd.emplace_back(opt_withscores);
    }

    return {true,
            ZInterStoreCommand(std::move(vct_key),
                               std::move(vct_cmd),
                               aggregate_type,
                               std::move(weights),
                               args[1])};
}

std::tuple<bool, ZDiffCommand> ParseZDiffCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "zdiff");
    if (args.size() < 3)
    {
        output->OnError("ERR wrong number of arguments for 'zdiff' command");
        return {false, ZDiffCommand()};
    }

    int64_t num_key;
    if (!string2ll(args[1].data(), args[1].size(), num_key) || num_key <= 0)
    {
        output->OnError(
            "ERR at least 1 input key is needed for 'zdiff' command");
        return {false, ZDiffCommand()};
    }

    if (args.size() != 2 + static_cast<size_t>(num_key) &&
        args.size() != 3 + static_cast<size_t>(num_key))
    {
        output->OnError("ERR syntax error");
        return {false, ZDiffCommand()};
    }

    std::vector<EloqKey> vct_key;
    std::vector<ZRangeCommand> vct_cmd;
    vct_key.reserve(num_key);
    vct_cmd.reserve(num_key);

    bool opt_withscores = false;
    if (args.size() == 3 + static_cast<size_t>(num_key))
    {
        if (stringcomp(args[2 + num_key], "withscores", 1))
        {
            opt_withscores = true;
        }
        else
        {
            output->OnError("ERR syntax error");
            return {false, ZDiffCommand()};
        }
    }

    ZRangeConstraint zc(false,
                        0,
                        -1,
                        ZRangeType::ZRANGE_RANK,
                        ZRangeDirection::ZRANGE_DIRECTION_FORWARD);

    for (size_t idx = 2; idx < 2 + static_cast<size_t>(num_key); ++idx)
    {
        vct_key.emplace_back(args[idx]);
        ZRangeSpec spec;
        spec.opt_start_ = 0;
        spec.opt_end_ = -1;
        vct_cmd.emplace_back(zc, std::move(spec), true);
    }

    if (opt_withscores)
    {
        vct_cmd[0].constraint_.opt_withscores_ = true;
    }

    return {true, ZDiffCommand(std::move(vct_key), std::move(vct_cmd))};
}

std::tuple<bool, ZRangeStoreCommand> ParseZRangeStoreCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "zrangestore");
    if (args.size() < 5)
    {
        output->OnError(
            "ERR wrong number of arguments for 'zrangestore' command");
        return {false, ZRangeStoreCommand()};
    }

    std::vector<std::string_view> args_without_dst;
    args_without_dst.reserve(args.size() - 1);
    args_without_dst.push_back(args[0]);
    for (size_t i = 2; i < args.size(); ++i)
    {
        args_without_dst.push_back(args[i]);
    }

    ZRangeConstraint zc(false,
                        0,
                        -1,
                        ZRangeType::ZRANGE_RANK,
                        ZRangeDirection::ZRANGE_DIRECTION_FORWARD);
    ZRangeSpec spec;
    ZRangeConstraint constraint;

    auto [success, err_code] =
        ParseZrangeElement(args_without_dst, constraint, spec);
    constraint.opt_withscores_ = true;
    if (!success)
    {
        output->OnError(redis_get_error_messages(err_code));
        return {false, ZRangeStoreCommand()};
    }

    return {true,
            ZRangeStoreCommand(args[2], constraint, std::move(spec), args[1])};
}

std::tuple<bool, ZDiffStoreCommand> ParseZDiffStoreCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "zdiffstore");
    if (args.size() < 4)
    {
        output->OnError(
            "ERR wrong number of arguments for 'zdiffstore' command");
        return {false, ZDiffStoreCommand()};
    }

    int64_t num_key;
    if (!string2ll(args[2].data(), args[2].size(), num_key) || num_key <= 0)
    {
        output->OnError(
            "ERR at least 1 input key is needed for 'zdiffstore' command");
        return {false, ZDiffStoreCommand()};
    }

    if (args.size() != 3 + static_cast<size_t>(num_key))
    {
        output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
        return {false, ZDiffStoreCommand()};
    }

    std::vector<EloqKey> vct_key;
    std::vector<ZRangeCommand> vct_cmd;
    vct_key.reserve(num_key);
    vct_cmd.reserve(num_key);

    ZRangeConstraint zc(false,
                        0,
                        -1,
                        ZRangeType::ZRANGE_RANK,
                        ZRangeDirection::ZRANGE_DIRECTION_FORWARD);

    for (size_t idx = 3; idx < 3 + static_cast<size_t>(num_key); ++idx)
    {
        vct_key.emplace_back(args[idx]);
        ZRangeSpec spec;
        spec.opt_start_ = 0;
        spec.opt_end_ = -1;
        vct_cmd.emplace_back(zc, std::move(spec), true);
    }

    vct_cmd[0].constraint_.opt_withscores_ = true;
    return {true,
            ZDiffStoreCommand(std::move(vct_key), std::move(vct_cmd), args[1])};
}

std::tuple<bool, EloqKey, ZAddCommand> ParseZIncrByCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "zincrby");
    if (args.size() != 4)
    {
        output->OnError("ERR wrong number of arguments for 'zincrby' command");
        return {false, EloqKey(), ZAddCommand()};
    }

    ZParams zparams;
    zparams.flags = ZADD_IN_INCR;

    std::pair<double, EloqString> ele;

    if (args[2] == "+inf" || args[2] == "inf")
    {
        ele.first = std::numeric_limits<double>::infinity();
    }
    else if (args[2] == "-inf")
    {
        ele.first = std::numeric_limits<double>::infinity() * -1;
    }
    else
    {
        if (!string2double(args[2].begin(), args[2].size(), ele.first))
        {
            output->OnError("ERR value is not a valid float");
            return {false, EloqKey(), ZAddCommand()};
        }
    }

    ele.second = EloqString(args[3]);

    return {
        true,
        EloqKey(args[1]),
        ZAddCommand(std::move(ele), zparams, ZAddCommand::ElementType::pair)};
}

std::tuple<bool, ZInterCardCommand> ParseZInterCardCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "zintercard");

    if (args.size() < 3)
    {
        output->OnError(
            "ERR wrong number of arguments for 'zintercard' command");
        return {false, ZInterCardCommand()};
    }

    int64_t num_key;
    if (!string2ll(args[1].data(), args[1].size(), num_key) || num_key <= 0)
    {
        output->OnError(
            "ERR at least 1 input key is needed for 'zintercard' command");
        return {false, ZInterCardCommand()};
    }

    if (args.size() < 2 + (size_t) num_key)
    {
        output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
        return {false, ZInterCardCommand()};
    }

    int64_t limit = INT64_MAX;
    if (args.size() > 2 + (size_t) num_key)
    {
        if (args.size() != 4 + (size_t) num_key)
        {
            output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
            return {false, ZInterCardCommand()};
        }

        if (!stringcomp(args[args.size() - 2], "limit", 1))
        {
            output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
            return {false, ZInterCardCommand()};
        }

        if (!string2ll(args.back().data(), args.back().size(), limit) ||
            limit < 0)
        {
            output->OnError(redis_get_error_messages(RD_ERR_LIMIT_NEGATIVE));
            return {false, ZInterCardCommand()};
        }

        if (limit == 0)
        {
            limit = INT64_MAX;
        }
    }

    std::vector<EloqKey> vct_key;
    std::vector<SZScanCommand> vct_cmd;
    vct_key.reserve(num_key);
    vct_cmd.reserve(num_key);

    for (int64_t i = 2; i < num_key + 2; i++)
    {
        vct_key.emplace_back(args[i]);
        vct_cmd.emplace_back(false);
    }

    return {true,
            ZInterCardCommand(std::move(vct_key), std::move(vct_cmd), limit)};
}

std::pair<bool, MDelCommand> ParseDelCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "del");
    if (args.size() < 2)
    {
        output->OnError("ERR wrong number of arguments for 'del' command");
        return {false, MDelCommand()};
    }

    std::unordered_set<std::string_view> s;
    std::vector<EloqKey> vct_key;
    std::vector<DelCommand> vct_cmd;
    vct_key.reserve(args.size() - 1);
    vct_cmd.reserve(args.size() - 1);

    for (auto it = args.begin() + 1; it != args.end(); it++)
    {
        if (s.find(*it) == s.end())
        {
            s.insert(*it);
            vct_key.emplace_back(*it);
            vct_cmd.emplace_back();
        }
    }

    return {true, MDelCommand(std::move(vct_key), std::move(vct_cmd))};
}

std::pair<bool, MExistsCommand> ParseExistsCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "exists");
    if (args.size() < 2)
    {
        output->OnError("ERR wrong number of arguments for 'exists' command");
        return {false, MExistsCommand()};
    }

    std::vector<EloqKey> vct_key;
    std::vector<ExistsCommand> vct_cmd;
    vct_key.reserve(args.size() - 1);
    vct_cmd.reserve(args.size() - 1);

    // EXISTS command does not deduplicate repeated key.
    for (auto it = args.begin() + 1; it != args.end(); it++)
    {
        auto key_it = std::find(vct_key.begin(), vct_key.end(), *it);
        if (key_it == vct_key.end())
        {
            vct_key.emplace_back(*it);
            vct_cmd.emplace_back(1);
        }
        else
        {
            auto cmd_it = vct_cmd.begin() + (key_it - vct_key.begin());
            cmd_it->cnt_++;
        }
    }

    return {true, MExistsCommand(std::move(vct_key), std::move(vct_cmd))};
}

std::pair<bool, MSetCommand> ParseMSetCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "mset");
    if (args.size() < 3 || args.size() % 2 == 0)
    {
        output->OnError("ERR wrong number of arguments for 'mset' command");
        return {false, MSetCommand()};
    }

    std::vector<EloqKey> vct_key;
    std::vector<SetCommand> vct_cmd;
    size_t sz = (args.size() - 1) / 2;
    vct_key.reserve(sz);
    vct_cmd.reserve(sz);

    for (auto it = args.begin() + 1; it != args.end(); it += 2)
    {
        auto key_it = std::find(vct_key.begin(), vct_key.end(), *it);
        if (key_it == vct_key.end())
        {
            vct_key.emplace_back(*it);
            vct_cmd.emplace_back(*(it + 1));
        }
        else
        {
            auto cmd_it = vct_cmd.begin() + (key_it - vct_key.begin());
            assert(cmd_it != vct_cmd.end());
            *key_it = *it;
            *cmd_it = SetCommand(*(it + 1));
        }
    }
    return {true, MSetCommand(std::move(vct_key), std::move(vct_cmd))};
}

std::pair<bool, MSetNxCommand> ParseMSetNxCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "msetnx");
    if (args.size() < 3 || args.size() % 2 == 0)
    {
        output->OnError("ERR wrong number of arguments for 'msetnx' command");
        return {false, MSetNxCommand({}, {}, {})};
    }

    std::vector<EloqKey> keys;
    std::vector<ExistsCommand> exist_cmds;
    std::vector<SetCommand> set_cmds;

    size_t sz = (args.size() - 1) / 2;
    keys.reserve(sz);
    exist_cmds.reserve(sz);
    set_cmds.reserve(sz);

    for (auto it = args.begin() + 1; it != args.end(); it += 2)
    {
        auto key_it = std::find(keys.begin(), keys.end(), *it);
        if (key_it == keys.end())
        {
            keys.emplace_back(*it);
            exist_cmds.emplace_back(1);
            set_cmds.emplace_back(*(it + 1));
        }
        else
        {
            auto exist_it = exist_cmds.begin() + (key_it - keys.begin());
            exist_it->cnt_++;
            auto set_it = set_cmds.begin() + (key_it - keys.begin());
            *set_it = SetCommand(*(it + 1));
        }
    }

    return {true,
            MSetNxCommand(
                std::move(keys), std::move(exist_cmds), std::move(set_cmds))};
}

std::pair<bool, MGetCommand> ParseMGetCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "mget");
    if (args.size() < 2)
    {
        output->OnError("ERR wrong number of arguments for 'mget' command");
        return {false, MGetCommand()};
    }

    std::vector<EloqKey> vct_key;
    std::vector<GetCommand> vct_cmd;
    std::vector<GetCommand *> vct_raw_cmd;
    vct_key.reserve(args.size() - 1);
    vct_cmd.reserve(args.size() - 1);

    for (auto it = args.begin() + 1; it != args.end(); it++)
    {
        auto key_it = std::find(vct_key.begin(), vct_key.end(), *it);
        GetCommand *cmd_ptr = nullptr;
        if (key_it == vct_key.end())
        {
            vct_key.emplace_back(*it);
            vct_cmd.emplace_back();
            cmd_ptr = &vct_cmd.back();
        }
        else
        {
            auto cmd_it = vct_cmd.begin() + (key_it - vct_key.begin());
            cmd_ptr = &(*cmd_it);
        }
        vct_raw_cmd.emplace_back(cmd_ptr);
    }
    return {
        true,
        MGetCommand(
            std::move(vct_key), std::move(vct_cmd), std::move(vct_raw_cmd))};
}

StoreListCommand::StoreListCommand(const StoreListCommand &rhs)
{
    elements_.reserve(rhs.elements_.size());
    for (const EloqString &eloq_str : rhs.elements_)
    {
        elements_.emplace_back(eloq_str.Clone());
    }
}

std::unique_ptr<txservice::TxRecord> StoreListCommand::CreateObject(
    const std::string *image) const
{
    auto list_obj = std::make_unique<RedisListObject>();
    if (image)
    {
        size_t offset = 0;
        list_obj->Deserialize(image->data(), offset);
        assert(offset == image->size());
    }
    return list_obj;
}

txservice::ExecResult StoreListCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    result_.err_code_ = RD_OK;
    result_.int_val_ = elements_.size();
    // STORE command in SORT always overwrite the object with one exception:
    // nothing to store and the object was empty, in which case the STORE
    // command won't proceed to ExecuteOn.
    return txservice::ExecResult::Write;
}

txservice::TxObject *StoreListCommand::CommitOn(txservice::TxObject *obj_ptr)
{
    RedisListObject *robj = dynamic_cast<RedisListObject *>(obj_ptr);
    if (!elements_.empty())
    {
        if (!robj || !robj->Empty())
        {
            robj = new RedisListObject();
        }

        assert(robj->Empty());
        robj->CommitRPush(elements_);
    }
    else
    {
        robj = nullptr;
    }

    return robj;
}

void StoreListCommand::Serialize(std::string &str) const
{
    uint8_t cmd_type = static_cast<uint8_t>(RedisCommandType::STORE_LIST);
    str.append(reinterpret_cast<const char *>(&cmd_type), sizeof(cmd_type));

    uint32_t elements_cnt = elements_.size();
    str.append(reinterpret_cast<const char *>(&elements_cnt),
               sizeof(elements_cnt));
    for (const EloqString &element : elements_)
    {
        uint32_t element_len = element.Length();
        str.append(reinterpret_cast<const char *>(&element_len),
                   sizeof(element_len));
        str.append(element.Data(), element_len);
    }
}

void StoreListCommand::Deserialize(std::string_view cmd_image)
{
    const char *ptr = cmd_image.data();

    const uint32_t elements_cnt = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += sizeof(elements_cnt);
    elements_.reserve(elements_cnt);

    for (uint32_t i = 0; i < elements_cnt; i++)
    {
        const uint32_t element_len = *reinterpret_cast<const uint32_t *>(ptr);
        ptr += sizeof(element_len);
        elements_.emplace_back(ptr, element_len);
        ptr += element_len;
    }
}

void StoreListCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.err_code_ == RD_OK)
    {
        reply->OnInt(result_.int_val_);
    }
    else
    {
        assert(result_.err_code_ != RD_NIL);
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

txservice::ExecResult SortableLoadCommand::ExecuteOn(
    const txservice::TxObject &object)
{
    result_.err_code_ = RD_OK;

    const RedisEloqObject &eloq_obj =
        static_cast<const RedisEloqObject &>(object);
    switch (eloq_obj.ObjectType())
    {
    case RedisObjectType::List:
    {
        const RedisListObject &list_obj =
            static_cast<const RedisListObject &>(eloq_obj);
        list_obj.Execute(*this);
        break;
    }
    case RedisObjectType::Set:
    {
        const RedisHashSetObject &set_obj =
            static_cast<const RedisHashSetObject &>(eloq_obj);
        set_obj.Execute(*this);
        break;
    }
    case RedisObjectType::Zset:
    {
        const RedisZsetObject &zset_obj =
            static_cast<const RedisZsetObject &>(eloq_obj);
        zset_obj.Execute(*this);
        break;
    }
    default:
    {
        result_.err_code_ = RD_ERR_WRONG_TYPE;
        break;
    }
    }

    return result_.err_code_ == RD_OK ? txservice::ExecResult::Write
                                      : txservice::ExecResult::Fail;
}

void SortableLoadCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.err_code_ == RD_OK || result_.err_code_ == RD_NIL)
    {
        const std::vector<std::string> &elems = result_.elems_;
        reply->OnArrayStart(elems.size());
        for (const std::string &elem : elems)
        {
            reply->OnString(elem);
        }
        reply->OnArrayEnd();
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

SortCommand::SortCommand(
    const std::string_view &sort_key,
    int64_t offset,
    int64_t count,
    const std::optional<std::string_view> &by_pattern,
    const std::vector<std::string_view> &get_pattern_vec,
    bool desc,
    bool alpha,
    const std::optional<std::string_view> &store_destination)
    : sort_key_(sort_key),
      offset_(offset),
      count_(count),
      desc_(desc),
      alpha_(alpha),
      by_pattern_(by_pattern),
      store_destination_(store_destination)
{
    if (!get_pattern_vec.empty())
    {
        get_pattern_vec_.reserve(get_pattern_vec.size());
        for (const std::string_view &get_pattern : get_pattern_vec)
        {
            get_pattern_vec_.emplace_back(get_pattern);
        }
    }
}

void SortCommand::OutputResult(OutputHandler *reply) const
{
    if (result_.err_code_ == RD_OK || result_.err_code_ == RD_NIL)
    {
        if (store_destination_.has_value())
        {
            reply->OnInt(result_.result_.size());
        }
        else
        {
            reply->OnArrayStart(result_.result_.size());
            for (const auto &optional_str : result_.result_)
            {
                if (optional_str.has_value())
                {
                    reply->OnString(optional_str.value());
                }
                else
                {
                    reply->OnNil();
                }
            }
            reply->OnArrayEnd();
        }
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

std::function<bool(const SortCommand::SortObject &a,
                   const SortCommand::SortObject &b)>
SortCommand::LessFunc() const
{
    if (!desc_ && !alpha_)
    {
        return [](const SortObject &a, const SortObject &b)
        { return a.score_ != b.score_ ? a.score_ < b.score_ : a.id_ < b.id_; };
    }
    else if (desc_ && !alpha_)
    {
        return [](const SortObject &a, const SortObject &b)
        { return a.score_ != b.score_ ? a.score_ > b.score_ : a.id_ > b.id_; };
    }
    else if (!desc_ && alpha_)
    {
        return [](const SortObject &a, const SortObject &b) {
            return a.cmpkey_ != b.cmpkey_ ? a.cmpkey_ < b.cmpkey_
                                          : a.id_ < b.id_;
        };
    }
    else
    {
        return [](const SortObject &a, const SortObject &b) {
            return a.cmpkey_ != b.cmpkey_ ? a.cmpkey_ > b.cmpkey_
                                          : a.id_ > b.id_;
        };
    }
}

absl::Span<const SortCommand::SortObject> SortCommand::Limit(
    const std::vector<SortObject> &sort_vector) const
{
    int64_t sz = sort_vector.size();
    int64_t offset = offset_ < 0 ? 0 : offset_;
    int64_t end = count_ < 0 ? sz : offset + count_;
    offset = offset > sz ? sz : offset;
    end = end > sz ? sz : end;

    absl::Span<const SortObject> span =
        absl::MakeSpan(sort_vector.data() + offset, sort_vector.data() + end);
    return span;
}

bool SortCommand::Load(RedisServiceImpl *redis_impl,
                       txservice::TransactionExecution *txm,
                       OutputHandler *error,
                       const txservice::TableName *table_name,
                       const std::string_view &key,
                       RedisObjectType &obj_type,
                       std::vector<std::string> &values)
{
    EloqKey eloq_key(key);
    SortableLoadCommand cmd;
    ObjectCommandTxRequest req(table_name, &eloq_key, &cmd, false, true, txm);
    bool success = redis_impl->ExecuteTxRequest(txm, &req, nullptr, error);
    if (success)
    {
        if (cmd.result_.err_code_ == RD_OK)
        {
            obj_type = cmd.result_.obj_type_;
            values = std::move(cmd.result_.elems_);
        }
        else if (cmd.result_.err_code_ == RD_NIL)
        {
            obj_type = RedisObjectType::Unknown;
            assert(values.empty());
        }
        else
        {
            success = false;
            obj_type = RedisObjectType::Unknown;
            cmd.OutputResult(error);
        }
    }
    return success;
}

bool SortCommand::MultiGet(RedisServiceImpl *redis_impl,
                           txservice::TransactionExecution *txm,
                           OutputHandler *error,
                           const TableName *table_name,
                           const Pattern &pattern,
                           const std::vector<AccessKey> &access_keys,
                           std::vector<std::optional<std::string>> &values)
{
    bool success = false;

    values.reserve(access_keys.size());

    assert(!pattern.IsPoundSymbol() && pattern.ContainStarSymbol());
    if (pattern.ContainHashField())
    {
        std::vector<EloqKey> keys;
        std::vector<HGetCommand> cmds;
        keys.reserve(access_keys.size());
        cmds.reserve(access_keys.size());

        for (const AccessKey &access : access_keys)
        {
            keys.emplace_back(std::string_view(access.object_key_));
            cmds.emplace_back(std::string_view(access.field_key_.value()));
        }

        MHGetCommand mhget_cmd(std::move(keys), std::move(cmds));
        MultiObjectCommandTxRequest mhget_req(
            table_name, &mhget_cmd, false, true, txm);
        success = redis_impl->ExecuteMultiObjTxRequest(
            txm, &mhget_req, nullptr, error);
        if (success)
        {
            for (const HGetCommand &cmd : mhget_cmd.cmds_)
            {
                int32_t err_code = cmd.result_.err_code_;
                if (err_code == RD_OK)
                {
                    values.emplace_back(
                        std::get<std::string>(std::move(cmd.result_.result_)));
                }
                else
                {
                    assert(err_code == RD_NIL || err_code == RD_ERR_WRONG_TYPE);
                    values.emplace_back(std::nullopt);
                }
            }
        }
    }
    else
    {
        std::vector<EloqKey> keys;
        std::vector<GetCommand> cmds;
        keys.reserve(access_keys.size());
        cmds.reserve(access_keys.size());

        for (const AccessKey &access : access_keys)
        {
            keys.emplace_back(std::move(access.object_key_));
            cmds.emplace_back();
        }

        MGetCommand mget_cmd(std::move(keys), std::move(cmds));
        MultiObjectCommandTxRequest mget_req(
            table_name, &mget_cmd, false, true, txm);
        success = redis_impl->ExecuteMultiObjTxRequest(
            txm, &mget_req, nullptr, error);
        if (success)
        {
            for (const GetCommand &cmd : mget_cmd.cmds_)
            {
                int32_t err_code = cmd.result_.err_code_;
                if (err_code == RD_OK)
                {
                    values.emplace_back(std::move(cmd.result_.str_));
                }
                else
                {
                    assert(err_code == RD_NIL || err_code == RD_ERR_WRONG_TYPE);
                    values.emplace_back(std::nullopt);
                }
            }
        }
    }

    return success;
}

bool SortCommand::Store(RedisServiceImpl *redis_impl,
                        txservice::TransactionExecution *txm,
                        OutputHandler *error,
                        const txservice::TableName *table_name,
                        const std::string_view &key,
                        std::vector<EloqString> values)
{
    EloqKey eloq_key(key);
    StoreListCommand cmd(std::move(values));
    ObjectCommandTxRequest req(table_name, &eloq_key, &cmd, false, true, txm);
    bool success = redis_impl->ExecuteTxRequest(txm, &req, nullptr, error);
    if (success)
    {
        if (cmd.result_.err_code_ != RD_OK)
        {
            assert(cmd.result_.err_code_ != RD_NIL);
            cmd.OutputResult(error);
            success = false;
        }
    }
    return success;
}

void SortCommand::PrepareStringSortObjects(const std::vector<std::string> &ids,
                                           std::vector<SortObject> &sort_objs)
{
    size_t sz = ids.size();
    sort_objs.reserve(sz);

    for (size_t i = 0; i < sz; i++)
    {
        const std::string &id = ids[i];
        sort_objs.emplace_back(id, id);
    }
}

void SortCommand::PrepareStringSortObjects(
    const std::vector<std::string> &ids,
    const std::vector<std::optional<std::string>> &by_values,
    std::vector<SortObject> &sort_objs)
{
    size_t sz = ids.size();
    assert(sz == by_values.size());
    sort_objs.reserve(sz);

    assert(sz == by_values.size());
    for (size_t i = 0; i < sz; i++)
    {
        const std::string &id = ids[i];
        const std::string &by_value = by_values[i].value_or("");
        sort_objs.emplace_back(id, by_value);
    }
}

bool SortCommand::PrepareScoreSortObjects(const std::vector<std::string> &ids,
                                          std::vector<SortObject> &sort_objs,
                                          OutputHandler *error)
{
    bool success = true;
    size_t sz = ids.size();
    sort_objs.reserve(sz);

    for (size_t i = 0; i < sz; i++)
    {
        const std::string &id = ids[i];
        double score = 0;
        success = string2double(id.data(), id.size(), score);
        if (success)
        {
            sort_objs.emplace_back(id, score);
        }
        else
        {
            error->OnError(
                "ERR One or more scores can't be converted into double");
            break;
        }
    }
    return success;
}

bool SortCommand::PrepareScoreSortObjects(
    const std::vector<std::string> &ids,
    const std::vector<std::optional<std::string>> &by_values,
    std::vector<SortObject> &sort_objs,
    OutputHandler *error)
{
    bool success = true;
    size_t sz = ids.size();
    assert(sz == by_values.size());
    sort_objs.reserve(sz);

    for (size_t i = 0; i < sz; i++)
    {
        const std::string &id = ids[i];
        const std::string &by_value = by_values[i].value_or("0");
        double score = 0;
        success = string2double(by_value.data(), by_value.size(), score);
        if (success)
        {
            sort_objs.emplace_back(id, score);
        }
        else
        {
            error->OnError(
                "ERR One or more scores can't be converted into double");
            break;
        }
    }
    return success;
}

SortCommand::Pattern::Pattern(const std::string_view &pattern)
{
    assert(!pattern.empty());
    if (pattern.size() == 1 && pattern.front() == '#')
    {
        is_pound_symbol_ = true;
    }
    else
    {
        size_t pos_star = pattern.find_first_of('*');
        if (pos_star != std::string::npos)
        {
            contain_star_symbol_ = true;

            key_prefix_ = std::string_view(pattern.data(), pos_star);

            size_t pos_pointer = pattern.find("->", pos_star + 1);
            if (pos_pointer != std::string_view::npos &&
                pos_pointer + 2 < pattern.size())
            {
                key_postfix_ = std::string_view(pattern.data() + pos_star + 1,
                                                pos_pointer - pos_star - 1);
                hash_field_.emplace(pattern.data() + pos_pointer + 2,
                                    pattern.size() - pos_pointer - 2);
            }
            else
            {
                key_postfix_ = std::string_view(pattern.data() + pos_star + 1,
                                                pattern.size() - pos_star - 1);
            }
        }
    }
}

SortCommand::AccessKey SortCommand::Pattern::Pattern::MakeAccessKey(
    const std::string_view &arg) const
{
    AccessKey key;
    key.object_key_.append(key_prefix_);
    key.object_key_.append(arg);
    key.object_key_.append(key_postfix_);
    if (hash_field_.has_value())
    {
        key.field_key_.emplace(hash_field_.value());
    }
    return key;
}

std::pair<bool, MWatchCommand> ParseWatchCommand(
    const std::vector<std::string> &args, OutputHandler *output)
{
    assert(args[0] == "watch");
    if (args.size() < 2)
    {
        output->OnError("ERR wrong number of arguments for 'watch' command");
        return {false, MWatchCommand()};
    }

    std::vector<EloqKey> vct_key;
    std::vector<WatchCommand> vct_cmd;
    vct_key.reserve(args.size() - 1);
    vct_cmd.reserve(args.size() - 1);

    for (auto it = args.begin() + 1; it != args.end(); it++)
    {
        vct_key.emplace_back(*it);
        vct_cmd.emplace_back();
    }
    return {true, MWatchCommand(std::move(vct_key), std::move(vct_cmd))};
}

std::tuple<bool, EloqKey, SAddCommand> ParseSAddCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "sadd");
    if (args.size() < 3)
    {
        output->OnError("ERR wrong number of arguments for 'sadd' command");
        return {false, EloqKey(), SAddCommand()};
    }

    std::vector<EloqString> set_elements;
    set_elements.reserve(args.size() - 2);
    std::unordered_set<std::string_view> set(args.size() - 2);

    for (size_t i = 2; i < args.size(); i++)
    {
        if (!set.insert(args[i]).second)
        {
            continue;
        }

        set_elements.emplace_back(args[i]);
    }

    return {true, EloqKey(args[1]), SAddCommand(std::move(set_elements))};
}

std::tuple<bool, EloqKey, SMembersCommand> ParseSMembersCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "smembers");
    if (args.size() != 2)
    {
        output->OnError("ERR wrong number of arguments for 'smerbers' command");
        return {false, EloqKey(), SMembersCommand()};
    }

    return {true, EloqKey(args[1]), SMembersCommand()};
}

std::tuple<bool, EloqKey, SRemCommand> ParseSRemCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "srem");
    if (args.size() < 3)
    {
        output->OnError("ERR wrong number of arguments for 'sadd' command");
        return {false, EloqKey(), SRemCommand()};
    }

    std::vector<EloqString> set_elements;
    set_elements.reserve(args.size() - 2);
    std::unordered_set<std::string_view> set(args.size() - 2);

    for (size_t i = 2; i < args.size(); i++)
    {
        if (!set.insert(args[i]).second)
        {
            continue;
        }

        set_elements.emplace_back(args[i]);
    }

    return {true, EloqKey(args[1]), SRemCommand(std::move(set_elements))};
}

std::tuple<bool, EloqKey, SCardCommand> ParseSCardCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "scard");
    if (args.size() != 2)
    {
        output->OnError("ERR wrong number of arguments for 'scard' command");
        return {false, EloqKey(), SCardCommand()};
    }

    return {true, EloqKey(args[1]), SCardCommand()};
}

std::tuple<bool, SDiffCommand> ParseSDiffCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "sdiff");
    if (args.size() < 2)
    {
        output->OnError("ERR wrong number of arguments for 'sdiff' command");
        return {false, SDiffCommand()};
    }

    std::vector<EloqKey> vct_key;
    std::vector<SMembersCommand> vct_cmd;
    size_t sz = args.size() - 1;
    vct_key.reserve(sz);
    vct_cmd.reserve(sz);

    for (auto it = args.begin() + 1; it != args.end(); it++)
    {
        vct_key.emplace_back(*it);
        vct_cmd.emplace_back();
    }

    return {true, SDiffCommand(std::move(vct_key), std::move(vct_cmd))};
}

std::tuple<bool, SDiffStoreCommand> ParseSDiffStoreCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "sdiffstore");
    if (args.size() < 3)
    {
        output->OnError(
            "ERR wrong number of arguments for 'sdiffstore' command");
        return {false, SDiffStoreCommand()};
    }

    std::vector<EloqKey> vct_key;
    std::vector<SMembersCommand> vct_cmd;
    size_t sz = args.size() - 2;
    vct_key.reserve(sz);
    vct_cmd.reserve(sz);

    for (auto it = args.begin() + 2; it != args.end(); it++)
    {
        vct_key.emplace_back(*it);
        vct_cmd.emplace_back();
    }

    return {true,
            SDiffStoreCommand(std::move(vct_key), std::move(vct_cmd), args[1])};
}

std::tuple<bool, SInterCommand> ParseSInterCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "sinter");
    if (args.size() < 2)
    {
        output->OnError("ERR wrong number of arguments for 'sinter' command");
        return {false, SInterCommand()};
    }

    std::vector<EloqKey> vct_key;
    std::vector<SMembersCommand> vct_cmd;
    size_t sz = args.size() - 1;
    vct_key.reserve(sz);
    vct_cmd.reserve(sz);

    for (auto it = args.begin() + 1; it != args.end(); it++)
    {
        vct_key.emplace_back(*it);
        vct_cmd.emplace_back();
    }

    return {true, SInterCommand(std::move(vct_key), std::move(vct_cmd))};
}

std::tuple<bool, SInterStoreCommand> ParseSInterStoreCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "sinterstore");
    if (args.size() < 3)
    {
        output->OnError(
            "ERR wrong number of arguments for 'sinterstore' command");
        return {false, SInterStoreCommand()};
    }

    std::vector<EloqKey> vct_key;
    std::vector<SMembersCommand> vct_cmd;
    size_t sz = args.size() - 2;
    vct_key.reserve(sz);
    vct_cmd.reserve(sz);

    for (auto it = args.begin() + 2; it != args.end(); it++)
    {
        vct_key.emplace_back(*it);
        vct_cmd.emplace_back();
    }

    return {
        true,
        SInterStoreCommand(std::move(vct_key), std::move(vct_cmd), args[1])};
}

std::tuple<bool, SInterCardCommand> ParseSInterCardCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "sintercard");
    if (args.size() < 3)
    {
        output->OnError(
            "ERR wrong number of arguments for 'sintercard' command");
        return {false, SInterCardCommand()};
    }

    int64_t num;
    if (!string2ll(args[1].data(), args[1].size(), num) || num < 1)
    {
        output->OnError(redis_get_error_messages(RD_ERR_NUMBER_GREATER_ZERO));
        return {false, SInterCardCommand()};
    }

    if (static_cast<size_t>(num) > args.size() - 2)
    {
        output->OnError(redis_get_error_messages(RD_ERR_NUMBER_GREATER_ARGS));
        return {false, SInterCardCommand()};
    }

    if (static_cast<size_t>(num) != args.size() - 2 &&
        (static_cast<size_t>(num) != args.size() - 4 ||
         !stringcomp(args[args.size() - 2], "limit", 1)))
    {
        output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
        return {false, SInterCardCommand()};
    }

    int64_t limit = INT64_MAX;
    if (static_cast<size_t>(num) == args.size() - 4)
    {
        const auto &sv = args[args.size() - 1];
        if (!string2ll(sv.data(), sv.size(), limit) || limit < 0)
        {
            output->OnError(redis_get_error_messages(RD_ERR_LIMIT_NEGATIVE));
            return {false, SInterCardCommand()};
        }

        if (limit == 0)
        {
            limit = INT64_MAX;
        }
    }

    std::vector<EloqKey> vct_key;
    std::vector<SMembersCommand> vct_cmd;
    size_t sz = num;
    vct_key.reserve(sz);
    vct_cmd.reserve(sz);

    for (int64_t i = 2; i < num + 2; i++)
    {
        vct_key.emplace_back(args[i]);
        vct_cmd.emplace_back();
    }

    return {true,
            SInterCardCommand(std::move(vct_key), std::move(vct_cmd), limit)};
}

std::tuple<bool, EloqKey, SIsMemberCommand> ParseSIsMemberCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "sismember");
    if (args.size() != 3)
    {
        output->OnError(
            "ERR wrong number of arguments for 'sismember' command");
        return {false, EloqKey(), SIsMemberCommand()};
    }

    return {true, EloqKey(args[1]), SIsMemberCommand(args[2])};
}

std::tuple<bool, EloqKey, SMIsMemberCommand> ParseSMIsMemberCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "smismember");
    if (args.size() < 3)
    {
        output->OnError(
            "ERR wrong number of arguments for 'smismember' command");
        return {false, EloqKey(), SMIsMemberCommand()};
    }

    std::vector<EloqString> mems;
    mems.reserve(args.size() - 2);
    for (auto sv_it = args.begin() + 2; sv_it != args.end(); sv_it++)
    {
        mems.emplace_back(*sv_it);
    }

    return {true, EloqKey(args[1]), SMIsMemberCommand(std::move(mems))};
}

std::tuple<bool, SMoveCommand> ParseSMoveCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "smove");
    if (args.size() != 4)
    {
        output->OnError("ERR wrong number of arguments for 'smove' command");
        return {false, SMoveCommand()};
    }

    std::vector<EloqString> mem;
    mem.emplace_back(args[3]);

    return {true,
            SMoveCommand(EloqKey(args[1]),
                         SRemCommand(std::move(mem)),
                         EloqKey(args[2]),
                         SAddCommand(std::vector<EloqString>(), false))};
}

std::tuple<bool, LMoveCommand> ParseLMoveCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "lmove");
    if (args.size() != 5)
    {
        output->OnError("ERR wrong number of arguments for 'lmove' command");
        return {false, LMoveCommand()};
    }

    bool is_left_for_src;
    if (stringcomp("left", args[3], 1))
    {
        is_left_for_src = true;
    }
    else if (stringcomp("right", args[3], 1))
    {
        is_left_for_src = false;
    }
    else
    {
        output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
        return {false, LMoveCommand()};
    }

    bool is_left_for_dst;
    if (stringcomp("left", args[4], 1))
    {
        is_left_for_dst = true;
    }
    else if (stringcomp("right", args[4], 1))
    {
        is_left_for_dst = false;
    }
    else
    {
        output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
        return {false, LMoveCommand()};
    }

    return {true,
            LMoveCommand(args[1], is_left_for_src, args[2], is_left_for_dst)};
}

std::tuple<bool, RPopLPushCommand> ParseRPopLPushCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "rpoplpush");
    if (args.size() != 3)
    {
        output->OnError(
            "ERR wrong number of arguments for 'rpoplpush' command");
        return {false, RPopLPushCommand()};
    }

    return {true, RPopLPushCommand(args[1], args[2])};
}

std::tuple<bool, SUnionCommand> ParseSUnionCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "sunion");
    if (args.size() < 2)
    {
        output->OnError("ERR wrong number of arguments for 'sunion' command");
        return {false, SUnionCommand()};
    }

    std::vector<EloqKey> vct_key;
    std::vector<SMembersCommand> vct_cmd;
    size_t sz = args.size();
    vct_key.reserve(sz - 1);
    vct_cmd.reserve(sz - 1);

    for (size_t i = 1; i < sz; i++)
    {
        vct_key.emplace_back(args[i]);
        vct_cmd.emplace_back();
    }

    return {true, SUnionCommand(std::move(vct_key), std::move(vct_cmd))};
}

std::tuple<bool, SUnionStoreCommand> ParseSUnionStoreCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "sunionstore");
    if (args.size() < 3)
    {
        output->OnError(
            "ERR wrong number of arguments for 'sunionstore' command");
        return {false, SUnionStoreCommand()};
    }

    std::vector<EloqKey> vct_key;
    std::vector<SMembersCommand> vct_cmd;
    size_t sz = args.size();
    vct_key.reserve(sz - 2);
    vct_cmd.reserve(sz - 2);

    for (size_t i = 2; i < sz; i++)
    {
        vct_key.emplace_back(args[i]);
        vct_cmd.emplace_back();
    }

    return {
        true,
        SUnionStoreCommand(std::move(vct_key), std::move(vct_cmd), args[1])};
}

std::tuple<bool, EloqKey, SRandMemberCommand> ParseSRandMemberCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "srandmember");
    if (args.size() != 2 && args.size() != 3)
    {
        output->OnError(
            "ERR wrong number of arguments for 'srandmember' command");
        return {false, EloqKey(), SRandMemberCommand()};
    }

    bool count_provided = false;
    int64_t count = 1;
    if (args.size() == 3)
    {
        if (string2ll(args[2].data(), args[2].size(), count))
        {
            count_provided = true;
        }
        else
        {
            output->OnError(
                redis_get_error_messages(RD_ERR_NUMBER_GREATER_ZERO));
            return {false, EloqKey(), SRandMemberCommand()};
        }
    }

    if (count <= LLONG_MIN || count >= LLONG_MAX)
    {
        output->OnError(redis_get_error_messages(RD_ERR_VALUE_OUT_OF_RANGE));
        return {false, EloqKey(), SRandMemberCommand()};
    }

    return {true, EloqKey(args[1]), SRandMemberCommand(count, count_provided)};
}

std::tuple<bool, EloqKey, SPopCommand> ParseSPopCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "spop");
    if (args.size() != 2 && args.size() != 3)
    {
        output->OnError("ERR wrong number of arguments for 'spop' command");
        return {false, EloqKey(), SPopCommand()};
    }

    int64_t count = -1;
    if (args.size() == 2)
    {
        return {true, EloqKey(args[1]), SPopCommand(count)};
    }

    if (args.size() == 3)
    {
        if (!string2ll(args[2].data(), args[2].size(), count) || count < 0)
        {
            output->OnError(redis_get_error_messages(RD_ERR_POSITIVE_INVALID));
            return {false, EloqKey(), SPopCommand()};
        }
    }

    if (count >= LLONG_MAX)
    {
        output->OnError(redis_get_error_messages(RD_ERR_VALUE_OUT_OF_RANGE));
        return {false, EloqKey(), SPopCommand()};
    }

    return {true, EloqKey(args[1]), SPopCommand(count)};
}

std::tuple<bool, EloqKey, SScanCommand> ParseSScanCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "sscan");
    if (args.size() < 3)
    {
        output->OnError("ERR wrong number of arguments for 'sscan' command");
        return {false, EloqKey(), SScanCommand()};
    }

    int64_t cursor = 0;
    bool match = false;
    std::string_view pattern;
    int64_t count = 0;

    if (!string2ll(args[2].data(), args[2].size(), cursor))
    {
        output->OnError(redis_get_error_messages(RD_ERR_INVALID_CURSOR));
        return {false, EloqKey(), SScanCommand()};
    }
    if (cursor < 0)
    {
        output->OnError(redis_get_error_messages(RD_ERR_WRONG_TYPE));
        return {false, EloqKey(), SScanCommand()};
    }

    size_t pos = 3;
    if (IsOdd(args.size() - pos))
    {
        output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
        return {false, EloqKey(), SScanCommand()};
    }
    while (pos < args.size())
    {
        if (stringcomp("match", args[pos], 1))
        {
            match = true;
            pattern = args[pos + 1];
        }
        else if (stringcomp("count", args[pos], 1))
        {
            if (!string2ll(args[pos + 1].data(), args[pos + 1].size(), count))
            {
                output->OnError(
                    redis_get_error_messages(RD_ERR_DIGITAL_INVALID));
                return {false, EloqKey(), SScanCommand()};
            }
            if (count < 1)
            {
                output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
                return {false, EloqKey(), SScanCommand()};
            }
        }
        else
        {
            output->OnError(redis_get_error_messages(RD_ERR_WRONG_TYPE));
            return {false, EloqKey(), SScanCommand()};
        }

        pos += 2;
    }

    return {
        true, EloqKey(args[1]), SScanCommand(cursor, match, pattern, count)};
}

std::tuple<bool, EloqKey, HDelCommand> ParseHDelCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "hdel");
    if (args.size() < 3)
    {
        output->OnError("ERR wrong number of arguments for 'hdel' command");
        return {false, EloqKey(), HDelCommand()};
    }
    std::vector<EloqString> elements;
    elements.reserve(args.size() - 2);
    for (auto sv_it = args.begin() + 2; sv_it != args.end(); sv_it++)
    {
        elements.emplace_back(*sv_it);
    }

    // Remove duplicate elements
    elements.erase(std::unique(elements.begin(), elements.end()),
                   elements.end());

    return {true, EloqKey(args[1]), HDelCommand(std::move(elements))};
}

std::tuple<bool, EloqKey, HExistsCommand> ParseHExistsCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "hexists");
    if (args.size() != 3)
    {
        output->OnError("ERR wrong number of arguments for 'hexists' command");
        return {false, EloqKey(), HExistsCommand()};
    }
    return {true, EloqKey(args[1]), HExistsCommand(args[2])};
}

std::tuple<bool, EloqKey, HMGetCommand> ParseHMGetCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "hmget");
    if (args.size() < 3)
    {
        output->OnError("ERR wrong number of arguments for 'hmget' command");
        return {false, EloqKey(), HMGetCommand()};
    }

    std::vector<EloqString> hmget_command_fields;
    hmget_command_fields.reserve(args.size() - 2);

    for (size_t i = 2; i < args.size(); i++)
    {
        hmget_command_fields.emplace_back(args[i]);
    }

    return {
        true, EloqKey(args[1]), HMGetCommand(std::move(hmget_command_fields))};
}

std::tuple<bool, EloqKey, HKeysCommand> ParseHKeysCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "hkeys");
    if (args.size() != 2)
    {
        output->OnError("ERR wrong number of arguments for 'hkeys' command");
        return {false, EloqKey(), HKeysCommand()};
    }

    return {true, EloqKey(args[1]), HKeysCommand()};
}

std::tuple<bool, EloqKey, HValsCommand> ParseHValsCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "hvals");
    if (args.size() != 2)
    {
        output->OnError("ERR wrong number of arguments for 'hvals' command");
        return {false, EloqKey(), HValsCommand()};
    }

    return {true, EloqKey(args[1]), HValsCommand()};
}

std::tuple<bool, EloqKey, HGetAllCommand> ParseHGetAllCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "hgetall");
    if (args.size() != 2)
    {
        output->OnError("ERR wrong number of arguments for 'hgetall' command");
        return {false, EloqKey(), HGetAllCommand()};
    }

    return {true, EloqKey(args[1]), HGetAllCommand()};
}

std::tuple<bool, EloqKey, HSetNxCommand> ParseHSetNxCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "hsetnx");
    if (args.size() != 4)
    {
        output->OnError("ERR wrong number of arguments for 'hsetnx' command");
        return {false, EloqKey(), HSetNxCommand()};
    }

    return {true, EloqKey(args[1]), HSetNxCommand(args[2], args[3])};
}

std::tuple<bool, EloqKey, HRandFieldCommand> ParseHRandFieldCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "hrandfield");
    if (args.size() < 2)
    {
        output->OnError(
            "ERR wrong number of arguments for 'hrandfield' command");
        return {false, EloqKey(), HRandFieldCommand()};
    }
    else if (args.size() == 2)
    {
        return {true, EloqKey(args[1]), HRandFieldCommand()};
    }
    else
    {
        int64_t count = 0;
        if (!string2ll(args[2].data(), args[2].size(), count))
        {
            output->OnError(redis_get_error_messages(RD_ERR_DIGITAL_INVALID));
            return {false, EloqKey(), HRandFieldCommand()};
        }

        if (args.size() > 4 ||
            (args.size() == 4 && strcasecmp(args[3].data(), "withvalues")))
        {
            output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
            return {false, EloqKey(), HRandFieldCommand()};
        }

        bool with_values = args.size() == 4;

        if (!with_values && (count <= LLONG_MIN || count >= LLONG_MAX))
        {
            output->OnError(
                redis_get_error_messages(RD_ERR_VALUE_OUT_OF_RANGE));
            return {false, EloqKey(), HRandFieldCommand()};
        }
        else if (with_values &&
                 (count <= LLONG_MIN / 2 || count > (LLONG_MAX - 1) / 2))
        {
            output->OnError(
                redis_get_error_messages(RD_ERR_VALUE_OUT_OF_RANGE));
            return {false, EloqKey(), HRandFieldCommand()};
        }

        return {true, EloqKey(args[1]), HRandFieldCommand(count, with_values)};
    }
    return {false, EloqKey(), HRandFieldCommand()};
}

std::tuple<bool, EloqKey, HScanCommand> ParseHScanCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "hscan");
    if (args.size() < 3)
    {
        output->OnError("ERR wrong number of arguments for 'hscan' command");
        return {false, EloqKey(), HScanCommand()};
    }

    int64_t cursor = 0;
    if (!string2ll(args[2].data(), args[2].size(), cursor))
    {
        output->OnError(redis_get_error_messages(RD_ERR_INVALID_CURSOR));
        return {false, EloqKey(), HScanCommand()};
    }
    if (cursor < 0)
    {
        output->OnError(
            "WRONGTYPE Operation against a key holding the wrong kind of "
            "value");
        return {false, EloqKey(), HScanCommand()};
    }

    size_t args_index = 3;

    int64_t count = 0;
    bool match = false;
    EloqString pattern;
    bool novalues = false;

    // if (IsOdd(args.size() - args_index) ||
    //     !stringcomp(args.back(), "novalues", 1))
    // {
    //     output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
    //     return {false, EloqKey(), HScanCommand()};
    // }

    // According to the redis command implementation, the query is
    // determined by the last time input parameter
    while (args_index < args.size())
    {
        if (stringcomp(args[args_index], "match", 1))
        {
            match = true;
            pattern = EloqString(args[args_index + 1]);
            args_index += 2;
        }
        else if (stringcomp(args[args_index], "count", 1))
        {
            if (!string2ll(args[args_index + 1].data(),
                           args[args_index + 1].size(),
                           count))
            {
                output->OnError(
                    redis_get_error_messages(RD_ERR_DIGITAL_INVALID));
                return {false, EloqKey(), HScanCommand()};
            }
            args_index += 2;
        }
        else if (stringcomp(args[args_index], "novalues", 1))
        {
            novalues = true;
            args_index += 1;
        }
        else
        {
            output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
            return {false, EloqKey(), HScanCommand()};
        }
    }

    return {true,
            EloqKey(args[1]),
            HScanCommand(cursor, match, std::move(pattern), count, novalues)};
}

std::tuple<bool, EloqKey, HIncrByCommand> ParseHIncrByCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "hincrby");
    if (args.size() != 4)
    {
        output->OnError("ERR wrong number of arguments for 'hincrby' command");
        return {false, EloqKey(), HIncrByCommand()};
    }

    int64_t score;
    if (!EloqKV::string2ll(args[3].data(), args[3].size(), score))
    {
        output->OnError(redis_get_error_messages(RD_ERR_DIGITAL_INVALID));
        return {false, EloqKey(), HIncrByCommand()};
    }

    return {true, EloqKey(args[1]), HIncrByCommand(args[2], score)};
}

std::tuple<bool, EloqKey, HIncrByFloatCommand> ParseHIncrByFloatCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "hincrbyfloat");
    if (args.size() != 4)
    {
        output->OnError(
            "ERR wrong number of arguments for 'hincrbyfloat' command");
        return {false, EloqKey(), HIncrByFloatCommand()};
    }

    long double val;
    if (!string2ld(args[3].data(), args[3].size(), val))
    {
        output->OnError(redis_get_error_messages(RD_ERR_FLOAT_VALUE));
        return {false, EloqKey(), HIncrByFloatCommand()};
    }

    return {true, EloqKey(args[1]), HIncrByFloatCommand(args[2], val)};
}

std::tuple<bool, EloqKey, HLenCommand> ParseHLenCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "hlen");
    if (args.size() != 2ul)
    {
        output->OnError("ERR wrong number of arguments for 'hlen' command");
        return {false, EloqKey(), HLenCommand()};
    }
    return {true, EloqKey(args[1]), HLenCommand()};
}

std::tuple<bool, EloqKey, HStrLenCommand> ParseHStrLenCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "hstrlen");
    if (args.size() != 3ul)
    {
        output->OnError("ERR wrong number of arguments for 'hstrlen' command");
        return {false, EloqKey(), HStrLenCommand()};
    }

    const std::string_view &field = args[2];
    return {true, EloqKey(args[1]), HStrLenCommand(field)};
}

std::tuple<bool, EloqKey, LLenCommand> ParseLLenCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "llen");
    if (args.size() != 2ul)
    {
        output->OnError("ERR wrong number of arguments for 'llen' command");
        return {false, EloqKey(), LLenCommand()};
    }
    return {true, EloqKey(args[1]), LLenCommand()};
}

std::tuple<bool, EloqKey, LTrimCommand> ParseLTrimCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "ltrim");
    if (args.size() != 4ul)
    {
        output->OnError("ERR wrong number of arguments for 'ltrim' command");
        return {false, EloqKey(), LTrimCommand()};
    }

    int64_t indexes[2];
    if (!string2ll(args[2].data(), args[2].size(), indexes[0]) ||
        !string2ll(args[3].data(), args[3].size(), indexes[1]))
    {
        output->OnError(redis_get_error_messages(RD_ERR_DIGITAL_INVALID));
        return {false, EloqKey(), LTrimCommand()};
    }

    return {true, args[1], LTrimCommand(indexes[0], indexes[1])};
}

std::tuple<bool, EloqKey, LIndexCommand> ParseLIndexCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "lindex");
    if (args.size() != 3ul)
    {
        output->OnError("ERR wrong number of arguments for 'lindex' command");
        return {false, EloqKey(), LIndexCommand()};
    }

    int64_t index;
    if (!string2ll(args[2].data(), args[2].size(), index))
    {
        output->OnError(redis_get_error_messages(RD_ERR_DIGITAL_INVALID));
        return {false, EloqKey(), LIndexCommand()};
    }

    return {true, args[1], LIndexCommand(index)};
}

std::tuple<bool, EloqKey, LInsertCommand> ParseLInsertCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "linsert");
    if (args.size() != 5)
    {
        output->OnError("ERR wrong number of arguments for 'linsert' command");
        return {false, EloqKey(), LInsertCommand()};
    }

    bool is_before;

    if (stringcomp("before", args[2], 1))
    {
        is_before = true;
    }
    else if (stringcomp("after", args[2], 1))
    {
        is_before = false;
    }
    else
    {
        output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
        return {false, EloqKey(), LInsertCommand()};
    }

    return {true, args[1], LInsertCommand(is_before, args[3], args[4])};
}

std::tuple<bool, EloqKey, LPosCommand> ParseLPosCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "lpos");
    if (args.size() < 3 || args.size() > 9)
    {
        output->OnError("ERR wrong number of arguments for 'lpos' command");
        return {false, EloqKey(), LPosCommand()};
    }

    int64_t rank = 1;
    int64_t count = -1;
    uint64_t len = 0;

    for (size_t i = 3; i < args.size(); i += 2)
    {
        // A flag is found and there is an argument after this flag.
        if (stringcomp(args[i], "rank", 1) && i + 1 < args.size())
        {
            if (!string2ll(args[i + 1].data(), args[i + 1].size(), rank))
            {
                output->OnError(
                    redis_get_error_messages(RD_ERR_DIGITAL_INVALID));
                return {false, EloqKey(), LPosCommand()};
            }

            if (rank == 0)
            {
                output->OnError(redis_get_error_messages(RD_ERR_ZERO_RANK));
                return {false, EloqKey(), LPosCommand()};
            }
            else if (rank <= LLONG_MIN || rank >= LLONG_MAX)
            {
                // RANK does not accept LLONG_MIN as its value
                output->OnError(
                    redis_get_error_messages(RD_ERR_VALUE_OUT_OF_RANGE));
                return {false, EloqKey(), LPosCommand()};
            }
        }
        else if (stringcomp(args[i], "count", 1) && i + 1 < args.size())
        {
            if (!string2ll(args[i + 1].data(), args[i + 1].size(), count))
            {
                output->OnError(
                    "ERR value is not an unsigned integer or out of range");
                return {false, EloqKey(), LPosCommand()};
            }
        }
        else if (stringcomp(args[i], "maxlen", 1) && i + 1 < args.size())
        {
            if (!string2ull(args[i + 1].data(), args[i + 1].size(), len))
            {
                output->OnError(
                    "ERR value is not an unsigned integer or out of range");
                return {false, EloqKey(), LPosCommand()};
            }
        }
        else
        {
            output->OnError("ERR syntax error");
            return {false, EloqKey(), LPosCommand()};
        }
    }

    return {true, args[1], LPosCommand(args[2], rank, count, len)};
}

std::tuple<bool, EloqKey, LSetCommand> ParseLSetCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "lset");
    if (args.size() != 4)
    {
        output->OnError("ERR wrong number of arguments for 'lset' command");
        return {false, EloqKey(), LSetCommand()};
    }

    int64_t index;
    if (!string2ll(args[2].data(), args[2].size(), index))
    {
        output->OnError(redis_get_error_messages(RD_ERR_DIGITAL_INVALID));
        return {false, EloqKey(), LSetCommand()};
    }

    return {true, args[1], LSetCommand(index, args[3])};
}

std::tuple<bool, EloqKey, LRemCommand> ParseLRemCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "lrem");
    if (args.size() != 4)
    {
        output->OnError("ERR wrong number of arguments for 'lrem' command");
        return {false, EloqKey(), LRemCommand()};
    }

    int64_t count;
    if (!string2ll(args[2].data(), args[2].size(), count))
    {
        output->OnError("ERR count is not an integer or out of range");
        return {false, EloqKey(), LRemCommand()};
    }

    return {true, args[1], LRemCommand(count, args[3])};
}

std::tuple<bool, EloqKey, LPushXCommand> ParseLPushXCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "lpushx");
    if (args.size() < 3)
    {
        output->OnError("ERR wrong number of arguments for 'lpushx' command");
        return {false, EloqKey(), LPushXCommand()};
    }

    std::vector<EloqString> elements;
    elements.reserve(args.size() - 2);
    for (auto arg_it = args.begin() + 2; arg_it != args.end(); ++arg_it)
    {
        elements.emplace_back(*arg_it);
    }

    return {true, args[1], LPushXCommand(std::move(elements))};
}

std::tuple<bool, EloqKey, RPushXCommand> ParseRPushXCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "rpushx");
    if (args.size() < 3)
    {
        output->OnError("ERR wrong number of arguments for 'rpushx' command");
        return {false, EloqKey(), RPushXCommand()};
    }

    std::vector<EloqString> elements;
    elements.reserve(args.size() - 2);
    for (auto arg_it = args.begin() + 2; arg_it != args.end(); ++arg_it)
    {
        elements.emplace_back(*arg_it);
    }

    return {true, args[1], RPushXCommand(std::move(elements))};
}

std::tuple<bool, SortCommand> ParseSortCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "sort" || args[0] == "sort_ro");

    const std::string_view &key = args[1];
    std::optional<std::string_view> store_key;

    bool desc = false, alpha = false;
    int64_t limit_start = 0, limit_count = -1;
    bool syntax_error = false;
    std::optional<std::string_view> by_pattern;
    std::vector<std::string_view> get_pattern_vec;

    for (size_t j = 2; j < args.size(); j++)
    {
        size_t leftargs = args.size() - j - 1;
        if (!strcasecmp(args[j].data(), "asc"))
        {
            desc = false;
        }
        else if (!strcasecmp(args[j].data(), "desc"))
        {
            desc = true;
        }
        else if (!strcasecmp(args[j].data(), "alpha"))
        {
            alpha = true;
        }
        else if (!strcasecmp(args[j].data(), "limit") && leftargs >= 2)
        {
            if (!string2ll(args[j + 1].data(), args[j + 1].size(), limit_start))
            {
                syntax_error = true;
                break;
            }
            if (!string2ll(args[j + 2].data(), args[j + 2].size(), limit_count))
            {
                syntax_error = true;
                break;
            }
            j += 2;
        }
        else if (args[0] == "sort" && !strcasecmp(args[j].data(), "store") &&
                 leftargs >= 1)
        {
            store_key.emplace(args[j + 1]);
            j++;
        }
        else if (!strcasecmp(args[j].data(), "by") && leftargs >= 1)
        {
            by_pattern.emplace(args[j + 1]);
            j++;
        }
        else if (!strcasecmp(args[j].data(), "get") && leftargs >= 1)
        {
            get_pattern_vec.emplace_back(args[j + 1]);
            j++;
        }
        else
        {
            syntax_error = true;
            break;
        }
    }

    if (syntax_error)
    {
        output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
        return {false, SortCommand()};
    }

    return {true,
            SortCommand(key,
                        limit_start,
                        limit_count,
                        by_pattern,
                        get_pattern_vec,
                        desc,
                        alpha,
                        store_key)};
}

std::tuple<bool, ScanCommand> ParseScanCommand(
    RedisConnectionContext *ctx,
    const std::vector<std::string_view> &args,
    OutputHandler *output)
{
    assert(args[0] == "scan");
    if (args.size() < 2 || args.size() % 2 != 0)
    {
        output->OnError("ERR wrong number of arguments for 'scan' command");
        return {false, ScanCommand()};
    }

    std::string_view pattern;
    int64_t count = 1000000;
    RedisObjectType obj_type = RedisObjectType::Unknown;
    BucketScanCursor *scan_cursor = nullptr;
    uint64_t cursor_id = 0;
    {
        if (!string2ull(args[1].data(), args[1].size(), cursor_id))
        {
            output->OnError(redis_get_error_messages(RD_ERR_INVALID_CURSOR));
            return {false, ScanCommand()};
        }

        if (cursor_id != 0)
        {
            scan_cursor = ctx->FindBucketScanCursor(cursor_id);
            if (scan_cursor == nullptr)
            {
                output->OnError(
                    redis_get_error_messages(RD_ERR_INVALID_CURSOR));
                return {false, ScanCommand()};
            }
        }
    }

    size_t pos = 2;
    while (pos < args.size())
    {
        if (stringcomp("match", args[pos], 1))
        {
            pattern = args[pos + 1];
        }
        else if (stringcomp("count", args[pos], 1))
        {
            if (!string2ll(args[pos + 1].data(), args[pos + 1].size(), count))
            {
                output->OnError(
                    redis_get_error_messages(RD_ERR_DIGITAL_INVALID));
                return {false, ScanCommand()};
            }
            if (count <= 0)
            {
                output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
                return {false, ScanCommand()};
            }
        }
        else if (stringcomp("type", args[pos], 1))
        {
            std::string_view sv = args[pos + 1];
            if (stringcomp("string", sv, 1))
            {
                obj_type = RedisObjectType::String;
            }
            else if (stringcomp("list", sv, 1))
            {
                obj_type = RedisObjectType::List;
            }
            else if (stringcomp("set", sv, 1))
            {
                obj_type = RedisObjectType::Set;
            }
            else if (stringcomp("zset", sv, 1))
            {
                obj_type = RedisObjectType::Zset;
            }
            else if (stringcomp("hash", sv, 1))
            {
                obj_type = RedisObjectType::Hash;
            }
            else
            {
                output->OnArrayStart(2);
                output->OnString("0");
                output->OnArrayStart(0);
                output->OnArrayEnd();
                output->OnArrayEnd();

                return {false, ScanCommand()};
            }
        }
        else
        {
            output->OnError(redis_get_error_messages(RD_ERR_WRONG_TYPE));
            return {false, ScanCommand()};
        }

        pos += 2;
    }

    return {true, ScanCommand(scan_cursor, pattern, count, obj_type)};
}

std::tuple<bool, ScanCommand> ParseKeysCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "keys");
    if (args.size() != 2)
    {
        output->OnError("ERR wrong number of arguments for 'keys' command");
        return {false, ScanCommand()};
    }

    return {true, ScanCommand(args[1])};
}

std::tuple<bool, BLMoveCommand> ParseBLMoveCommand(
    const std::vector<std::string_view> &args,
    OutputHandler *output,
    bool multi)
{
    assert(args[0] == "blmove");
    if (args.size() != 6)
    {
        output->OnError("wrong number of arguments for 'blmove' command");
        return {false, BLMoveCommand()};
    }

    bool s_left;
    if (stringcomp(args[3], "left", 1))
    {
        s_left = true;
    }
    else if (stringcomp(args[3], "right", 1))
    {
        s_left = false;
    }
    else
    {
        output->OnError("ERR syntax error");
        return {false, BLMoveCommand()};
    }

    bool d_left;
    if (stringcomp(args[4], "left", 1))
    {
        d_left = true;
    }
    else if (stringcomp(args[4], "right", 1))
    {
        d_left = false;
    }
    else
    {
        output->OnError("ERR syntax error");
        return {false, BLMoveCommand()};
    }

    double secs;
    if (!string2double(args[5].data(), args[5].size(), secs))
    {
        output->OnError("ERR timeout is not a float or out of range");
        return {false, BLMoveCommand()};
    }
    if (secs < 0)
    {
        output->OnError("ERR timeout is negative");
        return {false, BLMoveCommand()};
    }
    else if (secs == 0)
    {
        secs = 3600 * 24 * 365;
    }

    uint64_t ts = multi ? 0
                        : txservice::LocalCcShards::ClockTs() +
                              static_cast<uint64_t>(secs * 1000000);
    txservice::BlockOperation bo = multi
                                       ? txservice::BlockOperation::NoBlock
                                       : txservice::BlockOperation::PopNoBlock;
    return {true,
            BLMoveCommand(EloqKey(args[1]),
                          BlockLPopCommand(bo, s_left, 1),
                          EloqKey(args[2]),
                          LMovePushCommand(d_left),
                          ts)};
}

std::tuple<bool, BLMPopCommand> ParseBLMPopCommand(
    const std::vector<std::string_view> &args,
    OutputHandler *output,
    bool multi)
{
    assert(args[0] == "blmpop");
    if (args.size() < 5)
    {
        output->OnError("wrong number of arguments for 'blmpop' command");
        return {false, BLMPopCommand()};
    }

    double secs;
    if (!string2double(args[1].data(), args[1].size(), secs))
    {
        output->OnError("ERR timeout is not a float or out of range");
        return {false, BLMPopCommand()};
    }
    if (secs < 0)
    {
        output->OnError("ERR timeout is negative");
        return {false, BLMPopCommand()};
    }
    else if (secs == 0)
    {
        secs = 3600 * 24 * 365;
    }
    uint64_t ts = multi ? 0
                        : txservice::LocalCcShards::ClockTs() +
                              static_cast<uint64_t>(secs * 1000000);

    int64_t num;
    if (!string2ll(args[2].data(), args[2].size(), num) || num < 0)
    {
        output->OnError("ERR numkeys should be greater than 0");
        return {false, BLMPopCommand()};
    }

    if (num + 4 != static_cast<int64_t>(args.size()) &&
        num + 6 != static_cast<int64_t>(args.size()))
    {
        output->OnError("ERR syntax error");
        return {false, BLMPopCommand()};
    }

    bool is_left;
    if (stringcomp(args[num + 3], "left", 1))
    {
        is_left = true;
    }
    else if (stringcomp(args[num + 3], "right", 1))
    {
        is_left = false;
    }
    else
    {
        output->OnError("ERR syntax error");
        return {false, BLMPopCommand()};
    }

    int64_t cnt = 1;
    if (static_cast<int64_t>(args.size()) == num + 6)
    {
        if (stringcomp(args[num + 4], "count", 1))
            if (!string2ll(args[num + 5].data(), args[num + 5].size(), cnt) ||
                cnt <= 0)
            {
                output->OnError("ERR syntax error");
                return {false, BLMPopCommand()};
            }
    }

    std::vector<EloqKey> vct_key;
    std::vector<BlockLPopCommand> vct_cmd;
    vct_key.reserve(num);
    vct_cmd.reserve(num);
    txservice::BlockOperation bo = multi
                                       ? txservice::BlockOperation::NoBlock
                                       : txservice::BlockOperation::PopNoBlock;

    for (int64_t i = 0; i < num; i++)
    {
        vct_key.emplace_back(args[i + 3]);
        vct_cmd.emplace_back(bo, is_left, cnt);
    }

    return {true,
            BLMPopCommand(std::move(vct_key), std::move(vct_cmd), ts, true)};
}

std::tuple<bool, BLMPopCommand> ParseBLPopCommand(
    const std::vector<std::string_view> &args,
    OutputHandler *output,
    bool multi)
{
    assert(args[0] == "blpop");
    if (args.size() < 3)
    {
        output->OnError("wrong number of arguments for 'blpop' command");
        return {false, BLMPopCommand()};
    }

    double secs;
    size_t pos = args.size() - 1;
    if (!string2double(args[pos].data(), args[pos].size(), secs))
    {
        output->OnError("ERR timeout is not a float or out of range");
        return {false, BLMPopCommand()};
    }
    if (secs < 0)
    {
        output->OnError("ERR timeout is negative");
        return {false, BLMPopCommand()};
    }
    else if (secs == 0)
    {
        secs = 3600 * 24 * 365;
    }

    uint64_t ts = 0;
    uint64_t increment = 0;
    const double max_secs = LLONG_MAX / 1000000.0;
    uint64_t clockTs = txservice::LocalCcShards::ClockTs();

    // Check for overflow
    if (secs <= max_secs)
    {
        increment = static_cast<uint64_t>(secs * 1000000);
    }
    else
    {
        output->OnError(redis_get_error_messages(RD_ERR_VALUE_OUT_OF_RANGE));
        return {false, BLMPopCommand()};
    }

    if (clockTs <= LLONG_MAX - increment)
    {
        ts = clockTs + increment;
    }
    else
    {
        output->OnError(redis_get_error_messages(RD_ERR_VALUE_OUT_OF_RANGE));
        return {false, BLMPopCommand()};
    }
    if (multi)
    {
        ts = 0;
    }

    std::vector<EloqKey> vct_key;
    std::vector<BlockLPopCommand> vct_cmd;
    vct_key.reserve(args.size() - 2);
    vct_cmd.reserve(args.size() - 2);
    txservice::BlockOperation bo = multi
                                       ? txservice::BlockOperation::NoBlock
                                       : txservice::BlockOperation::PopNoBlock;

    for (size_t i = 1; i < args.size() - 1; i++)
    {
        vct_key.emplace_back(args[i]);
        vct_cmd.emplace_back(bo, true, 1);
    }

    return {true,
            BLMPopCommand(std::move(vct_key), std::move(vct_cmd), ts, false)};
}

std::tuple<bool, BLMPopCommand> ParseBRPopCommand(
    const std::vector<std::string_view> &args,
    OutputHandler *output,
    bool multi)
{
    assert(args[0] == "brpop");
    if (args.size() < 3)
    {
        output->OnError("wrong number of arguments for 'brpop' command");
        return {false, BLMPopCommand()};
    }

    double secs;
    size_t pos = args.size() - 1;
    if (!string2double(args[pos].data(), args[pos].size(), secs))
    {
        output->OnError("ERR timeout is not a float or out of range");
        return {false, BLMPopCommand()};
    }
    if (secs < 0)
    {
        output->OnError("ERR timeout is negative");
        return {false, BLMPopCommand()};
    }
    else if (secs == 0)
    {
        secs = 3600 * 24 * 365;
    }
    uint64_t ts = multi ? 0
                        : txservice::LocalCcShards::ClockTs() +
                              static_cast<uint64_t>(secs * 1000000);

    std::vector<EloqKey> vct_key;
    std::vector<BlockLPopCommand> vct_cmd;
    vct_key.reserve(args.size() - 2);
    vct_cmd.reserve(args.size() - 2);
    txservice::BlockOperation bo = multi
                                       ? txservice::BlockOperation::NoBlock
                                       : txservice::BlockOperation::PopNoBlock;

    for (size_t i = 1; i < args.size() - 1; i++)
    {
        vct_key.emplace_back(args[i]);
        vct_cmd.emplace_back(bo, false, 1);
    }

    return {true,
            BLMPopCommand(std::move(vct_key), std::move(vct_cmd), ts, false)};
}

std::tuple<bool, BLMoveCommand> ParseBRPopLPushCommand(
    const std::vector<std::string_view> &args,
    OutputHandler *output,
    bool multi)
{
    assert(args[0] == "brpoplpush");
    if (args.size() != 4)
    {
        output->OnError("wrong number of arguments for 'brpoplpush' command");
        return {false, BLMoveCommand()};
    }

    double secs;
    if (!string2double(args[3].data(), args[3].size(), secs))
    {
        output->OnError("ERR timeout is not a float or out of range");
        return {false, BLMoveCommand()};
    }
    if (secs < 0)
    {
        output->OnError("ERR timeout is negative");
        return {false, BLMoveCommand()};
    }
    else if (secs == 0)
    {
        secs = 3600 * 24 * 365;
    }

    uint64_t ts = multi ? 0
                        : txservice::LocalCcShards::ClockTs() +
                              static_cast<uint64_t>(secs * 1000000);
    txservice::BlockOperation bo = multi
                                       ? txservice::BlockOperation::NoBlock
                                       : txservice::BlockOperation::PopNoBlock;
    return {true,
            BLMoveCommand(EloqKey(args[1]),
                          BlockLPopCommand(bo, false, 1),
                          EloqKey(args[2]),
                          LMovePushCommand(true),
                          ts)};
}

std::tuple<bool, EloqKey, DumpCommand> ParseDumpCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "dump");
    if (args.size() != 2ul)
    {
        output->OnError("ERR wrong number of arguments for 'dump' command");
        return {false, EloqKey(), DumpCommand()};
    }

    return {true, EloqKey(args[1]), DumpCommand()};
}

std::tuple<bool, EloqKey, RestoreCommand> ParseRestoreCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "restore");
    if (args.size() < 4)
    {
        output->OnError("ERR wrong number of arguments for 'restore' command");
        return {false, EloqKey(), RestoreCommand()};
    }

    int64_t ttl, lfu_freq = -1, lru_idle = -1, lru_clock = -1;
    bool replace = false, absttl = false;
    for (size_t j = 4; j < args.size(); j++)
    {
        int additional = args.size() - j - 1;
        if (!strcasecmp(args[j].data(), "replace"))
        {
            replace = true;
        }
        else if (!strcasecmp(args[j].data(), "absttl"))
        {
            absttl = true;
        }
        else if (!strcasecmp(args[j].data(), "idletime") && additional >= 1 &&
                 lfu_freq == -1)
        {
            if (!string2ll(args[j + 1].data(), args[j + 1].size(), lru_idle))
            {
                output->OnError(
                    redis_get_error_messages(RD_ERR_DIGITAL_INVALID));
                return {false, EloqKey(), RestoreCommand()};
            }
            if (lru_idle < 0)
            {
                output->OnError("Invalid IDLETIME value, must be >= 0");
                return {false, EloqKey(), RestoreCommand()};
            }
            (void) lru_clock;  // Evicit option is not supported.
            j++;               // Consume additional arg.
        }
        else if (!strcasecmp(args[j].data(), "freq") && additional >= 1 &&
                 lru_idle == -1)
        {
            if (!string2ll(args[j + 1].data(), args[j + 1].size(), lfu_freq))
            {
                output->OnError(
                    redis_get_error_messages(RD_ERR_DIGITAL_INVALID));
                return {false, EloqKey(), RestoreCommand()};
            }
            if (lfu_freq < 0 || lfu_freq > 255)
            {
                output->OnError("Invalid FREQ value, must be >= 0 and <= 255");
                return {false, EloqKey(), RestoreCommand()};
            }
            j++;  // Consume additional arg.
        }
        else
        {
            output->OnError(redis_get_error_messages(RD_ERR_SYNTAX));
            return {false, EloqKey(), RestoreCommand()};
        }
    }

    if (!string2ll(args[2].data(), args[2].size(), ttl))
    {
        output->OnError(redis_get_error_messages(RD_ERR_DIGITAL_INVALID));
        return {false, EloqKey(), RestoreCommand()};
    }
    else if (ttl < 0)
    {
        output->OnError("Invalid TTL value, must be >= 0");
        return {false, EloqKey(), RestoreCommand()};
    }

    if (!RestoreCommand::VerifyDumpPayload(args[3]))
    {
        output->OnError("DUMP payload version or checksum are wrong");
        return {false, EloqKey(), RestoreCommand()};
    }

    if (ttl && !absttl)
    {
        ttl +=
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::microseconds(txservice::LocalCcShards::ClockTs()))
                .count();
    }

    uint64_t expire_when =
        ttl > 0 ? std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::milliseconds(ttl))
                      .count()
                : UINT64_MAX;
    return {true,
            EloqKey(args[1]),
            RestoreCommand(args[3].data(),
                           args[3].size() - sizeof(DumpCommand::dump_version_) -
                               sizeof(uint64_t),
                           expire_when,
                           replace)};
}

bool ParseFlushDBCommand(const std::vector<std::string_view> &args,
                         OutputHandler *output)
{
    if (args.size() > 1)
    {
        output->OnError("ERR syntax error");
        return false;
    }
    else
    {
        return true;
    }
}

bool ParseFlushALLCommand(const std::vector<std::string_view> &args,
                          OutputHandler *output)
{
    if (args.size() > 1)
    {
        output->OnError("ERR syntax error");
        return false;
    }
    else
    {
        return true;
    }
}

#ifdef WITH_FAULT_INJECT
std::tuple<bool, RedisFaultInjectCommand> ParseFaultInjectCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "fault_inject");
    //     0         1         2        3
    // fault_inject name node_group_id params
    // params is not mandatory
    if (args.size() < 3)
    {
        output->OnError("ERR syntax error");
        return {false, RedisFaultInjectCommand()};
    }

    int64_t node_group_id;
    if (!string2ll(args[2].data(), args[2].size(), node_group_id))
    {
        output->OnError(redis_get_error_messages(RD_ERR_DIGITAL_INVALID));
        return {false, RedisFaultInjectCommand()};
    }

    const std::string_view &fault_inject_name = args[1];
    if (args.size() == 4)
    {
        const std::string_view &params = args[3];
        return {
            true,
            RedisFaultInjectCommand(fault_inject_name, params, node_group_id)};
    }
    else
    {
        return {true,
                RedisFaultInjectCommand(fault_inject_name, "", node_group_id)};
    }
}
#endif

std::tuple<bool, EloqKey, RedisMemoryUsageCommand> ParseMemoryUsageCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    if (args.size() != 3)
    {
        output->OnError("wrong number of arguments for 'memory usage' command");
        return {false, EloqKey(), RedisMemoryUsageCommand()};
    }

    return {true, EloqKey(args[2]), RedisMemoryUsageCommand(args[2].size())};
}

std::tuple<bool, EloqKey, ExpireCommand> ParseExpireCommand(
    const std::vector<std::string_view> &args,
    OutputHandler *output,
    bool is_pexpire,
    bool is_expireat)
{
    if (args.size() < 3 || args.size() > 5)
    {
        output->OnError("Wrong number of arguments for 'expire' command");
        return {false, EloqKey(), ExpireCommand()};
    }

    uint8_t flags = 0;
    int64_t expire_ts_tmp = 0;
    uint64_t expire_ts = UINT64_MAX;
    if (is_expireat)
    {
        if (!EloqKV::string2ll(args[2].data(), args[2].size(), expire_ts_tmp))
        {
            output->OnError("not an integer");
            return {false, EloqKey(), ExpireCommand()};
        }

        if (!is_pexpire)
        {
            if (EloqKV::will_multiplication_overflow(expire_ts_tmp, 1000ll))
            {
                output->OnError("ERR invalid expire time in 'pexpire' command");
                return {false, EloqKey(), ExpireCommand()};
            }
            expire_ts_tmp = expire_ts_tmp * 1000;
        }

        // expire immediately if expire ts is negative
        expire_ts = expire_ts_tmp < 0 ? 0 : expire_ts_tmp;
    }
    else
    {
        if (!EloqKV::string2ll(args[2].data(), args[2].size(), expire_ts_tmp))
        {
            output->OnError("not an integer");
            return {false, EloqKey(), ExpireCommand()};
        }

        uint64_t current_ts = txservice::LocalCcShards::ClockTsInMillseconds();
        if (!is_pexpire)
        {
            if (EloqKV::will_multiplication_overflow(expire_ts_tmp, 1000ll))
            {
                output->OnError("ERR invalid expire time in 'expire' command");
                return {false, EloqKey(), ExpireCommand()};
            }
            expire_ts_tmp = expire_ts_tmp * 1000;
        }

        if (EloqKV::will_addition_overflow(static_cast<int64_t>(current_ts),
                                           expire_ts_tmp))
        {
            std::stringstream ss;
            ss << "ERR invalid expire time in '"
               << (is_pexpire ? "pexpire" : "expire") << "' command";
            output->OnError(ss.str());
            return {false, EloqKey(), ExpireCommand()};
        }

        expire_ts =
            (current_ts + expire_ts_tmp) < 0 ? 0 : (current_ts + expire_ts_tmp);
    }

    if (args.size() >= 4)
    {
        if (stringcomp(args[3], "nx", 1))
        {
            flags |= EXPIRE_NX;
        }
        else if (stringcomp(args[3], "xx", 1))
        {
            flags |= EXPIRE_XX;
        }
        else if (stringcomp(args[3], "gt", 1))
        {
            flags |= EXPIRE_GT;
        }
        else if (stringcomp(args[3], "lt", 1))
        {
            flags |= EXPIRE_LT;
        }
        else
        {
            std::stringstream ss;
            ss << "ERR Unsupported option " << args[3];
            output->OnError(ss.str());
            return {false, EloqKey(), ExpireCommand()};
        }
    }

    if (args.size() == 5)
    {
        if (stringcomp(args[4], "nx", 1))
        {
            flags |= EXPIRE_NX;
        }
        else if (stringcomp(args[4], "xx", 1))
        {
            flags |= EXPIRE_XX;
        }
        else if (stringcomp(args[4], "gt", 1))
        {
            flags |= EXPIRE_GT;
        }
        else if (stringcomp(args[4], "lt", 1))
        {
            flags |= EXPIRE_LT;
        }
        else
        {
            std::stringstream ss;
            ss << "ERR Unsupported option " << args[4];
            output->OnError(ss.str());
            return {false, EloqKey(), ExpireCommand()};
        }
    }

    if ((flags & EXPIRE_LT) && (flags & EXPIRE_GT))
    {
        std::stringstream ss;
        ss << "ERR GT and LT options at the same time are not compatible";
        output->OnError(ss.str());
        return {false, EloqKey(), ExpireCommand()};
    }

    if (((flags & EXPIRE_NX) && (flags & EXPIRE_GT)) ||
        ((flags & EXPIRE_NX) && (flags & EXPIRE_LT)) ||
        ((flags & EXPIRE_NX) && (flags & EXPIRE_XX)))
    {
        std::stringstream ss;
        ss << "ERR NX and XX, GT or LT options at the same time are not "
              "compatible";
        output->OnError(ss.str());
        return {false, EloqKey(), ExpireCommand()};
    }

    return {true, EloqKey(args[1]), ExpireCommand(expire_ts, flags)};
}

std::tuple<bool, EloqKey, TTLCommand> ParseTTLCommand(
    const std::vector<std::string_view> &args,
    OutputHandler *output,
    bool is_pttl,
    bool is_expire_time,
    bool is_pexpire_time)
{
    assert(is_pttl ? (args[0] == "pttl" || args[0] == "PTTL") : true);

    assert(is_expire_time ? (args[0] == "expiretime" || args[0] == "EXPIRETIME")
                          : true);
    assert(is_pexpire_time
               ? (args[0] == "pexpiretime" || args[0] == "PEXPIRETIME")
               : true);

    if (args.size() != 2)
    {
        output->OnError("wrong number of arguments for 'ttl' command");
        return {false,
                EloqKey(),
                TTLCommand(is_pttl, is_expire_time, is_pexpire_time)};
    }
    return {true,
            EloqKey(args[1]),
            TTLCommand(is_pttl, is_expire_time, is_pexpire_time)};
}

std::tuple<bool, EloqKey, PersistCommand> ParsePersistCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "persist" || args[0] == "PERSIST");
    if (args.size() != 2)
    {
        output->OnError("wrong number of arguments for 'persist' command");
        return {false, EloqKey(), PersistCommand()};
    }
    return {true, EloqKey(args[1]), PersistCommand()};
}

std::tuple<bool, EloqKey, GetExCommand> ParseGetExCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    if (args.size() < 2 || args.size() > 4)
    {
        output->OnError("Wrong number of arguments for 'getex' command");
        return {false, EloqKey(), GetExCommand()};
    }

    uint64_t expire_ts = UINT64_MAX;
    int64_t expire_ts_tmp = 0;
    bool is_px = false;
    bool is_at = false;
    bool is_persist = false;

    if (args.size() == 2)
    {
        return {true, EloqKey(args[1]), GetExCommand(expire_ts, is_persist)};
    }
    else if (args.size() == 3 && (args[2] == "persist" || args[2] == "PERSIST"))
    {
        is_px = false;
        is_at = false;
        is_persist = true;
    }
    else if (args.size() == 4)  // case of EX, PX, EXAT, PXAT
    {
        is_persist = false;
        if (args[2] == "ex" || args[2] == "EX")
        {
            is_px = false;
            is_at = false;
        }
        else if (args[2] == "px" || args[2] == "PX")
        {
            is_px = true;
            is_at = false;
        }
        else if (args[2] == "exat" || args[2] == "EXAT")
        {
            is_px = false;
            is_at = true;
        }
        else if (args[2] == "pxat" || args[2] == "PXAT")
        {
            is_px = true;
            is_at = true;
        }
        else
        {
            std::stringstream ss;
            ss << "wrong argument of " << args[2] << " for 'getex' command";
            output->OnError(ss.str());
            return {false, EloqKey(), GetExCommand()};
        }

        if (!EloqKV::string2ll(args[3].data(), args[3].size(), expire_ts_tmp))
        {
            std::stringstream ss;
            ss << "ERR invalid expire time in 'getex' command";
            output->OnError(ss.str());
            return {false, EloqKey(), GetExCommand()};
        }
    }
    else
    {
        std::stringstream ss;
        ss << "wrong argument of " << args[2] << " for 'getex' command";
        output->OnError(ss.str());
        return {false, EloqKey(), GetExCommand()};
    }

    if (!is_persist)
    {
        if (!is_px)
        {
            if (EloqKV::will_multiplication_overflow(expire_ts_tmp, 1000ll))
            {
                output->OnError("ERR invalid expire time in 'getex' command");
                return {false, EloqKey(), GetExCommand()};
            }
            expire_ts_tmp = expire_ts_tmp * 1000;
        }

        if (is_at)
        {
            expire_ts = expire_ts_tmp;
        }
        else
        {
            uint64_t current_ts =
                txservice::LocalCcShards::ClockTsInMillseconds();
            if (EloqKV::will_addition_overflow(static_cast<int64_t>(current_ts),
                                               expire_ts_tmp))
            {
                output->OnError("ERR invalid expire time in 'getex' command");
                return {false, EloqKey(), GetExCommand()};
            }
            expire_ts = (current_ts + expire_ts_tmp) < 0
                            ? 0
                            : (current_ts + expire_ts_tmp);
        }
    }

    return {true, EloqKey(args[1]), GetExCommand(expire_ts, is_persist)};
}

std::tuple<bool, TimeCommand> ParseTimeCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    if (args.size() > 1)
    {
        output->OnError("ERR wrong number of arguments for 'time' command");
        return {false, TimeCommand()};
    }
    return {true, TimeCommand()};
}

std::tuple<bool, CreateVecIndexCommand> ParseCreateVecIndexCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "eloqvec.create" || args[0] == "ELOQVEC.CREATE");

    // Minimum required parameters: ELOQVEC.CREATE <index_name> ON
    // <hash_set_name> COLUMN <vector_column_name> DIMENSIONS <dimensions>
    // METRIC <metric_type> ALGORITHM <algorithm> That's 12 arguments minimum
    // (including the command)
    if (args.size() < 12)
    {
        output->OnError(
            "ERR wrong number of arguments for 'eloqvec.create' command");
        return {false, CreateVecIndexCommand()};
    }

    // Variables to hold parsed parameters
    std::string_view index_name;
    std::string_view hash_set_name;
    std::string_view vector_column_name;
    uint64_t dimensions;
    EloqVec::Algorithm algorithm;
    EloqVec::DistanceMetric metric_type;
    std::unordered_map<std::string, std::string> alg_params;

    size_t pos = 1;

    // Parse index_name
    index_name = args[pos++];

    // Parse ON keyword and hash_set_name
    if (!stringcomp("on", args[pos], 1))
    {
        output->OnError("ERR syntax error: expected 'ON' keyword");
        return {false, CreateVecIndexCommand()};
    }
    pos++;
    hash_set_name = args[pos++];

    // Parse COLUMN keyword and vector_column_name
    if (!stringcomp("column", args[pos], 1))
    {
        output->OnError("ERR syntax error: expected 'COLUMN' keyword");
        return {false, CreateVecIndexCommand()};
    }
    pos++;
    vector_column_name = args[pos++];

    // Parse DIMENSIONS keyword and dimensions value
    if (!stringcomp("dimensions", args[pos], 1))
    {
        output->OnError("ERR syntax error: expected 'DIMENSIONS' keyword");
        return {false, CreateVecIndexCommand()};
    }
    pos++;
    if (!string2ull(args[pos].data(), args[pos].size(), dimensions))
    {
        output->OnError(
            "ERR invalid value for DIMENSIONS: must be a positive integer");
        return {false, CreateVecIndexCommand()};
    }
    pos++;

    // Parse METRIC keyword and metric_type
    if (!stringcomp("metric", args[pos], 1))
    {
        output->OnError("ERR syntax error: expected 'METRIC' keyword");
        return {false, CreateVecIndexCommand()};
    }
    pos++;
    // Convert metric string to proper format for string_to_distance_metric
    metric_type = EloqVec::string_to_distance_metric(args[pos]);
    if (metric_type == EloqVec::DistanceMetric::UNKNOWN)
    {
        output->OnError(
            "ERR unsupported algorithm: only COSINE, L2SQ and IP are "
            "supported");
        return {false, CreateVecIndexCommand()};
    }
    pos++;

    // Parse ALGORITHM keyword and algorithm
    if (!stringcomp("algorithm", args[pos], 1))
    {
        output->OnError("ERR syntax error: expected 'ALGORITHM' keyword");
        return {false, CreateVecIndexCommand()};
    }
    pos++;
    algorithm = EloqVec::string_to_algorithm(args[pos]);
    if (algorithm == EloqVec::Algorithm::UNKNOWN)
    {
        output->OnError("ERR unsupported algorithm: only HNSW are supported");
        return {false, CreateVecIndexCommand()};
    }
    pos++;

    // Parse optional parameters
    while (pos < args.size())
    {
        if (pos + 1 >= args.size())
        {
            output->OnError("ERR syntax error: parameter missing value");
            return {false, CreateVecIndexCommand()};
        }

        std::string_view param = args[pos];
        std::string_view value = args[pos + 1];

        if (stringcomp("connectivity", param, 1))
        {
            int64_t connectivity;
            if (!string2ll(value.data(), value.size(), connectivity) ||
                connectivity <= 0)
            {
                output->OnError(
                    "ERR invalid value for CONNECTIVITY: must be a positive "
                    "integer");
                return {false, CreateVecIndexCommand()};
            }
            alg_params["connectivity"] = std::string(value);
        }
        else if (stringcomp("ef_construct", param, 1))
        {
            int64_t ef_construct;
            if (!string2ll(value.data(), value.size(), ef_construct) ||
                ef_construct <= 0)
            {
                output->OnError(
                    "ERR invalid value for EF_CONSTRUCT: must be a positive "
                    "integer");
                return {false, CreateVecIndexCommand()};
            }
            alg_params["ef_construct"] = std::string(value);
        }
        else if (stringcomp("ef_search", param, 1))
        {
            int64_t ef_search;
            if (!string2ll(value.data(), value.size(), ef_search) ||
                ef_search <= 0)
            {
                output->OnError(
                    "ERR invalid value for EF_SEARCH: must be a positive "
                    "integer");
                return {false, CreateVecIndexCommand()};
            }
            alg_params["ef_search"] = std::string(value);
        }
        else if (stringcomp("scalar_type", param, 1))
        {
            std::string scalar_type_str = std::string(value);
            std::transform(scalar_type_str.begin(),
                           scalar_type_str.end(),
                           scalar_type_str.begin(),
                           ::tolower);
            if (scalar_type_str != "f32" && scalar_type_str != "f64" &&
                scalar_type_str != "i8" && scalar_type_str != "i16" &&
                scalar_type_str != "i32")
            {
                output->OnError(
                    "ERR unsupported scalar type: supported types are f32, "
                    "f64, i8, i16, i32");
                return {false, CreateVecIndexCommand()};
            }
            alg_params["scalar_type"] = scalar_type_str;
        }
        else
        {
            output->OnError("ERR unknown parameter: " + std::string(param));
            return {false, CreateVecIndexCommand()};
        }

        pos += 2;
    }

    // Create command with all parsed parameters using constructor
    return {true,
            CreateVecIndexCommand(index_name,
                                  hash_set_name,
                                  vector_column_name,
                                  dimensions,
                                  algorithm,
                                  metric_type,
                                  std::move(alg_params))};
}

bool CreateVecIndexCommand::Execute(RedisServiceImpl *redis_impl,
                                    RedisConnectionContext *ctx,
                                    const txservice::TableName *table,
                                    txservice::TransactionExecution *txm,
                                    OutputHandler *output,
                                    bool auto_commit)
{
    return redis_impl->ExecuteCommand(
        ctx, txm, table, this, output, auto_commit);
}

void CreateVecIndexCommand::OutputResult(OutputHandler *reply,
                                         RedisConnectionContext *ctx) const
{
    // TODO: Implement vector index creation result output
    // This should output either:
    // - Success message on successful index creation
    // - Error message on failure
    if (result_.err_code_ == RD_OK)
    {
        reply->OnString("OK");
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

std::tuple<bool, InfoVecIndexCommand> ParseInfoVecIndexCommand(
    const std::vector<std::string_view> &args, OutputHandler *output)
{
    assert(args[0] == "eloqvec.info" || args[0] == "ELOQVEC.INFO");

    // ELOQVEC.INFO <index_name>
    if (args.size() != 2)
    {
        output->OnError(
            "ERR wrong number of arguments for 'eloqvec.info' command");
        return {false, InfoVecIndexCommand()};
    }

    std::string_view index_name = args[1];

    // Create command with parsed parameter
    return {true, InfoVecIndexCommand(index_name)};
}

bool InfoVecIndexCommand::Execute(RedisServiceImpl *redis_impl,
                                  RedisConnectionContext *ctx,
                                  const txservice::TableName *table,
                                  txservice::TransactionExecution *txm,
                                  OutputHandler *output,
                                  bool auto_commit)
{
    return redis_impl->ExecuteCommand(
        ctx, txm, table, this, output, auto_commit);
}

void InfoVecIndexCommand::OutputResult(OutputHandler *reply,
                                       RedisConnectionContext *ctx) const
{
    // TODO: Implement vector index info result output
    // This should output index information such as:
    // - Index name, hash set, vector column
    // - Dimensions, algorithm, metric type
    // - Index statistics (number of vectors, etc.)
    if (result_.err_code_ == RD_OK)
    {
        // For now, return a simple placeholder response
        reply->OnArrayStart(8);
        reply->OnString("index_name");
        reply->OnString(index_name_.StringView());
        reply->OnString("status");
        reply->OnString("ready");
        reply->OnString("algorithm");
        reply->OnString("HNSW");
        reply->OnString("metric");
        reply->OnString("cosine");
        reply->OnArrayEnd();
    }
    else
    {
        reply->OnError(redis_get_error_messages(result_.err_code_));
    }
}

}  // namespace EloqKV
