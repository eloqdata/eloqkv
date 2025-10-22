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
#include <gflags/gflags.h>

#include "INIReader.h"
#include "data_store_handler.h"
#include "data_substrate.h"
#include "kv_store.h"
#if ELOQDS
#include <filesystem>

#include "eloq_data_store_service/data_store_service.h"
#include "eloq_data_store_service/data_store_service_config.h"
#include "data_store_service_client.h"
#endif
#if defined(DATA_STORE_TYPE_ROCKSDB)
#include "store_handler/rocksdb_handler.h"
#endif

#if defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_S3) ||                       \
    defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_GCS)
#include "eloq_data_store_service/rocksdb_cloud_data_store_factory.h"
#elif defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB)
#include "eloq_data_store_service/rocksdb_data_store_factory.h"
#elif defined(DATA_STORE_TYPE_ELOQDSS_ELOQSTORE)
#include "eloq_data_store_service/eloq_store_data_store_factory.h"
#endif

#ifdef DATA_STORE_TYPE_DYNAMODB
#include "store_handler/dynamo_handler.h"
#endif

#ifdef DATA_STORE_TYPE_BIGTABLE
#include "store_handler/bigtable_handler.h"
#endif

#if defined(DATA_STORE_TYPE_DYNAMODB)
DEFINE_string(dynamodb_endpoint, "", "Endpoint of KvStore Dynamodb");
DEFINE_string(dynamodb_keyspace, "eloq", "KeySpace of Dynamodb KvStore");
DEFINE_string(dynamodb_region,
              "ap-northeast-1",
              "Region of the used trable in DynamoDB");
#endif

#ifdef DATA_STORE_TYPE_BIGTABLE
DEFINE_string(bigtable_project_id, "", "Project id of BigTable");
DEFINE_string(bigtable_instance_id, "", "Instance id of BigTable");
DEFINE_string(bigtable_keyspace, "eloq", "KeySpace of BigTable");
#endif
// aws_secret_key
#if defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_S3)
DECLARE_string(aws_access_key_id);
DECLARE_string(aws_secret_key);
#elif defined(DATA_STORE_TYPE_DYNAMODB) ||                                     \
    (defined(USE_ROCKSDB_LOG_STATE) && defined(WITH_ROCKSDB_CLOUD))
DEFINE_string(aws_access_key_id, "", "AWS SDK access key id");
DEFINE_string(aws_secret_key, "", "AWS SDK secret key");
#endif

#if ELOQDS
DEFINE_string(eloq_dss_peer_node,
              "",
              "EloqDataStoreService peer node address. Used to fetch eloq-dss "
              "topology from a working eloq-dss server.");
DEFINE_string(eloq_dss_branch_name,
              "development",
              "Branch name of EloqDataStore");
DEFINE_string(eloq_dss_config_file_path,
              "",
              "EloqDataStoreService config file path. Used to load eloq-dss "
              "config from a file.");
#endif

DEFINE_uint32(snapshot_sync_worker_num, 0, "Snpashot sync worker num");

