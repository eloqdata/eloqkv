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
#include <glog/logging.h>

#include <catch2/catch_all.hpp>
#include <filesystem>

#include "hnsw_vector_index.h"
#include "vector_type.h"

namespace EloqVec
{
static std::unique_ptr<VectorIndex> vector_index(nullptr);
static IndexConfig index_config;
static std::string index_path = "hnsw_vector_index.index";
static size_t metadata_size = 64;

void SetUpIndexConfig()
{
    index_config.dimension = 4;
    index_config.algorithm = Algorithm::HNSW;
    index_config.distance_metric = DistanceMetric::COSINE;
    index_config.params = {
        {"m", "16"}, {"ef_construction", "128"}, {"ef_search", "64"}};
    index_config.max_elements = 100000;
}

/* id range of this case is [1, 100] */
TEST_CASE("VectorCache HNSW Add Operation", "[vector-cache-hnsw]")
{
    DLOG(INFO) << "VectorCache HNSW Add Operation start";
    REQUIRE(vector_index != nullptr);
    REQUIRE(vector_index->is_ready());

    std::vector<float> vector = {1.0, 2.0, 3.0, 4.0};
    VectorId vector_id;
    vector_id.id_ = 1;
    IndexOpResult result = vector_index->add(vector, vector_id);
    REQUIRE(result.error == VectorOpResult::SUCCEED);

    std::vector<float> vec_result;
    vector_index->get(vector_id, vec_result);
    REQUIRE(vec_result.size() == 4);
    REQUIRE(vec_result[0] == 1.0);
    REQUIRE(vec_result[1] == 2.0);
    REQUIRE(vec_result[2] == 3.0);
    REQUIRE(vec_result[3] == 4.0);

    DLOG(INFO) << "VectorCache HNSW Add Operation done";
}

/* id range of this case is [101, 200] */
TEST_CASE("VectorCache HNSW Batch Add Operation", "[vector-cache-hnsw]")
{
    DLOG(INFO) << "VectorCache HNSW Batch Add Operation start";
    REQUIRE(vector_index != nullptr);
    REQUIRE(vector_index->is_ready());

    std::vector<std::vector<float>> vectors = {{101.0, 102.0, 103.0, 104.0},
                                               {105.0, 106.0, 107.0, 108.0}};
    std::vector<VectorId> ids(2);
    ids[0].id_ = 101;
    ids[1].id_ = 102;
    IndexOpResult result = vector_index->add_batch(vectors, ids);
    REQUIRE(result.error == VectorOpResult::SUCCEED);

    std::vector<float> vec_result;
    vector_index->get(ids[1], vec_result);
    REQUIRE(vec_result.size() == 4);
    REQUIRE(vec_result[0] == 105.0);
    REQUIRE(vec_result[1] == 106.0);
    REQUIRE(vec_result[2] == 107.0);
    REQUIRE(vec_result[3] == 108.0);

    DLOG(INFO) << "VectorCache HNSW Batch Add Operation done";
}

/* id range of this case is [201, 300] */
TEST_CASE("VectorCache HNSW Search Operation", "[vector-cache-hnsw]")
{
    DLOG(INFO) << "VectorCache HNSW Search Operation start";
    REQUIRE(vector_index != nullptr);
    REQUIRE(vector_index->is_ready());

    std::vector<float> vector = {201.0, 202.0, 203.0, 204.0};
    VectorId vector_id;
    vector_id.id_ = 201;
    IndexOpResult result = vector_index->add(vector, vector_id);
    REQUIRE(result.error == VectorOpResult::SUCCEED);

    std::vector<float> query_vector = {201.0, 202.0, 203.0, 204.0};
    SearchResult search_result;
    result = vector_index->search(query_vector, 1, 0, search_result);
    REQUIRE(result.error == VectorOpResult::SUCCEED);
    REQUIRE(search_result.ids.size() == 1);
    REQUIRE(search_result.distances[0] == 0.0);

    DLOG(INFO) << "VectorCache HNSW Search Operation done";
}

/* id range of this case is [301, 400] */
TEST_CASE("VectorCache HNSW Save & ReInit Operation", "[vector-cache-hnsw]")
{
    DLOG(INFO) << "VectorCache HNSW Save & ReInit Operation start";
    REQUIRE(vector_index != nullptr);
    REQUIRE(vector_index->is_ready());

    // add a vector
    std::vector<float> vector = {301.0, 302.0, 303.0, 304.0};
    VectorId vector_id;
    vector_id.id_ = 301;
    IndexOpResult res = vector_index->add(vector, vector_id);
    REQUIRE(res.error == VectorOpResult::SUCCEED);

    bool result = vector_index->save(index_path);
    REQUIRE(result);

    // reinitialize the index
    vector_index.reset();
    vector_index = EloqVec::create_hnsw_vector_index();
    REQUIRE(vector_index != nullptr);
    REQUIRE(vector_index->initialize(index_config, index_path));
    REQUIRE(vector_index->is_ready());

    // get the vector
    std::vector<float> vector_result;
    res = vector_index->get(vector_id, vector_result);
    REQUIRE(res.error == VectorOpResult::SUCCEED);
    REQUIRE(vector_result.size() == 4);
    REQUIRE(vector_result[0] == 301.0);
    REQUIRE(vector_result[1] == 302.0);
    REQUIRE(vector_result[2] == 303.0);
    REQUIRE(vector_result[3] == 304.0);

    // add a vector
    std::vector<float> vector1 = {311.0, 312.0, 313.0, 314.0};
    VectorId vector_id1;
    vector_id1.id_ = 311;
    res = vector_index->add(vector1, vector_id1);
    REQUIRE(res.error == VectorOpResult::SUCCEED);

    DLOG(INFO) << "VectorCache HNSW Save & ReInit Operation done";
}

/* id range of this case is [401, 500] */
TEST_CASE("VectorCache HNSW Remove Operation", "[vector-cache-hnsw]")
{
    DLOG(INFO) << "VectorCache HNSW Remove Operation start";
    REQUIRE(vector_index != nullptr);
    REQUIRE(vector_index->is_ready());

    std::vector<float> vector = {401.0, 402.0, 403.0, 404.0};
    VectorId vector_id;
    vector_id.id_ = 401;
    IndexOpResult result = vector_index->add(vector, vector_id);
    REQUIRE(result.error == VectorOpResult::SUCCEED);

    result = vector_index->remove(vector_id);
    REQUIRE(result.error == VectorOpResult::SUCCEED);

    // get the vector
    std::vector<float> vec_result;
    result = vector_index->get(vector_id, vec_result);
    REQUIRE(result.error == VectorOpResult::SUCCEED);
    REQUIRE(vec_result.size() == 0);

    DLOG(INFO) << "VectorCache HNSW Remove Operation done";
}

/* id range of this case is [501, 600] */
TEST_CASE("VectorCache HNSW Concurrency Add Operation", "[vector-cache-hnsw]")
{
    DLOG(INFO) << "VectorCache HNSW Concurrency Add Operation start";
    REQUIRE(vector_index != nullptr);
    REQUIRE(vector_index->is_ready());

    std::vector<std::thread> threads;
    for (int i = 0; i < 10; ++i)
    {
        threads.emplace_back(
            [i]()
            {
                float step = i * 5.0f;
                std::vector<float> vector = {
                    501.0f + step, 502.0f + step, 503.0f + step, 504.0f + step};
                VectorId vector_id;
                vector_id.id_ = 501 + i;
                IndexOpResult result = vector_index->add(vector, vector_id);
                assert(result.error == VectorOpResult::SUCCEED);
            });
    }
    for (auto &thread : threads)
    {
        thread.join();
    }

    // get the vector
    std::vector<float> vec_result;
    VectorId vector_id;
    vector_id.id_ = 501;
    IndexOpResult result = vector_index->get(vector_id, vec_result);
    REQUIRE(result.error == VectorOpResult::SUCCEED);
    REQUIRE(vec_result.size() == 4);
    REQUIRE(vec_result[0] == 501.0);
    REQUIRE(vec_result[1] == 502.0);
    REQUIRE(vec_result[2] == 503.0);
    REQUIRE(vec_result[3] == 504.0);

    DLOG(INFO) << "VectorCache HNSW Concurrency Add Operation done";
}

/* id range of this case is [601, 1600] */
TEST_CASE("VectorCache HNSW Concurrency Add Batch Operation",
          "[vector-cache-hnsw]")
{
    DLOG(INFO) << "VectorCache HNSW Concurrency Add Batch Operation start";
    REQUIRE(vector_index != nullptr);
    REQUIRE(vector_index->is_ready());

    // add vector with 10 workers concurrently
    const uint32_t worker_num = 10;
    const uint32_t batch_vector_num = 100;
    std::vector<std::thread> threads;
    for (uint32_t i = 0; i < worker_num; ++i)
    {
        threads.emplace_back(
            [i, batch_vector_num]()
            {
                float worker_step = i * batch_vector_num;
                std::vector<VectorId> ids(batch_vector_num);
                std::vector<std::vector<float>> vectors;
                for (uint32_t j = 0; j < batch_vector_num; ++j)
                {
                    ids[j].id_ = 601 + worker_step + j;
                    float step = worker_step + j * 4.0f;
                    vectors.push_back({601.0f + step,
                                       602.0f + step,
                                       603.0f + step,
                                       604.0f + step});
                }

                IndexOpResult result = vector_index->add_batch(vectors, ids);
                assert(result.error == VectorOpResult::SUCCEED);
            });
    }
    for (auto &thread : threads)
    {
        thread.join();
    }

    // get the vector
    std::vector<float> vec_result;
    VectorId vector_id;
    vector_id.id_ = 601;
    IndexOpResult result = vector_index->get(vector_id, vec_result);
    REQUIRE(result.error == VectorOpResult::SUCCEED);
    REQUIRE(vec_result.size() == 4);
    REQUIRE(vec_result[0] == 601.0);
    REQUIRE(vec_result[1] == 602.0);
    REQUIRE(vec_result[2] == 603.0);
    REQUIRE(vec_result[3] == 604.0);

    DLOG(INFO) << "VectorCache HNSW Concurrency Add Batch Operation done";
}

/* id range of this case is [1601, 1700] */
TEST_CASE("VectorCache HNSW Concurrency Search Operation",
          "[vector-cache-hnsw]")
{
    DLOG(INFO) << "VectorCache HNSW Concurrency Search Operation start";
    REQUIRE(vector_index != nullptr);
    REQUIRE(vector_index->is_ready());

    // add a batch of vectors
    std::vector<std::vector<float>> vectors = {
        {1601.0, 1602.0, 1603.0, 1604.0},
        {1611.0, 1612.0, 1613.0, 1614.0},
        {1621.0, 1622.0, 1623.0, 1624.0},
        {1631.0, 1632.0, 1633.0, 1634.0},
        {1641.0, 1642.0, 1643.0, 1644.0},
        {1651.0, 1652.0, 1653.0, 1654.0},
        {1661.0, 1662.0, 1663.0, 1664.0},
        {1671.0, 1672.0, 1673.0, 1674.0},
        {1681.0, 1682.0, 1683.0, 1684.0},
        {1691.0, 1692.0, 1693.0, 1694.0}};
    std::vector<VectorId> ids(10);
    ids[0].id_ = 1601;
    ids[1].id_ = 1602;
    ids[2].id_ = 1603;
    ids[3].id_ = 1604;
    ids[4].id_ = 1605;
    ids[5].id_ = 1606;
    ids[6].id_ = 1607;
    ids[7].id_ = 1608;
    ids[8].id_ = 1609;
    ids[9].id_ = 1610;
    IndexOpResult result = vector_index->add_batch(vectors, ids);
    REQUIRE(result.error == VectorOpResult::SUCCEED);

    // search sequentially
    const uint32_t worker_num = 10;
    for (uint32_t i = 0; i < 10; ++i)
    {
        float worker_step = i * 10.0f;
        std::vector<float> query_vector = {1601.0f + worker_step,
                                           1602.0f + worker_step,
                                           1603.0f + worker_step,
                                           1604.0f + worker_step};
        SearchResult search_result;
        result = vector_index->search(query_vector, 1, i, search_result);
        REQUIRE(result.error == VectorOpResult::SUCCEED);
        REQUIRE(search_result.ids.size() == 1);
    }

    // search the vector with 10 workers concurrently
    std::vector<std::thread> threads;
    for (uint32_t i = 0; i < worker_num; ++i)
    {
        threads.emplace_back(
            [i]()
            {
                float worker_step = i * 10.0f;
                // get the query vector
                std::vector<float> vector;
                VectorId vector_id;
                vector_id.id_ = 1601 + i;
                IndexOpResult result = vector_index->get(vector_id, vector);
                assert(result.error == VectorOpResult::SUCCEED);
                assert(vector.size() == 4);
                assert(vector[0] == 1601.0f + worker_step);
                assert(vector[1] == 1602.0f + worker_step);
                assert(vector[2] == 1603.0f + worker_step);
                assert(vector[3] == 1604.0f + worker_step);

                // search the vector
                std::vector<float> query_vector = {1601.0f + worker_step,
                                                   1602.0f + worker_step,
                                                   1603.0f + worker_step,
                                                   1604.0f + worker_step};
                SearchResult search_result;
                result =
                    vector_index->search(query_vector, 1, i, search_result);
                assert(result.error == VectorOpResult::SUCCEED);
                assert(search_result.ids.size() == 1);
            });
    }
    for (auto &thread : threads)
    {
        thread.join();
    }

    DLOG(INFO) << "VectorCache HNSW Concurrency Search Operation done";
}

/* id range of this case is [1701, 1800] */
TEST_CASE("VectorCache HNSW Concurrency Add Batch & Search Operation",
          "[vector-cache-hnsw]")
{
    DLOG(INFO)
        << "VectorCache HNSW Concurrency Add Batch & Search Operation start";
    REQUIRE(vector_index != nullptr);
    REQUIRE(vector_index->is_ready());

    // add a batch of vectors
    std::vector<std::vector<float>> vectors = {
        {1701.0, 1702.0, 1703.0, 1704.0},
        {1711.0, 1712.0, 1713.0, 1714.0},
        {1721.0, 1722.0, 1723.0, 1724.0},
        {1731.0, 1732.0, 1733.0, 1734.0},
        {1741.0, 1742.0, 1743.0, 1744.0},
        {1751.0, 1752.0, 1753.0, 1754.0},
        {1761.0, 1762.0, 1763.0, 1764.0},
        {1771.0, 1772.0, 1773.0, 1774.0},
        {1781.0, 1782.0, 1783.0, 1784.0},
        {1791.0, 1792.0, 1793.0, 1794.0},
    };
    std::vector<VectorId> ids(10);
    ids[0].id_ = 1701;
    ids[1].id_ = 1702;
    ids[2].id_ = 1703;
    ids[3].id_ = 1704;
    ids[4].id_ = 1705;
    ids[5].id_ = 1706;
    ids[6].id_ = 1707;
    ids[7].id_ = 1708;
    ids[8].id_ = 1709;
    ids[9].id_ = 1710;
    IndexOpResult result = vector_index->add_batch(vectors, ids);
    REQUIRE(result.error == VectorOpResult::SUCCEED);

    // add batch vector with search concurrently
    // add vector worker
    const uint32_t loop_num = 10;
    std::thread add_thread(
        [&]()
        {
            for (uint32_t i = 0; i < loop_num; ++i)
            {
                float worker_step = i * 2 * 10.0f;
                std::vector<std::vector<float>> vectors = {
                    {1801.0f + worker_step,
                     1802.0f + worker_step,
                     1803.0f + worker_step,
                     1804.0f + worker_step},
                    {1811.0f + worker_step,
                     1812.0f + worker_step,
                     1813.0f + worker_step,
                     1814.0f + worker_step},
                };
                uint64_t step = i * 5.0f;
                std::vector<VectorId> ids(2);
                ids[0].id_ = 1711 + step;
                ids[1].id_ = 1712 + step;
                IndexOpResult result = vector_index->add_batch(vectors, ids);
                assert(result.error == VectorOpResult::SUCCEED);
            }
        });

    // search vector worker
    std::thread search_thread(
        [&]()
        {
            for (uint32_t i = 0; i < loop_num; ++i)
            {
                float worker_step = i * 10.0f;
                std::vector<float> query_vector = {1701.0f + worker_step,
                                                   1702.0f + worker_step,
                                                   1703.0f + worker_step,
                                                   1704.0f + worker_step};
                SearchResult search_result;
                result =
                    vector_index->search(query_vector, 1, i, search_result);
                assert(result.error == VectorOpResult::SUCCEED);
                assert(search_result.ids.size() == 1);
            }
        });

    add_thread.join();
    search_thread.join();

    DLOG(INFO)
        << "VectorCache HNSW Concurrency Add Batch & Search Operation done";
}

/* id range of this case is [1801, 1900] */
TEST_CASE("VectorCache HNSW Invalid Parameters Operation",
          "[vector-cache-hnsw]")
{
    DLOG(INFO) << "VectorCache HNSW Invalid Parameters Operation start";
    REQUIRE(vector_index != nullptr);
    REQUIRE(vector_index->is_ready());

    // add a batch of vectors
    std::vector<std::vector<float>> vectors = {
        {1801.0, 1802.0, 1803.0, 1804.0},
        {1811.0, 1812.0, 1813.0, 1814.0},
    };
    std::vector<VectorId> ids(4);
    ids[0].id_ = 1801;
    ids[1].id_ = 1802;
    ids[2].id_ = 1803;
    ids[3].id_ = 1804;
    IndexOpResult result = vector_index->add_batch(vectors, ids);
    REQUIRE(result.error == VectorOpResult::INDEX_ADD_FAILED);

    // add a vector with invalid parameters
    std::vector<float> vector = {1801.0, 1802.0, 1803.0, 1804.0, 1805.0};
    VectorId vector_id;
    vector_id.id_ = 1801;
    result = vector_index->add(vector, vector_id);
    REQUIRE(result.error == VectorOpResult::VECTOR_DIMENSION_MISMATCH);

    // add a batch of vectors with invalid parameters
    vectors = {
        {1801.0, 1802.0, 1803.0, 1804.0, 1805.0},
        {1811.0, 1812.0, 1813.0, 1814.0, 1815.0},
    };
    ids.resize(2);
    ids[0].id_ = 1801;
    ids[1].id_ = 1802;
    result = vector_index->add_batch(vectors, ids);
    REQUIRE(result.error == VectorOpResult::VECTOR_DIMENSION_MISMATCH);

    DLOG(INFO) << "VectorCache HNSW Invalid Parameters Operation done";
}

/* id range of this case is [1901, 2000] */
TEST_CASE("VectorCache HNSW Concurrency Remove Operation",
          "[vector-cache-hnsw]")
{
    DLOG(INFO) << "VectorCache HNSW Concurrency Remove Operation start";
    REQUIRE(vector_index != nullptr);
    REQUIRE(vector_index->is_ready());

    // add a batch of vectors
    std::vector<std::vector<float>> vectors = {
        {1901.0, 1902.0, 1903.0, 1904.0},
        {1911.0, 1912.0, 1913.0, 1914.0},
        {1921.0, 1922.0, 1923.0, 1924.0},
        {1931.0, 1932.0, 1933.0, 1934.0},
        {1941.0, 1942.0, 1943.0, 1944.0},
        {1951.0, 1952.0, 1953.0, 1954.0},
        {1961.0, 1962.0, 1963.0, 1964.0},
        {1971.0, 1972.0, 1973.0, 1974.0},
        {1981.0, 1982.0, 1983.0, 1984.0},
        {1991.0, 1992.0, 1993.0, 1994.0},
    };
    std::vector<VectorId> ids(10);
    ids[0].id_ = 1901;
    ids[1].id_ = 1902;
    ids[2].id_ = 1903;
    ids[3].id_ = 1904;
    ids[4].id_ = 1905;
    ids[5].id_ = 1906;
    ids[6].id_ = 1907;
    ids[7].id_ = 1908;
    ids[8].id_ = 1909;
    ids[9].id_ = 1910;
    IndexOpResult result = vector_index->add_batch(vectors, ids);
    REQUIRE(result.error == VectorOpResult::SUCCEED);

    // remove the vector concurrently
    const uint32_t worker_num = 10;
    std::vector<std::thread> threads;
    for (uint32_t i = 0; i < worker_num; ++i)
    {
        threads.emplace_back(
            [i]()
            {
                float worker_step = i * 10.0f;
                // get the vector
                std::vector<float> vector_result;
                VectorId vector_id;
                vector_id.id_ = 1901 + i;
                IndexOpResult result =
                    vector_index->get(vector_id, vector_result);
                assert(result.error == VectorOpResult::SUCCEED);
                assert(vector_result.size() == 4);
                assert(vector_result[0] == 1901.0f + worker_step);
                assert(vector_result[1] == 1902.0f + worker_step);
                assert(vector_result[2] == 1903.0f + worker_step);
                assert(vector_result[3] == 1904.0f + worker_step);

                // remove the vector
                result = vector_index->remove(vector_id);
                assert(result.error == VectorOpResult::SUCCEED);

                // get the vector
                result = vector_index->get(vector_id, vector_result);
                assert(result.error == VectorOpResult::SUCCEED);
                assert(vector_result.size() == 0);
            });
    }
    for (auto &thread : threads)
    {
        thread.join();
    }

    DLOG(INFO) << "VectorCache HNSW Concurrency Remove Operation done";
}

/* id range of this case is [2001, 2100] */
TEST_CASE("VectorCache HNSW Add Operation with Metadata", "[vector-cache-hnsw]")
{
    DLOG(INFO) << "VectorCache HNSW Add Operation with Metadata start";
    REQUIRE(vector_index != nullptr);
    REQUIRE(vector_index->is_ready());

    // add a vector with metadata
    std::vector<float> vector = {2001.0, 2002.0, 2003.0, 2004.0};
    VectorId vector_id;
    vector_id.id_ = 2001;
    vector_id.metadata_.resize(metadata_size, 0);
    for (size_t i = 0; i < metadata_size; ++i)
    {
        vector_id.metadata_[i] = 'a' + i;
    }
    IndexOpResult result = vector_index->add(vector, vector_id);
    REQUIRE(result.error == VectorOpResult::SUCCEED);

    std::vector<float> vec_result;
    vector_index->get(vector_id, vec_result);
    REQUIRE(vec_result.size() == 4);
    REQUIRE(vec_result[0] == 2001.0);
    REQUIRE(vec_result[1] == 2002.0);
    REQUIRE(vec_result[2] == 2003.0);
    REQUIRE(vec_result[3] == 2004.0);

    DLOG(INFO) << "VectorCache HNSW Add Operation with Metadata done";
}

/* id range of this case is [2101, 2200] */
TEST_CASE("VectorCache HNSW Batch Add Operation with Metadata",
          "[vector-cache-hnsw]")
{
    DLOG(INFO) << "VectorCache HNSW Batch Add Operation with Metadata start";
    REQUIRE(vector_index != nullptr);
    REQUIRE(vector_index->is_ready());

    std::vector<std::vector<float>> vectors = {
        {2101.0, 2102.0, 2103.0, 2104.0}, {2111.0, 2112.0, 2113.0, 2114.0}};
    std::vector<VectorId> ids(2);
    ids[0].id_ = 2101;
    ids[1].id_ = 2102;
    ids[0].metadata_.resize(metadata_size, 0);
    for (size_t i = 0; i < metadata_size; ++i)
    {
        ids[0].metadata_[i] = 'b' + i;
    }
    ids[1].metadata_.resize(metadata_size, 0);
    for (size_t i = 0; i < metadata_size; ++i)
    {
        ids[1].metadata_[i] = 'b' + i;
    }
    IndexOpResult result = vector_index->add_batch(vectors, ids);
    REQUIRE(result.error == VectorOpResult::SUCCEED);

    std::vector<float> vec_result;
    vector_index->get(ids[1], vec_result);
    REQUIRE(vec_result.size() == 4);
    REQUIRE(vec_result[0] == 2111.0);
    REQUIRE(vec_result[1] == 2112.0);
    REQUIRE(vec_result[2] == 2113.0);
    REQUIRE(vec_result[3] == 2114.0);

    DLOG(INFO) << "VectorCache HNSW Batch Add Operation with Metadata done";
}

/* id range of this case is [2201, 2300] */
TEST_CASE("VectorCache HNSW Remove Operation with Metadata",
          "[vector-cache-hnsw]")
{
    DLOG(INFO) << "VectorCache HNSW Remove Operation with Metadata start";
    REQUIRE(vector_index != nullptr);
    REQUIRE(vector_index->is_ready());

    std::vector<float> vector = {2201.0, 2202.0, 2203.0, 2204.0};
    VectorId vector_id;
    vector_id.id_ = 2201;
    vector_id.metadata_.resize(metadata_size, 0);
    for (size_t i = 0; i < metadata_size; ++i)
    {
        vector_id.metadata_[i] = 'd' + i;
    }
    IndexOpResult result = vector_index->add(vector, vector_id);
    REQUIRE(result.error == VectorOpResult::SUCCEED);

    result = vector_index->remove(vector_id);
    REQUIRE(result.error == VectorOpResult::SUCCEED);

    // get the vector
    std::vector<float> vec_result;
    result = vector_index->get(vector_id, vec_result);
    REQUIRE(result.error == VectorOpResult::SUCCEED);
    REQUIRE(vec_result.size() == 0);

    DLOG(INFO) << "VectorCache HNSW Remove Operation with Metadata done";
}

/* id range of this case is [2301, 2400] */
TEST_CASE("VectorCache HNSW Search Operation with Metadata",
          "[vector-cache-hnsw]")
{
    DLOG(INFO) << "VectorCache HNSW Search Operation with Metadata start";
    REQUIRE(vector_index != nullptr);
    REQUIRE(vector_index->is_ready());

    std::vector<float> vector = {2301.0, 2302.0, 2303.0, 2304.0};
    VectorId vector_id;
    vector_id.id_ = 2301;
    vector_id.metadata_.resize(metadata_size, 0);
    for (size_t i = 0; i < metadata_size; ++i)
    {
        vector_id.metadata_[i] = 'e' + i;
    }
    IndexOpResult result = vector_index->add(vector, vector_id);
    REQUIRE(result.error == VectorOpResult::SUCCEED);

    std::vector<float> query_vector = {2301.0, 2302.0, 2303.0, 2304.0};
    SearchResult search_result;
    result = vector_index->search(query_vector, 1, 0, search_result);
    REQUIRE(result.error == VectorOpResult::SUCCEED);
    REQUIRE(search_result.ids.size() == 1);
    REQUIRE(search_result.distances[0] == 0.0);

    DLOG(INFO) << "VectorCache HNSW Search Operation with Metadata done";
}

/* id range of this case is [2401, 2500] */
TEST_CASE("VectorCache HNSW Save & Load Operation with Metadata",
          "[vector-cache-hnsw]")
{
    // TODO(ysw): Disable this test case for now
    return;
    DLOG(INFO) << "VectorCache HNSW Save & Load Operation with Metadata start";
    REQUIRE(vector_index != nullptr);
    REQUIRE(vector_index->is_ready());

    // add a vector
    std::vector<float> vector = {2401.0, 2402.0, 2403.0, 2404.0};
    VectorId vector_id;
    vector_id.id_ = 2401;
    vector_id.metadata_.resize(metadata_size, 0);
    for (size_t i = 0; i < metadata_size; ++i)
    {
        vector_id.metadata_[i] = 'f' + i;
    }
    IndexOpResult res = vector_index->add(vector, vector_id);
    REQUIRE(res.error == VectorOpResult::SUCCEED);

    bool result = vector_index->save(index_path);
    REQUIRE(result);

    // reinitialize the index
    vector_index.reset();
    vector_index = EloqVec::create_hnsw_vector_index();
    REQUIRE(vector_index != nullptr);
    REQUIRE(vector_index->initialize(index_config, index_path));
    REQUIRE(vector_index->is_ready());

    // get the vector
    std::vector<float> vector_result;
    res = vector_index->get(vector_id, vector_result);
    REQUIRE(res.error == VectorOpResult::SUCCEED);
    REQUIRE(vector_result.size() == 4);
    REQUIRE(vector_result[0] == 2401.0);
    REQUIRE(vector_result[1] == 2402.0);
    REQUIRE(vector_result[2] == 2403.0);
    REQUIRE(vector_result[3] == 2404.0);

    // add a vector
    std::vector<float> vector1 = {2411.0f, 2412.0f, 2413.0f, 2414.0f};
    VectorId vector_id1;
    vector_id1.id_ = 2411;
    vector_id1.metadata_.resize(metadata_size, 0);
    for (size_t i = 0; i < metadata_size; ++i)
    {
        vector_id1.metadata_[i] = 'g' + i;
    }
    res = vector_index->add(vector1, vector_id1);
    REQUIRE(res.error == VectorOpResult::SUCCEED);

    DLOG(INFO) << "VectorCache HNSW Save & Load Operation with Metadata done";
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

    // set up the index configuration
    EloqVec::SetUpIndexConfig();

    // clean up the index file
    if (!EloqVec::index_path.empty() &&
        std::filesystem::exists(EloqVec::index_path))
    {
        std::filesystem::remove(EloqVec::index_path);
    }

    if (!EloqVec::HNSWVectorIndex::validate_config(EloqVec::index_config))
    {
        DLOG(ERROR) << "Invalid hnsw vector index configuration";
        return -1;
    }

    // create and initialize the hnsw vector index
    EloqVec::vector_index = EloqVec::create_hnsw_vector_index();
    if (!EloqVec::vector_index->initialize(EloqVec::index_config,
                                           EloqVec::index_path))
    {
        DLOG(ERROR) << "Failed to initialize hnsw vector";
        return -1;
    }

    cres = session.run();
    // reset the vector index
    EloqVec::vector_index.reset();
    return cres;
}
