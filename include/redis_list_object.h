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
#include <utility>
#include <vector>

#include "eloq_string.h"
#include "redis_command.h"
#include "redis_object.h"
#include "tx_command.h"

namespace EloqKV
{
class RedisListObject : public RedisEloqObject
{
public:
    RedisListObject();
    RedisListObject(const RedisListObject &);
    RedisListObject(RedisListObject &&rhs);
    ~RedisListObject() override = default;

    RedisObjectType ObjectType() const override
    {
        return RedisObjectType::List;
    }

    TxRecord::Uptr Clone() const override
    {
        return std::make_unique<RedisListObject>(*this);
    }

    size_t SerializedLength() const override
    {
        return serialized_length_;
    }

    void Serialize(std::vector<char> &buf, size_t &offset) const override
    {
        buf.resize(offset + serialized_length_);

        // to check that serialized_length_ is calculated correctly
        uint64_t serialized = 1 + sizeof(uint32_t);

        // Why do we need to serialize object types?
        // Because we only store the serialized bytes in kvstore, we need the
        // object type to specify which kind of redis object to create and
        // deserialize it when do read.
        int8_t obj_type = static_cast<int8_t>(RedisObjectType::List);
        std::copy(&obj_type, &obj_type + 1, buf.begin() + offset);
        offset += 1;

        uint32_t cnt = list_object_.size();
        const char *cnt_ptr = reinterpret_cast<const char *>(&cnt);
        std::copy(cnt_ptr, cnt_ptr + sizeof(cnt), buf.begin() + offset);
        offset += sizeof(cnt);

        for (auto &it : list_object_)
        {
            uint32_t size = it.Length();
            serialized += sizeof(uint32_t) + size;
            const char *size_ptr = reinterpret_cast<const char *>(&size);
            std::copy(size_ptr, size_ptr + sizeof(size), buf.begin() + offset);
            offset += sizeof(size);

            std::copy(it.Data(), it.Data() + size, buf.begin() + offset);
            offset += size;
        }
        assert(serialized_length_ == serialized);
    }

    void Serialize(std::string &str) const override
    {
        uint64_t old_len = str.size();
        str.reserve(old_len + serialized_length_);

        int8_t obj_type_val = static_cast<int8_t>(RedisObjectType::List);
        str.append(1, obj_type_val);

        uint32_t cnt = list_object_.size();
        const char *cnt_ptr = reinterpret_cast<const char *>(&cnt);
        str.append(cnt_ptr, sizeof(cnt));

        for (auto &it : list_object_)
        {
            uint32_t size = it.Length();
            const char *size_ptr = reinterpret_cast<const char *>(&size);
            str.append(size_ptr, sizeof(size));

            str.append(it.Data(), size);
        }
        assert(serialized_length_ == str.size() - old_len);
        (void) old_len;
    }

    void Deserialize(const char *buf, size_t &offset) override
    {
        serialized_length_ = 1 + sizeof(uint32_t);
        // Check the object type.
        RedisObjectType obj_type =
            static_cast<RedisObjectType>(*(buf + offset));
        assert(obj_type == RedisObjectType::List);
        (void) obj_type;
        offset += 1;

        uint32_t cnt = *reinterpret_cast<const uint32_t *>(buf + offset);
        offset += sizeof(cnt);

        for (uint32_t i = 0; i < cnt; i++)
        {
            uint32_t size = *reinterpret_cast<const uint32_t *>(buf + offset);
            offset += sizeof(size);
            serialized_length_ += sizeof(uint32_t) + size;

            list_object_.emplace_back(buf + offset, size);
            offset += size;
        }
    }

    // The equal here refers to whether the objects pointed to are consistent.
    // It is possible that one is an owned object and the other is a reference
    // object, but as long as the objects they point to are consistent, then
    // they are consistent under the current semantics.
    bool operator==(const RedisListObject &rhs) const
    {
        if (rhs.list_object_.size() != list_object_.size())
        {
            return false;
        }

        for (uint32_t i = 1; i < list_object_.size(); i++)
        {
            if (rhs.list_object_[i].StringView() !=
                list_object_[i].StringView())
            {
                return false;
            }
        }

        return true;
    }

    bool Empty() const
    {
        return list_object_.empty();
    }

    size_t Size() const
    {
        return list_object_.size();
    }

    txservice::TxRecord::Uptr AddTTL(uint64_t ttl) override;

    bool Execute(RPushCommand &cmd) const;
    bool Execute(LPushCommand &cmd) const;
    void Execute(LRangeCommand &cmd) const;
    void Execute(LPopCommand &cmd) const;
    void Execute(RPopCommand &cmd) const;
    void Execute(LLenCommand &cmd) const;
    void Execute(LTrimCommand &cmd) const;
    void Execute(LIndexCommand &cmd) const;
    bool Execute(LInsertCommand &cmd) const;
    void Execute(LPosCommand &cmd) const;
    bool Execute(LSetCommand &cmd) const;
    bool Execute(LRemCommand &cmd) const;
    bool Execute(LPushXCommand &cmd) const;
    bool Execute(RPushXCommand &cmd) const;
    bool Execute(LMovePopCommand &cmd) const;
    bool Execute(LMovePushCommand &cmd) const;
    void Execute(SortableLoadCommand &cmd) const;
    /**
     * @brief Pop an element into result and return true if exist, or return
     * false
     */
    bool Execute(BlockLPopCommand &cmd) const;