bool DataSubstrate::InitializeStorageHandler(const INIReader &config_reader)
{
    if (CheckCommandLineFlagIsDefault("snapshot_sync_worker_num") &&
        !config_reader.HasValue("store", "snapshot_sync_worker_num"))
    {
        FLAGS_snapshot_sync_worker_num =
            std::max(core_config_.core_num / 4, static_cast<uint32_t>(1));
    }
#if defined(DATA_STORE_TYPE_DYNAMODB)
    std::string dynamodb_endpoint =
        !CheckCommandLineFlagIsDefault("dynamodb_endpoint")
            ? FLAGS_dynamodb_endpoint
            : config_reader.GetString(
                  "store", "dynamodb_endpoint", FLAGS_dynamodb_endpoint);
    std::string dynamodb_keyspace =
        !CheckCommandLineFlagIsDefault("dynamodb_keyspace")
            ? FLAGS_dynamodb_keyspace
            : config_reader.GetString(
                  "store", "dynamodb_keyspace", FLAGS_dynamodb_keyspace);
    std::string dynamodb_region =
        !CheckCommandLineFlagIsDefault("dynamodb_region")
            ? FLAGS_dynamodb_region
            : config_reader.GetString(
                  "store", "dynamodb_region", FLAGS_dynamodb_region);
    std::string aws_access_key_id =
        !CheckCommandLineFlagIsDefault("aws_access_key_id")
            ? FLAGS_aws_access_key_id
            : config_reader.GetString(
                  "store", "aws_access_key_id", FLAGS_aws_access_key_id);
    std::string aws_secret_key =
        !CheckCommandLineFlagIsDefault("aws_secret_key")
            ? FLAGS_aws_secret_key
            : config_reader.GetString(
                  "store", "aws_secret_key", FLAGS_aws_secret_key);
    bool ddl_skip_kv = false;
    uint16_t worker_pool_size = core_num_ * 2;

    store_hd_ = std::make_unique<EloqDS::DynamoHandler>(dynamodb_keyspace,
                                                        dynamodb_endpoint,
                                                        dynamodb_region,
                                                        aws_access_key_id,
                                                        aws_secret_key,
                                                        core_config_.bootstrap,
                                                        ddl_skip_kv,
                                                        worker_pool_size,
                                                        false);

#elif defined(DATA_STORE_TYPE_BIGTABLE)
    std::string bigtable_keyspace =
        !CheckCommandLineFlagIsDefault("bigtable_keyspace")
            ? FLAGS_bigtable_keyspace
            : config_reader.GetString(
                  "store", "bigtable_keyspace", FLAGS_bigtable_keyspace);
    std::string bigtable_project_id =
        !CheckCommandLineFlagIsDefault("bigtable_project_id")
            ? FLAGS_bigtable_project_id
            : config_reader.GetString(
                  "store", "bigtable_project_id", FLAGS_bigtable_project_id);
    std::string bigtable_instance_id =
        !CheckCommandLineFlagIsDefault("bigtable_instance_id")
            ? FLAGS_bigtable_instance_id
            : config_reader.GetString(
                  "store", "bigtable_instance_id", FLAGS_bigtable_instance_id);
    bool ddl_skip_kv = false;
    store_hd_ = std::make_unique<EloqDS::BigTableHandler>(
        bigtable_keyspace, bigtable_project_id, bigtable_instance_id, core_config_.bootstrap,
        ddl_skip_kv);

#elif defined(DATA_STORE_TYPE_ROCKSDB)
    bool is_single_node =
        (network_config_.standby_ip_list.empty() &&
         network_config_.voter_ip_list.empty() &&
         network_config_.ip_list.find(',') == network_config_.ip_list.npos);

    EloqShare::RocksDBConfig rocksdb_config(config_reader,
                                            core_config_.data_path);

    store_hd_ = std::make_unique<EloqKV::RocksDBHandlerImpl>(
        rocksdb_config,
        (core_config_.bootstrap || is_single_node),
        core_config_.enable_cache_replacement);

#elif ELOQDS
    bool is_single_node =
        (network_config_.standby_ip_list.empty() &&
         network_config_.voter_ip_list.empty() &&
         network_config_.ip_list.find(',') == network_config_.ip_list.npos);

    std::string eloq_dss_peer_node =
        !CheckCommandLineFlagIsDefault("eloq_dss_peer_node")
            ? FLAGS_eloq_dss_peer_node
            : config_reader.GetString(
                  "store", "eloq_dss_peer_node", FLAGS_eloq_dss_peer_node);

    std::string eloq_dss_data_path = core_config_.data_path + "/eloq_dss";
    if (!std::filesystem::exists(eloq_dss_data_path))
    {
        std::filesystem::create_directories(eloq_dss_data_path);
    }

    std::string dss_config_file_path =   !CheckCommandLineFlagIsDefault("eloq_dss_config_file_path")
            ? FLAGS_eloq_dss_config_file_path
            : config_reader.GetString(
                  "store", "eloq_dss_config_file_path", FLAGS_eloq_dss_config_file_path);

    if (dss_config_file_path.empty())
    {
        dss_config_file_path = eloq_dss_data_path + "/dss_config.ini";
    }

    EloqDS::DataStoreServiceClusterManager ds_config;
    if (std::filesystem::exists(dss_config_file_path))
    {
        bool load_res = ds_config.Load(dss_config_file_path);
        if (!load_res)
        {
            LOG(ERROR) << "Failed to load dss config file  : "
                       << dss_config_file_path;
            return false;
        }
    }
    else
    {
        if (!eloq_dss_peer_node.empty())
        {
            ds_config.SetThisNode(network_config_.local_ip,
                                  network_config_.local_port + 7);
            // Fetch ds topology from peer node
            if (!EloqDS::DataStoreService::FetchConfigFromPeer(
                    eloq_dss_peer_node, ds_config))
            {
                LOG(ERROR) << "Failed to fetch config from peer node: "
                           << eloq_dss_peer_node;
                return false;
            }

            // Save the fetched config to the local file
            if (!ds_config.Save(dss_config_file_path))
            {
                LOG(ERROR) << "Failed to save config to file: "
                           << dss_config_file_path;
                return false;
            }
        }
        else if (core_config_.bootstrap || is_single_node)
        {
            // Initialize the data store service config
            ds_config.Initialize(network_config_.local_ip,
                                 network_config_.local_port + 7);
            if (!ds_config.Save(dss_config_file_path))
            {
                LOG(ERROR) << "Failed to save config to file: "
                           << dss_config_file_path;
                return false;
            }
        }
        else
        {
            LOG(ERROR) << "Failed to load data store service config file: "
                       << dss_config_file_path;
            return false;
        }
    }

#if defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_S3) ||                       \
    defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_GCS)
    EloqDS::RocksDBConfig rocksdb_config(config_reader, eloq_dss_data_path);
    EloqDS::RocksDBCloudConfig rocksdb_cloud_config(config_reader);
    rocksdb_cloud_config.branch_name_ = FLAGS_eloq_dss_branch_name;
    auto ds_factory = std::make_unique<EloqDS::RocksDBCloudDataStoreFactory>(
        rocksdb_config,
        rocksdb_cloud_config,
        core_config_.enable_cache_replacement);

