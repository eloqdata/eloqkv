#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "base64url.h"

namespace EloqKV
{

struct NamespaceToken
{
    uint8_t bytes[16]{0};
    bool valid{false};

    NamespaceToken() = default;

    explicit NamespaceToken(std::string_view raw_bytes)
    {
        if (raw_bytes.size() == 16)
        {
            std::memcpy(bytes, raw_bytes.data(), 16);
            valid = true;
        }
    }

    static NamespaceToken FromBase64Url(std::string_view b64_str)
    {
        NamespaceToken token;
        if (b64_str.size() == 22)
        {
            token.valid = Base64UrlDecode(b64_str, token.bytes, 16);
        }
        return token;
    }

    std::string ToString() const
    {
        if (!valid)
            return "";
        return Base64UrlEncode(bytes, 16);
    }

    std::string_view RawView() const
    {
        return std::string_view(reinterpret_cast<const char *>(bytes), 16);
    }

    bool operator==(const NamespaceToken &other) const
    {
        return std::memcmp(bytes, other.bytes, 16) == 0;
    }

    bool operator!=(const NamespaceToken &other) const
    {
        return !(*this == other);
    }

    bool operator<(const NamespaceToken &other) const
    {
        return std::memcmp(bytes, other.bytes, 16) < 0;
    }
};

NamespaceToken GenerateRandomToken();

}  // namespace EloqKV
