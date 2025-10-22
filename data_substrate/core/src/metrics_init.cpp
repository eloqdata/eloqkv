#include "INIReader.h"
#include "data_substrate.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "metrics.h"
#include "metrics_registry_impl.h"
#include "store/data_store_handler.h"
#include "tx_service_metrics.h"

#if (WITH_LOG_SERVICE)
#include "log_service_metrics.h"
#endif
#ifdef ELOQ_MODULE_ELOQSQL
#include "mysql_metrics.h"  //TDOO(liunyl): move this file to correct location
#endif
#ifdef ELOQ_MODULE_ELOQKV
#include "redis_metrics.h"
#endif

DEFINE_bool(enable_metrics, false, "Enable metrics");
DEFINE_string(metrics_port, "18081", "Metrics port");
namespace EloqKV
{
enum struct RedisCommandType;
extern const std::vector<std::pair<const char *, RedisCommandType>>
    command_types;
}  // namespace EloqKV

bool DataSubstrate::InitializeMetrics(const INIReader &config_reader)
{
    /* Parse metrics config */
    metrics::enable_metrics =
        config_reader.GetBoolean("metrics", "enable_metrics", false);
    DLOG(INFO) << "enable_metrics: "
               << (metrics::enable_metrics ? "ON" : "OFF");
    if (metrics::enable_metrics)
    {
        FLAGS_metrics_port = std::to_string(config_reader.GetInteger(
            "metrics", "metrics_port", std::stoi(FLAGS_metrics_port)));

        LOG(INFO) << "metrics_port: " << FLAGS_metrics_port;

        // global metrics
        metrics::enable_memory_usage =
            config_reader.GetBoolean("metrics", "enable_memory_usage", true);
        LOG(INFO) << "enable_memory_usage: "
                  << (metrics::enable_memory_usage ? "ON" : "OFF");
        if (metrics::enable_memory_usage)
        {
            metrics::collect_memory_usage_round = config_reader.GetInteger(
                "metrics", "collect_memory_usage_round", 10000);
            LOG(INFO) << "collect memory usage every "
                      << metrics::collect_memory_usage_round << " round(s)";
        }

        metrics::enable_cache_hit_rate =
            config_reader.GetBoolean("metrics", "enable_cache_hit_rate", true);
        LOG(INFO) << "enable_cache_hit_rate: "
                  << (metrics::enable_cache_hit_rate ? "ON" : "OFF");

        // tx metrics
        metrics::enable_tx_metrics =
            config_reader.GetBoolean("metrics", "enable_tx_metrics", true);
        LOG(INFO) << "enable_tx_metrics: "
                  << (metrics::enable_tx_metrics ? "ON" : "OFF");
        if (metrics::enable_tx_metrics)
        {
            metrics::collect_tx_duration_round =
                config_reader.GetInteger("metrics",
                                         "collect_tx_duration_round",
                                         metrics::collect_tx_duration_round);
            LOG(INFO) << "collect tx duration every "
                      << metrics::collect_tx_duration_round << " round(s)";
        }

        // busy round metrics
        metrics::enable_busy_round_metrics = config_reader.GetBoolean(
            "metrics", "enable_busy_round_metrics", true);
        LOG(INFO) << "enable_busy_round_metrics: "
                  << (metrics::enable_busy_round_metrics ? "ON" : "OFF");
        if (metrics::enable_busy_round_metrics)
        {
            metrics::busy_round_threshold =
                config_reader.GetInteger("metrics", "busy_round_threshold", 10);
            LOG(INFO) << "collect busy round metrics when reaching the "
                         "busy round "
                         "threshold "
                      << metrics::busy_round_threshold;
        }

        // remote request metrics
        metrics::enable_remote_request_metrics = config_reader.GetBoolean(
            "metrics", "enable_busy_round_metrics", metrics::enable_tx_metrics);
        LOG(INFO) << "enable_remote_request_metrics: "
                  << (metrics::enable_remote_request_metrics ? "ON" : "OFF");
        if (core_config_.enable_data_store)
        {
            metrics::enable_kv_metrics =
                config_reader.GetBoolean("metrics", "enable_kv_metrics", true);
            LOG(INFO) << "enable_kv_metrics: "
                      << (metrics::enable_kv_metrics ? "ON" : "OFF");
        }

#if (WITH_LOG_SERVICE)
        if (core_config_.enable_wal)
        {
            // log_service metrics
            metrics::enable_log_service_metrics = config_reader.GetBoolean(
                "metrics", "enable_log_service_metrics", true);
            LOG(INFO) << "enable_log_service_metrics: "
                      << (metrics::enable_log_service_metrics ? "ON" : "OFF");
        }
#endif

        // failed forward msgs metrics
        metrics::enable_standby_metrics =
            config_reader.GetBoolean("metrics", "enable_standby_metrics", true);
        LOG(INFO) << "enable standby metrics: "
                  << (metrics::enable_standby_metrics ? "ON" : "OFF");
        if (metrics::enable_standby_metrics)
        {
            metrics::collect_standby_metrics_round = config_reader.GetInteger(
                "metrics", "collect_standby_metrics_round", 10000);
            LOG(INFO) << "collect standby metrics every "
                      << metrics::collect_standby_metrics_round << " round(s)";
        }
#ifdef ELOQ_MODULE_ELOQSQL
        metrics::enable_mysql_tx_metrics = config_reader.GetBoolean(
            "metrics", "enable_mysql_tx_metrics", true);
        metrics::enable_mysql_dml_metrics = config_reader.GetBoolean(
            "metrics", "enable_mysql_dml_metrics", true);
#endif
        setenv("ELOQ_METRICS_PORT", FLAGS_metrics_port.c_str(), false);
        eloq_metrics_app::MetricsRegistryImpl::MetricsRegistryResult
            metrics_registry_result =
                eloq_metrics_app::MetricsRegistryImpl::GetRegistry();

        if (metrics_registry_result.not_ok_ != nullptr)
        {
            LOG(ERROR)
                << "!!!!!!!! Failed to initialize MetricsRegristry !!!!!!!!";
            return false;
        }

        metrics_registry_ =
            std::move(metrics_registry_result.metrics_registry_);

        if (core_config_.enable_data_store)
        {
            metrics::CommonLabels kv_common_common_labels{};
            kv_common_common_labels["node_ip"] = network_config_.local_ip;
            kv_common_common_labels["node_port"] =
                std::to_string(network_config_.local_port);
            store_hd_->RegisterKvMetrics(metrics_registry_.get(),
                                         kv_common_common_labels);
        }

#ifdef ELOQ_MODULE_ELOQSQL
        metrics::CommonLabels mysql_common_labels{};
        mysql_common_labels["node_ip"] = network_config_.local_ip;
        mysql_common_labels["node_port"] =
            std::to_string(network_config_.local_port);
        metrics::register_mysql_metrics(metrics_registry_.get(),
                                        mysql_common_labels);
#endif

#ifdef ELOQ_MODULE_ELOQKV
        metrics::CommonLabels redis_common_labels{};
        redis_common_labels["node_ip"] = network_config_.local_ip;
        redis_common_labels["node_port"] =
            std::to_string(network_config_.local_port);
        redis_common_labels["node_id"] =
            std::to_string(network_config_.node_id);
        metrics::register_redis_metrics(metrics_registry_.get(),
                                        redis_common_labels,
                                        core_config_.core_num);
        metrics::redis_meter->Collect(
            metrics::NAME_MAX_CONNECTION,
            DataSubstrate::GetGlobal()->GetCoreConfig().maxclients);
        for (const auto &[cmd, _] : EloqKV::command_types)
        {
            std::vector<metrics::LabelGroup> label_groups = {{"type", {cmd}}};

            external_metrics.push_back(
                std::make_tuple(metrics::NAME_REDIS_COMMAND_DURATION,
                                metrics::Type::Histogram,
                                label_groups));
            external_metrics.push_back(
                std::make_tuple(metrics::NAME_REDIS_COMMAND_TOTAL,
                                metrics::Type::Counter,
                                label_groups));
        }
        for (const auto &access_type : {"read", "write"})
        {
            external_metrics.push_back(
                std::make_tuple(metrics::NAME_REDIS_COMMAND_AGGREGATED_TOTAL,
                                metrics::Type::Counter,
                                std::vector<metrics::LabelGroup>{
                                    {"access_type", {access_type}}}));
            external_metrics.push_back(
                std::make_tuple(metrics::NAME_REDIS_COMMAND_AGGREGATED_DURATION,
                                metrics::Type::Histogram,
                                std::vector<metrics::LabelGroup>{
                                    {"access_type", {access_type}}}));
        }
#endif

        tx_service_common_labels_["node_ip"] = network_config_.local_ip;
        tx_service_common_labels_["node_port"] =
            std::to_string(network_config_.local_port);
        tx_service_common_labels_["node_id"] =
            std::to_string(network_config_.node_id);
    }

    return true;
}