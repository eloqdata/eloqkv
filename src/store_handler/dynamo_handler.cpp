#include "dynamo_handler.h"

#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/utils/Array.h>
#include <aws/core/utils/Outcome.h>
#include <aws/core/utils/UUID.h>
#include <aws/core/utils/threading/Executor.h>
#include <aws/dynamodb/model/BatchWriteItemRequest.h>
#include <aws/dynamodb/model/BatchWriteItemResult.h>
#include <aws/dynamodb/model/CreateTableRequest.h>
#include <aws/dynamodb/model/DeleteItemRequest.h>
#include <aws/dynamodb/model/DeleteItemResult.h>
#include <aws/dynamodb/model/DeleteRequest.h>
#include <aws/dynamodb/model/DeleteTableRequest.h>
#include <aws/dynamodb/model/DeleteTableResult.h>
#include <aws/dynamodb/model/DescribeTableRequest.h>
#include <aws/dynamodb/model/DescribeTimeToLiveRequest.h>
#include <aws/dynamodb/model/DescribeTimeToLiveResult.h>
#include <aws/dynamodb/model/GetItemRequest.h>
#include <aws/dynamodb/model/ListTablesRequest.h>
#include <aws/dynamodb/model/ListTablesResult.h>
#include <aws/dynamodb/model/PutItemRequest.h>
#include <aws/dynamodb/model/PutItemResult.h>
#include <aws/dynamodb/model/QueryRequest.h>
#include <aws/dynamodb/model/QueryResult.h>
#include <aws/dynamodb/model/ScanRequest.h>
#include <aws/dynamodb/model/TimeToLiveDescription.h>
#include <aws/dynamodb/model/TimeToLiveSpecification.h>
#include <aws/dynamodb/model/TransactWriteItemsRequest.h>
#include <aws/dynamodb/model/TransactWriteItemsResult.h>
#include <aws/dynamodb/model/UpdateItemRequest.h>
#include <aws/dynamodb/model/UpdateItemResult.h>
#include <aws/dynamodb/model/UpdateTimeToLiveRequest.h>
#include <aws/dynamodb/model/UpdateTimeToLiveResult.h>

#include <algorithm>  //std::min
#include <map>
#include <memory>
#include <set>

#include "kv_store.h"
#include "tx_service.h"
// #include "sequences.h"
#include "bthread/timer_thread.h"
#include "butil/string_splitter.h"
#include "dynamo_handler_typed.h"
#include "dynamo_scanner.h"
#include "tx_service/include/error_messages.h"
#include "tx_service/include/tx_record.h"

using namespace Aws::DynamoDB::Model;

// dynamo handler meter
std::unique_ptr<metrics::Meter> EloqDS::dynamo_metrics_meter;
static const metrics::Name DYNAMO_FLUSH_ROWS_COUNT_NAME_{
    "dynamo_flush_rows_count"};
static const metrics::Name DYNAMO_LOAD_SLICE_DURATION_NAME_{
    "dynamo_load_slice_duration"};
static const metrics::Name DYNAMO_READ_DURATION_NAME_{"dynamo_read_duration"};

typedef struct DynamoCatalog
{
    std::string partition_key_;
    ScalarAttributeType pk_type_;
    std::string sort_key_;
    ScalarAttributeType sk_type_;
} DynamoCatalog;

static const std::string dynamo_table_catalog_name = "eloqkv_tables";
static const std::string dynamo_database_catalog_name = "eloqkv_databases";
static const std::string dynamo_mvcc_archive_name = "mvcc_archives";
static const std::string dynamo_table_statistics_version_name =
    "table_statistics_version";
static const std::string dynamo_table_statistics_name = "table_statistics";
static const std::string dynamo_range_table_name = "table_ranges";
static const std::string dynamo_last_range_id_name =
    "table_last_range_partition_id";
static const std::string dynamo_cluster_config_name = "cluster_config";

static const std::unordered_map<std::string, DynamoCatalog> dynamo_sys_tables(
    {{std::string(dynamo_table_catalog_name),
      {std::string("tablename"),
       ScalarAttributeType::S,
       std::string(),
       ScalarAttributeType::NOT_SET}},
     {std::string(dynamo_database_catalog_name),
      {std::string("dbname"),
       ScalarAttributeType::S,
       std::string(),
       ScalarAttributeType::NOT_SET}},
     {std::string(dynamo_mvcc_archive_name),
      {std::string("tblname___key"),
       ScalarAttributeType::B,
       std::string("commit_ts"),
       ScalarAttributeType::N}},
     {std::string(dynamo_table_statistics_version_name),
      {std::string("tablename"),
       ScalarAttributeType::S,
       std::string(),
       ScalarAttributeType::NOT_SET}},
     {std::string(dynamo_table_statistics_name),
      {std::string("tablename"),
       ScalarAttributeType::S,
       std::string("version:indextype:indexname:segment_id"),
       ScalarAttributeType::S}},
     {std::string(dynamo_cluster_config_name),
      {std::string("pk"),
       ScalarAttributeType::N,
       std::string(),
       ScalarAttributeType::NOT_SET}}
#ifdef RANGE_PARTITION_ENABLED
     ,
     {std::string(dynamo_range_table_name),
      {std::string("tablename"),
       ScalarAttributeType::S,
       std::string("start_key"),
       ScalarAttributeType::B}},
     {std::string(dynamo_last_range_id_name),
      {std::string("tablename"),
       ScalarAttributeType::S,
       std::string(),
       ScalarAttributeType::NOT_SET}}
#endif
    });

const int dynamo_api_retry = 5;
// Max batch size for BatchWrite.
const int dynamo_batch_size = 25;
// Max number of async requests sent before waiting for previous results.
const int dynamo_max_futures = 32;
// DynamoDB treat timestamps older than 5 years as invalid timestamps
// and would never expire them.
const int dynamo_no_expire_ttl = 0;
// We expire deleted items after 24 hours.
const int dynamo_expire_ttl = 86400;
static thread_local std::unique_ptr<EloqDS::PartitionFinder> partition_finder;

EloqDS::DynamoHandler::DynamoHandler(const std::string &keyspace,
                                     const std::string &endpoint,
                                     const std::string &region,
                                     const std::string &aws_access_key_id,
                                     const std::string &aws_secret_key,
                                     bool bootstrap,
                                     bool ddl_skip_kv,
                                     int worker_pool_size,
                                     bool skip_putall)
    : keyspace_(keyspace),
      is_bootstrap_(bootstrap),
      ddl_skip_kv_(ddl_skip_kv),
      /*sdk_options_(),*/ worker_pool_(worker_pool_size),
      skip_putall_(skip_putall)
{
    // This must be called before doing anything else with this library.
    // Aws::InitAPI(sdk_options_);

    Aws::Client::ClientConfiguration clientConfig;
    clientConfig.region = region;
    if (endpoint.size())
    {
        // Override endpoint if provided.
        clientConfig.endpointOverride = endpoint;
    }
    clientConfig.executor =
        Aws::MakeShared<Aws::Utils::Threading::PooledThreadExecutor>(
            "dynamo-executor", 10);
    if (aws_access_key_id.empty() || aws_secret_key.empty())
    {
        client_ = std::make_unique<Aws::DynamoDB::DynamoDBClient>(clientConfig);
    }
    else
    {
        Aws::Auth::AWSCredentials credentials(aws_access_key_id,
                                              aws_secret_key);
        client_ = std::make_unique<Aws::DynamoDB::DynamoDBClient>(credentials,
                                                                  clientConfig);
    }
}

EloqDS::DynamoHandler::~DynamoHandler()
{
    worker_pool_.Shutdown();
    // Aws::ShutdownAPI(sdk_options_);
}

/*
 * @brief
 * Create system tables eloqkv_databases, eloqkv_tables
 * if they do not exist yet. This function should be safe to be
 * called concurrently by other hosts.
 *
 * eloqkv_databases
 * {
 *  dbname              String  (Parition Key),
 *  definition          Binary
 * }
 *
 * eloqkv_tables
 * {
 *  tablename           String  (Partition Key),
 *  content             Binary,
 *  timestamp           Number,
 *  statistics          Binary,
 *  kvtablename,        String,
 *  kvindexname,        Map
 * }
 *
 * mvcc_archives
 * {
 *  tblname___key          String  (Partition Key),
 *  commit_ts           Number  (Sort Key),
 *  commit_ts           Number,
 *  payload_status      Number,
 *  payload             Binary
 * }
 *
 * table_ranges
 * {
 *   tablename           String  (Partition Key),
 *   ___mono_key___      Binary  (Sort Key),
 *   ___partition_key___ Number,
 *   ___slice_keys___    List,
 *   ___slice_sizes___   List,
 *   ___version___       Number
 * }
 *
 * table_last_range_partition_id
 * {
 *   tablename           String  (Partition Key),
 *   last_partition_id   Number
 * }
 *
 * table_statistics
 * {
 *  tablename           String  (Partition Key),
 *  version:indextype:indexname:segment_id  String (Sort Key),
 *  records             Number,
 *  samplekeys          NumberSet
 * }
 *
 * table_statistics_version
 * {
 *   tablename           String  (Partition Key),
 *   version             Number
 * }
 *
 * cluster_config
 * {
 *  pk                  Number  (Partition Key),
 *  ngids               List,
 *  ips                 List,
 *  ports               List,
 *  ng_members          List,
 *  version             Number
 * }
 */
