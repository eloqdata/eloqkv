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

#include <functional>
#include <string_view>
#include <vector>

#include "eloq_string.h"
#include "redis_string_match.h"
#include "tx_key.h"

namespace EloqKV
{

uint16_t CRC16_XMODEM(const char *ptr, int32_t len);

class EloqKey
{
public:
    EloqKey() = default;

    EloqKey(const char *key_buf, size_t key_len) : key_(key_buf, key_len)
    {
    }

    EloqKey(std::string_view str_view) : key_(str_view)
    {
    }

    // Deep copy the key_
    EloqKey(const EloqKey &rhs) : key_(rhs.key_.Clone())
    {
    }

    EloqKey(EloqKey &&rhs) : key_(std::move(rhs.key_))
    {
    }

    static txservice::TxKey Create(const char *data, size_t size)
    {
        return txservice::TxKey(std::make_unique<EloqKey>(data, size));
    }

    static txservice::TxKey CreateDefault()
    {
        return txservice::TxKey(std::make_unique<EloqKey>());
    }

    EloqKey &operator=(EloqKey &&rhs) noexcept
    {
        if (this != &rhs)
        {
            key_ = std::move(rhs.key_);
        }
        return *this;
    }

    EloqKey &operator=(const EloqKey &rhs)
    {
        if (this != &rhs)
        {
            // Deep copy the key
            key_ = rhs.key_.Clone();
        }

        return *this;
    }

    friend bool operator==(const EloqKey &lhs, const EloqKey &rhs)
    {
        const EloqKey *neg_ptr = EloqKey::NegativeInfinity();
        const EloqKey *pos_ptr = EloqKey::PositiveInfinity();

        if (&lhs == neg_ptr || &lhs == pos_ptr || &rhs == neg_ptr ||
            &rhs == pos_ptr)
        {
            return &lhs == &rhs;
        }

        return lhs.key_ == rhs.key_;
    }

    friend bool operator!=(const EloqKey &lhs, const EloqKey &rhs)
    {
        return !(lhs == rhs);
    }

    friend bool operator<(const EloqKey &lhs, const EloqKey &rhs)
    {
        const EloqKey *neg_ptr = EloqKey::NegativeInfinity();
        const EloqKey *pos_ptr = EloqKey::PositiveInfinity();

        if (&lhs == neg_ptr)
        {
            // Negative infinity is less than any key, except itself.
            return &rhs != neg_ptr;
        }
        else if (&lhs == pos_ptr || &rhs == neg_ptr)
        {
            return false;
        }
        else if (&rhs == pos_ptr)
        {
            // Positive infinity is greater than any key, except itself.
            return &lhs != pos_ptr;
        }

        return lhs.key_.StringView() < rhs.key_.StringView();
    }

    friend bool operator<=(const EloqKey &lhs, const EloqKey &rhs)
    {
        return lhs < rhs || lhs == rhs;
    }

    // Notice: Use CRC16 to calculate the hash and the hash code is actually
    // only 16 bits.
    size_t Hash() const
    {
        //  Hash tags are a way to ensure that multiple keys are allocated in
        //  the same hash slot. If the key contains a "{...}" pattern only the
        //  substring between { and } is hashed in order to obtain the hash
        //  slot.
        // https://redis.io/docs/reference/cluster-spec/#hash-tags

        const char *buf = Buf();
        int32_t keylen = static_cast<int32_t>(Length());

        return Hash(buf, keylen);
    }

    static size_t Hash(const char *ptr, int32_t keylen)
    {
        const char *buf = ptr;

        int s, e; /* start-end indexes of { and } */
        /* Search the first occurrence of '{'. */
        for (s = 0; s < keylen; s++)
        {
            if (buf[s] == '{')
            {
                break;
            }
        }

        /* No '{' ? Hash the whole key. This is the base case. */
        if (s == keylen)
        {
            return CRC16_XMODEM(buf, keylen);
        }

        /* '{' found? Check if we have the corresponding '}'. */
        for (e = s + 1; e < keylen; e++)
        {
            if (buf[e] == '}')
            {
                break;
            }
        }
        /* No '}' or nothing between {} ? Hash the whole key. */
        if (e == keylen || e == s + 1)
        {
            return CRC16_XMODEM(buf, keylen);
        }

        /* If we are here there is both a { and a } on its right. Hash
         * what is in the middle between { and }. */
        return CRC16_XMODEM(buf + s + 1, e - s - 1);
    }

    void Serialize(std::vector<char> &buf, size_t &offset) const
    {
        std::string_view key_view = key_.StringView();

        uint16_t len_val = (uint16_t) key_view.size();
        buf.resize(offset + sizeof(uint16_t) + len_val);
        const char *val_ptr =
            static_cast<const char *>(static_cast<const void *>(&len_val));
        std::copy(val_ptr, val_ptr + sizeof(uint16_t), buf.begin() + offset);
        offset += sizeof(uint16_t);
        std::copy(key_view.begin(), key_view.end(), buf.begin() + offset);
        offset += len_val;
    }

