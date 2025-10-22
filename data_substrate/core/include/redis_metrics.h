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
#include <memory>
#include <string>

#include "metrics.h"
#include "meter.h"

namespace metrics
{
inline const Name NAME_REDIS_CONNECTION_COUNT{"redis_connection_count"};
inline const Name NAME_MAX_CONNECTION{"redis_max_connections"};

inline const Name NAME_REDIS_COMMAND_TOTAL{"redis_command_total"};
inline const Name NAME_REDIS_COMMAND_DURATION{"redis_command_duration"};

inline const Name NAME_REDIS_COMMAND_AGGREGATED_TOTAL{
    "redis_command_aggregated_total"};
inline const Name NAME_REDIS_COMMAND_AGGREGATED_DURATION{
    "redis_command_aggregated_duration"};

inline const Name NAME_REDIS_SLOW_LOG_LEN{"redis_slow_log_len"};

inline size_t collect_redis_command_duration_round{0};
inline std::unique_ptr<Meter> redis_meter{nullptr};
  
inline void register_redis_metrics(MetricsRegistry *metrics_registry,
                                CommonLabels &common_labels,
                                size_t core_num)
{
    redis_meter = std::make_unique<Meter>(metrics_registry, common_labels);
    redis_meter->Register(metrics::NAME_REDIS_CONNECTION_COUNT,
                           metrics::Type::Gauge);
    redis_meter->Register(metrics::NAME_MAX_CONNECTION, metrics::Type::Gauge);
    std::vector<metrics::LabelGroup> labels;
    labels.emplace_back("core_id", std::vector<std::string>());
    for (size_t idx = 0; idx < core_num; ++idx)
    {
        labels[0].second.push_back(std::to_string(idx));
    }

    redis_meter->Register(metrics::NAME_REDIS_SLOW_LOG_LEN,
                           metrics::Type::Gauge,
                           std::move(labels));

}
}  // namespace metrics
