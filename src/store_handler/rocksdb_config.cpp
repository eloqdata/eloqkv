#include "rocksdb_config.h"

#include <ctime>
#include <iomanip>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>

#include "glog/logging.h"
#include "store_handler/kv_store.h"

#if KV_DATA_STORE_TYPE == KV_DATA_STORE_TYPE_ROCKSDB

#if ROCKSDB_CLOUD_FS()
#include "rocksdb/cloud/db_cloud.h"
#endif

DEFINE_string(rocksdb_info_log_level, "INFO", "RocksDB store info log level");
DEFINE_bool(rocksdb_enable_stats, false, "RocksDB store enable stats");
DEFINE_uint32(rocksdb_stats_dump_period_sec,
              600,
              "RocksDB stats dump period sec");
DEFINE_string(rocksdb_storage_path, "", "RocksDB store storage path");
DEFINE_uint32(rocksdb_max_write_buffer_number,
              8,
              "RocksDB store max write buffer number");
DEFINE_uint32(rocksdb_max_background_jobs,
              4,
              "RocksDB store max background jobs");
DEFINE_uint32(rocksdb_max_background_flush,
              0, /*Given 0, RocksDB will automatically adjust*/
              "RocksDB store max background flush");
DEFINE_uint32(rocksdb_max_background_compaction,
              0, /*Given 0, RocksDB will automatically adjust*/
              "RocksDB store max background compaction");
DEFINE_string(rocksdb_target_file_size_base,
              "64MB", /*Given 0, RocksDB will use default value 64MB*/
              "RocksDB store target file size base");
DEFINE_uint32(rocksdb_target_file_size_multiplier,
              1, /*Given 0, RocksDB will use default value 1*/
              "RocksDB store target file size multiplier");
DEFINE_string(rocksdb_write_buffer_size,
              "64MB", /*Given 0, RocksDB will use default value 64MB*/
              "RocksDB store write buffer size");
DEFINE_bool(rocksdb_use_direct_io_for_flush_and_compaction,
            false,
            "RocksDB store use direct io for flush and compaction");
DEFINE_bool(rocksdb_use_direct_io_for_read,
            false,
            "RocksDB store use direct io for read");
DEFINE_uint32(rocksdb_level0_stop_writes_trigger,
              36, /*Given 0, RocksDB will use its default value 36*/
              "RocksDB store level0 stop writes trigger");
DEFINE_uint32(rocksdb_level0_slowdown_writes_trigger,
              20, /*Given 0, RocksDB will use its default value 20*/
              "RocksDB store level0 slowdown writes trigger");
DEFINE_uint32(rocksdb_level0_file_num_compaction_trigger,
              4, /*Given 0, RocksDB will use its default value 4*/
              "RocksDB store level0 file num compaction trigger");
DEFINE_string(rocksdb_max_bytes_for_level_base,
              "256MB", /*Given 0, RocksDB will use default value 256MB*/
              "RocksDB store max bytes for level base");
DEFINE_uint32(rocksdb_max_bytes_for_level_multiplier,
              10, /*Given 0, RocksDB will use default value 10*/
              "RocksDB store max bytes for level multiplier");
DEFINE_string(rocksdb_compaction_style,
              "level",
              "RocksDB store compaction style");
DEFINE_string(rocksdb_soft_pending_compaction_bytes_limit,
              "64GB", /*Given 0, RocksDB will use default value 64GB*/
              "RocksDB store soft pending compaction bytes limit");
DEFINE_string(rocksdb_hard_pending_compaction_bytes_limit,
              "256GB", /*Given 0, RocksDB will use default value 256GB*/
              "RocksDB store hard pending compaction bytes limit");
DEFINE_uint32(rocksdb_max_subcompactions,
              1, /*Given 0, RocksDB will use default value 1*/
              "RocksDB store max subcompactions");
DEFINE_string(rocksdb_write_rate_limit,
              "0MB", /*Given 0, RocksDB will not have write limit*/
              "RocksDB store write_rate limit (bytes per second)");
DEFINE_uint32(rocksdb_query_worker_num, 16, "RocksDB async query worker num");
DEFINE_string(
    rocksdb_batch_write_size,
    "1MB", /*Adjust for balancing the memory footprint and throughput*/
    "RocksDB batch write size when doing checkpoint");
