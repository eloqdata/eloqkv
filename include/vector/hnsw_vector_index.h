/**
 * @file hnsw_vector_index.h
 * @brief HNSW (Hierarchical Navigable Small World) vector index implementation
 *
 * This file defines the HNSWVectorIndex class that implements the VectorIndex
 * interface using the usearch library for high-performance approximate nearest
 * neighbor search.
 *
 * @author EloqData Inc.
 * @date 2025
 */

#pragma once

#include <memory>
#include <mutex>
#include <unordered_map>
#include <usearch/index_dense.hpp>
#include <usearch/index_plugins.hpp>

#include "vector_index.h"
namespace EloqVec
{

/**
 * @brief HNSW vector index implementation using usearch library
 *
 * This class provides an HNSW-based implementation of the VectorIndex
 * interface, offering high-performance approximate nearest neighbor search with
 * configurable accuracy and speed trade-offs.
 */
class HNSWVectorIndex : public VectorIndex
{
public:
    static bool validate_config(const IndexConfig &config);

    /**
     * @brief Constructor
     */
    HNSWVectorIndex();

    /**
     * @brief Destructor
     */
    virtual ~HNSWVectorIndex();

    // VectorIndex interface implementation
    bool initialize(const IndexConfig &config,
                    const std::string &path) override;
    bool save(const std::string &path) override;

    IndexOpResult search(const std::vector<float> &query_vector,
                         size_t k,
                         size_t thread_id,
                         SearchResult &result,
                         bool exact = false,
                         std::optional<std::function<bool(uint64_t)>> filter =
                             std::nullopt) override;

    IndexOpResult add(const std::vector<float> &vector, uint64_t id) override;
    IndexOpResult add_batch(const std::vector<std::vector<float>> &vectors,
                            const std::vector<uint64_t> &ids) override;
    IndexOpResult remove(uint64_t id) override;
    IndexOpResult update(const std::vector<float> &vector,
                         uint64_t id) override;
    IndexOpResult get(uint64_t id, std::vector<float> &vector) override;
    size_t memory_usage() override;
    bool is_ready() override;
    size_t size() override;
    bool optimize() override;
    std::string get_type() const override;
    bool set_search_params(
        std::unordered_map<std::string, std::string> params) override;
    bool set_update_params(
        std::unordered_map<std::string, std::string> params) override;

private:
    /**
     * @brief Convert DistanceMetric enum to usearch metric type
     *
     * @param metric EloqVec distance metric
     * @return usearch metric type
     */
    unum::usearch::metric_kind_t convert_metric(DistanceMetric metric) const;

    /**
     * @brief Initialize usearch index with configuration
     *
     * @param config Configuration parameters
     * @return true if successful, false otherwise
     */
    bool initialize_usearch_index(const IndexConfig &config);

    /**
     * @brief Load an existing index from storage.
     *
     * @param config Configuration parameters for max_elements
     * @param file_path Path to the index file
     * @return true if loading successful, false otherwise
     */
    bool load(const IndexConfig &config, const std::string &file_path);

private:
    // usearch index instance
    unum::usearch::index_dense_t usearch_index_;

    // Thread safety
    std::shared_mutex index_mutex_;

    // Internal state
    bool initialized_;
};

/**
 * @brief Factory function for creating HNSWVectorIndex instances
 *
 * @return Unique pointer to HNSWVectorIndex instance
 */
std::unique_ptr<VectorIndex> create_hnsw_vector_index();

}  // namespace EloqVec
