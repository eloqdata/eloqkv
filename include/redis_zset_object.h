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
#include <absl/container/btree_set.h>
#include <absl/container/flat_hash_map.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "redis_command.h"
#include "redis_object.h"
#include "tx_command.h"

namespace EloqKV
{
class RedisZsetObject : public RedisEloqObject
{
public:
    RedisZsetObject();
    RedisZsetObject(const RedisZsetObject &rhs);
    RedisZsetObject(RedisZsetObject &&rhs);
    ~RedisZsetObject() override = default;

    RedisObjectType ObjectType() const override
    {
        return RedisObjectType::Zset;
    }

    TxRecord::Uptr Clone() const override
    {
        return std::make_unique<RedisZsetObject>(*this);
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
        int8_t obj_type = static_cast<int8_t>(RedisObjectType::Zset);
        std::copy(&obj_type, &obj_type + 1, buf.begin() + offset);
        offset += 1;

        uint32_t cnt = z_hash_map_.size();
        const char *cnt_ptr = reinterpret_cast<const char *>(&cnt);
        std::copy(cnt_ptr, cnt_ptr + sizeof(uint32_t), buf.begin() + offset);
        offset += sizeof(uint32_t);

        for (auto &[key, value] : z_hash_map_)
        {
            uint32_t ks = key.length();
            const char *ks_ptr = reinterpret_cast<const char *>(&ks);
            std::copy(ks_ptr, ks_ptr + sizeof(uint32_t), buf.begin() + offset);
            offset += sizeof(uint32_t);

            std::copy(key.data(), key.data() + ks, buf.begin() + offset);
            offset += ks;

            const char *ds = reinterpret_cast<const char *>(&value);
            std::copy(ds, ds + sizeof(double), buf.begin() + offset);
            offset += sizeof(double);
            serialized += sizeof(uint32_t) + key.size() + sizeof(double);
        }

        assert(serialized_length_ == serialized);
    }

