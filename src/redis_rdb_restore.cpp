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
/*
 * Portions of the Redis RDB RESTORE compatibility logic in this file are
 * derived from or informed by Redis 7.2.9 source code, which is licensed
 * under the BSD 3-Clause License.
 *
 * See:
 *   third_party/licenses/redis-7.2.9-BSD-3-Clause.txt
 *   third_party/licenses/lzf-bsd-or-gpl.txt
 */
#include "redis_rdb_restore.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "eloq_string.h"
#include "redis_hash_object.h"
#include "redis_list_object.h"
#include "redis_set_object.h"
#include "redis_string_num.h"
#include "redis_string_object.h"
#include "redis_zset_object.h"

namespace EloqKV
{
namespace
{
constexpr uint8_t kRdbTypeString = 0;
constexpr uint8_t kRdbTypeList = 1;
constexpr uint8_t kRdbTypeSet = 2;
constexpr uint8_t kRdbTypeZSet = 3;
constexpr uint8_t kRdbTypeHash = 4;
constexpr uint8_t kRdbTypeZSet2 = 5;
constexpr uint8_t kRdbTypeListZipList = 10;
constexpr uint8_t kRdbTypeSetIntSet = 11;
constexpr uint8_t kRdbTypeZSetZipList = 12;
constexpr uint8_t kRdbTypeHashZipList = 13;
constexpr uint8_t kRdbTypeQuickList = 14;
constexpr uint8_t kRdbTypeHashListPack = 16;
constexpr uint8_t kRdbTypeZSetListPack = 17;
constexpr uint8_t kRdbTypeQuickList2 = 18;
constexpr uint8_t kRdbTypeSetListPack = 20;

constexpr uint8_t kQuickListNodeContainerPlain = 1;
constexpr uint8_t kQuickListNodeContainerPacked = 2;
constexpr size_t kMaxLzfDecodedLength = 64ULL * 1024ULL * 1024ULL;
constexpr size_t kMaxCollectionEntries = 1ULL << 20;

void PrefixEloqOuterType(RedisObjectType type, std::string &payload)
{
    uint8_t obj_type = static_cast<uint8_t>(type);
    payload.append(reinterpret_cast<const char *>(&obj_type), sizeof(obj_type));
}

bool LzfDecompress(std::string_view compressed,
                   size_t expected_len,
                   std::string &output);

bool ValidateCount(uint64_t count,
                   size_t min_bytes_per_item,
                   size_t remaining,
                   size_t hard_cap = kMaxCollectionEntries)
{
    if (count > hard_cap)
    {
        return false;
    }

    if (min_bytes_per_item == 0)
    {
        return true;
    }

    return count <= remaining / min_bytes_per_item;
}

class Reader
{
public:
    explicit Reader(std::string_view data)
        : ptr_(reinterpret_cast<const uint8_t *>(data.data())),
          end_(ptr_ + data.size())
    {
    }

    bool ReadByte(uint8_t &value)
    {
        if (ptr_ >= end_)
        {
            return false;
        }
        value = *ptr_++;
        return true;
    }

    bool ReadBytes(size_t len, std::string_view &value)
    {
        if (Remaining() < len)
        {
            return false;
        }
        value = std::string_view(reinterpret_cast<const char *>(ptr_), len);
        ptr_ += len;
        return true;
    }

    bool ReadLen(uint64_t &len, bool &is_encoded)
    {
        uint8_t first = 0;
        if (!ReadByte(first))
        {
            return false;
        }

        uint8_t type = (first & 0xC0) >> 6;
        if (type == 0)
        {
            len = first & 0x3F;
            is_encoded = false;
            return true;
        }
        if (type == 1)
        {
            uint8_t next = 0;
            if (!ReadByte(next))
            {
                return false;
            }
            len = (static_cast<uint64_t>(first & 0x3F) << 8) | next;
            is_encoded = false;
            return true;
        }
        if (type == 2)
        {
            is_encoded = false;
            if (first == 0x80)
            {
                uint32_t be32 = 0;
                if (!ReadBE32(be32))
                {
                    return false;
                }
                len = be32;
                return true;
            }
            if (first == 0x81)
            {
                uint64_t be64 = 0;
                if (!ReadBE64(be64))
                {
                    return false;
                }
                len = be64;
                return true;
            }
            return false;
        }

        len = first & 0x3F;
        is_encoded = true;
        return true;
    }

