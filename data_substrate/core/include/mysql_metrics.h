#pragma once
#include <memory>
#include "metrics.h"
#include "meter.h"

namespace metrics
{
extern std::unique_ptr<Meter> mysql_meter;
inline bool enable_mysql_tx_metrics{false};
inline bool enable_mysql_dml_metrics{false};
extern Map<int, std::string_view> cmd_to_string_map;

inline const Name NAME_MYSQL_TX_DURATION{"mysql_tx_duration"};
inline const Name NAME_MYSQL_PROCESSED_TX_TOTAL{"mysql_processed_tx_total"};
inline const Name NAME_MYSQL_PROCESSED_QUERY_TOTAL{
    "mysql_processed_query_total"};
inline const Name NAME_MYSQL_CONNECTION_COUNT{"mysql_connection_count"};
inline const Name NAME_MAX_CONNECTIONS{"mysql_max_connections"};
inline const Name NAME_MYSQL_DML_TOTAL{"mysql_dml_total"};
inline const Name NAME_MYSQL_DML_DURATION{"mysql_dml_duration"};

void register_mysql_metrics(MetricsRegistry *metrics_registry,
                                CommonLabels common_labels= {});

#define TRACK_TX_METRICS(thd)                                                 \
  if (metrics::enable_mysql_tx_metrics &&                                     \
      !(thd)->transaction->tx_registered_)                                    \
  {                                                                           \
    (thd)->transaction->tx_start_= metrics::Clock::now();                     \
    (thd)->transaction->tx_registered_= true;                                 \
  }

#define COLLECT_TX_METRICS(thd)                                               \
  if (metrics::enable_mysql_tx_metrics && (thd)->transaction->tx_registered_) \
  {                                                                           \
    (thd)->transaction->tx_registered_= false;                                \
    metrics::mysql_meter->CollectDuration(metrics::NAME_MYSQL_TX_DURATION,    \
                                          (thd)->transaction->tx_start_);     \
    metrics::mysql_meter->Collect(metrics::NAME_MYSQL_PROCESSED_TX_TOTAL, 1); \
  }

#define IS_MONITORED_DML(dml)                                                 \
    ((dml) == SQLCOM_SELECT ||                                                \
     (dml) == SQLCOM_INSERT ||                                                \
     (dml) == SQLCOM_INSERT_SELECT ||                                         \
     (dml) == SQLCOM_UPDATE ||                                                \
     (dml) == SQLCOM_UPDATE_MULTI ||                                          \
     (dml) == SQLCOM_DELETE ||                                                \
     (dml) == SQLCOM_DELETE_MULTI ||                                          \
     (dml) == SQLCOM_LOAD)

/*
 Usage:
   metrics::TimePoint dml_start;
   TRACK_DML_METRICS(dml, dml_start)
 */
#define TRACK_DML_METRICS(dml, dml_start)                                     \
  {                                                                           \
    if (metrics::enable_mysql_dml_metrics && IS_MONITORED_DML(dml))           \
    {                                                                         \
      dml_start= metrics::Clock::now();                                       \
    }                                                                         \
  }

/*
 Usage:
   TRACK_DML_METRICS_DONE(dml, dml_start)
 */
#define TRACK_DML_METRICS_DONE(dml, dml_start)                                \
  {                                                                           \
    int dml_type= static_cast<int>(dml);                                      \
    if (metrics::enable_mysql_dml_metrics && IS_MONITORED_DML(dml))           \
    {                                                                         \
      std::string_view dml_sv= metrics::cmd_to_string_map.at(dml_type);       \
      metrics::mysql_meter->CollectDuration(metrics::NAME_MYSQL_DML_DURATION, \
                                            dml_start, dml_sv);               \
      metrics::mysql_meter->Collect(metrics::NAME_MYSQL_DML_TOTAL, 1,         \
                                    dml_sv);                                  \
    }                                                                         \
  }
} // namespace metrics