    void Serialize(std::string &str) const override
    {
        uint64_t old_len = str.size();
        str.reserve(old_len + serialized_length_);

        int8_t obj_type_val = static_cast<int8_t>(RedisObjectType::Zset);
        str.append(1, obj_type_val);

        uint32_t cnt = z_hash_map_.size();
        const char *cnt_ptr = reinterpret_cast<const char *>(&cnt);
        str.append(cnt_ptr, sizeof(uint32_t));

        for (auto &[field, score] : z_hash_map_)
        {
            uint32_t ks = field.length();
            const char *ks_ptr = reinterpret_cast<const char *>(&ks);
            str.append(ks_ptr, sizeof(uint32_t));

            str.append(field.data(), ks);

            const char *ds = reinterpret_cast<const char *>(&score);
            str.append(ds, sizeof(double));
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
        assert(obj_type == RedisObjectType::Zset);
        (void) obj_type;
        offset += 1;

        const uint32_t cnt = *reinterpret_cast<const uint32_t *>(buf + offset);
        offset += sizeof(uint32_t);

        for (uint32_t i = 0; i < cnt; i++)
        {
            uint32_t size = *reinterpret_cast<const uint32_t *>(buf + offset);
            offset += sizeof(uint32_t);

            EloqString field(buf + offset, size);
            offset += size;

            double score = *reinterpret_cast<const double *>(buf + offset);
            offset += sizeof(double);

            serialized_length_ += sizeof(double) + sizeof(uint32_t) + size;

            auto it = z_ordered_set_.insert(ZNode(std::move(field), score));
            z_hash_map_.emplace(it.first->field_.StringView(), score);
        }
    }

    const absl::flat_hash_map<std::string_view, double> &Elements() const
    {
        return z_hash_map_;
    }

    txservice::TxRecord::Uptr AddTTL(uint64_t ttl) override;

    std::tuple<int, int, bool> ZAddXX(
        std::vector<std::pair<double, EloqString>> &elements, bool ch) const;
    std::tuple<int, int, bool> ZAddNX(
        std::vector<std::pair<double, EloqString>> &elements) const;
    std::tuple<int, int, bool> ZAddLT(
        std::vector<std::pair<double, EloqString>> &elements,
        bool ch,
        bool xx) const;
    std::tuple<int, int, bool> ZAddGT(
        std::vector<std::pair<double, EloqString>> &elements,
        bool ch,
        bool xx) const;
    std::tuple<int, int, bool> ZAdd(
        std::vector<std::pair<double, EloqString>> &elements, bool ch) const;
    std::tuple<int, EloqString, bool> ZAddIncr(
        std::variant<std::monostate,
                     std::pair<double, EloqString>,
                     std::vector<std::pair<double, EloqString>>> &elements,
        ZParams &params,
        ZAddCommand::ElementType &type) const;

    bool Execute(ZAddCommand &cmd) const;
    bool Execute(ZRemCommand &cmd) const;
    bool Execute(ZRemRangeCommand &cmd) const;
    void Execute(ZScoreCommand &cmd) const;
    void Execute(ZRangeCommand &cmd) const;
    bool Execute(ZPopCommand &cmd) const;
    void Execute(ZLexCountCommand &cmd) const;
    void Execute(ZCountCommand &cmd) const;
    void Execute(ZCardCommand &cmd) const;
    void Execute(ZScanCommand &cmd) const;
    void Execute(ZRandMemberCommand &cmd) const;
    void Execute(ZRankCommand &cmd) const;
    void Execute(SortableLoadCommand &cmd) const;
    void Execute(ZMScoreCommand &cmd) const;
    void Execute(SZScanCommand &cmd) const;

    void CommitZAdd(
        std::variant<std::monostate,
                     std::pair<double, EloqString>,
                     std::vector<std::pair<double, EloqString>>> &elements,
        ZParams &params,
        bool should_not_move_string);
    bool CommitZPop(ZPopCommand::PopType pop_type, int64_t count);
    bool CommitZRem(std::vector<EloqString> &elements);
    bool CommitZRemRange(ZRangeType &range_type, ZRangeSpec &spec);

    void CommitZNew(std::vector<std::pair<double, EloqString>> &);

    struct ZNode
    {
        ZNode() = default;

        // this node type is view, use to binary search
        ZNode(std::string_view field_sv, double score)
            : field_(field_sv), score_(score)
        {
        }

        // this node will own string, and used to insert.
        ZNode(EloqString &&field, double score)
            : field_(std::move(field)), score_(score)
        {
            assert(field_.Type() != EloqString::StorageType::View);
        }
        EloqString field_;
        double score_;
    };

    // The equal here refers to whether the objects pointed to are consistent.
    // It is possible that one is an owned object and the other is a reference
    // object, but as long as the objects they point to are consistent, then
    // they are consistent under the current semantics.
    bool operator==(const RedisZsetObject &rhs) const
    {
        for (auto &[key, value] : z_hash_map_)
        {
            if (rhs.z_hash_map_.find(key)->second != value)
            {
                return false;
            }
        }

        for (auto &node : z_ordered_set_)
        {
            auto it = rhs.z_ordered_set_.find(node);
            if (it == rhs.z_ordered_set_.end())
            {
                return false;
            }
            if (it->score_ != node.score_)
            {
                return false;
            }
            if (it->field_.StringView() != node.field_.StringView())
            {
                return false;
            }
        }

        if (rhs.z_hash_map_.size() != z_hash_map_.size())
        {
            return false;
        }

        if (rhs.z_ordered_set_.size() != z_ordered_set_.size())
        {
            return false;
        }

        return true;
    }

private:
    void DelField(std::string_view sv);

    std::vector<EloqString> GetZrangeResult(ZRangeConstraint &arg,
                                            ZRangeSpec &spec) const;

    std::vector<EloqString> ZRangeByRank(ZRangeConstraint &arg,
                                         ZRangeSpec &spec) const;
    std::vector<EloqString> ZRangeByScore(ZRangeConstraint &arg,
                                          ZRangeSpec &spec) const;
    std::vector<EloqString> ZRangeByLex(ZRangeConstraint &arg,
                                        ZRangeSpec &spec) const;

    int ZRemRangeByScore(ZRangeSpec &spec) const;
    int ZRemRangeByLex(ZRangeSpec &spec) const;
    int ZRemRangeByRank(ZRangeSpec &spec) const;

    // shared by zcount and zcard
    int ZCount(ZRangeSpec &spec) const;

    std::tuple<bool, std::set<ZNode>::iterator, std::set<ZNode>::iterator>
    GetRangeByLexForCurrentScore(double current_score, ZRangeSpec &spec) const;

    /**
     * Custom comparison function to implement heterogeneous
     * comparison
     */
    struct Cmp
    {
        using is_transparent = void;

        bool operator()(const ZNode &a, const ZNode &b) const
        {
            if (a.score_ != b.score_)
            {
                return a.score_ < b.score_;
            }
            return a.field_ < b.field_;
        }

        // lower bound
        bool operator()(const ZNode &a, const double &b) const
        {
            return a.score_ < b;
        }

        // upper bound
        bool operator()(const double &a, const ZNode &b) const
        {
            return a < b.score_;
        }
    };

protected:
    /**
     * use set(red-black tree), search, range
     * search, removal, and insertion operations have logarithmic
     * complexity. ZNode own string and map use string_view
     * refer to this object_string in order to reduce memory usage
     * use absl::flat_hash_map to replace std::map
     */
    std::set<ZNode, Cmp> z_ordered_set_;
    absl::flat_hash_map<std::string_view, double> z_hash_map_;

    // The estimated memory size of the object. Limit the objects size so that
    // the persistent storage flush wouldn't fail.
    uint64_t serialized_length_{};
};

class RedisZsetTTLObject : public RedisZsetObject
{
public:
    RedisZsetTTLObject() : RedisZsetObject(), ttl_(UINT64_MAX)
    {
    }

