#pragma once

#include <string>
#include <string_view>

namespace EloqKV
{
enum class RestorePayloadFormat
{
    EloqKV,
    RedisRdb
};

bool ConvertRedisDumpPayloadToEloqPayload(std::string_view dump_payload,
                                          std::string &eloq_payload);
}  // namespace EloqKV
