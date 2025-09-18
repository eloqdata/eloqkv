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

#include <shared_mutex>
#include <usearch/index.hpp>
#include <usearch/index_dense.hpp>
#include <usearch/index_plugins.hpp>

namespace EloqVec
{

using namespace unum::usearch;

HNSWVectorIndex::HNSWVectorIndex() : initialized_(false)
{
}

HNSWVectorIndex::~HNSWVectorIndex() = default;

bool HNSWVectorIndex::initialize(const IndexConfig &config)
{
    std::lock_guard<std::shared_mutex> lock(index_mutex_);

    config_ = config;
    return initialize_usearch_index(config);
}

/**
 * @brief Loads an HNSW index from disk into this instance.
 *
 * Attempts to load a usearch-backed index from the file at |path|. The method
 * acquires an exclusive lock for thread safety and will fail if |path| is
 * empty or the index is already initialized.
 *
 * On successful load the internal index is set and the instance becomes ready
 * (initialized_ = true). If loading fails or an exception is caught the
 * instance remains not-initialized (initialized_ = false).
 *
 * @param path Filesystem path to the serialized index file.
 * @return true if the index was loaded and the instance is initialized; false
 *         otherwise (invalid path, already initialized, load failure, or
 *         exception).
 */
bool HNSWVectorIndex::load(const std::string &path)
{
    std::lock_guard<std::shared_mutex> lock(index_mutex_);

    if (path.empty())
    {
        return false;
    }

    if (initialized_)
    {
        return false;
    }

    try
    {
        // Load the index from file using usearch API
        auto load_result = usearch_index_.load(path.c_str());
        if (!load_result)
        {
            return false;
        }

        // Update state
        initialized_ = true;

        return true;
    }
    catch (const std::exception &e)
    {
        initialized_ = false;
        return false;
    }
}

bool HNSWVectorIndex::save(const std::string &path)
{
    std::lock_guard<std::shared_mutex> lock(index_mutex_);

    if (!initialized_)
    {
        return false;
    }

    if (path.empty())
    {
        return false;
    }

    try
    {
        // Save the index to file using usearch API
        auto save_result = usearch_index_.save(path.c_str());
        if (!save_result)
        {
            return false;
        }

        return true;
    }
    catch (const std::exception &e)
    {
        return false;
    }

    return true;
}

/**
 * @brief Validate an index configuration for HNSW index creation.
 *
 * Performs basic sanity checks on the provided IndexConfig:
 * - dimension and max_elements must be non-zero.
 * - config.params may contain only the keys "m", "ef_construction", and
 * "ef_search".
 *
 * @param config Configuration to validate; its `dimension` and `max_elements`
 * are checked, and `params` is restricted to the allowed keys listed above.
 * @return true if the configuration passes all checks and is usable for
 * initialization.
 * @return false if any check fails.
 */
bool HNSWVectorIndex::validate_config(const IndexConfig &config)
{
    if (config.dimension == 0)
    {
        return false;
    }

    if (config.max_elements == 0)
    {
        return false;
    }

    // Verify the passedd in additional parameters
    for (const auto &param : config.params)
    {
        if (param.first != "m" && param.first != "ef_construction" &&
            param.first != "ef_search")
        {
            return false;
        }
    }

    return true;
}

bool HNSWVectorIndex::initialize_usearch_index(const IndexConfig &config)
{
    try
    {
        // Convert our distance metric to usearch metric
        metric_kind_t metric_type = convert_metric(config.distance_metric);

        // Create usearch index configuration
        index_dense_config_t index_config;
        // Get HNSW-specific parameters from config

        // Try to get custom parameters from config.params
        auto m_it = config.params.find("m");
        if (m_it != config.params.end())
        {
            index_config.connectivity = std::stoul(m_it->second);
        }

        auto ef_it = config.params.find("ef_construction");
        if (ef_it != config.params.end())
        {
            index_config.expansion_add = std::stoul(ef_it->second);
        }

        auto ef_search_it = config.params.find("ef_search");
        if (ef_search_it != config.params.end())
        {
            index_config.expansion_search = std::stoul(ef_search_it->second);
        }

        // Initialize the usearch index
        metric_punned_t metric(
            config.dimension, metric_type, scalar_kind_t::f32_k);
        auto init_result = index_dense_t::make(metric, index_config);
        if (init_result.error)
        {
            return false;
        }
        if (!init_result.index.try_reserve(index_limits_t(config.max_elements)))
        {
            return false;
        }

        usearch_index_ = std::move(init_result);
        initialized_ = true;

        return true;
    }
    catch (const std::exception &e)
    {
        initialized_ = false;
        return false;
    }
}

/**
 * @brief Convert a DistanceMetric to the corresponding usearch metric_kind_t.
 *
 * Maps supported DistanceMetric values to usearch's metric kinds:
 * - DistanceMetric::L2SQ -> metric_kind_t::l2sq_k
 * - DistanceMetric::IP   -> metric_kind_t::ip_k
 * - DistanceMetric::COSINE -> metric_kind_t::cos_k
 *
 * @param metric The distance metric to convert.
 * @return metric_kind_t The matching usearch metric kind. Unrecognized values
 * default to metric_kind_t::l2sq_k.
 */
metric_kind_t HNSWVectorIndex::convert_metric(DistanceMetric metric) const
{
    switch (metric)
    {
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

/**
 * @brief Searches the HNSW index for the nearest neighbors of a query vector.
 *
 * Performs either a regular or filtered k-NN search against the initialized
 * usearch HNSW index and populates the provided SearchResult with matched IDs
 * and distances.
 *
 * @param query_vector Query vector; its size must equal the index dimension
 * configured at initialization.
 * @param k Number of nearest neighbors to retrieve.
 * @param result Output parameter that will be resized and filled with matching
 * IDs and distances.
 * @param exact If true, request an exact search mode when supported by the
 * underlying index; otherwise allow approximate search.
 * @param filter Optional predicate invoked with an item ID to exclude or
 * include items during search.
 *
 * @return IndexOpResult Indicates success or a specific failure:
 * - VectorOpResult::SUCCEED: search completed and `result` is populated.
 * - VectorOpResult::INDEX_NOT_EXIST: index is not initialized.
 * - VectorOpResult::VECTOR_DIMENSION_MISMATCH: query_vector size does not match
 * index dimension.
 * - VectorOpResult::INDEX_INTERNAL_ERROR: an internal error occurred (error
 * message provided).
 */
IndexOpResult HNSWVectorIndex::search(
    const std::vector<float> &query_vector,
    size_t k,
    SearchResult &result,
    bool exact,
    std::optional<std::function<bool(uint64_t)>> filter)
{
    std::shared_lock<std::shared_mutex> lock(index_mutex_);
    if (!initialized_)
    {
        return IndexOpResult(VectorOpResult::INDEX_NOT_EXIST,
                             "Index not initialized");
    }

    if (query_vector.size() != config_.dimension)
    {
        return IndexOpResult(VectorOpResult::VECTOR_DIMENSION_MISMATCH,
                             "Query vector dimension mismatch");
    }

    try
    {
        if (filter)
        {
            // Use filtered search with predicate
            auto search_result = usearch_index_.filtered_search(
                query_vector.data(),
                k,
                [&filter](auto key) { return (*filter)(key); },
                0,  // thread
                exact);

            if (search_result.error)
            {
                return IndexOpResult(VectorOpResult::INDEX_INTERNAL_ERROR,
                                     search_result.error.what());
            }

            // Extract results
            result.ids.resize(search_result.count);
            result.distances.resize(search_result.count);
            search_result.dump_to(result.ids.data(), result.distances.data());
        }
        else
        {
            // Use regular search
            auto search_result = usearch_index_.search(query_vector.data(),
                                                       k,
                                                       0,  // thread
                                                       exact);

            if (search_result.error)
            {
                return IndexOpResult(VectorOpResult::INDEX_INTERNAL_ERROR,
                                     search_result.error.what());
            }

            // Extract results
            result.ids.resize(search_result.count);
            result.distances.resize(search_result.count);
            search_result.dump_to(result.ids.data(), result.distances.data());
        }

        return IndexOpResult(VectorOpResult::SUCCEED, "");
    }
    catch (const std::exception &e)
    {
        return IndexOpResult(VectorOpResult::INDEX_INTERNAL_ERROR, e.what());
    }
}

/**
 * @brief Adds a single vector with the given identifier to the HNSW index.
 *
 * The provided vector must match the index's configured dimensionality; the
 * method returns an error result if the index is not initialized or the
 * dimension differs.
 *
 * @param vector The float vector to insert; length must equal the index
 * dimension.
 * @param id     Unique identifier for the vector within the index.
 * @return IndexOpResult Result of the operation. Possible results:
 *         - VectorOpResult::SUCCEED on success.
 *         - VectorOpResult::INDEX_NOT_EXIST if the index is not initialized.
 *         - VectorOpResult::VECTOR_DIMENSION_MISMATCH if the vector length
 * differs from the index dimension.
 *         - VectorOpResult::INDEX_INTERNAL_ERROR if the underlying index
 * reports an error or an exception occurs.
 */
IndexOpResult HNSWVectorIndex::add(const std::vector<float> &vector,
                                   uint64_t id)
{
    std::shared_lock<std::shared_mutex> lock(index_mutex_);

    if (!initialized_)
    {
        return IndexOpResult(VectorOpResult::INDEX_NOT_EXIST,
                             "Index not initialized");
    }

    if (vector.size() != config_.dimension)
    {
        return IndexOpResult(VectorOpResult::VECTOR_DIMENSION_MISMATCH,
                             "Vector dimension mismatch");
    }

    try
    {
        // Add vector to usearch index
        auto add_result = usearch_index_.add(id, vector.data(), 0, true);
        if (add_result.error)
        {
            return IndexOpResult(VectorOpResult::INDEX_INTERNAL_ERROR,
                                 add_result.error.what());
        }

        return IndexOpResult(VectorOpResult::SUCCEED, "");
    }
    catch (const std::exception &e)
    {
        return IndexOpResult(VectorOpResult::INDEX_INTERNAL_ERROR, e.what());
    }
}

/**
 * @brief Adds multiple vectors to the HNSW index in a single operation.
 *
 * Attempts to insert each vector in `vectors` with the corresponding identifier
 * in `ids`. The two input containers must have equal length; each vector must
 * match the index's configured dimension. Vectors are inserted sequentially;
 * failure of any insertion aborts the operation and returns the corresponding
 * error.
 *
 * @param vectors Collection of vectors to add. Each inner vector must have
 * length equal to the index dimension.
 * @param ids Parallel collection of identifiers; must have the same size as
 * `vectors`. Each id is assigned to the vector at the same index in `vectors`.
 * @return IndexOpResult
 *         - SUCCEED on successful insertion of all vectors.
 *         - INDEX_NOT_EXIST if the index is not initialized.
 *         - VECTOR_DIMENSION_MISMATCH if any vector does not match the
 * configured dimension.
 *         - UNKNOWN if a usearch add operation reports an error (returned
 * message contains the underlying error).
 *         - INDEX_INTERNAL_ERROR if an unexpected exception occurs (returned
 * message contains the exception text).
 */
IndexOpResult HNSWVectorIndex::add_batch(
    const std::vector<std::vector<float>> &vectors,
    const std::vector<uint64_t> &ids)
{
    std::shared_lock<std::shared_mutex> lock(index_mutex_);

    assert(vectors.size() == ids.size());
    if (!initialized_)
    {
        return IndexOpResult(VectorOpResult::INDEX_NOT_EXIST,
                             "Index not initialized");
    }

    try
    {
        // Add vectors one by one (usearch doesn't have native batch add)
        for (size_t i = 0; i < vectors.size(); ++i)
        {
            if (vectors[i].size() != config_.dimension)
            {
                return IndexOpResult(VectorOpResult::VECTOR_DIMENSION_MISMATCH,
                                     "Vector dimension mismatch");
            }

            auto add_result =
                usearch_index_.add(ids[i], vectors[i].data(), 0, true);
            if (add_result.error)
            {
                return IndexOpResult(VectorOpResult::UNKNOWN,
                                     add_result.error.what());
            }
        }

        return IndexOpResult(VectorOpResult::SUCCEED, "");
    }
    catch (const std::exception &e)
    {
        return IndexOpResult(VectorOpResult::INDEX_INTERNAL_ERROR, e.what());
    }
}

/**
 * @brief Remove a vector from the index by its identifier.
 *
 * Attempts to remove the entry with the given persistent identifier from the
 * underlying HNSW index. If the index has not been initialized the operation
 * fails with INDEX_NOT_EXIST. On success returns SUCCEED; internal failures
 * (including exceptions) are reported as INDEX_INTERNAL_ERROR with an
 * explanatory message.
 *
 * @param id Identifier of the vector to remove.
 * @return IndexOpResult Result of the operation (SUCCEED, INDEX_NOT_EXIST, or
 * INDEX_INTERNAL_ERROR).
 */
IndexOpResult HNSWVectorIndex::remove(uint64_t id)
{
    std::shared_lock<std::shared_mutex> lock(index_mutex_);

    if (!initialized_)
    {
        return IndexOpResult(VectorOpResult::INDEX_NOT_EXIST,
                             "Index not initialized");
    }

    try
    {
        // Remove vector from usearch index
        auto remove_result = usearch_index_.remove(id);
        if (remove_result.error)
        {
            return IndexOpResult(VectorOpResult::INDEX_INTERNAL_ERROR,
                                 remove_result.error.what());
        }

        return IndexOpResult(VectorOpResult::SUCCEED, "");
    }
    catch (const std::exception &e)
    {
        return IndexOpResult(VectorOpResult::INDEX_INTERNAL_ERROR, e.what());
    }
}

/**
 * @brief Replace the vector for a given ID in the HNSW index.
 *
 * Attempts to remove any existing vector with the provided `id` and then adds
 * the supplied `vector` in its place.
 *
 * @param vector New feature vector to associate with `id`. Must have the same
 *               dimensionality as the index (config_.dimension).
 * @param id     Unique identifier for the vector to update.
 *
 * @return IndexOpResult
 * - VectorOpResult::SUCCEED on success.
 * - VectorOpResult::INDEX_NOT_EXIST if the index has not been initialized.
 * - VectorOpResult::VECTOR_DIMENSION_MISMATCH if `vector.size()` differs from
 *   the index dimension.
 * - VectorOpResult::INDEX_INTERNAL_ERROR if the underlying usearch add
 *   operation fails or an exception is thrown; the result message contains
 *   the underlying error text.
 */
IndexOpResult HNSWVectorIndex::update(const std::vector<float> &vector,
                                      uint64_t id)
{
    std::shared_lock<std::shared_mutex> lock(index_mutex_);

    if (!initialized_)
    {
        return IndexOpResult(VectorOpResult::INDEX_NOT_EXIST,
                             "Index not initialized");
    }

    if (vector.size() != config_.dimension)
    {
        return IndexOpResult(VectorOpResult::VECTOR_DIMENSION_MISMATCH,
                             "Vector dimension mismatch");
    }

    try
    {
        // For update, we need to remove the old vector and add the new one
        // First, try to remove the existing vector (ignore if it doesn't exist)
        auto remove_result = usearch_index_.remove(id);
        // Don't check remove_result.error as the vector might not exist

        // Add the new vector
        auto add_result = usearch_index_.add(id, vector.data(), 0, true);
        if (add_result.error)
        {
            return IndexOpResult(VectorOpResult::INDEX_INTERNAL_ERROR,
                                 add_result.error.what());
        }

        return IndexOpResult(VectorOpResult::SUCCEED, "");
    }
    catch (const std::exception &e)
    {
        return IndexOpResult(VectorOpResult::INDEX_INTERNAL_ERROR, e.what());
    }
}

size_t HNSWVectorIndex::memory_usage()
{
    std::shared_lock<std::shared_mutex> lock(index_mutex_);
    if (!initialized_)
    {
        return 0;
    }
    return usearch_index_.memory_usage();
}

bool HNSWVectorIndex::is_ready()
{
    std::shared_lock<std::shared_mutex> lock(index_mutex_);
    return initialized_;
}

size_t HNSWVectorIndex::get_dimension()
{
    std::shared_lock<std::shared_mutex> lock(index_mutex_);
    if (!initialized_)
    {
        return 0;
    }
    return config_.dimension;
}

size_t HNSWVectorIndex::size()
{
    std::shared_lock<std::shared_mutex> lock(index_mutex_);
    if (!initialized_)
    {
        return 0;
    }
    return usearch_index_.size();
}

bool HNSWVectorIndex::optimize()
{
    std::shared_lock<std::shared_mutex> lock(index_mutex_);

    if (!initialized_)
    {
        return false;
    }

    // TODO: Implement optimization functionality
    return true;
}

std::string HNSWVectorIndex::get_type() const
{
    return "HNSW";
}

bool HNSWVectorIndex::set_search_params(
    std::unordered_map<std::string, std::string> params)
{
    std::lock_guard<std::shared_mutex> lock(index_mutex_);
    if (params.find("ef_search") == params.end())
    {
        return false;
    }

    size_t ef_search = std::stoul(params["ef_search"]);
    usearch_index_.change_expansion_search(ef_search);

    return true;
}

bool HNSWVectorIndex::set_update_params(
    std::unordered_map<std::string, std::string> params)
{
    std::lock_guard<std::shared_mutex> lock(index_mutex_);
    if (params.find("ef_construction") == params.end())
    {
        return false;
    }
    size_t ef_construction = std::stoul(params["ef_construction"]);
    usearch_index_.change_expansion_add(ef_construction);
    return true;
}

std::unique_ptr<VectorIndex> create_hnsw_vector_index()
{
    return std::make_unique<HNSWVectorIndex>();
}

}  // namespace EloqVec
