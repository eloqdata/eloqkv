#include <absl/container/flat_hash_set.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "eloq_string.h"
#include "redis_command.h"
#include "redis_object.h"
#include "tx_command.h"

namespace EloqKV
{
class RedisHashSetObject : public RedisEloqObject
{
public:
    RedisHashSetObject()
    {
        serialized_length_ = 1 + sizeof(uint32_t);
    }
    RedisHashSetObject(const RedisHashSetObject &);
    RedisHashSetObject(RedisHashSetObject &&rhs);
    ~RedisHashSetObject() override = default;

    RedisObjectType ObjectType() const override
    {
        return RedisObjectType::Set;
    }

    TxRecord::Uptr Clone() const override
    {
        return std::make_unique<RedisHashSetObject>(*this);
    }

    txservice::TxRecord::Uptr AddTTL(uint64_t) override;

    bool Execute(SAddCommand &cmd) const;
    void Execute(SMembersCommand &cmd) const;
    void Execute(SRemCommand &cmd) const;
    void Execute(SCardCommand &cmd) const;
    void Execute(SIsMemberCommand &cmd) const;
    void Execute(SMIsMemberCommand &cmd) const;
    void Execute(SRandMemberCommand &cmd) const;
    void Execute(SPopCommand &cmd) const;
    void Execute(SScanCommand &cmd) const;
    void Execute(SortableLoadCommand &cmd) const;
    void Execute(SZScanCommand &cmd) const;
    void Execute(ZRangeCommand &cmd) const;

    bool CommitSAdd(std::vector<EloqString> &paras,
                    bool should_not_move_string);
    bool CommitSRem(const std::vector<EloqString> &paras);
    bool CommitSPop(const std::vector<EloqString> &members);

    size_t SerializedLength() const override
    {
        return serialized_length_;
    }

    void Serialize(std::vector<char> &buf, size_t &offset) const override;

    void Serialize(std::string &str) const override;

    void Deserialize(const char *buf, size_t &offset) override;

    size_t ElementsSize()
    {
        return hash_set_.size();
    }

    const absl::flat_hash_set<EloqString> &Elements() const
    {
        return hash_set_;
    }

protected:
    absl::flat_hash_set<EloqString> hash_set_;
    // The estimated memory size of the object. Limit the objects size so that
    // the persistent storage flush wouldn't fail.
    uint64_t serialized_length_{};
};

class RedisHashSetTTLObject : public RedisHashSetObject
{
public:
    RedisHashSetTTLObject() : RedisHashSetObject(), ttl_(UINT64_MAX)
    {
    }

    RedisHashSetTTLObject(const RedisHashSetTTLObject &other)
        : RedisHashSetObject(other), ttl_(other.ttl_)
    {
    }

    RedisHashSetTTLObject(RedisHashSetObject &&other, uint64_t ttl)
        : RedisHashSetObject(std::move(other)), ttl_(ttl)
    {
    }

    void SetTTL(uint64_t ttl) override
    {
        ttl_ = ttl;
    }

    uint64_t GetTTL() const override
    {
        return ttl_;
    }

    bool HasTTL() const override
    {
        return true;
    }

    RedisObjectType ObjectType() const override
    {
        return RedisObjectType::Set;
    }

    size_t SerializedLength() const override
    {
        // sizeof(ttl) + sizeof(hash_object)
        return sizeof(uint64_t) + RedisHashSetObject::SerializedLength();
    }

    void Serialize(std::vector<char> &buf, size_t &offset) const override;

    void Serialize(std::string &str) const override;

    void Deserialize(const char *buf, size_t &offset) override;

    TxRecord::Uptr Clone() const override
    {
        auto ptr = std::make_unique<RedisHashSetTTLObject>(*this);
        return ptr;
    }

    TxRecord::Uptr RemoveTTL() override
    {
        return std::make_unique<RedisHashSetObject>(std::move(*this));
    }

private:
    uint64_t ttl_{UINT64_MAX};
};

}  // namespace EloqKV
