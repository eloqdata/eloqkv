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
#include "redis_zset_object.h"

#include <absl/container/flat_hash_set.h>
#include <butil/fast_rand.h>

#include <algorithm>
#include <cstdint>
#include <limits>  // std::numeric_limits<>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <variant>

#include "eloq_algorithm.h"
#include "eloq_string.h"
#include "redis_command.h"
#include "redis_errors.h"
#include "redis_string_match.h"
#include "redis_string_num.h"

namespace EloqKV
{
extern const uint64_t MAX_OBJECT_SIZE;

RedisZsetObject::RedisZsetObject()
{
    serialized_length_ = 1 + sizeof(uint32_t);
}

RedisZsetObject::RedisZsetObject(const RedisZsetObject &rhs)
    : RedisEloqObject(rhs), serialized_length_(rhs.serialized_length_)
{
    for (const auto &[field, score] : rhs.z_ordered_set_)
    {
        auto iter = z_ordered_set_.insert(ZNode(field.Clone(), score));
        z_hash_map_.emplace(iter.first->field_.StringView(), score);
    }
}

RedisZsetObject::RedisZsetObject(RedisZsetObject &&rhs)
    : RedisEloqObject(std::move(rhs)),
      z_ordered_set_(std::move(rhs.z_ordered_set_)),
      z_hash_map_(std::move(rhs.z_hash_map_)),
      serialized_length_(rhs.serialized_length_)
{
    rhs.serialized_length_ = 1 + sizeof(uint32_t);
}

void RedisZsetObject::DelField(std::string_view sv)
{
    auto its = z_hash_map_.find(sv);
    if (its != z_hash_map_.end())
    {
        auto it = z_ordered_set_.find(ZNode(sv, its->second));
        assert(it != z_ordered_set_.end());
        serialized_length_ -= sizeof(double) + sizeof(uint32_t) + sv.size();
        z_ordered_set_.erase(it);
        z_hash_map_.erase(its);
    }
    assert(serialized_length_ <= MAX_OBJECT_SIZE);
}

txservice::TxRecord::Uptr RedisZsetObject::AddTTL(uint64_t ttl)
{
    return std::make_unique<RedisZsetTTLObject>(std::move(*this), ttl);
}

std::tuple<int, int, bool> RedisZsetObject::ZAddXX(
    std::vector<std::pair<double, EloqString>> &elements, bool ch) const
{
    std::unordered_map<std::string_view, double> used_field;
    int cnt = 0;
    bool modified = false;

    auto result = elements.begin();
    for (auto it = elements.begin(); it != elements.end(); it++)
    {
        double num = it->first;
        std::string_view field_sv = it->second.StringView();
        auto hash_it = z_hash_map_.find(field_sv);
        auto used_it = used_field.find(field_sv);
        bool used_empty = used_it == used_field.end();
        bool ha_empty = hash_it == z_hash_map_.end();

        if (used_empty && ha_empty)
        {
            continue;
        }

        if (!used_empty)
        {
            if (num != used_it->second)
            {
                modified = true;
            }
            cnt += (ch && num != used_it->second) ? 1 : 0;
            used_it->second = num;
        }
        else if (!ha_empty)
        {
            if (num != hash_it->second)
            {
                modified = true;
            }
            cnt += (ch && num != hash_it->second) ? 1 : 0;
            used_field.insert({field_sv, num});
        }

        if (it != result)
        {
            *result = std::move(*it);
        }
        ++result;
    }

    elements.erase(result, elements.end());
    return {RD_OK, cnt, modified};
}

std::tuple<int, int, bool> RedisZsetObject::ZAddNX(
    std::vector<std::pair<double, EloqString>> &elements) const
{
    std::unordered_set<std::string_view> used_field;
    int cnt = 0;

    auto result = elements.begin();
    for (auto it = elements.begin(); it != elements.end(); it++)
    {
        std::string_view field_sv = it->second.StringView();
        auto hash_it = z_hash_map_.find(field_sv);
        auto used_it = used_field.find(field_sv);
        if (used_it == used_field.end())
        {
            if (hash_it != z_hash_map_.end())
            {
                continue;
            }

            cnt++;
            if (result != it)
            {
                *result = std::move(*it);
            }
            ++result;

            used_field.insert(field_sv);
        }
    }
    elements.erase(result, elements.end());
    return {RD_OK, cnt, cnt > 0};
}

std::tuple<int, int, bool> RedisZsetObject::ZAddLT(
    std::vector<std::pair<double, EloqString>> &elements,
    bool ch,
    bool xx) const
{
    std::unordered_map<std::string_view, double> used_field;
    int cnt = 0;
    bool modified = false;

    // this loop like std::remove_if to move all can't execute to the end of
    // vector and erase it, so serialize and commit will not do illegal order
    auto result = elements.begin();
    for (auto it = elements.begin(); it != elements.end(); it++)
    {
        double num = it->first;
        std::string_view field_sv = it->second.StringView();

        auto used_it = used_field.find(field_sv);
        auto hash_it = z_hash_map_.find(field_sv);
        bool dt_empty = used_it == used_field.end();
        bool ha_empty = hash_it == z_hash_map_.end();
        bool lt = true;

        if (!ha_empty)
        {
            lt = num < hash_it->second;
        }
        if (!dt_empty)
        {
            lt = num < used_it->second;
        }

        if (!lt || (xx && dt_empty && ha_empty))
        {
            continue;
        }

        if (used_it == used_field.end())
        {
            if (ha_empty || num < hash_it->second)
            {
                modified = true;
            }
            cnt += (ha_empty) || (ch && num < hash_it->second);
            used_field.emplace(field_sv, num);
        }
        else if (num < used_it->second)
        {
            if (num < used_it->second)
            {
                modified = true;
            }
            cnt += (ch && num < used_it->second);
            used_it->second = num;
        }

        if (result != it)
        {
            *result = std::move(*it);
        }
        result++;
    }
    elements.erase(result, elements.end());
    return {RD_OK, cnt, modified};
}

std::tuple<int, int, bool> RedisZsetObject::ZAddGT(
    std::vector<std::pair<double, EloqString>> &elements,
    bool ch,
    bool xx) const
{
    std::unordered_map<std::string_view, double> used_field;
    int cnt = 0;
    bool modified = false;

    // this loop like std::remove_if to move all can't execute to the end of
    // vector and erase it, so serialize and commit will not do illegal order
    auto result = elements.begin();
    for (auto it = elements.begin(); it != elements.end(); it++)
    {
        double num = it->first;
        std::string_view field_sv = it->second.StringView();

        auto used_it = used_field.find(field_sv);
        auto hash_it = z_hash_map_.find(field_sv);
        bool dt_empty = used_it == used_field.end();
        bool ha_empty = hash_it == z_hash_map_.end();
        bool lt = true;

        if (!ha_empty)
        {
            lt = num > hash_it->second;
        }
        if (!dt_empty)
        {
            lt = num > used_it->second;
        }

        if (!lt || (xx && dt_empty && ha_empty))
        {
            continue;
        }

        if (used_it == used_field.end())
        {
            if (ha_empty || num > hash_it->second)
            {
                modified = true;
            }
            cnt += (ha_empty) || (ch && num > hash_it->second);
            used_field.emplace(field_sv, num);
        }
        else if (num > used_it->second)
        {
            if (num > used_it->second)
            {
                modified = true;
            }
            cnt += (ch && num > used_it->second);
            used_it->second = num;
        }

        if (result != it)
        {
            *result = std::move(*it);
        }
        result++;
    }
    elements.erase(result, elements.end());
    return {RD_OK, cnt, modified};
}

std::tuple<int, int, bool> RedisZsetObject::ZAdd(
    std::vector<std::pair<double, EloqString>> &elements, bool ch) const
{
    int cnt = 0;
    bool modified = false;

    std::unordered_map<std::string_view, double> used_field;
    for (auto &[num, field] : elements)
    {
        auto field_sv = field.StringView();
        auto used_it = used_field.find(field_sv);
        if (used_it == used_field.end())
        {
            used_field.emplace(field_sv, num);
            auto hash_it = z_hash_map_.find(field.StringView());
            if (hash_it == z_hash_map_.end() || hash_it->second != num)
            {
                modified = true;
            }
            cnt += (hash_it == z_hash_map_.end()) ||
                   (ch && hash_it->second != num);
        }
        else
        {
            if (used_it->second != num)
            {
                modified = true;
            }
            cnt += ch && (used_it->second != num);
            used_it->second = num;
        }
    }

    return {RD_OK, cnt, modified};
}

// this command only accepct one pair, like zincrby
// return can be nil, first element mark this command reply int/nil, second one
// is ans
std::tuple<int, EloqString, bool> RedisZsetObject::ZAddIncr(
    std::variant<std::monostate,
                 std::pair<double, EloqString>,
                 std::vector<std::pair<double, EloqString>>> &elements,
    ZParams &params,
    ZAddCommand::ElementType &type) const
{
    auto &[dbl, field] = std::get<std::pair<double, EloqString>>(elements);
    auto num = dbl;
    std::string_view field_sv = field.StringView();
    auto hash_it = z_hash_map_.find(field_sv);
    bool flag = true;
    bool na_infi = false, po_infi = false;

    na_infi |= (num == std::numeric_limits<double>::infinity() * -1);
    po_infi |= (num == std::numeric_limits<double>::infinity());

    if (params.XX())
    {
        flag = hash_it != z_hash_map_.end();
    }
    if (params.NX() && hash_it != z_hash_map_.end())
    {
        elements = std::monostate{};
        type = ZAddCommand::ElementType::monostate;
        return {RD_NIL, EloqString(), false};
    }
    if (params.GT() && ((hash_it == z_hash_map_.end()) ||
                        (hash_it->second + num <= hash_it->second)))
    {
        elements = std::monostate{};
        type = ZAddCommand::ElementType::monostate;
        return {RD_NIL, EloqString(), false};
    }
    if (params.LT() && ((hash_it == z_hash_map_.end()) ||
                        (hash_it->second + num >= hash_it->second)))
    {
        elements = std::monostate{};
        type = ZAddCommand::ElementType::monostate;
        return {RD_NIL, EloqString(), false};
    }
    if (flag && hash_it != z_hash_map_.end())
    {
        // -inf and +inf can't sum in redis
        na_infi |=
            (hash_it->second == std::numeric_limits<double>::infinity() * -1);
        po_infi |= (hash_it->second == std::numeric_limits<double>::infinity());

        // in redis incr -inf can't add by +inf, if this command is illegal
        // remove it and it won't commit
        flag = !(na_infi && po_infi);

        num += hash_it->second;
    }

    if (!flag)
    {
        elements = std::monostate{};
        type = ZAddCommand::ElementType::monostate;
        return {RD_ERR_SCORE_NAN, EloqString(), false};
    }

    return {
        RD_OK, EloqString(d2string(num).data(), d2string(num).size()), true};
}

bool RedisZsetObject::Execute(ZAddCommand &cmd) const
{
    RedisZsetResult &result = cmd.zset_result_;

    uint64_t size_increased = 0;
    switch (cmd.type_)
    {
    case ZAddCommand::ElementType::pair:
    {
        const auto &pair =
            std::get<std::pair<double, EloqString>>(cmd.elements_);
        size_increased +=
            sizeof(double) + sizeof(uint32_t) + pair.second.Length();
        break;
    }
    case ZAddCommand::ElementType::vector:
    {
        const auto &pairs =
            std::get<std::vector<std::pair<double, EloqString>>>(cmd.elements_);
        for (const auto &p : pairs)
        {
            size_increased +=
                sizeof(double) + sizeof(uint32_t) + p.second.Length();
        }
        break;
    }
    default:
        break;
    }

    if (serialized_length_ + size_increased > MAX_OBJECT_SIZE)
    {
        result.err_code_ = RD_ERR_OBJECT_TOO_BIG;
        return false;
    }

    bool modified = false;

    if (cmd.params_.INCR())
    {
        std::tie(result.err_code_, result.result_, modified) =
            ZAddIncr(cmd.elements_, cmd.params_, cmd.type_);
    }
    else if (cmd.params_.LT())
    {
        std::tie(result.err_code_, result.result_, modified) = ZAddLT(
            std::get<std::vector<std::pair<double, EloqString>>>(cmd.elements_),
            cmd.params_.CH(),
            cmd.params_.XX());
    }
    else if (cmd.params_.GT())
    {
        std::tie(result.err_code_, result.result_, modified) = ZAddGT(
            std::get<std::vector<std::pair<double, EloqString>>>(cmd.elements_),
            cmd.params_.CH(),
            cmd.params_.XX());
    }
    else if (cmd.params_.NX())
    {
        std::tie(result.err_code_, result.result_, modified) =
            ZAddNX(std::get<std::vector<std::pair<double, EloqString>>>(
                cmd.elements_));
    }
    else if (cmd.params_.XX())
    {
        std::tie(result.err_code_, result.result_, modified) = ZAddXX(
            std::get<std::vector<std::pair<double, EloqString>>>(cmd.elements_),
            cmd.params_.CH());
    }
    else
    {
        std::tie(result.err_code_, result.result_, modified) = ZAdd(
            std::get<std::vector<std::pair<double, EloqString>>>(cmd.elements_),
            cmd.params_.CH());
    }

    return modified;
}

void RedisZsetObject::CommitZAdd(
    std::variant<std::monostate,
                 std::pair<double, EloqString>,
                 std::vector<std::pair<double, EloqString>>> &elements,
    ZParams &params,
    bool should_not_move_string)
{
    auto update_element =
        [this, &params, &should_not_move_string](double &num, EloqString &field)
    {
        auto hash_it = z_hash_map_.find(field.StringView());

        if (params.INCR())
        {
            num += (hash_it == z_hash_map_.end()) ? 0 : hash_it->second;
        }

        bool newly_insert = hash_it == z_hash_map_.end();
        size_t field_len = field.Length();
        if (hash_it != z_hash_map_.end())
        {
            auto sit =
                z_ordered_set_.find(ZNode(hash_it->first, hash_it->second));
            z_ordered_set_.erase(sit);
            z_hash_map_.erase(hash_it);
        }

        std::pair<
            std::set<RedisZsetObject::ZNode, RedisZsetObject::Cmp>::iterator,
            bool>
            it;
        if (field.Type() == EloqString::StorageType::View ||
            should_not_move_string)
        {
            // copy EloqString for normal commands which store string_view
            it = z_ordered_set_.insert(ZNode(field.Clone(), num));
        }
        else
        {
            // move EloqString for log replay commands and cloned commands
            it = z_ordered_set_.insert(ZNode(std::move(field), num));
        }
        if (newly_insert)
        {
            serialized_length_ += sizeof(double) + sizeof(uint32_t) + field_len;
        }
        z_hash_map_.emplace(it.first->field_.StringView(), num);
    };

    if (std::holds_alternative<std::vector<std::pair<double, EloqString>>>(
            elements))
    {
        for (auto &[num, field] :
             std::get<std::vector<std::pair<double, EloqString>>>(elements))
        {
            update_element(num, field);
        }
    }
    else if (std::holds_alternative<std::pair<double, EloqString>>(elements))
    {
        auto &[num, field] = std::get<std::pair<double, EloqString>>(elements);
        update_element(num, field);
    }
    else
    {
    }

    assert(serialized_length_ <= MAX_OBJECT_SIZE);
}

void RedisZsetObject::CommitZNew(
    std::vector<std::pair<double, EloqString>> &elements)
{
    z_ordered_set_.clear();
    z_hash_map_.clear();

    std::pair<std::set<RedisZsetObject::ZNode, RedisZsetObject::Cmp>::iterator,
              bool>
        it;

    for (auto &[score, field] : elements)
    {
        if (field.Type() == EloqString::StorageType::View)
        {
            // copy EloqString for normal commands which store string_view
            it = z_ordered_set_.insert(ZNode(field.Clone(), score));
        }
        else
        {
            // move EloqString for log replay commands and cloned commands
            it = z_ordered_set_.insert(ZNode(std::move(field), score));
        }

        z_hash_map_.emplace(it.first->field_.StringView(), score);
    }
}

/**
 * This command can be divided into three other
 * @param arg
 * @param spec
 * @return
 */
std::vector<EloqString> RedisZsetObject::GetZrangeResult(ZRangeConstraint &arg,
                                                         ZRangeSpec &spec) const
{
    assert(arg.rangetype_ != ZRangeType::ZRANGE_AUTO);
    switch (arg.rangetype_)
    {
    case ZRangeType::ZRANGE_RANK:
    {
        return ZRangeByRank(arg, spec);
    }
    case ZRangeType::ZRANGE_SCORE:
    {
        return ZRangeByScore(arg, spec);
    }
    case ZRangeType::ZRANGE_LEX:
    {
        return ZRangeByLex(arg, spec);
    }
    default:
        assert(false && "Unknown range type");
        return {};
    }
}

/**
 * get ans between pos_begin and pos_end in rev other
 * @param pos_begin
 * @param pos_end
 * @param withscores output this object_scores behind it
 * @return
 */
std::vector<EloqString> EmplaceRevElements(
    std::set<RedisZsetObject::ZNode>::iterator pos_begin,
    std::set<RedisZsetObject::ZNode>::iterator pos_end,
    int &offset,
    int &limit,
    bool withscores)
{
    auto size = static_cast<int>(std::distance(pos_begin, pos_end));

    auto rev_begin = std::make_reverse_iterator(pos_end);
    auto rev_end = std::make_reverse_iterator(pos_begin);

    if (size <= offset)
    {
        offset -= size;
        return {};
    }

    assert(offset <= size);
    if (offset != 0)
    {
        std::advance(rev_begin, offset);
        offset = 0;
    }

    std::vector<EloqString> ans;

    if (withscores)
    {
        ans.reserve(size * 2);
        for (; rev_begin != rev_end && (limit == -1 || limit > 0); ++rev_begin)
        {
            ans.emplace_back(rev_begin->field_.Clone());
            ans.emplace_back(d2string(rev_begin->score_).data(),
                             d2string(rev_begin->score_).size());

            limit -= limit > 0 ? 1 : 0;
        }
    }
    else
    {
        ans.reserve(size);
        for (; rev_begin != rev_end && (limit == -1 || limit > 0); ++rev_begin)
        {
            ans.emplace_back(rev_begin->field_.Clone());

            limit -= limit > 0 ? 1 : 0;
        }
    }
    return ans;
}

/**
 * get ans between pos_begin ans pos_end in forward other
 * @param pos_begin
 * @param pos_end
 * @param size
 * @param limit
 * @param withscores
 * @return
 */
std::vector<EloqString> EmplaceForwardElements(
    std::set<RedisZsetObject::ZNode>::iterator pos_begin,
    std::set<RedisZsetObject::ZNode>::iterator pos_end,
    int &offset,
    int &limit,
    bool withscores)
{
    auto size = static_cast<int>(std::distance(pos_begin, pos_end));

    if (size <= offset)
    {
        offset -= size;
        return {};
    }

    assert(offset <= size);

    if (offset != 0)
    {
        std::advance(pos_begin, offset);
        offset = 0;
    }

    std::vector<EloqString> ans;

    if (withscores)
    {
        ans.reserve(size * 2);
        for (; pos_begin != pos_end && (limit == -1 || limit > 0); ++pos_begin)
        {
            ans.emplace_back(pos_begin->field_.Clone());
            ans.emplace_back(d2string(pos_begin->score_).data(),
                             d2string(pos_begin->score_).size());

            limit -= limit > 0 ? 1 : 0;
        }
    }
    else
    {
        ans.reserve(size);

        for (; pos_begin != pos_end && (limit == -1 || limit > 0); ++pos_begin)
        {
            ans.emplace_back(pos_begin->field_.Clone());

            limit -= limit > 0 ? 1 : 0;
        }
    }
    return ans;
}

std::vector<EloqString> RedisZsetObject::ZRangeByRank(ZRangeConstraint &arg,
                                                      ZRangeSpec &spec) const
{
    assert(arg.opt_limit_ == -1 && arg.opt_offset_ == 0);

    int pos_start = std::get<int>(spec.opt_start_);
    int pos_end = std::get<int>(spec.opt_end_);
    int zset_size = z_ordered_set_.size();

    if (pos_start < 0)
    {
        pos_start += zset_size;
        if (pos_start < 0)
        {
            pos_start = 0;
        }
    }
    else if (pos_start > 0 && pos_start >= zset_size)
    {
        // Out-of-Bounds
        return {};
    }

    if (pos_end < 0)
    {
        if (-pos_end > zset_size)
        {
            // Out-of-Bounds
            return {};
        }
        pos_end += zset_size;
    }
    else if (pos_end >= zset_size)
    {
        pos_end = zset_size - 1;
    }
    pos_end++;

    if (pos_start >= pos_end)
    {
        return {};
    }

    if (arg.direction_ == ZRangeDirection::ZRANGE_DIRECTION_REVERSE)
    {
        auto start_iter = z_ordered_set_.rbegin();
        auto end_iter = z_ordered_set_.rbegin();
        std::advance(start_iter, pos_start);

        std::advance(end_iter, pos_end);
        std::vector<EloqString> ans;
        uint32_t size = pos_end - pos_start;
        if (arg.opt_withscores_)
        {
            ans.reserve(size * 2);
            for (; start_iter != end_iter; ++start_iter)
            {
                ans.emplace_back(start_iter->field_.Clone());
                if (start_iter->score_ ==
                    std::numeric_limits<double>::infinity())
                {
                    ans.emplace_back("inf");
                }
                else if (start_iter->score_ ==
                         std::numeric_limits<double>::infinity() * -1)
                {
                    ans.emplace_back("-inf");
                }
                else
                {
                    ans.emplace_back(d2string(start_iter->score_).data(),
                                     d2string(start_iter->score_).size());
                }
            }
        }
        else
        {
            ans.reserve(size);
            for (; start_iter != end_iter; ++start_iter)
            {
                ans.emplace_back(start_iter->field_.Clone());
            }
        }
        return ans;
    }
    else
    {
        auto start_iter = z_ordered_set_.begin();
        auto end_iter = z_ordered_set_.begin();
        std::advance(start_iter, pos_start);
        std::advance(end_iter, pos_end);

        return EmplaceForwardElements(start_iter,
                                      end_iter,
                                      arg.opt_offset_,
                                      arg.opt_limit_,
                                      arg.opt_withscores_);
    }
}

std::vector<EloqString> RedisZsetObject::ZRangeByScore(ZRangeConstraint &arg,
                                                       ZRangeSpec &spec) const
{
    double num_start = std::get<double>(spec.opt_start_);
    double num_end = std::get<double>(spec.opt_end_);
    if (num_start > num_end)
        return {};

    auto znode_start_it = spec.minex_ ? z_ordered_set_.upper_bound(num_start)
                                      : z_ordered_set_.lower_bound(num_start);

    auto znode_end_it = spec.maxex_ ? z_ordered_set_.lower_bound(num_end)
                                    : z_ordered_set_.upper_bound(num_end);

    if (arg.direction_ == ZRangeDirection::ZRANGE_DIRECTION_REVERSE)
    {
        return EmplaceRevElements(znode_start_it,
                                  znode_end_it,
                                  arg.opt_offset_,
                                  arg.opt_limit_,
                                  arg.opt_withscores_);
    }
    else
    {
        return EmplaceForwardElements(znode_start_it,
                                      znode_end_it,
                                      arg.opt_offset_,
                                      arg.opt_limit_,
                                      arg.opt_withscores_);
    }
}

int RedisZsetObject::ZCount(ZRangeSpec &spec) const
{
    double num_start = std::get<double>(spec.opt_start_);
    double num_end = std::get<double>(spec.opt_end_);

    if (num_start > num_end)
        return 0;

    auto znode_start_it = spec.minex_ ? z_ordered_set_.upper_bound(num_start)
                                      : z_ordered_set_.lower_bound(num_start);
    auto znode_end_it = spec.maxex_ ? z_ordered_set_.lower_bound(num_end)
                                    : z_ordered_set_.upper_bound(num_end);
    int num = std::distance(znode_start_it, znode_end_it);

    return num;
}

/**
 * process bylex input with ( or [
 * Returns iterators between pos_start and pos_end
 * minex or maxex is true mean this pos is not include
 * @param current_score
 * @param spec
 * @return
 */
std::tuple<bool,
           std::set<RedisZsetObject::ZNode>::iterator,
           std::set<RedisZsetObject::ZNode>::iterator>
RedisZsetObject::GetRangeByLexForCurrentScore(double current_score,
                                              ZRangeSpec &spec) const
{
    std::set<RedisZsetObject::ZNode>::iterator pos_start, pos_end;
    std::string_view query_start_sv =
        std::get<EloqString>(spec.opt_start_).StringView();
    std::string_view query_end_sv =
        std::get<EloqString>(spec.opt_end_).StringView();

    if (spec.negative_)
    {
        pos_start = z_ordered_set_.lower_bound(current_score);
    }
    else if (spec.minex_)
    {
        pos_start =
            z_ordered_set_.upper_bound(ZNode(query_start_sv, current_score));
    }
    else
    {
        pos_start =
            z_ordered_set_.lower_bound(ZNode(query_start_sv, current_score));
    }

    if (spec.positive_)
    {
        pos_end = z_ordered_set_.upper_bound(current_score);
    }
    else if (spec.maxex_)
    {
        pos_end =
            z_ordered_set_.lower_bound(ZNode(query_end_sv, current_score));
    }
    else
    {
        pos_end =
            z_ordered_set_.upper_bound(ZNode(query_end_sv, current_score));
    }

    if (pos_start == z_ordered_set_.end() ||
        pos_start->score_ > current_score ||
        (pos_start->field_.StringView() > query_end_sv && !spec.positive_))
    {
        /*
        nothing to return for the current score(assume it is 0) if:

        1. zset only has one score and query_start_sv is beyond the zset range;
        (zset{0,b,0,c} query_range{[d,[dd})

        2. zset has multiple scores and query_start_sv is beyond the zset range
        for the current score;
        (zset{0,b,0,c,1,bb,1,cc} query_range{[d,[dd})

        3. query_end_sv is less than the smallest lex value in zset for the
        current score;
        (zset{0,b,0,c} query_range{[a,[aa})

        */
        return {false, pos_start, pos_end};
    }

    return {true, pos_start, pos_end};
}

std::vector<EloqString> RedisZsetObject::ZRangeByLex(ZRangeConstraint &arg,
                                                     ZRangeSpec &spec) const
{
    if (spec.null_)
    {
        return {};
    }
    auto fsv = std::get<EloqString>(spec.opt_start_).StringView();
    auto ssv = std::get<EloqString>(spec.opt_end_).StringView();

    if (!spec.negative_ && !spec.positive_ &&
        (fsv > ssv || (fsv == ssv && (spec.minex_ || spec.maxex_))))
    {
        return {};
    }

    std::vector<EloqString> ans;
    if (arg.direction_ == ZRangeDirection::ZRANGE_DIRECTION_REVERSE)
    {
        auto reverse_lex_start = z_ordered_set_.rbegin();

        /**
         * begin in rbegin, search each score one by noe, if revser_lex_start ==
         * rend or limit == 0, break and return.
         * limit == -1 mean infinity, otherwise when input an object limit--
         */
        while (reverse_lex_start != z_ordered_set_.rend() &&
               arg.opt_limit_ != 0)
        {
            double current_score = reverse_lex_start->score_;
            auto [succeed, fs, fe] =
                GetRangeByLexForCurrentScore(current_score, spec);
            if (succeed)
            {
                std::vector<EloqString> this_score_ans =
                    EmplaceRevElements(fs,
                                       fe,
                                       arg.opt_offset_,
                                       arg.opt_limit_,
                                       arg.opt_withscores_);
                std::move(std::make_move_iterator(this_score_ans.begin()),
                          std::make_move_iterator(this_score_ans.end()),
                          std::back_inserter(ans));
            }

            auto next_index = z_ordered_set_.lower_bound(current_score);
            reverse_lex_start = std::make_reverse_iterator(next_index);
        }
    }
    else
    {
        auto lex_start = z_ordered_set_.begin();
        while (lex_start != z_ordered_set_.end() && arg.opt_limit_ != 0)
        {
            double current_score = lex_start->score_;
            auto [succeed, fs, fe] =
                GetRangeByLexForCurrentScore(current_score, spec);
            if (succeed)
            {
                std::vector<EloqString> this_score_ans =
                    EmplaceForwardElements(fs,
                                           fe,
                                           arg.opt_offset_,
                                           arg.opt_limit_,
                                           arg.opt_withscores_);
                std::move(std::make_move_iterator(this_score_ans.begin()),
                          std::make_move_iterator(this_score_ans.end()),
                          std::back_inserter(ans));
            }
            lex_start = z_ordered_set_.upper_bound(current_score);
        }
    }

    return ans;
}

void RedisZsetObject::Execute(ZRangeCommand &cmd) const
{
    RedisZsetResult &result = cmd.zset_result_;
    result.err_code_ = RD_OK;
    result.result_ = GetZrangeResult(cmd.constraint_, cmd.spec_);
}

void RedisZsetObject::Execute(ZCountCommand &cmd) const
{
    RedisZsetResult &result = cmd.zset_result_;
    result.err_code_ = RD_OK;
    result.result_ = ZCount(cmd.spec_);
}

void RedisZsetObject::Execute(ZScanCommand &cmd) const
{
    RedisZsetResult &zscan_result = cmd.zset_result_;
    std::vector<EloqString> scan_ans;

    // *scan_ans.begin() is cursor, set default value "0".
    // If there is unread elements, update cursor with the next record to read.
    scan_ans.emplace_back("0");

    uint64_t count = cmd.count_ > 0 ? cmd.count_ : z_ordered_set_.size();

    auto tmp_it = z_ordered_set_.begin();
    if (cmd.with_cursor_)
    {
        tmp_it = z_ordered_set_.find(
            ZNode(cmd.cursor_field_.StringView(), cmd.cursor_score_));
        if (tmp_it == z_ordered_set_.end())
        {
            // Invalid cursor: the element may be deleted.
            tmp_it = z_ordered_set_.begin();
        }
    }

    if (!cmd.match_)
    {
        // Array reply: [cursor, member,score, member, score, ...]
        scan_ans.reserve(count * 2 + 1);
    }
    for (; tmp_it != z_ordered_set_.end() && count > 0; tmp_it++)
    {
        if (!cmd.match_ || stringmatchlen(cmd.pattern_.Data(),
                                          cmd.pattern_.Length(),
                                          tmp_it->field_.Data(),
                                          tmp_it->field_.Length(),
                                          0))
        {
            scan_ans.emplace_back(tmp_it->field_.Clone());
            scan_ans.emplace_back(d2string(tmp_it->score_).data(),
                                  d2string(tmp_it->score_).size());
            --count;
        }
    }

    // update cursor
    if (tmp_it != z_ordered_set_.end())
    {
        std::string output =
            ZScanCommand::SerializeCursorZNode(tmp_it->field_, tmp_it->score_);
        scan_ans[0] = EloqString(output.data(), output.size());
    }

    zscan_result.result_ = std::move(scan_ans);
    zscan_result.err_code_ = RD_OK;
}

void RedisZsetObject::Execute(ZCardCommand &cmd) const
{
    RedisZsetResult &result = cmd.zset_result_;
    result.err_code_ = RD_OK;
    result.result_ = static_cast<int>(z_ordered_set_.size());
}

bool RedisZsetObject::Execute(ZRemCommand &cmd) const
{
    RedisZsetResult &result = cmd.zset_result_;
    int ans = 0;
    for (auto &it : cmd.elements_)
    {
        ans += z_hash_map_.find(it.StringView()) != z_hash_map_.end();
    }
    result.err_code_ = RD_OK;
    result.result_ = ans;
    return ans > 0;
}

void RedisZsetObject::Execute(SortableLoadCommand &cmd) const
{
    std::vector<std::string> elements;
    elements.reserve(z_hash_map_.size());
    for (const ZNode &znode : z_ordered_set_)
    {
        elements.emplace_back(znode.field_.String());
    }
    cmd.result_.obj_type_ = RedisObjectType::Zset;
    cmd.result_.elems_ = std::move(elements);
}

bool RedisZsetObject::CommitZRem(std::vector<EloqString> &elements)
{
    for (auto &element : elements)
    {
        auto its = z_hash_map_.find(element.StringView());
        if (its != z_hash_map_.end())
        {
            auto it =
                z_ordered_set_.find(ZNode(element.StringView(), its->second));
            assert(it != z_ordered_set_.end());
            serialized_length_ -=
                sizeof(double) + sizeof(uint32_t) + element.Length();
            z_ordered_set_.erase(it);
            z_hash_map_.erase(its);
        }
    }

    assert(serialized_length_ <= MAX_OBJECT_SIZE);

    return z_ordered_set_.empty();
}

int RedisZsetObject::ZRemRangeByScore(ZRangeSpec &spec) const
{
    double num_start = std::get<double>(spec.opt_start_);
    double num_end = std::get<double>(spec.opt_end_);
    if (num_start > num_end)
    {
        return 0;
    }

    auto znode_start_it = spec.minex_ ? z_ordered_set_.upper_bound(num_start)
                                      : z_ordered_set_.lower_bound(num_start);

    auto znode_end_it = spec.maxex_ ? z_ordered_set_.lower_bound(num_end)
                                    : z_ordered_set_.upper_bound(num_end);

    int remove_cnt = 0;
    for (; znode_start_it != znode_end_it; ++znode_start_it)
    {
        remove_cnt++;
    }

    return remove_cnt;
}

int RedisZsetObject::ZRemRangeByLex(ZRangeSpec &spec) const
{
    if (spec.null_)
    {
        return 0;
    }
    auto min_sv = std::get<EloqString>(spec.opt_start_).StringView();
    auto max_sv = std::get<EloqString>(spec.opt_end_).StringView();

    if (!spec.negative_ && !spec.positive_ &&
        (min_sv > max_sv || (min_sv == max_sv && (spec.minex_ || spec.maxex_))))
    {
        return 0;
    }

    int remove_cnt = 0;
    auto lex_start = z_ordered_set_.begin();
    while (lex_start != z_ordered_set_.end())
    {
        // begin from the smallest score
        double current_score = lex_start->score_;

        // get the range from this score
        auto [succeed, lex_start_of_this_score, lex_end_of_this_score] =
            GetRangeByLexForCurrentScore(current_score, spec);

        if (succeed)
        {
            remove_cnt +=
                std::distance(lex_start_of_this_score, lex_end_of_this_score);
        }

        // move to the next score
        lex_start = z_ordered_set_.upper_bound(current_score);
    }
    return remove_cnt;
}

int RedisZsetObject::ZRemRangeByRank(ZRangeSpec &spec) const
{
    // assert(arg.opt_limit_ == -1 && arg.opt_offset_ == 0);
    int remove_cnt = 0;

    int pos_start = std::get<int>(spec.opt_start_);
    int pos_end = std::get<int>(spec.opt_end_);
    int zset_size = z_ordered_set_.size();

    if (pos_start < 0)
    {
        pos_start += zset_size;
        if (pos_start < 0)
        {
            pos_start = 0;
        }
    }
    else if (pos_start > 0 && pos_start >= zset_size)
    {
        // Out-of-Bounds
        return remove_cnt;
    }

    if (pos_end < 0)
    {
        if (-pos_end > zset_size)
        {
            // Out-of-Bounds
            return remove_cnt;
        }
        pos_end += zset_size;
    }
    else if (pos_end >= zset_size)
    {
        pos_end = zset_size - 1;
    }
    pos_end++;

    if (pos_start >= pos_end)
    {
        return remove_cnt;
    }

    auto start_iter = z_ordered_set_.begin();
    auto end_iter = z_ordered_set_.begin();

    std::advance(start_iter, pos_start);
    std::advance(end_iter, pos_end);

    remove_cnt = std::distance(start_iter, end_iter);

    return remove_cnt;
}

bool RedisZsetObject::Execute(ZRemRangeCommand &cmd) const
{
    RedisZsetResult &zset_result = cmd.zset_result_;
    zset_result.err_code_ = RD_OK;

    assert(cmd.range_type_ != ZRangeType::ZRANGE_AUTO);

    int remove_cnt = 0;
    switch (cmd.range_type_)
    {
    case ZRangeType::ZRANGE_SCORE:
    {
        remove_cnt = ZRemRangeByScore(cmd.spec_);
        break;
    }
    case ZRangeType::ZRANGE_LEX:
    {
        remove_cnt = ZRemRangeByLex(cmd.spec_);
        break;
    }
    case ZRangeType::ZRANGE_RANK:
    {
        remove_cnt = ZRemRangeByRank(cmd.spec_);
        break;
    }
    default:
        assert(false && "Unknown range type");
    }
    zset_result.result_ = remove_cnt;
    return remove_cnt > 0;
}

bool RedisZsetObject::CommitZRemRange(ZRangeType &range_type, ZRangeSpec &spec)
{
    assert(range_type != ZRangeType::ZRANGE_AUTO);
    switch (range_type)
    {
    case ZRangeType::ZRANGE_SCORE:
    {
        double num_start = std::get<double>(spec.opt_start_);
        double num_end = std::get<double>(spec.opt_end_);
        if (num_start > num_end)
        {
            return false;
        }

        auto znode_start_it = spec.minex_
                                  ? z_ordered_set_.upper_bound(num_start)
                                  : z_ordered_set_.lower_bound(num_start);

        auto znode_end_it = spec.maxex_ ? z_ordered_set_.lower_bound(num_end)
                                        : z_ordered_set_.upper_bound(num_end);

        for (; znode_start_it != znode_end_it;)
        {
            auto z_hash_map_it =
                z_hash_map_.find(znode_start_it->field_.StringView());
            assert(z_hash_map_it != z_hash_map_.end());
            serialized_length_ -= sizeof(double) + sizeof(uint32_t) +
                                  znode_start_it->field_.Length();
            z_hash_map_.erase(z_hash_map_it);
            znode_start_it = z_ordered_set_.erase(znode_start_it);
        }

        assert(serialized_length_ <= MAX_OBJECT_SIZE);

        return z_ordered_set_.empty();
    }
    case ZRangeType::ZRANGE_LEX:
    {
        auto min_sv = std::get<EloqString>(spec.opt_start_).StringView();
        auto max_sv = std::get<EloqString>(spec.opt_end_).StringView();

        if (!spec.negative_ && !spec.positive_ &&
            (min_sv > max_sv ||
             (min_sv == max_sv && (spec.minex_ || spec.maxex_))))
        {
            return false;
        }

        auto lex_start = z_ordered_set_.begin();
        while (lex_start != z_ordered_set_.end())
        {
            // begin from the smallest score
            double current_score = lex_start->score_;

            // get the range from this score
            auto [succeed, lex_start_of_this_score, lex_end_of_this_score] =
                GetRangeByLexForCurrentScore(current_score, spec);

            if (succeed)
            {
                for (; lex_start_of_this_score != lex_end_of_this_score;)
                {
                    auto z_hash_map_it = z_hash_map_.find(
                        lex_start_of_this_score->field_.StringView());
                    assert(z_hash_map_it != z_hash_map_.end());
                    serialized_length_ -=
                        sizeof(double) + sizeof(uint32_t) +
                        lex_start_of_this_score->field_.Length();
                    z_hash_map_.erase(z_hash_map_it);
                    lex_start_of_this_score =
                        z_ordered_set_.erase(lex_start_of_this_score);
                }
            }

            // move to the next score
            lex_start = z_ordered_set_.upper_bound(current_score);
        }

        assert(serialized_length_ <= MAX_OBJECT_SIZE);

        return z_ordered_set_.empty();
    }
    case ZRangeType::ZRANGE_RANK:
    {
        int pos_start = std::get<int>(spec.opt_start_);
        int pos_end = std::get<int>(spec.opt_end_);
        int zset_size = z_ordered_set_.size();

        if (pos_start < 0)
        {
            pos_start += zset_size;
            if (pos_start < 0)
            {
                pos_start = 0;
            }
        }
        else if (pos_start > 0 && pos_start >= zset_size)
        {
            // Out-of-Bounds
            return false;
        }

        if (pos_end < 0)
        {
            if (-pos_end > zset_size)
            {
                // Out-of-Bounds
                return false;
            }
            pos_end += zset_size;
        }
        else if (pos_end >= zset_size)
        {
            pos_end = zset_size - 1;
        }
        pos_end++;

        if (pos_start >= pos_end)
        {
            return false;
        }

        auto start_iter = z_ordered_set_.begin();
        auto end_iter = z_ordered_set_.begin();

        std::advance(start_iter, pos_start);
        std::advance(end_iter, pos_end);

        for (; start_iter != end_iter;)
        {
            auto z_hash_map_it =
                z_hash_map_.find(start_iter->field_.StringView());
            assert(z_hash_map_it != z_hash_map_.end());
            serialized_length_ -=
                sizeof(double) + sizeof(uint32_t) + start_iter->field_.Length();
            z_hash_map_.erase(z_hash_map_it);
            start_iter = z_ordered_set_.erase(start_iter);
        }

        assert(serialized_length_ <= MAX_OBJECT_SIZE);

        return z_ordered_set_.empty();
    }
    default:
        assert(false && "Unknown range type");
        return false;
    }
}

void RedisZsetObject::Execute(ZScoreCommand &cmd) const
{
    RedisZsetResult &result = cmd.zset_result_;
    auto place = z_hash_map_.find(cmd.element_.StringView());
    result.err_code_ = RD_OK;
    if (place == z_hash_map_.end())
    {
        result.err_code_ = RD_NIL;
    }
    else
    {
        result.result_ = EloqString(d2string(place->second).data(),
                                    d2string(place->second).size());
    }
}

bool RedisZsetObject::Execute(ZPopCommand &cmd) const
{
    RedisZsetResult &result = cmd.zset_result_;

    std::vector<EloqString> ans;
    if (cmd.pop_type_ == ZPopCommand::PopType::POPMIN)
    {
        auto iter = z_ordered_set_.begin();
        for (int64_t idx = 0; idx < cmd.count_ && iter != z_ordered_set_.end();
             ++idx, ++iter)
        {
            ans.emplace_back(iter->field_.Clone());
            ans.emplace_back(d2string(iter->score_).data(),
                             d2string(iter->score_).size());
        }
    }
    else
    {
        auto iter = z_ordered_set_.rbegin();
        for (int64_t idx = 0; idx < cmd.count_ && iter != z_ordered_set_.rend();
             ++idx, ++iter)
        {
            ans.emplace_back(iter->field_.Clone());
            ans.emplace_back(d2string(iter->score_).data(),
                             d2string(iter->score_).size());
        }
    }

    bool success = !ans.empty();
    result.err_code_ = RD_OK;
    result.result_ = std::move(ans);
    return success;
}

bool RedisZsetObject::CommitZPop(ZPopCommand::PopType pop_type, int64_t count)
{
    // Remove all fields
    if (count >= static_cast<int64_t>(z_ordered_set_.size()))
    {
        z_ordered_set_.clear();
        z_hash_map_.clear();
        serialized_length_ = 1 + sizeof(uint32_t);
        return true;
    }

    if (pop_type == ZPopCommand::PopType::POPMIN)
    {
        auto iter = z_ordered_set_.begin();
        for (int64_t idx = 0; idx < count && iter != z_ordered_set_.end();
             ++idx, ++iter)
        {
            serialized_length_ -=
                sizeof(double) + sizeof(uint32_t) + iter->field_.Length();
            z_hash_map_.erase(iter->field_.StringView());
        }

        auto end_iter = z_ordered_set_.begin();
        std::advance(end_iter, count);
        z_ordered_set_.erase(z_ordered_set_.begin(), end_iter);
    }
    else
    {
        assert(pop_type == ZPopCommand::PopType::POPMAX);
        auto iter = z_ordered_set_.rbegin();
        for (int64_t idx = 0; idx < count && iter != z_ordered_set_.rend();
             ++idx, ++iter)
        {
            serialized_length_ -=
                sizeof(double) + sizeof(uint32_t) + iter->field_.Length();
            z_hash_map_.erase(iter->field_.StringView());
        }

        auto start_iter = z_ordered_set_.begin();
        std::advance(start_iter, z_ordered_set_.size() - count);
        z_ordered_set_.erase(start_iter, z_ordered_set_.end());
    }

    assert(serialized_length_ <= MAX_OBJECT_SIZE);

    return z_ordered_set_.empty();
}

void RedisZsetObject::Execute(ZLexCountCommand &cmd) const
{
    RedisZsetResult &result = cmd.zset_result_;
    result.err_code_ = RD_OK;

    auto &spec = cmd.spec_;
    if (spec.null_)
    {
        result.result_ = 0;
        return;
    }

    assert(std::holds_alternative<EloqString>(spec.opt_start_));
    assert(std::holds_alternative<EloqString>(spec.opt_end_));

    auto fsv = std::get<EloqString>(spec.opt_start_).StringView();
    auto ssv = std::get<EloqString>(spec.opt_end_).StringView();

    if (!spec.negative_ && !spec.positive_ &&
        (fsv > ssv || (fsv == ssv && (spec.minex_ || spec.maxex_))))
    {
        result.result_ = 0;
        return;
    }

    size_t count = 0;

    auto lex_start = z_ordered_set_.begin();
    while (lex_start != z_ordered_set_.end())
    {
        double current_score = lex_start->score_;
        auto [succeed, fs, fe] =
            GetRangeByLexForCurrentScore(current_score, spec);
        if (succeed)
        {
            count += std::distance(fs, fe);
        }
        lex_start = z_ordered_set_.upper_bound(current_score);
    }

    result.result_ = static_cast<int>(count);
}

void RedisZsetObject::Execute(ZRandMemberCommand &cmd) const
{
    RedisZsetResult &result = cmd.zset_result_;
    result.err_code_ = RD_OK;

    int32_t sz = static_cast<int32_t>(z_hash_map_.size());
    bool distinct = cmd.count_ >= 0;
    int64_t count =
        cmd.count_ >= 0 ? (cmd.count_ < sz ? cmd.count_ : sz) : -cmd.count_;

    int32_t z_hash_map_index = 0;
    std::vector<EloqString> string_list;
    if (cmd.with_scores_)
    {
        string_list.resize(count * 2);
    }
    else
    {
        string_list.resize(count);
    }

    auto resulter = [](const std::string_view &key,
                       double score,
                       bool with_scores,
                       size_t idx,
                       std::vector<EloqString> &result)
    {
        if (with_scores)
        {
            result[idx * 2] = EloqString(key.data(), key.size());
            if (score == std::numeric_limits<double>::infinity())
            {
                result[idx * 2 + 1] = EloqString("inf");
            }
            else if (score == std::numeric_limits<double>::infinity() * -1)
            {
                result[idx * 2 + 1] = EloqString("-inf");
            }
            else
            {
                int64_t floor_integer = std::floor(score);
                if (score == floor_integer)
                {
                    result[idx * 2 + 1] =
                        EloqString(std::to_string(floor_integer).data(),
                                   std::to_string(floor_integer).size());
                }
                else
                {
                    result[idx * 2 + 1] =
                        EloqString(std::to_string(score).data(),
                                   std::to_string(score).size());
                }
            }
        }
        else
        {
            result[idx] = EloqString(key.data(), key.size());
        }
    };

    if (sz > 0 && count > 0 && sz / count > 100 &&
        z_hash_map_.bucket_count() > static_cast<size_t>(count))
    {
        /*
        absl::flat_hash_set<size_t> visited;
        size_t bucket_cnt = z_hash_map_.bucket_count();
        while (z_hash_map_index < count)
        {
            uint64_t idx = butil::fast_rand() % bucket_cnt;
            if (distinct)
            {
                if (visited.contains(idx))
                {
                    continue;
                }

                visited.insert(idx);
            }

            for (auto it = z_hash_map_.begin(idx);
                 it != z_hash_map_.end(idx) && z_hash_map_index < count;
                 ++it)
            {
                resulter(it->first,
                         it->second,
                         cmd.with_scores_,
                         z_hash_map_index,
                         string_list);

                z_hash_map_index++;
            }
        }

        result.result_ = std::move(string_list);
        */

        auto hiter = z_hash_map_.begin();
        while (z_hash_map_index < count)
        {
            resulter(hiter->first,
                     hiter->second,
                     cmd.with_scores_,
                     z_hash_map_index,
                     string_list);

            ++z_hash_map_index;
            ++hiter;

            if (hiter == z_hash_map_.end())
            {
                hiter = z_hash_map_.begin();
            }
        }

        result.result_ = std::move(string_list);
    }
    else
    {
        std::multimap<int32_t, int64_t> map;
        GenRandMap(sz, count, !distinct, map);

        auto z_hash_map_it = z_hash_map_.begin();

        for (auto miter = map.begin(); miter != map.end(); miter++)
        {
            while (z_hash_map_index < miter->first)
            {
                z_hash_map_index++;
                z_hash_map_it++;
            }
            assert(z_hash_map_index == miter->first);

            resulter(z_hash_map_it->first,
                     z_hash_map_it->second,
                     cmd.with_scores_,
                     miter->second,
                     string_list);
        }

        result.result_ = std::move(string_list);
    }
}

void RedisZsetObject::Execute(ZRankCommand &cmd) const
{
    RedisZsetResult &zset_result = cmd.zset_result_;

    auto z_hash_map_it = z_hash_map_.find(cmd.member_.StringView());
    if (z_hash_map_it != z_hash_map_.end())
    {
        int32_t rank;
        auto z_ordered_set_it = z_ordered_set_.find(
            ZNode(cmd.member_.StringView(), z_hash_map_it->second));
        if (cmd.is_rev_)
        {
            rank = std::distance(z_ordered_set_it, z_ordered_set_.end()) - 1;
        }
        else
        {
            rank = std::distance(z_ordered_set_.begin(), z_ordered_set_it);
        }

        if (cmd.with_score_)
        {
            std::pair<int32_t, std::string> pr;
            pr.first = rank;
            pr.second = d2string(z_hash_map_it->second);

            zset_result.result_ = std::move(pr);
            zset_result.err_code_ = RD_OK;
        }
        else
        {
            zset_result.result_ = rank;
            zset_result.err_code_ = RD_OK;
        }
    }
    else
    {
        zset_result.err_code_ = RD_NIL;
    }
}

void RedisZsetObject::Execute(ZMScoreCommand &cmd) const
{
    RedisZsetResult &result = cmd.zset_result_;
    result.err_code_ = RD_OK;
    std::vector<EloqString> vct_res;

    for (const auto &mon : cmd.vct_elm_)
    {
        auto iter = z_hash_map_.find(mon.StringView());
        if (iter == z_hash_map_.end())
        {
            vct_res.push_back(EloqString());
        }
        else
        {
            vct_res.push_back(EloqString(d2string(iter->second).data(),
                                         d2string(iter->second).size()));
        }
    }

    result.result_ = std::move(vct_res);
}

void RedisZsetObject::Execute(SZScanCommand &cmd) const
{
    RedisSetScanResult &set_scan_result = cmd.set_scan_result_;
    set_scan_result.err_code_ = RD_OK;

    if (cmd.withscores_)
    {
        std::vector<std::pair<EloqString, double>> result;

        for (const auto &znode : z_ordered_set_)
        {
            result.emplace_back(znode.field_.Clone(), znode.score_);
        }

        set_scan_result.result_ = std::move(result);
    }
    else
    {
        std::vector<EloqString> result;
        for (const auto &znode : z_ordered_set_)
        {
            result.emplace_back(znode.field_.Clone());
        }
        set_scan_result.result_ = std::move(result);
    }
}

}  // namespace EloqKV