bool EloqDS::DynamoHandler::Connect()
{
    DescribeTableRequest dtr;
    std::unordered_map<std::string, bool> sys_table_active = {
        {dynamo_table_catalog_name, false},
        {dynamo_database_catalog_name, false},
        {dynamo_mvcc_archive_name, false}};

    bool need_init = false;
    for (const auto &[tablename, sys_table] : dynamo_sys_tables)
    {
        dtr.SetTableName(keyspace_ + '.' + tablename);

        // check if sys table already exists
        const DescribeTableOutcome &dtr_result = client_->DescribeTable(dtr);

        if (dtr_result.IsSuccess())
        {
            // Check if the table is in active status
            const TableStatus &status =
                dtr_result.GetResult().GetTable().GetTableStatus();

            if (status == TableStatus::ACTIVE)
            {
                sys_table_active[tablename] = true;
                continue;
            }

            // Any status other than ACTIVE and CREATING is invalid
            if (status != TableStatus::CREATING)
                return false;
            // Other host might be creating the table now, let's wait for them
            // to complete
        }
        else if (dtr_result.GetError().GetErrorType() ==
                 Aws::DynamoDB::DynamoDBErrors::RESOURCE_NOT_FOUND)
        {
            need_init = true;
            // Create the sys table if it doesn't exist yet
            CreateTableRequest ctr;
            AttributeDefinition partition_key;
            partition_key.WithAttributeName(sys_table.partition_key_)
                .WithAttributeType(sys_table.pk_type_);
            ctr.AddAttributeDefinitions(std::move(partition_key));

            KeySchemaElement pk_schema;
            pk_schema.WithAttributeName(sys_table.partition_key_)
                .WithKeyType(Aws::DynamoDB::Model::KeyType::HASH);
            ctr.AddKeySchema(std::move(pk_schema));
            ctr.SetTableName(keyspace_ + '.' + tablename);
            ctr.SetBillingMode(BillingMode::PAY_PER_REQUEST);

            // Add sort key if exists
            if (sys_table.sort_key_.size())
            {
                AttributeDefinition sort_key;
                sort_key.WithAttributeName(sys_table.sort_key_)
                    .WithAttributeType(sys_table.sk_type_);
                ctr.AddAttributeDefinitions(std::move(sort_key));
                KeySchemaElement sk_schema;
                sk_schema.WithAttributeName(sys_table.sort_key_)
                    .WithKeyType(Aws::DynamoDB::Model::KeyType::RANGE);
                ctr.AddKeySchema(std::move(sk_schema));
            }

            const CreateTableOutcome &ctr_result = client_->CreateTable(ctr);

            // If the error is RESOURCE_IN_USE that means someone else just
            // created this table before us. Lets wait for the table to become
            // active. Otherwise the create request has failed, return false
            if (!ctr_result.IsSuccess() &&
                ctr_result.GetError().GetErrorType() !=
                    Aws::DynamoDB::DynamoDBErrors::RESOURCE_IN_USE)
            {
                return false;
            }
        }
        else
        {
            return false;
        }
    }

    // Create table request is an async request, verify if the tables are in
    // active state if they are just created.
    for (auto &[tablename, active] : sys_table_active)
    {
        if (active)
        {
            continue;
        }
        dtr.SetTableName(keyspace_ + '.' + tablename);
        for (uint retry = 0; retry < 25; ++retry)
        {
            const DescribeTableOutcome &dtr_result =
                client_->DescribeTable(dtr);
            if (!dtr_result.IsSuccess())
            {
                return false;
            }
            if (dtr_result.GetResult().GetTable().GetTableStatus() ==
                TableStatus::ACTIVE)
            {
                active = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        // Create Table failed or timed out
        if (!active)
        {
            LOG(INFO) << "Create sys table " << tablename << " failed";
            return false;
        }
    }

    // Set TTL attribute for archive table
    DynamoCatalogInfo archive_info;
    archive_info.kv_table_name_ = dynamo_mvcc_archive_name;
    UpdateDynamoTTL(&archive_info);
    if (!archive_info.ttl_set)
    {
        LOG(INFO) << "Set TTL on MVCC archive table failed";
    }
    else
    {
        archive_ttl_set_ = true;
    }

    if (need_init && !InitPreBuiltTables())
    {
        return false;
    }

    ScheduleTimerTasks();
    return true;
}

void EloqDS::DynamoHandler::ScheduleTimerTasks()
{
    timer_thd_.start(nullptr);
    CleanDefunctKvTables(this);
}

bool EloqDS::DynamoHandler::InitializeClusterConfig(
    const std::unordered_map<uint32_t, std::vector<txservice::NodeConfig>>
        &ng_configs)
{
    PutItemRequest pir;
    pir.SetTableName(keyspace_ + '.' + dynamo_cluster_config_name);
    AttributeValue pk_val, ngids_val, nodeids_val, ips_val, ports_val,
        ng_members_val, members_candidate_val, version_val;
    // Pk in this table is just a dummy place holder for now.
    pk_val.SetN(0);
    // use 2 as initial version since commit_ts = 1 means entry just initialized
    // and not valid yet.
    version_val.SetN(2);
    // extract nodes
    std::vector<txservice::NodeConfig> nodes;
    txservice::ExtractNodesConfigs(ng_configs, nodes);

    for (const txservice::NodeConfig &node_info : nodes)
    {
        std::shared_ptr<AttributeValue> nid =
            std::make_shared<AttributeValue>();
        nid->SetN(std::to_string(node_info.node_id_));
        nodeids_val.AddLItem(std::move(nid));
        std::shared_ptr<AttributeValue> ip = std::make_shared<AttributeValue>();
        ip->SetS(node_info.host_name_);
        ips_val.AddLItem(std::move(ip));
        std::shared_ptr<AttributeValue> port =
            std::make_shared<AttributeValue>();
        port->SetN(std::to_string(node_info.port_));
        ports_val.AddLItem(std::move(port));
    }

    for (const auto &[ng_id, ng_members] : ng_configs)
    {
        std::shared_ptr<AttributeValue> ng_id_val =
            std::make_shared<AttributeValue>();
        ng_id_val->SetN(std::to_string(ng_id));
        ngids_val.AddLItem(std::move(ng_id_val));
        std::string members;
        std::string members_candidate;
        // std::vector<NodeConfig> member_list;
        for (const auto &m : ng_members)
        {
            members.append(std::to_string(m.node_id_)).append(" ");
            members_candidate.append(std::to_string((int) m.is_candidate_))
                .append(" ");
        }
        members.pop_back();
        members_candidate.pop_back();

        std::shared_ptr<AttributeValue> member =
            std::make_shared<AttributeValue>();
        member->SetS(std::move(members));
        ng_members_val.AddLItem(std::move(member));
        std::shared_ptr<AttributeValue> member_candidate =
            std::make_shared<AttributeValue>();
        member_candidate->SetS(std::move(members_candidate));
        members_candidate_val.AddLItem(std::move(member_candidate));
    }

    pir.AddItem("pk", std::move(pk_val));
    pir.AddItem("ngids", std::move(ngids_val));
    pir.AddItem("node_ids", std::move(nodeids_val));
    pir.AddItem("ips", std::move(ips_val));
    pir.AddItem("ports", std::move(ports_val));
    pir.AddItem("ng_members", std::move(ng_members_val));
    pir.AddItem("ng_members_is_candidate", std::move(members_candidate_val));
    pir.AddItem("version", std::move(version_val));
    auto result = client_->PutItem(pir);
    if (!result.IsSuccess())
    {
        LOG(INFO) << "Failed to initialize cluster config in DynamoDB: "
                  << result.GetError().GetMessage();
        return false;
    }
    return true;
}

bool EloqDS::DynamoHandler::ReadClusterConfig(
    std::unordered_map<uint32_t, std::vector<txservice::NodeConfig>>
        &ng_configs,
    uint64_t &version,
    bool &uninitialized)
{
    GetItemRequest gr;
    gr.SetTableName(keyspace_ + '.' + dynamo_cluster_config_name);
    gr.SetConsistentRead(true);
    AttributeValue pk;
    pk.SetN(0);
    gr.AddKey("pk", std::move(pk));

    GetItemOutcome result = client_->GetItem(gr);
    if (!result.IsSuccess())
    {
        LOG(INFO) << "Failed to read cluster config from DynamoDB: "
                  << result.GetError().GetMessage();
        return false;
    }

    // Reference the retrieved fields/values.
    const Aws::Map<Aws::String, AttributeValue> &item =
        result.GetResult().GetItem();
    if (item.size() == 0)
    {
        uninitialized = true;
        return false;
    }

    std::vector<uint32_t> ng_list;
    std::map<uint32_t, std::pair<std::string, uint16_t>> node_ip_map;
    std::vector<std::string> ip_list;
    std::vector<uint16_t> port_list;
    auto &ngids = item.at("ngids").GetL();
    for (auto &ngid : ngids)
    {
        ng_list.push_back(std::stoul(ngid->GetN()));
    }
    auto &ips = item.at("ips").GetL();
    auto &ports = item.at("ports").GetL();
    auto &node_ids = item.at("node_ids").GetL();
    for (size_t idx = 0; idx < node_ids.size(); idx++)
    {
        node_ip_map.try_emplace(
            std::stoul(node_ids.at(idx)->GetN()),
            std::make_pair(ips.at(idx)->GetS(),
                           std::stoul(ports.at(idx)->GetN())));
    }

    auto &members = item.at("ng_members").GetL();
    auto &members_candidate = item.at("ng_members_is_candidate").GetL();
    for (size_t idx = 0; idx < members.size(); idx++)
    {
        std::vector<NodeConfig> ng_config;
        std::string member = members.at(idx)->GetS();
        std::string member_candidate = members_candidate.at(idx)->GetS();
        // parse member string
        std::istringstream iss(member);
        std::istringstream candidate_iss(member_candidate);
        std::string nid_str;
        std::string candidate_str;
        while (iss >> nid_str && candidate_iss >> candidate_str)
        {
            uint32_t nid = std::stoi(nid_str);
            bool is_candidate = static_cast<bool>(std::stoi(candidate_str));
            ng_config.emplace_back(nid,
                                   node_ip_map.at(nid).first,
                                   node_ip_map.at(nid).second,
                                   is_candidate);
        }
        ng_configs.try_emplace(ng_list.at(idx), std::move(ng_config));
    }
    version = std::stoull(item.at("version").GetN());

    return true;
}

bool EloqDS::DynamoHandler::UpdateClusterConfig(
    const std::unordered_map<uint32_t, std::vector<txservice::NodeConfig>>
        &new_cnf,
    uint64_t version)
{
    PutItemRequest pir;
    pir.SetTableName(keyspace_ + '.' + dynamo_cluster_config_name);
    AttributeValue pk_val, ngids_val, nodeids_val, ips_val, ports_val,
        ng_members_val, members_candidate_val, version_val;
    // Pk in this table is just a dummy place holder for now.
    pk_val.SetN(0);
    version_val.SetN(std::to_string(version));

    // extract nodes
    std::vector<txservice::NodeConfig> nodes;
    txservice::ExtractNodesConfigs(new_cnf, nodes);

    for (const txservice::NodeConfig &node_info : nodes)
    {
        std::shared_ptr<AttributeValue> nid =
            std::make_shared<AttributeValue>();
        nid->SetN(std::to_string(node_info.node_id_));
        nodeids_val.AddLItem(std::move(nid));
        std::shared_ptr<AttributeValue> ip = std::make_shared<AttributeValue>();
        ip->SetS(node_info.host_name_);
        ips_val.AddLItem(std::move(ip));
        std::shared_ptr<AttributeValue> port =
            std::make_shared<AttributeValue>();
        port->SetN(std::to_string(node_info.port_));
        ports_val.AddLItem(std::move(port));
    }

    for (const auto &[ng_id, ng_members] : new_cnf)
    {
        std::shared_ptr<AttributeValue> ng_id_val =
            std::make_shared<AttributeValue>();
        ng_id_val->SetN(std::to_string(ng_id));
        ngids_val.AddLItem(std::move(ng_id_val));
        std::string members;
        std::string members_candidate;
        // std::vector<NodeConfig> member_list;
        for (const auto &m : ng_members)
        {
            members.append(std::to_string(m.node_id_)).append(" ");
            members_candidate.append(std::to_string((int) m.is_candidate_))
                .append(" ");
        }
        members.pop_back();
        members_candidate.pop_back();

        std::shared_ptr<AttributeValue> member =
            std::make_shared<AttributeValue>();
        member->SetS(std::move(members));
        ng_members_val.AddLItem(std::move(member));
        std::shared_ptr<AttributeValue> member_candidate =
            std::make_shared<AttributeValue>();
        member_candidate->SetS(std::move(members_candidate));
        members_candidate_val.AddLItem(std::move(member_candidate));
    }

    pir.AddItem("pk", std::move(pk_val));
    pir.AddItem("ngids", std::move(ngids_val));
    pir.AddItem("node_ids", std::move(nodeids_val));
    pir.AddItem("ips", std::move(ips_val));
    pir.AddItem("ports", std::move(ports_val));
    pir.AddItem("ng_members", std::move(ng_members_val));
    pir.AddItem("ng_members_is_candidate", std::move(members_candidate_val));
    pir.AddItem("version", std::move(version_val));
    auto result = client_->PutItem(pir);
    if (!result.IsSuccess())
    {
        LOG(INFO) << "Failed to update cluster config in DynamoDB: "
                  << result.GetError().GetMessage();
        return false;
    }
    return true;
}

bool EloqDS::DynamoHandler::PutAll(std::vector<txservice::FlushRecord> &batch,
                                   const txservice::TableName &table_name,
                                   const txservice::TableSchema *table_schema,
                                   uint32_t node_group)
{
    if (skip_putall_)
    {
        LOG(INFO) << "skip PutAll, table:" << table_name.String()
                  << ", batch size:" << batch.size();
        return true;
    }
    LOG(INFO) << "PutAll begin, table:" << table_name.String()
              << ", batch size:" << batch.size();
    uint64_t begin_ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();

    DynamoCatalogInfo *kv_info =
        static_cast<DynamoCatalogInfo *>(table_schema->GetKVCatalogInfo());
    if (!kv_info->ttl_set)
    {
        // Update TTL for the table if it is not set on create.
        UpdateDynamoTTL(kv_info);
    }

    if (partition_finder == nullptr)
    {
        partition_finder = PartitionFinderFactory::Create();
    }

#ifdef RANGE_PARTITION_ENABLED
    dynamic_cast<RangePartitionFinder *>(partition_finder.get())
        ->Init(tx_service_, node_group);
#endif
    std::vector<std::pair<uint, Partition>> target_partitions;
    PartitionResultType rt =
        partition_finder->FindPartitions(table_name, batch, target_partitions);
    if (rt != PartitionResultType::NORMAL)
    {
        partition_finder->ReleaseReadLocks();
        return false;
    }
    assert(target_partitions.size());
    // Make sure each worker has decent amount of work to do.
    uint worker_cnt = std::min((int) worker_pool_.WorkerPoolSize(),
                               (int) target_partitions.size());
    uint workload = target_partitions.size() / worker_cnt;
    std::mutex worker_mux;
    std::condition_variable worker_cv;
    uint finished_cnt = 0;
    std::atomic_bool res = true;
    // Assign a slice of the checkpoint vector to each worker.
    for (uint i = 0; i < worker_cnt - 1; i++)
    {
        worker_pool_.SubmitWork(
            [this,
             &table_name,
             &batch,
             &target_partitions,
             i,
             workload,
             table_schema,
             node_group,
             &worker_mux,
             &worker_cv,
             &finished_cnt,
             &res]
            {
                PutAllThread(
                    this,
                    &table_name,
                    &batch,
                    (target_partitions.cbegin() + (i + 1) * workload)->first,
                    std::vector(
                        target_partitions.cbegin() + i * workload,
                        target_partitions.cbegin() + ((i + 1) * workload)),
                    table_schema,
                    node_group,
                    &res);
                std::unique_lock<std::mutex> worker_lk(worker_mux);
                finished_cnt++;
                worker_cv.notify_one();
            });
    }
    worker_pool_.SubmitWork(
        [this,
         &table_name,
         &batch,
         &target_partitions,
         worker_cnt,
         workload,
         table_schema,
         node_group,
         &worker_mux,
         &worker_cv,
         &finished_cnt,
         &res]
        {
            PutAllThread(this,
                         &table_name,
                         &batch,
                         batch.size(),
                         std::vector(target_partitions.cbegin() +
                                         (worker_cnt - 1) * workload,
                                     target_partitions.cend()),
                         table_schema,
                         node_group,
                         &res);
            std::unique_lock<std::mutex> worker_lk(worker_mux);
            finished_cnt++;
            worker_cv.notify_one();
        });
    {
        std::unique_lock<std::mutex> worker_lk(worker_mux);
        worker_cv.wait(worker_lk,
                       [&finished_cnt, &worker_cnt]
                       { return worker_cnt == finished_cnt; });
    }

    partition_finder->ReleaseReadLocks();

    uint64_t end_ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    LOG(INFO) << "PutAll end, table:" << table_name.String()
              << ", result:" << static_cast<int>(res.load())
              << ", batch size:" << batch.size()
              << ", used time(ms):" << (end_ts - begin_ts);
    return res.load();
}

/**
 * @brief Worker thread function for putall/putskall. Each thread is assigned
 * at least dynamo_max_futures/3 partitions and 3000 records from a single
 * table. The reason that we spawned worker threads to process requests in
 * parallel is that we have found request preparation phase is taking much
 * more time compared to the waiting time of sent futures, thus we cannot
 * completely consume the write throughput of DynamoDB with only one thread.
 *
 * @param handler
 * @param table_name
 * @param batch
 * @param end
 * @param target_partitions
 * @param table_schema
 * @param node_group
 */
void EloqDS::DynamoHandler::PutAllThread(
    DynamoHandler *handler,
    const txservice::TableName *table_name,
    std::vector<txservice::FlushRecord> *batch,
    uint32_t end,
    std::vector<std::pair<uint, Partition>> target_partitions,
    const txservice::TableSchema *table_schema,
    uint32_t node_group,
    std::atomic_bool *put_res)
{
    bool putall_success = true;
    std::vector<std::pair<BatchWriteItemOutcomeCallable, size_t>> flush_futures;
    std::vector<std::pair<uint, uint>> partition_offset;
    DynamoCatalogInfo *kv_info =
        static_cast<DynamoCatalogInfo *>(table_schema->GetKVCatalogInfo());
    const txservice::RecordSchema *rec_schema = table_schema->RecordSchema();
#ifdef RANGE_PARTITION_ENABLED
    std::map<int32_t, const TxKey *> ranges;
#endif
    const std::string *kv_table_name;
    if (table_name->IsBase())
    {
        kv_table_name = &kv_info->kv_table_name_;
    }
    else
    {
        kv_table_name = &kv_info->kv_index_names_.at(*table_name);
    }

    // Set up partition offset, which is a vector that stores current idx
    // in this partition, and the last idx in this partition.
    for (auto part_it = target_partitions.begin();
         part_it != target_partitions.end();
         part_it++)
    {
        if (std::next(part_it) != target_partitions.end())
        {
            partition_offset.emplace_back(part_it->first,
                                          std::next(part_it)->first);
        }
        else
        {
            partition_offset.emplace_back(part_it->first, end);
        }
    }
    auto part_it = target_partitions.begin();
    auto idx_it = partition_offset.begin();

    Aws::Vector<WriteRequest> write_reqs;

    // DynamoDB has a 1000 WCU throughput limitation on a single partition.
    // In order to maximize DynamoDB throughput, we need to avoid only sending
    // requests on a single partition. After sending a batch from the current
    // partition, we need to move on to the next partition.  We remove processed
    // partitions from target_partitions so we should end up with an empty
    // vector when all records in all partitions are processed.
    while (target_partitions.size() &&
           Sharder::Instance().LeaderTerm(node_group) > 0 &&
           put_res->load(std::memory_order_acquire))
    {
        // When we reached the last partition, start
        // from beginning again.
        if (part_it == target_partitions.end())
        {
            part_it = target_partitions.begin();
            idx_it = partition_offset.begin();
        }
        if (idx_it->first >= idx_it->second)
        {
            // Done with this partition.
            part_it = target_partitions.erase(part_it);
            idx_it = partition_offset.erase(idx_it);
            continue;
        }
        assert(part_it != target_partitions.end());
        assert(idx_it->first < idx_it->second);
        Partition out_partition = part_it->second;
        int32_t pk1 = out_partition.Pk1();
#ifdef RANGE_PARTITION_ENABLED
        const txservice::TxKey *key = batch->at(idx_it->first).Key();
        int32_t new_pk1 = out_partition.NewPk1(key);
        assert(out_partition.RangeOwner() == node_group);
        pk1 = new_pk1 == -1 ? pk1 : new_pk1;
#endif
        uint64_t now = LocalCcShards::ClockTsInMillseconds();
        for (; idx_it->first < idx_it->second &&
               write_reqs.size() < dynamo_batch_size;
             idx_it->first++)
        {
            using namespace txservice;
            FlushRecord &ckpt_rec = batch->at(idx_it->first);

            WriteRequest write_req;
            if (ckpt_rec.payload_status_ != RecordStatus::Deleted &&
                (!ckpt_rec.Payload()->HasTTL() ||
                 ckpt_rec.Payload()->GetTTL() > now))
            {
                AttributeValue pk, sk, version, deleted, ttl;
                PutRequest put_req;
                pk.SetN(pk1);

                put_req.AddItem(dynamo_partition_key_attribute_name,
                                std::move(pk));
                DynamoHandlerTyped::BindDynamoReqForKey(
                    sk, *ckpt_rec.Key().GetKey<EloqKV::EloqKey>());

                version.SetN(std::to_string(ckpt_rec.commit_ts_));
                deleted.SetBool(
                    ckpt_rec.payload_status_ == RecordStatus::Deleted ? true
                                                                      : false);
                if (ckpt_rec.payload_status_ == RecordStatus::Deleted)
                {
                    // Set TTL on the timestamp column. Make DynamoDB delete
                    // this record after 24 hours. We don't need to set the rest
                    // of the cols since this row will become a tombstone row.
                    // Also the payload of ckpt record is empty.
                    int commit_ts_in_sec =
                        std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::microseconds(ckpt_rec.commit_ts_))
                            .count();
                    ttl.SetN(
                        std::to_string(commit_ts_in_sec + dynamo_expire_ttl));
                }
                else
                {
                    const txservice::TxRecord *ckpt_payload =
                        ckpt_rec.Payload();

                    // We need to specify all columns otherwise put request will
                    // remove unset columns from this row.
                    if (ckpt_payload->HasTTL())
                    {
                        int ttl_in_sec =
                            std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::microseconds(
                                    ckpt_payload->GetTTL()))
                                .count();
                        ttl.SetN(ttl_in_sec);
                    }
                    else
                    {
                        ttl.SetN(dynamo_no_expire_ttl);
                    }
                    DynamoHandlerTyped::BindDynamoReqForPayload(
                        put_req, ckpt_payload, rec_schema, *table_name);

                    DynamoHandlerTyped::BindBlobFieldForUnpackInfo(
                        put_req, ckpt_payload);
                }
                put_req.AddItem(dynamo_sort_key_attribute_name, std::move(sk));
                put_req.AddItem("___version___", std::move(version));
                put_req.AddItem("___deleted___", std::move(deleted));
                put_req.AddItem("___exp_time___", std::move(ttl));

                write_req.SetPutRequest(std::move(put_req));
            }
            else
            {
                // expired or deleted keys.
                AttributeValue pk, sk;
                DeleteRequest del_req;
                pk.SetN(pk1);

                del_req.AddKey(dynamo_partition_key_attribute_name,
                               std::move(pk));
                DynamoHandlerTyped::BindDynamoReqForKey(
                    sk, *ckpt_rec.Key().GetKey<EloqKV::EloqKey>());
                del_req.AddKey(dynamo_sort_key_attribute_name, std::move(sk));
                write_req.SetDeleteRequest(std::move(del_req));
            }

            write_reqs.push_back(std::move(write_req));
        }

        if (write_reqs.size() < dynamo_batch_size &&
            target_partitions.size() > 1)
        {
            // Batch is not full and current partition is not the last.
            continue;
        }

        // Send out the batch and store the future. We stop and wait for results
        // for every dynamo_max_futures batch requests sent.
        size_t flush_size = write_reqs.size();
        assert(flush_size);
        BatchWriteItemRequest batch_req;
        batch_req.AddRequestItems(handler->keyspace_ + '.' + *kv_table_name,
                                  std::move(write_reqs));
        flush_futures.emplace_back(
            handler->client_->BatchWriteItemCallable(std::move(batch_req)),
            flush_size);

        // Iterator advance
        if (idx_it->first >= idx_it->second)
        {
            part_it = target_partitions.erase(part_it);
            idx_it = partition_offset.erase(idx_it);
        }
        else
        {
            idx_it++, part_it++;
        }

        // Wait until all inflight requests done
        if (flush_futures.size() == dynamo_max_futures ||
            target_partitions.empty())
        {
            for (auto fut_it = flush_futures.begin();
                 fut_it != flush_futures.end();)
            {
                const BatchWriteItemOutcome &res = fut_it->first.get();
                if (!res.IsSuccess() && !res.GetError().ShouldRetry())
                {
                    put_res->compare_exchange_strong(putall_success, false);
                    LOG(ERROR)
                        << "putall on table " << table_name->String()
                        << " failed, errmsg: " << res.GetError().GetMessage();
                    return;
                }
                if (res.GetResult().GetUnprocessedItems().size() > 0 &&
                    !handler->RetryUnprocessedItems(res))
                {
                    put_res->compare_exchange_strong(putall_success, false);
                    LOG(ERROR) << "putall on table " << table_name->String()
                               << " failed.";
                    return;
                }

                if (metrics::enable_kv_metrics)
                {
                    metrics::kv_meter->Collect(
                        metrics::NAME_KV_FLUSH_ROWS_TOTAL,
                        fut_it->second,
                        "base");
                }

                fut_it = flush_futures.erase(fut_it);
            }
        }
    }
    if (Sharder::Instance().LeaderTerm(node_group) < 0)
    {
        LOG(WARNING) << "DynamoHandler: leader transferred of ng#"
                     << node_group;
        put_res->compare_exchange_strong(putall_success, false);
    }
}

/*
 * @brief: DynamoHandler API for create table and drop table.
 *
 * This function should also handle create/drop of secondary key indexes,
 * insert/delete of related catalog rows in table catalog.
 * Execution result will be put in hd_res
 */
void EloqDS::DynamoHandler::UpsertTable(
    const txservice::TableSchema *old_table_schema,
    const txservice::TableSchema *table_schema,
    txservice::OperationType op_type,
    uint64_t write_time,
    txservice::NodeGroupId ng_id,
    int64_t tx_term,
    txservice::CcHandlerResult<txservice::Void> *hd_res,
    const txservice::AlterTableInfo *alter_table_info,
    txservice::CcRequestBase *cc_req,
    txservice::CcShard *ccs,
    txservice::CcErrorCode *err_code)
{
    int64_t leader_term = Sharder::Instance().TryPinNodeGroupData(ng_id);
    if (leader_term < 0)
    {
        hd_res->SetError(CcErrorCode::TX_NODE_NOT_LEADER);
        return;
    }

    std::shared_ptr<void> defer_unpin(
        nullptr,
        [ng_id](void *) { Sharder::Instance().UnpinNodeGroupData(ng_id); });

    if (leader_term != tx_term)
    {
        hd_res->SetError(CcErrorCode::NG_TERM_CHANGED);
        return;
    }

    const std::shared_ptr<UpsertTableData> table_data =
        std::make_shared<UpsertTableData>(this,
                                          &table_schema->GetBaseTableName(),
                                          old_table_schema,
                                          table_schema,
                                          op_type,
                                          write_time,
                                          defer_unpin,
                                          hd_res,
                                          alter_table_info);

    switch (op_type)
    {
    case txservice::OperationType::DropTable:
    {
        const std::string &old_kv_table_name =
            static_cast<const DynamoCatalogInfo *>(
                old_table_schema->GetKVCatalogInfo())
                ->kv_table_name_;
        DropKvTableAsync(old_kv_table_name);

        DeleteTableRequest fake_req;
        DeleteTableResult fake_res;
        DeleteTableOutcome fake_outcome(std::move(fake_res));
        OnDeleteDynamoTable(client_.get(), fake_req, fake_outcome, table_data);
        break;
    }
    case txservice::OperationType::CreateTable:
    {
        // Fill ./mysql/sequences table schema for Sequences::instance_
        // At this point the schema operation of ./mysql/sequences is
        // irrevertible and the schema will not be changed so we can safely
        // install the schema here. if (table_schema->GetBaseTableName() ==
        // Sequences::table_name_)
        if (table_schema->GetBaseTableName() ==
            DynamoHandlerTyped::sequence_table_name_)
        {
            // Sequences::SetTableSchema(
            //     static_cast<const MysqlTableSchema *>(table_schema));
            DynamoHandlerTyped::SetSequenceTableSchema(table_schema);
        }
        CreateTableRequest ctr;
        AttributeDefinition partition_key, sort_key;
        // Partition key should always be the pk generated by tx_service
        partition_key.WithAttributeName(dynamo_partition_key_attribute_name)
            .WithAttributeType(ScalarAttributeType::N);
        ctr.AddAttributeDefinitions(std::move(partition_key));

        KeySchemaElement pk_schema, sk_schema;
        pk_schema.WithAttributeName(dynamo_partition_key_attribute_name)
            .WithKeyType(Aws::DynamoDB::Model::KeyType::HASH);
        ctr.AddKeySchema(std::move(pk_schema));

        // Sort key should be the binary value packed from mysql primary key
        // cols
        sort_key.WithAttributeName(dynamo_sort_key_attribute_name)
            .WithAttributeType(ScalarAttributeType::B);
        ctr.AddAttributeDefinitions(sort_key);
        sk_schema.WithAttributeName(dynamo_sort_key_attribute_name)
            .WithKeyType(Aws::DynamoDB::Model::KeyType::RANGE);
        ctr.AddKeySchema(std::move(sk_schema));
        const std::string &kv_table_name =
            static_cast<const DynamoCatalogInfo *>(
                table_schema->GetKVCatalogInfo())
                ->kv_table_name_;
        ctr.SetTableName(keyspace_ + '.' + kv_table_name.data());
        ctr.SetBillingMode(BillingMode::PAY_PER_REQUEST);
        client_->CreateTableAsync(ctr, OnCreateDynamoTable, table_data);
        break;
    }
    case txservice::OperationType::AddIndex:
    {
        assert(table_data->alter_table_info_->index_add_count_ > 0 &&
               table_data->alter_table_info_->index_add_count_ ==
                   table_data->alter_table_info_->index_add_names_.size());
        const std::shared_ptr<const UpsertTableData> new_data =
            std::make_shared<const UpsertTableData>(
                *table_data,
                table_data->alter_table_info_->index_add_names_.cbegin());
        UpsertSkTable(new_data);
        break;
    }
    case txservice::OperationType::DropIndex:
    {
        assert(table_data->alter_table_info_->index_drop_count_ > 0 &&
               table_data->alter_table_info_->index_drop_count_ ==
                   table_data->alter_table_info_->index_drop_names_.size());
        const std::shared_ptr<const UpsertTableData> new_data =
            std::make_shared<const UpsertTableData>(
                *table_data,
                table_data->alter_table_info_->index_drop_names_.cbegin(),
                false);
        UpsertSkTable(new_data);
        break;
    }
    case txservice::OperationType::Update:
        UpsertCatalog(table_data);
        break;
    case OperationType::TruncateTable:
    {
        const std::string &old_kv_table_name =
            static_cast<const DynamoCatalogInfo *>(
                old_table_schema->GetKVCatalogInfo())
                ->kv_table_name_;
        DropKvTableAsync(old_kv_table_name);

        const std::string &new_kv_table_name =
            static_cast<const DynamoCatalogInfo *>(
                table_schema->GetKVCatalogInfo())
                ->kv_table_name_;
        CreateKvTableIfNotExists(new_kv_table_name);

        UpsertCatalog(table_data);
        break;
    }
    default:
        LOG(INFO) << "Unsupported command for DynamoHandler::UpsertTable.";
        break;
    }
}