    bool ReadString(std::string &value)
    {
        uint64_t len = 0;
        bool is_encoded = false;
        if (!ReadLen(len, is_encoded))
        {
            return false;
        }

        if (!is_encoded)
        {
            std::string_view bytes;
            if (!ReadBytes(static_cast<size_t>(len), bytes))
            {
                return false;
            }
            value.assign(bytes.data(), bytes.size());
            return true;
        }

        switch (len)
        {
        case 0:
        {
            uint8_t n = 0;
            if (!ReadByte(n))
            {
                return false;
            }
            value = std::to_string(static_cast<int8_t>(n));
            return true;
        }
        case 1:
        {
            int16_t n = 0;
            if (!ReadLE(n))
            {
                return false;
            }
            value = std::to_string(n);
            return true;
        }
        case 2:
        {
            int32_t n = 0;
            if (!ReadLE(n))
            {
                return false;
            }
            value = std::to_string(n);
            return true;
        }
        case 3:
        {
            uint64_t compressed_len = 0;
            uint64_t original_len = 0;
            bool encoded_len = false;
            if (!ReadLen(compressed_len, encoded_len) || encoded_len)
            {
                return false;
            }
            if (!ReadLen(original_len, encoded_len) || encoded_len)
            {
                return false;
            }
            std::string_view bytes;
            if (!ReadBytes(static_cast<size_t>(compressed_len), bytes))
            {
                return false;
            }
            return LzfDecompress(
                bytes, static_cast<size_t>(original_len), value);
        }
        default:
            return false;
        }
    }

    bool ReadZSetScore(double &score)
    {
        uint8_t len = 0;
        if (!ReadByte(len))
        {
            return false;
        }
        if (len == 253)
        {
            score = std::numeric_limits<double>::quiet_NaN();
            return true;
        }
        if (len == 254)
        {
            score = std::numeric_limits<double>::infinity();
            return true;
        }
        if (len == 255)
        {
            score = -std::numeric_limits<double>::infinity();
            return true;
        }

        std::string_view bytes;
        if (!ReadBytes(len, bytes))
        {
            return false;
        }

        long double value = 0;
        if (!string2ld(bytes.data(), bytes.size(), value))
        {
            return false;
        }
        score = static_cast<double>(value);
        return true;
    }

    bool ReadBinaryDouble(double &score)
    {
        return ReadLE(score);
    }

    size_t Remaining() const
    {
        return static_cast<size_t>(end_ - ptr_);
    }

private:
    template <typename T>
    bool ReadLE(T &value)
    {
        if (Remaining() < sizeof(T))
        {
            return false;
        }
        std::memcpy(&value, ptr_, sizeof(T));
        ptr_ += sizeof(T);
        return true;
    }

    bool ReadBE32(uint32_t &value)
    {
        if (Remaining() < sizeof(uint32_t))
        {
            return false;
        }
        value = (static_cast<uint32_t>(ptr_[0]) << 24) |
                (static_cast<uint32_t>(ptr_[1]) << 16) |
                (static_cast<uint32_t>(ptr_[2]) << 8) | ptr_[3];
        ptr_ += sizeof(uint32_t);
        return true;
    }

    bool ReadBE64(uint64_t &value)
    {
        if (Remaining() < sizeof(uint64_t))
        {
            return false;
        }
        value = (static_cast<uint64_t>(ptr_[0]) << 56) |
                (static_cast<uint64_t>(ptr_[1]) << 48) |
                (static_cast<uint64_t>(ptr_[2]) << 40) |
                (static_cast<uint64_t>(ptr_[3]) << 32) |
                (static_cast<uint64_t>(ptr_[4]) << 24) |
                (static_cast<uint64_t>(ptr_[5]) << 16) |
                (static_cast<uint64_t>(ptr_[6]) << 8) | ptr_[7];
        ptr_ += sizeof(uint64_t);
        return true;
    }

