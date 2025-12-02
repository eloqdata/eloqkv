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
#include <absl/container/flat_hash_map.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "eloq_string.h"
#include "redis_command.h"
#include "redis_object.h"
#include "tx_command.h"
#include "tx_record.h"

namespace EloqKV
{
class RedisHashObject : public RedisEloqObject
{
public:
    RedisHashObject()
    {
        serialized_length_ = 1 + sizeof(uint32_t);
    }
    RedisHashObject(const RedisHashObject &rhs)
        : RedisEloqObject(rhs), hash_map_(rhs.hash_map_)
    {
        serialized_length_ = rhs.serialized_length_;
    }
    RedisHashObject(RedisHashObject &&rhs)
        : RedisEloqObject(rhs), hash_map_(std::move(rhs.hash_map_))
    {
        serialized_length_ = rhs.serialized_length_;
        rhs.serialized_length_ = 1 + sizeof(uint32_t);
    }
    ~RedisHashObject() override = default;
    RedisHashObject operator=(const RedisHashObject &rhs) = delete;
    RedisHashObject operator=(RedisHashObject &&rhs) = delete;

    txservice::TxRecord::Uptr Clone() const override
    {
        return std::make_unique<RedisHashObject>(*this);
    }

    RedisObjectType ObjectType() const override
    {
        return RedisObjectType::Hash;
    }

    size_t SerializedLength() const override
    {
        return serialized_length_;
    }

    void Serialize(std::vector<char> &buf, size_t &offset) const override
    {
        size_t off_old = offset;
        // new size = offset + size(real hash size) + sizeof every key-value
        // size + (uint32)cnt(count of hash)
        buf.resize(offset + serialized_length_);

        // Why do we need to serialize object types?
        // Because we only store the serialized bytes in kvstore, we need the
        // object type to specify which kind of redis object to create and
        // deserialize it when do read.
        int8_t obj_type = static_cast<int8_t>(RedisObjectType::Hash);
        std::copy(&obj_type, &obj_type + 1, buf.begin() + offset);
        offset += 1;

        uint32_t cnt = static_cast<uint32_t>(hash_map_.size());
        const char *cnt_ptr = reinterpret_cast<const char *>(&cnt);
        std::copy(cnt_ptr, cnt_ptr + sizeof(uint32_t), buf.begin() + offset);
        offset += sizeof(uint32_t);

        for (const auto &[key, value] : hash_map_)
        {
            uint32_t ks = key.Length();
            uint32_t vs = value.Length();
            const char *ks_ptr = reinterpret_cast<const char *>(&ks);
            const char *vs_ptr = reinterpret_cast<const char *>(&vs);
            std::copy(ks_ptr, ks_ptr + sizeof(uint32_t), buf.begin() + offset);
            offset += sizeof(uint32_t);

            std::copy(key.Data(), key.Data() + ks, buf.begin() + offset);
            offset += ks;

            std::copy(vs_ptr, vs_ptr + sizeof(uint32_t), buf.begin() + offset);
            offset += sizeof(uint32_t);

            std::copy(value.Data(), value.Data() + vs, buf.begin() + offset);
            offset += vs;
        }
        assert(off_old + serialized_length_ == offset);
        (void) off_old;
    }

    void Serialize(std::string &str) const override
    {
        size_t off_old = str.size();
        str.reserve(off_old + serialized_length_);

        int8_t obj_type_val = static_cast<int8_t>(RedisObjectType::Hash);
        str.append(1, obj_type_val);

        uint32_t cnt = static_cast<uint32_t>(hash_map_.size());
        const char *cnt_ptr = reinterpret_cast<const char *>(&cnt);
        str.append(cnt_ptr, sizeof(uint32_t));

        for (const auto &[key, value] : hash_map_)
        {
            uint32_t ks = key.Length(), vs = value.Length();
            const char *ks_ptr = reinterpret_cast<const char *>(&ks);
            const char *vs_ptr = reinterpret_cast<const char *>(&vs);
            str.append(ks_ptr, sizeof(uint32_t));
            str.append(key.Data(), ks);

            str.append(vs_ptr, sizeof(uint32_t));
            str.append(value.Data(), vs);
        }

        assert(serialized_length_ + off_old == str.size());
        (void) off_old;
    }