void EloqDS::DynamoHandler::OnDeleteDynamoTable(
    const Aws::DynamoDB::DynamoDBClient *client,
    const DeleteTableRequest &request,
    const DeleteTableOutcome &result,
    const std::shared_ptr<const Aws::Client::AsyncCallerContext> &context)
{
    const std::shared_ptr<const UpsertTableData> table_data =
        std::dynamic_pointer_cast<const UpsertTableData>(context);
    assert(table_data->op_type_ == txservice::OperationType::DropTable);
    if (!result.IsSuccess() &&
        result.GetError().GetErrorType() !=
            Aws::DynamoDB::DynamoDBErrors::RESOURCE_NOT_FOUND)
    {
        LOG(ERROR) << "Delete dynamo table failed, "
                   << result.GetError().GetMessage();
        table_data->hd_res_->SetError(CcErrorCode::DATA_STORE_ERR);
        return;
    }
    uint index_cnt = table_data->old_table_schema_->IndexesSize();
    if (index_cnt != 0)
    {
        // const auto *mysql_table_shema=
        //     static_cast<const MysqlTableSchema *>(table_data->table_schema_);
        const std::unordered_map<
            uint,
            std::pair<txservice::TableName, txservice::SecondaryKeySchema>>
            *indexes =
                DynamoHandlerTyped::GetIndexes(table_data->old_table_schema_);

        const std::shared_ptr<const UpsertTableData> new_data =
            std::make_shared<const UpsertTableData>(*table_data,
                                                    indexes->cbegin());
        UpsertSkTable(new_data);
    }
    else
    {
        UpsertCatalog(table_data);
    }
}

void EloqDS::DynamoHandler::OnCreateDynamoTable(
    const Aws::DynamoDB::DynamoDBClient *client,
    const CreateTableRequest &request,
    const CreateTableOutcome &result,
    const std::shared_ptr<const Aws::Client::AsyncCallerContext> &context)
{
    const std::shared_ptr<const UpsertTableData> table_data =
        std::dynamic_pointer_cast<const UpsertTableData>(context);
    assert(table_data->op_type_ == txservice::OperationType::CreateTable);
    if (!result.IsSuccess() &&
        result.GetError().GetErrorType() !=
            Aws::DynamoDB::DynamoDBErrors::RESOURCE_IN_USE)
    {
        LOG(ERROR) << "Create dynamo table failed: "
                   << result.GetError().GetExceptionName() << " "
                   << result.GetError().GetMessage();
        table_data->hd_res_->SetError(CcErrorCode::DATA_STORE_ERR);
        return;
    }
    uint index_cnt = table_data->table_schema_->IndexesSize();
    if (index_cnt != 0)
    {
        // const auto *mysql_table_shema=
        //     static_cast<const MysqlTableSchema *>(table_data->table_schema_);
        const std::unordered_map<
            uint,
            std::pair<txservice::TableName, txservice::SecondaryKeySchema>> *
            indexes = DynamoHandlerTyped::GetIndexes(table_data->table_schema_);
        // mysql_table_shema->GetIndexes();

        const std::shared_ptr<const UpsertTableData> new_data =
            std::make_shared<const UpsertTableData>(*table_data,
                                                    indexes->cbegin());

        UpsertSkTable(new_data);
    }
    else
    {
        UpsertCatalog(table_data);
    }
}

static inline bool check_ttl_on_table(
    const std::string &table, const Aws::DynamoDB::DynamoDBClient *client)
{
    DescribeTimeToLiveRequest desc_req;
    desc_req.SetTableName(table);

    const DescribeTimeToLiveOutcome &outcome =
        client->DescribeTimeToLive(desc_req);
    if (outcome.IsSuccess())
    {
        const TimeToLiveDescription &desc =
            outcome.GetResult().GetTimeToLiveDescription();
        if (desc.TimeToLiveStatusHasBeenSet() &&
            desc.GetAttributeName() == std::string("___exp_time___"))
        {
            return true;
        }
    }

    return false;
}

static inline bool set_ttl_on_table(const UpdateTimeToLiveRequest &set_req,
                                    const Aws::DynamoDB::DynamoDBClient *client)
{
    uint8_t retry = 0;

    for (retry = 0; retry < dynamo_api_retry; ++retry)
    {
        const UpdateTimeToLiveOutcome &result =
            client->UpdateTimeToLive(set_req);
        // Repeated update ttl request might result in Validation Exception,
        // ignore it.
        if (!result.IsSuccess() &&
            result.GetError().GetErrorType() !=
                Aws::DynamoDB::DynamoDBErrors::VALIDATION)
        {
            // Retry later if table is not ready yet, do not block the aws
            // worker thread.
            if (result.GetError().GetErrorType() ==
                Aws::DynamoDB::DynamoDBErrors::RESOURCE_NOT_FOUND)
            {
                LOG(ERROR) << "Update TTL on table " << set_req.GetTableName()
                           << " timed out due to table not ready.";
            }
            else
            {
                // Update TTL failed.
                LOG(ERROR) << "Update TTL on table " << set_req.GetTableName()
                           << " failed. Errmsg: "
                           << result.GetError().GetMessage();
            }
            return false;
        }
        else
        {
            break;
        }
    }

    if (retry == dynamo_api_retry)
    {
        LOG(ERROR) << "Update TTL on table " << set_req.GetTableName()
                   << " timed out due to table not ready.";
        return false;
    }

    return true;
}

/**
 * @brief Set expire time column to be the TTL column in
 * DynamoDB. This column will be set on deletion, and DynamoDB
 * will clean up dead tuples after they expire after 24 hours.
 *
 * @param table_data
 */
void EloqDS::DynamoHandler::UpdateDynamoTTL(DynamoCatalogInfo *dynamo_info)
{
    std::string &kv_table_name = dynamo_info->kv_table_name_;
    UpdateTimeToLiveRequest set_req;
    TimeToLiveSpecification specification;
    specification.SetEnabled(true);
    specification.SetAttributeName("___exp_time___");
    set_req.SetTimeToLiveSpecification(std::move(specification));

    // Set TTL on base table
    set_req.SetTableName(keyspace_ + '.' + kv_table_name);
    if (!check_ttl_on_table(set_req.GetTableName(), client_.get()) &&
        !set_ttl_on_table(set_req, client_.get()))
    {
        return;
    }

    // Set TTL on the sk tables
    for (auto index : dynamo_info->kv_index_names_)
    {
        set_req.SetTableName(keyspace_ + '.' + index.second);
        if (!check_ttl_on_table(set_req.GetTableName(), client_.get()) &&
            !set_ttl_on_table(set_req, client_.get()))
        {
            return;
        }
    }

    dynamo_info->ttl_set = true;
}

void EloqDS::DynamoHandler::UpsertCatalog(
    const std::shared_ptr<const UpsertTableData> table_data)
{
    switch (table_data->op_type_)
    {
    case txservice::OperationType::DropTable:
    {
        DeleteItemRequest req;
        req.SetTableName(table_data->dynamo_hd_->keyspace_ + '.' +
                         dynamo_table_catalog_name);
        req.AddKey("tablename",
                   AttributeValue(table_data->table_name_->String()));
        table_data->dynamo_hd_->client_->DeleteItemAsync(
            req, OnDeleteCatalog, table_data);
        break;
    }
    case txservice::OperationType::CreateTable:
    case txservice::OperationType::Update:
    {
        PutItemRequest req;
        req.SetTableName(table_data->dynamo_hd_->keyspace_ + '.' +
                         dynamo_table_catalog_name);

        const DynamoCatalogInfo *dynamo_info =
            static_cast<const DynamoCatalogInfo *>(
                table_data->table_schema_->GetKVCatalogInfo());
        std::string frm, kv_info, key_schemas_ts_str;
        const std::string &schema_image =
            table_data->table_schema_->SchemaImage();
        DeserializeSchemaImage(schema_image, frm, kv_info, key_schemas_ts_str);

        AttributeValue content, version, kvtablename, kvindexname;

        Aws::Utils::ByteBuffer content_bytes(
            reinterpret_cast<const unsigned char *>(frm.data()), frm.size());
        content.SetB(content_bytes);

        version.SetN(std::to_string(table_data->table_schema_->Version()));

        req.AddItem("tablename",
                    AttributeValue(table_data->table_name_->String()));
        req.AddItem("content", std::move(content));
        req.AddItem("version", std::move(version));
        req.AddItem("kvtablename", AttributeValue(dynamo_info->kv_table_name_));
        AttributeValue key_schemas_ts;
        uint64_t schema_ts = table_data->table_schema_->KeySchema()->SchemaTs();
        key_schemas_ts.AddMEntry(
            table_data->table_name_->String(),
            std::make_shared<AttributeValue>(std::to_string(schema_ts)));
        AttributeValue index_names;
        if (dynamo_info->kv_index_names_.size() != 0)
        {
            for (auto it = dynamo_info->kv_index_names_.cbegin();
                 it != dynamo_info->kv_index_names_.cend();
                 ++it)
            {
                index_names.AddMEntry(
                    it->first.String(),
                    std::make_shared<AttributeValue>(it->second));
                schema_ts = table_data->table_schema_->IndexKeySchema(it->first)
                                ->SchemaTs();
                key_schemas_ts.AddMEntry(it->first.String(),
                                         std::make_shared<AttributeValue>(
                                             std::to_string(schema_ts)));
            }
        }
        else
        {
            index_names.SetM(
                Aws::Map<Aws::String, const std::shared_ptr<AttributeValue>>());
        }
        req.AddItem("kvindexname", index_names);
        req.AddItem("keyschemasts", key_schemas_ts);
        table_data->dynamo_hd_->client_->PutItemAsync(
            req, OnPutCatalog, table_data);
        break;
    }
    case txservice::OperationType::AddIndex:
    case txservice::OperationType::DropIndex:
    {
        UpdateItemRequest req;
        req.SetTableName(table_data->dynamo_hd_->keyspace_ + '.' +
                         dynamo_table_catalog_name);
        // set key
        req.AddKey("tablename",
                   AttributeValue(table_data->table_name_->String()));

        const DynamoCatalogInfo *dynamo_info =
            static_cast<const DynamoCatalogInfo *>(
                table_data->table_schema_->GetKVCatalogInfo());
        std::string frm, kv_info, key_schemas_ts_str;
        const std::string &schema_image =
            table_data->table_schema_->SchemaImage();
        DeserializeSchemaImage(schema_image, frm, kv_info, key_schemas_ts_str);

        AttributeValue content, version, kvindexname;
        Aws::Utils::ByteBuffer bytes(
            reinterpret_cast<const unsigned char *>(frm.data()), frm.size());
        content.SetB(bytes);
        version.SetN(std::to_string(table_data->table_schema_->Version()));

        AttributeValue key_schemas_ts;
        uint64_t schema_ts = table_data->table_schema_->KeySchema()->SchemaTs();
        key_schemas_ts.AddMEntry(
            table_data->table_name_->String(),
            std::make_shared<AttributeValue>(std::to_string(schema_ts)));
        AttributeValue index_names;
        if (dynamo_info->kv_index_names_.size() != 0)
        {
            for (auto it = dynamo_info->kv_index_names_.cbegin();
                 it != dynamo_info->kv_index_names_.cend();
                 ++it)
            {
                index_names.AddMEntry(
                    it->first.String(),
                    std::make_shared<AttributeValue>(it->second));
                schema_ts = table_data->table_schema_->IndexKeySchema(it->first)
                                ->SchemaTs();
                key_schemas_ts.AddMEntry(it->first.String(),
                                         std::make_shared<AttributeValue>(
                                             std::to_string(schema_ts)));
            }
        }
        else
        {
            index_names.SetM(
                Aws::Map<Aws::String, const std::shared_ptr<AttributeValue>>());
        }

        // construct the SET update expression
        Aws::String update_expression(
            "SET #a = :valueA, #b = :valueB, #c = :valueC, #d = :valueD");
        req.SetUpdateExpression(std::move(update_expression));
        // construct attribute names
        Aws::Map<Aws::String, Aws::String> expressionAttributeNames;
        expressionAttributeNames["#a"] = "content";
        expressionAttributeNames["#b"] = "version";
        expressionAttributeNames["#c"] = "kvindexname";
        expressionAttributeNames["#d"] = "keyschemasts";
        req.SetExpressionAttributeNames(std::move(expressionAttributeNames));
        // construct attribute values
        Aws::Map<Aws::String, AttributeValue> expressionAttributeValues;
        expressionAttributeValues[":valueA"] = content;
        expressionAttributeValues[":valueB"] = version;
        expressionAttributeValues[":valueC"] = index_names;
        expressionAttributeValues[":valueD"] = key_schemas_ts;
        req.SetExpressionAttributeValues(std::move(expressionAttributeValues));
        // update the item
        table_data->dynamo_hd_->client_->UpdateItemAsync(
            req, OnUpdateCatalog, table_data);
        break;
    }
    case OperationType::TruncateTable:
    {
        UpdateItemRequest req;
        req.SetTableName(table_data->dynamo_hd_->keyspace_ + '.' +
                         dynamo_table_catalog_name);
        req.AddKey("tablename",
                   AttributeValue(table_data->table_name_->String()));

        Aws::String update_expression(
            "SET #a = :valueA, #b = :valueB, #c = :valueC");
        req.SetUpdateExpression(update_expression);

        Aws::Map<Aws::String, Aws::String> attrNames;
        attrNames["#a"] = "keyschemasts";
        attrNames["#b"] = "tableversion";
        attrNames["#c"] = "kvtablename";
        req.SetExpressionAttributeNames(attrNames);

        Aws::Map<Aws::String, AttributeValue> attrValues;
        attrValues[":valueA"] = AttributeValue(
            std::to_string(table_data->table_schema_->Version()));
        attrValues[":valueB"] = AttributeValue(
            std::to_string(table_data->table_schema_->Version()));

        const std::string &new_kv_table_name =
            static_cast<const DynamoCatalogInfo *>(
                table_data->table_schema_->GetKVCatalogInfo())
                ->kv_table_name_;
        attrValues[":valueC"] = AttributeValue(new_kv_table_name);
        req.SetExpressionAttributeValues(attrValues);

        const UpdateItemOutcome &outcome =
            table_data->dynamo_hd_->client_->UpdateItem(req);
        if (outcome.IsSuccess())
        {
            table_data->hd_res_->SetFinished();
            LOG(INFO) << "Passed to update tableversion when truncate a "
                         "table.";
        }
        else
        {
            LOG(INFO) << "Failed to update tableversion when truncate a table, "
                      << outcome.GetError().GetMessage();
        }
        break;
    }
    default:
        LOG(INFO) << "Unsupported command for DynamoHandler::UpsertCatalog.";
        break;
    }
}

void EloqDS::DynamoHandler::OnDeleteCatalog(
    const Aws::DynamoDB::DynamoDBClient *client,
    const DeleteItemRequest &request,
    const DeleteItemOutcome &result,
    const std::shared_ptr<const Aws::Client::AsyncCallerContext> &context)
{
    const std::shared_ptr<const UpsertTableData> table_data =
        std::dynamic_pointer_cast<const UpsertTableData>(context);
    assert(table_data->op_type_ == txservice::OperationType::DropTable);
    if (!result.IsSuccess())
    {
        LOG(ERROR) << "Delete catalog failed: "
                   << result.GetError().GetExceptionName() << " "
                   << result.GetError().GetMessage();
        table_data->hd_res_->SetError(CcErrorCode::DATA_STORE_ERR);
        return;
    }

    UpsertTableStatistics(table_data);
}

void EloqDS::DynamoHandler::OnPutCatalog(
    const Aws::DynamoDB::DynamoDBClient *client,
    const PutItemRequest &request,
    const PutItemOutcome &result,
    const std::shared_ptr<const Aws::Client::AsyncCallerContext> &context)
{
    const std::shared_ptr<const UpsertTableData> table_data =
        std::dynamic_pointer_cast<const UpsertTableData>(context);
    assert(table_data->op_type_ != txservice::OperationType::DropTable);

    if (!result.IsSuccess())
    {
        LOG(INFO) << "put catalog failed: "
                  << result.GetError().GetExceptionName() << " "
                  << result.GetError().GetMessage();
        table_data->hd_res_->SetError(CcErrorCode::DATA_STORE_ERR);
        return;
    }

    if (table_data->op_type_ == txservice::OperationType::Update)
    {
        table_data->hd_res_->SetFinished();
    }
    else
    {
        assert(table_data->op_type_ == txservice::OperationType::CreateTable ||
               table_data->op_type_ == txservice::OperationType::DropTable ||
               table_data->op_type_ == txservice::OperationType::AddIndex ||
               table_data->op_type_ == txservice::OperationType::DropIndex);
        UpsertTableStatistics(table_data);
    }
}

void EloqDS::DynamoHandler::OnUpdateCatalog(
    const Aws::DynamoDB::DynamoDBClient *client,
    const UpdateItemRequest &request,
    const UpdateItemOutcome &result,
    const std::shared_ptr<const Aws::Client::AsyncCallerContext> &context)
{
    const std::shared_ptr<const UpsertTableData> table_data =
        std::dynamic_pointer_cast<const UpsertTableData>(context);
    assert(table_data->op_type_ == txservice::OperationType::AddIndex ||
           table_data->op_type_ == txservice::OperationType::DropIndex);

    if (!result.IsSuccess())
    {
        LOG(INFO) << "update catalog failed: "
                  << result.GetError().GetExceptionName() << " "
                  << result.GetError().GetMessage();
        table_data->hd_res_->SetError(CcErrorCode::DATA_STORE_ERR);
        return;
    }
// NOTE: Range partition is not supported in Dynamo yet.
#ifdef RANGE_PARTITION_ENABLED
    SetupRange(table_data);
#else
    table_data->hd_res_->SetFinished();
#endif
}

void EloqDS::DynamoHandler::UpsertSkTable(
    const std::shared_ptr<const UpsertTableData> table_data)
{
    const DynamoCatalogInfo *dynamo_info =
        static_cast<const DynamoCatalogInfo *>(
            table_data->table_schema_->GetKVCatalogInfo());
    switch (table_data->op_type_)
    {
    case txservice::OperationType::DropTable:
    case txservice::OperationType::DropIndex:
    {
        const std::string &kv_index_name =
            (table_data->op_type_ == txservice::OperationType::DropTable)
                ? dynamo_info->kv_index_names_.at(
                      table_data->indexes_it_->second.first)
                : (table_data->drop_indexes_it_->second);

        table_data->dynamo_hd_->DropKvTableAsync(kv_index_name);

        DeleteTableRequest fake_req;
        DeleteTableResult fake_res;
        DeleteTableOutcome fake_result(std::move(fake_res));
        OnDeleteDynamoSkTable(table_data->dynamo_hd_->client_.get(),
                              fake_req,
                              fake_result,
                              table_data);
        break;
    }
    case txservice::OperationType::CreateTable:
    case txservice::OperationType::AddIndex:
    {
        const std::string &kv_index_name =
            (table_data->op_type_ == txservice::OperationType::CreateTable)
                ? dynamo_info->kv_index_names_.at(
                      table_data->indexes_it_->second.first)
                : (table_data->add_indexes_it_->second);
        CreateTableRequest req;
        req.SetTableName(table_data->dynamo_hd_->keyspace_ + '.' +
                         kv_index_name);
        AttributeDefinition partition_key, sort_key;
        partition_key.WithAttributeName(dynamo_partition_key_attribute_name)
            .WithAttributeType(ScalarAttributeType::N);
        req.AddAttributeDefinitions(std::move(partition_key));
        sort_key.WithAttributeName(dynamo_sort_key_attribute_name)
            .WithAttributeType(ScalarAttributeType::B);
        req.AddAttributeDefinitions(std::move(sort_key));

        KeySchemaElement pk_schema, sk_schema;
        pk_schema.WithAttributeName(dynamo_partition_key_attribute_name)
            .WithKeyType(Aws::DynamoDB::Model::KeyType::HASH);
        req.AddKeySchema(std::move(pk_schema));
        sk_schema.WithAttributeName(dynamo_sort_key_attribute_name)
            .WithKeyType(Aws::DynamoDB::Model::KeyType::RANGE);
        req.AddKeySchema(std::move(sk_schema));
        req.SetBillingMode(BillingMode::PAY_PER_REQUEST);

        table_data->dynamo_hd_->client_->CreateTableAsync(
            req, OnCreateDynamoSkTable, table_data);
        break;
    }
    default:
        LOG(INFO) << "Unsupported command for DynamoHandler::UpsertSkTable.";
        break;
    }
}

