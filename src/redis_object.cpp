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
#include "redis_object.h"  // RedisEloqObject

#include <memory>

#include "redis_hash_object.h"
#include "redis_list_object.h"
#include "redis_set_object.h"
#include "redis_string_object.h"
#include "redis_zset_object.h"
namespace EloqKV
{
txservice::TxRecord::Uptr RedisEloqObject::DeserializeObject(
    const char *buf, size_t &offset) const
{
    int8_t obj_type_int = static_cast<int8_t>(*(buf + offset));
    RedisObjectType obj_type = static_cast<RedisObjectType>(obj_type_int);
    txservice::TxRecord::Uptr typed_rec;
    switch (obj_type)
    {
    case RedisObjectType::String:
        typed_rec = std::make_unique<RedisStringObject>();
        break;
    case RedisObjectType::List:
        typed_rec = std::make_unique<RedisListObject>();
        break;
    case RedisObjectType::Hash:
        typed_rec = std::make_unique<RedisHashObject>();
        break;
    case RedisObjectType::Zset:
        typed_rec = std::make_unique<RedisZsetObject>();
        break;
    case RedisObjectType::Set:
        typed_rec = std::make_unique<RedisHashSetObject>();
        break;
    case RedisObjectType::TTLString:
        typed_rec = std::make_unique<RedisStringTTLObject>();
        break;
    case RedisObjectType::TTLHash:
        typed_rec = std::make_unique<RedisHashTTLObject>();
        break;
    case RedisObjectType::TTLList:
        typed_rec = std::make_unique<RedisListTTLObject>();
        break;
    case RedisObjectType::TTLZset:
        typed_rec = std::make_unique<RedisZsetTTLObject>();
        break;
    case RedisObjectType::TTLSet:
        typed_rec = std::make_unique<RedisHashSetTTLObject>();
        break;
    default:
        assert(false);
    }

    typed_rec->Deserialize(buf, offset);
    return typed_rec;
}
}  // namespace EloqKV
