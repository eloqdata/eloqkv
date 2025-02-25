#pragma once

#include <brpc/channel.h>
#include <bthread/condition_variable.h>
#include <bthread/mutex.h>

#include <chrono>
#include <memory>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "data_store.h"
#include "data_store_factory.h"
#include "data_store_service_config.h"
#include "data_store_service_util.h"
#include "ds_request.pb.h"
#include "kv_store.h"
#include "thread_worker_pool.h"

namespace EloqDS
{
#if ROCKSDB_CLOUD_FS()
/**
 * @brief Wrapper class for any object that needs to be cached with TTL.
 */
class TTLWrapper
{
public:
    TTLWrapper()
    {
        UpdateLastAccessTime();
    }

    ~TTLWrapper() = default;

    // Delete copy operations to avoid accidental copies.
    TTLWrapper(const TTLWrapper &) = delete;
    TTLWrapper &operator=(const TTLWrapper &) = delete;

    // Default move operations (if needed).
    TTLWrapper(TTLWrapper &&) noexcept = default;
    TTLWrapper &operator=(TTLWrapper &&) noexcept = default;

    // Accessors
    uint64_t GetLastAccessTime() const
    {
        return last_access_time_;
    }

    void UpdateLastAccessTime()
    {
        last_access_time_ =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
    }

protected:
    bool InUse() const
    {
        return in_use_;
    }

    void SetInUse(bool in_use)
    {
        in_use_ = in_use;
    }

private:
    bool in_use_{false};
    uint64_t last_access_time_{0};

    friend class TTLWrapperCache;
};

/**
 * @brief Cache for TTLWrapper objects with cache ID as key.
 *        Cached objects are checked for TTL expiration periodically,
 *        and to be removed if expired and not in use.
 */
class TTLWrapperCache
{
public:
    TTLWrapperCache();

    ~TTLWrapperCache();

    /**
     * @brief Start the TTL check worker.
     */
    void TTLCheckWorker();

    /**
     * @brief Emplace a TTLWrapper object with the specified session ID.
     * @param session_id The cache ID.
     * @param iter The TTLWrapper object to be cached.
     */
    void Emplace(std::string &cache_id, std::unique_ptr<TTLWrapper> iter);

    /**
     * @brief Borrow a TTLWrapper object with the specified cache ID.
     *        The borrowed object is marked as in use.
     * @param cache_id The cache ID.
     * @return The borrowed TTLWrapper object.
     */
    TTLWrapper *Borrow(const std::string &cache_id);

    /**
     * @brief Return a TTLWrapper object with the specified cache ID.
     *        The returned object is marked as not in use, and the last access
     * time is updated.
     * @param cache_id The cache ID.
     * @param iter The TTLWrapper object to be returned.
     */
    void Return(TTLWrapper *iter);

    /**
     * @brief Erase a TTLWrapper object with the specified cache ID.
     * @param cache_id The cache ID.
     */
    void Erase(const std::string &cache_id);

    /**
     * @brief Clear the cache
     *       All cached objects are removed.
     *       This function should be called after all cached objects are not in
     * use.
     */
    void Clear();

private:
    // Scan iterator TTL check interval in milliseconds
    static const uint64_t TTL_CHECK_INTERVAL_MS_{3000};

    // scan iterator cache
    bthread::Mutex mutex_;
    bthread::ConditionVariable ttl_wrapper_cache_cv_;
    std::unordered_map<std::string, std::unique_ptr<TTLWrapper>>
        ttl_wrapper_cache_;
    // scan iterator TTL check
    bool ttl_check_running_{false};
    std::unique_ptr<ThreadWorkerPool> ttl_check_worker_;
};

class DataStoreService : EloqDS::remote::DataStoreRpcService
{
public:
    DataStoreService(const DataStoreServiceClusterManager &config,
                     const std::string &config_file_path,
                     const std::string &migration_log_path,
                     std::unique_ptr<DataStoreFactory> &&data_store_factory);

    ~DataStoreService();

    bool StartService();

    bool ConnectDataStore(
        std::unordered_map<uint32_t, std::unique_ptr<DataStore>>
            &&data_store_map);

    bool DisconnectDataStore();

    bool ConnectAndStartDataStore(uint32_t data_shard_id,
                                  DSShardStatus open_mode,
                                  bool create_db_if_missing = false);

    // =======================================================
    // Group: External RPC function interface called by Client
    // =======================================================

