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
#pragma once

#include <algorithm>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "tx_service/include/cc/local_cc_shards.h"
#include "tx_service/include/tx_object.h"
#include "tx_service/include/tx_record.h"

namespace EloqKV
{
// Don't change the RedisObjectTypes order, because object serialization used
// its int vlaue.
enum struct RedisObjectType
{
    Unknown = -1,
    String = 0,
    List = 1,
    Hash = 2,
    Del = 3,
    Zset = 4,
    Set = 5,
    TTLString = 6,
    TTLList = 7,
    TTLHash = 8,
    TTLZset = 10,
    TTLSet = 11
};

struct RedisEloqObject : public txservice::TxObject
{
public:
    TxRecord::Uptr Clone() const override
    {
        assert(false);
        return nullptr;
        // return std::make_unique<RedisEloqObject>(*this);
    }

    void Serialize(std::vector<char> &buf, size_t &offset) const override
    {
    }

    void Serialize(std::string &str) const override
    {
    }

    void Deserialize(const char *buf, size_t &offset) override
    {
    }

    void Copy(const TxRecord &rhs) override
    {
        assert(false);
    }

    std::string ToString() const override
    {
        assert(false);
        return "";
    }

    virtual RedisObjectType ObjectType() const
    {
        return RedisObjectType::Unknown;
    }

    bool IsMatchType(int32_t obj_type) const
    {
        return obj_type < 0 || static_cast<int32_t>(ObjectType()) == obj_type;
    }

    TxRecord::Uptr DeserializeObject(const char *buf,
                                     size_t &offset) const override;

    void SetEncodedBlob(const unsigned char *blob_ptr,
                        size_t blob_size) override
    {
        // deserialize object from blob str
        size_t offset = 0;
        Deserialize(reinterpret_cast<const char *>(blob_ptr), offset);
    }

    void SetUnpackInfo(const unsigned char *unpack_ptr,
                       size_t unpack_size) override
    {
        // Do nothing
    }

    static TxRecord::Uptr Create()
    {
        return std::make_unique<RedisEloqObject>();
    }
};
}  // namespace EloqKV
