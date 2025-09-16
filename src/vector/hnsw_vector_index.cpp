/**
 * @file hnsw_vector_index.cpp
 * @brief HNSW vector index implementation
 * 
 * This file contains the implementation of the HNSWVectorIndex class.
 * 
 * @author EloqData Inc.
 * @date 2025
 */

#include "vector/hnsw_vector_index.h"
#include <usearch/index.hpp>
#include <usearch/index_dense.hpp>
#include <usearch/index_plugins.hpp>
#include <shared_mutex>

namespace EloqVec {

using namespace unum::usearch;

HNSWVectorIndex::HNSWVectorIndex()
    : initialized_(false)
{
}

HNSWVectorIndex::~HNSWVectorIndex() = default;

bool HNSWVectorIndex::initialize(const IndexConfig& config) {
    std::lock_guard<std::shared_mutex> lock(index_mutex_);
    
    if (!validate_config(config)) {
        return false;
    }
    
    config_ = config;
    return initialize_usearch_index(config);
}

bool HNSWVectorIndex::load(const std::string& path) {
    std::lock_guard<std::shared_mutex> lock(index_mutex_);
 
    if (path.empty()) {
        return false;
    }

    if (initialized_) {
        return false;
    }
 
    try {
        // Load the index from file using usearch API
        auto load_result = usearch_index_.load(path.c_str());
        if (!load_result) {
            return false;
        }

        // Update state
        initialized_ = true;
        
        return true;
    } catch (const std::exception& e) {
        initialized_ = false;
        return false;
    }

    return true;
}

bool HNSWVectorIndex::save(const std::string& path) {
    std::lock_guard<std::shared_mutex> lock(index_mutex_);
    
    if (!initialized_) {
        return false;
    }
    
    if (path.empty()) {
        return false;
    }
    
    try {
        // Save the index to file using usearch API
        auto save_result = usearch_index_.save(path.c_str());
        if (!save_result) {
            return false;
        }
        
        return true;
    } catch (const std::exception& e) {
        return false;
    }

    return true;
}

bool HNSWVectorIndex::validate_config(const IndexConfig& config) const {
    if (config.dimension == 0) {
        return false;
    }
 
    if (config.max_elements == 0) {
        return false;
    }

    // Verify the passedd in additional parameters
    for (const auto& param : config.params) {
        if (param.first != "m" && param.first != "ef_construction" && param.first != "ef_search") {
            return false;
        }
    }

    return true;
}

bool HNSWVectorIndex::initialize_usearch_index(const IndexConfig& config) {
    try {
        // Convert our distance metric to usearch metric
        metric_kind_t metric_type = convert_metric(config.distance_metric);
        
        // Create usearch index configuration
        index_dense_config_t index_config;
        // Get HNSW-specific parameters from config
        
        // Try to get custom parameters from config.params
        auto m_it = config.params.find("m");
        if (m_it != config.params.end()) {
            index_config.connectivity = std::stoul(m_it->second);
        }
        
        auto ef_it = config.params.find("ef_construction");
        if (ef_it != config.params.end()) {
            index_config.expansion_add = std::stoul(ef_it->second);
        }

        auto ef_search_it = config.params.find("ef_search");
        if (ef_search_it != config.params.end()) {
            index_config.expansion_search = std::stoul(ef_search_it->second);
        }

        // Initialize the usearch index
        metric_punned_t metric(config.dimension, metric_type, scalar_kind_t::f32_k);
        auto init_result = index_dense_t::make(metric, index_config);
        if (init_result.error) {
            return false;
        }
        if (!init_result.index.try_reserve(index_limits_t(config.max_elements))) {
            return false;
        }

        usearch_index_ = std::move(init_result);
        initialized_ = true;
        
        return true;
    } catch (const std::exception& e) {
        initialized_ = false;
        return false;
    }
}

metric_kind_t HNSWVectorIndex::convert_metric(DistanceMetric metric) const {
    switch (metric) {
        case DistanceMetric::L2SQ:
            return metric_kind_t::l2sq_k;
        case DistanceMetric::IP:
            return metric_kind_t::ip_k;
        case DistanceMetric::COSINE:
            return metric_kind_t::cos_k;
        default:
            return metric_kind_t::l2sq_k;
    }
}

IndexOpResult HNSWVectorIndex::search(
    const std::vector<float>& query_vector,
    size_t k,
    SearchResult &result,
    bool exact,
    std::optional<std::function<bool(uint64_t)>> filter
) {
    std::shared_lock<std::shared_mutex> lock(index_mutex_);
    if (!initialized_) {
        return IndexOpResult(VectorOpResult::INDEX_NOT_EXIST, "Index not initialized");
    }
    
    if (query_vector.size() != config_.dimension) {
        return IndexOpResult(VectorOpResult::VECTOR_DIMENSION_MISMATCH, "Query vector dimension mismatch");
    }
    
    try {
        if (filter) {
            // Use filtered search with predicate
            auto search_result = usearch_index_.filtered_search(
                query_vector.data(), 
                k, 
                [&filter](auto key) { return (*filter)(key); },
                0,  // thread
                exact
            );
            
            if (search_result.error) {
                return IndexOpResult(VectorOpResult::INDEX_INTERNAL_ERROR, search_result.error.what());
            }
            
            // Extract results
            result.ids.resize(search_result.count);
            result.distances.resize(search_result.count);
            search_result.dump_to(result.ids.data(), result.distances.data());
        } else {
            // Use regular search
            auto search_result = usearch_index_.search(
                query_vector.data(), 
                k, 
                0,  // thread
                exact
            );
            
            if (search_result.error) {
                return IndexOpResult(VectorOpResult::INDEX_INTERNAL_ERROR, search_result.error.what());
            }
            
            // Extract results
            result.ids.resize(search_result.count);
            result.distances.resize(search_result.count);
            search_result.dump_to(result.ids.data(), result.distances.data());
        }
        
        return IndexOpResult(VectorOpResult::SUCCEED, "");
    } catch (const std::exception& e) {
        return IndexOpResult(VectorOpResult::INDEX_INTERNAL_ERROR, e.what());
    }
}

IndexOpResult HNSWVectorIndex::add(const std::vector<float>& vector, uint64_t id) {
    std::shared_lock<std::shared_mutex> lock(index_mutex_);
    
    if (!initialized_) {
        return IndexOpResult(VectorOpResult::INDEX_NOT_EXIST, "Index not initialized");
    }
    
    if (vector.size() != config_.dimension) {
        return IndexOpResult(VectorOpResult::VECTOR_DIMENSION_MISMATCH, "Vector dimension mismatch");
    }
    
    try {
        // Add vector to usearch index
        auto add_result = usearch_index_.add(id, vector.data(), 0, true);
        if (add_result.error) {
            return IndexOpResult(VectorOpResult::INDEX_INTERNAL_ERROR, add_result.error.what());
        }
        
        return IndexOpResult(VectorOpResult::SUCCEED, "");
    } catch (const std::exception& e) {
        return IndexOpResult(VectorOpResult::INDEX_INTERNAL_ERROR, e.what());
    }
}

IndexOpResult HNSWVectorIndex::add_batch(
    const std::vector<std::vector<float>>& vectors,
    const std::vector<uint64_t>& ids
) {
    std::shared_lock<std::shared_mutex> lock(index_mutex_);
    
    assert(vectors.size() == ids.size());
    if (!initialized_) {
        return IndexOpResult(VectorOpResult::INDEX_NOT_EXIST, "Index not initialized");
    }
    
    try {
        // Add vectors one by one (usearch doesn't have native batch add)
        for (size_t i = 0; i < vectors.size(); ++i) {
            if (vectors[i].size() != config_.dimension) {
                return IndexOpResult(VectorOpResult::VECTOR_DIMENSION_MISMATCH, "Vector dimension mismatch");
            }
            
            auto add_result = usearch_index_.add(ids[i], vectors[i].data(), 0, true);
            if (add_result.error) {
                return IndexOpResult(VectorOpResult::UNKNOWN, add_result.error.what());
            }
        }
        
        return IndexOpResult(VectorOpResult::SUCCEED, "");
    } catch (const std::exception& e) {
        return IndexOpResult(VectorOpResult::INDEX_INTERNAL_ERROR, e.what());
    }
}

IndexOpResult HNSWVectorIndex::remove(uint64_t id) {
    std::shared_lock<std::shared_mutex> lock(index_mutex_);
    
    if (!initialized_) {
        return IndexOpResult(VectorOpResult::INDEX_NOT_EXIST, "Index not initialized");
    }
    
    try {
        // Remove vector from usearch index
        auto remove_result = usearch_index_.remove(id);
        if (remove_result.error) {
            return IndexOpResult(VectorOpResult::INDEX_INTERNAL_ERROR, remove_result.error.what());
        }
        
        return IndexOpResult(VectorOpResult::SUCCEED, "");
    } catch (const std::exception& e) {
        return IndexOpResult(VectorOpResult::INDEX_INTERNAL_ERROR, e.what());
    }
}

IndexOpResult HNSWVectorIndex::update(const std::vector<float>& vector, uint64_t id) {
    std::shared_lock<std::shared_mutex> lock(index_mutex_);
    
    if (!initialized_) {
        return IndexOpResult(VectorOpResult::INDEX_NOT_EXIST, "Index not initialized");
    }
    
    if (vector.size() != config_.dimension) {
        return IndexOpResult(VectorOpResult::VECTOR_DIMENSION_MISMATCH, "Vector dimension mismatch");
    }
    
    try {
        // For update, we need to remove the old vector and add the new one
        // First, try to remove the existing vector (ignore if it doesn't exist)
        auto remove_result = usearch_index_.remove(id);
        // Don't check remove_result.error as the vector might not exist
        
        // Add the new vector
        auto add_result = usearch_index_.add(id, vector.data(), 0, true);
        if (add_result.error) {
            return IndexOpResult(VectorOpResult::INDEX_INTERNAL_ERROR, add_result.error.what());
        }
        
        return IndexOpResult(VectorOpResult::SUCCEED, "");
    } catch (const std::exception& e) {
        return IndexOpResult(VectorOpResult::INDEX_INTERNAL_ERROR, e.what());
    }
}

size_t HNSWVectorIndex::memory_usage() {
    std::shared_lock<std::shared_mutex> lock(index_mutex_);
    if (!initialized_) {
        return 0;
    }
    return usearch_index_.memory_usage();
}


bool HNSWVectorIndex::is_ready() {
    std::shared_lock<std::shared_mutex> lock(index_mutex_);
    return initialized_;
}

size_t HNSWVectorIndex::get_dimension() {
    std::shared_lock<std::shared_mutex> lock(index_mutex_);
    if (!initialized_) {
        return 0;
    }
    return config_.dimension;
}

size_t HNSWVectorIndex::size() {
    std::shared_lock<std::shared_mutex> lock(index_mutex_);
    if (!initialized_) {
        return 0;
    }
    return usearch_index_.size();
}

bool HNSWVectorIndex::optimize() {
    std::shared_lock<std::shared_mutex> lock(index_mutex_);
    
    if (!initialized_) {
        return false;
    }
    
    // TODO: Implement optimization functionality
    return true;
}

std::string HNSWVectorIndex::get_type() const {
    return "HNSW";
}

bool HNSWVectorIndex::set_search_params(std::unordered_map<std::string, std::string> params) {
    std::lock_guard<std::shared_mutex> lock(index_mutex_);
    if (params.find("ef_search") == params.end()) {
        return false;
    }

    size_t ef_search = std::stoul(params["ef_search"]);
    usearch_index_.change_expansion_search(ef_search);
    
    return true;
}

bool HNSWVectorIndex::set_update_params(std::unordered_map<std::string, std::string> params) {
    std::lock_guard<std::shared_mutex> lock(index_mutex_);
    if (params.find("ef_construction") == params.end()) {
        return false;
    }
    size_t ef_construction = std::stoul(params["ef_construction"]);
    usearch_index_.change_expansion_add(ef_construction);
    return true;
}

std::unique_ptr<VectorIndex> create_hnsw_vector_index() {
    return std::make_unique<HNSWVectorIndex>();
}

} // namespace EloqVec
