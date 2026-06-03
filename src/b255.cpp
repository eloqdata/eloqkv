#include "b255.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace EloqKV
{

std::string b255e(uint64_t id)
{
    if (id == 0)
    {
        return std::string(1, '\x00');
    }
    const unsigned char delim = static_cast<unsigned char>(B255_DELIMITER);
    char buf[9];
    int pos = 9;
    uint64_t temp = id;
    while (temp > 0)
    {
        uint64_t digit = temp % 255;
        buf[--pos] = static_cast<char>(digit < delim ? digit : digit + 1);
        temp /= 255;
    }
    return std::string(buf + pos, 9 - pos);
}

std::string b255prefix(uint64_t id)
{
    return b255e(id) + B255_DELIMITER;
}

std::optional<uint64_t> b255d(std::string_view s)
{
    if (s.empty() || s.size() > 9)
    {
        return std::nullopt;
    }
    const unsigned char delim = static_cast<unsigned char>(B255_DELIMITER);
    uint64_t id = 0;
    for (char c : s)
    {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc == delim)
        {
            return std::nullopt;
        }
        uint64_t digit = uc < delim ? uc : uc - 1;
        if (id > (std::numeric_limits<uint64_t>::max() - digit) / 255)
        {
            return std::nullopt;
        }
        id = id * 255 + digit;
    }
    return id;
}

}  // namespace EloqKV
