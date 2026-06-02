#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace EloqKV
{

inline bool Base64UrlDecode(std::string_view src,
                            uint8_t *dst_bytes,
                            size_t dst_len)
{
    if (src.size() % 4 == 1)
        return false;

    auto char_to_val = [](char c) -> int
    {
        if (c >= 'A' && c <= 'Z')
            return c - 'A';
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 26;
        if (c >= '0' && c <= '9')
            return c - '0' + 52;
        if (c == '-')
            return 62;
        if (c == '_')
            return 63;
        return -1;
    };

    uint32_t val = 0;
    int bits = 0;
    size_t dst_idx = 0;
    for (char c : src)
    {
        int code = char_to_val(c);
        if (code < 0)
            return false;
        val = (val << 6) | code;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            if (dst_idx >= dst_len)
                return false;
            dst_bytes[dst_idx++] = static_cast<uint8_t>((val >> bits) & 0xFF);
        }
    }

    if (bits > 0)
    {
        if ((val & ((1U << bits) - 1)) != 0)
            return false;
    }

    return dst_idx == dst_len;
}

inline std::string Base64UrlEncode(const uint8_t *bytes, size_t length)
{
    static const char lookup[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    std::string result;
    result.reserve((length + 2) / 3 * 4);

    size_t i = 0;
    for (; i + 2 < length; i += 3)
    {
        uint32_t val = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
        result.push_back(lookup[(val >> 18) & 0x3F]);
        result.push_back(lookup[(val >> 12) & 0x3F]);
        result.push_back(lookup[(val >> 6) & 0x3F]);
        result.push_back(lookup[val & 0x3F]);
    }

    if (i < length)
    {
        uint32_t val = bytes[i] << 16;
        if (i + 1 < length)
        {
            val |= bytes[i + 1] << 8;
        }

        result.push_back(lookup[(val >> 18) & 0x3F]);
        result.push_back(lookup[(val >> 12) & 0x3F]);

        if (i + 1 < length)
        {
            result.push_back(lookup[(val >> 6) & 0x3F]);
        }
    }

    return result;
}

}  // namespace EloqKV
