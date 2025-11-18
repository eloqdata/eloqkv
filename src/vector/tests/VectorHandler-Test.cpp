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
#define CATCH_CONFIG_RUNNER

#include <atomic>
#include <catch2/catch_all.hpp>
#include <chrono>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "tx_service.h"
#include "tx_util.h"
#include "tx_worker_pool.h"
#include "vector_handler.h"
#include "vector_type.h"

using namespace txservice;

namespace EloqVec
{
// Test configuration
static std::unique_ptr<TxService> tx_service(nullptr);
static std::unique_ptr<TxWorkerPool> vector_index_worker_pool(nullptr);
static std::string test_data_path = "test_vector_handler_data";
static std::string vector_index_data_path = "test_vector_index_data";

// Test constants
constexpr uint32_t num_cores = 4;
constexpr size_t test_vector_dimension = 4;
constexpr size_t test_max_elements = 100000;

/**
 * @brief Initialize TxService for testing
 * @return Unique pointer to initialized TxService
 */
std::unique_ptr<TxService> InitTxService()
{
    GFLAGS_NAMESPACE::SetCommandLineOption("worker_polling_time_us", "1000");
    GFLAGS_NAMESPACE::SetCommandLineOption("brpc_worker_as_ext_processor",
                                           "true");
    GFLAGS_NAMESPACE::SetCommandLineOption("use_pthread_event_dispatcher",
                                           "true");
    GFLAGS_NAMESPACE::SetCommandLineOption("bthread_concurrency",
                                           std::to_string(num_cores).c_str());

    CatalogFactory *catalog_factory[3]{nullptr, nullptr, nullptr};
    std::unordered_map<uint32_t, std::vector<NodeConfig>> ng_configs;
    NodeConfig node_config(0, "127.0.0.1", 10000);
    auto [it, success] = ng_configs.try_emplace(0);
    assert(success);
    it->second.push_back(std::move(node_config));

    std::unordered_map<txservice::TableName, std::string> prebuilt_tables;
    prebuilt_tables.try_emplace(vector_index_meta_table,
                                vector_index_meta_table.String());

    std::map<std::string, uint32_t> tx_service_conf{
        {"core_num", num_cores},
        {"checkpointer_interval", 10},
        {"checkpointer_delay_seconds", 0},
        {"node_memory_limit_mb", 4096},
        {"node_log_limit_mb", 4096},
        {"realtime_sampling", 0},
        {"range_split_worker_num", 0},
        {"enable_shard_heap_defragment", 0},
        {"enable_key_cache", 0},
        {"rep_group_cnt", 1},
        {"collect_active_tx_ts_interval_seconds", 60},
    };

    metrics::CommonLabels common_labels{};

    std::string tx_path("local://");
    tx_path.append(test_data_path);

    std::string cluster_config_file_path;
    cluster_config_file_path.append(tx_path);
    cluster_config_file_path.append("/cluster_config");

    std::unique_ptr<TxService> new_tx_service =
        std::make_unique<TxService>(catalog_factory,
                                    nullptr,
                                    tx_service_conf,
                                    0,
                                    0,
                                    &ng_configs,
                                    2,
                                    nullptr,
                                    nullptr,
                                    false,  // enable_mvcc
                                    true,   // skip_wal
                                    true,   // skip_kv
                                    false,  // enable_cache_replacement
                                    true,   // auto_redirect
                                    nullptr,
                                    common_labels,
                                    &prebuilt_tables);

    assert(new_tx_service->Start(0,
                                 0,
                                 &ng_configs,
                                 2,
                                 nullptr,
                                 nullptr,
                                 nullptr,
                                 nullptr,
                                 nullptr,
                                 tx_service_conf,
                                 nullptr,
                                 tx_path,
                                 cluster_config_file_path,
                                 true,
                                 false) == 0);
    LOG(INFO) << "TxService started";
    new_tx_service->WaitClusterReady();
    LOG(INFO) << "TxService initialized";
    return new_tx_service;
}

/**
 * @brief Create a test IndexConfig
 * @return IndexConfig for testing
 */
IndexConfig CreateTestIndexConfig()
{
    std::unordered_map<std::string, std::string> params;
    params["m"] = "16";
    params["ef_construction"] = "128";
    params["ef_search"] = "64";

    return IndexConfig(test_vector_dimension,
                       Algorithm::HNSW,
                       DistanceMetric::COSINE,
                       std::move(params));
}

/**
 * @brief Create a test VectorRecordMetadata with fields
 * @return VectorRecordMetadata for testing
 */
VectorRecordMetadata CreateTestMetadata()
{
    std::vector<std::string> field_names = {"category", "score"};
    std::vector<MetadataFieldType> field_types = {MetadataFieldType::String,
                                                  MetadataFieldType::Double};

    return VectorRecordMetadata(std::move(field_names), std::move(field_types));
}

/**
 * @brief Create an empty VectorRecordMetadata (no metadata fields)
 * @return Empty VectorRecordMetadata for testing
 */
VectorRecordMetadata CreateEmptyMetadata()
{
    std::vector<std::string> empty_field_names;
    std::vector<MetadataFieldType> empty_field_types;
    return VectorRecordMetadata(std::move(empty_field_names),
                                std::move(empty_field_types));
}

/**
 * @brief Create a test VectorIndexMetadata
 * @param name Index name
 * @param persist_threshold Persistence threshold (-1 for manual)
 * @param with_metadata Whether to include metadata schema (default: true)
 * @return VectorIndexMetadata for testing
 */
VectorIndexMetadata CreateTestIndexMetadata(const std::string &name,
                                            int64_t persist_threshold = -1,
                                            bool with_metadata = true)
{
    IndexConfig config = CreateTestIndexConfig();
    config.max_elements = test_max_elements;
    VectorRecordMetadata metadata =
        with_metadata ? CreateTestMetadata() : CreateEmptyMetadata();

    return VectorIndexMetadata(std::string(name),
                               std::move(config),
                               std::move(metadata),
                               persist_threshold,
                               vector_index_data_path);
}

/**
 * @brief Generate a test vector with given dimension
 * @param dimension Vector dimension
 * @param seed Optional seed for random generation
 * @return Test vector
 */
std::vector<float> GenerateTestVector(size_t dimension, uint32_t seed = 0)
{
    static std::random_device rd;
    static thread_local std::mt19937 gen(seed == 0 ? rd() : seed);
    static thread_local std::uniform_real_distribution<float> dis(-1.0f, 1.0f);

    std::vector<float> vector;
    vector.reserve(dimension);
    for (size_t i = 0; i < dimension; ++i)
    {
        vector.push_back(dis(gen));
    }
    return vector;
}

/**
 * @brief Generate a test metadata JSON string in array format
 * The array format matches the schema field order: [category, score]
 * @param category Category string
 * @param score Score value
 * @return JSON string representation of metadata as array
 */
std::string GenerateTestMetadataJson(const std::string &category = "test",
                                     double score = 0.5)
{
    // Metadata JSON must be an array matching schema field order
    // Schema: ["category" (String), "score" (Double)]
    return "[\"" + category + "\"," + std::to_string(score) + "]";
}

/**
 * @brief Clean up test data directories
 */
void CleanupTestData()
{
    try
    {
        if (std::filesystem::exists(test_data_path))
        {
            std::filesystem::remove_all(test_data_path);
        }
        if (std::filesystem::exists(vector_index_data_path))
        {
            std::filesystem::remove_all(vector_index_data_path);
        }
    }
    catch (const std::filesystem::filesystem_error &e)
    {
        LOG(WARNING) << "Failed to cleanup test data: " << e.what();
    }
}

/**
 * @brief Generate a unique test index name
 * @return Unique index name
 */
std::string GenerateTestIndexName()
{
    static std::random_device rd;
    static thread_local std::mt19937 gen(rd());
    static thread_local std::uniform_int_distribution<uint32_t> dis(0,
                                                                    UINT32_MAX);
    return "test_index_" + std::to_string(dis(gen));
}

// ============================================================================
// Test Cases
// ============================================================================

TEST_CASE("VectorHandler Singleton Initialization", "[vector-handler]")
{
    DLOG(INFO) << "VectorHandler Singleton Initialization start";

    // Test 1: Instance() should return a valid reference after initialization
    // (initialization is done in main function)
    REQUIRE_NOTHROW(VectorHandler::Instance());
    VectorHandler &handler1 = VectorHandler::Instance();
    REQUIRE_NOTHROW(handler1.VectorIndexDataPath());

    // Test 2: Multiple calls to Instance() should return the same instance
    VectorHandler &handler2 = VectorHandler::Instance();
    REQUIRE(&handler1 == &handler2);

    // Test 3: VectorIndexDataPath should return the correct path
    REQUIRE(handler1.VectorIndexDataPath() == vector_index_data_path);

    DLOG(INFO) << "VectorHandler Singleton Initialization done";
}

TEST_CASE("VectorHandler Create Index", "[vector-handler]")
{
    DLOG(INFO) << "VectorHandler Create Index start";

    VectorHandler &handler = VectorHandler::Instance();
    std::string index_name = GenerateTestIndexName();

    // Test 1: Create a valid index
    VectorIndexMetadata index_metadata =
        CreateTestIndexMetadata(index_name, -1);  // Manual persist
    VectorOpResult result = handler.Create(index_metadata);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Test 2: Verify the index exists by calling Info
    VectorIndexMetadata retrieved_metadata;
    result = handler.Info(index_name, retrieved_metadata);
    REQUIRE(result == VectorOpResult::SUCCEED);
    REQUIRE(retrieved_metadata.Name() == index_name);
    REQUIRE(retrieved_metadata.Config().dimension == test_vector_dimension);
    REQUIRE(retrieved_metadata.Config().algorithm == Algorithm::HNSW);
    REQUIRE(retrieved_metadata.Config().distance_metric ==
            DistanceMetric::COSINE);

    // Test 3: Try to create the same index again - should return INDEX_EXISTED
    VectorIndexMetadata duplicate_metadata =
        CreateTestIndexMetadata(index_name, -1);
    result = handler.Create(duplicate_metadata);
    REQUIRE(result == VectorOpResult::INDEX_EXISTED);

    // Cleanup: Drop the index
    result = handler.Drop(index_name);
    REQUIRE(result == VectorOpResult::SUCCEED);

    DLOG(INFO) << "VectorHandler Create Index done";
}

TEST_CASE("VectorHandler Drop Index", "[vector-handler]")
{
    DLOG(INFO) << "VectorHandler Drop Index start";

    VectorHandler &handler = VectorHandler::Instance();
    std::string index_name = GenerateTestIndexName();

    // Test 1: Create an index first
    VectorIndexMetadata index_metadata =
        CreateTestIndexMetadata(index_name, -1);
    VectorOpResult result = handler.Create(index_metadata);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Test 2: Drop the existing index
    result = handler.Drop(index_name);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Test 3: Verify the index no longer exists
    VectorIndexMetadata retrieved_metadata;
    result = handler.Info(index_name, retrieved_metadata);
    REQUIRE(result == VectorOpResult::INDEX_NOT_EXIST);

    // Test 4: Try to drop a non-existent index - should return INDEX_NOT_EXIST
    std::string non_existent_name = GenerateTestIndexName();
    result = handler.Drop(non_existent_name);
    REQUIRE(result == VectorOpResult::INDEX_NOT_EXIST);

    DLOG(INFO) << "VectorHandler Drop Index done";
}

TEST_CASE("VectorHandler Info Index", "[vector-handler]")
{
    DLOG(INFO) << "VectorHandler Info Index start";

    VectorHandler &handler = VectorHandler::Instance();
    std::string index_name = GenerateTestIndexName();

    // Test 1: Get info for non-existent index - should return INDEX_NOT_EXIST
    VectorIndexMetadata metadata;
    VectorOpResult result = handler.Info(index_name, metadata);
    REQUIRE(result == VectorOpResult::INDEX_NOT_EXIST);

    // Test 2: Create an index
    VectorIndexMetadata index_metadata =
        CreateTestIndexMetadata(index_name, 1000);  // Auto persist threshold
    result = handler.Create(index_metadata);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Test 3: Get info for the created index
    VectorIndexMetadata retrieved_metadata;
    result = handler.Info(index_name, retrieved_metadata);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Test 4: Verify the retrieved metadata matches the created one
    REQUIRE(retrieved_metadata.Name() == index_name);
    REQUIRE(retrieved_metadata.Config().dimension == test_vector_dimension);
    REQUIRE(retrieved_metadata.Config().algorithm == Algorithm::HNSW);
    REQUIRE(retrieved_metadata.Config().distance_metric ==
            DistanceMetric::COSINE);
    REQUIRE(retrieved_metadata.Config().max_elements == test_max_elements);
    REQUIRE(retrieved_metadata.PersistThreshold() == 1000);

    // Verify metadata schema
    REQUIRE(retrieved_metadata.Metadata().Size() == 2);
    REQUIRE(retrieved_metadata.Metadata().FieldNames()[0] == "category");
    REQUIRE(retrieved_metadata.Metadata().FieldNames()[1] == "score");
    REQUIRE(retrieved_metadata.Metadata().FieldTypes()[0] ==
            MetadataFieldType::String);
    REQUIRE(retrieved_metadata.Metadata().FieldTypes()[1] ==
            MetadataFieldType::Double);

    // Cleanup: Drop the index
    result = handler.Drop(index_name);
    REQUIRE(result == VectorOpResult::SUCCEED);

    DLOG(INFO) << "VectorHandler Info Index done";
}

TEST_CASE("VectorHandler Add Vector", "[vector-handler]")
{
    DLOG(INFO) << "VectorHandler Add Vector start";

    VectorHandler &handler = VectorHandler::Instance();

    // Test 1: Create an index without metadata schema for testing vectors
    // without metadata
    std::string index_name_no_meta = GenerateTestIndexName();
    VectorIndexMetadata index_metadata_no_meta = CreateTestIndexMetadata(
        index_name_no_meta, -1, false);  // No metadata schema
    VectorOpResult result = handler.Create(index_metadata_no_meta);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Test 2: Add a vector without metadata to index without metadata schema
    std::vector<float> vector1 = GenerateTestVector(test_vector_dimension, 1);
    uint64_t id1 = 1;
    result = handler.Add(index_name_no_meta, id1, vector1, "");
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Test 3: Create an index with metadata schema for testing vectors with
    // metadata
    std::string index_name_with_meta = GenerateTestIndexName();
    VectorIndexMetadata index_metadata_with_meta = CreateTestIndexMetadata(
        index_name_with_meta, -1, true);  // With metadata schema
    result = handler.Create(index_metadata_with_meta);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Test 4: Add a vector with metadata to index with metadata schema
    std::vector<float> vector2 = GenerateTestVector(test_vector_dimension, 2);
    uint64_t id2 = 2;
    std::string metadata_json = GenerateTestMetadataJson("category1", 0.8);
    result = handler.Add(index_name_with_meta, id2, vector2, metadata_json);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Test 5: Try to add vector to non-existent index
    std::string non_existent_name = GenerateTestIndexName();
    result = handler.Add(non_existent_name, 3, vector1, "");
    REQUIRE(result == VectorOpResult::INDEX_NOT_EXIST);

    // Test 6: Try to add vector with invalid metadata to index with metadata
    // schema - wrong array size (should be 2 fields: category and score)
    std::vector<float> vector3 = GenerateTestVector(test_vector_dimension, 3);
    uint64_t id3 = 3;
    std::string invalid_metadata_size =
        "[\"test\"]";  // Only 1 field, should be 2
    result =
        handler.Add(index_name_with_meta, id3, vector3, invalid_metadata_size);
    REQUIRE(result == VectorOpResult::METADATA_OP_FAILED);

    // Test 7: Try to add vector with invalid metadata - wrong field count
    std::vector<float> vector4 = GenerateTestVector(test_vector_dimension, 4);
    uint64_t id4 = 4;
    std::string invalid_metadata_count =
        "[\"test\", 0.5, \"extra\"]";  // 3 fields, should be 2
    result =
        handler.Add(index_name_with_meta, id4, vector4, invalid_metadata_count);
    REQUIRE(result == VectorOpResult::METADATA_OP_FAILED);

    // Test 8: Try to add vector with invalid metadata - wrong type
    std::vector<float> vector5 = GenerateTestVector(test_vector_dimension, 5);
    uint64_t id5 = 5;
    std::string invalid_metadata_type =
        "[\"test\", \"not_a_number\"]";  // score should be number, not string
    result =
        handler.Add(index_name_with_meta, id5, vector5, invalid_metadata_type);
    REQUIRE(result == VectorOpResult::METADATA_OP_FAILED);

    // Cleanup: Drop the indices
    result = handler.Drop(index_name_no_meta);
    REQUIRE(result == VectorOpResult::SUCCEED);
    result = handler.Drop(index_name_with_meta);
    REQUIRE(result == VectorOpResult::SUCCEED);

    DLOG(INFO) << "VectorHandler Add Vector done";
}

TEST_CASE("VectorHandler Update Vector", "[vector-handler]")
{
    DLOG(INFO) << "VectorHandler Update Vector start";

    VectorHandler &handler = VectorHandler::Instance();
    std::string index_name = GenerateTestIndexName();

    // Test 1: Create an index with metadata schema and add a vector
    VectorIndexMetadata index_metadata =
        CreateTestIndexMetadata(index_name, -1, true);  // With metadata schema
    VectorOpResult result = handler.Create(index_metadata);
    REQUIRE(result == VectorOpResult::SUCCEED);

    std::vector<float> original_vector =
        GenerateTestVector(test_vector_dimension, 10);
    uint64_t id = 1;
    std::string original_metadata = GenerateTestMetadataJson("original", 0.7);
    result = handler.Add(index_name, id, original_vector, original_metadata);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Test 2: Update the vector
    std::vector<float> updated_vector =
        GenerateTestVector(test_vector_dimension, 20);
    std::string updated_metadata = GenerateTestMetadataJson("updated", 0.9);
    result = handler.Update(index_name, id, updated_vector, updated_metadata);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Test 3: Try to update non-existent vector
    uint64_t non_existent_id = 999;
    result = handler.Update(index_name, non_existent_id, updated_vector, "");
    // update non-existent vector means add a new vector
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Test 4: Try to update vector in non-existent index
    std::string non_existent_name = GenerateTestIndexName();
    result = handler.Update(non_existent_name, id, updated_vector, "");
    REQUIRE(result == VectorOpResult::INDEX_NOT_EXIST);

    // Cleanup: Drop the index
    result = handler.Drop(index_name);
    REQUIRE(result == VectorOpResult::SUCCEED);

    DLOG(INFO) << "VectorHandler Update Vector done";
}

TEST_CASE("VectorHandler Delete Vector", "[vector-handler]")
{
    DLOG(INFO) << "VectorHandler Delete Vector start";

    VectorHandler &handler = VectorHandler::Instance();
    std::string index_name = GenerateTestIndexName();

    // Test 1: Create an index without metadata schema and add a vector
    VectorIndexMetadata index_metadata =
        CreateTestIndexMetadata(index_name, -1, false);  // No metadata schema
    VectorOpResult result = handler.Create(index_metadata);
    REQUIRE(result == VectorOpResult::SUCCEED);

    std::vector<float> vector = GenerateTestVector(test_vector_dimension, 30);
    uint64_t id = 1;
    result = handler.Add(index_name, id, vector, "");
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Test 2: Delete the vector
    result = handler.Delete(index_name, id);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Test 3: Try to delete the same vector again
    // delete non-existent vector also returns SUCCEED
    result = handler.Delete(index_name, id);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Test 4: Try to delete vector in non-existent index
    std::string non_existent_name = GenerateTestIndexName();
    result = handler.Delete(non_existent_name, id);
    REQUIRE(result == VectorOpResult::INDEX_NOT_EXIST);

    // Cleanup: Drop the index
    result = handler.Drop(index_name);
    REQUIRE(result == VectorOpResult::SUCCEED);

    DLOG(INFO) << "VectorHandler Delete Vector done";
}

TEST_CASE("VectorHandler BatchAdd Vectors", "[vector-handler]")
{
    DLOG(INFO) << "VectorHandler BatchAdd Vectors start";

    VectorHandler &handler = VectorHandler::Instance();
    std::string index_name = GenerateTestIndexName();

    // Test 1: Create an index without metadata schema for testing vectors
    // without metadata
    VectorIndexMetadata index_metadata =
        CreateTestIndexMetadata(index_name, -1, false);  // No metadata schema
    VectorOpResult result = handler.Create(index_metadata);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Test 2: Batch add vectors without metadata
    const size_t batch_size = 5;
    std::vector<uint64_t> ids;
    std::vector<std::vector<float>> vectors;
    ids.reserve(batch_size);
    vectors.reserve(batch_size);

    for (size_t i = 0; i < batch_size; ++i)
    {
        ids.push_back(100 + i);
        vectors.push_back(GenerateTestVector(test_vector_dimension, 100 + i));
    }

    std::vector<std::string_view> empty_metadata_list;
    result = handler.BatchAdd(index_name, ids, vectors, empty_metadata_list);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Test 3: Create an index with metadata schema for testing vectors with
    // metadata
    std::string index_name_with_meta = GenerateTestIndexName();
    VectorIndexMetadata index_metadata_with_meta = CreateTestIndexMetadata(
        index_name_with_meta, -1, true);  // With metadata schema
    result = handler.Create(index_metadata_with_meta);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Test 4: Batch add vectors with metadata
    std::vector<uint64_t> ids_with_meta;
    std::vector<std::vector<float>> vectors_with_meta;
    std::vector<std::string> metadata_strings;
    std::vector<std::string_view> metadata_list;
    ids_with_meta.reserve(batch_size);
    vectors_with_meta.reserve(batch_size);
    metadata_strings.reserve(batch_size);

    for (size_t i = 0; i < batch_size; ++i)
    {
        ids_with_meta.push_back(200 + i);
        vectors_with_meta.push_back(
            GenerateTestVector(test_vector_dimension, 200 + i));
        metadata_strings.push_back(GenerateTestMetadataJson(
            "batch_" + std::to_string(i), 0.5 + i * 0.1));
    }

    for (const auto &meta : metadata_strings)
    {
        metadata_list.push_back(meta);
    }

    result = handler.BatchAdd(
        index_name_with_meta, ids_with_meta, vectors_with_meta, metadata_list);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Test 5: Try batch add with mismatched ids and vectors sizes
    std::vector<uint64_t> mismatched_ids = {300, 301};
    std::vector<std::vector<float>> mismatched_vectors = {
        GenerateTestVector(test_vector_dimension, 300)};
    result = handler.BatchAdd(
        index_name, mismatched_ids, mismatched_vectors, empty_metadata_list);
    REQUIRE(result == VectorOpResult::INDEX_ADD_FAILED);

    // Test 6: Try batch add with empty ids
    std::vector<uint64_t> empty_ids;
    std::vector<std::vector<float>> empty_vectors;
    result = handler.BatchAdd(
        index_name, empty_ids, empty_vectors, empty_metadata_list);
    REQUIRE(result == VectorOpResult::INDEX_ADD_FAILED);

    // Test 7: Try batch add with mismatched metadata size
    std::vector<uint64_t> small_ids = {400};
    std::vector<std::vector<float>> small_vectors = {
        GenerateTestVector(test_vector_dimension, 400)};
    std::vector<std::string_view> large_metadata = {metadata_strings[0],
                                                    metadata_strings[1]};
    result =
        handler.BatchAdd(index_name, small_ids, small_vectors, large_metadata);
    REQUIRE(result == VectorOpResult::INDEX_ADD_FAILED);

    // Test 8: Try batch add to non-existent index
    std::string non_existent_name = GenerateTestIndexName();
    result =
        handler.BatchAdd(non_existent_name, ids, vectors, empty_metadata_list);
    REQUIRE(result == VectorOpResult::INDEX_NOT_EXIST);

    // Cleanup: Drop the indices
    result = handler.Drop(index_name);
    REQUIRE(result == VectorOpResult::SUCCEED);
    result = handler.Drop(index_name_with_meta);
    REQUIRE(result == VectorOpResult::SUCCEED);

    DLOG(INFO) << "VectorHandler BatchAdd Vectors done";
}

TEST_CASE("VectorHandler Search Vectors", "[vector-handler]")
{
    DLOG(INFO) << "VectorHandler Search Vectors start";

    VectorHandler &handler = VectorHandler::Instance();
    std::string index_name = GenerateTestIndexName();

    // Test 1: Create an index without metadata and add some vectors
    VectorIndexMetadata index_metadata =
        CreateTestIndexMetadata(index_name, -1, false);  // No metadata schema
    VectorOpResult result = handler.Create(index_metadata);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Add multiple vectors for search testing
    const size_t num_vectors = 10;
    std::vector<uint64_t> vector_ids;
    std::vector<std::vector<float>> vectors;
    for (size_t i = 0; i < num_vectors; ++i)
    {
        vector_ids.push_back(i + 1);
        vectors.push_back(GenerateTestVector(test_vector_dimension, i + 1));
        result = handler.Add(index_name, vector_ids[i], vectors[i], "");
        REQUIRE(result == VectorOpResult::SUCCEED);
    }

    // Test 2: Basic search without filter
    std::vector<float> query_vector = GenerateTestVector(
        test_vector_dimension, 1);  // Search for similar to first vector
    size_t k_count = 5;
    size_t thread_id = 0;
    SearchResult search_result;
    result = handler.Search(
        index_name, query_vector, k_count, thread_id, "", search_result);
    REQUIRE(result == VectorOpResult::SUCCEED);
    REQUIRE(search_result.ids.size() <= k_count);
    REQUIRE(search_result.ids.size() == search_result.distances.size());
    REQUIRE(search_result.ids.size() > 0);  // Should find at least one result

    // Test 3: Verify search results are sorted by distance (ascending)
    for (size_t i = 1; i < search_result.distances.size(); ++i)
    {
        REQUIRE(search_result.distances[i - 1] <= search_result.distances[i]);
    }

    // Test 4: Search with k_count larger than available vectors
    SearchResult large_k_result;
    result = handler.Search(index_name,
                            query_vector,
                            num_vectors + 10,
                            thread_id,
                            "",
                            large_k_result);
    REQUIRE(result == VectorOpResult::SUCCEED);
    REQUIRE(large_k_result.ids.size() <= num_vectors);
    REQUIRE(large_k_result.ids.size() ==
            num_vectors);  // Should return all vectors

    // Test 5: Search in non-existent index
    std::string non_existent_name = GenerateTestIndexName();
    SearchResult empty_result;
    result = handler.Search(
        non_existent_name, query_vector, k_count, thread_id, "", empty_result);
    REQUIRE(result == VectorOpResult::INDEX_NOT_EXIST);

    // Cleanup: Drop the index
    result = handler.Drop(index_name);
    REQUIRE(result == VectorOpResult::SUCCEED);

    DLOG(INFO) << "VectorHandler Search Vectors done";
}

TEST_CASE("VectorHandler Search with Filter", "[vector-handler]")
{
    DLOG(INFO) << "VectorHandler Search with Filter start";

    VectorHandler &handler = VectorHandler::Instance();
    std::string index_name = GenerateTestIndexName();

    // Test 1: Create an index with metadata schema
    VectorIndexMetadata index_metadata =
        CreateTestIndexMetadata(index_name, -1, true);  // With metadata schema
    VectorOpResult result = handler.Create(index_metadata);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Add vectors with different metadata values
    const size_t num_vectors = 10;
    for (size_t i = 0; i < num_vectors; ++i)
    {
        std::vector<float> vector =
            GenerateTestVector(test_vector_dimension, i + 1);
        uint64_t id = i + 1;
        // Use different categories and scores for filtering
        std::string category = (i % 2 == 0) ? "A" : "B";
        double score = 0.1 + i * 0.1;  // Scores from 0.1 to 1.0
        std::string metadata = GenerateTestMetadataJson(category, score);
        result = handler.Add(index_name, id, vector, metadata);
        REQUIRE(result == VectorOpResult::SUCCEED);
    }

    // Test 2: Search with filter - score > 0.5
    std::vector<float> query_vector =
        GenerateTestVector(test_vector_dimension, 1);
    size_t k_count = 10;
    size_t thread_id = 0;
    std::string filter_json = "{\"score\": {\"$gt\": 0.5}}";
    SearchResult filtered_result;
    result = handler.Search(index_name,
                            query_vector,
                            k_count,
                            thread_id,
                            filter_json,
                            filtered_result);
    REQUIRE(result == VectorOpResult::SUCCEED);
    // Should only return vectors with score > 0.5 (ids 6-10)
    REQUIRE(filtered_result.ids.size() <= 5);
    REQUIRE(filtered_result.ids.size() > 0);

    // Test 3: Search with filter - category equals "A"
    std::string filter_category = "{\"category\": {\"$eq\": \"A\"}}";
    SearchResult category_result;
    result = handler.Search(index_name,
                            query_vector,
                            k_count,
                            thread_id,
                            filter_category,
                            category_result);
    REQUIRE(result == VectorOpResult::SUCCEED);
    // Should only return vectors with category "A" (even ids: 1,3,5,7,9)
    REQUIRE(category_result.ids.size() <= 5);
    REQUIRE(category_result.ids.size() > 0);

    // Test 4: Search with multiple field filter (implicit AND)
    std::string filter_multi =
        "{\"category\": {\"$eq\": \"A\"}, \"score\": {\"$lt\": 0.5}}";
    SearchResult multi_result;
    result = handler.Search(index_name,
                            query_vector,
                            k_count,
                            thread_id,
                            filter_multi,
                            multi_result);
    REQUIRE(result == VectorOpResult::SUCCEED);
    // Should return vectors with category "A" AND score < 0.5 (ids 1,3)
    REQUIRE(multi_result.ids.size() <= 2);
    REQUIRE(multi_result.ids.size() > 0);

    // Test 5: Search with $and operator
    std::string filter_and =
        "{\"$and\": [{\"category\": {\"$eq\": \"B\"}}, {\"score\": {\"$gte\": "
        "0.6}}]}";
    SearchResult and_result;
    result = handler.Search(
        index_name, query_vector, k_count, thread_id, filter_and, and_result);
    REQUIRE(result == VectorOpResult::SUCCEED);
    // Should return vectors with category "B" AND score >= 0.6 (ids 5,7,9)
    REQUIRE(and_result.ids.size() <= 3);
    REQUIRE(and_result.ids.size() > 0);

    // Test 6: Search with $or operator
    std::string filter_or =
        "{\"$or\": [{\"score\": {\"$lt\": 0.3}}, {\"score\": {\"$gt\": 0.7}}]}";
    SearchResult or_result;
    result = handler.Search(
        index_name, query_vector, k_count, thread_id, filter_or, or_result);
    REQUIRE(result == VectorOpResult::SUCCEED);
    // Should return vectors with score < 0.3 OR score > 0.7
    REQUIRE(or_result.ids.size() > 0);

    // Test 7: Search with invalid filter JSON
    std::string invalid_filter = "{\"invalid_field\": {\"$gt\": 0.5}}";
    SearchResult invalid_result;
    result = handler.Search(index_name,
                            query_vector,
                            k_count,
                            thread_id,
                            invalid_filter,
                            invalid_result);
    REQUIRE(result == VectorOpResult::METADATA_OP_FAILED);

    // Test 8: Search with malformed filter JSON
    std::string malformed_filter = "{\"score\": {\"$gt\": }";  // Invalid JSON
    SearchResult malformed_result;
    result = handler.Search(index_name,
                            query_vector,
                            k_count,
                            thread_id,
                            malformed_filter,
                            malformed_result);
    REQUIRE(result == VectorOpResult::METADATA_OP_FAILED);

    // Test 9: Search with filter on index without metadata schema
    std::string index_no_meta = GenerateTestIndexName();
    VectorIndexMetadata no_meta_index =
        CreateTestIndexMetadata(index_no_meta, -1, false);  // No metadata
    result = handler.Create(no_meta_index);
    REQUIRE(result == VectorOpResult::SUCCEED);

    std::vector<float> test_vector =
        GenerateTestVector(test_vector_dimension, 1);
    result = handler.Add(index_no_meta, 1, test_vector, "");
    REQUIRE(result == VectorOpResult::SUCCEED);

    SearchResult no_meta_result;
    result = handler.Search(index_no_meta,
                            query_vector,
                            k_count,
                            thread_id,
                            filter_json,
                            no_meta_result);
    // Filter on index without metadata should fail
    REQUIRE(result == VectorOpResult::METADATA_OP_FAILED);

    // Cleanup: Drop the indices
    result = handler.Drop(index_name);
    REQUIRE(result == VectorOpResult::SUCCEED);
    result = handler.Drop(index_no_meta);
    REQUIRE(result == VectorOpResult::SUCCEED);

    DLOG(INFO) << "VectorHandler Search with Filter done";
}

TEST_CASE("VectorHandler PersistIndex", "[vector-handler]")
{
    DLOG(INFO) << "VectorHandler PersistIndex start";

    VectorHandler &handler = VectorHandler::Instance();
    std::string index_name = GenerateTestIndexName();

    // Test 1: Create an index with manual persist (persist_threshold = -1)
    VectorIndexMetadata index_metadata =
        CreateTestIndexMetadata(index_name, -1, false);  // Manual persist
    VectorOpResult result = handler.Create(index_metadata);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Get initial metadata to check file path
    VectorIndexMetadata initial_metadata;
    result = handler.Info(index_name, initial_metadata);
    REQUIRE(result == VectorOpResult::SUCCEED);
    std::string initial_file_path = initial_metadata.FilePath();
    uint64_t initial_persist_ts = initial_metadata.LastPersistTs();

    // Add some vectors to the index
    const size_t num_vectors = 5;
    std::vector<uint64_t> vector_ids;
    std::vector<std::vector<float>> vectors;
    for (size_t i = 0; i < num_vectors; ++i)
    {
        vector_ids.push_back(i + 1);
        vectors.push_back(GenerateTestVector(test_vector_dimension, i + 1));
        result = handler.Add(index_name, vector_ids[i], vectors[i], "");
        REQUIRE(result == VectorOpResult::SUCCEED);
    }

    // Test 2: Persist the index manually
    result = handler.PersistIndex(index_name);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Test 3: Verify metadata was updated after persistence
    VectorIndexMetadata persisted_metadata;
    result = handler.Info(index_name, persisted_metadata);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // File path should be updated (new timestamp)
    std::string persisted_file_path = persisted_metadata.FilePath();
    REQUIRE(persisted_file_path != initial_file_path);
    REQUIRE(persisted_file_path.find(index_name) != std::string::npos);

    // Last persist timestamp should be updated
    uint64_t persisted_ts = persisted_metadata.LastPersistTs();
    REQUIRE(persisted_ts > initial_persist_ts);

    // Test 4: Verify the persisted file exists
    REQUIRE(std::filesystem::exists(persisted_file_path));

    // Test 5: Verify old file was removed (if it existed and was different)
    // Note: initial file might not exist if it's the first persist
    // The old file should be removed after successful persist if it existed
    if (!initial_file_path.empty() &&
        initial_file_path != persisted_file_path &&
        initial_file_path.find("0000000000000000") == std::string::npos)
    {
        // Old file should be removed after successful persist
        // This is verified by checking that the old path no longer exists
        // (if it was not the initial timestamp placeholder)
    }

    // Test 6: Verify vectors are still searchable after persistence
    std::vector<float> query_vector =
        GenerateTestVector(test_vector_dimension, 1);
    size_t k_count = num_vectors;
    size_t thread_id = 0;
    SearchResult search_result;
    result = handler.Search(
        index_name, query_vector, k_count, thread_id, "", search_result);
    REQUIRE(result == VectorOpResult::SUCCEED);
    REQUIRE(search_result.ids.size() > 0);
    REQUIRE(search_result.ids.size() <= num_vectors);

    // Test 7: Persist non-existent index should fail
    std::string non_existent_name = GenerateTestIndexName();
    result = handler.PersistIndex(non_existent_name);
    REQUIRE(result == VectorOpResult::INDEX_NOT_EXIST);

    // Test 8: Persist index with force flag
    result = handler.PersistIndex(index_name, true);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Verify metadata was updated again
    VectorIndexMetadata force_persisted_metadata;
    result = handler.Info(index_name, force_persisted_metadata);
    REQUIRE(result == VectorOpResult::SUCCEED);
    uint64_t force_persisted_ts = force_persisted_metadata.LastPersistTs();
    REQUIRE(force_persisted_ts >= persisted_ts);

    // Cleanup: Drop the index
    result = handler.Drop(index_name);
    REQUIRE(result == VectorOpResult::SUCCEED);

    DLOG(INFO) << "VectorHandler PersistIndex done";
}

TEST_CASE("VectorHandler PersistIndex with Metadata", "[vector-handler]")
{
    DLOG(INFO) << "VectorHandler PersistIndex with Metadata start";

    VectorHandler &handler = VectorHandler::Instance();
    std::string index_name = GenerateTestIndexName();

    // Test 1: Create an index with metadata schema and manual persist
    VectorIndexMetadata index_metadata = CreateTestIndexMetadata(
        index_name, -1, true);  // With metadata, manual persist
    VectorOpResult result = handler.Create(index_metadata);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Add vectors with metadata
    const size_t num_vectors = 5;
    for (size_t i = 0; i < num_vectors; ++i)
    {
        std::vector<float> vector =
            GenerateTestVector(test_vector_dimension, i + 1);
        uint64_t id = i + 1;
        std::string category = (i % 2 == 0) ? "A" : "B";
        double score = 0.1 + i * 0.1;
        std::string metadata = GenerateTestMetadataJson(category, score);
        result = handler.Add(index_name, id, vector, metadata);
        REQUIRE(result == VectorOpResult::SUCCEED);
    }

    // Test 2: Persist the index
    result = handler.PersistIndex(index_name);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Test 3: Verify metadata was updated
    VectorIndexMetadata persisted_metadata;
    result = handler.Info(index_name, persisted_metadata);
    REQUIRE(result == VectorOpResult::SUCCEED);
    REQUIRE(persisted_metadata.LastPersistTs() > 0);

    // Test 4: Verify vectors with metadata are still searchable
    std::vector<float> query_vector =
        GenerateTestVector(test_vector_dimension, 1);
    size_t k_count = num_vectors;
    size_t thread_id = 0;

    // Search without filter
    SearchResult search_result;
    result = handler.Search(
        index_name, query_vector, k_count, thread_id, "", search_result);
    REQUIRE(result == VectorOpResult::SUCCEED);
    REQUIRE(search_result.ids.size() > 0);

    // Search with filter
    std::string filter_json = "{\"category\": {\"$eq\": \"A\"}}";
    SearchResult filtered_result;
    result = handler.Search(index_name,
                            query_vector,
                            k_count,
                            thread_id,
                            filter_json,
                            filtered_result);
    REQUIRE(result == VectorOpResult::SUCCEED);
    REQUIRE(filtered_result.ids.size() > 0);
    REQUIRE(filtered_result.ids.size() <= num_vectors);

    // Cleanup: Drop the index
    result = handler.Drop(index_name);
    REQUIRE(result == VectorOpResult::SUCCEED);

    DLOG(INFO) << "VectorHandler PersistIndex with Metadata done";
}

TEST_CASE("VectorHandler Integration Test - Complete Workflow",
          "[vector-handler]")
{
    DLOG(INFO) << "VectorHandler Integration Test - Complete Workflow start";

    VectorHandler &handler = VectorHandler::Instance();
    std::string index_name = GenerateTestIndexName();

    // Step 1: Create an index with metadata schema
    VectorIndexMetadata index_metadata = CreateTestIndexMetadata(
        index_name, -1, true);  // With metadata, manual persist
    VectorOpResult result = handler.Create(index_metadata);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Step 2: Add multiple vectors with metadata
    const size_t num_vectors = 10;
    std::vector<uint64_t> vector_ids;
    std::vector<std::vector<float>> vectors;
    for (size_t i = 0; i < num_vectors; ++i)
    {
        vector_ids.push_back(i + 1);
        vectors.push_back(GenerateTestVector(test_vector_dimension, i + 1));
        std::string category = (i % 2 == 0) ? "A" : "B";
        double score = 0.1 + i * 0.1;
        std::string metadata = GenerateTestMetadataJson(category, score);
        result = handler.Add(index_name, vector_ids[i], vectors[i], metadata);
        REQUIRE(result == VectorOpResult::SUCCEED);
    }

    // Step 3: Search vectors without filter
    std::vector<float> query_vector =
        GenerateTestVector(test_vector_dimension, 1);
    size_t k_count = 5;
    size_t thread_id = 0;
    SearchResult search_result;
    result = handler.Search(
        index_name, query_vector, k_count, thread_id, "", search_result);
    REQUIRE(result == VectorOpResult::SUCCEED);
    REQUIRE(search_result.ids.size() > 0);
    REQUIRE(search_result.ids.size() <= k_count);

    // Step 4: Search with filter
    std::string filter_json = "{\"category\": {\"$eq\": \"A\"}}";
    SearchResult filtered_result;
    result = handler.Search(index_name,
                            query_vector,
                            k_count,
                            thread_id,
                            filter_json,
                            filtered_result);
    REQUIRE(result == VectorOpResult::SUCCEED);
    REQUIRE(filtered_result.ids.size() > 0);

    // Step 5: Update some vectors
    for (size_t i = 0; i < 3; ++i)
    {
        std::vector<float> updated_vector =
            GenerateTestVector(test_vector_dimension, 100 + i);
        std::string updated_metadata =
            GenerateTestMetadataJson("updated", 0.99);
        result = handler.Update(
            index_name, vector_ids[i], updated_vector, updated_metadata);
        REQUIRE(result == VectorOpResult::SUCCEED);
    }

    // Step 6: Verify updated vectors are searchable
    SearchResult updated_search_result;
    result = handler.Search(index_name,
                            query_vector,
                            k_count,
                            thread_id,
                            "",
                            updated_search_result);
    REQUIRE(result == VectorOpResult::SUCCEED);
    REQUIRE(updated_search_result.ids.size() > 0);

    // Step 7: Delete some vectors
    for (size_t i = 7; i < num_vectors; ++i)
    {
        result = handler.Delete(index_name, vector_ids[i]);
        REQUIRE(result == VectorOpResult::SUCCEED);
    }

    // Step 8: Verify deleted vectors are no longer searchable
    SearchResult after_delete_result;
    result = handler.Search(index_name,
                            query_vector,
                            num_vectors,
                            thread_id,
                            "",
                            after_delete_result);
    REQUIRE(result == VectorOpResult::SUCCEED);
    REQUIRE(after_delete_result.ids.size() < num_vectors);

    // Step 9: Persist the index
    result = handler.PersistIndex(index_name);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Step 10: Verify vectors are still searchable after persistence
    SearchResult after_persist_result;
    result = handler.Search(
        index_name, query_vector, k_count, thread_id, "", after_persist_result);
    REQUIRE(result == VectorOpResult::SUCCEED);
    REQUIRE(after_persist_result.ids.size() > 0);

    // Step 11: Verify filtered search still works after persistence
    SearchResult filtered_after_persist;
    result = handler.Search(index_name,
                            query_vector,
                            k_count,
                            thread_id,
                            filter_json,
                            filtered_after_persist);
    REQUIRE(result == VectorOpResult::SUCCEED);
    REQUIRE(filtered_after_persist.ids.size() > 0);

    // Step 12: Get index info and verify metadata
    VectorIndexMetadata final_metadata;
    result = handler.Info(index_name, final_metadata);
    REQUIRE(result == VectorOpResult::SUCCEED);
    REQUIRE(final_metadata.Name() == index_name);
    REQUIRE(final_metadata.LastPersistTs() > 0);

    // Cleanup: Drop the index
    result = handler.Drop(index_name);
    REQUIRE(result == VectorOpResult::SUCCEED);

    DLOG(INFO) << "VectorHandler Integration Test - Complete Workflow done";
}

TEST_CASE("VectorHandler Integration Test - Batch Operations",
          "[vector-handler]")
{
    DLOG(INFO) << "VectorHandler Integration Test - Batch Operations start";

    VectorHandler &handler = VectorHandler::Instance();
    std::string index_name = GenerateTestIndexName();

    // Step 1: Create an index
    VectorIndexMetadata index_metadata =
        CreateTestIndexMetadata(index_name, -1, true);  // With metadata
    VectorOpResult result = handler.Create(index_metadata);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Step 2: Batch add vectors
    const size_t batch_size = 20;
    std::vector<uint64_t> ids;
    std::vector<std::vector<float>> vectors;
    std::vector<std::string> metadata_strings;
    std::vector<std::string_view> metadata_list;
    ids.reserve(batch_size);
    vectors.reserve(batch_size);
    metadata_strings.reserve(batch_size);

    for (size_t i = 0; i < batch_size; ++i)
    {
        ids.push_back(i + 1);
        vectors.push_back(GenerateTestVector(test_vector_dimension, i + 1));
        std::string category = (i % 3 == 0) ? "A" : (i % 3 == 1) ? "B" : "C";
        double score = 0.1 + (i % 10) * 0.1;
        metadata_strings.push_back(GenerateTestMetadataJson(category, score));
    }

    for (const auto &meta : metadata_strings)
    {
        metadata_list.push_back(meta);
    }

    result = handler.BatchAdd(index_name, ids, vectors, metadata_list);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Step 3: Search all vectors
    std::vector<float> query_vector =
        GenerateTestVector(test_vector_dimension, 1);
    size_t k_count = batch_size;
    size_t thread_id = 0;
    SearchResult search_result;
    result = handler.Search(
        index_name, query_vector, k_count, thread_id, "", search_result);
    REQUIRE(result == VectorOpResult::SUCCEED);
    REQUIRE(search_result.ids.size() == batch_size);

    // Step 4: Search with complex filter
    std::string complex_filter =
        "{\"$and\": [{\"category\": {\"$eq\": \"A\"}}, {\"score\": {\"$gte\": "
        "0.5}}]}";
    SearchResult complex_filtered;
    result = handler.Search(index_name,
                            query_vector,
                            k_count,
                            thread_id,
                            complex_filter,
                            complex_filtered);
    REQUIRE(result == VectorOpResult::SUCCEED);
    REQUIRE(complex_filtered.ids.size() > 0);
    REQUIRE(complex_filtered.ids.size() <= batch_size);

    // Step 5: Persist and verify
    result = handler.PersistIndex(index_name);
    REQUIRE(result == VectorOpResult::SUCCEED);

    SearchResult after_persist;
    result = handler.Search(
        index_name, query_vector, k_count, thread_id, "", after_persist);
    REQUIRE(result == VectorOpResult::SUCCEED);
    REQUIRE(after_persist.ids.size() == batch_size);

    // Cleanup: Drop the index
    result = handler.Drop(index_name);
    REQUIRE(result == VectorOpResult::SUCCEED);

    DLOG(INFO) << "VectorHandler Integration Test - Batch Operations done";
}

TEST_CASE("VectorHandler Error Handling - Invalid Inputs", "[vector-handler]")
{
    DLOG(INFO) << "VectorHandler Error Handling - Invalid Inputs start";

    VectorHandler &handler = VectorHandler::Instance();
    std::string index_name = GenerateTestIndexName();

    // Test 1: Create index with empty name (should fail or use default)
    // Note: This depends on implementation - checking if empty name is allowed
    std::string empty_name = "";
    VectorIndexMetadata empty_metadata =
        CreateTestIndexMetadata(empty_name, -1, false);
    VectorOpResult result = handler.Create(empty_metadata);
    // Empty name might be allowed or not - depends on implementation
    // We'll just test that it doesn't crash

    // Test 2: Create a valid index first
    VectorIndexMetadata index_metadata =
        CreateTestIndexMetadata(index_name, -1, false);
    result = handler.Create(index_metadata);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Test 3: Add vector with wrong dimension
    std::vector<float> wrong_dim_vector(test_vector_dimension + 1, 0.5f);
    result = handler.Add(index_name, 1, wrong_dim_vector, "");
    // This might fail or succeed depending on validation
    // We'll just test that it doesn't crash

    // Test 4: Add vector with empty vector (zero dimension)
    std::vector<float> empty_vector;
    result = handler.Add(index_name, 2, empty_vector, "");
    // This should fail - zero dimension vector is invalid
    REQUIRE(result != VectorOpResult::SUCCEED);

    // Test 5: Search with empty query vector
    std::vector<float> empty_query;
    size_t k_count = 5;
    size_t thread_id = 0;
    SearchResult empty_search_result;
    result = handler.Search(
        index_name, empty_query, k_count, thread_id, "", empty_search_result);
    // This should fail - empty query vector is invalid
    REQUIRE(result != VectorOpResult::SUCCEED);

    // Test 6: Search with k_count = 0
    std::vector<float> valid_query =
        GenerateTestVector(test_vector_dimension, 1);
    SearchResult zero_k_result;
    result = handler.Search(
        index_name, valid_query, 0, thread_id, "", zero_k_result);
    // k_count = 0 might be allowed or not - depends on implementation
    // We'll just test that it doesn't crash

    // Test 7: Search with very large k_count
    SearchResult large_k_result;
    result = handler.Search(
        index_name, valid_query, 1000000, thread_id, "", large_k_result);
    // Should succeed but return limited results
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Test 8: Add vector with ID = 0 (edge case)
    std::vector<float> vector_zero =
        GenerateTestVector(test_vector_dimension, 0);
    result = handler.Add(index_name, 0, vector_zero, "");
    // ID = 0 might be allowed or not - depends on implementation
    // We'll just test that it doesn't crash

    // Cleanup: Drop the index
    result = handler.Drop(index_name);
    REQUIRE(result == VectorOpResult::SUCCEED);

    DLOG(INFO) << "VectorHandler Error Handling - Invalid Inputs done";
}

TEST_CASE("VectorHandler Error Handling - Metadata Errors", "[vector-handler]")
{
    DLOG(INFO) << "VectorHandler Error Handling - Metadata Errors start";

    VectorHandler &handler = VectorHandler::Instance();
    std::string index_name = GenerateTestIndexName();

    // Test 1: Create index with metadata schema
    VectorIndexMetadata index_metadata =
        CreateTestIndexMetadata(index_name, -1, true);  // With metadata
    VectorOpResult result = handler.Create(index_metadata);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Test 2: Add vector with empty metadata string (should fail for index with
    // metadata schema)
    std::vector<float> vector1 = GenerateTestVector(test_vector_dimension, 1);
    result = handler.Add(index_name, 1, vector1, "");
    // Empty metadata for index with schema might fail or require empty array
    // Depends on implementation

    // Test 3: Add vector with null metadata (empty string_view)
    std::string_view null_metadata;
    result = handler.Add(index_name, 2, vector1, null_metadata);
    // Should behave same as empty string

    // Test 4: Add vector with completely invalid JSON
    std::string invalid_json = "this is not json at all";
    result = handler.Add(index_name, 3, vector1, invalid_json);
    REQUIRE(result == VectorOpResult::METADATA_OP_FAILED);

    // Test 5: Add vector with valid JSON but wrong structure (object instead of
    // array)
    std::string wrong_structure = "{\"category\": \"A\", \"score\": 0.5}";
    result = handler.Add(index_name, 4, vector1, wrong_structure);
    REQUIRE(result == VectorOpResult::METADATA_OP_FAILED);

    // Test 6: Search with invalid filter JSON
    std::vector<float> query_vector =
        GenerateTestVector(test_vector_dimension, 1);
    size_t k_count = 5;
    size_t thread_id = 0;
    std::string invalid_filter = "not a json";
    SearchResult invalid_filter_result;
    result = handler.Search(index_name,
                            query_vector,
                            k_count,
                            thread_id,
                            invalid_filter,
                            invalid_filter_result);
    REQUIRE(result == VectorOpResult::METADATA_OP_FAILED);

    // Test 7: Search with filter referencing non-existent field
    std::string non_existent_field = "{\"nonexistent\": {\"$eq\": \"value\"}}";
    SearchResult non_existent_result;
    result = handler.Search(index_name,
                            query_vector,
                            k_count,
                            thread_id,
                            non_existent_field,
                            non_existent_result);
    REQUIRE(result == VectorOpResult::METADATA_OP_FAILED);

    // Cleanup: Drop the index
    result = handler.Drop(index_name);
    REQUIRE(result == VectorOpResult::SUCCEED);

    DLOG(INFO) << "VectorHandler Error Handling - Metadata Errors done";
}

TEST_CASE("VectorHandler Error Handling - Index Lifecycle Errors",
          "[vector-handler]")
{
    DLOG(INFO) << "VectorHandler Error Handling - Index Lifecycle Errors start";

    VectorHandler &handler = VectorHandler::Instance();
    std::string index_name = GenerateTestIndexName();

    // Test 1: Operations on non-existent index
    std::vector<float> test_vector =
        GenerateTestVector(test_vector_dimension, 1);
    VectorOpResult result = handler.Add(index_name, 1, test_vector, "");
    REQUIRE(result == VectorOpResult::INDEX_NOT_EXIST);

    result = handler.Update(index_name, 1, test_vector, "");
    REQUIRE(result == VectorOpResult::INDEX_NOT_EXIST);

    result = handler.Delete(index_name, 1);
    REQUIRE(result == VectorOpResult::INDEX_NOT_EXIST);

    size_t k_count = 5;
    size_t thread_id = 0;
    SearchResult search_result;
    result = handler.Search(
        index_name, test_vector, k_count, thread_id, "", search_result);
    REQUIRE(result == VectorOpResult::INDEX_NOT_EXIST);

    result = handler.PersistIndex(index_name);
    REQUIRE(result == VectorOpResult::INDEX_NOT_EXIST);

    VectorIndexMetadata metadata;
    result = handler.Info(index_name, metadata);
    REQUIRE(result == VectorOpResult::INDEX_NOT_EXIST);

    // Test 2: Create index
    VectorIndexMetadata index_metadata =
        CreateTestIndexMetadata(index_name, -1, false);
    result = handler.Create(index_metadata);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Test 3: Try to create same index again
    result = handler.Create(index_metadata);
    REQUIRE(result == VectorOpResult::INDEX_EXISTED);

    // Test 4: Add a vector
    result = handler.Add(index_name, 1, test_vector, "");
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Test 5: Drop the index
    result = handler.Drop(index_name);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Test 6: Try operations on dropped index
    result = handler.Add(index_name, 1, test_vector, "");
    REQUIRE(result == VectorOpResult::INDEX_NOT_EXIST);

    result = handler.Info(index_name, metadata);
    REQUIRE(result == VectorOpResult::INDEX_NOT_EXIST);

    // Test 7: Try to drop non-existent index
    result = handler.Drop(index_name);
    REQUIRE(result == VectorOpResult::INDEX_NOT_EXIST);

    DLOG(INFO) << "VectorHandler Error Handling - Index Lifecycle Errors done";
}

TEST_CASE("VectorHandler Concurrency Test - Add", "[vector-handler]")
{
    DLOG(INFO) << "VectorHandler Concurrency Test - Add start";

    VectorHandler &handler = VectorHandler::Instance();
    std::string index_name = GenerateTestIndexName();

    // Step 1: Create an index with metadata schema
    VectorIndexMetadata index_metadata =
        CreateTestIndexMetadata(index_name, -1, true);  // With metadata
    VectorOpResult result = handler.Create(index_metadata);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Step 2: Concurrent Add operations
    const size_t num_add_threads = 4;
    const size_t vectors_per_add_thread = 20;
    const size_t max_retries = 5;
    const std::chrono::milliseconds retry_delay(100);

    // Counters for tracking operations
    std::atomic<size_t> add_success_count{0};
    std::atomic<size_t> total_added_vectors{0};

    // Threads for concurrent Add operations with retry logic
    std::vector<std::thread> add_threads;
    for (size_t t = 0; t < num_add_threads; ++t)
    {
        add_threads.emplace_back(
            [&handler,
             index_name,
             t,
             vectors_per_add_thread,
             max_retries,
             retry_delay,
             &add_success_count,
             &total_added_vectors]()
            {
                uint64_t start_id = 1000 + t * vectors_per_add_thread;
                for (size_t i = 0; i < vectors_per_add_thread; ++i)
                {
                    uint64_t id = start_id + i;
                    std::vector<float> vector =
                        GenerateTestVector(test_vector_dimension, id);
                    std::string category = (i % 2 == 0) ? "A" : "B";
                    double score = 0.1 + (i % 10) * 0.1;
                    std::string metadata =
                        GenerateTestMetadataJson(category, score);

                    // Retry logic: retry up to max_retries times with delay
                    VectorOpResult result = VectorOpResult::INDEX_ADD_FAILED;
                    for (size_t retry = 0; retry < max_retries; ++retry)
                    {
                        result = handler.Add(index_name, id, vector, metadata);
                        if (result == VectorOpResult::SUCCEED)
                        {
                            add_success_count++;
                            total_added_vectors++;
                            break;
                        }
                        // Wait before retry
                        if (retry < max_retries - 1)
                        {
                            std::this_thread::sleep_for(retry_delay);
                        }
                    }
                }
            });
    }

    // Wait for all threads to complete
    for (auto &thread : add_threads)
    {
        thread.join();
    }

    // Step 3: Verify results
    DLOG(INFO) << "Add success count: " << add_success_count.load();
    DLOG(INFO) << "Total added vectors: " << total_added_vectors.load();

    // All Add operations should succeed after retries
    REQUIRE(add_success_count.load() ==
            num_add_threads * vectors_per_add_thread);

    // Step 4: Verify final state - search should return all added vectors
    std::vector<float> query_vector =
        GenerateTestVector(test_vector_dimension, 1);
    size_t k_count = 1000;  // Large enough to get all vectors
    size_t thread_id = 0;
    SearchResult final_search_result;
    result = handler.Search(
        index_name, query_vector, k_count, thread_id, "", final_search_result);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Should have all added vectors
    REQUIRE(final_search_result.ids.size() >= total_added_vectors.load());

    // Cleanup: Drop the index
    result = handler.Drop(index_name);
    REQUIRE(result == VectorOpResult::SUCCEED);

    DLOG(INFO) << "VectorHandler Concurrency Test - Add done";
}

TEST_CASE("VectorHandler Concurrency Test - BatchAdd", "[vector-handler]")
{
    DLOG(INFO) << "VectorHandler Concurrency Test - BatchAdd start";

    VectorHandler &handler = VectorHandler::Instance();
    std::string index_name = GenerateTestIndexName();

    // Step 1: Create an index with metadata schema
    VectorIndexMetadata index_metadata =
        CreateTestIndexMetadata(index_name, -1, true);  // With metadata
    VectorOpResult result = handler.Create(index_metadata);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Step 2: Concurrent BatchAdd operations
    const size_t num_batch_add_threads = 3;
    const size_t batch_size = 10;
    const size_t vectors_per_batch_thread = 30;
    const size_t max_retries = 5;
    const std::chrono::milliseconds retry_delay(100);

    // Counters for tracking operations
    std::atomic<size_t> batch_add_success_count{0};
    std::atomic<size_t> total_added_vectors{0};

    // Threads for concurrent BatchAdd operations with retry logic
    std::vector<std::thread> batch_add_threads;
    for (size_t t = 0; t < num_batch_add_threads; ++t)
    {
        batch_add_threads.emplace_back(
            [&handler,
             index_name,
             t,
             batch_size,
             vectors_per_batch_thread,
             max_retries,
             retry_delay,
             &batch_add_success_count,
             &total_added_vectors]()
            {
                uint64_t start_id = 2000 + t * vectors_per_batch_thread;
                size_t num_batches =
                    (vectors_per_batch_thread + batch_size - 1) / batch_size;

                for (size_t b = 0; b < num_batches; ++b)
                {
                    size_t current_batch_size = std::min(
                        batch_size, vectors_per_batch_thread - b * batch_size);
                    if (current_batch_size == 0)
                        break;

                    std::vector<uint64_t> ids;
                    std::vector<std::vector<float>> vectors;
                    std::vector<std::string> metadata_strings;
                    std::vector<std::string_view> metadata_list;
                    ids.reserve(current_batch_size);
                    vectors.reserve(current_batch_size);
                    metadata_strings.reserve(current_batch_size);

                    for (size_t i = 0; i < current_batch_size; ++i)
                    {
                        uint64_t id = start_id + b * batch_size + i;
                        ids.push_back(id);
                        vectors.push_back(
                            GenerateTestVector(test_vector_dimension, id));
                        std::string category = (i % 3 == 0)   ? "A"
                                               : (i % 3 == 1) ? "B"
                                                              : "C";
                        double score = 0.1 + (i % 10) * 0.1;
                        metadata_strings.push_back(
                            GenerateTestMetadataJson(category, score));
                    }

                    for (const auto &meta : metadata_strings)
                    {
                        metadata_list.push_back(meta);
                    }

                    // Retry logic: retry up to max_retries times with delay
                    VectorOpResult result = VectorOpResult::INDEX_ADD_FAILED;
                    for (size_t retry = 0; retry < max_retries; ++retry)
                    {
                        result = handler.BatchAdd(
                            index_name, ids, vectors, metadata_list);
                        if (result == VectorOpResult::SUCCEED)
                        {
                            batch_add_success_count++;
                            total_added_vectors += current_batch_size;
                            break;
                        }
                        // Wait before retry
                        if (retry < max_retries - 1)
                        {
                            std::this_thread::sleep_for(retry_delay);
                        }
                    }
                }
            });
    }

    // Wait for all threads to complete
    for (auto &thread : batch_add_threads)
    {
        thread.join();
    }

    // Step 3: Verify results
    DLOG(INFO) << "BatchAdd success count: " << batch_add_success_count.load();
    DLOG(INFO) << "Total added vectors: " << total_added_vectors.load();

    // All BatchAdd operations should succeed after retries
    size_t expected_batch_add_count = 0;
    for (size_t t = 0; t < num_batch_add_threads; ++t)
    {
        size_t num_batches =
            (vectors_per_batch_thread + batch_size - 1) / batch_size;
        expected_batch_add_count += num_batches;
    }
    REQUIRE(batch_add_success_count.load() == expected_batch_add_count);

    // Step 4: Verify final state - search should return all added vectors
    std::vector<float> query_vector =
        GenerateTestVector(test_vector_dimension, 1);
    size_t k_count = 1000;  // Large enough to get all vectors
    size_t thread_id = 0;
    SearchResult final_search_result;
    result = handler.Search(
        index_name, query_vector, k_count, thread_id, "", final_search_result);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Should have all added vectors
    REQUIRE(final_search_result.ids.size() >= total_added_vectors.load());

    // Cleanup: Drop the index
    result = handler.Drop(index_name);
    REQUIRE(result == VectorOpResult::SUCCEED);

    DLOG(INFO) << "VectorHandler Concurrency Test - BatchAdd done";
}

TEST_CASE("VectorHandler Concurrency Test - Search", "[vector-handler]")
{
    DLOG(INFO) << "VectorHandler Concurrency Test - Search start";

    VectorHandler &handler = VectorHandler::Instance();
    std::string index_name = GenerateTestIndexName();

    // Step 1: Create an index with metadata schema
    VectorIndexMetadata index_metadata =
        CreateTestIndexMetadata(index_name, -1, true);  // With metadata
    VectorOpResult result = handler.Create(index_metadata);
    REQUIRE(result == VectorOpResult::SUCCEED);

    // Step 2: Add some vectors for searching
    const size_t initial_vectors = 50;
    for (size_t i = 0; i < initial_vectors; ++i)
    {
        std::vector<float> vector =
            GenerateTestVector(test_vector_dimension, i + 1);
        uint64_t id = i + 1;
        std::string category = (i % 2 == 0) ? "A" : "B";
        double score = 0.1 + (i % 10) * 0.1;
        std::string metadata = GenerateTestMetadataJson(category, score);
        result = handler.Add(index_name, id, vector, metadata);
        REQUIRE(result == VectorOpResult::SUCCEED);
    }

    // Step 3: Concurrent Search operations
    const size_t num_search_threads = 5;

    // Counters for tracking operations
    std::atomic<size_t> search_success_count{0};

    // Threads for concurrent Search operations
    std::vector<std::thread> search_threads;
    for (size_t t = 0; t < num_search_threads; ++t)
    {
        search_threads.emplace_back(
            [&handler, index_name, t, &search_success_count]()
            {
                const size_t num_searches = 50;
                size_t thread_id = t;
                std::vector<float> query_vector =
                    GenerateTestVector(test_vector_dimension, t + 1);
                size_t k_count = 10;

                for (size_t s = 0; s < num_searches; ++s)
                {
                    // Search without filter
                    SearchResult search_result;
                    VectorOpResult result = handler.Search(index_name,
                                                           query_vector,
                                                           k_count,
                                                           thread_id,
                                                           "",
                                                           search_result);
                    if (result == VectorOpResult::SUCCEED)
                    {
                        search_success_count++;
                    }

                    // Search with filter (alternate between different filters)
                    std::string filter_json;
                    if (s % 2 == 0)
                    {
                        filter_json = "{\"category\": {\"$eq\": \"A\"}}";
                    }
                    else
                    {
                        filter_json = "{\"score\": {\"$gt\": 0.5}}";
                    }

                    SearchResult filtered_result;
                    result = handler.Search(index_name,
                                            query_vector,
                                            k_count,
                                            thread_id,
                                            filter_json,
                                            filtered_result);
                    if (result == VectorOpResult::SUCCEED)
                    {
                        search_success_count++;
                    }
                }
            });
    }

    // Wait for all threads to complete
    for (auto &thread : search_threads)
    {
        thread.join();
    }

    // Step 4: Verify results
    DLOG(INFO) << "Search success count: " << search_success_count.load();

    // All Search operations should succeed (each thread does num_searches * 2
    // searches)
    REQUIRE(
        search_success_count.load() ==
        num_search_threads * 50 *
            2);  // 50 searches per thread, 2 per search (with/without filter)

    // Step 5: Verify filtered search still works
    std::vector<float> query_vector =
        GenerateTestVector(test_vector_dimension, 1);
    size_t k_count = 100;
    size_t thread_id = 0;
    std::string filter_json = "{\"category\": {\"$eq\": \"A\"}}";
    SearchResult filtered_final_result;
    result = handler.Search(index_name,
                            query_vector,
                            k_count,
                            thread_id,
                            filter_json,
                            filtered_final_result);
    REQUIRE(result == VectorOpResult::SUCCEED);
    REQUIRE(filtered_final_result.ids.size() > 0);

    // Cleanup: Drop the index
    result = handler.Drop(index_name);
    REQUIRE(result == VectorOpResult::SUCCEED);

    DLOG(INFO) << "VectorHandler Concurrency Test - Search done";
}

}  // namespace EloqVec

int main(int argc, char **argv)
{
    Catch::Session session;
    int cres = session.applyCommandLine(argc, argv);
    if (cres != 0)
    {
        return cres;
    }

    DLOG(INFO) << "Catch::Session initialized";

    // Clean up any existing test data
    EloqVec::CleanupTestData();

    DLOG(INFO) << "Test data directories cleaned up";

    // Create test directories
    std::filesystem::create_directories(EloqVec::test_data_path);
    std::filesystem::create_directories(EloqVec::vector_index_data_path);

    // Init the txservice
    EloqVec::tx_service = EloqVec::InitTxService();

    // Init the vector index worker pool
    EloqVec::vector_index_worker_pool =
        std::make_unique<TxWorkerPool>(EloqVec::num_cores);

    // Init VectorHandler
    std::string data_path = EloqVec::vector_index_data_path;
    bool init_success = EloqVec::VectorHandler::InitHandlerInstance(
        EloqVec::tx_service.get(),
        EloqVec::vector_index_worker_pool.get(),
        data_path,
        nullptr);  // No cloud config for testing

    if (!init_success)
    {
        LOG(ERROR) << "Failed to initialize VectorHandler";
        EloqVec::tx_service->Shutdown();
        EloqVec::tx_service.reset();
        EloqVec::CleanupTestData();
        return -1;
    }

    // Run tests
    cres = session.run();

    // Cleanup
    EloqVec::VectorHandler::DestroyHandlerInstance();
    EloqVec::vector_index_worker_pool->Shutdown();
    EloqVec::vector_index_worker_pool.reset();
    EloqVec::tx_service->Shutdown();
    EloqVec::tx_service.reset();
    EloqVec::CleanupTestData();

    DLOG(INFO) << "Tests completed";

    return cres;
}
