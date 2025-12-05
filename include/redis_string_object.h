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
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "eloq_string.h"
#include "local_cc_shards.h"
#include "redis_command.h"
#include "redis_object.h"

namespace EloqKV
{
extern const uint64_t MAX_OBJECT_SIZE;

struct RedisStringObject : public RedisEloqObject
{
public:
    RedisStringObject() = default;

    RedisStringObject(std::string_view str_view) : str_obj_(str_view)
    {
    }

    RedisStringObject(const char *data, size_t length) : str_obj_(data, length)
    {
    }

    RedisStringObject(const RedisStringObject &rhs)
        : RedisEloqObject(rhs), str_obj_(rhs.str_obj_)
    {
    }

    RedisStringObject(RedisStringObject &&rhs) = default;
    ~RedisStringObject() override = default;

    size_t SerializedLength() const override
    {
        return 1 + sizeof(uint32_t) + str_obj_.Length();
    }

    static bool CheckSerializedLength(size_t new_string_size)
    {
        uint64_t new_len = 1 + sizeof(uint32_t) + new_string_size;
        return new_len <= MAX_OBJECT_SIZE;
    }

    void Serialize(std::vector<char> &buf, size_t &offset) const override
    {
        // object type (1 byte), string length (4 bytes), string content (string
        // length bytes)

        std::string_view str_view = str_obj_.StringView();

        // Why do we need to serialize object types?
        // Because we only store the serialized bytes in kvstore, we need the
        // object type to specify which kind of redis object to create and
        // deserialize it when do read.
        int8_t obj_type = static_cast<int8_t>(RedisObjectType::String);

        uint32_t blob_len = str_view.size();
        buf.resize(offset + sizeof(int8_t) + sizeof(uint32_t) + blob_len);

        std::copy(&obj_type, &obj_type + 1, buf.begin() + offset);
        offset += 1;

        const unsigned char *val_ptr = static_cast<const unsigned char *>(
            static_cast<const void *>(&blob_len));
        std::copy(val_ptr, val_ptr + sizeof(uint32_t), buf.begin() + offset);
        offset += sizeof(uint32_t);

        std::copy(str_view.begin(), str_view.end(), buf.begin() + offset);
        offset += blob_len;
    }

    void Serialize(std::string &str) const override
    {
        // object type (1 byte), string length (4 bytes), string content (string
        // length bytes)

        // Why do we need to serialize object types?
        // Because we only store the serialized bytes in kvstore, we need the
        // object type to specify which kind of redis object to create and
        // deserialize it when do read.

        int8_t obj_type_val = static_cast<int8_t>(RedisObjectType::String);
        str.append(sizeof(int8_t), obj_type_val);

        std::string_view str_view = str_obj_.StringView();

        uint32_t blob_len = str_view.size();

        const char *blob_len_ptr =
            static_cast<const char *>(static_cast<const void *>(&blob_len));

        str.append(blob_len_ptr, sizeof(uint32_t));
        str.append(str_view.data(), blob_len);
    }

    void Deserialize(const char *buf, size_t &offset) override
    {
        // object type (1 byte), string length (4 bytes), string content (string
        // length bytes)

        // Check the object type.
        RedisObjectType obj_type =
            static_cast<RedisObjectType>(*(buf + offset));
        assert(obj_type == RedisObjectType::String);
        (void) obj_type;
        offset += 1;

        const uint32_t *len_ptr =
            reinterpret_cast<const uint32_t *>(buf + offset);
        uint32_t len = *len_ptr;
        offset += sizeof(uint32_t);

        str_obj_ = EloqString(buf + offset, len);

        offset += len;
    }

    TxRecord::Uptr Clone() const override
    {
        auto ptr = std::make_unique<RedisStringObject>(*this);
        return ptr;
    }

    void Copy(const TxRecord &rhs) override
    {
        const RedisStringObject &typed_rhs =
            static_cast<const RedisStringObject &>(rhs);

        str_obj_ = typed_rhs.str_obj_;
    }

    std::string ToString() const override
    {
        return std::string(str_obj_.StringView());
    }

    RedisStringObject &operator=(const RedisStringObject &rhs)
    {
        if (this != &rhs)
        {
            str_obj_ = rhs.str_obj_;
        }
        return *this;
    }

    RedisStringObject &operator=(const RedisStringObject &&other) noexcept
    {
        if (this != &other)
        {
            str_obj_ = std::move(other.str_obj_);
        }
        return *this;
    }

    size_t Length() const
    {
        return str_obj_.StringView().size();
    }

    size_t MemUsage() const override
    {
        std::string_view str_view = str_obj_.StringView();

        if (str_view.size() <= 16)
        {
            return sizeof(EloqString);
        }
        else
        {
            return sizeof(EloqString) + str_view.size();
        }
    }

    RedisObjectType ObjectType() const override
    {
        return RedisObjectType::String;
    }

    std::string_view StringView() const
    {
        return str_obj_.StringView();
    }

    bool NeedsDefrag(mi_heap_t *heap) override;
    TxRecord::Uptr AddTTL(uint64_t ttl) override;

