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

#include "vector_index.h"
#include <memory>
#include <mutex>
#include <unordered_map>

#include <usearch/index_dense.hpp>
#include <usearch/index_plugins.hpp>
namespace EloqVec {

/**
 * @brief HNSW vector index implementation using usearch library
 * 
 * This class provides an HNSW-based implementation of the VectorIndex interface,
 * offering high-performance approximate nearest neighbor search with configurable
 * accuracy and speed trade-offs.
 */
class HNSWVectorIndex : public VectorIndex {
public:
    /**
     * @brief Constructor
     */
    HNSWVectorIndex();

    /**
     * @brief Destructor
     */
    virtual ~HNSWVectorIndex();

    // VectorIndex interface implementation
    bool initialize(const IndexConfig& config) override;
    bool load(const std::string& path) override;
    bool save(const std::string& path) override;
    
    SearchResult search(
        const std::vector<float>& query_vector,
        size_t k,
        bool exact = false,
        std::optional<std::function<bool(uint64_t)>> filter = std::nullopt
    ) override;
    
    bool add(const std::vector<float>& vector, uint64_t id) override;
    bool add_batch(
        const std::vector<std::vector<float>>& vectors,
        const std::vector<uint64_t>& ids
    ) override;
    bool remove(uint64_t id) override;
    bool update(const std::vector<float>& vector, uint64_t id) override;
    size_t memory_usage() override;
    bool is_ready() override;
    size_t get_dimension() override;
    size_t size() override;
    bool optimize() override;
    std::string get_type() const override;
    bool set_search_params(std::unordered_map<std::string, std::string> params) override;
    bool set_update_params(std::unordered_map<std::string, std::string> params) override;

private:
    /**
     * @brief Convert DistanceMetric enum to usearch metric type
     * 
     * @param metric EloqVec distance metric
     * @return usearch metric type
     */
    unum::usearch::metric_kind_t convert_metric(DistanceMetric metric) const;

    /**
     * @brief Validate configuration parameters
     * 
     * @param config Configuration to validate
     * @return true if valid, false otherwise
     */
    bool validate_config(const IndexConfig& config) const;

    /**
     * @brief Initialize usearch index with configuration
     * 
     * @param config Configuration parameters
     * @return true if successful, false otherwise
     */
    bool initialize_usearch_index(const IndexConfig& config);

private:
    // usearch index instance
    unum::usearch::index_dense_t usearch_index_;
    
    // Configuration
    IndexConfig config_;
    
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

} // namespace EloqVec
