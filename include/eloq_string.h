#pragma once

#include <mimalloc.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <memory>
#include <string>

/**
 * EloqString is a versatile string type that have both value semantics and
 * reference semantics, that is, it could be either a complete string or a view
 * type string. You can use EloqString to store a complete
 * string value(like std::string), and you can also use it to reference a piece
 * of string stored elsewhere(like std::string_view). Think it like a
 * std::variant<std::string, std::string_view>.
 *
 * This flexibility leads to complexity and danger. Extra cautions are required
 * to use EloqString correctly. For view type EloqString, the space it
 * references might become invalid. For performance issue, we don't want
 * unnecessary allocation, so the copy/move constructor/assignment copy an
 * exactly the same view for view type EloqString. And the semantic won't
 * change, i.e. a copy/assignment of a view type EloqString is still a view type
 * EloqString. For value type EloqString, the copy/assignment is totally safe
 * just like std::string.
 * When you use a EloqString, it should be clear in your mind whether it's a
 * string_view or a std::string. When you pass a EloqString around, first ask
 * yourself whether you need an extra Clone as it could be a view type. If so,
 * use Clone() instead of copy/move constructor/assignment.
 */
namespace EloqKV
{
class EloqString
{
public:
    EloqString();
    explicit EloqString(const char *ptr);
    explicit EloqString(const char *ptr, uint32_t length);
    explicit EloqString(std::string_view str_view);
    explicit EloqString(std::string str) = delete;
    EloqString(EloqString &&rhs);
    EloqString(const EloqString &rhs);
    ~EloqString();

    /**
     * Clone a complete EloqString that is value semantics, that is, the
     * returned EloqString owns the complete string value and has a Stack or
     * Heap StorageType.
     *
     * @return
     */
    EloqString Clone() const;

    void Clear();

    std::string_view StringView() const;
    std::string String() const;

    bool operator==(std::string_view str_view) const;
    bool operator==(const std::string &str) const;
    bool operator==(const EloqString &rhs) const;

    EloqString &operator=(EloqString &&rhs) noexcept;
    EloqString &operator=(const EloqString &rhs);
    size_t Hash() const
    {
        return std::hash<std::string_view>{}(StringView());
    }

    bool operator<(const EloqString &rhs) const
    {
        return StringView() < rhs.StringView();
    }

    enum struct StorageType
    {
        Stack,
        Heap,
        View
    };

    StorageType Type() const
    {
        switch (Mask())
        {
        case 0:
            return StorageType::Stack;
        case 1:
            return StorageType::Heap;
        case 2:
            return StorageType::View;
        default:
            return StorageType::Stack;
        }
    }
    uint32_t Length() const
    {
        switch (Mask())
        {
        case 0:
            return stack_str_.length_ >> 2;
        case 1:
            return heap_str_.length_;
        case 2:
            return view_str_.length_;
        default:
            return 0;
        }
    }
    const char *Data() const
    {
        switch (Mask())
        {
        case 0:
            return stack_str_.stack_array_.data();
        case 1:
            return heap_str_.heap_array_.get();
        default:
            return view_str_.ptr_;
        }
    }

    bool NeedsDefrag(mi_heap_t *heap)
    {
        bool defraged = false;
        if (Type() == StorageType::Heap)
        {
            float heap_array_char_utilization = mi_heap_page_utilization(
                heap, static_cast<void *>(heap_str_.heap_array_.get()));
            if (heap_array_char_utilization < 0.8)
            {
                defraged = true;
            }
        }
        return defraged;
    }

private:
    uint8_t Mask() const
    {
        return view_str_.flag_ & 0x03;
    }

    void InitStack(uint8_t length)
    {
        // If the current string has a heap allocation, de-allocates it
        // first.
        if (Type() == StorageType::Heap)
        {
            heap_str_.heap_array_ = nullptr;
        }
        stack_str_.length_ = length << 2;
    }

    void InitHeap(size_t length)
    {
        // If the current string has a heap allocation, de-allocates it
        // first.
        if (Type() == StorageType::Heap)
        {
            heap_str_.heap_array_ = nullptr;
        }
        // Rounds to 16, so that allocated memory is 16-byte aligned.
        heap_str_.capacity_ = ((length - 1) | 15) + 1;
        heap_str_.length_ = length;
        heap_str_.heap_array_.release();
        heap_str_.heap_array_ = std::make_unique<char[]>(heap_str_.capacity_);
        heap_str_.capacity_ = 1 | (heap_str_.capacity_ << 2);
    }

    void InitHeap(size_t capacity,
                  size_t length,
                  std::unique_ptr<char[]> heap_obj)
    {
        // If the current string has a heap allocation, de-allocates it
        // first.
        if (Type() == StorageType::Heap)
        {
            heap_str_.heap_array_ = nullptr;
        }
        heap_str_.capacity_ = capacity;
        heap_str_.length_ = length;
        heap_str_.heap_array_.release();
        heap_str_.heap_array_ = std::move(heap_obj);
    }

    void InitView(const char *ptr, size_t length)
    {
        // If the current string has a heap allocation, de-allocates it
        // first.
        if (Type() == StorageType::Heap)
        {
            heap_str_.heap_array_ = nullptr;
        }
        view_str_.flag_ = 2;
        view_str_.length_ = length;
        view_str_.ptr_ = ptr;
    }

    struct HeapString
    {
        size_t capacity_;
        size_t length_;
        std::unique_ptr<char[]> heap_array_;
    };

    struct StackString
    {
        uint8_t length_;
        std::array<char, 23> stack_array_;
    };

    struct ViewString
    {
        uint8_t flag_;
        size_t length_;
        const char *ptr_;
    };

    union
    {
        ViewString view_str_{};
        StackString stack_str_;
        HeapString heap_str_;
    };
};

}  // namespace EloqKV

template <>
struct std::hash<EloqKV::EloqString>
{
    std::size_t operator()(const EloqKV::EloqString &str) const noexcept
    {
        return str.Hash();
    }
};

template <>
struct std::equal_to<EloqKV::EloqString>
{
    bool operator()(const EloqKV::EloqString &lhs,
                    const EloqKV::EloqString &rhs) const
    {
        return lhs == rhs;
    }
};

template <>
struct std::less<EloqKV::EloqString>
{
    bool operator()(const EloqKV::EloqString &lhs,
                    const EloqKV::EloqString &rhs) const
    {
        return lhs < rhs;
    }
};