DECLARE_uint32(snapshot_sync_worker_num);

DEFINE_uint32(
    rocksdb_periodic_compaction_seconds,
    24 * 60 * 60, /*sst files older than 1 day will be pick up for compaction*/
    "RocksDB periodic compaction seconds");

static std::tm parseTime(const std::string &timeStr)
{
    std::tm tm = {};

    std::istringstream ss(timeStr);
    ss >> std::get_time(&tm, "%H:%M");
    return tm;
}

inline bool is_number(const std::string &str)
{
    // regular expression for matching number format
    std::regex pattern("^[-+]?[0-9]*\\.?[0-9]+([eE][-+]?[0-9]+)?$");
    return std::regex_match(str, pattern);
}

inline std::string_view remove_last_two(const std::string_view &str)
{
    if (str.length() <= 2)
    {
        return "";
    }
    return std::string_view(str.data(), str.size() - 2);
}

inline std::string_view get_last_two(const std::string_view &str)
{
    if (str.length() <= 2)
    {
        return "";
    }
    return std::string_view(str.data() + str.size() - 2, 2);
}

inline uint64_t unit_num(const std::string_view &unit_str)
{
    if (unit_str == "MB" || unit_str == "mb")
    {
        return 1024 * 1024L;
    }
    else if (unit_str == "GB" || unit_str == "gb")
    {
        return 1024 * 1024 * 1024L;
    }
    else if (unit_str == "TB" || unit_str == "tb")
    {
        return 1024 * 1024 * 1024 * 1024L;
    }

    return 1L;
}

inline bool ends_with(const std::string_view &str,
                      const std::string_view &suffix)
{
    if (str.compare(str.size() - suffix.size(), suffix.size(), suffix) != 0)
    {
        return false;
    }

    return true;
}

inline bool is_valid_size(const std::string_view &size_str_v)
{
    bool is_right_end =
        ends_with(size_str_v, "MB") || ends_with(size_str_v, "mb") ||
        ends_with(size_str_v, "GB") || ends_with(size_str_v, "gb") ||
        ends_with(size_str_v, "TB") || ends_with(size_str_v, "tb");

    if (!is_right_end)
    {
        return false;
    }

    std::string num_str;
    num_str = remove_last_two(size_str_v);

    if (!is_number(num_str))
    {
        return false;
    }

    return true;
}

inline uint64_t parse_size(const std::string &size_str)
{
    std::string_view size_str_v(size_str);
    assert(is_valid_size(size_str_v));
    std::string_view unit_str = get_last_two(size_str_v);
    uint64_t unit = unit_num(unit_str);
    std::string_view num_str = remove_last_two(size_str_v);
    uint64_t num = std::stol(std::string(num_str));
    return num * unit;
}

static int getTimeZoneOffset()
{
    std::time_t now = std::time(nullptr);
    std::tm local_tm = *std::localtime(&now);
    std::tm utc_tm = *std::gmtime(&now);
    int offset =
        std::difftime(std::mktime(&local_tm), std::mktime(&utc_tm)) / 60;
    return offset;
}

static std::tm toUTC(const std::tm &localTime, int offsetMinutes)
{
    std::tm utcTime = localTime;
    std::time_t localEpoch = std::mktime(&utcTime);
    localEpoch -= offsetMinutes * 60;  // Convert offset to seconds and adjust
    utcTime = *std::gmtime(&localEpoch);
    return utcTime;
}

static std::string formatTime(const std::tm &time)
{
    std::ostringstream ss;
    ss << std::put_time(&time, "%H:%M");
    return ss.str();
}

static std::string GetDefaultOffPeakTimeUtc()
{
    const std::string dialy_offpeak_time_start = "00:00";
    const std::string dialy_offpeal_time_end = "05:00";
    std::tm start_tm = parseTime(dialy_offpeak_time_start);
    std::tm end_tm = parseTime(dialy_offpeal_time_end);
    int tz_offset = getTimeZoneOffset();
    std::tm start_tm_utc = toUTC(start_tm, tz_offset);
    std::tm end_tm_utc = toUTC(end_tm, tz_offset);
    return formatTime(start_tm_utc) + "-" + formatTime(end_tm_utc);
};