    void Deserialize(const char *buf, size_t &offset) override
    {
        size_t init_off = offset;
        // Check the object type.
        RedisObjectType obj_type =
            static_cast<RedisObjectType>(*(buf + offset));
        assert(obj_type == RedisObjectType::Hash);
        (void) obj_type;
        offset += 1;

        const uint32_t cnt = *reinterpret_cast<const uint32_t *>(buf + offset);
        offset += sizeof(uint32_t);

        for (uint32_t i = 0; i < cnt; i++)
        {
            const uint32_t ks =
                *reinterpret_cast<const uint32_t *>(buf + offset);
            offset += sizeof(uint32_t);
            EloqString key(buf + offset, ks);
            offset += ks;

            const uint32_t vs =
                *reinterpret_cast<const uint32_t *>(buf + offset);
            offset += sizeof(uint32_t);
            EloqString value(buf + offset, vs);
            offset += vs;

            SetField(key, value);
        }

        serialized_length_ = offset - init_off;
    }

    // The equal here refers to whether the objects pointed to are consistent.
    // It is possible that one is an owned object and the other is a reference
    // object, but as long as the objects they point to are consistent, then
    // they are consistent under the current semantics.
    bool operator==(const RedisHashObject &rhs) const
    {
        for (auto &[key, value] : hash_map_)
        {
            auto it = rhs.hash_map_.find(key);
            if (it == rhs.hash_map_.end())
            {
                return false;
            }
            if (it->second.StringView() != value.StringView())
            {
                return false;
            }
        }

        return hash_map_.size() == rhs.hash_map_.size();
    }

    const absl::flat_hash_map<EloqString, EloqString> &Elements() const
    {
        return hash_map_;
    }

    TxRecord::Uptr AddTTL(uint64_t ttl) override;

    bool Execute(HSetCommand &) const;
    void Execute(HGetCommand &) const;
    void Execute(HLenCommand &) const;
    void Execute(HStrLenCommand &) const;
    CommandExecuteState Execute(HDelCommand &) const;
    void Execute(HExistsCommand &) const;
    void Execute(HGetAllCommand &) const;
    bool Execute(HIncrByCommand &) const;
    void Execute(HMGetCommand &) const;
    void Execute(HKeysCommand &) const;
    void Execute(HValsCommand &) const;
    bool Execute(HSetNxCommand &) const;
    void Execute(HRandFieldCommand &) const;
    void Execute(HScanCommand &) const;
    bool Execute(HIncrByFloatCommand &) const;

    void CommitHset(std::vector<std::pair<EloqString, EloqString>> &);
    bool CommitHdel(std::vector<EloqString> &);
    void CommitHincrby(EloqString &, int64_t);
    void CommitHSetNx(EloqString &, EloqString &);
    void CommitHIncrByFloat(EloqString &, long double);

protected:
    void SetField(EloqString &field, EloqString &value);
    absl::flat_hash_map<EloqString, EloqString> hash_map_;
    // The estimated memory size of the object. Limit the objects size so that
    // the persistent storage flush wouldn't fail.
    uint64_t serialized_length_{};
};

class RedisHashTTLObject : public RedisHashObject
{
public:
    RedisHashTTLObject() : RedisHashObject(), ttl_(UINT64_MAX)
    {
    }

    RedisHashTTLObject(const RedisHashTTLObject &other)
        : RedisHashObject(other), ttl_(other.ttl_)
    {
    }

    RedisHashTTLObject(RedisHashObject &&other, uint64_t ttl)
        : RedisHashObject(std::move(other)), ttl_(ttl)
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
        // Return base class type, commands can process ttl class like its base
        // class
        return RedisObjectType::Hash;
    }

    size_t SerializedLength() const override
    {
        // sizeof(ttl) + sizeof(hash_object)
        return sizeof(uint64_t) + RedisHashObject::SerializedLength();
    }

