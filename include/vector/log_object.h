/**
 * @file log_object.h
 * @brief LogObject class for buffered delta operations in vector indices
 *
 * This file defines the LogObject class that provides static methods
 * for managing log objects used for buffered delta operations in vector
 * indices. Each log object consists of metadata and individual log items stored
 * as separate keys in the storage system.
 *
 * @author EloqData Inc.
 * @date 2025
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Forward declaration
namespace txservice
{
class TransactionExecution;
}

namespace EloqVec
{

/**
 * @brief Operation types for log items
 */
enum class LogOperationType : uint8_t
{
    INSERT = 0,
    UPDATE = 1,
    DELETE = 2
};

/**
 * @brief Structure representing a single log item
 */
struct log_item_t
{
    LogOperationType operation_type;
    std::string key;
    std::string value;
    uint64_t timestamp;
    uint64_t sequence_id;

    log_item_t() = default;
    log_item_t(LogOperationType op,
               const std::string &k,
               const std::string &val,
               uint64_t ts,
               uint64_t seq_id)
        : operation_type(op),
          key(k),
          value(val),
          timestamp(ts),
          sequence_id(seq_id)
    {
    }
};

/**
 * @brief Structure representing log metadata
 */
struct log_metadata_t
{
    uint64_t total_items;
    uint64_t head_item_sequence_id;
    uint64_t tail_item_sequence_id;
    uint64_t next_id;

    log_metadata_t()
        : total_items(0),
          head_item_sequence_id(0),
          tail_item_sequence_id(0),
          next_id(1)
    {
    }
};

/**
 * @brief Error codes for log operations
 */
enum class LogError : int
{
    SUCCESS = 0,
    LOG_NOT_FOUND = -1,
    LOG_ALREADY_EXISTS = -2,
    INVALID_SEQUENCE_ID = -3,
    SERIALIZATION_ERROR = -4,
    DESERIALIZATION_ERROR = -5,
    STORAGE_ERROR = -6,
    INVALID_PARAMETER = -7
};

/**
 * @brief LogObject class for managing buffered delta operations
 *
 * This class provides static methods for creating, managing, and removing
 * log objects used for buffered delta operations in vector indices.
 * Each log object consists of metadata and individual log items stored
 * as separate keys in the storage system.
 */
class LogObject
{
public:
    /**
     * @brief Create a new log object
     *
     * Creates a new log object with initialized metadata. The log object
     * will be ready to accept log items.
     *
     * @param log_name Name of the log object to create
     * @param txm Transaction execution context (required, non-null)
     * @return LogError::SUCCESS if creation successful, error code otherwise
     */
    static LogError create_log(const std::string &log_name,
                               txservice::TransactionExecution *txm);

    /**
     * @brief Remove a log object and all its data
     *
     * Removes the entire log object including metadata and all associated
     * log items. This operation is irreversible.
     *
     * @param log_name Name of the log object to remove
     * @param txm Transaction execution context (required, non-null)
     * @return LogError::SUCCESS if removal successful, error code otherwise
     */
    static LogError remove_log(const std::string &log_name,
                               txservice::TransactionExecution *txm);

    /**
     * @brief Check if a log object exists
     *
     * @param log_name Name of the log object to check
     * @param txm Transaction execution context (required, non-null)
     * @return true if log object exists, false otherwise
     */
    static bool exists(const std::string &log_name,
                       txservice::TransactionExecution *txm);

    /**
     * @brief Append new log items to the tail
     *
     * Adds new log items to the end of the log. The sequence_id in the
     * item will be automatically assigned if not set.
     *
     * @param log_name Name of the log object
     * @param items Log items to append
     * @param log_id Log ID of the last log appended
     * @param log_count Number of log items in the log object after appending
     * @param txm Transaction execution context (required, non-null)
     * @return LogError::SUCCESS if append successful, error code otherwise
     */
    static LogError append_log(const std::string &log_name,
                               std::vector<log_item_t> &items,
                               uint64_t &log_id,
                               uint64_t &log_count,
                               txservice::TransactionExecution *txm);

    /**
     * @brief Truncate logs up to specified ID
     *
     * Removes all log items with sequence_id <= to_id. This operation
     * is used for log cleanup and maintenance.
     *
     * @param log_name Name of the log object
     * @param to_id Sequence ID up to which to truncate (inclusive)
     * @param log_count Number of log items in the log object after truncation
     * @param txm Transaction execution context (required, non-null)
     * @return LogError::SUCCESS if truncation successful, error code otherwise
     */
    static LogError truncate_log(const std::string &log_name,
                                 uint64_t to_id,
                                 uint64_t &log_count,
                                 txservice::TransactionExecution *txm);

    /**
     * @brief Scan logs from oldest to specified ID
     *
     * Retrieves all log items with sequence_id <= to_id, ordered by
     * sequence_id from oldest to newest.
     *
     * @param log_name Name of the log object
     * @param to_id Maximum sequence ID to include (inclusive)
     * @param items Output vector to store retrieved log items
     * @param txm Transaction execution context (required, non-null)
     * @return LogError::SUCCESS if scan successful, error code otherwise
     */
    static LogError scan_log(const std::string &log_name,
                             uint64_t to_id,
                             std::vector<log_item_t> &items,
                             txservice::TransactionExecution *txm);

    /**
     * @brief Get log statistics
     *
     * Retrieves statistics about the log object including total items,
     * sequence ID range, and total size.
     *
     * @param log_name Name of the log object
     * @param txm Transaction execution context (required, non-null)
     * @return log_metadata_t containing log metadata
     */
    static log_metadata_t get_stats(const std::string &log_name,
                                    txservice::TransactionExecution *txm);

private:
    /**
     * @brief Serialize metadata to string
     *
     * @param meta Metadata to serialize
     * @param result Serialized metadata string
     */
    static void serialize_metadata(const log_metadata_t &meta,
                                   std::string &result);

    /**
     * @brief Deserialize metadata from string
     *
     * @param data Serialized metadata string
     * @param data_size Size of the serialized metadata
     * @return Deserialized metadata
     */
    static log_metadata_t deserialize_metadata(const char *data,
                                               size_t data_size);

    /**
     * @brief Serialize log item to string
     *
     * @param item Log item to serialize
     * @param result Serialized log item string
     */
    static void serialize_log_item(const log_item_t &item, std::string &result);

    /**
     * @brief Deserialize log item from string
     *
     * @param data Serialized log item string
     * @param data_size Size of the serialized log item
     * @return Deserialized log item
     */
    static log_item_t deserialize_log_item(const char *data, size_t data_size);

    /**
     * @brief Get metadata key for a log
     *
     * @param log_name Name of the log object
     * @return Metadata key string
     */
    static std::string get_metadata_key(const std::string &log_name);

    /**
     * @brief Get log item key for a specific sequence ID
     *
     * @param log_name Name of the log object
     * @param sequence_id Sequence ID of the log item
     * @return Log item key string
     */
    static std::string get_log_item_key(const std::string &log_name,
                                        uint64_t sequence_id);
};

}  // namespace EloqVec
