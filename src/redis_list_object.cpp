#include "redis_list_object.h"

#include <redis_string_object.h>

#include <string>
#include <utility>

#include "butil/logging.h"
#include "redis_errors.h"
#include "redis_string_num.h"

namespace EloqKV
{
extern const uint64_t MAX_OBJECT_SIZE;

RedisListObject::RedisListObject()
{
    serialized_length_ = 1 + sizeof(uint32_t);
}

RedisListObject::RedisListObject(const RedisListObject &rhs)
    : serialized_length_(rhs.serialized_length_)
{
    for (const auto &str : rhs.list_object_)
    {
        // Deep copy the EloqString.
        std::string_view sv = str.StringView();
        list_object_.emplace_back(sv.data(), sv.size());
    }
}

RedisListObject::RedisListObject(RedisListObject &&rhs)
    : RedisEloqObject(std::move(rhs)),
      list_object_(std::move(rhs.list_object_)),
      serialized_length_(rhs.serialized_length_)
{
    rhs.serialized_length_ = 1 + sizeof(uint32_t);
}

bool RedisListObject::ConvertListIndex(int64_t &index) const
{
    if (index < 0)
    {
        index = (int64_t) list_object_.size() + index;
        if (index < 0)
        {
            index = 0;
            return true;
        }
    }
    return false;
}

txservice::TxRecord::Uptr RedisListObject::AddTTL(uint64_t ttl)
{
    return std::make_unique<RedisListTTLObject>(std::move(*this), ttl);
}

bool RedisListObject::Execute(RPushCommand &cmd) const
{
    RedisListResult &result = cmd.result_;
    uint64_t len_increased = 0;
    for (const auto &ele : cmd.elements_)
    {
        len_increased += sizeof(uint32_t) + ele.Length();
    }
    if (serialized_length_ + len_increased > MAX_OBJECT_SIZE)
    {
        result.err_code_ = RD_ERR_OBJECT_TOO_BIG;
        return false;
    }

    result.ret_ = list_object_.size() + cmd.elements_.size();
    result.err_code_ = RD_OK;
    return true;
}

bool RedisListObject::Execute(LPushCommand &cmd) const
{
    RedisListResult &result = cmd.result_;
    uint64_t len_increased = 0;
    for (const auto &ele : cmd.elements_)
    {
        len_increased += sizeof(uint32_t) + ele.Length();
    }
    if (serialized_length_ + len_increased > MAX_OBJECT_SIZE)
    {
        result.err_code_ = RD_ERR_OBJECT_TOO_BIG;
        return false;
    }

    result.ret_ = list_object_.size() + cmd.elements_.size();
    result.err_code_ = RD_OK;
    return true;
}

void RedisListObject::Execute(LRangeCommand &cmd) const
{
    RedisListResult &result = cmd.result_;
    int64_t left_index = cmd.start_;
    int64_t right_index = cmd.end_;

    ConvertListIndex(left_index);
    bool right_index_out_of_range = ConvertListIndex(right_index);

    if (left_index > right_index ||
        left_index >= static_cast<int64_t>(list_object_.size()) ||
        right_index_out_of_range)
    {
        result.result_ = std::vector<std::string>();
        result.ret_ = 0;
        result.err_code_ = RD_OK;
        return;
    }

    int64_t idx = left_index;
    std::vector<std::string> elements;
    for (auto it = list_object_.begin() + left_index;
         it != list_object_.end() && idx <= right_index;
         ++it)
    {
        std::string_view element = it->StringView();
        elements.emplace_back(element.data(), element.size());
        ++idx;
    }
    result.result_ = std::move(elements);

    result.ret_ = right_index - left_index + 1;
    result.err_code_ = RD_OK;
}

void RedisListObject::Execute(LPopCommand &cmd) const
{
    RedisListResult &result = cmd.result_;
    size_t idx = 0;
    int64_t count = cmd.count_ == -1 ? 1 : cmd.count_;
    assert(count >= 0);
    size_t num = std::min(static_cast<size_t>(count), list_object_.size());
    std::vector<std::string> elements;
    for (auto it = list_object_.begin(); it != list_object_.end() && idx < num;
         ++it, idx++)
    {
        std::string_view element = it->StringView();
        elements.emplace_back(element.data(), element.size());
    }
    result.result_ = std::move(elements);

    result.ret_ = num;
    result.err_code_ = RD_OK;
}

void RedisListObject::Execute(RPopCommand &cmd) const
{
    RedisListResult &result = cmd.result_;
    size_t idx = 0;
    int64_t count = cmd.count_ == -1 ? 1 : cmd.count_;
    assert(count >= 0);
    size_t num = std::min(static_cast<size_t>(count), list_object_.size());
    std::vector<std::string> elements;
    for (auto it = list_object_.rbegin();
         it != list_object_.rend() && idx < num;
         ++it, idx++)
    {
        std::string_view element = it->StringView();
        elements.emplace_back(element.data(), element.size());
    }
    result.result_ = std::move(elements);

    result.ret_ = num;
    result.err_code_ = RD_OK;
}

bool RedisListObject::Execute(LMovePopCommand &cmd) const
{
    RedisListResult &list_result = cmd.result_;

    if (list_object_.empty())
    {
        list_result.ret_ = 0;
        return false;
    }

    if (cmd.is_left_)
    {
        list_result.result_ = list_object_.begin()->Clone();
    }
    else
    {
        list_result.result_ = list_object_.rbegin()->Clone();
    }

    list_result.ret_ = 1;
    list_result.err_code_ = RD_OK;
    return true;
}

bool RedisListObject::Execute(LMovePushCommand &cmd) const
{
    RedisListResult &list_result = cmd.result_;

    if (serialized_length_ + cmd.element_.Length() > MAX_OBJECT_SIZE)
    {
        list_result.err_code_ = RD_ERR_OBJECT_TOO_BIG;
        return false;
    }

    list_result.result_ = cmd.element_.Clone();
    list_result.ret_ = 1;
    list_result.err_code_ = RD_OK;
    return true;
}

void RedisListObject::Execute(LLenCommand &cmd) const
{
    RedisListResult &result = cmd.result_;
    result.ret_ = list_object_.size();
}

void RedisListObject::Execute(LTrimCommand &cmd) const
{
    // start and end can also be negative numbers indicating offsets from the
    // end of the list, where -1 is the last element of the list, -2 the
    // penultimate element and so on.
    // Here will convert the negative number to positive number and will change
    // them to suitable value if they exceed the list range.
    RedisListResult &result = cmd.result_;
    // If the start < 0, it will convert to positive value follow the rule
    // above. If the result of start still exceed the range of 0, it will set to
    // 0.
    if (cmd.start_ < 0)
    {
        cmd.start_ = list_object_.size() + cmd.start_;
        if (cmd.start_ < 0)
        {
            cmd.start_ = 0;
        }
    }
    // If start > list size, it will set to list size.
    else if (cmd.start_ > static_cast<int64_t>(list_object_.size()))
    {
        cmd.start_ = list_object_.size();
    }
    // If the end is less than 0, it will convert the positive value following
    // the above rule. If the result of end is still less than 0, set end to 0
    // and set start to 1 to remove all elements in the list.
    if (cmd.end_ < 0)
    {
        cmd.end_ = list_object_.size() + cmd.end_;
        if (cmd.end_ < 0)
        {
            cmd.start_ = 1;
            cmd.end_ = 0;
        }
    }
    // If end > list size, it will set to list size - 1, because the end element
    // is include in the result, so it is the last element's position.
    else if (cmd.end_ >= static_cast<int64_t>(list_object_.size()))
    {
        cmd.end_ = list_object_.size() - 1;
    }
    // Here to calc the number of surplus elements. Because the range include
    // start and end elements, so it will add 1.
    result.ret_ = cmd.end_ - cmd.start_ + 1;
}

void RedisListObject::Execute(LIndexCommand &cmd) const
{
    RedisListResult &result = cmd.result_;
    if (cmd.index_ < 0)
    {
        cmd.index_ = list_object_.size() + cmd.index_;
        if (cmd.index_ < 0)
        {
            result.err_code_ = RD_NIL;
            return;
        }
    }
    else if (cmd.index_ >= static_cast<int64_t>(list_object_.size()))
    {
        result.err_code_ = RD_NIL;
        return;
    }
    std::string_view element = list_object_[cmd.index_].StringView();
    std::vector<std::string> elements;
    elements.emplace_back(element.data(), element.size());
    result.result_ = std::move(elements);
    result.ret_ = 1;
    result.err_code_ = RD_OK;
}

bool RedisListObject::Execute(LInsertCommand &cmd) const
{
    RedisListResult &result = cmd.result_;

    for (auto iter = list_object_.begin(); iter != list_object_.end(); ++iter)
    {
        if (*iter == cmd.pivot_)
        {
            // pivot found
            if (serialized_length_ + sizeof(uint32_t) + cmd.element_.Length() >
                MAX_OBJECT_SIZE)
            {
                result.ret_ = -1;
                result.err_code_ = RD_ERR_OBJECT_TOO_BIG;
                return false;
            }
            result.ret_ = list_object_.size() + 1;
            result.err_code_ = RD_OK;
            return true;
        }
    }

    // pivot not found
    result.ret_ = -1;
    result.err_code_ = RD_NIL;
    return false;
}

void RedisListObject::Execute(LPosCommand &cmd) const
{
    RedisListResult &list_result = cmd.result_;
    int64_t count = cmd.count_ == -1 ? 1 : cmd.count_;
    assert(count >= 0);
    int match_cnt = 0;

    // If MAXLEN is specified adjust the range to search
    int maxlen = cmd.len_ > 0 ? cmd.len_ : list_object_.size();

    std::vector<int64_t> elements;

    // If RANK is negative, start from the end of the list
    if (cmd.rank_ < 0)
    {
        auto reverse_iter = list_object_.rbegin();
        for (int pos = -1;
             pos >= -maxlen && reverse_iter != list_object_.rend();
             --pos, ++reverse_iter)
        {
            if (*reverse_iter == cmd.element_ &&
                ++match_cnt >= std::abs(cmd.rank_))
            {
                int64_t index = list_object_.size() + pos;
                elements.emplace_back(index);
                if (elements.size() == static_cast<size_t>(count))
                {
                    list_result.err_code_ = RD_OK;
                    break;
                }
            }
        }
    }
    else
    {
        auto iter = list_object_.begin();
        for (int index = 0; index < maxlen && iter != list_object_.end();
             ++index, ++iter)
        {
            if (*iter == cmd.element_ && ++match_cnt >= cmd.rank_)
            {
                elements.emplace_back(index);
                if (elements.size() == static_cast<size_t>(count))
                {
                    list_result.err_code_ = RD_OK;
                    break;
                }
            }
        }

        /*
        auto iter = std::lower_bound(
            list_object_.begin(), list_object_.end(), cmd.element_);
        while (iter != list_object_.end() && *iter == cmd.element_ &&
               match_cnt < cmd.rank_ && elements.size() < cmd.count_)
        {
            elements.emplace_back(std::distance(list_object_.begin(), iter));
            ++iter;
            ++match_cnt;
        }
        */
    }
    list_result.result_ = std::move(elements);

    // If no matches were found, set the error code to RD_NIL
    if (std::get<std::vector<int64_t>>(list_result.result_).empty())
    {
        list_result.err_code_ = RD_NIL;
    }
    else
    {
        list_result.err_code_ = RD_OK;
    }
}

bool RedisListObject::Execute(LSetCommand &cmd) const
{
    RedisListResult &result = cmd.result_;

    if (cmd.index_ < 0)
    {
        ConvertListIndex(cmd.index_);
    }

    if (cmd.index_ >= 0 &&
        cmd.index_ < static_cast<int64_t>(list_object_.size()))
    {
        auto iter = list_object_.begin();
        std::advance(iter, cmd.index_);
        if (serialized_length_ - iter->Length() + cmd.element_.Length() >
            MAX_OBJECT_SIZE)
        {
            result.ret_ = 0;
            result.err_code_ = RD_ERR_OBJECT_TOO_BIG;
            return false;
        }
        result.ret_ = 1;
        result.err_code_ = RD_OK;
        return true;
    }
    else
    {
        result.ret_ = 0;
        result.err_code_ = RD_ERR_INDEX_OUT_OF_RANGE;
        return false;
    }
}

void RedisListObject::Execute(LRemCommand &cmd) const
{
    RedisListResult &list_result = cmd.result_;

    list_result.ret_ = 0;
    int to_be_removed_cnt = std::abs(cmd.count_);
    int removed_cnt = 0;

    // If COUNT is negative, start from the end of the list
    if (cmd.count_ < 0)
    {
        auto reverse_iter = list_object_.rbegin();
        for (; reverse_iter != list_object_.rend() &&
               removed_cnt < to_be_removed_cnt;
             ++reverse_iter)
        {
            if (*reverse_iter == cmd.element_)
            {
                list_result.ret_++;
                removed_cnt++;
            }
        }
    }
    else if (cmd.count_ > 0)
    {
        auto iter = list_object_.begin();
        for (; iter != list_object_.end() && removed_cnt < to_be_removed_cnt;
             ++iter)
        {
            if (*iter == cmd.element_)
            {
                list_result.ret_++;
                removed_cnt++;
            }
        }
    }
    else
    {
        auto iter = list_object_.begin();
        for (; iter != list_object_.end(); ++iter)
        {
            if (*iter == cmd.element_)
            {
                list_result.ret_++;
            }
        }
    }
}

bool RedisListObject::Execute(LPushXCommand &cmd) const
{
    RedisListResult &result = cmd.result_;
    uint64_t len_increased = 0;
    for (const auto &ele : cmd.elements_)
    {
        len_increased += sizeof(uint32_t) + ele.Length();
    }
    if (serialized_length_ + len_increased > MAX_OBJECT_SIZE)
    {
        result.err_code_ = RD_ERR_OBJECT_TOO_BIG;
        return false;
    }

    result.ret_ = list_object_.size() + cmd.elements_.size();
    return true;
}

bool RedisListObject::Execute(RPushXCommand &cmd) const
{
    RedisListResult &result = cmd.result_;
    uint64_t len_increased = 0;
    for (const auto &ele : cmd.elements_)
    {
        len_increased += sizeof(uint32_t) + ele.Length();
    }
    if (serialized_length_ + len_increased > MAX_OBJECT_SIZE)
    {
        result.err_code_ = RD_ERR_OBJECT_TOO_BIG;
        return false;
    }

    result.ret_ = list_object_.size() + cmd.elements_.size();
    return true;
}

void RedisListObject::Execute(SortableLoadCommand &cmd) const
{
    std::vector<std::string> elements;
    elements.reserve(list_object_.size());
    for (const EloqString &element : list_object_)
    {
        elements.emplace_back(element.String());
    }
    cmd.result_.obj_type_ = RedisObjectType::List;
    cmd.result_.elems_ = std::move(elements);
}

bool RedisListObject::Execute(BlockLPopCommand &cmd) const
{
    RedisListResult &result = cmd.result_;
    if (list_object_.size() > 0)
    {
        std::vector<std::string> elements;
        elements.reserve(cmd.count_);
        uint32_t cnt = static_cast<uint32_t>(list_object_.size());
        if (cnt > cmd.count_)
        {
            cnt = cmd.count_;
        }

        uint32_t i = 0;
        if (cmd.is_left_)
        {
            for (auto iter = list_object_.begin(); i < cnt; iter++, i++)
            {
                elements.push_back(iter->String());
            }
        }
        else
        {
            for (auto iter = list_object_.rbegin(); i < cnt; iter++, i++)
            {
                elements.push_back(iter->String());
            }
        }

        result.ret_ = elements.size();
        result.result_ = std::move(elements);
        return true;
    }
    else
    {
        return false;
    }
}

void RedisListObject::CommitLInsert(bool is_before,
                                    EloqString &pivot,
                                    EloqString &element)
{
    auto iter = list_object_.begin();
    for (; iter != list_object_.end(); ++iter)
    {
        if (*iter == pivot)
        {
            // pivot found
            if (!is_before)
            {
                ++iter;
            }
            break;
        }
    }

    serialized_length_ += sizeof(uint32_t) + element.Length();
    assert(serialized_length_ <= MAX_OBJECT_SIZE);
    if (element.Type() == EloqString::StorageType::View)
    {
        // copy EloqString for normal commands which store string_view
        list_object_.emplace(iter, element.Clone());
    }
    else
    {
        // move EloqString for log replay commands and cloned commands
        list_object_.emplace(iter, std::move(element));
    }
}

void RedisListObject::CommitLSet(int64_t index, EloqString &element)
{
    if (index < 0)
    {
        ConvertListIndex(index);
    }
    assert(index >= 0 && index < static_cast<int64_t>(list_object_.size()));

    auto iter = list_object_.begin();
    std::advance(iter, index);

    serialized_length_ = serialized_length_ - iter->Length() + element.Length();
    assert(serialized_length_ <= MAX_OBJECT_SIZE);
    if (element.Type() == EloqString::StorageType::View)
    {
        // copy EloqString for normal commands which store string_view
        *iter = element.Clone();
    }
    else
    {
        // move EloqString for log replay commands and cloned commands
        *iter = std::move(element);
    }
}

void RedisListObject::CommitRPush(std::vector<EloqString> &elements)
{
    for (auto &element : elements)
    {
        serialized_length_ += sizeof(uint32_t) + element.Length();
        assert(serialized_length_ <= MAX_OBJECT_SIZE);

        if (element.Type() == EloqString::StorageType::View)
        {
            // copy EloqString for normal commands which store string_view
            list_object_.emplace_back(element.Clone());
        }
        else
        {
            // move EloqString for log replay commands and cloned commands
            list_object_.emplace_back(std::move(element));
        }
    }
}

void RedisListObject::CommitLPush(std::vector<EloqString> &elements)
{
    for (auto &element : elements)
    {
        serialized_length_ += sizeof(uint32_t) + element.Length();
        assert(serialized_length_ <= MAX_OBJECT_SIZE);

        if (element.Type() == EloqString::StorageType::View)
        {
            // copy EloqString for normal commands which store string_view
            list_object_.emplace_front(element.Clone());
        }
        else
        {
            // move EloqString for cloned or log replay commands
            list_object_.emplace_front(std::move(element));
        }
    }
}

bool RedisListObject::CommitLPop(int64_t count)
{
    size_t pop_num = std::min(list_object_.size(), static_cast<size_t>(count));
    auto pop_end_it = list_object_.begin() + pop_num;

    for (auto it = list_object_.begin(); it != pop_end_it; ++it)
    {
        serialized_length_ -= (sizeof(uint32_t) + it->Length());
    }
    assert(serialized_length_ <= MAX_OBJECT_SIZE);

    list_object_.erase(list_object_.begin(), pop_end_it);
    return list_object_.empty();
}

bool RedisListObject::CommitRPop(int64_t count)
{
    auto pop_begin = list_object_.rbegin();
    std::advance(pop_begin,
                 std::min(static_cast<size_t>(count), list_object_.size()));

    for (auto it = pop_begin.base(); it != list_object_.end(); ++it)
    {
        serialized_length_ -= (sizeof(uint32_t) + it->Length());
    }
    assert(serialized_length_ <= MAX_OBJECT_SIZE);

    list_object_.erase(pop_begin.base(), list_object_.end());
    return list_object_.empty();
}

bool RedisListObject::CommitLMovePop(bool is_left)
{
    assert(!list_object_.empty());
    if (is_left)
    {
        serialized_length_ -=
            (sizeof(uint32_t) + list_object_.front().Length());
        list_object_.pop_front();
    }
    else
    {
        serialized_length_ -= (sizeof(uint32_t) + list_object_.back().Length());
        list_object_.pop_back();
    }
    return list_object_.empty();
}

void RedisListObject::CommitLMovePush(bool is_left,
                                      EloqString &element,
                                      bool should_not_move_string)
{
    serialized_length_ += sizeof(uint32_t) + element.Length();
    assert(serialized_length_ <= MAX_OBJECT_SIZE);
    if (is_left)
    {
        if (element.Type() == EloqString::StorageType::View ||
            should_not_move_string)
        {
            // copy EloqString for normal commands which store string_view
            list_object_.emplace_front(element.Clone());
        }
        else
        {
            // move EloqString for cloned or log replay commands
            list_object_.emplace_front(std::move(element));
        }
    }
    else
    {
        if (element.Type() == EloqString::StorageType::View ||
            should_not_move_string)
        {
            // copy EloqString for normal commands which store string_view
            list_object_.emplace_back(element.Clone());
        }
        else
        {
            // move EloqString for cloned or log replay commands
            list_object_.emplace_back(std::move(element));
        }
    }
}

bool RedisListObject::CommitLTrim(int64_t start, int64_t end)
{
    if (end + 1 < static_cast<int64_t>(list_object_.size()))
    {
        auto erase_begin = list_object_.begin() + end + 1;
        for (auto it = erase_begin; it != list_object_.end(); ++it)
        {
            serialized_length_ -= sizeof(uint32_t) + it->Length();
        }
        list_object_.erase(erase_begin, list_object_.end());
    }
    if (start > 0)
    {
        auto erase_end = list_object_.begin() + start;
        for (auto it = list_object_.begin(); it != erase_end; ++it)
        {
            serialized_length_ -= sizeof(uint32_t) + it->Length();
        }
        list_object_.erase(list_object_.begin(), erase_end);
    }
    assert(serialized_length_ <= MAX_OBJECT_SIZE);
    return list_object_.empty();
}

bool RedisListObject::CommitLRem(int64_t count, EloqString &element)
{
    int to_be_removed_cnt = std::abs(count);
    int removed_cnt = 0;
    if (count > 0)
    {
        auto iter = list_object_.begin();
        while (iter != list_object_.end() && removed_cnt < to_be_removed_cnt)
        {
            if (*iter == element)
            {
                serialized_length_ -= sizeof(uint32_t) + iter->Length();
                iter = list_object_.erase(iter);
                removed_cnt++;
            }
            else
            {
                ++iter;
            }
        }
    }
    else if (count < 0)
    {
        auto iter = list_object_.end();
        while (iter != list_object_.begin() && removed_cnt < to_be_removed_cnt)
        {
            --iter;
            if (*iter == element)
            {
                serialized_length_ -= sizeof(uint32_t) + iter->Length();
                iter = list_object_.erase(iter);
                removed_cnt++;
            }
        }
    }
    else
    {
        auto iter = list_object_.begin();
        for (; iter != list_object_.end();)
        {
            if (*iter == element)
            {
                serialized_length_ -= sizeof(uint32_t) + iter->Length();
                iter = list_object_.erase(iter);
            }
            else
            {
                ++iter;
            }
        }
    }
    return list_object_.empty();
}

bool RedisListObject::CommitBlockPop(bool is_left, uint32_t count)
{
    assert(!list_object_.empty());
    uint32_t cnt = static_cast<uint32_t>(list_object_.size());
    if (cnt > count)
    {
        cnt = count;
    }

    for (uint32_t i = 0; i < cnt; i++)
    {
        if (is_left)
        {
            serialized_length_ -=
                sizeof(uint32_t) + list_object_.front().Length();
            list_object_.pop_front();
        }
        else
        {
            serialized_length_ -=
                sizeof(uint32_t) + list_object_.back().Length();
            list_object_.pop_back();
        }
    }

    assert(serialized_length_ <= MAX_OBJECT_SIZE);

    return !list_object_.empty();
}
}  // namespace EloqKV