void EloqDS::DynamoHandler::OnCreateDynamoSkTable(
    const Aws::DynamoDB::DynamoDBClient *client,
    const CreateTableRequest &request,
    const CreateTableOutcome &result,
    const std::shared_ptr<const Aws::Client::AsyncCallerContext> &context)
{
    const std::shared_ptr<const UpsertTableData> table_data =
        std::dynamic_pointer_cast<const UpsertTableData>(context);
    assert(table_data->op_type_ == txservice::OperationType::CreateTable ||
           table_data->op_type_ == txservice::OperationType::AddIndex);
    if (!result.IsSuccess() &&
        result.GetError().GetErrorType() !=
            Aws::DynamoDB::DynamoDBErrors::RESOURCE_IN_USE)
    {
        LOG(INFO) << "create sk table failed: "
                  << result.GetError().GetExceptionName() << " "
                  << result.GetError().GetMessage();
        table_data->hd_res_->SetError(CcErrorCode::DATA_STORE_ERR);
        return;
    }
    if (table_data->op_type_ == txservice::OperationType::CreateTable)
    {
        const std::shared_ptr<const UpsertTableData> new_data =
            std::make_shared<const UpsertTableData>(
                *table_data, std::next(table_data->indexes_it_));
        // const auto *mysql_table_shema=
        //     static_cast<const MysqlTableSchema *>(table_data->table_schema_);
        const std::unordered_map<
            uint,
            std::pair<txservice::TableName, txservice::SecondaryKeySchema>> *
            indexes = DynamoHandlerTyped::GetIndexes(table_data->table_schema_);
        // mysql_table_shema->GetIndexes();

        if (new_data->indexes_it_ != indexes->cend())
        {
            UpsertSkTable(new_data);
        }
        else
        {
            UpsertCatalog(new_data);
        }
    }
    else
    {
        // create index kv table
        const std::shared_ptr<const UpsertTableData> new_data =
            std::make_shared<const UpsertTableData>(
                *table_data, std::next(table_data->add_indexes_it_));
        if (new_data->add_indexes_it_ !=
            new_data->alter_table_info_->index_add_names_.cend())
        {
            UpsertSkTable(new_data);
        }
        else
        {
            UpsertCatalog(new_data);
        }
    }
}

void EloqDS::DynamoHandler::OnDeleteDynamoSkTable(
    const Aws::DynamoDB::DynamoDBClient *client,
    const DeleteTableRequest &request,
    const DeleteTableOutcome &result,
    const std::shared_ptr<const Aws::Client::AsyncCallerContext> &context)
{
    const std::shared_ptr<const UpsertTableData> table_data =
        std::dynamic_pointer_cast<const UpsertTableData>(context);
    assert(table_data->op_type_ == txservice::OperationType::DropTable ||
           table_data->op_type_ == txservice::OperationType::DropIndex);
    if (!result.IsSuccess() &&
        result.GetError().GetErrorType() !=
            Aws::DynamoDB::DynamoDBErrors::RESOURCE_NOT_FOUND)
    {
        LOG(INFO) << "drop sk table failed: "
                  << result.GetError().GetExceptionName() << " "
                  << result.GetError().GetMessage();
        return;
    }
    if (table_data->op_type_ == txservice::OperationType::DropTable)
    {
        const std::shared_ptr<const UpsertTableData> new_data =
            std::make_shared<const UpsertTableData>(
                *table_data, std::next(table_data->indexes_it_));
        // const auto *mysql_table_shema=
        //     static_cast<const MysqlTableSchema *>(table_data->table_schema_);
        const std::unordered_map<
            uint,
            std::pair<txservice::TableName, txservice::SecondaryKeySchema>> *
            indexes = DynamoHandlerTyped::GetIndexes(table_data->table_schema_);
        // mysql_table_shema->GetIndexes();

        if (new_data->indexes_it_ != indexes->cend())
        {
            UpsertSkTable(new_data);
        }
        else
        {
            UpsertCatalog(new_data);
        }
    }
    else
    {
        // drop index kv table
        const std::shared_ptr<const UpsertTableData> new_data =
            std::make_shared<const UpsertTableData>(
                *table_data, std::next(table_data->drop_indexes_it_), false);
        if (new_data->drop_indexes_it_ !=
            new_data->alter_table_info_->index_drop_names_.cend())
        {
            UpsertSkTable(new_data);
        }
        else
        {
            UpsertCatalog(new_data);
        }
    }
}

void EloqDS::DynamoHandler::UpsertTableStatistics(
    const std::shared_ptr<const UpsertTableData> table_data)
{
    // Running in dynamo thread, which is blockable.

    if (table_data->op_type_ == txservice::OperationType::DropTable)
    {
        {
            const DynamoCatalog &dynamo_catalog =
                dynamo_sys_tables.at(dynamo_table_statistics_version_name);

            DeleteItemRequest req;
            req.SetTableName(table_data->dynamo_hd_->keyspace_ + "." +
                             dynamo_table_statistics_version_name);
            req.AddKey(
                dynamo_catalog.partition_key_,
                AttributeValue(table_data->table_name_->StringView().data()));
            DeleteItemOutcome outcome =
                table_data->dynamo_hd_->client_->DeleteItem(req);
            if (!outcome.IsSuccess())
            {
                LOG(ERROR) << "Delete table_statistics_version failed: "
                           << outcome.GetError().GetMessage();
                table_data->hd_res_->SetError(CcErrorCode::DATA_STORE_ERR);
                return;
            }
        }

        {
            const DynamoCatalog &dynamo_catalog =
                dynamo_sys_tables.at(dynamo_table_statistics_name);

            QueryRequest req;
            req.SetConsistentRead(true);
            req.SetTableName(table_data->dynamo_hd_->keyspace_ + "." +
                             dynamo_table_statistics_name);
            req.SetKeyConditionExpression("#pk = :pk");
            req.SetProjectionExpression("#pk, #sk");
            req.AddExpressionAttributeNames("#pk",
                                            dynamo_catalog.partition_key_);
            req.AddExpressionAttributeValues(
                ":pk", AttributeValue(table_data->table_name_->String()));
            req.AddExpressionAttributeNames("#sk", dynamo_catalog.sort_key_);

            Aws::Map<Aws::String, AttributeValue> last_key;
            do
            {
                if (!last_key.empty())
                {
                    req.SetExclusiveStartKey(last_key);
                }
                QueryOutcome outcome =
                    table_data->dynamo_hd_->client_->Query(req);
                if (!outcome.IsSuccess())
                {
                    LOG(ERROR) << "Query table_statistics failed: "
                               << outcome.GetError().GetMessage();
                    table_data->hd_res_->SetError(CcErrorCode::DATA_STORE_ERR);
                    return;
                }

                const Aws::Vector<Aws::Map<Aws::String, AttributeValue>>
                    &items = outcome.GetResult().GetItems();
                for (const Aws::Map<Aws::String, AttributeValue> &item : items)
                {
                    DeleteItemRequest delete_req;
                    delete_req.SetTableName(table_data->dynamo_hd_->keyspace_ +
                                            "." + dynamo_table_statistics_name);
                    delete_req.SetKey(item);
                    DeleteItemOutcome outcome =
                        table_data->dynamo_hd_->client_->DeleteItem(delete_req);
                    if (!outcome.IsSuccess())
                    {
                        LOG(ERROR) << "Delete table_statistics failed: "
                                   << outcome.GetError().GetMessage();
                        table_data->hd_res_->SetError(
                            CcErrorCode::DATA_STORE_ERR);
                        return;
                    }
                }

                last_key = outcome.GetResult().GetLastEvaluatedKey();
            } while (!last_key.empty());
        }
    }

    if (table_data->op_type_ == txservice::OperationType::CreateTable ||
        table_data->op_type_ == txservice::OperationType::AddIndex ||
        table_data->op_type_ == txservice::OperationType::DropIndex)
    {
        // For CREATE TABLE, will write the initial sequence record into the
        // sequence ccmap, and the record will be flush into the data store
        // during normal checkpoint, so there is no need to insert the initial
        // sequence record into data store here.
#ifdef RANGE_PARTITION_ENABLED
        SetupRange(table_data);
#else
        table_data->hd_res_->SetFinished();
#endif
    }
    else if (table_data->op_type_ == txservice::OperationType::DropTable)
    {
        // It doesn't matter, even if the table doesn't have an auto_increment
        // field.
        UpsertSequence(table_data);
    }
    else
    {
        assert(false);
    }
}

void EloqDS::DynamoHandler::UpsertSequence(
    const std::shared_ptr<const UpsertTableData> table_data)
{
    // const DynamoCatalogInfo *kv_info= static_cast<const DynamoCatalogInfo *>(
    //     Sequences::GetTableSchema()->GetKVCatalogInfo());
    const DynamoCatalogInfo *kv_info = static_cast<const DynamoCatalogInfo *>(
        DynamoHandlerTyped::GetSequenceTableSchema()->GetKVCatalogInfo());
    const std::string &kv_table_name = kv_info->kv_table_name_;

    // Assign fixed partition id for sequences
    int32_t pk1 = 0;

    if (table_data->op_type_ == txservice::OperationType::DropTable)
    {
        DeleteItemRequest req;
        AttributeValue pk, sk;

        req.SetTableName(table_data->dynamo_hd_->keyspace_ + '.' +
                         kv_table_name);
        // pk.SetN(static_cast<int>(
        //     Sequences::GenHashPk1(table_data->table_name_->String())));
        pk.SetN(pk1);
        req.AddKey(dynamo_partition_key_attribute_name, pk);
        sk.SetB(Aws::Utils::ByteBuffer(
            reinterpret_cast<const unsigned char *>(
                table_data->table_name_->StringView().data()),
            table_data->table_name_->StringView().length()));
        req.AddKey(dynamo_sort_key_attribute_name, std::move(sk));
        table_data->dynamo_hd_->client_->DeleteItemAsync(
            req, OnDeleteSequence, table_data);
    }
    else
    {
        assert(false);
    }
}

void EloqDS::DynamoHandler::OnPutSequence(
    const Aws::DynamoDB::DynamoDBClient *client,
    const PutItemRequest &request,
    const PutItemOutcome &result,
    const std::shared_ptr<const Aws::Client::AsyncCallerContext> &context)
{
    const std::shared_ptr<const UpsertTableData> table_data =
        std::dynamic_pointer_cast<const UpsertTableData>(context);
    assert(table_data->op_type_ != txservice::OperationType::DropTable);
    if (!result.IsSuccess())
    {
        LOG(ERROR) << "OnPutSequence failed: "
                   << result.GetError().GetExceptionName() << " "
                   << result.GetError().GetMessage();
        table_data->hd_res_->SetError(CcErrorCode::DATA_STORE_ERR);
        return;
    }
#ifdef RANGE_PARTITION_ENABLED
    SetupRange(table_data);
#else
    table_data->hd_res_->SetFinished();
#endif
}

void EloqDS::DynamoHandler::OnDeleteSequence(
    const Aws::DynamoDB::DynamoDBClient *client,
    const DeleteItemRequest &request,
    const DeleteItemOutcome &result,
    const std::shared_ptr<const Aws::Client::AsyncCallerContext> &context)
{
    const std::shared_ptr<const UpsertTableData> table_data =
        std::dynamic_pointer_cast<const UpsertTableData>(context);
    assert(table_data->op_type_ == txservice::OperationType::DropTable);
    if (!result.IsSuccess() &&
        result.GetError().GetErrorType() !=
            Aws::DynamoDB::DynamoDBErrors::RESOURCE_NOT_FOUND)
    {
        LOG(ERROR) << "OnDeleteSequence failed: "
                   << result.GetError().GetExceptionName() << " "
                   << result.GetError().GetMessage();
        table_data->hd_res_->SetError(CcErrorCode::DATA_STORE_ERR);
        return;
    }
#ifdef RANGE_PARTITION_ENABLED
    SetupRange(table_data);
#else
    table_data->hd_res_->SetFinished();
#endif
}

void EloqDS::DynamoHandler::SetupRange(
    const std::shared_ptr<const UpsertTableData> table_data)
{
    if (table_data->op_type_ == OperationType::CreateTable ||
        table_data->op_type_ == OperationType::DropTable)
    {
        if (!table_data->dynamo_hd_->UpsertRangeInfo(
                table_data, *table_data->table_name_) ||
            !table_data->dynamo_hd_->UpsertLastRangeId(
                table_data, *table_data->table_name_))
        {
            DLOG(ERROR) << "SetupRange failed";
            table_data->hd_res_->SetError(CcErrorCode::DATA_STORE_ERR);
            return;
        }
        if (table_data->table_schema_->IndexesSize() > 0)
        {
            auto sk_names = table_data->table_schema_->IndexNames();
            for (auto &sk : sk_names)
            {
                if (!table_data->dynamo_hd_->UpsertRangeInfo(table_data, sk) ||
                    !table_data->dynamo_hd_->UpsertLastRangeId(table_data, sk))
                {
                    DLOG(ERROR) << "SetupRange failed on sk";
                    table_data->hd_res_->SetError(CcErrorCode::DATA_STORE_ERR);
                    return;
                }
            }
        }
    }
    else if (table_data->op_type_ == OperationType::AddIndex ||
             table_data->op_type_ == OperationType::DropIndex)
    {
        const auto &sk_names =
            table_data->op_type_ == OperationType::AddIndex
                ? table_data->alter_table_info_->index_add_names_
                : table_data->alter_table_info_->index_drop_names_;
        assert(sk_names.size() > 0);
        for (auto &sk : sk_names)
        {
            if (!table_data->dynamo_hd_->UpsertRangeInfo(table_data,
                                                         sk.first) ||
                !table_data->dynamo_hd_->UpsertLastRangeId(table_data,
                                                           sk.first))
            {
                table_data->hd_res_->SetError(CcErrorCode::DATA_STORE_ERR);
                return;
            }
        }
    }
    else
    {
        // Only primary and secondry index table have range table.
        assert(false);
    }
    table_data->hd_res_->SetFinished();
}

bool EloqDS::DynamoHandler::UpsertLastRangeId(
    std::shared_ptr<const UpsertTableData> table_data,
    const TableName &table_name)
{
    if (table_data->op_type_ == OperationType::DropTable ||
        table_data->op_type_ == OperationType::DropIndex)
    {
        DeleteItemRequest del_req;
        del_req.SetTableName(keyspace_ + '.' + dynamo_last_range_id_name);
        del_req.AddKey("tablename", AttributeValue(table_name.String()));
        auto outcome = client_->DeleteItem(std::move(del_req));
        if (!outcome.IsSuccess())
        {
            LOG(ERROR) << "Delete last partition id for " << table_name.String()
                       << " failed. " << outcome.GetError().GetMessage();
            return false;
        }
        return true;
    }
    else if (table_data->op_type_ == OperationType::CreateTable ||
             table_data->op_type_ == OperationType::AddIndex)
    {
        PutItemRequest put_req;
        AttributeValue range_id;
        // if (table_name.StringView() == Sequences::mysql_seq_string)
        if (table_name.StringView() == DynamoHandlerTyped::mysql_seq_string_)
        {
            range_id.SetN(0);
        }
        else
        {
            range_id.SetN(
                Partition::InitialPartitionId(table_name.StringView()));
        }
        put_req.SetTableName(keyspace_ + '.' + dynamo_last_range_id_name);
        put_req.AddItem("tablename", AttributeValue(table_name.String()));
        put_req.AddItem("last_partition_id", std::move(range_id));
        auto outcome = client_->PutItem(std::move(put_req));
        if (!outcome.IsSuccess())
        {
            LOG(ERROR) << "Insert last partition id for " << table_name.String()
                       << " failed. " << outcome.GetError().GetMessage();
            return false;
        }
        return true;
    }

    return false;
}

bool EloqDS::DynamoHandler::UpsertRangeInfo(
    std::shared_ptr<const UpsertTableData> table_data,
    const TableName &table_name)
{
    if (table_data->op_type_ == OperationType::DropTable ||
        table_data->op_type_ == OperationType::DropIndex)
    {
        // Query for the range keys of this table.
        QueryRequest query_req;
        query_req.SetTableName(keyspace_ + '.' + dynamo_range_table_name);
        query_req.SetConsistentRead(true);
        query_req.SetProjectionExpression("start_key");
        query_req.SetKeyConditionExpression("tablename = :pk");
        query_req.AddExpressionAttributeValues(
            ":pk", AttributeValue(table_name.String()));
        while (1)
        {
            const auto &query_outcome = client_->Query(query_req);

            if (!query_outcome.IsSuccess())
            {
                LOG(ERROR) << "Query for table range info of "
                           << table_name.String() << " failed. "
                           << query_outcome.GetError().GetMessage();
                return false;
            }

            std::vector<WriteRequest> write_reqs;
            write_reqs.reserve(dynamo_batch_size);
            std::vector<BatchWriteItemOutcomeCallable> batch_futures;
            batch_futures.reserve(dynamo_max_futures);
            auto &items = query_outcome.GetResult().GetItems();
            for (auto &item : items)
            {
                if (write_reqs.size() == dynamo_batch_size)
                {
                    BatchWriteItemRequest batch_req;
                    batch_req.AddRequestItems(
                        keyspace_ + '.' + dynamo_range_table_name,
                        std::move(write_reqs));
                    batch_futures.push_back(
                        client_->BatchWriteItemCallable(std::move(batch_req)));

                    write_reqs.clear();
                }
                if (batch_futures.size() == dynamo_max_futures)
                {
                    for (auto &future : batch_futures)
                    {
                        const auto &write_res = future.get();
                        if (!write_res.IsSuccess())
                        {
                            LOG(ERROR) << "Delete table range info for "
                                       << table_name.String() << " failed. "
                                       << write_res.GetError().GetMessage();
                            return false;
                        }
                        if (write_res.GetResult().GetUnprocessedItems().size() >
                                0 &&
                            !RetryUnprocessedItems(write_res))
                        {
                            LOG(ERROR) << "Delete table range info for "
                                       << table_name.String() << " failed.";
                        }
                    }
                    batch_futures.clear();
                }
                WriteRequest write_req;
                DeleteRequest del_req;
                del_req.AddKey("tablename",
                               AttributeValue(table_name.String()));
                del_req.AddKey("start_key", item.at(std::string("start_key")));
                write_req.SetDeleteRequest(std::move(del_req));
                write_reqs.push_back(std::move(write_req));
            }
            if (write_reqs.size())
            {
                BatchWriteItemRequest batch_req;
                batch_req.AddRequestItems(
                    keyspace_ + '.' + dynamo_range_table_name,
                    std::move(write_reqs));
                batch_futures.push_back(
                    client_->BatchWriteItemCallable(std::move(batch_req)));
            }
            for (auto &future : batch_futures)
            {
                const auto &write_res = future.get();
                if (!write_res.IsSuccess())
                {
                    LOG(ERROR)
                        << "Delete table range info for " << table_name.String()
                        << " failed. " << write_res.GetError().GetMessage();
                    return false;
                }
                if (write_res.GetResult().GetUnprocessedItems().size() > 0 &&
                    !RetryUnprocessedItems(write_res))
                {
                    LOG(ERROR) << "Delete table range info for "
                               << table_name.String() << " failed.";
                }
            }
            const auto &last_key =
                query_outcome.GetResult().GetLastEvaluatedKey();
            if (last_key.size() == 0)
            {
                return true;
            }
            // Keep querying if there are more ranges not returned.
            query_req.SetExclusiveStartKey(std::move(last_key));
        }
    }
    else if (table_data->op_type_ == OperationType::CreateTable ||
             table_data->op_type_ == OperationType::AddIndex)
    {
        PutItemRequest put_req;
        AttributeValue range_id, range_key, version;
        // if (table_name.StringView() == Sequences::mysql_seq_string)
        if (table_name.StringView() == DynamoHandlerTyped::mysql_seq_string_)
        {
            range_id.SetN(0);
        }
        else
        {
            range_id.SetN(
                Partition::InitialPartitionId(table_name.StringView()));
        }
        range_key.SetB(Aws::Utils::ByteBuffer(
            &(DynamoHandlerTyped::neg_inf_packed_key_), 1));

        version.SetN(std::to_string(table_data->table_schema_->Version()));

        put_req.SetTableName(keyspace_ + '.' + dynamo_range_table_name);
        put_req.AddItem("tablename", AttributeValue(table_name.String()));
        put_req.AddItem("start_key", std::move(range_key));
        put_req.AddItem("partition_id", std::move(range_id));
        put_req.AddItem("version", std::move(version));
        auto outcome = client_->PutItem(std::move(put_req));
        if (!outcome.IsSuccess())
        {
            LOG(ERROR) << "Insert table range info for " << table_name.String()
                       << " failed. " << outcome.GetError().GetMessage();
            return false;
        }
        return true;
    }

    return false;
}

/*
 * @brief: Fetch record schema and commit_ts from table catalog
 * asynchronously, result will be put in fetch_req.
 */
void EloqDS::DynamoHandler::FetchTableCatalog(
    const txservice::TableName &ccm_table_name, FetchCatalogCc *fetch_cc)
{
    GetItemRequest req;
    req.SetTableName(keyspace_ + '.' + dynamo_table_catalog_name);
    req.AddKey("tablename", AttributeValue(ccm_table_name.String()));
    req.SetConsistentRead(true);

    client_->GetItemAsync(
        req,
        OnFetchCatalog,
        std::make_shared<const GeneralAsyncContext>(fetch_cc));
}