    void Serialize(std::vector<char> &buf, size_t &offset) const override
    {
        size_t off_old = offset;
        // new size = offset + ttl(uint64_t) + size(real hash size) + sizeof
        // every key-value size + (uint32)cnt(count of hash)
        buf.resize(offset + sizeof(uint64_t) + serialized_length_);

        // Why do we need to serialize object types?
        // Because we only store the serialized bytes in kvstore, we need the
        // object type to specify which kind of redis object to create and
        // deserialize it when do read.
        int8_t obj_type = static_cast<int8_t>(RedisObjectType::TTLHash);
        std::copy(&obj_type, &obj_type + 1, buf.begin() + offset);
        offset += 1;

        // serialize ttl_
        std::copy(&ttl_, &ttl_ + sizeof(uint64_t), buf.begin() + offset);
        offset += sizeof(uint64_t);

        uint32_t cnt = static_cast<uint32_t>(hash_map_.size());
        const char *cnt_ptr = reinterpret_cast<const char *>(&cnt);
        std::copy(cnt_ptr, cnt_ptr + sizeof(uint32_t), buf.begin() + offset);
        offset += sizeof(uint32_t);

        for (const auto &[key, value] : hash_map_)
        {
            uint32_t ks = key.Length();
            uint32_t vs = value.Length();
            const char *ks_ptr = reinterpret_cast<const char *>(&ks);
            const char *vs_ptr = reinterpret_cast<const char *>(&vs);
            std::copy(ks_ptr, ks_ptr + sizeof(uint32_t), buf.begin() + offset);
            offset += sizeof(uint32_t);

            std::copy(key.Data(), key.Data() + ks, buf.begin() + offset);
            offset += ks;

            std::copy(vs_ptr, vs_ptr + sizeof(uint32_t), buf.begin() + offset);
            offset += sizeof(uint32_t);

            std::copy(value.Data(), value.Data() + vs, buf.begin() + offset);
            offset += vs;
        }
        assert(off_old + serialized_length_ + sizeof(uint64_t) == offset);
        (void) off_old;
    }

    void Serialize(std::string &str) const override
    {
        size_t off_old = str.size();
        str.reserve(off_old + sizeof(uint64_t) + serialized_length_);

        int8_t obj_type_val = static_cast<int8_t>(RedisObjectType::TTLHash);
        str.append(1, obj_type_val);

        // serialize ttl_
        const char *ttl_ptr =
            static_cast<const char *>(static_cast<const void *>(&ttl_));
        str.append(ttl_ptr, sizeof(uint64_t));

        uint32_t cnt = static_cast<uint32_t>(hash_map_.size());
        const char *cnt_ptr = reinterpret_cast<const char *>(&cnt);
        str.append(cnt_ptr, sizeof(uint32_t));

        for (const auto &[key, value] : hash_map_)
        {
            uint32_t ks = key.Length(), vs = value.Length();
            const char *ks_ptr = reinterpret_cast<const char *>(&ks);
            const char *vs_ptr = reinterpret_cast<const char *>(&vs);
            str.append(ks_ptr, sizeof(uint32_t));
            str.append(key.Data(), ks);

            str.append(vs_ptr, sizeof(uint32_t));
            str.append(value.Data(), vs);
        }
        assert(serialized_length_ + off_old + sizeof(uint64_t) == str.size());
        (void) off_old;
    }

    void Deserialize(const char *buf, size_t &offset) override
    {
        size_t init_off = offset;
        // Check the object type.
        RedisObjectType obj_type =
            static_cast<RedisObjectType>(*(buf + offset));
        assert(obj_type == RedisObjectType::TTLHash);
        (void) obj_type;
        offset += 1;

        uint64_t *ttl_ptr = (uint64_t *) (buf + offset);
        ttl_ = *ttl_ptr;
        offset += sizeof(uint64_t);

        const uint32_t cnt = *reinterpret_cast<const uint32_t *>(buf + offset);
        offset += sizeof(uint32_t);

        for (uint32_t i = 0; i < cnt; i++)
        {
            const uint32_t ks =
                *reinterpret_cast<const uint32_t *>(buf + offset);
            offset += sizeof(uint32_t);
            EloqString key(buf + offset, ks);
            offset += ks;

            const uint32_t vs =
                *reinterpret_cast<const uint32_t *>(buf + offset);
            offset += sizeof(uint32_t);
            EloqString value(buf + offset, vs);
            offset += vs;

            SetField(key, value);
        }

        serialized_length_ = offset - init_off - sizeof(uint64_t);
    }

    TxRecord::Uptr Clone() const override
    {
        auto ptr = std::make_unique<RedisHashTTLObject>(*this);
        return ptr;
    }

    TxRecord::Uptr RemoveTTL() override
    {
        return std::make_unique<RedisHashObject>(std::move(*this));
    }

private:
    uint64_t ttl_{UINT64_MAX};
};
}  // namespace EloqKV