    void Execute(GetCommand &cmd) const;
    void Execute(GetDelCommand &cmd) const;
    bool Execute(IntOpCommand &cmd) const;
    bool Execute(FloatOpCommand &cmd) const;
    void Execute(StrLenCommand &cmd) const;
    void Execute(GetBitCommand &cmd) const;
    void Execute(GetRangeCommand &cmd) const;
    bool Execute(SetBitCommand &cmd) const;
    bool Execute(SetRangeCommand &cmd) const;
    bool Execute(AppendCommand &cmd) const;
    void Execute(BitCountCommand &cmd) const;
    bool Execute(BitFieldCommand &cmd) const;
    bool Execute(BitPosCommand &cmd) const;

    void CommitSet(EloqString &val);
    void CommitIncrDecr(int64_t incr);
    void CommitFloatIncr(double incr);
    void CommitSetBit(int64_t offset, int8_t val);
    void CommitSetRange(int64_t offset, EloqString &val);
    void CommitAppend(EloqString &val);
    void CommitBitField(std::vector<BitFieldCommand::SubCommand> &vct);

protected:
    static int64_t GetBitFieldValue(const uint8_t *p,
                                    int64_t offset,
                                    int64_t enc_bits,
                                    bool bsign);
    static void SetBitFieldValue(
        uint8_t *p, int64_t val, int64_t offset, int64_t enc_bits, bool bsign);
    static bool IsBitfieldUnoverflow(int64_t value,
                                     int64_t incr,
                                     int64_t enc_bits,
                                     bool bsign,
                                     BitFieldCommand::Overflow owtype,
                                     int64_t &limit);

protected:
    EloqString str_obj_;
};

struct RedisStringTTLObject : public RedisStringObject
{
public:
    RedisStringTTLObject() : RedisStringObject(), ttl_(UINT64_MAX)
    {
    }

    RedisStringTTLObject(const RedisStringTTLObject &other)
        : RedisStringObject(other), ttl_(other.ttl_)
    {
    }

    RedisStringTTLObject(RedisStringObject &&other, uint64_t ttl)
        : RedisStringObject(std::move(other)), ttl_(ttl)
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
        return RedisObjectType::String;
    }

    size_t SerializedLength() const override
    {
        return sizeof(size_t) + RedisStringObject::SerializedLength();
    }

    void Serialize(std::vector<char> &buf, size_t &offset) const override
    {
        std::string_view str_view = str_obj_.StringView();

        size_t blob_len = str_view.size();
        // offset + 1byte(obj type) + 8byte(ttl) + 4byte(len of blob) +
        // $(len)byte(blob)
        buf.resize(offset + sizeof(int8_t) + sizeof(uint64_t) +
                   sizeof(uint32_t) + blob_len);

        // Why do we need to serialize object types?
        // Because we only store the serialized bytes in kvstore, we need the
        // object type to specify which kind of redis object to create and
        // deserialize it when do read.
        int8_t obj_type = static_cast<int8_t>(RedisObjectType::TTLString);
        std::copy(&obj_type, &obj_type + 1, buf.begin() + offset);
        offset += 1;

        // serialize ttl_
        const char *ttl_ptr = reinterpret_cast<const char *>(&ttl_);
        std::copy(ttl_ptr, ttl_ptr + sizeof(uint64_t), buf.begin() + offset);
        offset += sizeof(uint64_t);

        const unsigned char *val_ptr = static_cast<const unsigned char *>(
            static_cast<const void *>(&blob_len));
        std::copy(val_ptr, val_ptr + sizeof(uint32_t), buf.begin() + offset);
        offset += sizeof(uint32_t);

        std::copy(str_view.begin(), str_view.end(), buf.begin() + offset);
        offset += blob_len;
    }

    void Serialize(std::string &str) const override
    {
        int8_t obj_type_val = static_cast<int8_t>(RedisObjectType::TTLString);
        str.append(1, obj_type_val);

        // serialize ttl_
        const char *ttl_ptr =
            static_cast<const char *>(static_cast<const void *>(&ttl_));
        str.append(ttl_ptr, sizeof(uint64_t));

        std::string_view str_view = str_obj_.StringView();

        uint32_t blob_len = str_view.size();

        const char *blob_len_ptr =
            static_cast<const char *>(static_cast<const void *>(&blob_len));

        str.append(blob_len_ptr, sizeof(uint32_t));
        str.append(str_view.data(), blob_len);
    }

    void Deserialize(const char *buf, size_t &offset) override
    {
        // Check the object type.
        RedisObjectType obj_type =
            static_cast<RedisObjectType>(*(buf + offset));
        assert(obj_type == RedisObjectType::TTLString);
        (void) obj_type;
        offset += 1;

        uint64_t *ttl_ptr = (uint64_t *) (buf + offset);
        ttl_ = *ttl_ptr;
        offset += sizeof(uint64_t);

        uint32_t *len_ptr = (uint32_t *) (buf + offset);
        uint32_t len = *len_ptr;
        offset += sizeof(uint32_t);

        str_obj_ = EloqString(buf + offset, len);

        offset += len;
    }

    TxRecord::Uptr Clone() const override
    {
        auto ptr = std::make_unique<RedisStringTTLObject>(*this);
        return ptr;
    }

    void Copy(const TxRecord &rhs) override
    {
        const RedisStringTTLObject &typed_rhs =
            static_cast<const RedisStringTTLObject &>(rhs);

        str_obj_ = typed_rhs.str_obj_;
        ttl_ = typed_rhs.ttl_;
    }

    TxRecord::Uptr RemoveTTL() override
    {
        return std::make_unique<RedisStringObject>(std::move(*this));
    }

private:
    uint64_t ttl_{UINT64_MAX};
};
}  // namespace EloqKV
