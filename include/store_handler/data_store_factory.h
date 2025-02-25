#pragma once

#include <memory>
#include <string>

#include "data_store.h"

namespace EloqDS
{

class DataStoreFactory
{
public:
    virtual ~DataStoreFactory() = default;

    virtual std::unique_ptr<DataStore> CreateDataStore(
        bool create_if_missing,
        uint32_t shard_id,
        DataStoreService *data_store_service,
        bool start_db = true) = 0;
};

}  // namespace EloqDS