#elif defined(DATA_STORE_TYPE_ELOQDSS_ROCKSDB)
    EloqDS::RocksDBConfig rocksdb_config(config_reader, eloq_dss_data_path);
    auto ds_factory = std::make_unique<EloqDS::RocksDBDataStoreFactory>(
        rocksdb_config, core_config_.enable_cache_replacement);

#elif defined(DATA_STORE_TYPE_ELOQDSS_ELOQSTORE)
    EloqDS::EloqStoreConfig eloq_store_config(config_reader,
                                              eloq_dss_data_path);
    auto ds_factory = std::make_unique<EloqDS::EloqStoreDataStoreFactory>(
        std::move(eloq_store_config));
#endif

    data_store_service_ = std::make_unique<EloqDS::DataStoreService>(
        ds_config,
        dss_config_file_path,
        eloq_dss_data_path + "/DSMigrateLog",
        std::move(ds_factory));
    // setup local data store service
    bool ret = data_store_service_->StartService(core_config_.bootstrap || is_single_node);
    if (!ret)
    {
        LOG(ERROR) << "Failed to start data store service";
        return false;
    }
    // setup data store service client
    txservice::CatalogFactory *catalog_factory[NUM_EXTERNAL_ENGINES] = {nullptr, nullptr, nullptr};

    for (size_t i = 0; i < NUM_EXTERNAL_ENGINES; i++)
    {
        catalog_factory[i] = engines_[i].catalog_factory;
    }

    store_hd_ = std::make_unique<EloqDS::DataStoreServiceClient>(
        catalog_factory, ds_config, data_store_service_.get());

#endif

    for (const auto &[table_name, _] : prebuilt_tables_)
    {
        store_hd_->AppendPreBuiltTable(table_name);
    }

    if (!store_hd_->Connect())
    {
        LOG(ERROR) << "!!!!!!!! Failed to connect to kvstore, startup is "
                      "terminated !!!!!!!!";
        return false;
    }
    return true;
}