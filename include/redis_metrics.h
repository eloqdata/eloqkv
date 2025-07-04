/**
 *    Copyright (C) 2025 EloqData Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under either of the following two licenses:
 *    1. GNU Affero General Public License, version 3, as published by the Free
 *    Software Foundation.
 *    2. GNU General Public License as published by the Free Software
 *    Foundation; version 2 of the License.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License or GNU General Public License for more
 *    details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    and GNU General Public License V2 along with this program.  If not, see
 *    <http://www.gnu.org/licenses/>.
 *
 */
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

inline const Name NAME_REDIS_SLOW_LOG_LEN{"redis_slow_log_len"};

inline size_t collect_redis_command_duration_round{0};
}  // namespace metrics