    void FetchRecords(::google::protobuf::RpcController *controller,
                      const ::EloqDS::remote::FetchRecordsRequest *request,
                      ::EloqDS::remote::FetchRecordsResponse *response,
                      ::google::protobuf::Closure *done) override;

    void BatchWriteRecords(
        ::google::protobuf::RpcController *controller,
        const ::EloqDS::remote::BatchWriteRecordsRequest *request,
        ::EloqDS::remote::BatchWriteRecordsResponse *response,
        ::google::protobuf::Closure *done) override;

    void CkptEnd(::google::protobuf::RpcController *controller,
                 const ::EloqDS::remote::CkptEndRequest *request,
                 ::EloqDS::remote::CkptEndResponse *response,
                 ::google::protobuf::Closure *done) override;

    void FetchTableCatalog(
        ::google::protobuf::RpcController *controller,
        const ::EloqDS::remote::FetchTableCatalogRequest *request,
        ::EloqDS::remote::FetchTableCatalogResponse *response,
        ::google::protobuf::Closure *done) override;

    void ReadClusterConfig(
        ::google::protobuf::RpcController *controller,
        const ::EloqDS::remote::ReadClusterConfigRequest *request,
        ::EloqDS::remote::ReadClusterConfigResponse *response,
        ::google::protobuf::Closure *done) override;

    void UpsertTable(::google::protobuf::RpcController *controller,
                     const ::EloqDS::remote::UpsertTableRequest *request,
                     ::EloqDS::remote::UpsertTableResponse *response,
                     ::google::protobuf::Closure *done) override;

    void UpdateClusterConfig(
        ::google::protobuf::RpcController *controller,
        const ::EloqDS::remote::UpdateClusterConfigRequest *request,
        ::EloqDS::remote::UpdateClusterConfigResponse *response,
        ::google::protobuf::Closure *done) override;

    // =======================================================================
    // Group: External local function called by service client if this service
    //        is co-located with client
    // =======================================================================

    bool InitializeClusterConfig(
        uint32_t req_shard_id,
        const std::unordered_map<
            uint32_t,
            std::vector<::EloqDS::remote::ClusterNodeConfig>> &ng_configs);

    void ReadClusterConfig(
        uint32_t req_shard_id,
        std::function<void(
            std::unordered_map<uint32_t,
                               std::vector<::EloqDS::remote::ClusterNodeConfig>>
                &&ng_configs,
            uint64_t version,
            bool uninitialized,
            const ::EloqDS::remote::CommonResult &result)> on_finish);

    void BatchWriteRecords(
        uint32_t req_shard_id,
        std::vector<EloqDS::remote::BatchWriteRecordsRequest::record> &&batch,
        const std::string &kv_table_name,
        std::function<void(const ::EloqDS::remote::CommonResult &result)>
            on_finish);

    void CkptEnd(
        uint32_t req_shard_id,
        const std::string &table_name,
        std::function<void(const ::EloqDS::remote::CommonResult &result)>
            on_finish);

    void FetchTableCatalog(
        uint32_t req_shard_id,
        const std::string table_name,
        std::function<void(std::string &&schema_image,
                           bool found,
                           uint64_t version_ts,
                           const ::EloqDS::remote::CommonResult &result)>
            on_finish);

    void UpsertTable(
        uint32_t req_shard_id,
        std::string table_name_str,
        std::string old_schema_img,
        std::string new_schema_img,
        ::EloqDS::remote::UpsertTableOperationType op_type,
        uint64_t commit_ts,
        std::function<void(const EloqDS::remote::CommonResult &result)>
            on_finish);

    bool FetchRecords(
        uint32_t req_shard_id,
        std::vector<::EloqDS::remote::FetchRecordsRequest::key> &&keys,
        std::function<
            void(std::vector<::EloqDS::remote::FetchRecordsResponse::record>
                     &&records,
                 const ::EloqDS::remote::CommonResult &result)> on_finish);

    void UpdateClusterConfig(
        uint32_t req_shard_id,
        uint64_t version,
        const std::unordered_map<
            uint32_t,
            std::vector<::EloqDS::remote::ClusterNodeConfig>>
            &new_cluster_configs,
        std::function<void(const ::EloqDS::remote::CommonResult &result)>
            on_finish);

    void PrepareShardingError(uint32_t req_shard_id,
                              uint32_t shard_id,
                              ::EloqDS::remote::CommonResult *result)
    {
        cluster_manager_.PrepareShardingError(req_shard_id, shard_id, result);
    }

    // =======================================================================
    // Group: External function for data scan operation
    // =======================================================================

