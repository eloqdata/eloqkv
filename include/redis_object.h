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

    static TxRecord::Uptr Create()
    {
        return std::make_unique<RedisEloqObject>();
    }
};
}  // namespace EloqKV
