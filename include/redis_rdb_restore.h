#pragma once

#include <string>
#include <string_view>

namespace EloqKV
{
struct RedisEloqObject;

enum class RestorePayloadFormat
{
    EloqKV,
    RedisRdb
};

bool ConvertRedisDumpPayloadToEloqPayload(std::string_view dump_payload,
                                          std::string &eloq_payload);

bool ConvertEloqObjectToRedisDumpPayload(const RedisEloqObject &object,
                                         std::string &dump_payload);
}  // namespace EloqKV
