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
#include "redis_set_object.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "eloq_algorithm.h"
#include "redis_string_match.h"

namespace EloqKV
{
extern const uint64_t MAX_OBJECT_SIZE;

RedisHashSetObject::RedisHashSetObject(const RedisHashSetObject &rhs)
    : RedisEloqObject(rhs), hash_set_(rhs.hash_set_)
{
    serialized_length_ = rhs.serialized_length_;
}

RedisHashSetObject::RedisHashSetObject(RedisHashSetObject &&rhs)
    : RedisEloqObject(rhs), hash_set_(std::move(rhs.hash_set_))
{
    serialized_length_ = rhs.serialized_length_;
    rhs.serialized_length_ = 1 + sizeof(uint32_t);
}

txservice::TxRecord::Uptr RedisHashSetObject::AddTTL(uint64_t ttl)
{
    return std::make_unique<RedisHashSetTTLObject>(std::move(*this), ttl);
}

CommandExecuteState RedisHashSetObject::Execute(SAddCommand &cmd) const
{
    RedisHashSetResult &result = cmd.result_;
    result.err_code_ = RD_OK;
    result.ret_ = 0;
    uint64_t len = 0;

    for (const auto &str : cmd.vct_paras_)
    {
        bool b = hash_set_.find(str) == hash_set_.end();
        result.ret_ += b ? 1 : 0;
        len += b ? (sizeof(uint32_t) + str.Length()) : 0;
    }

    if (serialized_length_ + len > MAX_OBJECT_SIZE)
    {
        result.err_code_ = RD_ERR_OBJECT_TOO_BIG;
        return CommandExecuteState::NoChange;
    }

    return result.ret_ > 0 ? CommandExecuteState::Modified
                           : CommandExecuteState::NoChange;
}

void RedisHashSetObject::Execute(SMembersCommand &cmd) const
{
    RedisHashSetResult &result = cmd.result_;
    result.err_code_ = RD_OK;
    result.ret_ = hash_set_.size();
    result.string_list_.reserve(hash_set_.size());

    for (const auto &str : hash_set_)
    {
        result.string_list_.push_back(str.Clone());
    }
}

CommandExecuteState RedisHashSetObject::Execute(SRemCommand &cmd) const
{
    RedisHashSetResult &result = cmd.result_;
    result.err_code_ = RD_OK;
    result.ret_ = 0;

    for (const auto &str : cmd.vct_paras_)
    {
        result.ret_ += hash_set_.find(str) == hash_set_.end() ? 0 : 1;
    }
    if (result.ret_ == 0)
    {
        return CommandExecuteState::NoChange;
    }

    bool empty_after_removal =
        static_cast<size_t>(result.ret_) >= hash_set_.size();
    return empty_after_removal ? CommandExecuteState::ModifiedToEmpty
                               : CommandExecuteState::Modified;
}

void RedisHashSetObject::Execute(SCardCommand &cmd) const
{
    RedisHashSetResult &result = cmd.result_;
    result.err_code_ = RD_OK;
    result.ret_ = hash_set_.size();
}
void RedisHashSetObject::Execute(SIsMemberCommand &cmd) const
{
    RedisHashSetResult &result = cmd.result_;
    result.err_code_ = RD_OK;
    result.ret_ = hash_set_.find(cmd.str_member_) == hash_set_.end() ? 0 : 1;
}

void RedisHashSetObject::Execute(SMIsMemberCommand &cmd) const
{
    RedisHashSetResult &result = cmd.result_;
    result.err_code_ = RD_OK;

    for (const auto &str : cmd.members_)
    {
        result.string_list_.emplace_back(
            hash_set_.find(str) == hash_set_.end() ? "0" : "1");
    }
}

void RedisHashSetObject::Execute(SRandMemberCommand &cmd) const
{
    RedisHashSetResult &result = cmd.result_;
    result.err_code_ = RD_OK;

    int64_t count = cmd.count_ < 0 ? -cmd.count_ : cmd.count_;
    bool distinct = cmd.count_ >= 0;
    int32_t sz = static_cast<int32_t>(hash_set_.size());
    // When a set is empty, it is removed. Empty set does not exist.
    assert(sz > 0);
    if (distinct)
    {
        count = sz < count ? sz : count;
    }
    int32_t cnt = 0;
    if (sz > 0 && count > 0 && sz / count > 100 &&
        static_cast<int64_t>(hash_set_.bucket_count()) > count)
    {
        /*
        absl::flat_hash_set<size_t> visited;
        // If the set much larger than the number of elements needed
        //, use rand int to find a random bucket.
        result.string_list_.resize(count);
        sz = hash_set_.bucket_count();
        while (cnt < count)
        {
            uint32_t idx = butil::fast_rand() % sz;
            if (distinct)
            {
                if (visited.contains(idx))
                {
                    continue;
                }
                visited.insert(idx);
            }
            for (auto it = hash_set_.begin(idx);
                 it != hash_set_.end(idx) && cnt < count;
                 it++)
            {
                result.string_list_[cnt] = it->Clone();
                cnt++;
            }
        }
        */

        result.string_list_.resize(count);
        auto hiter = hash_set_.begin();
        while (cnt < count)
        {
            result.string_list_[cnt] = hiter->Clone();
            ++cnt;
            ++hiter;

            if (hiter == hash_set_.end())
            {
                hiter = hash_set_.begin();
            }
        }
    }
    else
    {
        assert(count >= 0);
        std::multimap<int32_t, int64_t> map;
        GenRandMap(sz, count, !distinct, map);

        result.string_list_.resize(map.size());
        auto hiter = hash_set_.begin();
        for (auto miter = map.begin(); miter != map.end(); miter++)
        {
            while (cnt < miter->first)
            {
                cnt++;
                hiter++;
            }

            result.string_list_[miter->second] = hiter->Clone();
        }
    }
}

CommandExecuteState RedisHashSetObject::Execute(SPopCommand &cmd) const
{
    RedisHashSetResult &result = cmd.result_;
    result.err_code_ = RD_OK;

    int64_t count = cmd.count_ == -1 ? 1 : cmd.count_;
    int32_t sz = static_cast<int32_t>(hash_set_.size());
    count = sz < count ? sz : count;
    int32_t cnt = 0;

    if (sz > 0 && count > 0 && sz / count > 100 &&
        static_cast<int64_t>(hash_set_.bucket_count()) > count)
    {
        /*
        absl::flat_hash_set<size_t> visited;
        // If the set much larger than the number of elements needed
        //, use rand int to find a random bucket.
        result.string_list_.resize(count);
        sz = hash_set_.bucket_count();
        while (cnt < count)
        {
            uint32_t idx = butil::fast_rand() % sz;
            if (visited.contains(idx))
            {
                continue;
            }
            visited.insert(idx);
            for (auto it = hash_set_.begin(idx);
                 it != hash_set_.end(idx) && cnt < count;
                 it++)
            {
                result.string_list_[cnt] = it->Clone();
                cnt++;
            }
        }
        */

        result.string_list_.resize(count);
        auto hiter = hash_set_.begin();
        while (cnt < count)
        {
            result.string_list_[cnt] = hiter->Clone();
            ++cnt;
            ++hiter;

            if (hiter == hash_set_.end())
            {
                hiter = hash_set_.begin();
            }
        }
    }
    else
    {
        assert(count >= 0);

        std::multimap<int32_t, int64_t> map;
        GenRandMap(sz, (count < sz ? count : sz), false, map);
        if (cmd.count_ == -1 && map.size() == 0)
        {
            result.err_code_ = RD_NIL;
        }

        result.string_list_.resize(map.size());
        auto hiter = hash_set_.begin();

        for (auto miter = map.begin(); miter != map.end(); miter++)
        {
            while (cnt < miter->first)
            {
                cnt++;
                hiter++;
            }

            result.string_list_[miter->second] = hiter->Clone();
        }
    }
    if (result.string_list_.empty())
    {
        return CommandExecuteState::NoChange;
    }

    bool empty_after_removal = result.string_list_.size() >= hash_set_.size();
    return empty_after_removal ? CommandExecuteState::ModifiedToEmpty
                               : CommandExecuteState::Modified;
}

// TODO(lzx): Support Cursor and Count.(Batch Scanning)
// The iteration order of absl::flat_hash_set is instable. Thus we
// cannot implement batch Scanning now.
void RedisHashSetObject::Execute(SScanCommand &cmd) const
{
    RedisHashSetResult &result = cmd.result_;
    result.err_code_ = RD_OK;

    // *result.string_list_.begin() is cursor, set default value 0.
    // If fragment scan, return cursor after this time search
    result.string_list_.emplace_back("0");

    if (!cmd.match_)
    {
        // Array reply: [cursor, member, member,  ...]
        result.string_list_.reserve(hash_set_.size() + 1);
    }

    for (auto iter = hash_set_.begin(); iter != hash_set_.end(); iter++)
    {
        std::string_view sv = iter->StringView();
        if (!cmd.match_ || stringmatchlen(cmd.pattern_.Data(),
                                          cmd.pattern_.Length(),
                                          sv.data(),
                                          sv.size(),
                                          0))
        {
            result.string_list_.emplace_back(iter->Clone());
        }
    }
}

void RedisHashSetObject::Execute(SZScanCommand &cmd) const
{
    RedisSetScanResult &set_scan_result = cmd.set_scan_result_;
    set_scan_result.err_code_ = RD_OK;
    if (cmd.withscores_)
    {
        std::vector<std::pair<EloqString, double>> result;
        for (auto iter = hash_set_.begin(); iter != hash_set_.end(); iter++)
        {
            // Hash set has default score value: 1
            result.emplace_back(iter->Clone(), 1);
        }
        set_scan_result.result_ = std::move(result);
    }
    else
    {
        std::vector<EloqString> result;
        for (auto iter = hash_set_.begin(); iter != hash_set_.end(); iter++)
        {
            result.emplace_back(iter->Clone());
        }
        set_scan_result.result_ = std::move(result);
    }
}

void RedisHashSetObject::Execute(SortableLoadCommand &cmd) const
{
    std::vector<std::string> elements;
    elements.reserve(hash_set_.size());
    for (const EloqString &element : hash_set_)
    {
        elements.emplace_back(element.String());
    }
    cmd.result_.obj_type_ = RedisObjectType::Set;
    cmd.result_.elems_ = std::move(elements);
}

bool RedisHashSetObject::CommitSAdd(std::vector<EloqString> &paras,
                                    bool should_not_move_string)
{
    for (auto &element : paras)
    {
        bool b;
        size_t element_len = element.Length();
        if (element.Type() == EloqString::StorageType::View ||
            should_not_move_string)
        {
            // copy EloqString for normal commands which store string_view
            b = hash_set_.emplace(element.Clone()).second;
        }
        else
        {
            // move EloqString for log replay commands and cloned commands
            b = hash_set_.emplace(std::move(element)).second;
        }

        if (b)
        {
            serialized_length_ += sizeof(uint32_t) + element_len;
        }
    }
    return hash_set_.size() == 0;
}

bool RedisHashSetObject::CommitSRem(const std::vector<EloqString> &paras)
{
    for (auto &str : paras)
    {
        size_t sz = hash_set_.erase(str);
        if (sz > 0)
        {
            serialized_length_ -= sizeof(uint32_t) + str.Length();
        }
    }
    return hash_set_.size() == 0;
}

bool RedisHashSetObject::CommitSPop(const std::vector<EloqString> &members)
{
    for (const EloqString &member : members)
    {
        size_t sz = hash_set_.erase(member);
        if (sz > 0)
        {
            serialized_length_ -= sizeof(uint32_t) + member.Length();
        }
    }

    return hash_set_.size() == 0;
}

void RedisHashSetObject::Serialize(std::vector<char> &buf, size_t &offset) const
{
    size_t off_old = offset;
    buf.resize(offset + serialized_length_);

    // Why do we need to serialize object types?
    // Because we only store the serialized bytes in kvstore, we need the
    // object type to specify which kind of redis object to create and
    // deserialize it when do read.
    int8_t obj_type = static_cast<int8_t>(RedisObjectType::Set);
    std::copy(&obj_type, &obj_type + 1, buf.begin() + offset);
    offset += 1;

    uint32_t ele_num = static_cast<uint32_t>(hash_set_.size());
    const unsigned char *tlen_ptr =
        static_cast<const unsigned char *>(static_cast<const void *>(&ele_num));
    std::copy(tlen_ptr, tlen_ptr + sizeof(uint32_t), buf.begin() + offset);
    offset += sizeof(uint32_t);

    for (const auto &eloq_str : hash_set_)
    {
        const std::string_view str_view = eloq_str.StringView();
        uint32_t slen = static_cast<uint32_t>(str_view.size());
        const unsigned char *slen_ptr = static_cast<const unsigned char *>(
            static_cast<const void *>(&slen));
        std::copy(slen_ptr, slen_ptr + sizeof(uint32_t), buf.begin() + offset);
        offset += sizeof(uint32_t);

        std::copy(str_view.begin(), str_view.end(), buf.begin() + offset);
        offset += slen;
    }
    assert(off_old + serialized_length_ == offset);
    (void) off_old;
}

void RedisHashSetObject::Serialize(std::string &str) const
{
    size_t off_old = str.size();
    str.reserve(off_old + serialized_length_);

    int8_t obj_type_val = static_cast<int8_t>(RedisObjectType::Set);
    str.append(1, obj_type_val);

    uint32_t ele_num = static_cast<uint32_t>(hash_set_.size());
    const char *tlen_ptr =
        static_cast<const char *>(static_cast<const void *>(&ele_num));
    str.append(tlen_ptr, sizeof(uint32_t));

    for (const auto &eloq_str : hash_set_)
    {
        const std::string_view str_view = eloq_str.StringView();
        uint32_t slen = static_cast<uint32_t>(str_view.size());
        const char *slen_ptr =
            static_cast<const char *>(static_cast<const void *>(&slen));
        str.append(slen_ptr, sizeof(uint32_t));
        str.append(str_view.data(), slen);
    }
    assert(off_old + serialized_length_ == str.size());
    (void) off_old;
}

void RedisHashSetObject::Deserialize(const char *buf, size_t &offset)
{
    size_t init_off = offset;
    // Check the object type.
    RedisObjectType obj_type = static_cast<RedisObjectType>(*(buf + offset));
    assert(obj_type == RedisObjectType::Set);
    // This silences the -Wunused-but-set-variable warning without
    // any runtime overhead.
    (void) obj_type;
    offset += 1;

    assert(hash_set_.size() == 0);
    uint32_t ele_num = *reinterpret_cast<const uint32_t *>(buf + offset);
    offset += sizeof(uint32_t);

    hash_set_.reserve(ele_num);
    for (size_t i = 0; i < ele_num; i++)
    {
        uint32_t slen = *reinterpret_cast<const uint32_t *>(buf + offset);
        offset += sizeof(uint32_t);
        hash_set_.emplace(buf + offset, slen);
        offset += slen;
    }

    serialized_length_ = offset - init_off;
}

void RedisHashSetObject::Execute(ZRangeCommand &cmd) const
{
    std::vector<EloqString> elements;
    elements.reserve(hash_set_.size());
    for (const EloqString &element : hash_set_)
    {
        elements.emplace_back(element.Clone());
        if (cmd.constraint_.opt_withscores_)
        {
            elements.emplace_back("1");
        }
    }

    RedisZsetResult &result = cmd.zset_result_;
    result.err_code_ = RD_OK;
    result.result_ = std::move(elements);
}

void RedisHashSetTTLObject::Serialize(std::vector<char> &buf,
                                      size_t &offset) const
{
    size_t off_old = offset;
    buf.resize(offset + sizeof(uint64_t) + serialized_length_);

    // Why do we need to serialize object types?
    // Because we only store the serialized bytes in kvstore, we need the
    // object type to specify which kind of redis object to create and
    // deserialize it when do read.
    int8_t obj_type = static_cast<int8_t>(RedisObjectType::TTLSet);
    std::copy(&obj_type, &obj_type + 1, buf.begin() + offset);
    offset += 1;

    // serialize ttl_
    const char* ttl_ptr = reinterpret_cast<const char*>(&ttl_);
    std::copy(ttl_ptr, ttl_ptr + sizeof(uint64_t), buf.begin() + offset);
    offset += sizeof(uint64_t);

    uint32_t ele_num = static_cast<uint32_t>(hash_set_.size());
    const unsigned char *tlen_ptr =
        static_cast<const unsigned char *>(static_cast<const void *>(&ele_num));
    std::copy(tlen_ptr, tlen_ptr + sizeof(uint32_t), buf.begin() + offset);
    offset += sizeof(uint32_t);

    for (const auto &eloq_str : hash_set_)
    {
        const std::string_view str_view = eloq_str.StringView();
        uint32_t slen = static_cast<uint32_t>(str_view.size());
        const unsigned char *slen_ptr = static_cast<const unsigned char *>(
            static_cast<const void *>(&slen));
        std::copy(slen_ptr, slen_ptr + sizeof(uint32_t), buf.begin() + offset);
        offset += sizeof(uint32_t);

        std::copy(str_view.begin(), str_view.end(), buf.begin() + offset);
        offset += slen;
    }
    assert(off_old + serialized_length_ + sizeof(uint64_t) == offset);
    (void) off_old;
}

void RedisHashSetTTLObject::Serialize(std::string &str) const
{
    size_t off_old = str.size();
    str.reserve(off_old + sizeof(uint64_t) + serialized_length_);

    int8_t obj_type_val = static_cast<int8_t>(RedisObjectType::TTLSet);
    str.append(1, obj_type_val);

    // serialize ttl_
    const char *ttl_ptr =
        static_cast<const char *>(static_cast<const void *>(&ttl_));
    str.append(ttl_ptr, sizeof(uint64_t));

    uint32_t ele_num = static_cast<uint32_t>(hash_set_.size());
    const char *tlen_ptr =
        static_cast<const char *>(static_cast<const void *>(&ele_num));
    str.append(tlen_ptr, sizeof(uint32_t));

    for (const auto &eloq_str : hash_set_)
    {
        const std::string_view str_view = eloq_str.StringView();
        uint32_t slen = static_cast<uint32_t>(str_view.size());
        const char *slen_ptr =
            static_cast<const char *>(static_cast<const void *>(&slen));
        str.append(slen_ptr, sizeof(uint32_t));
        str.append(str_view.data(), slen);
    }

    assert(off_old + serialized_length_ + sizeof(uint64_t) == str.size());
}

void RedisHashSetTTLObject::Deserialize(const char *buf, size_t &offset)
{
    size_t init_off = offset;
    // Check the object type.
    RedisObjectType obj_type = static_cast<RedisObjectType>(*(buf + offset));
    assert(obj_type == RedisObjectType::TTLSet);
    offset += 1;
    (void) obj_type;

    uint64_t *ttl_ptr = (uint64_t *) (buf + offset);
    ttl_ = *ttl_ptr;
    offset += sizeof(uint64_t);

    assert(hash_set_.size() == 0);
    uint32_t ele_num = *reinterpret_cast<const uint32_t *>(buf + offset);
    offset += sizeof(uint32_t);

    hash_set_.reserve(ele_num);
    for (size_t i = 0; i < ele_num; i++)
    {
        uint32_t slen = *reinterpret_cast<const uint32_t *>(buf + offset);
        offset += sizeof(uint32_t);
        hash_set_.emplace(buf + offset, slen);
        offset += slen;
    }

    serialized_length_ = offset - init_off - sizeof(uint64_t);
}

}  // namespace EloqKV
