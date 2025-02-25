#include "redis_hash_object.h"

#include <absl/container/flat_hash_set.h>
#include <butil/fast_rand.h>

#include <string>
#include <vector>

#include "eloq_algorithm.h"
#include "eloq_string.h"
#include "redis_string_match.h"
#include "redis_string_num.h"

namespace EloqKV
{
extern const uint64_t MAX_OBJECT_SIZE;

bool RedisHashObject::Execute(EloqKV::HSetCommand &cmd) const
{
    RedisHashResult &result = cmd.result_;
    int ans = 0;
    int64_t len = 0;
    for (const auto &it : cmd.field_value_pairs_)
    {
        auto iter = hash_map_.find(it.first);
        if (iter == hash_map_.end())
        {
            ans++;
            len +=
                sizeof(uint32_t) * 2 + it.first.Length() + it.second.Length();
        }
        else
        {
            len = len + it.second.Length() - iter->second.Length();
        }
    }

    if (serialized_length_ + len > MAX_OBJECT_SIZE)
    {
        result.err_code_ = RD_ERR_OBJECT_TOO_BIG;
        return false;
    }
    else
    {
        result.result_ = ans;
        result.err_code_ = RD_OK;
        return true;
    }
}

void RedisHashObject::SetField(EloqString &field, EloqString &value)
{
    auto iter = hash_map_.find(field);
    if (iter == hash_map_.end())
    {
        serialized_length_ +=
            sizeof(uint32_t) * 2 + field.Length() + value.Length();
    }
    else
    {
        serialized_length_ =
            serialized_length_ + value.Length() - iter->second.Length();
    }

    if (field.Type() == EloqString::StorageType::View)
    {
        // copy EloqString for normal commands which store string_view
        hash_map_.insert_or_assign(iter, field.Clone(), value.Clone());
    }
    else
    {
        // move EloqString for log replay commands and cloned commands
        assert(value.Type() != EloqString::StorageType::View);
        hash_map_.insert_or_assign(iter, std::move(field), std::move(value));
    }
}

void RedisHashObject::CommitHset(
    std::vector<std::pair<EloqString, EloqString>> &elements)
{
    for (auto &[field, value] : elements)
    {
        SetField(field, value);
    }
}

void RedisHashObject::Execute(EloqKV::HGetCommand &cmd) const
{
    RedisHashResult &hash_result = cmd.result_;

    auto hash_it = hash_map_.find(cmd.field_);
    if (hash_it == hash_map_.end())
    {
        hash_result.err_code_ = RD_NIL;
    }
    else
    {
        hash_result.err_code_ = RD_OK;
        hash_result.result_ = hash_it->second.String();
    }
}

void RedisHashObject::Execute(EloqKV::HLenCommand &cmd) const
{
    RedisHashResult &hash_result = cmd.result_;
    hash_result.result_ = static_cast<int64_t>(hash_map_.size());
    hash_result.err_code_ = RD_OK;
}

void RedisHashObject::Execute(EloqKV::HStrLenCommand &cmd) const
{
    RedisHashResult &hash_result = cmd.result_;
    auto it = hash_map_.find(cmd.field_);
    if (it != hash_map_.end())
    {
        const EloqString &value = it->second;
        hash_result.result_ = static_cast<int64_t>(value.Length());
        hash_result.err_code_ = RD_OK;
    }
    else
    {
        hash_result.result_ = 0;
        hash_result.err_code_ = RD_OK;
    }
}

void RedisHashObject::Execute(EloqKV::HDelCommand &cmd) const
{
    RedisHashResult &hash_result = cmd.result_;
    int cnt = 0;
    for (auto &key : cmd.del_list_)
    {
        cnt += (hash_map_.find(key) != hash_map_.end());
    }
    if (cnt)
        hash_result.err_code_ = RD_OK;
    hash_result.result_ = cnt;
}

bool RedisHashObject::CommitHdel(std::vector<EloqString> &keys)
{
    for (const auto &key : keys)
    {
        auto iter = hash_map_.find(key);
        if (iter != hash_map_.end())
        {
            serialized_length_ -= sizeof(uint32_t) * 2 + iter->first.Length() +
                                  iter->second.Length();
            hash_map_.erase(iter);
        }
    }
    return hash_map_.size() == 0;
}

void RedisHashObject::Execute(EloqKV::HExistsCommand &cmd) const
{
    RedisHashResult &hash_result = cmd.result_;
    if (hash_map_.find(cmd.key_) != hash_map_.end())
    {
        hash_result.err_code_ = RD_OK;
    }
}

void RedisHashObject::Execute(EloqKV::HGetAllCommand &cmd) const
{
    RedisHashResult &hash_result = cmd.result_;
    std::vector<std::string> all_elements;
    all_elements.reserve(hash_map_.size() * 2);
    for (const auto &[key, value] : hash_map_)
    {
        all_elements.emplace_back(key.String());
        all_elements.emplace_back(value.String());
    }
    hash_result.result_ = std::move(all_elements);
    hash_result.err_code_ = RD_OK;
}

bool RedisHashObject::Execute(EloqKV::HIncrByCommand &cmd) const
{
    RedisHashResult &hash_result = cmd.result_;
    auto hash_it = hash_map_.find(cmd.field_);
    int64_t old_val = 0;
    int64_t len = 0;

    if (hash_it != hash_map_.end())
    {
        if (!string2ll(
                hash_it->second.Data(), hash_it->second.Length(), old_val))
        {
            hash_result.err_code_ = RD_ERR_HASH_VAL_ERROR;
            return false;
        }
        len -= hash_it->second.Length();
    }
    else
    {
        len += sizeof(uint32_t) * 2 + cmd.field_.Length();
    }

    int64_t incr = cmd.score_;

    if ((incr < 0 && old_val <= 0 && incr <= (LLONG_MIN - old_val)) ||
        (incr > 0 && old_val >= 0 && incr >= (LLONG_MAX - old_val)))
    {
        hash_result.err_code_ = RD_ERR_INCR_OVERFLOW;
        return false;
    }

    hash_result.err_code_ = RD_OK;
    hash_result.result_ = old_val + cmd.score_;
    len += 20;
    if (serialized_length_ + len > MAX_OBJECT_SIZE)
    {
        hash_result.err_code_ = RD_ERR_OBJECT_TOO_BIG;
        return false;
    }

    return true;
}

bool RedisHashObject::Execute(EloqKV::HIncrByFloatCommand &cmd) const
{
    RedisHashResult &hash_result = cmd.result_;
    auto hash_it = hash_map_.find(cmd.field_);
    long double old_val = 0;
    int64_t len = 0;

    if (hash_it != hash_map_.end())
    {
        const std::string_view &sv = hash_it->second.StringView();
        if (!string2ld(sv.data(), sv.size(), old_val))
        {
            hash_result.err_code_ = RD_ERR_FLOAT_VALUE;
            return false;
        }

        len -= hash_it->second.Length();
    }
    else
    {
        len += sizeof(uint32_t) * 2 + cmd.field_.Length();
    }

    old_val += cmd.incr_;
    if (isnan(old_val) || isinf(old_val))
    {
        hash_result.err_code_ = RD_ERR_INCR_NAN_OR_INFINITY;
        return false;
    }

    std::string str = ld2string(old_val);
    len += str.size();
    if (serialized_length_ + len > MAX_OBJECT_SIZE)
    {
        hash_result.err_code_ = RD_ERR_OBJECT_TOO_BIG;
        return false;
    }

    hash_result.result_ = std::move(str);
    hash_result.err_code_ = RD_OK;

    return true;
}

void RedisHashObject::CommitHincrby(EloqString &field, int64_t score)
{
    int64_t old_score = 0;
    auto hash_it = hash_map_.find(field);

    // origin hash value isn't int_64 type
    if (hash_it != hash_map_.end())
    {
        // It has been verified in execute
        bool b = string2ll(
            hash_it->second.Data(), hash_it->second.Length(), old_score);
        assert(b);
        (void) b;
        serialized_length_ -= hash_it->second.Length();
    }
    else
    {
        serialized_length_ += sizeof(uint32_t) * 2 + field.Length();
    }

    std::string final_score = std::to_string(old_score + score);
    serialized_length_ += final_score.size();
    if (field.Type() == EloqString::StorageType::View)
    {
        // commit on dirty_object before write log, deep copy EloqString to
        // emplace into map
        hash_map_.insert_or_assign(
            field.Clone(),
            EloqString(final_score.data(), final_score.length()));
    }
    else
    {
        // move resource after reply log
        hash_map_.insert_or_assign(
            std::move(field),
            EloqString(final_score.data(), final_score.length()));
    }
}

void RedisHashObject::CommitHIncrByFloat(EloqString &field, long double incr)
{
    long double old_val = 0;
    auto hash_it = hash_map_.find(field);

    // origin hash value isn't int_64 type
    if (hash_it != hash_map_.end())
    {
        bool b = string2ld(
            hash_it->second.Data(), hash_it->second.Length(), old_val);
        assert(b);
        (void) b;
        serialized_length_ -= hash_it->second.Length();
    }
    else
    {
        serialized_length_ += sizeof(uint32_t) * 2 + field.Length();
    }

    const std::string str = ld2string(old_val + incr);
    serialized_length_ += str.size();
    EloqString mv = EloqString(str.c_str(), str.size());

    if (field.Type() == EloqString::StorageType::View)
    {
        // commit on dirty_object before write log, deep copy EloqString to
        // emplace into map
        hash_map_.insert_or_assign(field.Clone(), mv);
    }
    else
    {
        // move resource after reply log
        hash_map_.insert_or_assign(std::move(field), mv);
    }
}

void RedisHashObject::Execute(EloqKV::HMGetCommand &cmd) const
{
    RedisHashResult &hash_result = cmd.result_;
    std::vector<std::optional<std::string>> elements;
    elements.reserve(cmd.fields_.size());

    for (const EloqString &field : cmd.fields_)
    {
        auto iter = hash_map_.find(field);
        if (iter != hash_map_.end())
        {
            const EloqString &value = iter->second;
            elements.emplace_back(
                std::make_optional<std::string>(value.String()));
        }
        else
        {
            elements.emplace_back(std::nullopt);
        }
    }
    hash_result.result_ = std::move(elements);
    hash_result.err_code_ = RD_OK;
}

void RedisHashObject::Execute(EloqKV::HKeysCommand &cmd) const
{
    RedisHashResult &hash_result = cmd.result_;
    std::vector<std::string> all_elements;
    all_elements.reserve(hash_map_.size());
    for (const auto &[key, value] : hash_map_)
    {
        all_elements.emplace_back(key.String());
    }
    hash_result.result_ = std::move(all_elements);
    hash_result.err_code_ = RD_OK;
}

void RedisHashObject::Execute(EloqKV::HValsCommand &cmd) const
{
    RedisHashResult &hash_result = cmd.result_;
    std::vector<std::string> all_elements;
    all_elements.reserve(hash_map_.size());
    for (const auto &[key, value] : hash_map_)
    {
        all_elements.emplace_back(value.String());
    }
    hash_result.result_ = std::move(all_elements);
    hash_result.err_code_ = RD_OK;
}

bool RedisHashObject::Execute(EloqKV::HSetNxCommand &cmd) const
{
    RedisHashResult &hash_result = cmd.result_;
    if (hash_map_.find(cmd.key_) == hash_map_.end())
    {
        if (serialized_length_ + sizeof(uint32_t) * 2 + cmd.key_.Length() +
                cmd.value_.Length() >
            MAX_OBJECT_SIZE)
        {
            hash_result.err_code_ = RD_ERR_OBJECT_TOO_BIG;
            return false;
        }
        else
        {
            hash_result.err_code_ = RD_OK;
            return true;
        }
    }
    else
    {
        return false;
    }
}

void RedisHashObject::CommitHSetNx(EloqString &key, EloqString &val)
{
    serialized_length_ += sizeof(uint32_t) * 2 + key.Length() + val.Length();

    if (key.Type() == EloqString::StorageType::View)
    {
        auto pr = hash_map_.insert_or_assign(key.Clone(), val.Clone());
        assert(pr.second);
        (void) pr;
    }
    else
    {
        auto pr = hash_map_.insert_or_assign(std::move(key), std::move(val));
        assert(pr.second);
        (void) pr;
    }
}

void RedisHashObject::Execute(EloqKV::HRandFieldCommand &cmd) const
{
    RedisHashResult &result = cmd.result_;
    result.err_code_ = RD_OK;

    assert(hash_map_.size() <= INT32_MAX);
    int32_t sz = static_cast<int32_t>(hash_map_.size());

    bool distinct = cmd.count_ >= 0;
    int64_t count = (cmd.count_ == INT64_MAX ? 1 : cmd.count_);
    count = (count >= 0 ? (count < sz ? count : sz) : -count);

    std::vector<std::string> rand_elements;
    if (cmd.with_values_)
    {
        rand_elements.resize(2 * count);
    }
    else
    {
        rand_elements.resize(count);
    }

    int32_t cnt = 0;
    if (sz > 0 && count > 0 && sz / count > 100 &&
        hash_map_.bucket_count() > static_cast<size_t>(count))
    {
        /*
        absl::flat_hash_set<size_t> visited;
        size_t bucket_cnt = hash_map_.bucket_count();
        while (cnt < count)
        {
            size_t idx = butil::fast_rand() % bucket_cnt;
            if (distinct)
            {
                if (visited.contains(idx))
                {
                    continue;
                }

                visited.insert(idx);
            }

            for (auto it = hash_map_.begin(idx);
                 it != hash_map_.end(idx) && cnt < count;
                 ++it)
            {
                if (cmd.with_values_)
                {
                    rand_elements[cnt * 2] = it->first.StringView();
                    rand_elements[cnt * 2 + 1] = it->second.StringView();
                }
                else
                {
                    rand_elements[cnt] = it->first.StringView();
                }

                cnt++;
            }
        }

        result.result_ = std::move(rand_elements);
        */

        auto hiter = hash_map_.begin();
        while (cnt < count)
        {
            if (cmd.with_values_)
            {
                rand_elements[cnt * 2] = hiter->first.StringView();
                rand_elements[cnt * 2 + 1] = hiter->second.StringView();
            }
            else
            {
                rand_elements[cnt] = hiter->first.StringView();
            }

            ++cnt;
            ++hiter;

            if (hiter == hash_map_.end())
            {
                hiter = hash_map_.begin();
            }
        }

        result.result_ = std::move(rand_elements);
    }
    else
    {
        std::multimap<int32_t, int64_t> map;
        GenRandMap(sz, count, !distinct, map);

        auto hiter = hash_map_.begin();
        for (auto miter = map.begin(); miter != map.end(); miter++)
        {
            while (cnt < miter->first)
            {
                cnt++;
                hiter++;
            }

            if (cmd.with_values_)
            {
                rand_elements[miter->second * 2] = hiter->first.StringView();
                rand_elements[miter->second * 2 + 1] =
                    hiter->second.StringView();
            }
            else
            {
                rand_elements[miter->second] = hiter->first.StringView();
            }
        }

        result.result_ = std::move(rand_elements);
    }
}

// TODO(lzx): Support Cursor and Count.(Batch Scanning)
// The iteration order of absl::flat_hash_map is instable. Thus we
// cannot implement batch Scanning now.
void RedisHashObject::Execute(EloqKV::HScanCommand &cmd) const
{
    RedisHashResult &hash_result = cmd.result_;
    hash_result.err_code_ = RD_OK;

    // *scan_ans.begin() is cursor, now set 0.
    // If fragment scan, return cursor after this time search
    std::vector<std::string> scan_ans;
    scan_ans.emplace_back(std::to_string(0));

    if (!cmd.match_)
    {
        if (cmd.novalues_)
        {
            scan_ans.reserve(hash_map_.size() + 1);
        }
        else
        {
            scan_ans.reserve(hash_map_.size() * 2 + 1);
        }
    }

    for (auto tmp_it = hash_map_.begin(); tmp_it != hash_map_.end(); tmp_it++)
    {
        auto &key = tmp_it->first;
        if (!cmd.match_ || stringmatchlen(cmd.pattern_.Data(),
                                          cmd.pattern_.Length(),
                                          key.Data(),
                                          key.Length(),
                                          0))
        {
            scan_ans.emplace_back(key.String());
            if (!cmd.novalues_)
            {
                scan_ans.emplace_back(tmp_it->second.String());
            }
        }
    }

    hash_result.result_ = std::move(scan_ans);
}

txservice::TxRecord::Uptr RedisHashObject::AddTTL(uint64_t ttl)
{
    return std::make_unique<RedisHashTTLObject>(std::move(*this), ttl);
}
}  // namespace EloqKV