void EloqDS::DynamoHandler::OnFetchCatalog(
    const Aws::DynamoDB::DynamoDBClient *client,
    const GetItemRequest &request,
    const GetItemOutcome &result,
    const std::shared_ptr<const Aws::Client::AsyncCallerContext> &context)
{
    const std::shared_ptr<const GeneralAsyncContext> fetch_data =
        std::dynamic_pointer_cast<const GeneralAsyncContext>(context);
    FetchCatalogCc *fetch_cc = static_cast<FetchCatalogCc *>(fetch_data->data_);

    if (!result.IsSuccess())
    {
        LOG(ERROR) << "Fetch catalog failed: "
                   << result.GetError().GetMessage();
        fetch_cc->SetFinish(txservice::RecordStatus::Unknown,
                            static_cast<int>(CcErrorCode::DATA_STORE_ERR));
        return;
    }

    const Aws::Map<Aws::String, AttributeValue> &item =
        result.GetResult().GetItem();
    if (item.size() > 0)
    {
        std::string &image = fetch_cc->CatalogImage();
        uint64_t &version = fetch_cc->CommitTs();
        std::string frm;

        version = std::stoull(item.at("version").GetS());
        auto key_schemas_ts = item.at("keyschemasts").GetS();
        auto &kv_table_name = item.at("kvtablename").GetS();

        // image.append(SerializeSchemaImage("", "", key_schemas_ts));
        image.append(kv_table_name);
        fetch_cc->SetFinish(txservice::RecordStatus::Normal, 0);
    }
    else
    {
        fetch_cc->CatalogImage().clear();
        fetch_cc->SetFinish(txservice::RecordStatus::Deleted, 0);
    }
}

void EloqDS::DynamoHandler::FetchCurrentTableStatistics(
    const txservice::TableName &ccm_table_name,
    FetchTableStatisticsCc *fetch_cc)
{
    fetch_cc->SetStoreHandler(this);

    const DynamoCatalog &dynamo_catalog =
        dynamo_sys_tables.at(dynamo_table_statistics_version_name);

    GetItemRequest req;
    req.SetTableName(keyspace_ + '.' + dynamo_table_statistics_version_name);
    req.AddKey(dynamo_catalog.partition_key_,
               AttributeValue(ccm_table_name.String()));
    req.SetConsistentRead(true);

    client_->GetItemAsync(
        req,
        OnFetchCurrentTableStatistics,
        std::make_shared<const GeneralAsyncContext>(fetch_cc));
}

void EloqDS::DynamoHandler::OnFetchCurrentTableStatistics(
    const Aws::DynamoDB::DynamoDBClient *client,
    const GetItemRequest &request,
    const GetItemOutcome &result,
    const std::shared_ptr<const Aws::Client::AsyncCallerContext> &context)
{
    const std::shared_ptr<const GeneralAsyncContext> fetch_data =
        std::dynamic_pointer_cast<const GeneralAsyncContext>(context);
    FetchTableStatisticsCc *fetch_cc =
        static_cast<FetchTableStatisticsCc *>(fetch_data->data_);

    if (!result.IsSuccess())
    {
        LOG(ERROR) << "Fetch current table statistics failed: "
                   << result.GetError().GetMessage();
        fetch_cc->SetFinish(
            static_cast<int>(txservice::CcErrorCode::DATA_STORE_ERR));
        return;
    }

    const Aws::Map<Aws::String, AttributeValue> &item =
        result.GetResult().GetItem();
    if (item.size() > 0)
    {
        int64_t version = std::stoll(item.at("version").GetN());
        assert(version > 0);
        fetch_cc->SetCurrentVersion(static_cast<uint64_t>(version));
        fetch_cc->StoreHandler()->FetchTableStatistics(fetch_cc->CatalogName(),
                                                       fetch_cc);
    }
    else
    {
        // empty statistics
        fetch_cc->SetFinish(0);
    }
}

void EloqDS::DynamoHandler::FetchTableStatistics(
    const txservice::TableName &ccm_table_name,
    FetchTableStatisticsCc *fetch_cc)
{
    const DynamoCatalog &dynamo_catalog =
        dynamo_sys_tables.at(dynamo_table_statistics_name);

    QueryRequest req;
    req.SetConsistentRead(true);
    req.SetTableName(keyspace_ + '.' + dynamo_table_statistics_name);
    req.SetKeyConditionExpression("#pk = :pk AND begins_with(#sk, :version)");
    req.AddExpressionAttributeNames("#pk", dynamo_catalog.partition_key_);
    req.AddExpressionAttributeValues(":pk",
                                     AttributeValue(ccm_table_name.String()));
    req.AddExpressionAttributeNames("#sk", dynamo_catalog.sort_key_);
    AttributeValue version_val;
    req.AddExpressionAttributeValues(
        ":version",
        AttributeValue(std::to_string(fetch_cc->CurrentVersion()).append(":")));
    client_->QueryAsync(req,
                        OnFetchTableStatistics,
                        std::make_shared<const GeneralAsyncContext>(fetch_cc));
}

void EloqDS::DynamoHandler::OnFetchTableStatistics(
    const Aws::DynamoDB::DynamoDBClient *client,
    const QueryRequest &request,
    const Aws::DynamoDB::Model::QueryOutcome &result,
    const std::shared_ptr<const Aws::Client::AsyncCallerContext> &context)
{
    const std::shared_ptr<const GeneralAsyncContext> fetch_data =
        std::dynamic_pointer_cast<const GeneralAsyncContext>(context);
    FetchTableStatisticsCc *fetch_cc =
        static_cast<FetchTableStatisticsCc *>(fetch_data->data_);

    if (!result.IsSuccess())
    {
        LOG(ERROR) << "Fetch table statistics failed: "
                   << result.GetError().GetMessage();
        fetch_cc->SetFinish(
            static_cast<int>(txservice::CcErrorCode::DATA_STORE_ERR));
        return;
    }

    const DynamoCatalog &dynamo_catalog =
        dynamo_sys_tables.at(dynamo_table_statistics_name);

    const Aws::Vector<Aws::Map<Aws::String, AttributeValue>> &items =
        result.GetResult().GetItems();
    for (const Aws::Map<Aws::String, AttributeValue> &item : items)
    {
        Aws::String sort_key = item.at(dynamo_catalog.sort_key_).GetS();
        int8_t indextype_i8 = 0;
        std::string indexname_str;
        butil::StringSplitter sp(sort_key, ':');
        {
            ++sp;  // skip version
            int ret = sp.to_int8(&indextype_i8);
            assert(ret == 0);
            ++sp;
            indexname_str.assign(sp.field(), sp.length());
        }

        TableName indexname(std::move(indexname_str),
                            static_cast<TableType>(indextype_i8));

        int64_t records_i64 = std::stoll(item.at("records").GetN());
        if (records_i64 >= 0)
        {
            uint64_t records = static_cast<uint64_t>(records_i64);
            fetch_cc->SetRecords(indexname, records);
        }

        std::vector<TxKey> samplekeys;
        Aws::Vector<Aws::Utils::ByteBuffer> byte_buffer_vec =
            item.at("samplekeys").GetBS();
        for (const Aws::Utils::ByteBuffer &byte_buffer : byte_buffer_vec)
        {
            TxKey samplekey = TxKey(std::make_unique<EloqKV::EloqKey>(
                (const char *) byte_buffer.GetUnderlyingData(),
                byte_buffer.GetLength()));
            samplekeys.emplace_back(std::move(samplekey));
        }

        fetch_cc->SamplePoolMergeFrom(indexname, std::move(samplekeys));
    }

    const auto &last_key = result.GetResult().GetLastEvaluatedKey();
    if (last_key.empty())
    {
        fetch_cc->SetFinish(0);
    }
    else
    {
        QueryRequest new_req = request;
        new_req.SetExclusiveStartKey(std::move(last_key));
        client->QueryAsync(std::move(new_req), OnFetchTableStatistics, context);
    }
}

bool EloqDS::DynamoHandler::UpsertTableStatistics(
    const txservice::TableName &ccm_table_name,
    const std::unordered_map<TableName,
                             std::pair<uint64_t, std::vector<txservice::TxKey>>>
        &sample_pool_map,
    uint64_t version)
{
    struct SortKey
    {
        std::string ToString()
        {
            Aws::StringStream sort_key_ss;

            // Cast tabletype to int16_t, because (u)char is not treated as a
            // number by stream.
            sort_key_ss << version_ << ':'
                        << static_cast<int16_t>(indexname_->Type()) << ':'
                        << indexname_->StringView() << ':' << segment_id_;

            return sort_key_ss.str();
        }

        int64_t version_{-1};
        const TableName *indexname_{nullptr};
        int64_t segment_id_{-1};
    };

    {
        const DynamoCatalog &dynamo_catalog =
            dynamo_sys_tables.at(dynamo_table_statistics_name);

        PutItemRequest base_req;
        base_req.SetTableName(keyspace_ + "." + dynamo_table_statistics_name);
        base_req.AddItem(dynamo_catalog.partition_key_,
                         AttributeValue(ccm_table_name.StringView().data()));

        SortKey sort_key;
        sort_key.version_ = version;

        for (const auto &[indexname, sample_pool] : sample_pool_map)
        {
            sort_key.indexname_ = &indexname;

            std::unique_ptr<AttributeValue> samplekeys_val =
                std::make_unique<AttributeValue>();

            uint32_t segment_id = 0;
            uint32_t segment_size = 0;
            size_t sz = sample_pool.second.size();
            for (size_t i = 0; i < sz; ++i)
            {
                const txservice::TxKey &samplekey = sample_pool.second[i];
                const EloqKV::EloqKey *redis_key =
                    samplekey.GetKey<EloqKV::EloqKey>();

                if (segment_size + redis_key->Size() >= row_max_size_)
                {
                    PutItemRequest req(base_req);
                    sort_key.segment_id_ = segment_id;
                    req.AddItem(dynamo_catalog.sort_key_,
                                AttributeValue(sort_key.ToString()));

                    // set records to -1 means uncomplete write.
                    AttributeValue records_val;
                    records_val.SetN(std::to_string(static_cast<int64_t>(-1)));
                    req.AddItem("records", std::move(records_val));
                    req.AddItem("samplekeys", *samplekeys_val);

                    PutItemOutcome outcome = client_->PutItem(req);
                    if (!outcome.IsSuccess())
                    {
                        LOG(ERROR)
                            << "Insert table_statistics failed, "
                            << outcome.GetError().GetMessage()
                            << ", tablename: " << ccm_table_name.StringView();
                        return false;
                    }

                    samplekeys_val = std::make_unique<AttributeValue>();
                    segment_id += 1;
                    segment_size = 0;
                }

                segment_size += redis_key->Size();
                DynamoHandlerTyped::AddKeyBytesIntoCollection(*samplekeys_val,
                                                              *redis_key);

                if (i == sz - 1)
                {
                    PutItemRequest req(base_req);
                    sort_key.segment_id_ = segment_id;
                    req.AddItem(dynamo_catalog.sort_key_,
                                AttributeValue(sort_key.ToString()));
                    AttributeValue records_val;
                    records_val.SetN(std::to_string(
                        static_cast<int64_t>(sample_pool.first)));
                    req.AddItem("records", std::move(records_val));
                    req.AddItem("samplekeys", *samplekeys_val);

                    PutItemOutcome outcome = client_->PutItem(req);
                    if (!outcome.IsSuccess())
                    {
                        LOG(ERROR)
                            << "Insert table_statistics failed, "
                            << outcome.GetError().GetMessage()
                            << ", tablename: " << ccm_table_name.StringView();
                        return false;
                    }
                    samplekeys_val.reset(nullptr);
                }
            }
        }
    }

    {
        const DynamoCatalog &dynamo_catalog =
            dynamo_sys_tables.at(dynamo_table_statistics_version_name);

        PutItemRequest req;
        req.SetTableName(keyspace_ + "." +
                         dynamo_table_statistics_version_name);
        req.AddItem(dynamo_catalog.partition_key_,
                    AttributeValue(ccm_table_name.StringView().data()));
        AttributeValue version_val;
        version_val.SetN(std::to_string(static_cast<int64_t>(version)));
        req.AddItem("version", std::move(version_val));
        PutItemOutcome outcome = client_->PutItem(req);
        if (!outcome.IsSuccess())
        {
            LOG(ERROR) << "Update table_statistics_version failed, "
                       << "error message: " << outcome.GetError().GetMessage()
                       << ", tablename: " << ccm_table_name.StringView();
            return false;
        }
    }

    {
        const DynamoCatalog &dynamo_catalog =
            dynamo_sys_tables.at(dynamo_table_statistics_name);

        QueryRequest req;
        req.SetConsistentRead(true);
        req.SetTableName(keyspace_ + "." + dynamo_table_statistics_name);
        req.SetKeyConditionExpression("#pk = :pk");
        req.SetProjectionExpression("#pk, #sk");
        req.AddExpressionAttributeNames("#pk", dynamo_catalog.partition_key_);
        req.AddExpressionAttributeValues(
            ":pk", AttributeValue(ccm_table_name.StringView().data()));
        req.AddExpressionAttributeNames("#sk", dynamo_catalog.sort_key_);

        Aws::Map<Aws::String, AttributeValue> last_key;
        do
        {
            if (!last_key.empty())
            {
                req.SetExclusiveStartKey(std::move(last_key));
            }

            QueryOutcome outcome = client_->Query(req);
            if (!outcome.IsSuccess())
            {
                LOG(ERROR) << "Query table_statistics failed, "
                           << "error message: "
                           << outcome.GetError().GetMessage()
                           << ", tablename: " << ccm_table_name.StringView();
                return false;
            }

            const Aws::Vector<Aws::Map<Aws::String, AttributeValue>> &items =
                outcome.GetResult().GetItems();
            for (const Aws::Map<Aws::String, AttributeValue> &item : items)
            {
                Aws::String sort_key = item.at(dynamo_catalog.sort_key_).GetS();
                long long int version_ll = -1;
                {
                    butil::StringSplitter sp(sort_key, ':');
                    int ret = sp.to_longlong(&version_ll);
                    assert(!ret && version_ll >= 0);
                }

                if (static_cast<uint64_t>(version_ll) < version)
                {
                    DeleteItemRequest delete_req;
                    delete_req.SetTableName(
                        keyspace_ + "." + dynamo_table_statistics_name.data());
                    delete_req.SetKey(item);
                    DeleteItemOutcome outcome = client_->DeleteItem(delete_req);
                    if (!outcome.IsSuccess())
                    {
                        LOG(ERROR)
                            << "Delete table_statistics failed, "
                            << "error message: "
                            << outcome.GetError().GetMessage()
                            << ", tablename: " << ccm_table_name.StringView();
                        return false;
                    }
                }
            }

            last_key = outcome.GetResult().GetLastEvaluatedKey();
        } while (!last_key.empty());
    }

    return true;
}

void EloqDS::DynamoHandler::FetchTableRanges(FetchTableRangesCc *fetch_cc)
{
    LOG(ERROR) << "DynamoHandler::FetchTableRanges not implemented";
    assert(false);
}

void EloqDS::DynamoHandler::OnFetchTableRanges(
    const Aws::DynamoDB::DynamoDBClient *client,
    const Aws::DynamoDB::Model::QueryRequest &request,
    const Aws::DynamoDB::Model::QueryOutcome &result,
    const std::shared_ptr<const Aws::Client::AsyncCallerContext> &context)
{
    LOG(ERROR) << "DynamoHandler::OnFetchTableRanges not implemented";
    assert(false);
}

void EloqDS::DynamoHandler::FetchRangeSlices(FetchRangeSlicesReq *fetch_cc)
{
    LOG(ERROR) << "DynamoHandler::FetchRangeSlices not implemented";
    assert(false);
}

void EloqDS::DynamoHandler::OnFetchRangeSlices(
    const Aws::DynamoDB::DynamoDBClient *client,
    const Aws::DynamoDB::Model::GetItemRequest &request,
    const Aws::DynamoDB::Model::GetItemOutcome &result,
    const std::shared_ptr<const Aws::Client::AsyncCallerContext> &context)
{
    LOG(ERROR) << "DynamoHandler::OnFetchRangeSlices not implemented";
    assert(false);
}

bool EloqDS::DynamoHandler::Read(const txservice::TableName &table_name,
                                 const txservice::TxKey &key,
                                 txservice::TxRecord &rec,
                                 bool &found,
                                 uint64_t &version_ts,
                                 const txservice::TableSchema *table_schema)
{
    const std::string &kv_table_name =
        table_schema->GetKVCatalogInfo()->GetKvTableName(table_name);

    GetItemRequest req;

    // Set up the request
    req.SetTableName(keyspace_ + '.' + kv_table_name);
    req.SetConsistentRead(true);

    if (partition_finder == nullptr)
    {
        partition_finder = PartitionFinderFactory::Create();
    }

#ifdef RANGE_PARTITION_ENABLED
    dynamic_cast<RangePartitionFinder *>(partition_finder.get())
        ->Init(tx_service_, UINT32_MAX);
#endif

    Partition pk;
    PartitionResultType rt =
        partition_finder->FindPartition(table_name, key, pk);
    if (rt != PartitionResultType::NORMAL)
    {
        partition_finder->ReleaseReadLocks();
        return false;
    }
    int32_t pk1 = pk.Pk1();
    partition_finder->ReleaseReadLocks();

    // Bind the keys
    AttributeValue sort_key, partition_key;
    partition_key.SetN(pk1);
    // sort_key.SetB(std::move(packed_key));
    DynamoHandlerTyped::BindDynamoReqForKey(sort_key,
                                            *key.GetKey<EloqKV::EloqKey>());
    req.AddKey(dynamo_partition_key_attribute_name, std::move(partition_key));
    req.AddKey(dynamo_sort_key_attribute_name, std::move(sort_key));

    metrics::TimePoint start;
    if (metrics::enable_kv_metrics)
    {
        start = metrics::Clock().now();
    }

    const GetItemOutcome &result = client_->GetItem(req);

    if (metrics::enable_kv_metrics)
    {
        metrics::kv_meter->CollectDuration(metrics::NAME_KV_READ_DURATION,
                                           start);
    }

    if (result.IsSuccess())
    {
        // Reference the retrieved fields/values.
        const Aws::Map<Aws::String, AttributeValue> &row =
            result.GetResult().GetItem();
        if (row.size() > 0)
        {
            bool deleted = row.at("___deleted___").GetBool();
            version_ts = std::stoull(row.at("___version___").GetN());
            if (!deleted)
            {
                found = true;
                DynamoHandlerTyped::ParsePayloadFromDynamoRow(
                    row, &rec, table_schema->RecordSchema(), table_name);

                DynamoHandlerTyped::ParseUnpackInfoFromDynamoRow(&rec, row);
            }
            else
            {
                found = false;
            }
        }
        else
        {
            found = false;
        }
        return true;
    }
    return false;
}

#ifdef ON_KEY_OBJECT
txservice::store::DataStoreHandler::DataStoreOpStatus
EloqDS::DynamoHandler::FetchRecord(txservice::FetchRecordCc *fetch_cc)
{
    const txservice::TableName &table_name = *fetch_cc->table_name_;
    const txservice::TxKey &key = fetch_cc->tx_key_;
    assert(fetch_cc->table_schema_ != nullptr);
    const std::string &kv_table_name =
        fetch_cc->table_schema_->GetKVCatalogInfo()->GetKvTableName(table_name);

    GetItemRequest req;

    // Set up the request
    req.SetTableName(keyspace_ + '.' + kv_table_name);
    req.SetConsistentRead(true);

    if (partition_finder == nullptr)
    {
        partition_finder = PartitionFinderFactory::Create();
    }

#ifdef RANGE_PARTITION_ENABLED
    dynamic_cast<RangePartitionFinder *>(partition_finder.get())
        ->Init(tx_service_, node_group);
#endif

    Partition pk;
    PartitionResultType rt =
        partition_finder->FindPartition(table_name, key, pk);
    if (rt != PartitionResultType::NORMAL)
    {
        partition_finder->ReleaseReadLocks();
        LOG(ERROR) << "Fetch record failed on FindPartition, rt:" << (int) rt;
        fetch_cc->SetFinish(static_cast<int>(CcErrorCode::DATA_STORE_ERR));
        return DataStoreOpStatus::Error;
    }
    int32_t pk1 = pk.Pk1();
    partition_finder->ReleaseReadLocks();

    // Bind the keys
    AttributeValue sort_key, partition_key;
    partition_key.SetN(pk1);
    DynamoHandlerTyped::BindDynamoReqForKey(sort_key,
                                            *key.GetKey<EloqKV::EloqKey>());
    req.AddKey(dynamo_partition_key_attribute_name, std::move(partition_key));
    req.AddKey(dynamo_sort_key_attribute_name, std::move(sort_key));
    req.SetConsistentRead(true);
    if (metrics::enable_kv_metrics)
    {
        fetch_cc->start_ = metrics::Clock::now();
    }
    client_->GetItemAsync(
        req,
        OnFetchRecord,
        std::make_shared<const GeneralAsyncContext>(fetch_cc));
    return DataStoreOpStatus::Success;
}