DEFINE_string(rocksdb_dialy_offpeak_time_utc,
              GetDefaultOffPeakTimeUtc(),
              "RocksDB dialy offpeak time in UTC in HH:mm-HH:mm format. The "
              "default value is 00:00-05:00 of local time zone");

#if ROCKSDB_CLOUD_FS()
DEFINE_string(rocksdb_cloud_bucket_name,
              "rocksdb-cloud-test",
              "RocksDB cloud bucket name");
DEFINE_string(rocksdb_cloud_bucket_prefix,
              "eloqkv-",
              "RocksDB cloud bucket prefix");
DEFINE_string(rocksdb_cloud_region, "ap-northeast-1", "RocksDB cloud region");
DEFINE_string(rocksdb_cloud_sst_file_cache_size,
              "20GB",
              "RocksDB cloud sst file cache size");
DEFINE_uint32(rocksdb_cloud_db_ready_timeout_sec,
              10,
              "RocksDB cloud db ready timeout us");
DEFINE_uint32(rocksdb_cloud_db_file_deletion_delay_sec,
              3600,
              "RocksDB cloud db file deletion delay");

DEFINE_uint32(rocksdb_cloud_warm_up_thread_num,
              1,
              "Rocksdb cloud warm up thread number");
#endif

DEFINE_bool(rocksdb_io_uring_enabled,
            false,
            "If true, enable the use of IO uring if the platform supports it");
extern "C" bool RocksDbIOUringEnable()
{
    return FLAGS_rocksdb_io_uring_enabled;
}

DEFINE_string(rocksdb_cloud_s3_endpoint_url,
              "",
              "S3 compatible object store (e.g. minio) endpoint URL only for "
              "development purpose");

DEFINE_string(rocksdb_cloud_aws_access_key_id, "", "AWS access key for s3");

DEFINE_string(rocksdb_cloud_aws_secret_access_key, "", "AWS secret key for s3");