    void CommitLInsert(bool is_before, EloqString &pivot, EloqString &element);
    void CommitLSet(int64_t index, EloqString &element);
    void CommitRPush(std::vector<EloqString> &elements);
    void CommitLPush(std::vector<EloqString> &elements);
    bool CommitLPop(int64_t count);
    bool CommitRPop(int64_t count);
    bool CommitLTrim(int64_t start, int64_t end);
    bool CommitLRem(int64_t count, EloqString &element);
    bool CommitLMovePop(bool is_left);
    void CommitLMovePush(bool is_left,
                         EloqString &element,
                         bool should_not_move_string);
    bool CommitBlockPop(bool is_left, uint32_t count);

    const std::deque<EloqString> &Elements() const
    {
        return list_object_;
    }

protected:
    bool ConvertListIndex(int64_t &index) const;

    std::deque<EloqString> list_object_;

    // The estimated memory size of the object. Limit the objects size so that
    // the persistent storage flush wouldn't fail.
    uint64_t serialized_length_{};
};

struct RedisListTTLObject : public RedisListObject
{
    RedisListTTLObject() : RedisListObject(), ttl_(UINT64_MAX)
    {
    }

    RedisListTTLObject(const RedisListTTLObject &other)
        : RedisListObject(other), ttl_(other.ttl_)
    {
    }

    RedisListTTLObject(const RedisListObject &&other, uint64_t ttl)
        : RedisListObject(std::move(other)), ttl_(ttl)
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
        return RedisObjectType::List;
    }

    size_t SerializedLength() const override
    {
        return sizeof(uint64_t) + RedisListObject::SerializedLength();
    }

    void Serialize(std::vector<char> &buf, size_t &offset) const override
    {
        buf.resize(offset + sizeof(uint64_t) + serialized_length_);

        // to check that serialized_length_ is calculated correctly
        uint64_t serialized = 1 + sizeof(uint32_t) + sizeof(uint64_t);

        // Why do we need to serialize object types?
        // Because we only store the serialized bytes in kvstore, we need the
        // object type to specify which kind of redis object to create and
        // deserialize it when do read.
        int8_t obj_type = static_cast<int8_t>(RedisObjectType::TTLList);
        std::copy(&obj_type, &obj_type + 1, buf.begin() + offset);
        offset += 1;

        // serialize ttl_
        std::copy(&ttl_, &ttl_ + sizeof(uint64_t), buf.begin() + offset);
        offset += sizeof(uint64_t);

        uint32_t cnt = list_object_.size();
        const char *cnt_ptr = reinterpret_cast<const char *>(&cnt);
        std::copy(cnt_ptr, cnt_ptr + sizeof(cnt), buf.begin() + offset);
        offset += sizeof(cnt);

        for (auto &it : list_object_)
        {
            uint32_t size = it.Length();
            serialized += sizeof(uint32_t) + size;
            const char *size_ptr = reinterpret_cast<const char *>(&size);
            std::copy(size_ptr, size_ptr + sizeof(size), buf.begin() + offset);
            offset += sizeof(size);

            std::copy(it.Data(), it.Data() + size, buf.begin() + offset);
            offset += size;
        }
        assert(SerializedLength() == serialized);
    }

    void Serialize(std::string &str) const override
    {
        uint64_t old_len = str.size();
        str.reserve(old_len + SerializedLength());

        int8_t obj_type_val = static_cast<int8_t>(RedisObjectType::TTLList);
        str.append(1, obj_type_val);

        // serialize ttl_
        const char *ttl_ptr =
            static_cast<const char *>(static_cast<const void *>(&ttl_));
        str.append(ttl_ptr, sizeof(uint64_t));

        uint32_t cnt = list_object_.size();
        const char *cnt_ptr = reinterpret_cast<const char *>(&cnt);
        str.append(cnt_ptr, sizeof(cnt));

        for (auto &it : list_object_)
        {
            uint32_t size = it.Length();
            const char *size_ptr = reinterpret_cast<const char *>(&size);
            str.append(size_ptr, sizeof(size));

            str.append(it.Data(), size);
        }

        assert(SerializedLength() == str.size() - old_len);
        (void) old_len;
    }

    void Deserialize(const char *buf, size_t &offset) override
    {
        serialized_length_ = 1 + sizeof(uint32_t);
        // Check the object type.
        RedisObjectType obj_type =
            static_cast<RedisObjectType>(*(buf + offset));
        assert(obj_type == RedisObjectType::TTLList);
        (void) obj_type;
        offset += 1;

        uint64_t *ttl_ptr = (uint64_t *) (buf + offset);
        ttl_ = *ttl_ptr;
        offset += sizeof(uint64_t);

        uint32_t cnt = *reinterpret_cast<const uint32_t *>(buf + offset);
        offset += sizeof(cnt);

        for (uint32_t i = 0; i < cnt; i++)
        {
            uint32_t size = *reinterpret_cast<const uint32_t *>(buf + offset);
            offset += sizeof(size);
            serialized_length_ += sizeof(uint32_t) + size;

            list_object_.emplace_back(buf + offset, size);
            offset += size;
        }
    }

    TxRecord::Uptr Clone() const override
    {
        auto ptr = std::make_unique<RedisListTTLObject>(*this);
        return ptr;
    }

    TxRecord::Uptr RemoveTTL() override
    {
        return std::make_unique<RedisListObject>(std::move(*this));
    }

private:
    uint64_t ttl_{UINT64_MAX};
};

}  // namespace EloqKV