void EloqDS::DynamoHandler::OnFetchRecord(
    const Aws::DynamoDB::DynamoDBClient *client,
    const GetItemRequest &request,
    const GetItemOutcome &result,
    const std::shared_ptr<const Aws::Client::AsyncCallerContext> &context)
{
    const std::shared_ptr<const GeneralAsyncContext> fetch_data =
        std::dynamic_pointer_cast<const GeneralAsyncContext>(context);
    FetchRecordCc *fetch_cc = static_cast<FetchRecordCc *>(fetch_data->data_);
    if (metrics::enable_kv_metrics)
    {
        metrics::kv_meter->CollectDuration(metrics::NAME_KV_READ_DURATION,
                                           fetch_cc->start_);
        metrics::kv_meter->Collect(metrics::NAME_KV_READ_TOTAL, 1);
    }
    if (!result.IsSuccess())
    {
        LOG(ERROR) << "FetchRecord failed: "
                   << result.GetError().GetExceptionName() << ", "
                   << result.GetError().GetMessage();
        fetch_cc->SetFinish(static_cast<int>(CcErrorCode::DATA_STORE_ERR));
        return;
    }
    else
    {
        // Reference the retrieved fields/values.
        const Aws::Map<Aws::String, AttributeValue> &row =
            result.GetResult().GetItem();
        if (row.size() > 0)
        {
            bool deleted = row.at("___deleted___").GetBool();
            if (!deleted)
            {
                fetch_cc->rec_status_ = RecordStatus::Normal;
                Aws::Utils::ByteBuffer payload_val =
                    row.at("___payload___").GetB();
                const char *payload_ptr = reinterpret_cast<const char *>(
                    payload_val.GetUnderlyingData());
                fetch_cc->rec_str_ =
                    std::string(payload_ptr, payload_val.GetLength());
                assert(!fetch_cc->rec_str_.empty());
            }
            else
            {
                fetch_cc->rec_status_ = RecordStatus::Deleted;
            }

            fetch_cc->rec_ts_ = std::stoull(row.at("___version___").GetN());
            fetch_cc->SetFinish(0);
        }
        else
        {
            fetch_cc->rec_ts_ = 1;
            fetch_cc->rec_status_ = RecordStatus::Deleted;
            fetch_cc->SetFinish(0);
        }
    }
}

#endif

std::unique_ptr<txservice::store::DataStoreScanner>
EloqDS::DynamoHandler::ScanForward(
    const txservice::TableName &table_name,
    uint32_t ng_id,
    const txservice::TxKey &start_key,
    bool inclusive,
    uint8_t key_parts,
    const std::vector<txservice::store::DataStoreSearchCond> &search_cond,
    const txservice::KeySchema *key_schema,
    const txservice::RecordSchema *rec_schema,
    const txservice::KVCatalogInfo *kv_info,
    bool scan_forward)
{
#ifdef RANGE_PARTITION_ENABLED
    return nullptr;
#else
    std::unique_ptr<DynamoScanner> scanner = nullptr;
    if (scan_forward)
    {
        scanner = std::make_unique<HashPartitionDynamoScanner<true>>(
            keyspace_,
            client_.get(),
            key_schema,
            rec_schema,
            table_name,
            kv_info,
            *start_key.GetKey<EloqKV::EloqKey>(),
            inclusive,
            search_cond);
    }
    else
    {
        scanner = std::make_unique<HashPartitionDynamoScanner<false>>(
            keyspace_,
            client_.get(),
            key_schema,
            rec_schema,
            table_name,
            kv_info,
            *start_key.GetKey<EloqKV::EloqKey>(),
            inclusive,
            search_cond);
    }

    scanner->MoveNext();
    return scanner;
#endif
}

bool EloqDS::DynamoHandler::FetchTable(const txservice::TableName &table_name,
                                       std::string &schema_image,
                                       bool &found,
                                       uint64_t &version_ts) const
{
    GetItemRequest req;

    // Set up the request.
    req.SetTableName(keyspace_ + '.' + dynamo_table_catalog_name);
    req.AddKey("tablename", AttributeValue(table_name.String()));
    req.SetConsistentRead(true);

    // Retrieve the item's fields and values
    const GetItemOutcome &result = client_->GetItem(req);
    if (result.IsSuccess())
    {
        // Reference the retrieved fields/values.
        const Aws::Map<Aws::String, AttributeValue> &item =
            result.GetResult().GetItem();
        if (item.size() > 0)
        {
            // Output each retrieved field and its value.
            found = true;
            std::string frm;
            DynamoCatalogInfo kv_info;
            Aws::Utils::ByteBuffer fetched_image = item.at("content").GetB();
            frm.append(reinterpret_cast<const char *>(
                           fetched_image.GetUnderlyingData()),
                       fetched_image.GetLength());
            version_ts = std::stoull(item.at("version").GetN());
            kv_info.kv_table_name_.append(item.at("kvtablename").GetS());
            const Aws::Map<Aws::String, const std::shared_ptr<AttributeValue>>
                index_names = item.at("kvindexname").GetM();
            for (auto index : index_names)
            {
                bool is_unique_sk = index.second->GetS().front() == 'u';
                txservice::TableName index_name(
                    index.first,
                    is_unique_sk ? txservice::TableType::UniqueSecondary
                                 : txservice::TableType::Secondary);
                kv_info.kv_index_names_.try_emplace(index_name,
                                                    index.second->GetS());
            }
            const Aws::Map<Aws::String, const std::shared_ptr<AttributeValue>>
                schemas_ts = item.at("keyschemasts").GetM();
            TableKeySchemaTs table_key_schemas_ts;
            for (auto schema_ts : schemas_ts)
            {
                txservice::TableType table_type;
                const Aws::String &name_str = schema_ts.first;
                if (name_str.find(txservice::UNIQUE_INDEX_NAME_PREFIX) !=
                    std::string::npos)
                {
                    table_type = txservice::TableType::UniqueSecondary;
                }
                else if (name_str.find(txservice::INDEX_NAME_PREFIX) !=
                         std::string::npos)
                {
                    table_type = txservice::TableType::Secondary;
                }
                else
                {
                    table_type = txservice::TableType::Primary;
                    table_key_schemas_ts.pk_schema_ts_ =
                        std::stoull(schema_ts.second->GetS());
                    continue;
                }
                txservice::TableName table_name(name_str, table_type);
                table_key_schemas_ts.sk_schemas_ts_.try_emplace(
                    std::move(table_name),
                    std::stoull(schema_ts.second->GetS()));
            }
            schema_image.append(SerializeSchemaImage(
                frm, kv_info.Serialize(), table_key_schemas_ts.Serialize()));
        }
        else
        {
            found = false;
        }
    }
    else
    {
        LOG(ERROR) << "Fetch table from dynamodb failed, "
                   << "error: " << result.GetError().GetExceptionName() << ","
                   << "error message: " << result.GetError().GetMessage()
                   << "table: " << table_name.StringView();
        return false;
    }
    return true;
}

bool EloqDS::DynamoHandler::DiscoverAllTableNames(
    std::vector<std::string> &norm_name_vec,
    const std::function<void()> *yield_fptr,
    const std::function<void()> *resume_fptr) const
{
    ScanRequest req;

    // Set up the request.
    req.SetTableName(keyspace_ + '.' + dynamo_table_catalog_name);
    req.SetProjectionExpression("tablename");
    req.SetConsistentRead(true);

    // Retrieve the item's fields and values
    ScanOutcome result = client_->Scan(req);
    while (result.IsSuccess())
    {
        // Reference the retrieved fields/values.
        const Aws::Vector<Aws::Map<Aws::String, AttributeValue>> &items =
            result.GetResult().GetItems();
        for (const auto &item : items)
        {
            // We only retrieved tablename col
            norm_name_vec.emplace_back(item.at("tablename").GetS().c_str());
        }

        // Check if there's any remaining data unfetched
        const Aws::Map<Aws::String, AttributeValue> &last_val =
            result.GetResult().GetLastEvaluatedKey();
        if (!last_val.size())
        {
            break;
        }
        req.SetExclusiveStartKey(std::move(last_val));
        result = client_->Scan(req);
    }
    if (!result.IsSuccess())
    {
        LOG(ERROR) << "Fetch all tables from dynamodb failed, "
                   << "error: " << result.GetError().GetExceptionName() << ","
                   << "error message: " << result.GetError().GetMessage();
        return false;
    }

    return true;
}

//-- database
bool EloqDS::DynamoHandler::UpsertDatabase(std::string_view db,
                                           std::string_view definition) const
{
    PutItemRequest putItemRequest;
    putItemRequest.SetTableName(keyspace_ + '.' + dynamo_database_catalog_name);

    AttributeValue def;
    Aws::Utils::ByteBuffer bytes(
        reinterpret_cast<const unsigned char *>(definition.data()),
        definition.size());
    def.SetB(bytes);

    // Add all AttributeValue objects.
    putItemRequest.AddItem("dbname", AttributeValue(db.data()));
    putItemRequest.AddItem("definition", std::move(def));

    const PutItemOutcome &result = client_->PutItem(putItemRequest);
    if (!result.IsSuccess())
    {
        LOG(ERROR) << "Upsert database from dynamodb failed, "
                   << "error: " << result.GetError().GetExceptionName() << ", "
                   << "error message: " << result.GetError().GetMessage()
                   << ", "
                   << "database: " << db;
        return false;
    }
    return true;
}

bool EloqDS::DynamoHandler::DropDatabase(std::string_view db) const
{
    DeleteItemRequest req;
    req.SetTableName(keyspace_ + '.' + dynamo_database_catalog_name);

    req.AddKey("dbname", AttributeValue(db.data()));
    const DeleteItemOutcome &result = client_->DeleteItem(req);
    if (!result.IsSuccess())
    {
        LOG(ERROR) << "Drop database from dynamodb failed, "
                   << "error: " << result.GetError().GetExceptionName() << ","
                   << "error message: " << result.GetError().GetMessage()
                   << "db: " << db;
        return false;
    }
    return true;
}
bool EloqDS::DynamoHandler::FetchDatabase(
    std::string_view db,
    std::string &definition,
    bool &found,
    const std::function<void()> *yield_fptr,
    const std::function<void()> *resume_fptr) const
{
    GetItemRequest req;

    // Set up the request.
    req.SetTableName(keyspace_ + '.' + dynamo_database_catalog_name);
    req.AddKey("dbname", AttributeValue(db.data()));
    req.SetConsistentRead(true);

    // Retrieve the item's fields and values
    const GetItemOutcome &result = client_->GetItem(req);
    if (result.IsSuccess())
    {
        // Reference the retrieved fields/values.
        const Aws::Map<Aws::String, AttributeValue> &item =
            result.GetResult().GetItem();
        if (item.size() > 0)
        {
            // Output each retrieved field and its value.
            found = true;
            const Aws::Utils::ByteBuffer &value = item.at("definition").GetB();
            definition.assign(
                reinterpret_cast<const char *>(value.GetUnderlyingData()),
                value.GetLength());
        }
        else
        {
            found = false;
        }
    }
    else
    {
        LOG(ERROR) << "Fetch database from dynamodb failed, "
                   << "error: " << result.GetError().GetExceptionName() << ","
                   << "error message: " << result.GetError().GetMessage() << ","
                   << "database: " << db;
        return false;
    }
    return true;
}
bool EloqDS::DynamoHandler::FetchAllDatabase(
    std::vector<std::string> &dbnames,
    const std::function<void()> *yield_fptr,
    const std::function<void()> *resume_fptr) const
{
    ScanRequest req;

    // Set up the request.
    req.SetTableName(keyspace_ + '.' + dynamo_database_catalog_name);
    req.SetProjectionExpression("dbname");
    req.SetConsistentRead(true);

    // Retrieve the item's fields and values
    ScanOutcome result = client_->Scan(req);
    while (result.IsSuccess())
    {
        // Reference the retrieved fields/values.
        const Aws::Vector<Aws::Map<Aws::String, AttributeValue>> &items =
            result.GetResult().GetItems();
        for (const auto &item : items)
        {
            // We only retrieved dbname col
            dbnames.emplace_back(item.at("dbname").GetS().c_str());
        }

        // Check if there's any remaining data unfetched
        const Aws::Map<Aws::String, AttributeValue> &last_val =
            result.GetResult().GetLastEvaluatedKey();
        if (!last_val.size())
        {
            break;
        }
        req.SetExclusiveStartKey(std::move(last_val));
        result = client_->Scan(req);
    }
    if (!result.IsSuccess())
    {
        LOG(ERROR) << "Fetch all databases from dynamodb failed, "
                   << "error: " << result.GetError().GetExceptionName() << ","
                   << "error message: " << result.GetError().GetMessage();
        return false;
    }
    return true;
}

bool EloqDS::DynamoHandler::DropKvTable(const std::string &kv_table_name) const
{
    DeleteTableRequest req;
    req.SetTableName(keyspace_ + "." + kv_table_name);
    DeleteTableOutcome outcome = client_->DeleteTable(req);
    bool ok = outcome.IsSuccess() ||
              outcome.GetError().GetErrorType() ==
                  Aws::DynamoDB::DynamoDBErrors::RESOURCE_NOT_FOUND;
    if (!ok)
    {
        LOG(ERROR) << "Drop kvtable failed, kvtablename: " << kv_table_name
                   << ", " << outcome.GetError().GetMessage();
    }

    return ok;
}

void EloqDS::DynamoHandler::DropKvTableAsync(
    const std::string &kv_table_name) const
{
    // No matter ddl_skip_kv and is_bootstrap is set, DynamoDB create table
    // always.
    DeleteTableRequest req;
    req.SetTableName(keyspace_ + "." + kv_table_name);
    client_->DeleteTableCallable(req);  // ignore error
}

bool EloqDS::DynamoHandler::DeleteOutOfRangeData(
    const txservice::TableName &table_name,
    int32_t partition_id,
    const txservice::TxKey *start_key,
    const txservice::TableSchema *table_schema)
{
    // Unlike CopyRangeData, all of the read and delete happens in the same
    // partition. So there's no need to launch worker thread since a single
    // thread is enough to consume all of the write capacity on one partition.
    const DynamoCatalogInfo *kv_info = static_cast<const DynamoCatalogInfo *>(
        table_schema->GetKVCatalogInfo());
    const std::string &kv_table_name =
        table_name.IsBase() ? kv_info->kv_table_name_
                            : kv_info->kv_index_names_.at(table_name);
    QueryRequest query_req;
    query_req.SetTableName(keyspace_ + '.' + kv_table_name);
    query_req.SetConsistentRead(true);
    query_req.SetKeyConditionExpression("#pk = :pk AND #sk >= :sk1");
    query_req.SetProjectionExpression("#sk");

    AttributeValue pk, sk1;
    pk.SetN(partition_id);
    const EloqKV::EloqKey *eloq_key = start_key->GetKey<EloqKV::EloqKey>();
    DynamoHandlerTyped::BindDynamoReqForKey(sk1, *eloq_key);

    // pk AttributeValue will be reused in BatchWriteItemRequest.
    query_req.AddExpressionAttributeValues(":pk", pk);
    query_req.AddExpressionAttributeValues(":sk1", std::move(sk1));
    query_req.AddExpressionAttributeNames("#pk",
                                          dynamo_partition_key_attribute_name);
    query_req.AddExpressionAttributeNames("#sk",
                                          dynamo_sort_key_attribute_name);
    while (1)
    {
        const auto &query_result = client_->Query(query_req);
        if (!query_result.IsSuccess())
        {
            LOG(INFO) << "Delete out of range query request failed: "
                      << query_result.GetError().GetMessage();
            return false;
        }

        auto &items =
            const_cast<Aws::Vector<Aws::Map<Aws::String, AttributeValue>> &>(
                query_result.GetResult().GetItems());
        size_t idx = 0;
        while (idx < items.size())
        {
            BatchWriteItemRequest batch_req;
            Aws::Vector<WriteRequest> write_reqs;
            for (; write_reqs.size() < dynamo_batch_size - 1 &&
                   idx < items.size();
                 ++idx)
            {
                WriteRequest write_req;
                DeleteRequest del_req;
                // Update the partition key of the read key and pass it into
                // PutRequest.
                auto &item = items.at(idx);
                del_req.AddKey(dynamo_partition_key_attribute_name, pk);
                del_req.AddKey(
                    dynamo_sort_key_attribute_name,
                    std::move(item.at(dynamo_sort_key_attribute_name)));
                write_req.SetDeleteRequest(std::move(del_req));
                write_reqs.push_back(std::move(write_req));
            }
            batch_req.AddRequestItems(keyspace_ + '.' + kv_table_name,
                                      std::move(write_reqs));
            const BatchWriteItemOutcome write_res =
                client_->BatchWriteItem(std::move(batch_req));
            if (!write_res.IsSuccess() && !write_res.GetError().ShouldRetry())
            {
                LOG(INFO) << "Delete out of range write request failed: "
                          << write_res.GetError().GetMessage();
                return false;
            }
            auto unprocessed = write_res.GetResult().GetUnprocessedItems();
            for (uint8_t retry = 0;
                 unprocessed.size() > 0 && retry < dynamo_api_retry;
                 retry++)
            {
                BatchWriteItemRequest batch_req;

                // Increase request interval exponentially to avoid throttling
                // error.
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(uint(pow(2, retry) * 100)));
                batch_req.SetRequestItems(unprocessed);
                const BatchWriteItemOutcome write_res =
                    client_->BatchWriteItem(std::move(batch_req));
                if (!write_res.IsSuccess() &&
                    !write_res.GetError().ShouldRetry())
                {
                    LOG(INFO) << "Delete out of range write request failed: "
                              << write_res.GetError().GetMessage();
                    return false;
                }
                unprocessed = write_res.GetResult().GetUnprocessedItems();
            }
            if (unprocessed.size() > 0)
            {
                LOG(INFO) << "Delete out of range write request failed after "
                             "max retry.";
                return false;
            }
        }

        const auto &last_key = query_result.GetResult().GetLastEvaluatedKey();
        if (last_key.size() == 0)
        {
            // Finished scanning assigned key range.
            break;
        }
        query_req.SetExclusiveStartKey(std::move(last_key));
    }
    return true;
}

bool EloqDS::DynamoHandler::UpsertRanges(
    const txservice::TableName &range_table_name,
    std::vector<txservice::SplitRangeInfo> range_info,
    uint64_t version)
{
    LOG(ERROR) << "DynamoHandler::UpsertRanges not implemented";
    assert(false);
    return false;
}

txservice::store::DataStoreHandler::DataStoreOpStatus
EloqDS::DynamoHandler::LoadRangeSlice(
    const txservice::TableName &table_name,
    const txservice::KVCatalogInfo *kv_info,
    uint32_t range_partition_id,
    txservice::LoadRangeSliceRequest *load_slice_req)
{
    LOG(ERROR) << "DynamoHandler::LoadRangeSlice not implemented";
    assert(false);
    return txservice::store::DataStoreHandler::DataStoreOpStatus::Error;
}

