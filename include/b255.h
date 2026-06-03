#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace EloqKV
{

constexpr char B255_DELIMITER = ':';

/**
 * Encodes a numeric namespace ID into a base-255 string excluding the delimiter
 * (:). This guarantees the encoded string does not contain the delimiter (:),
 * making : safe to use as a delimiter.
 */
std::string b255e(uint64_t id);
std::string b255prefix(uint64_t id);
std::optional<uint64_t> b255d(std::string_view s);

}  // namespace EloqKV