    RedisZsetTTLObject(const RedisZsetTTLObject &other)
        : RedisZsetObject(other), ttl_(other.ttl_)
    {
    }

    RedisZsetTTLObject(const RedisZsetObject &&other, uint64_t ttl)
        : RedisZsetObject(std::move(other)), ttl_(ttl)
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
        return RedisObjectType::Zset;
    }

    size_t SerializedLength() const override
    {
        return sizeof(uint64_t) + RedisZsetObject::SerializedLength();
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
        int8_t obj_type = static_cast<int8_t>(RedisObjectType::TTLZset);
        std::copy(&obj_type, &obj_type + 1, buf.begin() + offset);
        offset += 1;

        // serialize ttl_
        std::copy(&ttl_, &ttl_ + sizeof(uint64_t), buf.begin() + offset);
        offset += sizeof(uint64_t);

        uint32_t cnt = z_hash_map_.size();
        const char *cnt_ptr = reinterpret_cast<const char *>(&cnt);
        std::copy(cnt_ptr, cnt_ptr + sizeof(uint32_t), buf.begin() + offset);
        offset += sizeof(uint32_t);

        for (auto &[key, value] : z_hash_map_)
        {
            uint32_t ks = key.length();
            const char *ks_ptr = reinterpret_cast<const char *>(&ks);
            std::copy(ks_ptr, ks_ptr + sizeof(uint32_t), buf.begin() + offset);
            offset += sizeof(uint32_t);

            std::copy(key.data(), key.data() + ks, buf.begin() + offset);
            offset += ks;

            const char *ds = reinterpret_cast<const char *>(&value);
            std::copy(ds, ds + sizeof(double), buf.begin() + offset);
            offset += sizeof(double);
            serialized += sizeof(uint32_t) + key.size() + sizeof(double);
        }

        assert(SerializedLength() == serialized);
    }

    void Serialize(std::string &str) const override
    {
        uint64_t old_len = str.size();
        str.reserve(old_len + SerializedLength());
        int8_t obj_type_val = static_cast<int8_t>(RedisObjectType::TTLZset);
        str.append(1, obj_type_val);

        // serialize ttl_
        const char *ttl_ptr =
            static_cast<const char *>(static_cast<const void *>(&ttl_));
        str.append(ttl_ptr, sizeof(uint64_t));

        uint32_t cnt = z_hash_map_.size();
        const char *cnt_ptr = reinterpret_cast<const char *>(&cnt);
        str.append(cnt_ptr, sizeof(uint32_t));

        for (auto &[field, score] : z_hash_map_)
        {
            uint32_t ks = field.length();
            const char *ks_ptr = reinterpret_cast<const char *>(&ks);
            str.append(ks_ptr, sizeof(uint32_t));

            str.append(field.data(), ks);

            const char *ds = reinterpret_cast<const char *>(&score);
            str.append(ds, sizeof(double));
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
        assert(obj_type == RedisObjectType::TTLZset);
        (void) obj_type;
        offset += 1;

        uint64_t *ttl_ptr = (uint64_t *) (buf + offset);
        ttl_ = *ttl_ptr;
        offset += sizeof(uint64_t);

        const uint32_t cnt = *reinterpret_cast<const uint32_t *>(buf + offset);
        offset += sizeof(uint32_t);

        for (uint32_t i = 0; i < cnt; i++)
        {
            uint32_t size = *reinterpret_cast<const uint32_t *>(buf + offset);
            offset += sizeof(uint32_t);

            EloqString field(buf + offset, size);
            offset += size;

            double score = *reinterpret_cast<const double *>(buf + offset);
            offset += sizeof(double);

            serialized_length_ += sizeof(double) + sizeof(uint32_t) + size;

            auto it = z_ordered_set_.insert(ZNode(std::move(field), score));
            z_hash_map_.emplace(it.first->field_.StringView(), score);
        }
    }

    TxRecord::Uptr Clone() const override
    {
        auto ptr = std::make_unique<RedisZsetTTLObject>(*this);
        return ptr;
    }

    TxRecord::Uptr RemoveTTL() override
    {
        return std::make_unique<RedisZsetObject>(std::move(*this));
    }

private:
    uint64_t ttl_{UINT64_MAX};
};
}  // namespace EloqKV
