#pragma once

#include <butil/strings/string_piece.h>
#include <strings.h>

#include <cstring>
#include <string>
#include <string_view>
#include <utility>

static inline bool IsEq(butil::StringPiece s1, std::string_view s2)
{
    const size_t len = s2.size();
    return s1.size() == len && strncasecmp(s1.data(), s2.data(), len) == 0;
}

static inline bool IsEq(std::string_view s1, std::string_view s2)
{
    const size_t len = s2.size();
    return s1.size() == len && strncasecmp(s1.data(), s2.data(), len) == 0;
}

static inline bool IsEq(const std::string &s1, std::string_view s2)
{
    const size_t len = s2.size();
    return s1.size() == len && strncasecmp(s1.data(), s2.data(), len) == 0;
}

static inline bool IsEq(const char *s1, std::string_view s2)
{
    return IsEq(std::string_view(s1), s2);
}

template <typename... Args>
static inline bool IsEqAny(butil::StringPiece s1, Args &&...args)
{
    return (IsEq(s1, std::forward<Args>(args)) || ...);
}

template <typename... Args>
static inline bool IsEqAny(std::string_view s1, Args &&...args)
{
    return (IsEq(s1, std::forward<Args>(args)) || ...);
}

template <typename... Args>
static inline bool IsEqAny(const std::string &s1, Args &&...args)
{
    return (IsEq(s1, std::forward<Args>(args)) || ...);
}

template <typename... Args>
static inline bool IsEqAny(const char *s1, Args &&...args)
{
    return (IsEq(s1, std::forward<Args>(args)) || ...);
}