void EloqDS::DynamoHandler::OnLoadRangeSlice(
    const Aws::DynamoDB::DynamoDBClient *client,
    const QueryRequest &request,
    const QueryOutcome &result,
    const std::shared_ptr<const Aws::Client::AsyncCallerContext> &context)
{
    const std::shared_ptr<const LoadRangeSliceData> load_slice_data =
        std::dynamic_pointer_cast<const LoadRangeSliceData>(context);
    LoadRangeSliceRequest &load_slice_req = *load_slice_data->load_slice_req_;

    if (!result.IsSuccess())
    {
        if (load_slice_data->ddl_skip_kv_)
        {
            load_slice_req.SetFinish();
        }
        else if (result.GetError().GetErrorType() !=
                 Aws::DynamoDB::DynamoDBErrors::RESOURCE_NOT_FOUND)
        {
            LOG(ERROR) << "Load range slice failed: "
                       << result.GetError().GetMessage();
            load_slice_req.SetError();
        }
        else
        {
            client->QueryAsync(request, OnLoadRangeSlice, context);
        }
        return;
    }

    const auto &items = result.GetResult().GetItems();
    for (const auto &item : items)
    {
        // Skip deleted items.
        int64_t version_ts = std::stoll(item.at("___version___").GetN());
        bool deleted = item.at("___deleted___").GetBool();
        if (deleted)
        {
            // Only skip deleted items is the latest version is visible to
            // the snapshot ts. Otherwise their could be an older version
            // of the record in the archive table. We need to fill the deleted
            // record status and commit ts to cc map so that tx service can
            // fetch archive table if mvcc is enabled.
            assert(load_slice_req.SnapshotTs() > 0);
            if (load_slice_req.SnapshotTs() >= (uint64_t) version_ts)
            {
                continue;
            }
        }
        Aws::Utils::ByteBuffer key_buf = item.at("___mono_key___").GetB();
        std::unique_ptr<EloqKV::EloqKey> eloq_key =
            std::make_unique<EloqKV::EloqKey>(
                (const char *) key_buf.GetUnderlyingData(),
                key_buf.GetLength());
        std::unique_ptr<txservice::TxRecord> record = nullptr;
        if (!deleted)
        {
            record = DynamoHandlerTyped::ParsePayloadFromDynamoRow(
                item,
                load_slice_req.GetRecordSchema(),
                load_slice_req.TblName());
        }

        load_slice_req.AddDataItem(
            TxKey(std::move(eloq_key)), std::move(record), version_ts, deleted);
    }

    auto &last_key = result.GetResult().GetLastEvaluatedKey();
    if (last_key.size() == 0)
    {
        // All rows have been returned
        load_slice_req.SetFinish();

        if (metrics::enable_kv_metrics)
        {
            metrics::kv_meter->CollectDuration(
                metrics::NAME_KV_LOAD_SLICE_DURATION, load_slice_data->start_);
        }
    }
    else
    {
        QueryRequest new_req = const_cast<QueryRequest &>(request);
        new_req.SetExclusiveStartKey(std::move(last_key));
        client->QueryAsync(std::move(new_req), OnLoadRangeSlice, context);
    }
}

bool EloqDS::DynamoHandler::UpdateRangeSlices(
    const txservice::TableName &table_name,
    uint64_t version,
    txservice::TxKey range_start_key,
    std::vector<const txservice::StoreSlice *> slices,
    int32_t partition_id,
    uint64_t range_version)
{
    LOG(ERROR) << "DynamoHandler::UpdateRangeSlices not implemented";
    assert(false);
    return false;
}

static inline void get_comparable_ts(uint64_t ts, uchar *to)
{
    const uchar *ptr = (uchar *) &ts;
    to[0] = ptr[7];
    to[1] = ptr[6];
    to[2] = ptr[5];
    to[3] = ptr[4];
    to[4] = ptr[3];
    to[5] = ptr[2];
    to[6] = ptr[1];
    to[7] = ptr[0];
}

/**
 * @brief Write batch historical versions into DataStore.
 */
bool EloqDS::DynamoHandler::PutArchivesAll(
    uint32_t node_group,
    const txservice::TableName &table_name,
    const txservice::KVCatalogInfo *kv_info,
    std::vector<txservice::FlushRecord> &batch)
{
    if (!archive_ttl_set_)
    {
        // Set TTL attribute for archive table
        DynamoCatalogInfo archive_info;
        archive_info.kv_table_name_ = dynamo_mvcc_archive_name;
        UpdateDynamoTTL(&archive_info);
        if (!archive_info.ttl_set)
        {
            LOG(INFO) << "Set TTL on MVCC archive table failed";
        }
        else
        {
            archive_ttl_set_ = true;
        }
    }

    const std::string &kv_table_name = kv_info->GetKvTableName(table_name);

    // Make sure each worker has decent amount of work to do.
    uint worker_cnt = std::min(static_cast<int>(worker_pool_.WorkerPoolSize()),
                               static_cast<int>(batch.size() / 3000 + 1));
    uint workload = batch.size() / worker_cnt;
    std::mutex worker_mux;
    std::condition_variable worker_cv;
    uint finished_cnt = 0;
    std::atomic_bool result = true;
    // Assign a slice of the checkpoint vector to each worker.
    for (uint i = 0; i < worker_cnt - 1; i++)
    {
        worker_pool_.SubmitWork(
            [this,
             &kv_table_name,
             &batch,
             i,
             workload,
             node_group,
             &worker_mux,
             &worker_cv,
             &finished_cnt,
             &result]
            {
                PutArchivesThread(this,
                                  &batch,
                                  i * workload,
                                  (i + 1) * workload,
                                  kv_table_name,
                                  node_group,
                                  &result);
                std::unique_lock<std::mutex> worker_lk(worker_mux);
                finished_cnt++;
                worker_cv.notify_one();
            });
    }
    worker_pool_.SubmitWork(
        [this,
         &kv_table_name,
         &batch,
         worker_cnt,
         workload,
         node_group,
         &worker_mux,
         &worker_cv,
         &finished_cnt,
         &result]
        {
            PutArchivesThread(this,
                              &batch,
                              (worker_cnt - 1) * workload,
                              batch.size(),
                              kv_table_name,
                              node_group,
                              &result);
            std::unique_lock<std::mutex> worker_lk(worker_mux);
            finished_cnt++;
            worker_cv.notify_one();
        });

    {
        std::unique_lock<std::mutex> worker_lk(worker_mux);
        worker_cv.wait(worker_lk,
                       [&finished_cnt, &worker_cnt]
                       { return worker_cnt == finished_cnt; });
    }

    return result.load(std::memory_order_acquire);
}

/**
 * @brief Worker thread for PutArchivesAll. Check comments for
 * PutAllThread on why we use multi-threading for checkpoint operation.
 *
 * @param handler
 * @param batch
 * @param start
 * @param end
 * @param kv_table_name
 * @param node_group
 */
void EloqDS::DynamoHandler::PutArchivesThread(
    DynamoHandler *handler,
    std::vector<txservice::FlushRecord> *batch,
    uint32_t start,
    uint32_t end,
    const std::string &kv_table_name,
    uint32_t node_group,
    std::atomic_bool *res)
{
    size_t flush_idx = start;
    bool putall_success = true;

    while (flush_idx < end && Sharder::Instance().LeaderTerm(node_group) > 0 &&
           res->load(std::memory_order_acquire))
    {
        BatchWriteItemRequest batch_req;
        Aws::Vector<WriteRequest> write_reqs;

        for (; write_reqs.size() < dynamo_batch_size - 1 && flush_idx < end;
             ++flush_idx)
        {
            using namespace txservice;

            txservice::FlushRecord &rec = batch->at(flush_idx);
            const EloqKV::EloqKey *cce_key =
                rec.Key().GetKey<EloqKV::EloqKey>();

            WriteRequest write_req;
            AttributeValue tblname_key, version, payload, deleted, ttl;
            PutRequest put_req;

            // For partition key we use kv table name + eloq key combined
            // together. Due to the single partition throughput threshold with
            // DynamoDB, we cannot use table name as its partition key since
            // that would cause checkpoint speed limited by the single partition
            // threshold.
            std::string key_str(kv_table_name);
            // cce_key->Serialize(key_str);
            cce_key->Serialize(key_str);

            tblname_key.SetB(Aws::Utils::ByteBuffer(
                reinterpret_cast<const unsigned char *>(key_str.data()),
                key_str.size()));

            // bind commit ts
            version.SetN(std::to_string(rec.commit_ts_));

            // Set TTL on the timestamp column. Make DynamoDB delete this
            // record after 24 hours.
            ttl.SetN(std::to_string(rec.commit_ts_ + dynamo_expire_ttl));

            // bind payload status
            deleted.SetBool(
                rec.payload_status_ == RecordStatus::Deleted ? true : false);

            // bind payload
            std::string payload_str;
            const txservice::TxRecord *ckpt_payload = rec.Payload();
            if (ckpt_payload != nullptr)
            {
                ckpt_payload->Serialize(payload_str);
                payload.SetB(Aws::Utils::ByteBuffer(
                    reinterpret_cast<const unsigned char *>(payload_str.data()),
                    payload_str.size()));
            }
            else
            {
                payload.SetNull(true);
            }

            put_req.AddItem("tblname___key", std::move(tblname_key));
            put_req.AddItem("commit_ts", std::move(version));
            put_req.AddItem("deleted", std::move(deleted));
            put_req.AddItem("payload", std::move(payload));
            put_req.AddItem("___exp_time___", std::move(ttl));
            write_req.SetPutRequest(std::move(put_req));
            write_reqs.push_back(std::move(write_req));
        }
        size_t flush_size = write_reqs.size();
        batch_req.AddRequestItems(
            handler->keyspace_ + '.' + dynamo_mvcc_archive_name,
            std::move(write_reqs));
        const BatchWriteItemOutcome &batch_res =
            handler->client_->BatchWriteItem(std::move(batch_req));
        if (!batch_res.IsSuccess() && !batch_res.GetError().ShouldRetry())
        {
            res->compare_exchange_strong(putall_success, false);
            LOG(ERROR) << "put archives failed, errmsg: "
                       << batch_res.GetError().GetMessage();
            return;
        }
        if (batch_res.GetResult().GetUnprocessedItems().size() > 0 &&
            !handler->RetryUnprocessedItems(batch_res))
        {
            res->compare_exchange_strong(putall_success, false);
            LOG(ERROR) << "put archives failed.";
            return;
        }

        if (metrics::enable_kv_metrics)
        {
            metrics::kv_meter->Collect(
                metrics::NAME_KV_FLUSH_ROWS_TOTAL, flush_size, "archive");
        }
    }
}

void EloqDS::DynamoHandler::BatchReadThread(
    DynamoHandler *handler,
    std::vector<txservice::TxKey> *batch,
    uint32_t start,
    uint32_t end,
    const txservice::TableName *table_name,
    const txservice::TableSchema *table_schema,
    uint32_t node_group,
    std::atomic<bool> *read_success,
    std::mutex *result_mutex,
    std::vector<txservice::FlushRecord> *result)
{
    size_t flush_idx = start;
    while (flush_idx < end && read_success->load())
    {
        if ((flush_idx - start) / 1000 == 0 &&
            Sharder::Instance().LeaderTerm(node_group) <= 0)
        {
            return;
        }
        const txservice::TxKey &key = batch->at(flush_idx);
        flush_idx++;

        bool success = false;
        bool store_found = false;
        txservice::TxRecord::Uptr latest_rec =
            DynamoHandlerTyped::NewTxRecord(*table_name);
        uint64_t latest_version_ts = 1U;

        int retry = 0;
        while (!success && retry++ < 3)
        {
            success = handler->Read(*table_name,
                                    key,
                                    *latest_rec,
                                    store_found,
                                    latest_version_ts,
                                    table_schema);
        }
        if (!success)
        {
            read_success->store(false);
            return;
        }
        else
        {
            if (latest_version_ts == 1U)
            {
                continue;
            }
            std::unique_lock<std::mutex> result_lock(*result_mutex);
            auto &ref = result->emplace_back();
            ref.payload_status_ = store_found
                                      ? txservice::RecordStatus::Normal
                                      : txservice::RecordStatus::Deleted;
            ref.commit_ts_ = latest_version_ts;
            ref.SetKey(key.GetShallowCopy());
#ifndef ON_KEY_OBJECT
            ref.SetPayload(std::move(latest_rec));
#else
            assert(false);
            ref.SetPayload(latest_rec.get());
#endif
            result_lock.unlock();
        }
    }
}

bool EloqDS::DynamoHandler::CopyBaseToArchive(
    std::vector<txservice::TxKey> &batch,
    uint32_t node_group,
    const txservice::TableName &table_name,
    const txservice::TableSchema *table_schema)
{
    std::mutex result_mutex;
    std::vector<txservice::FlushRecord> archive_vec;
    std::atomic<bool> read_success{true};

    // Make sure each worker has decent amount of work to do.
    uint worker_cnt = std::min((int) worker_pool_.WorkerPoolSize(),
                               (int) batch.size() / 3000 + 1);
    uint workload = batch.size() / worker_cnt;
    std::mutex worker_mux, archive_vec_mux;
    std::condition_variable worker_cv;
    uint finished_cnt = 0;
    std::atomic_bool result = true;
    // Assign a slice of the checkpoint vector to each worker.
    for (uint i = 0; i < worker_cnt - 1; i++)
    {
        worker_pool_.SubmitWork(
            [this,
             &table_name,
             &batch,
             i,
             workload,
             table_schema,
             node_group,
             &worker_mux,
             &worker_cv,
             &finished_cnt,
             &result,
             &archive_vec,
             &archive_vec_mux]
            {
                BatchReadThread(this,
                                &batch,
                                i * workload,
                                (i + 1) * workload,
                                &table_name,
                                table_schema,
                                node_group,
                                &result,
                                &archive_vec_mux,
                                &archive_vec);
                std::unique_lock<std::mutex> worker_lk(worker_mux);
                finished_cnt++;
                worker_cv.notify_one();
            });
    }
    worker_pool_.SubmitWork(
        [this,
         &table_name,
         &batch,
         worker_cnt,
         workload,
         table_schema,
         node_group,
         &worker_mux,
         &worker_cv,
         &finished_cnt,
         &result,
         &archive_vec,
         &archive_vec_mux]
        {
            BatchReadThread(this,
                            &batch,
                            (worker_cnt - 1) * workload,
                            batch.size(),
                            &table_name,
                            table_schema,
                            node_group,
                            &result,
                            &archive_vec_mux,
                            &archive_vec);
            std::unique_lock<std::mutex> worker_lk(worker_mux);
            finished_cnt++;
            worker_cv.notify_one();
        });

    {
        std::unique_lock<std::mutex> worker_lk(worker_mux);
        worker_cv.wait(worker_lk,
                       [&finished_cnt, &worker_cnt]
                       { return worker_cnt == finished_cnt; });
    }

    if (!read_success.load())
    {
        return false;
    }

    bool ret = PutArchivesAll(
        node_group, table_name, table_schema->GetKVCatalogInfo(), archive_vec);
    if (!ret)
    {
        return false;
    }

    return true;
}

/**
 * @brief  Get the latest visible(commit_ts <= upper_bound_ts) historical
 * version.
 */
bool EloqDS::DynamoHandler::FetchVisibleArchive(
    const txservice::TableName &table_name,
    const txservice::KVCatalogInfo *kv_info,
    const txservice::TxKey &key,
    const uint64_t upper_bound_ts,
    txservice::TxRecord &rec,
    txservice::RecordStatus &rec_status,
    uint64_t &commit_ts)
{
    QueryRequest req;
    req.SetTableName(keyspace_ + '.' + dynamo_mvcc_archive_name);
    req.SetConsistentRead(true);

    const std::string &kv_table_name = kv_info->GetKvTableName(table_name);
    // We only need the latest visible version
    req.SetLimit(1);
    AttributeValue tblname_key, ts_val;

    // bind table name + key (partition key)
    std::string key_str(kv_table_name);
    key.Serialize(key_str);

    tblname_key.SetB(Aws::Utils::ByteBuffer(
        reinterpret_cast<const unsigned char *>(key_str.data()),
        key_str.size()));

    // bind ts_val, used to filter out unvisible versions.
    ts_val.SetN(std::to_string(upper_bound_ts));

    // set up the query expression
    req.SetKeyConditionExpression("tblname___key = :pk AND commit_ts <= :sk");
    req.AddExpressionAttributeValues(":pk", std::move(tblname_key));
    req.AddExpressionAttributeValues(":sk", std::move(ts_val));

    // Set the scan direction as backward to get the version with the largest ts
    req.SetScanIndexForward(false);
    const QueryOutcome &res = client_->Query(req);
    if (!res.IsSuccess())
    {
        LOG(INFO) << "Fetch from archive table failed, err: "
                  << res.GetError().GetMessage();
        return false;
    }
    if (res.GetResult().GetCount() == 0)
    {
        rec_status = txservice::RecordStatus::Deleted;
        return true;
    }

    const auto &row = res.GetResult().GetItems().at(0);
    commit_ts = std::stoull(row.at("commit_ts").GetN());
    bool deleted = row.at("deleted").GetBool();
    rec_status = deleted ? txservice::RecordStatus::Deleted
                         : txservice::RecordStatus::Normal;
    Aws::Utils::ByteBuffer payload_bytes = row.at("payload").GetB();
    if (payload_bytes.GetLength() > 0)
    {
        const char *content =
            reinterpret_cast<const char *>(payload_bytes.GetUnderlyingData());
        size_t offset = 0;
        rec.Deserialize(content, offset);
    }

    return true;
}

/**
 * @brief  Fetch all archives whose commit_ts >= from_ts.
 */
bool EloqDS::DynamoHandler::FetchArchives(
    const txservice::TableName &table_name,
    const txservice::KVCatalogInfo *kv_info,
    const txservice::TxKey &key,
    std::vector<txservice::VersionTxRecord> &archives,
    uint64_t from_ts)
{
    QueryRequest req;
    req.SetTableName(keyspace_ + '.' + dynamo_mvcc_archive_name);
    req.SetConsistentRead(true);
    req.SetLimit(dynamo_batch_size);
    Aws::Map<Aws::String, AttributeValue> start_key;
    const std::string &kv_table_name = kv_info->GetKvTableName(table_name);
    do
    {
        if (start_key.size())
        {
            req.SetExclusiveStartKey(std::move(start_key));
        }
        else
        {
            AttributeValue tblname_key, ts_val;

            // bind table name + key (partition key)
            std::string key_str(kv_table_name);
            key.Serialize(key_str);

            tblname_key.SetB(Aws::Utils::ByteBuffer(
                reinterpret_cast<const unsigned char *>(key_str.data()),
                key_str.size()));

            // bind ts_val, used to filter out versions.
            ts_val.SetN(std::to_string(from_ts));

            // set up the query expression
            req.SetKeyConditionExpression(
                "tblname___key = :pk AND commit_ts >= :sk");
            req.AddExpressionAttributeValues(":pk", std::move(tblname_key));
            req.AddExpressionAttributeValues(":sk", std::move(ts_val));
        }

        const QueryOutcome &res = client_->Query(req);
        if (!res.IsSuccess())
        {
            LOG(INFO) << "Fetch archive failed: "
                      << res.GetError().GetMessage();
            return false;
        }

        for (const auto &row : res.GetResult().GetItems())
        {
            txservice::VersionTxRecord &akv_rec = archives.emplace_back();
            akv_rec.commit_ts_ = std::stoull(row.at("commit_ts").GetN());
            bool deleted = row.at("deleted").GetBool();
            akv_rec.record_status_ = deleted ? txservice::RecordStatus::Deleted
                                             : txservice::RecordStatus::Normal;
            Aws::Utils::ByteBuffer payload_bytes = row.at("payload").GetB();
            if (payload_bytes.GetLength() > 0)
            {
                const char *content = reinterpret_cast<const char *>(
                    payload_bytes.GetUnderlyingData());
                size_t offset = 0;
                akv_rec.record_ = DynamoHandlerTyped::NewTxRecord(table_name);
                akv_rec.record_->Deserialize(content, offset);
            }
        }
        start_key = res.GetResult().GetLastEvaluatedKey();
    } while (start_key.size() > 0);

    return true;
}

std::string EloqDS::DynamoHandler::CreateKVCatalogInfo(
    const txservice::TableSchema *table_schema) const
{
    DynamoCatalogInfo kv_info;
    kv_info.kv_index_names_.clear();
    kv_info.kv_table_name_ =
        std::string("t").append(Aws::Utils::UUID::RandomUUID());

    std::vector index_names = table_schema->IndexNames();
    for (auto idx_it = index_names.begin(); idx_it < index_names.end();
         ++idx_it)
    {
        if (idx_it->Type() == txservice::TableType::Secondary)
        {
            kv_info.kv_index_names_.try_emplace(
                *idx_it,
                std::string("i").append(Aws::Utils::UUID::RandomUUID()));
        }
        else
        {
            assert((idx_it->Type() == txservice::TableType::UniqueSecondary));
            kv_info.kv_index_names_.try_emplace(
                *idx_it,
                std::string("u").append(Aws::Utils::UUID::RandomUUID()));
        }
    }
    return kv_info.Serialize();
}

txservice::KVCatalogInfo::uptr EloqDS::DynamoHandler::DeserializeKVCatalogInfo(
    const std::string &kv_info_str, size_t &offset) const
{
    DynamoCatalogInfo::uptr kv_info = std::make_unique<DynamoCatalogInfo>();
    kv_info->Deserialize(kv_info_str.data(), offset);
    return kv_info;
}

/**
 * @brief Generate new DynamoCatalogInfo that contains current kv table name
 * and new kv index names for altered table. Return the serialized string of
 * the altered catalog info.
 *
 * Note: out of this function, index table name in AlterTableInfo object is
 * formatted as <table_name><INDEX_NAME_PREFIX><index_name>.
 *
 * @param table_name
 * @param current_table_schema
 * @param alter_table_info
 * @return std::string, alter_table_info
 */
