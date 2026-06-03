#pragma once

#include <string>
#include <string_view>

#include "b255.h"

namespace EloqKV
{
namespace NamespacePrefix
{
constexpr char B255_DELIMITER = ':';

// Construct prefix: encoded_ns_id + B255_DELIMITER + encoded_epoch +
// B255_DELIMITER
inline std::string MakePrefix(std::string_view encoded_ns_id, uint64_t epoch)
{
    std::string prefix;
    prefix.reserve(encoded_ns_id.size() + 1 + 8 + 1);
    prefix.append(encoded_ns_id);
    prefix.push_back(B255_DELIMITER);
    prefix.append(b255e(epoch));
    prefix.push_back(B255_DELIMITER);
    return prefix;
}

// Parse prefix to extract namespace_id, epoch, and original key
inline bool Parse(std::string_view full_key,
                  std::string_view &ns_id,
                  uint64_t &epoch,
                  std::string_view &user_key)
{
    size_t ns_start = 0;
    size_t delim1 = full_key.find(B255_DELIMITER, ns_start);
    if (delim1 == std::string_view::npos || delim1 <= ns_start)
        return false;

    size_t epoch_start = delim1 + 1;
    size_t delim2 = full_key.find(B255_DELIMITER, epoch_start);
    if (delim2 == std::string_view::npos || delim2 <= epoch_start)
        return false;

    std::string_view encoded_epoch =
        full_key.substr(epoch_start, delim2 - epoch_start);
    auto decoded_epoch = b255d(encoded_epoch);
    if (!decoded_epoch.has_value())
        return false;

    ns_id = full_key.substr(ns_start, delim1 - ns_start);
    epoch = *decoded_epoch;
    user_key = full_key.substr(delim2 + 1);
    return true;
}

// Compose next prefix for range scans (exclusive upper limit)
inline std::string MakePrefixNext(std::string_view prefix)
{
    if (prefix.empty())
    {
        return "";
    }
    std::string next_prefix(prefix);
    for (int i = static_cast<int>(next_prefix.size()) - 1; i >= 0; --i)
    {
        auto c = static_cast<unsigned char>(next_prefix[i]);
        if (c != 0xFF)
        {
            next_prefix[i] = static_cast<char>(c + 1);
            next_prefix.resize(i + 1);
            return next_prefix;
        }
    }
    return "";
}
}  // namespace NamespacePrefix
}  // namespace EloqKV