    const uint8_t *ptr_;
    const uint8_t *end_;
};

int64_t SignExtend(uint64_t value, unsigned bits)
{
    uint64_t sign_bit = 1ULL << (bits - 1);
    uint64_t mask = (1ULL << bits) - 1;
    value &= mask;
    return (value ^ sign_bit) - sign_bit;
}

bool DecodeBackLen(const uint8_t *ptr,
                   const uint8_t *end,
                   size_t &bytes,
                   size_t &value)
{
    bytes = 0;
    value = 0;
    uint32_t shift = 0;
    while (true)
    {
        if (ptr + bytes >= end || shift >= sizeof(size_t) * 8)
        {
            return false;
        }
        uint8_t cur = ptr[bytes];
        value |= static_cast<size_t>(cur & 0x7FU) << shift;
        bytes++;
        if ((cur & 0x80U) == 0)
        {
            return true;
        }
        shift += 7;
    }
}

bool ReadListPackEntry(const uint8_t *&ptr,
                       const uint8_t *end,
                       std::string &value)
{
    if (ptr >= end || *ptr == 0xFF)
    {
        return false;
    }

    const uint8_t *entry_start = ptr;
    uint8_t first = *ptr++;
    if ((first & 0x80U) == 0)
    {
        value = std::to_string(first & 0x7FU);
    }
    else if ((first & 0xC0U) == 0x80U)
    {
        size_t len = first & 0x3FU;
        if (static_cast<size_t>(end - ptr) < len)
        {
            return false;
        }
        value.assign(reinterpret_cast<const char *>(ptr), len);
        ptr += len;
    }
    else if ((first & 0xE0U) == 0xC0U)
    {
        if (ptr >= end)
        {
            return false;
        }
        uint64_t raw = (static_cast<uint64_t>(first & 0x1FU) << 8) | *ptr++;
        value = std::to_string(SignExtend(raw, 13));
    }
    else if ((first & 0xF0U) == 0xE0U)
    {
        if (ptr >= end)
        {
            return false;
        }
        size_t len = (static_cast<size_t>(first & 0x0FU) << 8) | *ptr++;
        if (static_cast<size_t>(end - ptr) < len)
        {
            return false;
        }
        value.assign(reinterpret_cast<const char *>(ptr), len);
        ptr += len;
    }
    else
    {
        switch (first)
        {
        case 0xF0:
        {
            if (static_cast<size_t>(end - ptr) < 4)
            {
                return false;
            }
            uint32_t len = 0;
            std::memcpy(&len, ptr, sizeof(len));
            ptr += sizeof(len);
            if (static_cast<size_t>(end - ptr) < len)
            {
                return false;
            }
            value.assign(reinterpret_cast<const char *>(ptr), len);
            ptr += len;
            break;
        }
        case 0xF1:
        {
            if (static_cast<size_t>(end - ptr) < 2)
            {
                return false;
            }
            int16_t n = 0;
            std::memcpy(&n, ptr, sizeof(n));
            ptr += sizeof(n);
            value = std::to_string(n);
            break;
        }
        case 0xF2:
        {
            if (static_cast<size_t>(end - ptr) < 3)
            {
                return false;
            }
            uint32_t raw = static_cast<uint32_t>(ptr[0]) |
                           (static_cast<uint32_t>(ptr[1]) << 8) |
                           (static_cast<uint32_t>(ptr[2]) << 16);
            ptr += 3;
            value = std::to_string(SignExtend(raw, 24));
            break;
        }
        case 0xF3:
        {
            if (static_cast<size_t>(end - ptr) < 4)
            {
                return false;
            }
            int32_t n = 0;
            std::memcpy(&n, ptr, sizeof(n));
            ptr += sizeof(n);
            value = std::to_string(n);
            break;
        }
        case 0xF4:
        {
            if (static_cast<size_t>(end - ptr) < 8)
            {
                return false;
            }
            int64_t n = 0;
            std::memcpy(&n, ptr, sizeof(n));
            ptr += sizeof(n);
            value = std::to_string(n);
            break;
        }
        default:
            return false;
        }
    }

    size_t data_len = static_cast<size_t>(ptr - entry_start);
    size_t back_len_bytes = 0;
    size_t back_len_value = 0;
    if (!DecodeBackLen(ptr, end, back_len_bytes, back_len_value) ||
        back_len_value != data_len)
    {
        return false;
    }
    ptr += back_len_bytes;
    return true;
}

bool ReadListPackAll(std::string_view blob, std::vector<std::string> &entries)
{
    if (blob.size() < 7)
    {
        return false;
    }

    const uint8_t *ptr = reinterpret_cast<const uint8_t *>(blob.data());
    uint32_t total_bytes = 0;
    uint16_t count = 0;
    std::memcpy(&total_bytes, ptr, sizeof(total_bytes));
    std::memcpy(&count, ptr + sizeof(total_bytes), sizeof(count));
    if (total_bytes != blob.size())
    {
        return false;
    }

    ptr += sizeof(total_bytes) + sizeof(count);
    const uint8_t *end =
        reinterpret_cast<const uint8_t *>(blob.data()) + blob.size();
    while (ptr < end && *ptr != 0xFF)
    {
        const uint8_t *before = ptr;
        std::string value;
        if (!ReadListPackEntry(ptr, end, value) || ptr <= before)
        {
            return false;
        }
        entries.emplace_back(std::move(value));
    }

    if (ptr >= end || *ptr != 0xFF)
    {
        return false;
    }
    ptr++;

    if (ptr != end)
    {
        return false;
    }

    if (count != 0xFFFF && count != entries.size())
    {
        return false;
    }

    return true;
}

bool ParseIntSetBlob(std::string_view blob, std::vector<std::string> &entries)
{
    if (blob.size() < 8)
    {
        return false;
    }

    const uint8_t *ptr = reinterpret_cast<const uint8_t *>(blob.data());
    uint32_t encoding = 0;
    uint32_t count = 0;
    std::memcpy(&encoding, ptr, sizeof(encoding));
    std::memcpy(&count, ptr + sizeof(encoding), sizeof(count));
    ptr += sizeof(encoding) + sizeof(count);

    size_t bytes_per_entry = encoding;
    if (bytes_per_entry != 2 && bytes_per_entry != 4 && bytes_per_entry != 8)
    {
        return false;
    }
    if (blob.size() != 8 + static_cast<size_t>(count) * bytes_per_entry)
    {
        return false;
    }

    entries.reserve(count);
    for (uint32_t i = 0; i < count; i++)
    {
        if (bytes_per_entry == 2)
        {
            int16_t value = 0;
            std::memcpy(&value, ptr, sizeof(value));
            ptr += sizeof(value);
            entries.emplace_back(std::to_string(value));
        }
        else if (bytes_per_entry == 4)
        {
            int32_t value = 0;
            std::memcpy(&value, ptr, sizeof(value));
            ptr += sizeof(value);
            entries.emplace_back(std::to_string(value));
        }
        else
        {
            int64_t value = 0;
            std::memcpy(&value, ptr, sizeof(value));
            ptr += sizeof(value);
            entries.emplace_back(std::to_string(value));
        }
    }

    return true;
}

uint32_t ReadBE32(const uint8_t *ptr)
{
    return (static_cast<uint32_t>(ptr[0]) << 24) |
           (static_cast<uint32_t>(ptr[1]) << 16) |
           (static_cast<uint32_t>(ptr[2]) << 8) | ptr[3];
}

bool ReadZipListPrevLen(const uint8_t *&ptr, const uint8_t *end)
{
    if (ptr >= end)
    {
        return false;
    }
    uint8_t first = *ptr++;
    if (first < 254)
    {
        return true;
    }
    if (first != 254 || static_cast<size_t>(end - ptr) < 4)
    {
        return false;
    }
    ptr += 4;
    return true;
}

bool ReadZipListEntry(const uint8_t *&ptr,
                      const uint8_t *end,
                      std::string &value)
{
    if (!ReadZipListPrevLen(ptr, end) || ptr >= end)
    {
        return false;
    }

    uint8_t encoding = *ptr++;
    if ((encoding & 0xC0U) == 0x00U)
    {
        size_t len = encoding & 0x3FU;
        if (static_cast<size_t>(end - ptr) < len)
        {
            return false;
        }
        value.assign(reinterpret_cast<const char *>(ptr), len);
        ptr += len;
        return true;
    }
    if ((encoding & 0xC0U) == 0x40U)
    {
        if (ptr >= end)
        {
            return false;
        }
        size_t len = (static_cast<size_t>(encoding & 0x3FU) << 8) | *ptr++;
        if (static_cast<size_t>(end - ptr) < len)
        {
            return false;
        }
        value.assign(reinterpret_cast<const char *>(ptr), len);
        ptr += len;
        return true;
    }
    if (encoding == 0x80U)
    {
        if (static_cast<size_t>(end - ptr) < 4)
        {
            return false;
        }
        uint32_t len = ReadBE32(ptr);
        ptr += 4;
        if (static_cast<size_t>(end - ptr) < len)
        {
            return false;
        }
        value.assign(reinterpret_cast<const char *>(ptr), len);
        ptr += len;
        return true;
    }

    switch (encoding)
    {
    case 0xC0:
    {
        if (static_cast<size_t>(end - ptr) < 2)
        {
            return false;
        }
        int16_t n = 0;
        std::memcpy(&n, ptr, sizeof(n));
        ptr += sizeof(n);
        value = std::to_string(n);
        return true;
    }
    case 0xD0:
    {
        if (static_cast<size_t>(end - ptr) < 4)
        {
            return false;
        }
        int32_t n = 0;
        std::memcpy(&n, ptr, sizeof(n));
        ptr += sizeof(n);
        value = std::to_string(n);
        return true;
    }
    case 0xE0:
    {
        if (static_cast<size_t>(end - ptr) < 8)
        {
            return false;
        }
        int64_t n = 0;
        std::memcpy(&n, ptr, sizeof(n));
        ptr += sizeof(n);
        value = std::to_string(n);
        return true;
    }
    case 0xF0:
    {
        if (static_cast<size_t>(end - ptr) < 3)
        {
            return false;
        }
        uint32_t raw = static_cast<uint32_t>(ptr[0]) |
                       (static_cast<uint32_t>(ptr[1]) << 8) |
                       (static_cast<uint32_t>(ptr[2]) << 16);
        ptr += 3;
        value = std::to_string(SignExtend(raw, 24));
        return true;
    }
    case 0xFE:
    {
        if (ptr >= end)
        {
            return false;
        }
        int8_t n = static_cast<int8_t>(*ptr++);
        value = std::to_string(n);
        return true;
    }
    default:
        break;
    }

    if ((encoding & 0xF0U) == 0xF0U && encoding != 0xFFU)
    {
        value = std::to_string(static_cast<int>(encoding & 0x0FU) - 1);
        return true;
    }

    return false;
}

bool ReadZipListAll(std::string_view blob, std::vector<std::string> &entries)
{
    if (blob.size() < 11)
    {
        return false;
    }

    const uint8_t *begin = reinterpret_cast<const uint8_t *>(blob.data());
    uint32_t total_bytes = 0;
    std::memcpy(&total_bytes, begin, sizeof(total_bytes));
    if (total_bytes != blob.size())
    {
        return false;
    }

    const uint8_t *ptr = begin + 10;
    const uint8_t *end = begin + blob.size();
    while (ptr < end && *ptr != 0xFF)
    {
        const uint8_t *before = ptr;
        std::string value;
        if (!ReadZipListEntry(ptr, end, value) || ptr <= before)
        {
            return false;
        }
        entries.emplace_back(std::move(value));
    }

    if (ptr >= end || *ptr != 0xFF)
    {
        return false;
    }
    ptr++;
    return ptr == end;
}

bool LzfDecompress(std::string_view compressed,
                   size_t expected_len,
                   std::string &output)
{
    if (expected_len > kMaxLzfDecodedLength)
    {
        return false;
    }

    output.clear();
    output.resize(expected_len);

    const uint8_t *in = reinterpret_cast<const uint8_t *>(compressed.data());
    const uint8_t *in_end = in + compressed.size();
    uint8_t *out = reinterpret_cast<uint8_t *>(output.data());
    uint8_t *out_begin = out;
    uint8_t *out_end = out + expected_len;

    while (in < in_end)
    {
        uint32_t ctrl = *in++;
        if (ctrl < 32)
        {
            size_t literal_len = static_cast<size_t>(ctrl) + 1;
            if (static_cast<size_t>(in_end - in) < literal_len ||
                static_cast<size_t>(out_end - out) < literal_len)
            {
                return false;
            }

            std::memcpy(out, in, literal_len);
            in += literal_len;
            out += literal_len;
            continue;
        }

        size_t len = ctrl >> 5;
        if (in >= in_end || out == out_begin)
        {
            return false;
        }

        if (len == 7)
        {
            if (in >= in_end)
            {
                return false;
            }
            len += *in++;
        }

        size_t ref_offset = (static_cast<size_t>(ctrl & 0x1F) << 8) + *in++;
        len += 2;

        if (ref_offset + 1 > static_cast<size_t>(out - out_begin) ||
            static_cast<size_t>(out_end - out) < len)
        {
            return false;
        }

        uint8_t *ref = out - ref_offset - 1;
        while (len-- != 0)
        {
            *out++ = *ref++;
        }
    }

    return out == out_end;
}

bool SerializeStringPayload(const std::string &value, std::string &payload)
{
    PrefixEloqOuterType(RedisObjectType::String, payload);
    RedisStringObject obj(value.data(), value.size());
    obj.Serialize(payload);
    return true;
}

bool SerializeListPayload(const std::vector<std::string> &elements,
                          std::string &payload)
{
    PrefixEloqOuterType(RedisObjectType::List, payload);
    RedisListObject obj;
    std::vector<EloqString> values;
    values.reserve(elements.size());
    for (const std::string &element : elements)
    {
        values.emplace_back(element.data(), element.size());
    }
    obj.CommitRPush(values);
    obj.Serialize(payload);
    return true;
}

bool SerializeSetPayload(const std::vector<std::string> &elements,
                         std::string &payload)
{
    PrefixEloqOuterType(RedisObjectType::Set, payload);
    RedisHashSetObject obj;
    std::vector<EloqString> values;
    values.reserve(elements.size());
    for (const std::string &element : elements)
    {
        values.emplace_back(element.data(), element.size());
    }
    obj.CommitSAdd(values, false);
    obj.Serialize(payload);
    return true;
}

bool SerializeHashPayload(
    const std::vector<std::pair<std::string, std::string>> &elements,
    std::string &payload)
{
    PrefixEloqOuterType(RedisObjectType::Hash, payload);
    RedisHashObject obj;
    std::vector<std::pair<EloqString, EloqString>> values;
    values.reserve(elements.size());
    for (const auto &[field, value] : elements)
    {
        values.emplace_back(EloqString(field.data(), field.size()),
                            EloqString(value.data(), value.size()));
    }
    obj.CommitHset(values);
    obj.Serialize(payload);
    return true;
}

bool SerializeZSetPayload(
    const std::vector<std::pair<double, std::string>> &elements,
    std::string &payload)
{
    PrefixEloqOuterType(RedisObjectType::Zset, payload);

    uint8_t obj_type = static_cast<uint8_t>(RedisObjectType::Zset);
    payload.append(reinterpret_cast<const char *>(&obj_type), sizeof(obj_type));

    uint32_t cnt = static_cast<uint32_t>(elements.size());
    payload.append(reinterpret_cast<const char *>(&cnt), sizeof(cnt));

    for (const auto &[score, member] : elements)
    {
        uint32_t member_len = static_cast<uint32_t>(member.size());
        payload.append(reinterpret_cast<const char *>(&member_len),
                       sizeof(member_len));
        payload.append(member.data(), member.size());
        payload.append(reinterpret_cast<const char *>(&score), sizeof(score));
    }

    return true;
}

bool DecodeZipListPairs(std::string_view blob,
                        std::vector<std::pair<std::string, std::string>> &out)
{
    std::vector<std::string> entries;
    if (!ReadZipListAll(blob, entries) || (entries.size() % 2) != 0)
    {
        return false;
    }
    out.reserve(entries.size() / 2);
    for (size_t i = 0; i < entries.size(); i += 2)
    {
        out.emplace_back(std::move(entries[i]), std::move(entries[i + 1]));
    }
    return true;
}

bool DecodeZipListZSet(std::string_view blob,
                       std::vector<std::pair<double, std::string>> &out)
{
    std::vector<std::string> entries;
    if (!ReadZipListAll(blob, entries) || (entries.size() % 2) != 0)
    {
        return false;
    }
    out.reserve(entries.size() / 2);
    for (size_t i = 0; i < entries.size(); i += 2)
    {
        long double score = 0;
        if (!string2ld(entries[i + 1].data(), entries[i + 1].size(), score))
        {
            return false;
        }
        out.emplace_back(static_cast<double>(score), std::move(entries[i]));
    }
    return true;
}

bool DecodeRedisObjectPayload(std::string_view object_payload,
                              std::string &eloq_payload)
{
    Reader reader(object_payload);
    uint8_t type = 0;
    if (!reader.ReadByte(type))
    {
        return false;
    }

    switch (type)
    {
    case kRdbTypeString:
    {
        std::string value;
        if (!reader.ReadString(value) || reader.Remaining() != 0)
        {
            return false;
        }
        return SerializeStringPayload(value, eloq_payload);
    }
    case kRdbTypeList:
    {
        uint64_t len = 0;
        bool encoded = false;
        if (!reader.ReadLen(len, encoded) || encoded)
        {
            return false;
        }
        if (!ValidateCount(len, 1, reader.Remaining()))
        {
            return false;
        }
        std::vector<std::string> elements;
        elements.reserve(static_cast<size_t>(len));
        for (uint64_t i = 0; i < len; i++)
        {
            std::string value;
            if (!reader.ReadString(value))
            {
                return false;
            }
            elements.emplace_back(std::move(value));
        }
        return reader.Remaining() == 0 &&
               SerializeListPayload(elements, eloq_payload);
    }
    case kRdbTypeSet:
    {
        uint64_t len = 0;
        bool encoded = false;
        if (!reader.ReadLen(len, encoded) || encoded)
        {
            return false;
        }
        if (!ValidateCount(len, 1, reader.Remaining()))
        {
            return false;
        }
        std::vector<std::string> elements;
        elements.reserve(static_cast<size_t>(len));
        for (uint64_t i = 0; i < len; i++)
        {
            std::string value;
            if (!reader.ReadString(value))
            {
                return false;
            }
            elements.emplace_back(std::move(value));
        }
        return reader.Remaining() == 0 &&
               SerializeSetPayload(elements, eloq_payload);
    }
    case kRdbTypeHash:
    {
        uint64_t len = 0;
        bool encoded = false;
        if (!reader.ReadLen(len, encoded) || encoded)
        {
            return false;
        }
        if (!ValidateCount(len, 2, reader.Remaining()))
        {
            return false;
        }
        std::vector<std::pair<std::string, std::string>> elements;
        elements.reserve(static_cast<size_t>(len));
        for (uint64_t i = 0; i < len; i++)
        {
            std::string field, value;
            if (!reader.ReadString(field) || !reader.ReadString(value))
            {
                return false;
            }
            elements.emplace_back(std::move(field), std::move(value));
        }
        return reader.Remaining() == 0 &&
               SerializeHashPayload(elements, eloq_payload);
    }
    case kRdbTypeZSet:
    {
        uint64_t len = 0;
        bool encoded = false;
        if (!reader.ReadLen(len, encoded) || encoded)
        {
            return false;
        }
        if (!ValidateCount(len, 2, reader.Remaining()))
        {
            return false;
        }
        std::vector<std::pair<double, std::string>> elements;
        elements.reserve(static_cast<size_t>(len));
        for (uint64_t i = 0; i < len; i++)
        {
            std::string member;
            double score = 0;
            if (!reader.ReadString(member) || !reader.ReadZSetScore(score))
            {
                return false;
            }
            elements.emplace_back(score, std::move(member));
        }
        return reader.Remaining() == 0 &&
               SerializeZSetPayload(elements, eloq_payload);
    }
    case kRdbTypeZSet2:
    {
        uint64_t len = 0;
        bool encoded = false;
        if (!reader.ReadLen(len, encoded) || encoded)
        {
            return false;
        }
        if (!ValidateCount(len, 2, reader.Remaining()))
        {
            return false;
        }
        std::vector<std::pair<double, std::string>> elements;
        elements.reserve(static_cast<size_t>(len));
        for (uint64_t i = 0; i < len; i++)
        {
            std::string member;
            double score = 0;
            if (!reader.ReadString(member) || !reader.ReadBinaryDouble(score))
            {
                return false;
            }
            elements.emplace_back(score, std::move(member));
        }
        return reader.Remaining() == 0 &&
               SerializeZSetPayload(elements, eloq_payload);
    }
    case kRdbTypeListZipList:
    {
        std::string blob;
        if (!reader.ReadString(blob) || reader.Remaining() != 0)
        {
            return false;
        }
        std::vector<std::string> elements;
        if (!ReadZipListAll(blob, elements))
        {
            return false;
        }
        return SerializeListPayload(elements, eloq_payload);
    }
    case kRdbTypeSetIntSet:
    {
        std::string blob;
        if (!reader.ReadString(blob) || reader.Remaining() != 0)
        {
            return false;
        }
        std::vector<std::string> elements;
        if (!ParseIntSetBlob(blob, elements))
        {
            return false;
        }
        return SerializeSetPayload(elements, eloq_payload);
    }
    case kRdbTypeZSetZipList:
    {
        std::string blob;
        if (!reader.ReadString(blob) || reader.Remaining() != 0)
        {
            return false;
        }
        std::vector<std::pair<double, std::string>> elements;
        if (!DecodeZipListZSet(blob, elements))
        {
            return false;
        }
        return SerializeZSetPayload(elements, eloq_payload);
    }
    case kRdbTypeHashZipList:
    {
        std::string blob;
        if (!reader.ReadString(blob) || reader.Remaining() != 0)
        {
            return false;
        }
        std::vector<std::pair<std::string, std::string>> elements;
        if (!DecodeZipListPairs(blob, elements))
        {
            return false;
        }
        return SerializeHashPayload(elements, eloq_payload);
    }
    case kRdbTypeQuickList:
    {
        uint64_t node_count = 0;
        bool encoded = false;
        if (!reader.ReadLen(node_count, encoded) || encoded)
        {
            return false;
        }
        if (!ValidateCount(node_count, 1, reader.Remaining()))
        {
            return false;
        }
        std::vector<std::string> elements;
        for (uint64_t i = 0; i < node_count; i++)
        {
            std::string blob;
            if (!reader.ReadString(blob))
            {
                return false;
            }
            std::vector<std::string> node_entries;
            if (!ReadZipListAll(blob, node_entries))
            {
                return false;
            }
            elements.insert(elements.end(),
                            std::make_move_iterator(node_entries.begin()),
                            std::make_move_iterator(node_entries.end()));
        }
        return reader.Remaining() == 0 &&
               SerializeListPayload(elements, eloq_payload);
    }
    case kRdbTypeHashListPack:
    {
        std::string blob;
        if (!reader.ReadString(blob) || reader.Remaining() != 0)
        {
            return false;
        }
        std::vector<std::string> entries;
        if (!ReadListPackAll(blob, entries) || (entries.size() % 2) != 0)
        {
            return false;
        }
        std::vector<std::pair<std::string, std::string>> elements;
        elements.reserve(entries.size() / 2);
        for (size_t i = 0; i < entries.size(); i += 2)
        {
            elements.emplace_back(std::move(entries[i]),
                                  std::move(entries[i + 1]));
        }
        return SerializeHashPayload(elements, eloq_payload);
    }
    case kRdbTypeZSetListPack:
    {
        std::string blob;
        if (!reader.ReadString(blob) || reader.Remaining() != 0)
        {
            return false;
        }
        std::vector<std::string> entries;
        if (!ReadListPackAll(blob, entries) || (entries.size() % 2) != 0)
        {
            return false;
        }
        std::vector<std::pair<double, std::string>> elements;
        elements.reserve(entries.size() / 2);
        for (size_t i = 0; i < entries.size(); i += 2)
        {
            long double score = 0;
            if (!string2ld(entries[i + 1].data(), entries[i + 1].size(), score))
            {
                return false;
            }
            elements.emplace_back(static_cast<double>(score),
                                  std::move(entries[i]));
        }
        return SerializeZSetPayload(elements, eloq_payload);
    }
    case kRdbTypeQuickList2:
    {
        uint64_t node_count = 0;
        bool encoded = false;
        if (!reader.ReadLen(node_count, encoded) || encoded)
        {
            return false;
        }
        if (!ValidateCount(node_count, 1, reader.Remaining()))
        {
            return false;
        }
        std::vector<std::string> elements;
        for (uint64_t i = 0; i < node_count; i++)
        {
            uint8_t container = 0;
            std::string blob;
            if (!reader.ReadByte(container) || !reader.ReadString(blob))
            {
                return false;
            }
            if (container == kQuickListNodeContainerPlain)
            {
                elements.emplace_back(std::move(blob));
                continue;
            }
            if (container != kQuickListNodeContainerPacked)
            {
                return false;
            }
            std::vector<std::string> node_entries;
            if (!ReadListPackAll(blob, node_entries))
            {
                return false;
            }
            elements.insert(elements.end(),
                            std::make_move_iterator(node_entries.begin()),
                            std::make_move_iterator(node_entries.end()));
        }
        return reader.Remaining() == 0 &&
               SerializeListPayload(elements, eloq_payload);
    }
    case kRdbTypeSetListPack:
    {
        std::string blob;
        if (!reader.ReadString(blob) || reader.Remaining() != 0)
        {
            return false;
        }
        std::vector<std::string> elements;
        if (!ReadListPackAll(blob, elements))
        {
            return false;
        }
        return SerializeSetPayload(elements, eloq_payload);
    }
    default:
        return false;
    }
}
}  // namespace

bool ConvertRedisDumpPayloadToEloqPayload(std::string_view dump_payload,
                                          std::string &eloq_payload)
{
    eloq_payload.clear();
    return DecodeRedisObjectPayload(dump_payload, eloq_payload);
}
}  // namespace EloqKV
