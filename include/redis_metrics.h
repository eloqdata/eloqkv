#pragma once

#include <cstddef>
#include <string>

#include "metrics.h"

namespace metrics
{
inline const Name NAME_CONNECTION_COUNT{"redis_connection_count"};
inline const Name NAME_MAX_CONNECTION{"redis_max_connections"};

inline const Name NAME_REDIS_COMMAND_TOTAL{"redis_command_total"};
inline const Name NAME_REDIS_COMMAND_DURATION{"redis_command_duration"};

inline const Name NAME_REDIS_COMMAND_AGGREGATED_TOTAL{
    "redis_command_aggregated_total"};
inline const Name NAME_REDIS_COMMAND_AGGREGATED_DURATION{
    "redis_command_aggregated_duration"};

inline size_t collect_redis_command_duration_round{0};
}  // namespace metrics
