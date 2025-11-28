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
#include "eloq_string.h"

#include <algorithm>
#include <cstring>

namespace EloqKV
{
EloqString::EloqString()
{
    // Init to view with nullptr means it is nullptr string, distinguish with
    // string with length=0 "".
    InitView(nullptr, 0);
}

EloqString::EloqString(const char *ptr) : EloqString(ptr, strlen(ptr))
{
}

EloqString::EloqString(const char *ptr, uint32_t length)
{
    if (length <= 23)
    {
        InitStack(length);
        std::copy(ptr, ptr + length, stack_str_.stack_array_.begin());
    }
    else
    {
        InitHeap(length);
        std::copy(ptr, ptr + length, heap_str_.heap_array_.get());
    }
}

EloqString::EloqString(std::string_view str_view)
{
    InitView(str_view.data(), str_view.size());
}

EloqString::EloqString(EloqString &&rhs)
{
    std::string_view rhs_view = rhs.StringView();
    if (rhs.Type() == StorageType::View)
    {
        // For view type, we just copy the view so the semantic won't change.
        // If you want a complete value, use Clone() instead.
        InitView(rhs_view.data(), rhs_view.size());
    }
    else if (rhs_view.size() <= 23)
    {
        InitStack(rhs_view.size());

        std::copy(rhs_view.data(),
                  rhs_view.data() + rhs_view.size(),
                  stack_str_.stack_array_.begin());
    }
    else
    {
        assert(rhs.Type() == StorageType::Heap);
        InitHeap(rhs.heap_str_.capacity_,
                 rhs.heap_str_.length_,
                 std::move(rhs.heap_str_.heap_array_));
    }
    rhs.Clear();
}

EloqString::EloqString(const EloqString &rhs)
{
    std::string_view rhs_view = rhs.StringView();
    if (rhs.Type() == StorageType::View)
    {
        // For view type, we just copy the view so the semantic won't change.
        // A copy of a view is still a view. If you want a complete value, use
        // Clone() instead.
        InitView(rhs_view.data(), rhs_view.size());
    }
    else if (rhs_view.size() <= 23)
    {
        InitStack(rhs_view.size());

        std::copy(rhs_view.data(),
                  rhs_view.data() + rhs_view.size(),
                  stack_str_.stack_array_.begin());
    }
    else
    {
        InitHeap(rhs_view.size());

        std::copy(rhs_view.data(),
                  rhs_view.data() + rhs_view.size(),
                  heap_str_.heap_array_.get());
    }
}

EloqString::~EloqString()
{
    if (Type() == StorageType::Heap)
    {
        heap_str_.heap_array_ = nullptr;
    }
}

EloqString EloqString::Clone() const
{
    std::string_view sv = StringView();
    return EloqString(sv.data(), sv.size());
}

void EloqString::Clear()
{
    InitStack(0);
}

std::string_view EloqString::StringView() const
{
    const char *ptr = nullptr;
    size_t length = 0;

    switch (Mask())
    {
    case 0:
        ptr = stack_str_.stack_array_.data();
        length = stack_str_.length_ >> 2;
        break;
    case 1:
        ptr = heap_str_.heap_array_.get();
        length = heap_str_.length_;
        break;
    default:
        ptr = view_str_.ptr_;
        length = view_str_.length_;
        break;
    }

    return {ptr, length};
}

std::string EloqString::String() const
{
    const char *ptr = nullptr;
    size_t length = 0;

    switch (Mask())
    {
    case 0:
        ptr = stack_str_.stack_array_.data();
        length = stack_str_.length_ >> 2;
        break;
    case 1:
        ptr = heap_str_.heap_array_.get();
        length = heap_str_.length_;
    default:
        ptr = view_str_.ptr_;
        length = view_str_.length_;
        break;
    }

    return {ptr, length};
}

bool EloqString::operator==(std::string_view str_view) const
{
    return StringView() == str_view;
}

bool EloqString::operator==(const std::string &str) const
{
    return StringView() == std::string_view(str);
}

bool EloqString::operator==(const EloqString &rhs) const
{
    return StringView() == rhs.StringView();
}

EloqString &EloqString::operator=(EloqString &&rhs) noexcept
{
    if (this == &rhs)
    {
        return *this;
    }

    std::string_view rhs_view = rhs.StringView();
    if (rhs.Type() == StorageType::View)
    {
        // For view type, we just copy the view so the semantic won't change.
        // A copy of a view is still a view. If you want a complete value, use
        // Clone() instead.
        InitView(rhs_view.data(), rhs_view.size());
    }
    else if (rhs_view.size() <= 23)
    {
        InitStack(rhs_view.size());

        std::copy(rhs_view.data(),
                  rhs_view.data() + rhs_view.size(),
                  stack_str_.stack_array_.begin());
    }
    else
    {
        assert(rhs.Type() == StorageType::Heap);

        InitHeap(rhs.heap_str_.capacity_,
                 rhs.heap_str_.length_,
                 std::move(rhs.heap_str_.heap_array_));
    }

    rhs.Clear();
    return *this;
}

EloqString &EloqString::operator=(const EloqString &rhs)
{
    if (this == &rhs)
    {
        return *this;
    }

    std::string_view rhs_view = rhs.StringView();
    if (rhs.Type() == StorageType::View)
    {
        // For view type, we just copy the view so the semantic won't change.
        // A copy of a view is still a view. If you want a complete value, use
        // Clone() instead.
        InitView(rhs_view.data(), rhs_view.size());
    }
    else if (rhs_view.size() <= 23)
    {
        InitStack(rhs_view.size());

        std::copy(rhs_view.data(),
                  rhs_view.data() + rhs_view.size(),
                  stack_str_.stack_array_.begin());
    }
    else
    {
        if (Type() == StorageType::Heap)
        {
            size_t curr_capacity = heap_str_.capacity_ >> 2;
            if (curr_capacity < rhs_view.size())
            {
                heap_str_.heap_array_ = nullptr;
                InitHeap(rhs_view.size());
            }
            else
            {
                heap_str_.length_ = rhs_view.size();
            }
        }
        else
        {
            InitHeap(rhs_view.size());
        }

        std::copy(rhs_view.data(),
                  rhs_view.data() + rhs_view.size(),
                  heap_str_.heap_array_.get());
    }

    return *this;
}

}  // namespace EloqKV