    /**
     * @brief Generate a session id for scan operation
     * @return Session id
     */
    std::string GenerateSessionId();

    /**
     * @brief Scan open operation
     * @param scan_req Scan request
     * @param on_finish Callback function
     */
    void ScanOpen(
        const ::EloqDS::remote::ScanRequest &scan_req,
        std::function<void(const ::EloqDS::remote::ScanResponse &resp)>
            on_finish);

    /**
     * @brief Scan next operation
     * @param scan_req Scan request
     * @param on_finish Callback function
     */
    void ScanNext(
        const ::EloqDS::remote::ScanRequest &scan_req,
        std::function<void(const ::EloqDS::remote::ScanResponse &resp)>
            on_finish);

    /**
     * @brief Scan close operation
     * @param scan_req Scan request
     * @param on_finish Callback function
     */
    void ScanClose(
        const ::EloqDS::remote::ScanRequest &scan_req,
        std::function<void(const ::EloqDS::remote::ScanResponse &resp)>
            on_finish);

    /**
     * @brief Emplace scan iterator into scan iter cache
     * @param iter Scan iterator
     * @return Session id
     */
    void EmplaceScanIter(uint32_t shard_id,
                         std::string &session_id,
                         std::unique_ptr<TTLWrapper> iter);

    /**
     * @brief Find and mark scan iterator in use
     * @param session_id Session id
     * @return Scan iterator wrapper
     */
    TTLWrapper *BorrowScanIter(uint32_t shard_id,
                               const std::string &session_id);

    /**
     * @brief Return scan iterator to scan iter cache
     * @param iter Scan iterator wrapper
     */
    void ReturnScanIter(uint32_t shard_id, TTLWrapper *iter);

    /**
     * @brief Erase scan iterator from scan iter cache
     * @param session_id Session id
     */
    void EraseScanIter(uint32_t shard_id, const std::string &session_id);

    // =======================================================================
    // Group: External data store service node management RPC interface
    // =======================================================================

    void FetchDSSClusterConfig(
        ::google::protobuf::RpcController *controller,
        const ::google::protobuf::Empty *request,
        ::EloqDS::remote::FetchDSSClusterConfigResponse *response,
        ::google::protobuf::Closure *done) override;

    void UpdateDSSClusterConfig(
        ::google::protobuf::RpcController *controller,
        const ::EloqDS::remote::UpdateDSSClusterConfigRequest *request,
        ::EloqDS::remote::UpdateDSSClusterConfigResponse *response,
        ::google::protobuf::Closure *done) override;

    void ShardMigrate(::google::protobuf::RpcController *controller,
                      const ::EloqDS::remote::ShardMigrateRequest *request,
                      ::EloqDS::remote::ShardMigrateResponse *response,
                      ::google::protobuf::Closure *done) override;

    void ShardMigrateStatus(
        ::google::protobuf::RpcController *controller,
        const ::EloqDS::remote::ShardMigrateStatusRequest *request,
        ::EloqDS::remote::ShardMigrateStatusResponse *response,
        ::google::protobuf::Closure *done) override;

    void OpenDSShard(::google::protobuf::RpcController *controller,
                     const ::EloqDS::remote::OpenDSShardRequest *request,
                     ::EloqDS::remote::OpenDSShardResponse *response,
                     ::google::protobuf::Closure *done) override;

    void SwitchDSShardMode(
        ::google::protobuf::RpcController *controller,
        const ::EloqDS::remote::SwitchDSShardModeRequest *request,
        ::EloqDS::remote::SwitchDSShardModeResponse *response,
        ::google::protobuf::Closure *done) override;

    void UpdateDSShardConfig(
        ::google::protobuf::RpcController *controller,
        const ::EloqDS::remote::UpdateDSShardConfigRequest *request,
        ::EloqDS::remote::UpdateDSShardConfigResponse *response,
        ::google::protobuf::Closure *done) override;

    void FaultInjectForTest(::google::protobuf::RpcController *controller,
                            const ::EloqDS::remote::FaultInjectRequest *request,
                            ::EloqDS::remote::FaultInjectResponse *response,
                            ::google::protobuf::Closure *done) override;

    static bool FetchConfigFromPeer(const std::string &peer_addr,
                                    DataStoreServiceClusterManager &config);

    void ScanOpen(::google::protobuf::RpcController *controller,
                  const ::EloqDS::remote::ScanRequest *request,
                  ::EloqDS::remote::ScanResponse *response,
                  ::google::protobuf::Closure *done) override;

