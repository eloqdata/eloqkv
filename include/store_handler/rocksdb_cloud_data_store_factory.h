#pragma once

#include <memory>
#include <string>

#include "data_store.h"
#include "data_store_factory.h"
#include "rocksdb_cloud_config.h"
#include "rocksdb_cloud_data_store.h"
#include "rocksdb_config.h"

#if KV_DATA_STORE_TYPE == KV_DATA_STORE_TYPE_ROCKSDB

namespace EloqDS
{

class RocksDBCloudDataStoreFactory : public DataStoreFactory
{
public:
    RocksDBCloudDataStoreFactory(
        const ::EloqShare::RocksDBConfig &config,
#if ROCKSDB_CLOUD_FS()
        const ::EloqShare::RocksDBCloudConfig &cloud_config,
#endif
        const std::vector<TableName> &pre_built_tables,
        bool tx_enable_cache_replacement)
        : config_(config),
          cloud_config_(cloud_config),
          pre_built_tables_(pre_built_tables),
          tx_enable_cache_replacement_(tx_enable_cache_replacement)
    {
    }

    std::unique_ptr<DataStore> CreateDataStore(
        bool create_if_missing,
        uint32_t shard_id,
        DataStoreService *data_store_service,
        bool start_db = true) override
    {
        auto ds = std::make_unique<RocksDBCloudDataStore>(
            cloud_config_,
            config_,
            create_if_missing,
            tx_enable_cache_replacement_,
            shard_id,
            data_store_service);
        for (const auto &table_name : pre_built_tables_)
        {
            ds->AppendPreBuiltTable(table_name.StringView());
        }
        ds->Connect();
        if (start_db)
        {
            ds->StartDB();
        }
        return ds;
    }

private:
    ::EloqShare::RocksDBConfig config_;
#if ROCKSDB_CLOUD_FS()
    ::EloqShare::RocksDBCloudConfig cloud_config_;
#endif
    std::vector<TableName> pre_built_tables_;
    bool tx_enable_cache_replacement_;
};

}  // namespace EloqDS

#endif
