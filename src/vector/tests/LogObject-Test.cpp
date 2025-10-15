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
#include <random>
#include <thread>
#include <vector>

#include "tx_service.h"
#include "tx_util.h"
#include "vector/log_object.h"
#include "vector/vector_type.h"

using namespace txservice;

namespace EloqVec
{
static std::unique_ptr<TxService> tx_service(nullptr);
// Test configuration
constexpr uint32_t num_cores = 4;
constexpr uint32_t num_shards = 1024;
constexpr uint32_t hot_num_shards = 32;
constexpr uint32_t invalid_num_shards = 0;
constexpr std::string_view base_log_name_v{"test_log_object:index"};

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
    tx_path.append("test_data");

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

std::string get_log_name()
{
    static std::random_device rd;
    static thread_local std::mt19937 gen(rd());
    static thread_local std::uniform_int_distribution<uint32_t> dis(0,
                                                                    UINT32_MAX);
    std::string log_name(base_log_name_v);
    log_name.append(":");
    log_name.append(std::to_string(dis(gen)));
    return log_name;
}

// Helper: generate test log items
log_item_t generate_test_item()
{
    static std::random_device rd;
    static thread_local std::mt19937 gen(rd());
    static thread_local std::uniform_int_distribution<uint8_t> op_dis(0, 2);
    static thread_local std::uniform_int_distribution<uint32_t> key_dis(
        1, UINT32_MAX);
    static thread_local std::uniform_int_distribution<uint64_t> value_dis(
        1, UINT64_MAX);

    log_item_t item;
    item.operation_type = static_cast<LogOperationType>(op_dis(gen));
    item.key.append("test_key_").append(std::to_string(key_dis(gen)));
    item.value.append("test_value_").append(std::to_string(value_dis(gen)));
    item.sequence_id = 0;
    return item;
}

log_item_t generate_test_item(const uint32_t seed)
{
    static thread_local std::mt19937 gen(seed);
    static thread_local std::uniform_int_distribution<uint8_t> op_dis(0, 2);
    static thread_local std::uniform_int_distribution<uint32_t> key_dis(
        1, UINT32_MAX);
    static thread_local std::uniform_int_distribution<uint64_t> value_dis(
        1, UINT64_MAX);

    log_item_t item;
    item.operation_type = static_cast<LogOperationType>(op_dis(gen));
    item.key.append("test_key_").append(std::to_string(key_dis(gen)));
    item.value.append("test_value_").append(std::to_string(value_dis(gen)));
    item.sequence_id = 0;
    return item;
}

void create_log_object(const std::string &log_name, bool log_exist = false)
{
    TransactionExecution *txm = NewTxInit(
        tx_service.get(), IsolationLevel::RepeatableRead, CcProtocol::OCC);
    assert(txm != nullptr);
    LogError result = LogObject::create_sharded_logs(log_name, num_shards, txm);
    if (!log_exist)
    {
        assert(result == LogError::SUCCESS);
        assert(CommitTx(txm).first);
    }
    else
    {
        assert(result == LogError::LOG_ALREADY_EXISTS);
        AbortTx(txm);
    }
}

void remove_log_object(const std::string &log_name, bool log_exist = true)
{
    TransactionExecution *txm = NewTxInit(
        tx_service.get(), IsolationLevel::RepeatableRead, CcProtocol::OCC);
    assert(txm != nullptr);
    LogError result = LogObject::remove_sharded_logs(log_name, num_shards, txm);
    if (log_exist)
    {
        assert(result == LogError::SUCCESS);
        assert(CommitTx(txm).first);
    }
    else
    {
        assert(result == LogError::LOG_NOT_FOUND);
        AbortTx(txm);
    }
}

bool truncate_log_object(const std::string &log_name,
                         uint64_t &total_log_count,
                         uint32_t shards_cnt,
                         bool log_exist = true)
{
    TransactionExecution *txm = NewTxInit(
        tx_service.get(), IsolationLevel::RepeatableRead, CcProtocol::OCC);
    assert(txm != nullptr);
    LogError result = LogObject::truncate_all_sharded_logs(
        log_name, shards_cnt, total_log_count, txm);
    if (log_exist && result == LogError::SUCCESS)
    {
        assert(CommitTx(txm).first);
        return true;
    }
    else
    {
        assert(result == LogError::LOG_NOT_FOUND ||
               result == LogError::INVALID_PARAMETER ||
               result == LogError::STORAGE_ERROR);
        AbortTx(txm);
        return false;
    }
}

void scan_log_object(const std::string &log_name,
                     std::vector<log_item_t> &items,
                     uint32_t shards_cnt,
                     bool log_exist = true)
{
    TransactionExecution *txm = NewTxInit(
        tx_service.get(), IsolationLevel::RepeatableRead, CcProtocol::OCC);
    assert(txm != nullptr);
    LogError result =
        LogObject::scan_sharded_log(log_name, shards_cnt, items, txm);
    if (log_exist && result == LogError::SUCCESS)
    {
        assert(CommitTx(txm).first);
    }
    else
    {
        assert(result == LogError::LOG_NOT_FOUND ||
               result == LogError::INVALID_PARAMETER);
        AbortTx(txm);
    }
}

void append_items(const std::string &log_name,
                  uint32_t item_num,
                  uint32_t shards_cnt,
                  bool retry_if_failed,
                  std::vector<log_item_t> *const appended_items = nullptr,
                  const uint32_t *const seed = nullptr,
                  uint32_t *const retry_times = nullptr)
{
    TransactionExecution *txm = nullptr;
    for (uint32_t i = 0; i < item_num; ++i)
    {
        txm = NewTxInit(
            tx_service.get(), IsolationLevel::RepeatableRead, CcProtocol::OCC);
        assert(txm != nullptr);

        log_item_t item =
            seed != nullptr ? generate_test_item(*seed) : generate_test_item();
        item.timestamp =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        std::vector<log_item_t> items;
        items.emplace_back(std::move(item));
        uint64_t log_id, log_count;
        LogError result;
        do
        {
            result = LogObject::append_log_sharded(log_name,
                                                   items[0].key,
                                                   shards_cnt,
                                                   items,
                                                   log_id,
                                                   log_count,
                                                   txm);
            if (result == LogError::SUCCESS)
            {
                assert(log_count > 0);
                assert(CommitTx(txm).first);
                if (appended_items != nullptr)
                {
                    appended_items->push_back(std::move(items[0]));
                }
                break;
            }
            assert(result == LogError::STORAGE_ERROR ||
                   result == LogError::LOG_NOT_FOUND ||
                   result == LogError::INVALID_PARAMETER);
            AbortTx(txm);

            if (!retry_if_failed)
            {
                break;
            }
            assert(result == LogError::STORAGE_ERROR);
            // Retry after 10ms
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (retry_times != nullptr)
            {
                ++(*retry_times);
            }
            txm = NewTxInit(tx_service.get(),
                            IsolationLevel::RepeatableRead,
                            CcProtocol::OCC);
            assert(txm != nullptr);
        } while (true);
    }
}

void batch_append_items(const std::string &log_name,
                        uint32_t batch_num,
                        uint32_t batch_items_num,
                        uint32_t shards_cnt,
                        std::vector<log_item_t> *const appended_items = nullptr)
{
    TransactionExecution *txm = nullptr;
    for (uint32_t batch = 0; batch < batch_num; ++batch)
    {
        txm = NewTxInit(
            tx_service.get(), IsolationLevel::RepeatableRead, CcProtocol::OCC);
        assert(txm != nullptr);
        // Group keys by shard_id
        std::unordered_map<uint32_t, std::vector<log_item_t>> shard_groups;
        uint64_t timestamp =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();

        // Generate batch_items_num keys for this batch
        for (uint32_t i = 0; i < batch_items_num; ++i)
        {
            log_item_t item = generate_test_item();
            item.timestamp = timestamp;
            // Group by shard_id
            uint32_t shard_id = LogObject::get_shard_id(item.key, shards_cnt);
            auto [it, success] = shard_groups.try_emplace(shard_id);
            if (success)
            {
                it->second.reserve(batch_items_num);
            }
            it->second.push_back(std::move(item));
        }

        // Append each shard group separately
        for (auto &[shard_id, items] : shard_groups)
        {
            assert(!items.empty());
            uint64_t log_id, log_count;
            assert(LogError::SUCCESS ==
                   LogObject::append_log_sharded(log_name,
                                                 items[0].key,
                                                 shards_cnt,
                                                 items,
                                                 log_id,
                                                 log_count,
                                                 txm));
            assert(log_count > 0);
            if (appended_items != nullptr)
            {
                appended_items->insert(appended_items->end(),
                                       std::make_move_iterator(items.begin()),
                                       std::make_move_iterator(items.end()));
            }
        }
        assert(CommitTx(txm).first);
    }
}

void append_worker(const std::string &log_name,
                   uint32_t item_num,
                   uint32_t shards_cnt,
                   const uint32_t *const seed,
                   uint32_t *const retry_times)
{
    append_items(
        log_name, item_num, shards_cnt, true, nullptr, seed, retry_times);
}

TEST_CASE("LogObject Create and Remove", "[log-object]")
{
    DLOG(INFO) << "LogObject Create and Remove start";
    REQUIRE(tx_service != nullptr);
    std::string log_name = get_log_name();
    create_log_object(log_name);
    remove_log_object(log_name);
    DLOG(INFO) << "LogObject Create and Remove done";
}

TEST_CASE("LogObject Basic Operations", "[log-object]")
{
    DLOG(INFO) << "LogObject Basic Operations start";
    REQUIRE(tx_service != nullptr);
    // 1. create log object
    std::string log_name = get_log_name();
    create_log_object(log_name);

    // 2. append log items
    const uint32_t single_key_num = 10;
    const uint32_t batch_num = 1;
    const uint32_t batch_items_num = 10;

    std::vector<log_item_t> all_appended_items;
    all_appended_items.reserve(single_key_num + batch_num * batch_items_num);

    // Single key operations (single_key_num times)
    append_items(
        log_name, single_key_num, num_shards, true, &all_appended_items);
    REQUIRE(all_appended_items.size() == single_key_num);

    // Batch key operations (batch_num batches, each with batch_items_num keys)
    batch_append_items(
        log_name, batch_num, batch_items_num, num_shards, &all_appended_items);
    REQUIRE(all_appended_items.size() ==
            single_key_num + batch_num * batch_items_num);

    // 3. scan log
    std::vector<log_item_t> scanned_items;
    scan_log_object(log_name, scanned_items, num_shards);
    REQUIRE(scanned_items.size() == all_appended_items.size());

    // 4. truncate log
    uint64_t total_log_count = UINT64_MAX;
    truncate_log_object(log_name, total_log_count, num_shards);
    REQUIRE(total_log_count == 0);

    // 5. append log items again
    append_items(log_name, 1, num_shards, true);

    // 6. scan log
    scanned_items.clear();
    scan_log_object(log_name, scanned_items, num_shards);
    REQUIRE(scanned_items.size() == 1);

    // 7. remove log object
    remove_log_object(log_name);
    DLOG(INFO) << "LogObject Basic Operations done";
}

TEST_CASE("LogObject Concurrent Append Operations", "[log-object]")
{
    DLOG(INFO) << "LogObject Concurrent Append Operations start";
    REQUIRE(tx_service != nullptr);

    // 1. create log object
    std::string log_name = get_log_name();
    create_log_object(log_name);

    // 2. concurrent append log items
    // Two workers to append log items concurrently, each appending 100 log
    // items into hot shards.
    uint32_t worker1_retry_times{0};
    uint32_t worker2_retry_times{0};
    const uint32_t item_per_worker = 1000;
    // generate seed for worker thread
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> seed_dis;

    // worker 1
    uint32_t worker1_seed = seed_dis(gen);
    std::thread worker1(append_worker,
                        std::ref(log_name),
                        item_per_worker,
                        hot_num_shards,
                        &worker1_seed,
                        &worker1_retry_times);
    // worker 2
    uint32_t worker2_seed = seed_dis(gen);
    std::thread worker2(append_worker,
                        std::ref(log_name),
                        item_per_worker,
                        hot_num_shards,
                        &worker2_seed,
                        &worker2_retry_times);

    // wait for workers to finish
    worker1.join();
    worker2.join();

    // verify at least one conflict occurred
    uint32_t total_retry_times = worker1_retry_times + worker2_retry_times;
    REQUIRE(total_retry_times > 0);

    // 3. scan log.
    std::vector<log_item_t> scanned_items;
    scan_log_object(log_name, scanned_items, num_shards);
    // verify scanned items count matches appended items
    size_t expected_items_count = static_cast<size_t>(item_per_worker * 2);
    REQUIRE(scanned_items.size() == expected_items_count);

    // 4. remove log object
    remove_log_object(log_name);
    DLOG(INFO) << "LogObject Concurrent Append Operations done";
}

TEST_CASE("LogObject Concurrent Scan Operations", "[log-object]")
{
    DLOG(INFO) << "LogObject Concurrent Scan Operations start";
    REQUIRE(tx_service != nullptr);

    // 1. create log object
    std::string log_name = get_log_name();
    create_log_object(log_name);

    // 2. append log items into hot shards
    const uint32_t append_item_num = 1000;
    append_items(log_name, append_item_num, hot_num_shards, true);

    size_t worker1_scanned_count{0};
    size_t worker2_scanned_count{0};
    // 3. concurrent scan log items in multiple threads
    auto scan_worker = [&](size_t &worker_scanned_count)
    {
        std::vector<log_item_t> scanned_items;
        scan_log_object(log_name, scanned_items, num_shards);
        worker_scanned_count += scanned_items.size();
        assert(worker_scanned_count == append_item_num);
    };

    std::thread worker1(scan_worker, std::ref(worker1_scanned_count));
    std::thread worker2(scan_worker, std::ref(worker2_scanned_count));

    // wait for scan workers to finish
    worker1.join();
    worker2.join();

    size_t total_scanned_count = worker1_scanned_count + worker2_scanned_count;
    REQUIRE(total_scanned_count == append_item_num * 2);

    // 4. remove log object
    remove_log_object(log_name);
    DLOG(INFO) << "LogObject Concurrent Scan Operations done";
}

void truncate_worker(const std::string &log_name,
                     bool &truncate_succeed,
                     bool retry_if_failed)
{
    uint64_t log_count;
    do
    {
        if (truncate_log_object(log_name, log_count, num_shards))
        {
            assert(log_count == 0);
            truncate_succeed = true;
            return;
        }

        if (!retry_if_failed)
        {
            truncate_succeed = false;
            return;
        }
        // Retry after 10ms
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (true);
}

TEST_CASE("LogObject Concurrent Truncate Operations", "[log-object]")
{
    DLOG(INFO) << "LogObject Concurrent Truncate Operations start";
    REQUIRE(tx_service != nullptr);

    // 1. create log object
    std::string log_name = get_log_name();
    create_log_object(log_name);

    // 2. append log items into hot shards with batch mode
    const uint32_t batch_num = 10;
    const uint32_t batch_item_num = 100;
    batch_append_items(log_name, batch_num, batch_item_num, hot_num_shards);

    // 3. concurrent truncate log items
    bool truncate_succeed1{false};
    bool truncate_succeed2{false};
    std::thread worker1(truncate_worker,
                        std::ref(log_name),
                        std::ref(truncate_succeed1),
                        false);
    std::thread worker2(truncate_worker,
                        std::ref(log_name),
                        std::ref(truncate_succeed2),
                        false);

    // wait for truncate workers to finish
    worker1.join();
    worker2.join();

    bool truncate_succeed = truncate_succeed1 || truncate_succeed2;
    REQUIRE(truncate_succeed);

    // 4. scan log items
    std::vector<log_item_t> scanned_items;
    scan_log_object(log_name, scanned_items, num_shards);
    REQUIRE(scanned_items.empty());

    // 5. remove log object
    remove_log_object(log_name);

    DLOG(INFO) << "LogObject Concurrent Truncate Operations done";
}

bool try_create_log_object(const std::string &log_name,
                           std::mutex &mux,
                           std::condition_variable &cv,
                           bool &op_succeed)
{
    static std::atomic_flag creating = ATOMIC_FLAG_INIT;
    if (creating.test_and_set(std::memory_order_acquire))
    {
        // another thread is creating the same log object
        return false;
    }

    TransactionExecution *create_txm = NewTxInit(
        tx_service.get(), IsolationLevel::RepeatableRead, CcProtocol::OCC);
    assert(create_txm != nullptr);

    assert(LogObject::create_sharded_logs(log_name, num_shards, create_txm) ==
           LogError::SUCCESS);
    assert(CommitTx(create_txm).first);

    creating.clear(std::memory_order_release);

    std::lock_guard<std::mutex> lk(mux);
    op_succeed = true;
    cv.notify_all();

    return true;
}

bool try_remove_log_object(const std::string &log_name,
                           std::mutex &mux,
                           std::condition_variable &cv,
                           bool &op_succeed)
{
    static std::atomic_flag removing = ATOMIC_FLAG_INIT;
    if (removing.test_and_set(std::memory_order_acquire))
    {
        // another thread is removing the same log object
        return false;
    }

    TransactionExecution *remove_txm = NewTxInit(
        tx_service.get(), IsolationLevel::RepeatableRead, CcProtocol::OCC);
    assert(remove_txm != nullptr);
    assert(LogObject::remove_sharded_logs(log_name, num_shards, remove_txm) ==
           LogError::SUCCESS);
    assert(CommitTx(remove_txm).first);

    removing.clear(std::memory_order_release);

    std::lock_guard<std::mutex> lk(mux);
    op_succeed = true;
    cv.notify_all();

    return true;
}

void wait_for_op_succeed(std::mutex &mux,
                         std::condition_variable &cv,
                         bool &op_succeed)
{
    std::unique_lock<std::mutex> lk(mux);
    cv.wait(lk, [&]() { return op_succeed; });
}

void create_and_remove_worker(const std::string &log_name,
                              std::mutex &mux,
                              std::condition_variable &cv,
                              bool &op_succeed)
{
    // Try create log object
    if (!try_create_log_object(log_name, mux, cv, op_succeed))
    {
        wait_for_op_succeed(mux, cv, op_succeed);
    }

    TransactionExecution *worker_txm = NewTxInit(
        tx_service.get(), IsolationLevel::RepeatableRead, CcProtocol::OCC);
    assert(worker_txm != nullptr);
    // check if log object exists
    assert(LogObject::exists_sharded(log_name, num_shards, worker_txm) ==
           LogError::SUCCESS);
    assert(CommitTx(worker_txm).first);

    // reset op_succeed
    {
        std::lock_guard<std::mutex> lk(mux);
        op_succeed = false;
    }

    // Try remove log object
    if (!try_remove_log_object(log_name, mux, cv, op_succeed))
    {
        wait_for_op_succeed(mux, cv, op_succeed);
    }

    worker_txm = NewTxInit(
        tx_service.get(), IsolationLevel::RepeatableRead, CcProtocol::OCC);
    assert(worker_txm != nullptr);
    // check if log object does not exist
    assert(LogObject::exists_sharded(log_name, num_shards, worker_txm) ==
           LogError::LOG_NOT_FOUND);
    assert(CommitTx(worker_txm).first);
}

TEST_CASE("LogObject Concurrent Create and Remove Operations", "[log-object]")
{
    DLOG(INFO) << "LogObject Concurrent Create and Remove Operations start";
    REQUIRE(tx_service != nullptr);

    // 1. create log object
    std::string log_name = get_log_name();

    std::mutex mux;
    std::condition_variable cv;
    bool op_succeed = false;
    // 2. concurrent create and remove log items
    std::thread worker1(create_and_remove_worker,
                        log_name,
                        std::ref(mux),
                        std::ref(cv),
                        std::ref(op_succeed));
    std::thread worker2(create_and_remove_worker,
                        log_name,
                        std::ref(mux),
                        std::ref(cv),
                        std::ref(op_succeed));

    // wait for create and remove workers to finish
    worker1.join();
    worker2.join();

    // 3. check if log object does not exist
    TransactionExecution *txm = NewTxInit(
        tx_service.get(), IsolationLevel::RepeatableRead, CcProtocol::OCC);
    REQUIRE(txm != nullptr);
    REQUIRE(LogObject::exists_sharded(log_name, num_shards, txm) ==
            LogError::LOG_NOT_FOUND);
    REQUIRE(CommitTx(txm).first);

    DLOG(INFO) << "LogObject Concurrent Create and Remove Operations done";
}

TEST_CASE("LogObject Concurrent Append and Truncate Operations", "[log-object]")
{
    DLOG(INFO) << "LogObject Concurrent Append and Truncate Operations start";
    REQUIRE(tx_service != nullptr);

    // 1. create log object
    std::string log_name = get_log_name();
    create_log_object(log_name);

    // 2. append log items firstly
    const uint32_t item_num = 1000;
    append_items(log_name, item_num, num_shards, true);

    // 3. one worker try truncate log items and another worker try append log
    // items
    bool truncate_succeed{false};
    std::thread worker2(
        truncate_worker, std::ref(log_name), std::ref(truncate_succeed), true);
    std::thread worker1(append_worker,
                        std::ref(log_name),
                        item_num,
                        num_shards,
                        nullptr,
                        nullptr);

    worker1.join();
    worker2.join();
    REQUIRE(truncate_succeed);

    // 4. scan log items
    std::vector<log_item_t> scanned_items;
    scan_log_object(log_name, scanned_items, num_shards);
    REQUIRE(scanned_items.size() <= item_num);

    // 5. remove log object
    remove_log_object(log_name);

    DLOG(INFO) << "LogObject Concurrent Append and Truncate Operations done";
}

TEST_CASE("LogObject Operations on Empty LogObject", "[log-object]")
{
    DLOG(INFO) << "LogObject Operations on Empty LogObject start";
    REQUIRE(tx_service != nullptr);

    // 1. create log object
    std::string log_name = get_log_name();
    create_log_object(log_name);

    // 2. scan log items
    std::vector<log_item_t> scanned_items;
    scan_log_object(log_name, scanned_items, num_shards);
    REQUIRE(scanned_items.empty());

    // 3. truncate log items
    uint64_t total_log_count = UINT64_MAX;
    truncate_log_object(log_name, total_log_count, num_shards);
    REQUIRE(total_log_count == 0);

    // 4. remove log object
    remove_log_object(log_name);

    DLOG(INFO) << "LogObject Operations on Empty LogObject done";
}

TEST_CASE("LogObject Operations with Not Exist Log Object", "[log-object]")
{
    DLOG(INFO) << "LogObject Operations with Not Exist Log Object start";
    REQUIRE(tx_service != nullptr);

    std::string log_name = get_log_name();
    // 1. append log items with not exist log object
    append_items(log_name, 1000, num_shards, false);

    // 2. scan log items with not exist log object
    std::vector<log_item_t> scanned_items;
    scan_log_object(log_name, scanned_items, num_shards, false);
    REQUIRE(scanned_items.empty());

    // 3. truncate log items with not exist log object
    uint64_t total_log_count = UINT64_MAX;
    truncate_log_object(log_name, total_log_count, num_shards, false);
    REQUIRE(total_log_count == 0);

    // 4. remove not exist log object
    remove_log_object(log_name, false);

    DLOG(INFO) << "LogObject Operations with Not Exist Log Object done";
}

TEST_CASE("LogObject Operations with Invalid Parameters", "[log-object]")
{
    DLOG(INFO) << "LogObject Operations with Invalid Parameters start";
    REQUIRE(tx_service != nullptr);

    // 1. create log object
    std::string log_name = get_log_name();
    create_log_object(log_name);

    // 2. create the same log object
    create_log_object(log_name, true);

    // 3. append log items to the log object with invalid num_shards
    append_items(log_name, 1000, invalid_num_shards, false);

    // 4. scan log items with invalid num_shards
    std::vector<log_item_t> scanned_items;
    scan_log_object(log_name, scanned_items, invalid_num_shards);
    REQUIRE(scanned_items.empty());

    // 5. truncate log items with invalid num_shards
    uint64_t total_log_count = UINT64_MAX;
    truncate_log_object(log_name, total_log_count, invalid_num_shards);
    REQUIRE(total_log_count == UINT64_MAX);

    DLOG(INFO) << "LogObject Operations with Invalid Parameters done";
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

    // Init the txservice
    EloqVec::tx_service = EloqVec::InitTxService();
    cres = session.run();
    // shutdown the txservice
    EloqVec::tx_service->Shutdown();
    EloqVec::tx_service.reset();
    return cres;
}