    void ScanNext(::google::protobuf::RpcController *controller,
                  const ::EloqDS::remote::ScanRequest *request,
                  ::EloqDS::remote::ScanResponse *response,
                  ::google::protobuf::Closure *done) override;

    void ScanClose(::google::protobuf::RpcController *controller,
                   const ::EloqDS::remote::ScanRequest *request,
                   ::EloqDS::remote::ScanResponse *response,
                   ::google::protobuf::Closure *done) override;

    // =======================================================================
    // Group: Internal function for shard related operations
    // =======================================================================
    DSShardStatus FetchDSShardStatus(uint32_t shard_id);

    void AddListenerForUpdateConfig(
        std::function<void(const DataStoreServiceClusterManager &)> listener)
    {
        update_config_listener_ = listener;
    }

private:
    /**
     * @brief Append the key string of this node to the specified string stream.
     */
    void AppendThisNodeKey(std::stringstream &ss);

    bool SwitchToReadOnly(uint32_t shard_id, DSShardStatus expected);

    bool SwitchToReadWrite(uint32_t shard_id, DSShardStatus expected);

    bool SwitchToClosed(uint32_t shard_id, DSShardStatus expected);

    bool WriteMigrationLog(uint32_t shard_id,
                           const std::string &event_id,
                           const std::string &target_node_ip,
                           uint16_t target_node_port,
                           uint32_t migration_status,
                           uint64_t shard_version);

    bool ReadMigrationLog(uint32_t &shard_id,
                          std::string &event_id,
                          std::string &target_node_ip,
                          uint16_t &target_node_port,
                          uint32_t &migration_status,
                          uint64_t &shard_next_version);

    std::shared_mutex serv_mux_;
    int32_t service_port_;
    std::unique_ptr<brpc::Server> server_;

    DataStoreServiceClusterManager cluster_manager_;
    std::string config_file_path_;
    std::string migration_log_path_;

    // shard id to data store
    std::unordered_map<uint32_t, std::unique_ptr<DataStore>> data_store_map_;
    std::unordered_map<uint32_t, std::unique_ptr<TTLWrapperCache>>
        scan_iter_cache_map_;

    std::unique_ptr<DataStoreFactory> data_store_factory_;

    // Now, only for update client's config
    std::function<void(DataStoreServiceClusterManager &)>
        update_config_listener_;

private:
    struct MigrateLog
    {
        std::string event_id;
        uint32_t shard_id;
        std::string target_node_host;
        uint16_t target_node_port;
        uint32_t status{0};
        uint64_t shard_next_version{0};
        std::chrono::system_clock::time_point creation_time;

        MigrateLog() : creation_time(std::chrono::system_clock::now())
        {
        }

        MigrateLog(std::string event_id,
                   uint32_t shard_id,
                   std::string target_node_host,
                   uint16_t target_node_port,
                   uint32_t status,
                   uint64_t shard_next_version)
            : event_id(event_id),
              shard_id(shard_id),
              target_node_host(target_node_host),
              target_node_port(target_node_port),
              status(status),
              shard_next_version(shard_next_version),
              creation_time(std::chrono::system_clock::now())
        {
        }
    };

    void CleanupOldMigrateLogs();

    std::pair<remote::ShardMigrateError, std::string> NewMigrateTask(
        const std::string &event_id,
        int data_shard_id,
        std::string target_node_host,
        uint16_t target_node_port,
        uint64_t shard_next_version);
    void CheckAndRecoverMigrateTask();
    std::string GetMigrateLogPath(const std::string &event_id);
    bool MigrationLogExists();
    bool RemoveMigrationLog(const std::string &event_id);

    bool DoMigrate(const std::string &event_id, MigrateLog *log);
    bool NotifyTargetNodeOpenDSShard(const DSSNode &target_node,
                                     uint32_t data_shard_id,
                                     remote::DSShardStatus mode,
                                     const EloqDS::DSShard &shard_config);
    bool NotifyTargetNodeSwitchDSShardMode(const DSSNode &target_node,
                                           uint32_t data_shard_id,
                                           uint64_t data_shard_version,
                                           remote::DSShardStatus mode);
    bool NotifyNodesUpdateDSShardConfig(const std::set<DSSNode> &nodes,
                                        const DSShard &shard_config);

    ThreadWorkerPool migrate_worker_{1};

    // map{event_id->migrate_log}
    std::shared_mutex migrate_task_mux_;
    std::unordered_map<std::string, MigrateLog> migrate_task_map_;
};
#endif
}  // namespace EloqDS