namespace EloqShare
{
bool CheckCommandLineFlagIsDefault(const char *name)
{
    gflags::CommandLineFlagInfo flag_info;

    bool flag_found = gflags::GetCommandLineFlagInfo(name, &flag_info);
    // Make sure the flag is declared.
    assert(flag_found);
    (void) flag_found;

    // Return `true` if the flag has the default value and has not been set
    // explicitly from the cmdline or via SetCommandLineOption
    return flag_info.is_default;
}

RocksDBConfig::RocksDBConfig(const INIReader &config,
                             const std::string &eloq_data_path)
{
    info_log_level_ = !CheckCommandLineFlagIsDefault("rocksdb_info_log_level")
                          ? FLAGS_rocksdb_info_log_level
                          : config.GetString("store",
                                             "rocksdb_info_log_level",
                                             FLAGS_rocksdb_info_log_level);
    enable_stats_ =
        !CheckCommandLineFlagIsDefault("rocksdb_enable_stats")
            ? FLAGS_rocksdb_enable_stats
            : config.GetBoolean(
                  "store", "rocksdb_enable_stats", FLAGS_rocksdb_enable_stats);
    stats_dump_period_sec_ =
        !CheckCommandLineFlagIsDefault("rocksdb_stats_dump_period_sec")
            ? FLAGS_rocksdb_stats_dump_period_sec
            : config.GetInteger("store",
                                "rocksdb_stats_dump_period_sec",
                                FLAGS_rocksdb_stats_dump_period_sec);
    storage_path_ =
        !CheckCommandLineFlagIsDefault("rocksdb_storage_path")
            ? FLAGS_rocksdb_storage_path
            : config.GetString(
                  "store", "rocksdb_storage_path", FLAGS_rocksdb_storage_path);
    if (storage_path_.empty())
    {
        storage_path_.append(eloq_data_path);
        storage_path_.append("/rocksdb_data");
    }
    max_write_buffer_number_ =
        !CheckCommandLineFlagIsDefault("rocksdb_max_write_buffer_number")
            ? FLAGS_rocksdb_max_write_buffer_number
            : config.GetInteger("store",
                                "rocksdb_max_write_buffer_number",
                                FLAGS_rocksdb_max_write_buffer_number);
    max_background_jobs_ =
        !CheckCommandLineFlagIsDefault("rocksdb_max_background_jobs")
            ? FLAGS_rocksdb_max_background_jobs
            : config.GetInteger("store",
                                "rocksdb_max_background_jobs",
                                FLAGS_rocksdb_max_background_jobs);
    max_background_flush_ =
        !CheckCommandLineFlagIsDefault("rocksdb_max_background_flush")
            ? FLAGS_rocksdb_max_background_flush
            : config.GetInteger("store",
                                "rocksdb_max_background_flush",
                                FLAGS_rocksdb_max_background_flush);
    max_background_compaction_ =
        !CheckCommandLineFlagIsDefault("rocksdb_max_background_compaction")
            ? FLAGS_rocksdb_max_background_compaction
            : config.GetInteger("store",
                                "rocksdb_max_background_compaction",
                                FLAGS_rocksdb_max_background_compaction);
    std::string rocksdb_target_file_size_base =
        !CheckCommandLineFlagIsDefault("rocksdb_target_file_size_base")
            ? FLAGS_rocksdb_target_file_size_base
            : config.GetString("store",
                               "rocksdb_target_file_size_base",
                               FLAGS_rocksdb_target_file_size_base);
    target_file_size_base_bytes_ = parse_size(rocksdb_target_file_size_base);
    target_file_size_multiplier_ =
        !CheckCommandLineFlagIsDefault("rocksdb_target_file_size_multiplier")
            ? FLAGS_rocksdb_target_file_size_multiplier
            : config.GetInteger("store",
                                "rocksdb_target_file_size_multiplier",
                                FLAGS_rocksdb_target_file_size_multiplier);
    std::string rocksdb_write_buffer_size =
        !CheckCommandLineFlagIsDefault("rocksdb_write_buffer_size")
            ? FLAGS_rocksdb_write_buffer_size
            : config.GetString("store",
                               "rocksdb_write_buffer_size",
                               FLAGS_rocksdb_write_buffer_size);
    write_buffer_size_bytes_ = parse_size(rocksdb_write_buffer_size);
    use_direct_io_for_flush_and_compaction_ = !CheckCommandLineFlagIsDefault(
        "rocksdb_use_direct_io_for_flush_and_compaction");
    use_direct_io_for_read_ =
        !CheckCommandLineFlagIsDefault("rocksdb_use_direct_io_for_read");
    level0_stop_writes_trigger_ =
        !CheckCommandLineFlagIsDefault("rocksdb_level0_stop_writes_trigger")
            ? FLAGS_rocksdb_level0_stop_writes_trigger
            : config.GetInteger("store",
                                "rocksdb_level0_stop_writes_trigger",
                                FLAGS_rocksdb_level0_stop_writes_trigger);
    level0_slowdown_writes_trigger_ =
        !CheckCommandLineFlagIsDefault("rocksdb_level0_slowdown_writes_trigger")
            ? FLAGS_rocksdb_level0_slowdown_writes_trigger
            : config.GetInteger("store",
                                "rocksdb_level0_slowdown_writes_trigger",
                                FLAGS_rocksdb_level0_slowdown_writes_trigger);
    level0_file_num_compaction_trigger_ =
        !CheckCommandLineFlagIsDefault(
            "rocksdb_level0_file_num_compaction_trigger")
            ? FLAGS_rocksdb_level0_file_num_compaction_trigger
            : config.GetInteger(
                  "store",
                  "rocksdb_level0_file_num_compaction_trigger",
                  FLAGS_rocksdb_level0_file_num_compaction_trigger);
    std::string rocksdb_max_bytes_for_level_base =
        !CheckCommandLineFlagIsDefault("rocksdb_max_bytes_for_level_base")
            ? FLAGS_rocksdb_max_bytes_for_level_base
            : config.GetString("store",
                               "rocksdb_max_bytes_for_level_base",
                               FLAGS_rocksdb_max_bytes_for_level_base);
    max_bytes_for_level_base_bytes_ =
        parse_size(rocksdb_max_bytes_for_level_base);
    max_bytes_for_level_multiplier_ =
        !CheckCommandLineFlagIsDefault("rocksdb_max_bytes_for_level_multiplier")
            ? FLAGS_rocksdb_max_bytes_for_level_multiplier
            : config.GetInteger("store",
                                "rocksdb_max_bytes_for_level_multiplier",
                                FLAGS_rocksdb_max_bytes_for_level_multiplier);
    compaction_style_ =
        !CheckCommandLineFlagIsDefault("rocksdb_compaction_style")
            ? FLAGS_rocksdb_compaction_style
            : config.GetString("store",
                               "rocksdb_compaction_style",
                               FLAGS_rocksdb_compaction_style);
    std::string rocksdb_soft_pending_compaction_bytes_limit =
        !CheckCommandLineFlagIsDefault(
            "rocksdb_soft_pending_compaction_bytes_limit")
            ? FLAGS_rocksdb_soft_pending_compaction_bytes_limit
            : config.GetString(
                  "store",
                  "rocksdb_soft_pending_compaction_bytes_limit",
                  FLAGS_rocksdb_soft_pending_compaction_bytes_limit);
    soft_pending_compaction_bytes_limit_bytes_ =
        parse_size(rocksdb_soft_pending_compaction_bytes_limit);
    std::string rocksdb_hard_pending_compaction_bytes_limit =
        !CheckCommandLineFlagIsDefault(
            "rocksdb_hard_pending_compaction_bytes_limit")
            ? FLAGS_rocksdb_hard_pending_compaction_bytes_limit
            : config.GetString(
                  "store",
                  "rocksdb_hard_pending_compaction_bytes_limit",
                  FLAGS_rocksdb_hard_pending_compaction_bytes_limit);
    hard_pending_compaction_bytes_limit_bytes_ =
        parse_size(rocksdb_hard_pending_compaction_bytes_limit);
    max_subcompactions_ =
        !CheckCommandLineFlagIsDefault("rocksdb_max_subcompactions")
            ? FLAGS_rocksdb_max_subcompactions
            : config.GetInteger("store",
                                "rocksdb_max_subcompactions",
                                FLAGS_rocksdb_max_subcompactions);
    std::string rocksdb_write_rate_limit =
        !CheckCommandLineFlagIsDefault("rocksdb_write_rate_limit")
            ? FLAGS_rocksdb_write_rate_limit
            : config.GetString("store",
                               "rocksdb_write_rate_limit",
                               FLAGS_rocksdb_write_rate_limit);
    write_rate_limit_bytes_ = parse_size(rocksdb_write_rate_limit.c_str());
    query_worker_num_ =
        !CheckCommandLineFlagIsDefault("rocksdb_query_worker_num")
            ? FLAGS_rocksdb_query_worker_num
            : config.GetInteger("store",
                                "rocksdb_query_worker_num",
                                FLAGS_rocksdb_query_worker_num);

    std::string batch_write_size =
        !CheckCommandLineFlagIsDefault("rocksdb_batch_write_size")
            ? FLAGS_rocksdb_batch_write_size
            : config.GetString("store",
                               "rocksdb_batch_write_size",
                               FLAGS_rocksdb_batch_write_size);
    batch_write_size_ = parse_size(batch_write_size);

    snapshot_sync_worker_num_ =
        !CheckCommandLineFlagIsDefault("snapshot_sync_worker_num")
            ? FLAGS_snapshot_sync_worker_num
            : config.GetInteger("store",
                                "snapshot_sync_worker_num",
                                FLAGS_snapshot_sync_worker_num);

    periodic_compaction_seconds_ =
        !CheckCommandLineFlagIsDefault("rocksdb_periodic_compaction_seconds")
            ? FLAGS_rocksdb_periodic_compaction_seconds
            : config.GetInteger("store",
                                "rocksdb_periodic_compaction_seconds",
                                FLAGS_rocksdb_periodic_compaction_seconds);
    dialy_offpeak_time_utc_ =
        !CheckCommandLineFlagIsDefault("rocksdb_dialy_offpeak_time_utc")
            ? FLAGS_rocksdb_dialy_offpeak_time_utc
            : config.GetString("store",
                               "rocksdb_dialy_offpeak_time_utc",
                               FLAGS_rocksdb_dialy_offpeak_time_utc);
};

#if ROCKSDB_CLOUD_FS()

RocksDBCloudConfig::RocksDBCloudConfig(const INIReader &config)
{
#if ROCKSDB_CLOUD_FS_TYPE == ROCKSDB_CLOUD_FS_TYPE_S3
    aws_access_key_id_ =
        !CheckCommandLineFlagIsDefault("rocksdb_cloud_aws_access_key_id")
            ? FLAGS_rocksdb_cloud_aws_access_key_id
            : config.GetString("store",
                               "rocksdb_cloud_aws_access_key_id",
                               FLAGS_rocksdb_cloud_aws_access_key_id);
    aws_secret_key_ =
        !CheckCommandLineFlagIsDefault("rocksdb_cloud_aws_secret_access_key")
            ? FLAGS_rocksdb_cloud_aws_secret_access_key
            : config.GetString("store",
                               "rocksdb_cloud_aws_secret_access_key",
                               FLAGS_rocksdb_cloud_aws_secret_access_key);

#endif

    bucket_name_ = !CheckCommandLineFlagIsDefault("rocksdb_cloud_bucket_name")
                       ? FLAGS_rocksdb_cloud_bucket_name
                       : config.GetString("store",
                                          "rocksdb_cloud_bucket_name",
                                          FLAGS_rocksdb_cloud_bucket_name);
    bucket_prefix_ =
        !CheckCommandLineFlagIsDefault("rocksdb_cloud_bucket_prefix")
            ? FLAGS_rocksdb_cloud_bucket_prefix
            : config.GetString("store",
                               "rocksdb_cloud_bucket_prefix",
                               FLAGS_rocksdb_cloud_bucket_prefix);
    region_ =
        !CheckCommandLineFlagIsDefault("rocksdb_cloud_region")
            ? FLAGS_rocksdb_cloud_region
            : config.GetString(
                  "store", "rocksdb_cloud_region", FLAGS_rocksdb_cloud_region);
    std::string rocksdb_cloud_sst_file_cache_size =
        !CheckCommandLineFlagIsDefault("rocksdb_cloud_sst_file_cache_size")
            ? FLAGS_rocksdb_cloud_sst_file_cache_size
            : config.GetString("store",
                               "rocksdb_cloud_sst_file_cache_size",
                               FLAGS_rocksdb_cloud_sst_file_cache_size);
    uint32_t rocksdb_cloud_db_ready_timeout_sec =
        !CheckCommandLineFlagIsDefault("rocksdb_cloud_db_ready_timeout_sec")
            ? FLAGS_rocksdb_cloud_db_ready_timeout_sec
            : config.GetInteger("store",
                                "rocksdb_cloud_db_ready_timeout_sec",
                                FLAGS_rocksdb_cloud_db_ready_timeout_sec);
    uint32_t rocksdb_cloud_db_file_deletion_delay_sec =
        !CheckCommandLineFlagIsDefault(
            "rocksdb_cloud_db_file_deletion_delay_sec")
            ? FLAGS_rocksdb_cloud_db_file_deletion_delay_sec
            : config.GetInteger("store",
                                "rocksdb_cloud_db_file_deletion_delay_sec",
                                FLAGS_rocksdb_cloud_db_file_deletion_delay_sec);

    sst_file_cache_size_ =
        parse_size(rocksdb_cloud_sst_file_cache_size.c_str());
    db_ready_timeout_us_ = rocksdb_cloud_db_ready_timeout_sec * 1000000;
    db_file_deletion_delay_ = rocksdb_cloud_db_file_deletion_delay_sec;

    s3_endpoint_url_ =
        !CheckCommandLineFlagIsDefault("rocksdb_cloud_s3_endpoint_url")
            ? FLAGS_rocksdb_cloud_s3_endpoint_url
            : config.GetString("store",
                               "rocksdb_cloud_s3_endpoint_url",
                               FLAGS_rocksdb_cloud_s3_endpoint_url);

    warm_up_thread_num_ =
        !CheckCommandLineFlagIsDefault("rocksdb_cloud_warm_up_thread_num")
            ? FLAGS_rocksdb_cloud_warm_up_thread_num
            : config.GetInteger("store",
                                "rocksdb_cloud_warm_up_thread_num",
                                FLAGS_rocksdb_cloud_warm_up_thread_num);
}

#endif

}  // namespace EloqShare
#endif
