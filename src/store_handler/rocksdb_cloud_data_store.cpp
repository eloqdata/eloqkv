#include "rocksdb_cloud_data_store.h"

#include <aws/s3/S3Client.h>
#include <bthread/condition_variable.h>
#include <rocksdb/db.h>
#include <rocksdb/listener.h>
#include <rocksdb/rate_limiter.h>
#include <rocksdb/statistics.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "data_store_service.h"
#include "ds_request.pb.h"
#include "redis_string_num.h"
#if ROCKSDB_CLOUD_FS()
#include "rocksdb/cloud/cloud_storage_provider.h"
#endif
#include "store_util.h"

namespace EloqDS
{
#if ROCKSDB_CLOUD_FS()

RocksDBCloudDataStore::RocksDBCloudDataStore(
    const EloqShare::RocksDBCloudConfig &cloud_config,
    const EloqShare::RocksDBConfig &config,
    bool create_if_missing,
    bool tx_enable_cache_replacement,
    uint32_t shard_id,
    DataStoreService *data_store_service)
    : DataStore(shard_id, data_store_service),
      enable_stats_(config.enable_stats_),
      stats_dump_period_sec_(config.stats_dump_period_sec_),
      storage_path_(config.storage_path_ + "/ds_" + std::to_string(shard_id)),
      max_write_buffer_number_(config.max_write_buffer_number_),
      max_background_jobs_(config.max_background_jobs_),
      max_background_flushes_(config.max_background_flush_),
      max_background_compactions_(config.max_background_compaction_),
      target_file_size_base_(config.target_file_size_base_bytes_),
      target_file_size_multiplier_(config.target_file_size_multiplier_),
      write_buff_size_(config.write_buffer_size_bytes_),
      use_direct_io_for_flush_and_compaction_(
          config.use_direct_io_for_flush_and_compaction_),
      use_direct_io_for_read_(config.use_direct_io_for_read_),
      level0_stop_writes_trigger_(config.level0_stop_writes_trigger_),
      level0_slowdown_writes_trigger_(config.level0_slowdown_writes_trigger_),
      level0_file_num_compaction_trigger_(
          config.level0_file_num_compaction_trigger_),
      max_bytes_for_level_base_(config.max_bytes_for_level_base_bytes_),
      max_bytes_for_level_multiplier_(config.max_bytes_for_level_multiplier_),
      compaction_style_(config.compaction_style_),
      soft_pending_compaction_bytes_limit_(
          config.soft_pending_compaction_bytes_limit_bytes_),
      hard_pending_compaction_bytes_limit_(
          config.hard_pending_compaction_bytes_limit_bytes_),
      max_subcompactions_(config.max_subcompactions_),
      write_rate_limit_(config.write_rate_limit_bytes_),
      batch_write_size_(config.batch_write_size_),
      periodic_compaction_seconds_(config.periodic_compaction_seconds_),
      dialy_offpeak_time_utc_(config.dialy_offpeak_time_utc_),
      db_path_(storage_path_ + "/db/"),
      ckpt_path_(storage_path_ + "/rocksdb_snapshot/"),
      backup_path_(storage_path_ + "/backups/"),
      received_snapshot_path_(storage_path_ + "/received_snapshot/"),
      create_db_if_missing_(create_if_missing),
      tx_enable_cache_replacement_(tx_enable_cache_replacement),
      ttl_compaction_filter_(nullptr),
      pre_built_tables_(),
      cloud_config_(cloud_config),
      cloud_fs_(),
      cloud_env_(nullptr),
      db_(nullptr)
{
    info_log_level_ = EloqShare::StringToInfoLogLevel(config.info_log_level_);
    query_worker_pool_ =
        std::make_unique<ThreadWorkerPool>(config.query_worker_num_);
}

RocksDBCloudDataStore::~RocksDBCloudDataStore()
{
    Shutdown();
}

void RocksDBCloudDataStore::Shutdown()
{
    std::unique_lock<std::shared_mutex> db_lk(db_mux_);

    // shutdown query worker pool
    query_worker_pool_->Shutdown();

    if (db_ != nullptr)
    {
        for (auto &cfh : column_families_)
        {
            rocksdb::ColumnFamilyHandle *cfh_ptr = cfh.second.release();
            db_->DestroyColumnFamilyHandle(cfh_ptr);
        }
        column_families_.clear();
        db_->Close();
        db_->PauseBackgroundWork();
        delete db_;
        db_ = nullptr;
        ttl_compaction_filter_ = nullptr;
        cloud_env_ = nullptr;
        cloud_fs_ = nullptr;
    }
}

bool RocksDBCloudDataStore::AppendPreBuiltTable(std::string_view table_name)
{
    pre_built_tables_.emplace(table_name, table_name);
    return true;
}

std::string toLower(const std::string &str)
{
    std::string lowerStr = str;
    std::transform(lowerStr.begin(),
                   lowerStr.end(),
                   lowerStr.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return lowerStr;
}

rocksdb::S3ClientFactory RocksDBCloudDataStore::BuildS3ClientFactory(
    const std::string &endpoint)
{
    return [endpoint](const std::shared_ptr<Aws::Auth::AWSCredentialsProvider>
                          &credentialsProvider,
                      const Aws::Client::ClientConfiguration &baseConfig)
               -> std::shared_ptr<Aws::S3::S3Client>
    {
        // Check endpoint url start with http or https
        if (endpoint.empty())
        {
            return nullptr;
        }

        std::string endpoint_url = toLower(endpoint);

        bool secured_url = false;
        if (endpoint_url.rfind("http://", 0) == 0)
        {
            secured_url = false;
        }
        else if (endpoint_url.rfind("https://", 0) == 0)
        {
            secured_url = true;
        }
        else
        {
            LOG(ERROR) << "Invalid S3 endpoint url";
            std::abort();
        }

        // Create a new configuration based on the base config
        Aws::Client::ClientConfiguration config = baseConfig;
        config.endpointOverride = endpoint_url;
        if (secured_url)
        {
            config.scheme = Aws::Http::Scheme::HTTPS;
        }
        else
        {
            config.scheme = Aws::Http::Scheme::HTTP;
        }
        // Disable SSL verification for HTTPS
        config.verifySSL = false;

        // Create and return the S3 client
        if (credentialsProvider)
        {
            return std::make_shared<Aws::S3::S3Client>(
                credentialsProvider,
                config,
                Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
                true /* useVirtualAddressing */);
        }
        else
        {
            return std::make_shared<Aws::S3::S3Client>(config);
        }
    };
}

bool RocksDBCloudDataStore::StartDB(std::string cookie, std::string prev_cookie)
{
    if (db_)
    {
        // db is already started, no op
        DLOG(INFO) << "DBCloud already started";
        return true;
    }

#if ROCKSDB_CLOUD_FS_TYPE == ROCKSDB_CLOUD_FS_TYPE_S3
    if (cloud_config_.aws_access_key_id_.length() == 0 ||
        cloud_config_.aws_secret_key_.length() == 0)
    {
        LOG(INFO) << "No AWS_ACCESS_KEY_ID/AWS_SECRET_ACCESS_KEY "
                     "provided, use default credential provider";
        cfs_options_.credentials.type = rocksdb::AwsAccessType::kUndefined;
    }
    else
    {
        cfs_options_.credentials.InitializeSimple(
            cloud_config_.aws_access_key_id_, cloud_config_.aws_secret_key_);
    }

    rocksdb::Status status = cfs_options_.credentials.HasValid();
    if (!status.ok())
    {
        LOG(ERROR) << "Valid AWS_ACCESS_KEY_ID/AWS_SECRET_ACCESS_KEY "
                      "is required, error: "
                   << status.ToString();
        return false;
    }
#endif

    cfs_options_.src_bucket.SetBucketName(cloud_config_.bucket_name_,
                                          cloud_config_.bucket_prefix_);
    cfs_options_.src_bucket.SetRegion(cloud_config_.region_);
    cfs_options_.src_bucket.SetObjectPath("rocksdb_cloud");
    cfs_options_.dest_bucket.SetBucketName(cloud_config_.bucket_name_,
                                           cloud_config_.bucket_prefix_);
    cfs_options_.dest_bucket.SetRegion(cloud_config_.region_);
    cfs_options_.dest_bucket.SetObjectPath("rocksdb_cloud");
    // Add sst_file_cache for accerlating random access on sst files
    cfs_options_.sst_file_cache =
        rocksdb::NewLRUCache(cloud_config_.sst_file_cache_size_);
    // delay cloud file deletion for 1 hour
    cfs_options_.cloud_file_deletion_delay =
        std::chrono::seconds(cloud_config_.db_file_deletion_delay_);
    // sync cloudmanifest and manifest files when open db
    cfs_options_.resync_on_open = true;

    if (!cloud_config_.s3_endpoint_url_.empty())
    {
        cfs_options_.s3_client_factory =
            BuildS3ClientFactory(cloud_config_.s3_endpoint_url_);
    }

    DLOG(INFO) << "DBCloud Open";
    rocksdb::CloudFileSystem *cfs;
    // Open the cloud file system
    status = EloqShare::NewCloudFileSystem(cfs_options_, &cfs);
    if (!status.ok())
    {
        LOG(ERROR) << "Unable to create cloud storage filesystem, cloud type: "
#if ROCKSDB_CLOUD_FS_TYPE == ROCKSDB_CLOUD_FS_TYPE_S3
                   << "Aws"
#elif ROCKSDB_CLOUD_FS_TYPE == ROCKSDB_CLOUD_FS_TYPE_GCS
                   << "Gcp"
#endif
                   << ", at path rocksdb_cloud with bucket "
                   << cfs_options_.src_bucket.GetBucketName()
                   << ", with error: " << status.ToString();

        return false;
    }

    std::string cookie_on_open = "";
    std::string new_cookie_on_open = "";

    // TODO(githubzilla): ng_id is not used in the current implementation,
    // remove it later
    int64_t ng_id = 0;
    auto storage_provider = cfs->GetStorageProvider();
    int64_t max_term = EloqShare::FindMaxTermFromCloudManifestFiles(
        storage_provider,
        cloud_config_.bucket_prefix_,
        cloud_config_.bucket_name_,
        ng_id);

    if (max_term != -1)
    {
        cookie_on_open = EloqShare::MakeCloudManifestCookie(ng_id, max_term);
        new_cookie_on_open =
            EloqShare::MakeCloudManifestCookie(ng_id, max_term + 1);
    }
    else
    {
        cookie_on_open = "";
        new_cookie_on_open = EloqShare::MakeCloudManifestCookie(ng_id, 0);
    }

    // new CLOUDMANIFEST suffixed by cookie and epochID suffixed
    // MANIFEST files are generated, which won't overwrite the old ones
    // opened by previous leader
    cfs_options_.cookie_on_open = cookie_on_open;
    cfs_options_.new_cookie_on_open = new_cookie_on_open;

    DLOG(INFO) << "StartDB cookie_on_open: " << cfs_options_.cookie_on_open
               << " new_cookie_on_open: " << cfs_options_.new_cookie_on_open;

    // destroy the cfs after checking the cookie on open
    delete cfs;

    // Reopen the cloud file system
    status = EloqShare::NewCloudFileSystem(cfs_options_, &cfs);
    if (!status.ok())
    {
        LOG(ERROR) << "Unable to create cloud storage filesystem, cloud type: "
#if ROCKSDB_CLOUD_FS_TYPE == ROCKSDB_CLOUD_FS_TYPE_S3
                   << "Aws"
#elif ROCKSDB_CLOUD_FS_TYPE == ROCKSDB_CLOUD_FS_TYPE_GCS
                   << "Gcp"
#endif
                   << ", at path rocksdb_cloud with bucket "
                   << cfs_options_.src_bucket.GetBucketName()
                   << ", with error: " << status.ToString();

        std::abort();
    }
    cloud_fs_.reset(cfs);
    // Create options and use the AWS file system that we created
    // earlier
    cloud_env_ = rocksdb::NewCompositeEnv(cloud_fs_);

    return OpenCloudDB(cfs_options_);
}

bool RocksDBCloudDataStore::OpenCloudDB(
    const rocksdb::CloudFileSystemOptions &cfs_options)
{
    rocksdb::Options options;
    options.env = cloud_env_.get();
    options.create_if_missing = create_db_if_missing_;
    options.create_missing_column_families = true;
    // boost write performance by enabling unordered write
    options.unordered_write = true;

    // print db statistics every 60 seconds
    if (enable_stats_)
    {
        options.statistics = rocksdb::CreateDBStatistics();
        options.stats_dump_period_sec = stats_dump_period_sec_;
    }

    // Max background jobs number, rocksdb will auto turn max flush(1/4 of
    // max_background_jobs) and compaction jobs(3/4 of max_background_jobs)
    if (max_background_jobs_ > 0)
    {
        options.max_background_jobs = max_background_jobs_;
    }

    if (max_background_flushes_ > 0)
    {
        options.max_background_flushes = max_background_flushes_;
    }

    if (max_background_compactions_ > 0)
    {
        options.max_background_compactions = max_background_compactions_;
    }

    options.use_direct_io_for_flush_and_compaction =
        use_direct_io_for_flush_and_compaction_;
    options.use_direct_reads = use_direct_io_for_read_;

    // Set compation style
    if (compaction_style_ == "universal")
    {
        LOG(WARNING)
            << "Universal compaction has a size limitation. Please be careful "
               "when your DB (or column family) size is over 100GB";
        options.compaction_style = rocksdb::kCompactionStyleUniversal;
    }
    else if (compaction_style_ == "level")
    {
        options.compaction_style = rocksdb::kCompactionStyleLevel;
    }
    else if (compaction_style_ == "fifo")
    {
        LOG(ERROR) << "FIFO compaction style should not be used";
        std::abort();
    }
    else
    {
        LOG(ERROR) << "Invalid compaction style: " << compaction_style_;
        std::abort();
    }

    // set the max subcompactions
    if (max_subcompactions_ > 0)
    {
        options.max_subcompactions = max_subcompactions_;
    }

    // set the write rate limit
    if (write_rate_limit_ > 0)
    {
        options.rate_limiter.reset(
            rocksdb::NewGenericRateLimiter(write_rate_limit_));
    }

    options.info_log_level = info_log_level_;
    options.best_efforts_recovery = false;
    options.skip_checking_sst_file_sizes_on_db_open = true;
    options.skip_stats_update_on_db_open = true;
    // Important! keep atomic_flush true, since we disabled WAL
    options.atomic_flush = true;
    auto db_event_listener =
        std::make_shared<EloqShare::RocksDBEventListener>();
    options.listeners.emplace_back(db_event_listener);

    // The following two configuration items are setup for purpose of removing
    // expired kv data items according to their ttl Rocksdb will compact all sst
    // files which are older than periodic_compaction_seconds_ at
    // dialy_offpeak_time_utc_ Then all kv data items in the sst files will go
    // through the TTLCompactionFilter which is configurated for column family
    options.periodic_compaction_seconds = periodic_compaction_seconds_;
    options.daily_offpeak_time_utc = dialy_offpeak_time_utc_;

    // The max_open_files default value is -1, it cause DB open all files on
    // DB::Open() This behavior causes 2 effects,
    // 1. DB::Open() will be slow
    // 2. During DB::Open, some of the opened sst files keep in LRUCache
    // will be deleted due to LRU policy, which causes DB::Open failed
    options.max_open_files = 0;

    std::vector<rocksdb::ColumnFamilyDescriptor> cfds;
    rocksdb::ColumnFamilyOptions cf_options;

    if (target_file_size_base_ > 0)
    {
        cf_options.target_file_size_base = target_file_size_base_;
    }

    if (target_file_size_multiplier_ > 0)
    {
        cf_options.target_file_size_multiplier = target_file_size_multiplier_;
    }

    // mem table size
    if (write_buff_size_ > 0)
    {
        cf_options.write_buffer_size = write_buff_size_;
    }
    // Max write buffer number
    if (max_write_buffer_number_ > 0)
    {
        cf_options.max_write_buffer_number = max_write_buffer_number_;
    }

    if (level0_slowdown_writes_trigger_ > 0)
    {
        cf_options.level0_slowdown_writes_trigger =
            level0_slowdown_writes_trigger_;
    }

    if (level0_stop_writes_trigger_ > 0)
    {
        cf_options.level0_stop_writes_trigger = level0_stop_writes_trigger_;
    }

    if (level0_file_num_compaction_trigger_ > 0)
    {
        cf_options.level0_file_num_compaction_trigger =
            level0_file_num_compaction_trigger_;
    }

    if (soft_pending_compaction_bytes_limit_ > 0)
    {
        cf_options.soft_pending_compaction_bytes_limit =
            soft_pending_compaction_bytes_limit_;
    }

    if (hard_pending_compaction_bytes_limit_ > 0)
    {
        cf_options.hard_pending_compaction_bytes_limit =
            hard_pending_compaction_bytes_limit_;
    }

    if (max_bytes_for_level_base_ > 0)
    {
        cf_options.max_bytes_for_level_base = max_bytes_for_level_base_;
    }

    if (max_bytes_for_level_multiplier_ > 0)
    {
        cf_options.max_bytes_for_level_multiplier =
            max_bytes_for_level_multiplier_;
    }

    // set ttl compaction filter
    assert(ttl_compaction_filter_ == nullptr);
    ttl_compaction_filter_ = std::make_unique<EloqShare::TTLCompactionFilter>();

    cf_options.compaction_filter =
        static_cast<rocksdb::CompactionFilter *>(ttl_compaction_filter_.get());

    bool need_init_prebuilt_schema = false;
    // list all column families and open them
    std::vector<std::string> column_families;
    rocksdb::Status status = rocksdb::DBCloud::ListColumnFamilies(
        options, db_path_, &column_families);
    if (status.ok())
    {
        for (const auto &cf : column_families)
        {
            cfds.emplace_back(cf, cf_options);
        }
    }
    else
    {
        LOG(ERROR) << "Failed to list column families: " << status.ToString()
                   << ", use default prebuilt tables.";
        need_init_prebuilt_schema = true;
        cfds.emplace_back(rocksdb::kDefaultColumnFamilyName,
                          rocksdb::ColumnFamilyOptions());

        for (const auto &pre_built_table : pre_built_tables_)
        {
            std::string cf = pre_built_table.first;
            cfds.emplace_back(cf, cf_options);
        }
    }

    auto start = std::chrono::system_clock::now();
    std::vector<rocksdb::ColumnFamilyHandle *> cfhs;
    std::unique_lock<std::shared_mutex> db_lk(db_mux_);
    status =
        rocksdb::DBCloud::Open(options, db_path_, cfds, "", 0, &cfhs, &db_);

    auto end = std::chrono::system_clock::now();
    auto duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    DLOG(INFO) << "DBCloud Open took " << duration.count() << " ms";

    if (!status.ok())
    {
        ttl_compaction_filter_ = nullptr;

        assert(cfhs.empty());

        LOG(ERROR) << "Unable to open db at path " << storage_path_
                   << " with bucket " << cfs_options.src_bucket.GetBucketName()
                   << " with error: " << status.ToString();

        // db does not exist. This node cannot escalate to be the ng leader.
        return false;
    }

    if (cloud_config_.warm_up_thread_num_ != 0)
    {
        db_->WarmUp(cloud_config_.warm_up_thread_num_);
    }

    // Reset max_open_files to default value of -1 after DB::Open
    db_->SetDBOptions({{"max_open_files", "-1"}});

    // set the column family handlers
    for (auto cfh : cfhs)
    {
        column_families_.emplace(cfh->GetName(), cfh);
    }

    auto cf_pair = column_families_.find(rocksdb::kDefaultColumnFamilyName);
    assert(cf_pair != column_families_.end());
    rocksdb::ColumnFamilyHandle *cfh_default = cf_pair->second.get();
    assert(cfh_default != nullptr);

    if (need_init_prebuilt_schema)
    {
        // create redis table catalog info
        for (auto cfh : cfhs)
        {
            // Create and update prebuilt tables info
            std::string table_name = cfh->GetName();
            if (cfh->GetName() == rocksdb::kDefaultColumnFamilyName)
            {
                continue;
            }
            // find the table schema
            std::string table_key = table_name + "_catalog";
            rocksdb::PinnableWideColumns pinnable_table_catalog;
            auto status = db_->GetEntity(rocksdb::ReadOptions(),
                                         cfh_default,
                                         table_key,
                                         &pinnable_table_catalog);
            if (!status.ok())
            {
                if (status.IsNotFound())
                {
                    // if the table schema is not found, then we need to create
                    // it
                    auto pre_built_table = pre_built_tables_.find(table_name);
                    if (pre_built_table != pre_built_tables_.end())
                    {
                        rocksdb::WideColumns table_catalog_wc;
                        table_catalog_wc.emplace_back("kv_cf_name", table_name);
                        uint64_t version = 100U;
                        rocksdb::Slice commit_ts_value(
                            reinterpret_cast<const char *>(&version),
                            sizeof(uint64_t));
                        table_catalog_wc.emplace_back("version",
                                                      commit_ts_value);
                        status = db_->PutEntity(rocksdb::WriteOptions(),
                                                cfh_default,
                                                table_key,
                                                table_catalog_wc);
                        if (!status.ok())
                        {
                            LOG(ERROR) << "Unable to write table schema to db "
                                          "with error: "
                                       << status.ToString();
                            return false;
                        }
                        LOG(INFO)
                            << "Create table schema for table: " << table_name;
                    }
                    else
                    {
                        LOG(ERROR) << "Unable to get table schema for table: "
                                   << table_name << " in pre_built_tables_";
                        return false;
                    }
                }
                else
                {
                    LOG(ERROR)
                        << "Unable to get table schema for table: "
                        << table_name << " with error: " << status.ToString();
                    return false;
                }
            }
        }
    }

    LOG(INFO) << "RocksDB Cloud started";
    return true;
}

rocksdb::DBCloud *RocksDBCloudDataStore::GetDBPtr()
{
    return db_;
}

inline std::string MakeCloudManifestCookie(int64_t cc_ng_id, int64_t term)
{
    return std::to_string(cc_ng_id) + "-" + std::to_string(term);
}

inline std::string MakeCloudManifestFile(const std::string &dbname,
                                         int64_t cc_ng_id,
                                         int64_t term)
{
    if (cc_ng_id < 0 || term < 0)
    {
        return dbname + "/CLOUDMANIFEST";
    }

    assert(cc_ng_id >= 0 && term >= 0);

    return dbname + "/CLOUDMANIFEST-" + std::to_string(cc_ng_id) + "-" +
           std::to_string(term);
}

inline bool IsCloudManifestFile(const std::string &filename)
{
    return filename.find("CLOUDMANIFEST") != std::string::npos;
}

// Helper function to split a string by a delimiter
inline std::vector<std::string> SplitString(const std::string &str,
                                            char delimiter)
{
    std::vector<std::string> tokens;
    std::stringstream ss(str);
    std::string token;
    while (std::getline(ss, token, delimiter))
    {
        tokens.push_back(token);
    }
    return tokens;
}

inline bool GetCookieFromCloudManifestFile(const std::string &filename,
                                           int64_t &cc_ng_id,
                                           int64_t &term)
{
    const std::string prefix = "CLOUDMANIFEST";
    auto pos = filename.rfind('/');
    std::string manifest_part =
        (pos != std::string::npos) ? filename.substr(pos + 1) : filename;

    // Check if the filename starts with "CLOUDMANIFEST"
    if (manifest_part.find(prefix) != 0)
    {
        return false;
    }

    // Remove the prefix "CLOUDMANIFEST" to parse the rest
    std::string suffix = manifest_part.substr(prefix.size());

    // If there's no suffix
    if (suffix.empty())
    {
        cc_ng_id = -1;
        term = -1;
        return false;
    }
    else if (suffix[0] == '-')
    {
        // Parse the "-cc_ng_id-term" format
        suffix = suffix.substr(1);  // Remove the leading '-'
        std::vector<std::string> parts = SplitString(suffix, '-');
        if (parts.size() != 2)
        {
            return false;
        }

        bool res =
            EloqKV::string2ll(parts[0].c_str(), parts[0].size(), cc_ng_id);
        if (!res)
        {
            return false;
        }
        res = EloqKV::string2ll(parts[1].c_str(), parts[1].size(), term);
        return res;
    }

    return false;
}

inline int64_t FindMaxTermFromCloudManifestFiles(
    const std::shared_ptr<ROCKSDB_NAMESPACE::CloudStorageProvider>
        &storage_provider,
    const std::string &bucket_prefix,
    const std::string &bucket_name,
    const int64_t cc_ng_id_in_cookie)
{
    // find the max term cookie from cloud manifest files
    // read only db should be opened with the latest cookie
    std::vector<std::string> cloud_objects;
    auto st = storage_provider->ListCloudObjects(
        bucket_prefix + bucket_name, "rocksdb_cloud", &cloud_objects);
    if (!st.ok())
    {
        LOG(ERROR) << "Failed to list cloud objects, error: " << st.ToString();
        return -1;
    }

    int64_t max_term = -1;
    for (const auto &object : cloud_objects)
    {
        if (IsCloudManifestFile(object))
        {
            int64_t cc_ng_id;
            int64_t term;
            bool res = GetCookieFromCloudManifestFile(object, cc_ng_id, term);
            if (cc_ng_id_in_cookie == cc_ng_id && res)
            {
                if (term > max_term)
                {
                    max_term = term;
                }
            }
        }
    }
    DLOG(INFO) << "FindMaxTermFromCloudManifestFiles, cc_ng_id: "
               << cc_ng_id_in_cookie << " max_term: " << max_term;
    return max_term;
}

bool RocksDBCloudDataStore::Connect()
{
    // before opening rocksdb, rocksdb_storage_path_ must exist, create it
    // if not exist
    std::error_code error_code;
    bool rocksdb_storage_path_exists =
        std::filesystem::exists(db_path_, error_code);
    if (error_code.value() != 0)
    {
        LOG(ERROR) << "unable to check rocksdb directory: " << db_path_
                   << ", error code: " << error_code.value()
                   << ", error message: " << error_code.message();
        return false;
    }
    if (!rocksdb_storage_path_exists)
    {
        std::filesystem::create_directories(db_path_, error_code);
        if (error_code.value() != 0)
        {
            LOG(ERROR) << "unable to create rocksdb directory: " << db_path_
                       << ", error code: " << error_code.value()
                       << ", error message: " << error_code.message();
            return false;
        }
    }

    // Cleanup previous snapshots
    rocksdb_storage_path_exists =
        std::filesystem::exists(ckpt_path_, error_code);
    if (error_code.value() != 0)
    {
        LOG(ERROR) << "unable to check rocksdb directory: " << ckpt_path_
                   << ", error code: " << error_code.value()
                   << ", error message: " << error_code.message();
        return false;
    }
    if (!rocksdb_storage_path_exists)
    {
        std::filesystem::create_directories(ckpt_path_, error_code);
        if (error_code.value() != 0)
        {
            LOG(ERROR) << "unable to create rocksdb directory: " << ckpt_path_
                       << ", error code: " << error_code.value()
                       << ", error message: " << error_code.message();
            return false;
        }
    }
    else
    {
        // clean up previous snapshots
        for (const auto &entry :
             std::filesystem::directory_iterator(ckpt_path_))
        {
            std::filesystem::remove_all(entry.path());
        }
    }

    rocksdb_storage_path_exists =
        std::filesystem::exists(received_snapshot_path_, error_code);
    if (error_code.value() != 0)
    {
        LOG(ERROR) << "unable to check rocksdb directory: "
                   << received_snapshot_path_
                   << ", error code: " << error_code.value()
                   << ", error message: " << error_code.message();
        return false;
    }
    if (!rocksdb_storage_path_exists)
    {
        std::filesystem::create_directories(received_snapshot_path_,
                                            error_code);
        if (error_code.value() != 0)
        {
            LOG(ERROR) << "unable to create rocksdb directory: "
                       << received_snapshot_path_
                       << ", error code: " << error_code.value()
                       << ", error message: " << error_code.message();
            return false;
        }
    }
    else
    {
        // clean up previous snapshots
        for (const auto &entry :
             std::filesystem::directory_iterator(received_snapshot_path_))
        {
            std::filesystem::remove_all(entry.path());
        }
    }

    rocksdb_storage_path_exists =
        std::filesystem::exists(backup_path_, error_code);
    if (error_code.value() != 0)
    {
        LOG(ERROR) << "unable to check rocksdb directory: " << backup_path_
                   << ", error code: " << error_code.value()
                   << ", error message: " << error_code.message();
        return false;
    }
    if (!rocksdb_storage_path_exists)
    {
        std::filesystem::create_directories(backup_path_, error_code);
        if (error_code.value() != 0)
        {
            LOG(ERROR) << "unable to create rocksdb directory: " << backup_path_
                       << ", error code: " << error_code.value()
                       << ", error message: " << error_code.message();
            return false;
        }
    }

    return true;
}

bool RocksDBCloudDataStore::InitializeClusterConfig(
    uint32_t req_shard_id,
    const std::unordered_map<uint32_t,
                             std::vector<::EloqDS::remote::ClusterNodeConfig>>
        &ng_configs)
{
    auto shard_status = FetchDSShardStatus();
    if (shard_status != DSShardStatus::ReadWrite)
    {
        if (shard_status == DSShardStatus::Closed)
        {
            LOG(ERROR) << "InitializeClusterConfig failed: Requested data not "
                          "on local node.";
        }
        else
        {
            assert(shard_status == DSShardStatus::ReadOnly);
            LOG(ERROR)
                << "InitializeClusterConfig failed: Write to read-only DB.";
        }
        return false;
    }

    // Increase write counter before starting the write operation
    IncreaseWriteCounter();

    uint64_t version = 2;

    std::vector<uint32_t> ng_ids;
    std::vector<std::string> ng_members;
    std::vector<std::string> ng_members_is_candidate;
    std::vector<uint32_t> node_ids;
    std::vector<std::string> ips;
    std::vector<uint16_t> ports;

    std::vector<::EloqDS::remote::ClusterNodeConfig> nodes;
    EloqShare::ExtractNodesConfigs(ng_configs, nodes);
    for (const ::EloqDS::remote::ClusterNodeConfig &node : nodes)
    {
        node_ids.push_back(node.node_id());
        ips.push_back(node.host_name());
        ports.push_back(node.port());
    }

    for (auto ng_pair : ng_configs)
    {
        ng_ids.push_back(ng_pair.first);
        std::vector<uint32_t> group_members;
        std::vector<bool> group_members_is_candidate;
        for (auto &node : ng_pair.second)
        {
            group_members.push_back(node.node_id());
            group_members_is_candidate.push_back(node.is_candidate());
        }
        ng_members.push_back(EloqShare::SerializeVectorToString<uint32_t>(
            group_members, ",", EloqShare::uint32_str_converter));
        ng_members_is_candidate.push_back(
            EloqShare::SerializeVectorToString<bool>(
                group_members_is_candidate,
                ",",
                EloqShare::bool_str_converter));
    }

    std::string node_ids_str = EloqShare::SerializeVectorToString<uint32_t>(
        node_ids, " ", EloqShare::uint32_str_converter);
    std::string ips_str = EloqShare::SerializeVectorToString<std::string>(
        ips, " ", EloqShare::string_str_converter);
    std::string ports_str = EloqShare::SerializeVectorToString<uint16_t>(
        ports, " ", EloqShare::uint16_str_converter);
    std::string ng_ids_str = EloqShare::SerializeVectorToString<uint32_t>(
        ng_ids, " ", EloqShare::uint32_str_converter);
    std::string ng_members_str =
        EloqShare::SerializeVectorToString<std::string>(
            ng_members, " ", EloqShare::string_str_converter);
    std::string ng_members_is_candidate_str =
        EloqShare::SerializeVectorToString<std::string>(
            ng_members_is_candidate, " ", EloqShare::string_str_converter);
    std::string verstion_str = std::to_string(version);

    // Create a rocksdb::WideColumns object
    rocksdb::WideColumns cluster_config_wc;
    cluster_config_wc.emplace_back("node_ids", node_ids_str);
    cluster_config_wc.emplace_back("ips", ips_str);
    cluster_config_wc.emplace_back("ports", ports_str);
    cluster_config_wc.emplace_back("ng_ids", ng_ids_str);
    cluster_config_wc.emplace_back("ng_members", ng_members_str);
    cluster_config_wc.emplace_back("ng_members_is_candidate",
                                   ng_members_is_candidate_str);
    cluster_config_wc.emplace_back("version", verstion_str);

    // Write the cluster config to the rocksdb
    std::shared_lock<std::shared_mutex> db_lk(db_mux_);
    rocksdb::WriteOptions write_options;
    write_options.disableWAL = true;
    auto db = GetDBPtr();
    if (db == nullptr)
    {
        LOG(ERROR) << "Unable to write to db with error: db is not opened";
        DecreaseWriteCounter();  // Decrease counter before error return
        return false;
    }

    auto status =
        db->PutEntity(write_options,
                      GetColumnFamilyHandler(rocksdb::kDefaultColumnFamilyName),
                      "cluster_config",
                      cluster_config_wc);

    if (!status.ok())
    {
        LOG(ERROR) << "Unable to write to db with error: " << status.ToString();
        DecreaseWriteCounter();  // Decrease counter before error return
        return false;
    }

    rocksdb::FlushOptions flush_options;
    flush_options.allow_write_stall = false;
    flush_options.wait = true;
    status = db->Flush(flush_options);
    if (!status.ok())
    {
        LOG(ERROR) << "Unable to write to db with error: " << status.ToString();
        DecreaseWriteCounter();  // Decrease counter before error return
        return false;
    }

    DecreaseWriteCounter();  // Decrease counter before error return
    return true;
}

void RocksDBCloudDataStore::UpdateClusterConfig(
    uint32_t req_shard_id,
    uint64_t version,
    const std::unordered_map<uint32_t,
                             std::vector<::EloqDS::remote::ClusterNodeConfig>>
        &new_cnf,
    std::function<void(const ::EloqDS::remote::CommonResult &result)> on_finish)
{
    query_worker_pool_->SubmitWork(
        [this, req_shard_id, version, new_cnf, on_finish]()
        {
            auto shard_status = FetchDSShardStatus();
            if (shard_status != DSShardStatus::ReadWrite)
            {
                ::EloqDS::remote::CommonResult result;
                if (shard_status == DSShardStatus::Closed)
                {
                    data_store_service_->PrepareShardingError(
                        req_shard_id, shard_id_, &result);
                    on_finish(result);
                }
                else
                {
                    assert(shard_status == DSShardStatus::ReadOnly);
                    result.set_error_code(::EloqDS::remote::DataStoreError::
                                              WRITE_TO_READ_ONLY_DB);
                    result.set_error_msg("Write to read-only DB.");
                    on_finish(result);
                }
                return;
            }

            // Increase write counter at the start of the operation
            IncreaseWriteCounter();

            std::vector<uint32_t> ng_ids;
            std::vector<std::string> ng_members;
            std::vector<std::string> ng_members_is_candidate;
            std::vector<uint32_t> node_ids;
            std::vector<std::string> ips;
            std::vector<uint16_t> ports;

            std::vector<::EloqDS::remote::ClusterNodeConfig> nodes;
            EloqShare::ExtractNodesConfigs(new_cnf, nodes);
            for (const ::EloqDS::remote::ClusterNodeConfig &node : nodes)
            {
                node_ids.push_back(node.node_id());
                ips.push_back(node.host_name());
                ports.push_back(node.port());
            }

            for (auto ng_pair : new_cnf)
            {
                ng_ids.push_back(ng_pair.first);
                std::vector<uint32_t> group_members;
                std::vector<bool> group_members_is_candidate;
                for (auto &node : ng_pair.second)
                {
                    group_members.push_back(node.node_id());
                    group_members_is_candidate.push_back(node.is_candidate());
                }
                ng_members.push_back(
                    EloqShare::SerializeVectorToString<uint32_t>(
                        group_members, ",", EloqShare::uint32_str_converter));
                ng_members_is_candidate.push_back(
                    EloqShare::SerializeVectorToString<bool>(
                        group_members_is_candidate,
                        ",",
                        EloqShare::bool_str_converter));
            }

            std::string node_ids_str =
                EloqShare::SerializeVectorToString<uint32_t>(
                    node_ids, " ", EloqShare::uint32_str_converter);
            std::string ips_str =
                EloqShare::SerializeVectorToString<std::string>(
                    ips, " ", EloqShare::string_str_converter);
            std::string ports_str =
                EloqShare::SerializeVectorToString<uint16_t>(
                    ports, " ", EloqShare::uint16_str_converter);
            std::string ng_ids_str =
                EloqShare::SerializeVectorToString<uint32_t>(
                    ng_ids, " ", EloqShare::uint32_str_converter);
            std::string ng_members_str =
                EloqShare::SerializeVectorToString<std::string>(
                    ng_members, " ", EloqShare::string_str_converter);
            std::string ng_members_is_candidate_str =
                EloqShare::SerializeVectorToString<std::string>(
                    ng_members_is_candidate,
                    " ",
                    EloqShare::string_str_converter);
            std::string verstion_str = std::to_string(version);

            // Create a rocksdb::WideColumns object
            rocksdb::WideColumns cluster_config_wc;
            cluster_config_wc.emplace_back("node_ids", node_ids_str);
            cluster_config_wc.emplace_back("ips", ips_str);
            cluster_config_wc.emplace_back("ports", ports_str);
            cluster_config_wc.emplace_back("ng_ids", ng_ids_str);
            cluster_config_wc.emplace_back("ng_members", ng_members_str);
            cluster_config_wc.emplace_back("ng_members_is_candidate",
                                           ng_members_is_candidate_str);
            cluster_config_wc.emplace_back("version", verstion_str);

            // Write the cluster config to the rocksdb
            rocksdb::WriteOptions write_options;
            write_options.disableWAL = true;
            auto db = GetDBPtr();
            if (db == nullptr)
            {
                LOG(ERROR)
                    << "Unable to update cluster config, db is not opened";
                DecreaseWriteCounter();  // Decrease counter before error return
                ::EloqDS::remote::CommonResult result;
                result.set_error_code(
                    ::EloqDS::remote::DataStoreError::DB_NOT_OPEN);
                result.set_error_msg("DB is not opened");
                on_finish(result);
                return;
            }

            auto status = db->PutEntity(
                write_options,
                GetColumnFamilyHandler(rocksdb::kDefaultColumnFamilyName),
                "cluster_config",
                cluster_config_wc);

            if (!status.ok())
            {
                LOG(ERROR) << "Unable to write to db with error: "
                           << status.ToString();
                DecreaseWriteCounter();  // Decrease counter before error return
                ::EloqDS::remote::CommonResult result;
                result.set_error_code(
                    ::EloqDS::remote::DataStoreError::WRITE_FAILED);
                result.set_error_msg(status.ToString());
                on_finish(result);
                return;
            }

            rocksdb::FlushOptions flush_options;
            flush_options.allow_write_stall = false;
            flush_options.wait = true;
            status = db->Flush(flush_options);
            if (!status.ok())
            {
                LOG(ERROR) << "Unable to write to db with error: "
                           << status.ToString();
                DecreaseWriteCounter();  // Decrease counter before error return
                ::EloqDS::remote::CommonResult result;
                result.set_error_code(
                    ::EloqDS::remote::DataStoreError::WRITE_FAILED);
                result.set_error_msg(status.ToString());
                on_finish(result);
                return;
            }
            DecreaseWriteCounter();  // Decrease counter before successful
                                     // return
            ::EloqDS::remote::CommonResult result;
            result.set_error_code(::EloqDS::remote::DataStoreError::NO_ERROR);
            on_finish(result);
        });
}

void RocksDBCloudDataStore::ReadClusterConfig(
    uint32_t req_shard_id,
    std::function<void(
        std::unordered_map<uint32_t,
                           std::vector<::EloqDS::remote::ClusterNodeConfig>>
            &&ng_configs,
        uint64_t version,
        bool uninitialized,
        const ::EloqDS::remote::CommonResult &result)> on_finish)
{
    query_worker_pool_->SubmitWork(
        [this, req_shard_id, on_finish]()
        {
            DLOG(INFO) << "RocksDBCloudDataStore::ReadClusterConfig started";
            std::shared_lock<std::shared_mutex> db_lk(db_mux_);

            auto shard_status = FetchDSShardStatus();
            ::EloqDS::remote::CommonResult result;
            if (shard_status != DSShardStatus::ReadOnly &&
                shard_status != DSShardStatus::ReadWrite)
            {
                data_store_service_->PrepareShardingError(
                    req_shard_id, shard_id_, &result);
                on_finish(
                    std::unordered_map<
                        uint32_t,
                        std::vector<::EloqDS::remote::ClusterNodeConfig>>(),
                    0,
                    true,
                    result);
                return;
            }

            rocksdb::ReadOptions read_options;
            auto db = GetDBPtr();
            if (db == nullptr)
            {
                ::EloqDS::remote::CommonResult result;
                result.set_error_code(
                    ::EloqDS::remote::DataStoreError::DB_NOT_OPEN);
                result.set_error_msg("DB is not opened");
                on_finish(
                    std::unordered_map<
                        uint32_t,
                        std::vector<::EloqDS::remote::ClusterNodeConfig>>(),
                    0,
                    true,
                    result);
                return;
            }

            rocksdb::PinnableWideColumns pinnable_cluster_configs;
            auto status = db->GetEntity(
                read_options,
                GetColumnFamilyHandler(rocksdb::kDefaultColumnFamilyName),
                "cluster_config",
                &pinnable_cluster_configs);
            if (!status.ok())
            {
                ::EloqDS::remote::CommonResult result;
                result.set_error_code(
                    ::EloqDS::remote::DataStoreError::READ_FAILED);
                result.set_error_msg(status.ToString());
                on_finish(
                    std::unordered_map<
                        uint32_t,
                        std::vector<::EloqDS::remote::ClusterNodeConfig>>(),
                    0,
                    true,
                    result);
                return;
            }

            const rocksdb::WideColumns &cluster_configs =
                pinnable_cluster_configs.columns();

            if (cluster_configs.empty())
            {
                ::EloqDS::remote::CommonResult result;
                result.set_error_code(
                    ::EloqDS::remote::DataStoreError::NO_ERROR);

                on_finish(
                    std::unordered_map<
                        uint32_t,
                        std::vector<::EloqDS::remote::ClusterNodeConfig>>(),
                    0,
                    true,
                    result);
                return;
            }

            std::vector<uint32_t> node_ids;
            std::vector<std::string> ips;
            std::vector<uint16_t> ports;
            std::vector<uint32_t> ng_ids;
            std::vector<std::string> ng_members;
            std::vector<std::string> ng_members_is_candidate;
            uint64_t version = 0;
            std::unordered_map<uint32_t,
                               std::vector<::EloqDS::remote::ClusterNodeConfig>>
                ng_configs;
            for (const auto &config : cluster_configs)
            {
                if (config.name() == "node_ids")
                {
                    std::string node_ids_str;
                    node_ids_str = config.value().ToString();
                    EloqShare::DeserializeStringToVector<uint32_t>(
                        node_ids_str,
                        " ",
                        node_ids,
                        EloqShare::str_uint32_converter);
                }
                else if (config.name() == "ips")
                {
                    std::string ips_str;
                    ips_str = config.value().ToString();
                    EloqShare::DeserializeStringToVector<std::string>(
                        ips_str, " ", ips, EloqShare::str_string_converter);
                }
                else if (config.name() == "ports")
                {
                    std::string ports_str;
                    ports_str = config.value().ToString();
                    EloqShare::DeserializeStringToVector<uint16_t>(
                        ports_str, " ", ports, EloqShare::str_uint16_converter);
                }
                else if (config.name() == "ng_ids")
                {
                    std::string ng_ids_str;
                    ng_ids_str = config.value().ToString();
                    EloqShare::DeserializeStringToVector<uint32_t>(
                        ng_ids_str,
                        " ",
                        ng_ids,
                        EloqShare::str_uint32_converter);
                }
                else if (config.name() == "ng_members")
                {
                    std::string ng_members_str;
                    ng_members_str = config.value().ToString();
                    EloqShare::DeserializeStringToVector<std::string>(
                        ng_members_str,
                        " ",
                        ng_members,
                        EloqShare::str_string_converter);
                }
                else if (config.name() == "ng_members_is_candidate")
                {
                    std::string ng_members_candidate_str;
                    ng_members_candidate_str = config.value().ToString();
                    EloqShare::DeserializeStringToVector<std::string>(
                        ng_members_candidate_str,
                        " ",
                        ng_members_is_candidate,
                        EloqShare::str_string_converter);
                }
                else if (config.name() == "version")
                {
                    std::string version_str;
                    version_str = config.value().ToString();
                    version = std::stoul(version_str);
                }
            }

            std::map<uint32_t, std::pair<std::string, uint16_t>> node_ip_map;
            assert(ips.size() == node_ids.size());
            for (size_t node_idx = 0; node_idx < node_ids.size(); ++node_idx)
            {
                node_ip_map.try_emplace(
                    node_ids[node_idx],
                    std::make_pair(ips[node_idx], ports[node_idx]));
            }

            for (size_t group_idx = 0; group_idx < ng_ids.size(); ++group_idx)
            {
                std::vector<::EloqDS::remote::ClusterNodeConfig>
                    group_members_config;
                std::vector<uint32_t> group_members;
                EloqShare::DeserializeStringToVector<uint32_t>(
                    ng_members[group_idx],
                    ",",
                    group_members,
                    EloqShare::str_uint32_converter);

                std::vector<bool> group_members_is_candidate;
                EloqShare::DeserializeStringToVector<bool>(
                    ng_members_is_candidate[group_idx],
                    ",",
                    group_members_is_candidate,
                    EloqShare::str_bool_converter);
                assert(group_members.size() ==
                       group_members_is_candidate.size());
                for (size_t idx = 0; idx < group_members.size(); idx++)
                {
                    uint32_t nid = group_members[idx];
                    ::EloqDS::remote::ClusterNodeConfig node_config;
                    node_config.set_node_id(nid);
                    node_config.set_host_name(std::get<0>(node_ip_map.at(nid)));
                    node_config.set_port(std::get<1>(node_ip_map.at(nid)));
                    node_config.set_is_candidate(
                        group_members_is_candidate[idx]);
                    group_members_config.emplace_back(std::move(node_config));
                    DLOG(INFO) << "RocksDBCloudDataStore::"
                                  "ReadClusterConfig ng_ids["
                               << group_idx << "]: " << ng_ids[group_idx]
                               << " group_members_config.nid_: "
                               << group_members_config[idx].node_id()
                               << " group_members_config.host_name_: "
                               << group_members_config[idx].host_name()
                               << " group_members_config.port_: "
                               << group_members_config[idx].port()
                               << " group_members_config.is_candidate_: "
                               << group_members_config[idx].is_candidate()
                               << " version: " << version;
                }
                ng_configs.try_emplace(ng_ids[group_idx],
                                       std::move(group_members_config));
            }

            result.set_error_code(::EloqDS::remote::DataStoreError::NO_ERROR);
            on_finish(std::move(ng_configs), version, false, result);

            DLOG(INFO) << "RocksDBCloudDataStore::ReadClusterConfig finished";
        });
}

void RocksDBCloudDataStore::CkptEnd(
    uint32_t req_shard_id,
    const std::string &table_name,
    std::function<void(const ::EloqDS::remote::CommonResult &result)> on_finish)
{
    query_worker_pool_->SubmitWork(
        [this, req_shard_id, table_name, on_finish]()
        {
            auto shard_status = FetchDSShardStatus();
            if (shard_status != DSShardStatus::ReadWrite)
            {
                ::EloqDS::remote::CommonResult result;
                if (shard_status == DSShardStatus::Closed)
                {
                    data_store_service_->PrepareShardingError(
                        req_shard_id, shard_id_, &result);
                }
                else
                {
                    assert(shard_status == DSShardStatus::ReadOnly);
                    result.set_error_code(::EloqDS::remote::DataStoreError::
                                              WRITE_TO_READ_ONLY_DB);
                    result.set_error_msg("Write to read-only DB.");
                    on_finish(result);
                }
                return;
            }

            // Increase write counter at the start of the operation
            IncreaseWriteCounter();

            std::shared_lock<std::shared_mutex> db_lk(db_mux_);
            auto db = GetDBPtr();
            if (!db)
            {
                DecreaseWriteCounter();  // Decrease counter before error return
                ::EloqDS::remote::CommonResult result;
                result.set_error_code(
                    ::EloqDS::remote::DataStoreError::DB_NOT_OPEN);
                result.set_error_msg("DB is not opened");
                on_finish(result);
                return;
            }
            rocksdb::ColumnFamilyHandle *cfh =
                GetColumnFamilyHandler(table_name);
            assert(cfh != nullptr);
            rocksdb::FlushOptions flush_options;
            flush_options.allow_write_stall = true;
            flush_options.wait = true;
            ::EloqDS::remote::CommonResult result;
            auto status = GetDBPtr()->Flush(flush_options, cfh);
            if (!status.ok())
            {
                LOG(ERROR) << "Unable to flush db with error: "
                           << status.ToString();
                DecreaseWriteCounter();  // Decrease counter before error return
                result.set_error_code(
                    ::EloqDS::remote::DataStoreError::WRITE_FAILED);
                result.set_error_msg(status.ToString());
                on_finish(result);
                return;
            }
            DecreaseWriteCounter();  // Decrease counter before successful
                                     // return
            result.set_error_code(::EloqDS::remote::DataStoreError::NO_ERROR);
            on_finish(result);
        });
}

rocksdb::ColumnFamilyHandle *RocksDBCloudDataStore::GetColumnFamilyHandler(
    const std::string &cf)
{
    auto cfh = column_families_.find(cf);
    if (cfh != column_families_.cend())
    {
        std::unique_ptr<rocksdb::ColumnFamilyHandle> &cfh_ptr = cfh->second;
        return cfh_ptr.get();
    }
    return nullptr;
}

void RocksDBCloudDataStore::ResetColumnFamilyHandler(
    const std::string &old_cf,
    const std::string &new_cf,
    rocksdb::ColumnFamilyHandle *cfh)
{
    auto cfh_entry = column_families_.find(old_cf);
    assert(cfh_entry != column_families_.cend());
    std::unique_ptr<rocksdb::ColumnFamilyHandle> &cfh_ptr = cfh_entry->second;
    rocksdb::ColumnFamilyHandle *old_cfh = cfh_ptr.release();
    GetDBPtr()->DestroyColumnFamilyHandle(old_cfh);
    column_families_.erase(cfh_entry);
    assert(new_cf == cfh->GetName());
    auto pair = column_families_.emplace(new_cf, cfh);
    assert(pair.second);
}

void RocksDBCloudDataStore::BatchFetchRecords(
    uint32_t req_shard_id,
    std::vector<::EloqDS::remote::FetchRecordsRequest::key> &&keys,
    std::function<void(
        std::vector<::EloqDS::remote::FetchRecordsResponse::record> &&records,
        const ::EloqDS::remote::CommonResult &result)> on_finish)
{
    if (keys.empty())
    {
        ::EloqDS::remote::CommonResult result;
        result.set_error_code(::EloqDS::remote::DataStoreError::NO_ERROR);
        on_finish(std::vector<::EloqDS::remote::FetchRecordsResponse::record>(),
                  result);
        return;
    }

    query_worker_pool_->SubmitWork(
        [this, req_shard_id, req_keys = std::move(keys), on_finish]()
        {
            std::shared_lock<std::shared_mutex> db_lk(db_mux_);
            ::EloqDS::remote::CommonResult result;

            // validate shard status
            auto shard_status = FetchDSShardStatus();
            if (shard_status != DSShardStatus::ReadOnly &&
                shard_status != DSShardStatus::ReadWrite)
            {
                data_store_service_->PrepareShardingError(
                    req_shard_id, shard_id_, &result);
                on_finish(std::vector<
                              ::EloqDS::remote::FetchRecordsResponse::record>(),
                          result);
                return;
            }

            auto *db = GetDBPtr();
            if (db == nullptr)
            {
                DLOG(ERROR) << "GetDBPtr() return nullptr";
                result.set_error_code(
                    ::EloqDS::remote::DataStoreError::DB_NOT_OPEN);
                result.set_error_msg("DB is not opened");
                on_finish(std::vector<
                              ::EloqDS::remote::FetchRecordsResponse::record>(),
                          result);
                return;
            }

            // Special case: Single request optimization using db->Get()
            if (req_keys.size() == 1)
            {
                std::shared_lock<std::shared_mutex> db_lk(db_mux_);
                auto &req = req_keys[0];  // Get the single request

                // Get ColumnFamilyHandle
                rocksdb::ColumnFamilyHandle *cfh =
                    GetColumnFamilyHandler(req.kv_table_name_str());
                if (!cfh)
                {
                    DLOG(ERROR) << "Column Family " << req.kv_table_name_str()
                                << " not found.";
                    result.set_error_code(
                        ::EloqDS::remote::DataStoreError::TABLE_NOT_FOUND);
                    result.set_error_msg("Column Family " +
                                         req.kv_table_name_str() +
                                         " not found.");
                    on_finish(
                        std::vector<
                            ::EloqDS::remote::FetchRecordsResponse::record>(),
                        result);
                    return;
                }

                // Use db->Get() to fetch the record
                std::string value;
                rocksdb::ReadOptions read_opt;

                rocksdb::Status status =
                    db->Get(read_opt, cfh, req.key_str(), &value);

                ::EloqDS::remote::FetchRecordsResponse::record rec;

                if (status.ok())
                {
                    // Key found, deserialize the record
                    const char *payload = value.data();
                    const size_t payload_size = value.size();
                    bool is_deleted = false;
                    int64_t version_ts = 0;
                    std::string rec_str;

                    EloqShare::DeserializeRecord(
                        payload, payload_size, rec_str, is_deleted, version_ts);

                    if (is_deleted)
                    {
                        rec.set_is_deleted(true);
                    }
                    else
                    {
                        rec.set_payload(std::move(rec_str));
                        rec.set_is_deleted(false);
                    }
                    rec.set_version(version_ts);
                }
                else if (status.IsNotFound())
                {
                    // Key not found, treat as deleted
                    rec.set_is_deleted(true);
                    rec.set_version(1);
                }
                else
                {
                    LOG(ERROR) << "Error fetching key: " << req.key_str()
                               << ", Status: " << status.ToString();
                    result.set_error_code(
                        ::EloqDS::remote::DataStoreError::READ_FAILED);
                    result.set_error_msg(status.ToString());
                    on_finish(
                        std::vector<
                            ::EloqDS::remote::FetchRecordsResponse::record>(),
                        result);
                    return;
                }

                std::vector<::EloqDS::remote::FetchRecordsResponse::record>
                    records;
                records.push_back(rec);
                result.set_error_code(
                    ::EloqDS::remote::DataStoreError::NO_ERROR);
                on_finish(std::move(records), result);

                return;
            }

            // Prepare parameter vectors for MultiGet
            std::vector<rocksdb::ColumnFamilyHandle *> column_family_handlers;
            std::vector<rocksdb::Slice> keys;
            for (auto &key : req_keys)
            {
                rocksdb::ColumnFamilyHandle *cfh =
                    GetColumnFamilyHandler(key.kv_table_name_str());
                if (!cfh)
                {
                    DLOG(ERROR) << "Cloumn Family " << key.kv_table_name_str()
                                << " not found.";
                    result.set_error_code(
                        ::EloqDS::remote::DataStoreError::TABLE_NOT_FOUND);
                    result.set_error_msg("Cloumn Family " +
                                         key.kv_table_name_str() +
                                         " not found.");
                    on_finish(
                        std::vector<
                            ::EloqDS::remote::FetchRecordsResponse::record>(),
                        result);
                    return;
                }

                column_family_handlers.push_back(cfh);
                keys.emplace_back(std::move(key.key_str()));
            }

            // Fetch records from KV using MultiGet
            std::vector<std::string> values(req_keys.size());
            std::vector<std::string> timestamps(req_keys.size());
            rocksdb::ReadOptions read_opt;
            read_opt.async_io = true;
            read_opt.optimize_multiget_for_io = true;
            std::vector<rocksdb::Status> statuses = db->MultiGet(
                read_opt, column_family_handlers, keys, &values, &timestamps);

            // Process results, move into fetch_rec_reqs
            std::vector<::EloqDS::remote::FetchRecordsResponse::record> records(
                req_keys.size());
            for (size_t idx = 0; idx < req_keys.size(); ++idx)
            {
                auto &rec = records[idx];
                if (statuses[idx].ok())
                {
                    // Key found, deserialize the record
                    const char *payload = values[idx].data();
                    const size_t payload_size = values[idx].size();
                    bool is_deleted = false;
                    int64_t version_ts = 0;
                    std::string rec_str;

                    EloqShare::DeserializeRecord(
                        payload, payload_size, rec_str, is_deleted, version_ts);

                    if (is_deleted)
                    {
                        rec.set_is_deleted(true);
                    }
                    else
                    {
                        rec.set_payload(std::move(rec_str));
                    }
                    rec.set_version(version_ts);
                }
                else if (statuses[idx].IsNotFound())
                {
                    // Key not found, treat as deleted
                    rec.set_is_deleted(true);
                    rec.set_version(1);
                }
                else
                {
                    DLOG(ERROR)
                        << "Error fetching key: " << req_keys[idx].key_str()
                        << ", Status: " << statuses[idx].ToString();
                    result.set_error_code(
                        ::EloqDS::remote::DataStoreError::READ_FAILED);
                    result.set_error_msg(statuses[idx].ToString());
                    on_finish(
                        std::vector<
                            ::EloqDS::remote::FetchRecordsResponse::record>(),
                        result);
                    return;
                }
            }

            result.set_error_code(::EloqDS::remote::DataStoreError::NO_ERROR);
            on_finish(std::move(records), result);
        });
}

void RocksDBCloudDataStore::BatchWriteRecords(
    uint32_t req_shard_id,
    std::vector<EloqDS::remote::BatchWriteRecordsRequest::record> &&batch,
    const std::string &kv_table_name,
    std::function<void(const ::EloqDS::remote::CommonResult &result)> on_finish)
{
    if (batch.empty())
    {
        ::EloqDS::remote::CommonResult result;
        result.set_error_code(::EloqDS::remote::DataStoreError::NO_ERROR);
        on_finish(result);
        return;
    }

    query_worker_pool_->SubmitWork(
        [this,
         req_shard_id,
         batch = std::move(batch),
         kv_table_name,
         on_finish]() mutable
        {
            auto shard_status = FetchDSShardStatus();
            ::EloqDS::remote::CommonResult result;
            if (shard_status != DSShardStatus::ReadWrite)
            {
                if (shard_status == DSShardStatus::Closed)
                {
                    data_store_service_->PrepareShardingError(
                        req_shard_id, shard_id_, &result);
                    on_finish(result);
                }
                else
                {
                    assert(shard_status == DSShardStatus::ReadOnly);
                    result.set_error_code(::EloqDS::remote::DataStoreError::
                                              WRITE_TO_READ_ONLY_DB);
                    result.set_error_msg("Write to read-only DB.");
                    on_finish(result);
                }
                return;
            }

            std::shared_lock<std::shared_mutex> db_lk(db_mux_);
            auto db = GetDBPtr();
            if (!db)
            {
                DLOG(ERROR) << "DB is not opened";
                result.set_error_code(
                    ::EloqDS::remote::DataStoreError::DB_NOT_OPEN);
                result.set_error_msg("DB is not opened");
                on_finish(result);
                return;
            }

            // Increase write counter before starting the write operation
            IncreaseWriteCounter();

            rocksdb::WriteOptions write_options;
            write_options.disableWAL = true;
            rocksdb::WriteBatch write_batch;
            rocksdb::ColumnFamilyHandle *cfh =
                GetColumnFamilyHandler(kv_table_name);
            assert(cfh != nullptr);
            for (auto &rec : batch)
            {
                if (rec.is_deleted())
                {
                    write_batch.Delete(cfh, rec.key());
                }
                else
                {
                    write_batch.Put(cfh, rec.key(), rec.value());
                }
            }

            auto write_status = db->Write(write_options, &write_batch);

            // Decrease write counter after the write operation is complete
            DecreaseWriteCounter();

            if (!write_status.ok())
            {
                LOG(ERROR) << "PutAll end failed, table:" << kv_table_name
                           << ", result:" << static_cast<int>(write_status.ok())
                           << ", error: " << write_status.ToString()
                           << ", error code: " << write_status.code();
                result.set_error_code(
                    ::EloqDS::remote::DataStoreError::WRITE_FAILED);
                result.set_error_msg(write_status.ToString());
                on_finish(result);
                return;
            }

            result.set_error_code(::EloqDS::remote::DataStoreError::NO_ERROR);
            on_finish(result);
        });
}

void RocksDBCloudDataStore::FetchTableCatalog(
    uint32_t req_shard_id,
    const std::string &table_name,
    std::function<void(std::string &&schema_image,
                       bool found,
                       uint64_t version_ts,
                       const ::EloqDS::remote::CommonResult &result)> on_finish)
{
    query_worker_pool_->SubmitWork(
        [this, table_name, on_finish]()
        {
            bool found = false;
            uint64_t version_ts = 0;
            std::string schema_image;
            std::shared_lock<std::shared_mutex> db_lk(db_mux_);
            ::EloqDS::remote::CommonResult result;
            auto db = GetDBPtr();
            if (!db)
            {
                LOG(ERROR) << "Fetch table schema failed, DB is not opened";
                result.set_error_code(
                    ::EloqDS::remote::DataStoreError::DB_NOT_OPEN);
                result.set_error_msg("DB is not opened");
                on_finish(std::move(schema_image), found, version_ts, result);
                return;
            }

            std::string table_key = table_name + "_catalog";
            rocksdb::ColumnFamilyHandle *cfh =
                GetColumnFamilyHandler(rocksdb::kDefaultColumnFamilyName);
            rocksdb::PinnableWideColumns pinnable_table_catalog;
            auto status = GetDBPtr()->GetEntity(rocksdb::ReadOptions(),
                                                cfh,
                                                table_key,
                                                &pinnable_table_catalog);
            if (!status.ok())
            {
                if (status.IsNotFound())
                {
                    found = false;
                    result.set_error_code(
                        ::EloqDS::remote::DataStoreError::NO_ERROR);
                    on_finish(
                        std::move(schema_image), found, version_ts, result);
                    return;
                }
                LOG(ERROR) << "Fetch table schema failed, table: " << table_name
                           << ", error: " << status.ToString();
                result.set_error_code(
                    ::EloqDS::remote::DataStoreError::READ_FAILED);
                result.set_error_msg(status.ToString());
                on_finish(std::move(schema_image), found, version_ts, result);
                return;
            }

            found = true;
            std::string kv_cf_name;
            const rocksdb::WideColumns &table_catalog_wc =
                pinnable_table_catalog.columns();
            for (auto &column : table_catalog_wc)
            {
                if (column.name() == "version")
                {
                    const rocksdb::Slice &val = column.value();
                    assert(val.size() == sizeof(uint64_t));
                    version_ts =
                        *reinterpret_cast<const uint64_t *>(val.data());
                }
                if (column.name() == "kv_cf_name")
                {
                    const rocksdb::Slice &val = column.value();
                    kv_cf_name = val.ToString();
                }
            }

            // catalog image stores only kv_table_name
            assert(!kv_cf_name.empty());
            schema_image.append(kv_cf_name);
            result.set_error_code(::EloqDS::remote::DataStoreError::NO_ERROR);
            on_finish(std::move(schema_image), found, version_ts, result);
        });
}

void RocksDBCloudDataStore::UpsertTable(
    uint32_t req_shard_id,
    const std::string table_name_str,
    const std::string old_schema_img,
    const std::string new_schema_img,
    ::EloqDS::remote::UpsertTableOperationType op_type,
    uint64_t commit_ts,
    std::function<void(const ::EloqDS::remote::CommonResult &result)> on_finish)
{
    assert(op_type ==
           ::EloqDS::remote::UpsertTableOperationType::TruncateTable);

    query_worker_pool_->SubmitWork(
        [this,
         table_name_str,
         old_schema_img,
         new_schema_img,
         commit_ts,
         on_finish]()
        {
            auto shard_status = FetchDSShardStatus();
            ::EloqDS::remote::CommonResult result;
            if (shard_status != DSShardStatus::ReadWrite)
            {
                if (shard_status == DSShardStatus::Closed)
                {
                    data_store_service_->PrepareShardingError(
                        shard_id_, shard_id_, &result);
                    on_finish(result);
                }
                else
                {
                    assert(shard_status == DSShardStatus::ReadOnly);
                    result.set_error_code(::EloqDS::remote::DataStoreError::
                                              WRITE_TO_READ_ONLY_DB);
                    result.set_error_msg("Write to read-only DB.");
                    on_finish(result);
                }
                return;
            }

            // Increase write counter at the start of the operation
            IncreaseWriteCounter();

            std::string error_msg;
            std::stringstream msg_ss;

            std::shared_lock<std::shared_mutex> db_lk(db_mux_);
            auto db = GetDBPtr();
            if (!db)
            {
                error_msg = "Upsert table failed, DB is not opened";
                LOG(ERROR) << error_msg;
                DecreaseWriteCounter();  // Decrease counter before early return
                result.set_error_code(
                    ::EloqDS::remote::DataStoreError::DB_NOT_OPEN);
                result.set_error_msg(error_msg);
                on_finish(result);
                return;
            }

            std::unique_lock<std::mutex> ddl_lk(ddl_mux_);
            // check catalog version
            std::string catalog_cf_name = table_name_str + "_catalog";
            rocksdb::ColumnFamilyHandle *catalog_cfh =
                GetColumnFamilyHandler(rocksdb::kDefaultColumnFamilyName);
            rocksdb::PinnableWideColumns pinnable_table_catalog;
            auto status1 = db->GetEntity(rocksdb::ReadOptions(),
                                         catalog_cfh,
                                         catalog_cf_name,
                                         &pinnable_table_catalog);
            if (!status1.ok())
            {
                assert(false);

                msg_ss << "Not found table catalog in data store: "
                       << table_name_str;
                error_msg = msg_ss.str();
                LOG(ERROR) << error_msg;
                DecreaseWriteCounter();  // Decrease counter before early return
                result.set_error_code(
                    ::EloqDS::remote::DataStoreError::TABLE_NOT_FOUND);
                result.set_error_msg(error_msg);
                on_finish(result);
                return;
            }

            uint64_t store_schema_version = 0;
            const rocksdb::WideColumns &table_catalog_wc1 =
                pinnable_table_catalog.columns();
            for (auto &column : table_catalog_wc1)
            {
                if (column.name() == "version")
                {
                    const rocksdb::Slice &val = column.value();
                    assert(val.size() == sizeof(uint64_t));
                    store_schema_version =
                        *reinterpret_cast<const uint64_t *>(val.data());
                }
            }

            if (store_schema_version >= commit_ts)
            {
                DecreaseWriteCounter();  // Decrease counter before early return
                result.set_error_code(
                    ::EloqDS::remote::DataStoreError::NO_ERROR);
                on_finish(result);
                return;
            }

            // Create new column family and drop old column family
            rocksdb::ColumnFamilyHandle *new_cfh = nullptr;
            auto status = db->CreateColumnFamily(
                rocksdb::ColumnFamilyOptions(), new_schema_img, &new_cfh);
            if (!status.ok())
            {
                msg_ss << "Unable to create column family with error: "
                       << status.ToString() << ", cfname: " << new_schema_img;
                error_msg = msg_ss.str();
                LOG(ERROR) << error_msg;
                DecreaseWriteCounter();  // Decrease counter before early return
                result.set_error_code(
                    ::EloqDS::remote::DataStoreError::UPSERT_TABLE_FAILED);
                result.set_error_msg(error_msg);
                on_finish(result);
                return;
            }
            rocksdb::ColumnFamilyHandle *old_cfh =
                GetColumnFamilyHandler(old_schema_img);
            status = db->DropColumnFamily(old_cfh);
            if (!status.ok())
            {
                if (status.IsInvalidArgument())
                {
                    msg_ss << "Unable to drop column family with error: "
                           << status.ToString()
                           << ", cfname: " << old_schema_img
                           << " , it may has been dropped, ignore it";
                    error_msg = msg_ss.str();
                    LOG(ERROR) << error_msg;
                }
                else
                {
                    msg_ss << "Unable to drop column family with error: "
                           << status.ToString()
                           << ", cfname: " << old_schema_img;
                    error_msg = msg_ss.str();
                    LOG(ERROR) << error_msg;
                }

                DecreaseWriteCounter();  // Decrease counter before early return
                result.set_error_code(
                    ::EloqDS::remote::DataStoreError::UPSERT_TABLE_FAILED);
                result.set_error_msg(error_msg);
                on_finish(result);
                return;
            }
            ResetColumnFamilyHandler(old_schema_img, new_schema_img, new_cfh);

            // Update the table catalog
            std::string table_key = table_name_str + "_catalog";
            rocksdb::WideColumns table_catalog_wc;
            table_catalog_wc.emplace_back("kv_cf_name", new_schema_img);
            rocksdb::Slice commit_ts_value(
                reinterpret_cast<const char *>(&commit_ts), sizeof(uint64_t));
            table_catalog_wc.emplace_back("version", commit_ts_value);
            rocksdb::ColumnFamilyHandle *cfh_default =
                GetColumnFamilyHandler(rocksdb::kDefaultColumnFamilyName);
            DLOG(INFO) << "Update table catalog: " << table_key
                       << ", from old cf: " << old_schema_img
                       << " to new cf: " << new_schema_img
                       << ", version: " << commit_ts;

            rocksdb::WriteOptions w_opt;
            w_opt.disableWAL = true;
            status =
                db->PutEntity(w_opt, cfh_default, table_key, table_catalog_wc);
            if (!status.ok())
            {
                msg_ss << "Unable to write catalog info to db with error: "
                       << status.ToString() << ", table_name: " << table_key;
                error_msg = msg_ss.str();
                LOG(ERROR) << error_msg;
                DecreaseWriteCounter();  // Decrease counter before early return
                result.set_error_code(
                    ::EloqDS::remote::DataStoreError::UPSERT_TABLE_FAILED);
                result.set_error_msg(error_msg);
                on_finish(result);
                return;
            }
            // unlock ddl mutex
            ddl_lk.unlock();

            rocksdb::FlushOptions flush_options;
            flush_options.allow_write_stall = false;
            flush_options.wait = true;
            status = db->Flush(flush_options);
            if (!status.ok())
            {
                msg_ss << "Unable to flush db with error: "
                       << status.ToString();
                error_msg = msg_ss.str();
                LOG(ERROR) << error_msg;
                DecreaseWriteCounter();  // Decrease counter before early return
                result.set_error_code(
                    ::EloqDS::remote::DataStoreError::UPSERT_TABLE_FAILED);
                result.set_error_msg(error_msg);
                on_finish(result);
                return;
            }

            DecreaseWriteCounter();  // Decrease counter before successful
                                     // return
            result.set_error_code(::EloqDS::remote::DataStoreError::NO_ERROR);
            on_finish(result);
        });
}

void RocksDBCloudDataStore::WaitForPendingWrites()
{
    // Wait until all ongoing write requests are completed
    while (ongoing_write_requests_.load(std::memory_order_acquire) > 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

DSShardStatus RocksDBCloudDataStore::FetchDSShardStatus() const
{
    assert(data_store_service_ != nullptr);
    return data_store_service_->FetchDSShardStatus(shard_id_);
}

void RocksDBCloudDataStore::IncreaseWriteCounter()
{
    ongoing_write_requests_.fetch_add(1, std::memory_order_release);
}

void RocksDBCloudDataStore::DecreaseWriteCounter()
{
    ongoing_write_requests_.fetch_sub(1, std::memory_order_release);
}

void RocksDBCloudDataStore::ScanOpen(
    const ::EloqDS::remote::ScanRequest &scan_req,
    std::function<void(const ::EloqDS::remote::ScanResponse &response)>
        on_finish)
{
    query_worker_pool_->SubmitWork(
        [this,
         scan_req = std::move(scan_req),
         on_finish = std::move(on_finish)]()
        {
            uint32_t req_shard_id = scan_req.req_shard_id();
            const std::string &kv_table_name = scan_req.kv_table_name_str();
            bool start_key_is_neg_inf = scan_req.start_key_is_neg_inf();
            bool start_key_is_pos_inf = scan_req.start_key_is_pos_inf();
            const std::string &start_key = scan_req.start_key();
            bool scan_forward = scan_req.scan_forward();
            bool inclusive = scan_req.inclusive();

            std::shared_lock<std::shared_mutex> db_lk(db_mux_);

            auto shard_status = FetchDSShardStatus();
            ::EloqDS::remote::ScanResponse resp;
            ::EloqDS::remote::CommonResult *result = resp.mutable_result();
            if (shard_status != DSShardStatus::ReadOnly &&
                shard_status != DSShardStatus::ReadWrite)
            {
                data_store_service_->PrepareShardingError(
                    req_shard_id, shard_id_, result);
                on_finish(resp);
                return;
            }

            // Get ColumnFamilyHandle
            rocksdb::ColumnFamilyHandle *cfh =
                GetColumnFamilyHandler(kv_table_name);
            if (!cfh)
            {
                DLOG(ERROR)
                    << "Column Family " << kv_table_name << " not found.";
                ::EloqDS::remote::ScanResponse response;
                ::EloqDS::remote::CommonResult *result =
                    response.mutable_result();
                result->set_error_code(
                    ::EloqDS::remote::DataStoreError::TABLE_NOT_FOUND);
                result->set_error_msg("Column Family " + kv_table_name +
                                      " not found.");
                on_finish(response);
                return;
            }

            // Create a new iter
            rocksdb::ReadOptions read_options;
            read_options.async_io = true;
            auto *iter = GetDBPtr()->NewIterator(read_options, cfh);

            if (!start_key.empty() && !start_key_is_neg_inf &&
                !start_key_is_pos_inf)
            {
                rocksdb::Slice key(start_key);
                if (scan_forward)
                {
                    iter->Seek(key);
                    if (!inclusive && iter->Valid())
                    {
                        rocksdb::Slice curr_key = iter->key();
                        if (curr_key.ToStringView() == key.ToStringView())
                        {
                            iter->Next();
                        }
                    }
                }
                else
                {
                    iter->SeekForPrev(key);
                    if (!inclusive && iter->Valid())
                    {
                        rocksdb::Slice curr_key = iter->key();
                        if (curr_key.ToStringView() == key.ToStringView())
                        {
                            iter->Prev();
                        }
                    }
                }
            }
            else
            {
                if (scan_forward)
                {
                    iter->SeekToFirst();
                }
                else
                {
                    iter->SeekToLast();
                }
            }

            // Fetch the batch of records into the response
            uint32_t record_count = 0;
            while (iter->Valid() && record_count < scan_req.batch_size())
            {
                auto *new_item = resp.add_items();
                new_item->set_key(iter->key().ToString());
                new_item->set_value(iter->value().ToString());
                if (scan_forward)
                {
                    iter->Next();
                }
                else
                {
                    iter->Prev();
                }
                record_count++;
            }

            // run out of records, close iter
            if (!iter->Valid())
            {
                delete iter;
            }
            else
            {
                // Otherwise, save the iterator in the session map
                // Set session id in the response
                auto iter_wrapper =
                    std::make_unique<RocksDBIteratorTTLWrapper>(iter);
                std::string session_id =
                    data_store_service_->GenerateSessionId();
                // Set session id in the response
                resp.clear_session_id();
                resp.set_session_id(session_id);
                // Save the iterator in the session map
                data_store_service_->EmplaceScanIter(
                    shard_id_, session_id, std::move(iter_wrapper));
            }

            result->set_error_code(::EloqDS::remote::DataStoreError::NO_ERROR);
            on_finish(resp);
        });
}

void RocksDBCloudDataStore::ScanNext(
    const ::EloqDS::remote::ScanRequest &scan_req,
    std::function<void(const ::EloqDS::remote::ScanResponse &response)>
        on_finish)
{
    query_worker_pool_->SubmitWork(
        [this, scan_req = std::move(scan_req), on_finish = std::move(on_finish)]
        {
            uint32_t req_shard_id = scan_req.req_shard_id();
            bool scan_forward = scan_req.scan_forward();
            const std::string &session_id = scan_req.session_id();

            std::shared_lock<std::shared_mutex> db_lk(db_mux_);

            auto shard_status = FetchDSShardStatus();
            ::EloqDS::remote::ScanResponse resp;
            ::EloqDS::remote::CommonResult *result = resp.mutable_result();
            if (shard_status != DSShardStatus::ReadOnly &&
                shard_status != DSShardStatus::ReadWrite)
            {
                data_store_service_->PrepareShardingError(
                    req_shard_id, shard_id_, result);
                on_finish(resp);
                return;
            }

            TTLWrapper *iter_wrapper =
                data_store_service_->BorrowScanIter(shard_id_, session_id);
            rocksdb::Iterator *iter = nullptr;
            if (iter_wrapper != nullptr)
            {
                auto *rocksdb_iter_wrapper =
                    static_cast<RocksDBIteratorTTLWrapper *>(iter_wrapper);
                iter = rocksdb_iter_wrapper->GetIter();
            }
            else
            {
                // Run scan open if the session id is not found
                ScanOpen(scan_req, on_finish);
                return;
            }

            // Fetch the batch of records into the response
            uint32_t record_count = 0;
            while (iter->Valid() && record_count < scan_req.batch_size())
            {
                auto *new_item = resp.add_items();
                new_item->set_key(iter->key().ToString());
                new_item->set_value(iter->value().ToString());
                if (scan_forward)
                {
                    iter->Next();
                }
                else
                {
                    iter->Prev();
                }
                record_count++;
            }

            resp.clear_session_id();

            // run out of records, close iter
            if (!iter->Valid())
            {
                data_store_service_->EraseScanIter(shard_id_, session_id);
            }
            else
            {
                data_store_service_->ReturnScanIter(shard_id_, iter_wrapper);
                // Set session id carry over to the response
                resp.set_session_id(session_id);
            }

            result->set_error_code(::EloqDS::remote::DataStoreError::NO_ERROR);
            on_finish(resp);
        });
}

void RocksDBCloudDataStore::ScanClose(
    const ::EloqDS::remote::ScanRequest &scan_req,
    std::function<void(const ::EloqDS::remote::ScanResponse &response)>
        on_finish)
{
    query_worker_pool_->SubmitWork(
        [this,
         scan_req = std::move(scan_req),
         on_finish = std::move(on_finish)]()
        {
            std::shared_lock<std::shared_mutex> db_lk(db_mux_);

            uint32_t req_shard_id = scan_req.req_shard_id();
            const std::string &session_id = scan_req.session_id();
            auto shard_status = FetchDSShardStatus();
            ::EloqDS::remote::ScanResponse resp;
            ::EloqDS::remote::CommonResult *result = resp.mutable_result();
            if (shard_status != DSShardStatus::ReadOnly &&
                shard_status != DSShardStatus::ReadWrite)
            {
                data_store_service_->PrepareShardingError(
                    req_shard_id, shard_id_, result);
                on_finish(resp);
                return;
            }

            data_store_service_->EraseScanIter(shard_id_, session_id);

            result->set_error_code(::EloqDS::remote::DataStoreError::NO_ERROR);
            on_finish(resp);
        });
}

void RocksDBCloudDataStore::SwitchToReadOnly()
{
    WaitForPendingWrites();

    bthread::Mutex mutex;
    bthread::ConditionVariable cond_var;
    bool done = false;

    // pause all background jobs to stop compaction and obselete file
    // deletion
    query_worker_pool_->SubmitWork(
        [this, &mutex, &cond_var, &done]()
        {
            // Run pause background work in a separate thread to avoid blocking
            // bthread worker threads
            std::shared_lock<std::shared_mutex> db_lk(db_mux_);
            auto db = GetDBPtr();
            if (db != nullptr)
            {
                db->PauseBackgroundWork();
            }

            std::unique_lock<bthread::Mutex> lk(mutex);
            done = true;
            cond_var.notify_one();
        });

    std::unique_lock<bthread::Mutex> lk(mutex);

    while (!done)
    {
        cond_var.wait(lk);
    }
}

void RocksDBCloudDataStore::SwitchToReadWrite()
{
    // Since ContinueBackgroundWork() is non-blocking, we don't need the
    // thread synchronization machinery here
    std::shared_lock<std::shared_mutex> db_lk(db_mux_);
    auto db = GetDBPtr();
    if (db != nullptr)
    {
        db->ContinueBackgroundWork();
    }
}
#endif
}  // namespace EloqDS