std::string EloqDS::DynamoHandler::CreateNewKVCatalogInfo(
    const txservice::TableName &table_name,
    const txservice::TableSchema *current_table_schema,
    txservice::AlterTableInfo &alter_table_info)
{
    // Get current kv catalog info.
    const DynamoCatalogInfo *current_dynamo_catalog_info =
        static_cast<const DynamoCatalogInfo *>(
            current_table_schema->GetKVCatalogInfo());

    std::string new_kv_info, kv_table_name, new_kv_index_names;

    /* kv table name using current table name */
    kv_table_name = current_dynamo_catalog_info->kv_table_name_;
    uint32_t kv_val_len = kv_table_name.length();
    new_kv_info
        .append(reinterpret_cast<char *>(&kv_val_len), sizeof(kv_val_len))
        .append(kv_table_name.data(), kv_val_len);

    /* kv index names using new schema index names */
    // 1. remove dropped index kv name
    bool dropped = false;
    for (auto kv_index_it =
             current_dynamo_catalog_info->kv_index_names_.cbegin();
         kv_index_it != current_dynamo_catalog_info->kv_index_names_.cend();
         ++kv_index_it)
    {
        // Check if the index will be dropped.
        dropped = false;
        for (auto drop_index_it = alter_table_info.index_drop_names_.cbegin();
             alter_table_info.index_drop_count_ > 0 &&
             drop_index_it != alter_table_info.index_drop_names_.cend();
             drop_index_it++)
        {
            if (kv_index_it->first == drop_index_it->first)
            {
                dropped = true;
                // Remove dropped index
                alter_table_info.index_drop_names_[kv_index_it->first] =
                    kv_index_it->second;
                break;
            }
        }
        if (!dropped)
        {
            new_kv_index_names.append(kv_index_it->first.String())
                .append(" ")
                .append(kv_index_it->second)
                .append(" ");
        }
    }
    assert(alter_table_info.index_drop_names_.size() ==
           alter_table_info.index_drop_count_);

    // 2. add new index
    for (auto add_index_it = alter_table_info.index_add_names_.cbegin();
         alter_table_info.index_add_count_ > 0 &&
         add_index_it != alter_table_info.index_add_names_.cend();
         add_index_it++)
    {
        // get indedx kv table name
        std::string add_index_kv_name;
        if (add_index_it->first.Type() == txservice::TableType::Secondary)
        {
            add_index_kv_name =
                std::string("i").append(Aws::Utils::UUID::RandomUUID());
        }
        else
        {
            assert(add_index_it->first.Type() ==
                   txservice::TableType::UniqueSecondary);
            add_index_kv_name =
                std::string("u").append(Aws::Utils::UUID::RandomUUID());
        }

        new_kv_index_names.append(add_index_it->first.String())
            .append(" ")
            .append(add_index_kv_name.data())
            .append(" ");

        // add index kv table name
        alter_table_info.index_add_names_[add_index_it->first] =
            add_index_kv_name;
    }
    assert(alter_table_info.index_add_names_.size() ==
           alter_table_info.index_add_count_);

    /* create final new kv info */
    kv_val_len = new_kv_index_names.size();
    new_kv_info
        .append(reinterpret_cast<char *>(&kv_val_len), sizeof(kv_val_len))
        .append(new_kv_index_names.data(), kv_val_len);

    return new_kv_info;
}

bool EloqDS::DynamoHandler::RetryUnprocessedItems(
    const BatchWriteItemOutcome &outcome)
{
    auto unprocessed = outcome.GetResult().GetUnprocessedItems();
    auto need_sleep_time = std::chrono::milliseconds(10);
    for (uint retry = 0; unprocessed.size() > 0 && retry < dynamo_api_retry;
         retry++)
    {
        if (retry > 0)
        {
            need_sleep_time *= 2;
            if (retry >= 10)
            {
                LOG(INFO) << "retry writing to DynamoDB table, times " << retry;
            }
        }
        if (need_sleep_time > 60s)
        {
            LOG(WARNING)
                << "DynamoDB table are being throttled and retry count "
                   "exceeds the limit";
            return false;
        }
        std::this_thread::sleep_for(need_sleep_time);

        BatchWriteItemRequest retry_req;
        const auto &retry_res = client_->BatchWriteItem(
            retry_req.WithRequestItems(std::move(unprocessed)));
        if (!retry_res.IsSuccess() && !retry_res.GetError().ShouldRetry())
        {
            LOG(ERROR) << "BatchWriteItemRequest failed: "
                       << retry_res.GetError().GetMessage();
            return false;
        }
        unprocessed = retry_res.GetResult().GetUnprocessedItems();
    }
    return unprocessed.size() == 0;
}

bool EloqDS::DynamoHandler::ListKvTableCTimeMore1d(
    std::set<std::string> &kv_table_names) const
{
    ListTablesRequest req;

    while (true)
    {
        ListTablesOutcome outcome = client_->ListTables(req);
        if (!outcome.IsSuccess())
        {
            LOG(ERROR) << "List kv tables failed, "
                       << outcome.GetError().GetMessage();
            return false;
        }

        const Aws::Vector<Aws::String> &table_names =
            outcome.GetResult().GetTableNames();
        for (const Aws::String &table_name : table_names)
        {
            if (table_name.compare(0, keyspace_.size(), keyspace_) == 0)
            {
                // Remove prefix: keyspace + '.'
                std::string kv_table_name =
                    table_name.substr(keyspace_.size() + 1);
                if (dynamo_sys_tables.find(kv_table_name) ==
                        dynamo_sys_tables.end() ||
                    pre_built_table_names_.find(kv_table_name) ==
                        pre_built_table_names_.end())
                {
                    DescribeTableRequest req;
                    req.SetTableName(table_name);
                    DescribeTableOutcome outcome = client_->DescribeTable(req);
                    if (!outcome.IsSuccess())
                    {
                        if (outcome.GetError().GetErrorType() !=
                            Aws::DynamoDB::DynamoDBErrors::RESOURCE_NOT_FOUND)
                        {
                            LOG(ERROR) << "Describe kv table failed, "
                                       << outcome.GetError().GetMessage();
                            return false;
                        }
                    }
                    else
                    {
                        const TableDescription &table_desc =
                            outcome.GetResult().GetTable();
                        const Aws::Utils::DateTime &create_tm =
                            table_desc.GetCreationDateTime();
                        auto now = Aws::Utils::DateTime::Now();
                        std::chrono::milliseconds duration =
                            create_tm.Diff(now, create_tm);
                        if (duration > 24h)
                        {
                            kv_table_names.emplace(std::move(kv_table_name));
                        }
                    }
                }
            }
        }

        const Aws::String &last_value =
            outcome.GetResult().GetLastEvaluatedTableName();
        if (!last_value.empty())
        {
            req.SetExclusiveStartTableName(std::move(last_value));
        }
        else
        {
            break;
        }
    }

    return true;
}

bool EloqDS::DynamoHandler::ListVisibleKvTable(
    std::set<std::string> &kv_table_names) const
{
    ScanRequest req;
    req.SetTableName(keyspace_ + '.' + dynamo_table_catalog_name);
    req.SetProjectionExpression("kvtablename,kvindexname");
    req.SetConsistentRead(true);

    while (true)
    {
        ScanOutcome outcome = client_->Scan(req);
        if (!outcome.IsSuccess())
        {
            LOG(ERROR) << "Scan failed for table " << dynamo_table_catalog_name;
            return false;
        }

        const ScanResult &result = outcome.GetResult();
        const Aws::Vector<Aws::Map<Aws::String, AttributeValue>> &items =
            result.GetItems();
        for (const Aws::Map<Aws::String, AttributeValue> &item : items)
        {
            const Aws::String kvtablename = item.at("kvtablename").GetS();
            const Aws::Map<Aws::String, const std::shared_ptr<AttributeValue>>
                kvindexnames = item.at("kvindexname").GetM();

            DynamoCatalogInfo kv_info;
            kv_info.kv_table_name_.assign(kvtablename);
            for (const auto &[indexname, kvindexname] : kvindexnames)
            {
                bool is_unique_sk = kvindexname->GetS().front() == 'u';
                kv_info.kv_index_names_.try_emplace(
                    TableName(indexname,
                              is_unique_sk ? TableType::UniqueSecondary
                                           : TableType::Secondary),
                    kvindexname->GetS());
            }

            kv_table_names.emplace(kv_info.kv_table_name_);
            for (const auto &[index_name, kv_index_name] :
                 kv_info.kv_index_names_)
            {
                kv_table_names.emplace(kv_index_name);
            }
        }

        const auto &last_value = result.GetLastEvaluatedKey();
        if (!last_value.empty())
        {
            req.SetExclusiveStartKey(std::move(last_value));
        }
        else
        {
            break;
        }
    }

    return true;
}

void EloqDS::DynamoHandler::CleanDefunctKvTables(void *store_hd)
{
    DynamoHandler *dynamo_hd = static_cast<DynamoHandler *>(store_hd);

    auto task = [dynamo_hd]()
    {
        bool ok = true;

        std::set<std::string> tables_ctime_more_1d;
        std::set<std::string> tables_visible;
        std::set<std::string> tables_to_drop;

        ok = dynamo_hd->ListKvTableCTimeMore1d(tables_ctime_more_1d) &&
             dynamo_hd->ListVisibleKvTable(tables_visible);

        if (ok)
        {
            std::set_difference(
                tables_ctime_more_1d.begin(),
                tables_ctime_more_1d.end(),
                tables_visible.begin(),
                tables_visible.end(),
                std::inserter(tables_to_drop, tables_to_drop.end()));
            ok = std::all_of(
                tables_to_drop.begin(),
                tables_to_drop.end(),
                [dynamo_hd](const std::string &kv_table_name) -> bool
                { return dynamo_hd->DropKvTable(kv_table_name); });
        }

        dynamo_hd->timer_thd_.schedule(
            &DynamoHandler::CleanDefunctKvTables,
            dynamo_hd,
            butil::seconds_from_now(
                ok ? std::chrono::duration_cast<std::chrono::seconds>(24h)
                         .count()
                   : 5));
    };

    dynamo_hd->worker_pool_.SubmitWork(std::move(task));
}

EloqDS::DynamoCatalogInfo::DynamoCatalogInfo(const std::string &table_name,
                                             const std::string &index_names)
{
    std::stringstream ss(index_names);
    std::istream_iterator<std::string> begin(ss);
    std::istream_iterator<std::string> end;
    std::vector<std::string> tokens(begin, end);
    for (auto it = tokens.begin(); it != tokens.end(); ++it)
    {
        std::string kv_index_name;
        bool is_unique_sk = std::next(it, 1)->front() == 'u';
        txservice::TableName index_name(
            (*it).c_str(),
            (*it).length(),
            is_unique_sk ? txservice::TableType::UniqueSecondary
                         : txservice::TableType::Secondary);
        kv_index_name.append(*(++it));
        kv_index_names_.try_emplace(index_name, kv_index_name);
    }
    kv_table_name_ = table_name;
}

void EloqDS::DynamoCatalogInfo::Deserialize(const char *buf, size_t &offset)
{
    if (buf[0] == '\0')
    {
        return;
    }
    uint32_t *len_ptr = (uint32_t *) (buf + offset);
    uint32_t len_val = *len_ptr;
    offset += sizeof(uint32_t);

    kv_table_name_.clear();
    kv_table_name_.reserve(len_val);

    kv_table_name_.append(buf + offset, len_val);
    offset += len_val;

    len_ptr = (uint32_t *) (buf + offset);
    len_val = *len_ptr;
    offset += sizeof(uint32_t);
    if (len_val != 0)
    {
        std::string index_names(buf + offset, len_val);
        offset += len_val;
        std::stringstream ss(index_names);
        std::istream_iterator<std::string> begin(ss);
        std::istream_iterator<std::string> end;
        std::vector<std::string> tokens(begin, end);
        for (auto it = tokens.begin(); it != tokens.end(); ++it)
        {
            std::string kv_index_name;
            bool is_unique_sk = std::next(it, 1)->front() == 'u';
            txservice::TableName index_name(
                (*it).c_str(),
                (*it).length(),
                is_unique_sk ? txservice::TableType::UniqueSecondary
                             : txservice::TableType::Secondary);
            kv_index_name.append(*(++it));
            kv_index_names_.try_emplace(index_name, kv_index_name);
        }
    }
    else
    {
        kv_index_names_.clear();
    }
}

std::string EloqDS::DynamoCatalogInfo::Serialize() const
{
    std::string str;
    size_t len_sizeof = sizeof(uint32_t);
    uint32_t len_val = (uint32_t) kv_table_name_.size();
    char *len_ptr = reinterpret_cast<char *>(&len_val);
    str.append(len_ptr, len_sizeof);
    str.append(kv_table_name_.data(), len_val);

    std::string index_names;
    if (kv_index_names_.size() != 0)
    {
        for (auto it = kv_index_names_.cbegin(); it != kv_index_names_.cend();
             ++it)
        {
            index_names.append(it->first.String())
                .append(" ")
                .append(it->second)
                .append(" ");
        }
        index_names.substr(0, index_names.size() - 1);
    }
    else
    {
        index_names.clear();
    }
    len_val = (uint32_t) index_names.size();
    str.append(len_ptr, len_sizeof);
    str.append(index_names.data(), len_val);
    return str;
}

bool EloqDS::DynamoHandler::CreateKvTableIfNotExists(
    const std::string &kv_table_name)
{
    std::string kv_tbl_name = keyspace_ + '.' + kv_table_name;

    int retry = 0;
    while (retry <= 5)
    {
        // After create table request being sent to aws Dynamodb, it may take a
        // long time to wait the table be active.
        DescribeTableRequest dtr;
        dtr.SetTableName(kv_tbl_name);

        // check if sys table already exists
        const DescribeTableOutcome &dtr_result = client_->DescribeTable(dtr);

        if (dtr_result.IsSuccess())
        {
            // Check if the table is in active status
            const TableStatus &status =
                dtr_result.GetResult().GetTable().GetTableStatus();

            if (status == TableStatus::ACTIVE)
            {
                DLOG(INFO) << "KvTable exists: " << kv_table_name;
                return true;
            }

            // Any status other than ACTIVE and CREATING is invalid
            if (status != TableStatus::CREATING)
            {
                LOG(ERROR) << "Failed to create KvTable: " << kv_table_name
                           << ", status:" << (int) status;
                return false;
            }
            // Other host might be creating the table now, let's wait for them
            // to complete
            bthread_usleep(1000);
        }
        else if (dtr_result.GetError().GetErrorType() ==
                 Aws::DynamoDB::DynamoDBErrors::RESOURCE_NOT_FOUND)
        {
            DLOG(INFO) << "Create KvTable: " << kv_tbl_name;
            // Create the sys table if it doesn't exist yet
            CreateTableRequest ctr;
            ctr.SetTableName(kv_tbl_name);
            ctr.SetBillingMode(BillingMode::PAY_PER_REQUEST);

            AttributeDefinition partition_key, sort_key;
            // Partition key should always be the pk generated by tx_service
            partition_key.WithAttributeName(dynamo_partition_key_attribute_name)
                .WithAttributeType(ScalarAttributeType::N);
            ctr.AddAttributeDefinitions(std::move(partition_key));

            KeySchemaElement pk_schema, sk_schema;
            pk_schema.WithAttributeName(dynamo_partition_key_attribute_name)
                .WithKeyType(Aws::DynamoDB::Model::KeyType::HASH);
            ctr.AddKeySchema(std::move(pk_schema));

            // Sort key should be the binary value serialized from tx key
            sort_key.WithAttributeName(dynamo_sort_key_attribute_name)
                .WithAttributeType(ScalarAttributeType::B);
            ctr.AddAttributeDefinitions(sort_key);
            sk_schema.WithAttributeName(dynamo_sort_key_attribute_name)
                .WithKeyType(Aws::DynamoDB::Model::KeyType::RANGE);
            ctr.AddKeySchema(std::move(sk_schema));

            const CreateTableOutcome &ctr_result = client_->CreateTable(ctr);

            // If the error is RESOURCE_IN_USE that means someone else just
            // created this table before us. Lets wait for the table to become
            // active. Otherwise the create request has failed, return false
            if (!ctr_result.IsSuccess() &&
                ctr_result.GetError().GetErrorType() !=
                    Aws::DynamoDB::DynamoDBErrors::RESOURCE_IN_USE)
            {
                LOG(ERROR) << "Create KvTable failed: " << kv_tbl_name
                           << ", error: "
                           << ctr_result.GetError().GetExceptionName() << ","
                           << "error message: "
                           << ctr_result.GetError().GetMessage();
                return false;
            }
            // Check if the table is successfully created after 1000us
            bthread_usleep(1000);
        }
        else
        {
            LOG(ERROR) << "Create KvTable, check table status failed: "
                       << kv_tbl_name << ", error: "
                       << dtr_result.GetError().GetExceptionName() << ","
                       << "error message: "
                       << dtr_result.GetError().GetMessage();
            return false;
        }
    }
    LOG(ERROR) << "CreateKvTableIfNotExists failed after max retry";
    return false;
}

bool EloqDS::DynamoHandler::InitPreBuiltTables()
{
    DescribeTableRequest dtr;
    for (const auto &[tbl_name, kv_tbl_name] : pre_built_table_names_)
    {
        txservice::TableName table_name(tbl_name,
                                        txservice::TableType::Primary);

        // Check if the table already exist.
        dtr.SetTableName(keyspace_ + '.' + kv_tbl_name);
        const DescribeTableOutcome &dtr_result = client_->DescribeTable(dtr);
        if (dtr_result.IsSuccess())
        {
            LOG(INFO) << "table already exist : " << table_name.String();
            continue;
        }
        else if (dtr_result.GetError().GetErrorType() !=
                 Aws::DynamoDB::DynamoDBErrors::RESOURCE_NOT_FOUND)
        {
            LOG(ERROR) << "!!!! Check kv table error: "
                       << dtr_result.GetError().GetMessage();
            return false;
        }

        if (!CreateKvTableIfNotExists(kv_tbl_name))
        {
            LOG(ERROR) << "InitPreBuiltTables for " << tbl_name << " failed. ";
            return false;
        }

#ifdef RANGE_PARTITION_ENABLED
        // UpsertRangeInfo
        {
            PutItemRequest put_req;
            AttributeValue range_id, range_key, version;

            range_id.SetN(
                Partition::InitialPartitionId(table_name.StringView()));

            range_key.SetB(Aws::Utils::ByteBuffer(
                &(DynamoHandlerTyped::neg_inf_packed_key_), 1));

            version.SetN(std::to_string(1));

            put_req.SetTableName(keyspace_ + '.' + dynamo_range_table_name);
            put_req.AddItem("tablename", AttributeValue(table_name.String()));
            put_req.AddItem("start_key", std::move(range_key));
            put_req.AddItem("partition_id", std::move(range_id));
            put_req.AddItem("version", std::move(version));
            auto outcome = client_->PutItem(std::move(put_req));
            if (!outcome.IsSuccess())
            {
                LOG(ERROR) << "Insert table range info for "
                           << table_name.String() << " failed. "
                           << outcome.GetError().GetMessage();
                return false;
            }
            else
            {
                LOG(INFO) << "Insert table range info for "
                          << table_name.String() << " succeed. ";
            }
        }

        // UpsertLastRangeId
        {
            PutItemRequest put_req;
            AttributeValue range_id;

            range_id.SetN(
                Partition::InitialPartitionId(table_name.StringView()));

            put_req.SetTableName(keyspace_ + '.' + dynamo_last_range_id_name);
            put_req.AddItem("tablename", AttributeValue(table_name.String()));
            put_req.AddItem("last_partition_id", std::move(range_id));
            auto outcome = client_->PutItem(std::move(put_req));
            if (!outcome.IsSuccess())
            {
                LOG(ERROR) << "Insert last partition id for "
                           << table_name.String() << " failed. "
                           << outcome.GetError().GetMessage();
                return false;
            }
            else
            {
                LOG(INFO) << "Insert last partition id for "
                          << table_name.String() << " succeed. ";
            }
        }
#endif

        PutItemRequest preq;
        preq.SetTableName(keyspace_ + '.' + dynamo_table_catalog_name);
        preq.AddItem("tablename", AttributeValue(tbl_name));
        preq.AddItem("content",
                     AttributeValue().SetB(Aws::Utils::ByteBuffer(nullptr, 0)));
        preq.AddItem("kvtablename", AttributeValue(kv_tbl_name));
        preq.AddItem("kvindexname", AttributeValue("empty"));

        // keyschemats, use table version as key schema ts
        const char *table_version = "100";
        preq.AddItem("keyschemasts", AttributeValue(table_version));
        preq.AddItem("tableversion", AttributeValue(table_version));
        auto result = client_->PutItem(std::move(preq));
        if (!result.IsSuccess())
        {
            LOG(INFO) << "Failed to create table: " << tbl_name
                      << ", error:" << result.GetError().GetMessage();

            return false;
        }
        else
        {
            LOG(INFO) << "Created table: " << tbl_name;
        }
    }
    return true;
}