    void Serialize(std::string &str) const
    {
        std::string_view key_view = key_.StringView();

        size_t len_sizeof = sizeof(uint16_t);
        // A 2-byte integer represents key lengths up to 65535
        uint16_t len_val = (uint16_t) key_view.size();
        const char *len_ptr = reinterpret_cast<const char *>(&len_val);

        str.append(len_ptr, len_sizeof);
        str.append(key_view.data(), len_val);
    }

    void Deserialize(const char *buf,
                     size_t &offset,
                     const txservice::KeySchema *key_schema)
    {
        const uint16_t *len_ptr =
            reinterpret_cast<const uint16_t *>(buf + offset);
        uint16_t len_val = *len_ptr;
        offset += sizeof(uint16_t);

        key_ = EloqString(buf + offset, len_val);

        offset += len_val;
    }

    static size_t HashFromSerializedKey(const char *buf, size_t offset)
    {
        const uint16_t *len_ptr =
            reinterpret_cast<const uint16_t *>(buf + offset);
        uint16_t len_val = *len_ptr;

        const char *key_str_ptr = buf + offset + sizeof(uint16_t);

        return Hash(key_str_ptr, len_val);
    }

    std::string_view KVSerialize() const
    {
        return key_.StringView();
    }

    void KVDeserialize(const char *buf, size_t len)
    {
        key_ = EloqString(buf, len);
    }

    txservice::TxKey CloneTxKey() const
    {
        if (this == NegativeInfinity())
        {
            return txservice::TxKey(NegativeInfinity());
        }
        else if (this == PositiveInfinity())
        {
            return txservice::TxKey(PositiveInfinity());
        }
        else
        {
            return txservice::TxKey(std::make_unique<EloqKey>(*this));
        }
    }

    std::string ToString() const
    {
        return std::string(key_.StringView());
    }

    void SetPackedKey(const char *data, size_t len)
    {
        key_ = EloqString(data, len);
    }

    void Copy(const EloqKey &rhs)
    {
        key_ = rhs.key_;
    }

    size_t SerializedLength() const
    {
        return 0;
    }

    size_t MemUsage() const
    {
        return key_.StringView().size();
    }

    const char *Buf() const
    {
        return key_.StringView().data();
    }

    size_t Length() const
    {
        return key_.StringView().size();
    }

    const char *Data() const
    {
        return Buf();
    }

    size_t Size() const
    {
        return Length();
    }

    std::string_view StringView() const
    {
        return key_.StringView();
    }

    static bool IsMatch(const char *key_data,
                        size_t key_data_size,
                        const std::string_view &pattern)
    {
        if (pattern.empty())
        {
            return true;
        }

        return stringmatchlen(pattern.data(),
                              pattern.size(),
                              key_data,
                              key_data_size,
                              0) != 0;
    }

    bool IsMatch(const std::string_view &pattern) const
    {
        if (pattern.size() == 0)
        {
            return true;
        }

        return stringmatchlen(pattern.data(),
                              pattern.size(),
                              key_.Data(),
                              key_.Length(),
                              0) != 0;
    }

    static const EloqKey *NegativeInfinity()
    {
        static const EloqKey neg_inf;
        return &neg_inf;
    }

    static const EloqKey *PositiveInfinity()
    {
        static const EloqKey pos_inf;
        return &pos_inf;
    }

    static const txservice::TxKey *NegInfTxKey()
    {
        static const txservice::TxKey neg_inf_tx_key{NegativeInfinity()};
        return &neg_inf_tx_key;
    }

    static const txservice::TxKey *PosInfTxKey()
    {
        static const txservice::TxKey pos_inf_tx_key{PositiveInfinity()};
        return &pos_inf_tx_key;
    }

    static const EloqKey *PackedNegativeInfinity()
    {
        static char neg_inf_packed_key = 0x00;
        static const EloqKey neg_inf_key(&neg_inf_packed_key, 1);
        return &neg_inf_key;
    }

    static const txservice::TxKey *PackedNegativeInfinityTxKey()
    {
        static const txservice::TxKey packed_negative_infinity_tx_key{
            PackedNegativeInfinity()};
        return &packed_negative_infinity_tx_key;
    }

    txservice::KeyType Type() const
    {
        if (this == EloqKey::NegativeInfinity())
        {
            return txservice::KeyType::NegativeInf;
        }
        else if (this == EloqKey::PositiveInfinity())
        {
            return txservice::KeyType::PositiveInf;
        }
        else
        {
            return txservice::KeyType::Normal;
        }
    }

    bool NeedsDefrag(mi_heap_t *heap)
    {
        return key_.NeedsDefrag(heap);
    }

    static const ::txservice::TxKeyInterface *TxKeyImpl()
    {
        static const txservice::TxKeyInterface tx_key_impl{
            *EloqKey::NegativeInfinity()};
        return &tx_key_impl;
    }

private:
    EloqString key_;
};

}  // namespace EloqKV

namespace std
{

template <>
struct hash<EloqKV::EloqKey>
{
    size_t operator()(const EloqKV::EloqKey &key) const noexcept
    {
        return key.Hash();
    }
};

}  // namespace std
