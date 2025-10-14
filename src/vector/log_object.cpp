/**
 * @file log_object.cpp
 * @brief Implementation of LogObject class for buffered delta operations
 *
 * This file implements the LogObject class that provides static methods
 * for managing log objects used for buffered delta operations in vector
 * indices.
 *
 * @author EloqData Inc.
 * @date 2025
 */

#include "vector/log_object.h"

#include <algorithm>
#include <cstring>

#include "eloq_string_key_record.h"
#include "tx_execution.h"
#include "tx_request.h"
#include "vector/vector_type.h"

namespace EloqVec
{

// Helper functions that don't need transaction execution context

void LogObject::serialize_metadata(const log_metadata_t &meta,
                                   std::string &result)
{
    // Reserve space for fixed-size data (4 uint64_t fields)
    result.reserve(sizeof(uint64_t) * 4 + result.size());

    // Serialize total_items (uint64_t)
    result.append(reinterpret_cast<const char *>(&meta.total_items),
                  sizeof(uint64_t));

    // Serialize head_item_sequence_id (uint64_t)
    result.append(reinterpret_cast<const char *>(&meta.head_item_sequence_id),
                  sizeof(uint64_t));

    // Serialize tail_item_sequence_id (uint64_t)
    result.append(reinterpret_cast<const char *>(&meta.tail_item_sequence_id),
                  sizeof(uint64_t));

    // Serialize next_id (uint64_t)
    result.append(reinterpret_cast<const char *>(&meta.next_id),
                  sizeof(uint64_t));

    return;
}

log_metadata_t LogObject::deserialize_metadata(const char *data,
                                               size_t data_size)
{
    log_metadata_t meta;
    size_t offset = 0;

    // Deserialize total_items (uint64_t)
    if (offset + sizeof(uint64_t) <= data_size)
    {
        std::memcpy(&meta.total_items, data + offset, sizeof(uint64_t));
        offset += sizeof(uint64_t);
    }

    // Deserialize head_item_sequence_id (uint64_t)
    if (offset + sizeof(uint64_t) <= data_size)
    {
        std::memcpy(
            &meta.head_item_sequence_id, data + offset, sizeof(uint64_t));
        offset += sizeof(uint64_t);
    }

    // Deserialize tail_item_sequence_id (uint64_t)
    if (offset + sizeof(uint64_t) <= data_size)
    {
        std::memcpy(
            &meta.tail_item_sequence_id, data + offset, sizeof(uint64_t));
        offset += sizeof(uint64_t);
    }

    // Deserialize next_id (uint64_t)
    if (offset + sizeof(uint64_t) <= data_size)
    {
        std::memcpy(&meta.next_id, data + offset, sizeof(uint64_t));
    }

    return meta;
}

void LogObject::serialize_log_item(const log_item_t &item, std::string &result)
{
    // Reserve space for fixed-size data + estimated string sizes
    result.reserve(sizeof(uint8_t) + sizeof(uint64_t) * 2 + item.key.size() +
                   item.value.size() + 16 + result.size());

    // Serialize operation_type (uint8_t)
    uint8_t op_type = static_cast<uint8_t>(item.operation_type);
    result.append(reinterpret_cast<const char *>(&op_type), sizeof(uint8_t));

    // Serialize key (string): length + content
    uint64_t key_len = item.key.size();
    result.append(reinterpret_cast<const char *>(&key_len), sizeof(uint64_t));
    result.append(item.key);

    // Serialize value (string): length + content
    uint64_t value_len = item.value.size();
    result.append(reinterpret_cast<const char *>(&value_len), sizeof(uint64_t));
    result.append(item.value);

    // Serialize timestamp (uint64_t)
    result.append(reinterpret_cast<const char *>(&item.timestamp),
                  sizeof(uint64_t));

    // Serialize sequence_id (uint64_t)
    result.append(reinterpret_cast<const char *>(&item.sequence_id),
                  sizeof(uint64_t));

    return;
}

log_item_t LogObject::deserialize_log_item(const char *data, size_t data_size)
{
    log_item_t item;
    size_t offset = 0;

    // Deserialize operation_type (uint8_t)
    if (offset + sizeof(uint8_t) <= data_size)
    {
        uint8_t op_type;
        std::memcpy(&op_type, data + offset, sizeof(uint8_t));
        item.operation_type = static_cast<LogOperationType>(op_type);
        offset += sizeof(uint8_t);
    }

    // Deserialize key (string): length + content
    if (offset + sizeof(uint64_t) <= data_size)
    {
        uint64_t key_len;
        std::memcpy(&key_len, data + offset, sizeof(uint64_t));
        offset += sizeof(uint64_t);

        if (offset + key_len <= data_size)
        {
            item.key.assign(data + offset, key_len);
            offset += key_len;
        }
    }

    // Deserialize value (string): length + content
    if (offset + sizeof(uint64_t) <= data_size)
    {
        uint64_t value_len;
        std::memcpy(&value_len, data + offset, sizeof(uint64_t));
        offset += sizeof(uint64_t);

        if (offset + value_len <= data_size)
        {
            item.value.assign(data + offset, value_len);
            offset += value_len;
        }
    }

    // Deserialize timestamp (uint64_t)
    if (offset + sizeof(uint64_t) <= data_size)
    {
        std::memcpy(&item.timestamp, data + offset, sizeof(uint64_t));
        offset += sizeof(uint64_t);
    }

    // Deserialize sequence_id (uint64_t)
    if (offset + sizeof(uint64_t) <= data_size)
    {
        std::memcpy(&item.sequence_id, data + offset, sizeof(uint64_t));
    }

    return item;
}

std::string LogObject::get_metadata_key(const std::string &log_name)
{
    return "log:meta:" + log_name;
}

std::string LogObject::get_log_item_key(const std::string &log_name,
                                        uint64_t sequence_id)
{
    return "log:item:" + log_name + ":" + std::to_string(sequence_id);
}

// ===== SHARDED LOG HELPER FUNCTIONS =====

uint32_t LogObject::get_shard_id(const std::string &shard_key,
                                 uint32_t num_shards)
{
    if (num_shards == 0)
    {
        return 0;  // Avoid division by zero
    }

    // FNV-1a hash implementation for stable 64-bit hashing
    const uint64_t fnv_offset_basis = 14695981039346656037ULL;
    const uint64_t fnv_prime = 1099511628211ULL;

    uint64_t fnv64_hash = fnv_offset_basis;
    for (char c : shard_key)
    {
        fnv64_hash ^= static_cast<uint8_t>(c);
        fnv64_hash *= fnv_prime;
    }

    return static_cast<uint32_t>(fnv64_hash %
                                 static_cast<uint64_t>(num_shards));
}

std::string LogObject::get_shard_log_name(const std::string &base_log_name,
                                          uint32_t shard_id)
{
    return base_log_name + ":shard_" + std::to_string(shard_id);
}

/**
 * @brief Creates a new log metadata record for the named log.
 *
 * Ensures a transaction execution context is provided, verifies the log does
 * not already exist, and inserts an initial zeroed metadata record into
 * storage.
 *
 * @param log_name Name of the log to create.
 * @note The transaction execution pointer parameter is a service/client and is
 * not documented here.
 *
 * @return LogError::INVALID_PARAMETER if the transaction execution context is
 * null.
 * @return LogError::LOG_ALREADY_EXISTS if metadata for the given log_name
 * already exists.
 * @return LogError::STORAGE_ERROR on underlying storage/read/upsert failures.
 * @return LogError::SUCCESS on successful creation.
 */

LogError LogObject::create_log(const std::string &log_name,
                               txservice::TransactionExecution *txm)
{
    // Require non-null transaction execution context
    if (txm == nullptr)
    {
        return LogError::INVALID_PARAMETER;
    }

    // Create key for metadata
    std::string meta_key = get_metadata_key(log_name);
    txservice::TxKey tx_key =
        txservice::EloqStringKey::Create(meta_key.c_str(), meta_key.size());

    // Create record to read into
    txservice::EloqStringRecord record;

    // Create and execute read request
    txservice::ReadTxRequest read_req(&vector_index_meta_table,
                                      0,
                                      &tx_key,
                                      &record,
                                      true,
                                      false,
                                      false,
                                      0,
                                      false,
                                      false,
                                      false,
                                      nullptr,
                                      nullptr,
                                      nullptr);
    txm->Execute(&read_req);
    read_req.Wait();

    // Check errors and record status
    if (read_req.ErrorCode() != txservice::TxErrorCode::NO_ERROR)
    {
        return LogError::STORAGE_ERROR;  // Handle error
    }
    txservice::RecordStatus rec_status = read_req.Result().first;
    if (rec_status == txservice::RecordStatus::Normal)
    {
        return LogError::LOG_ALREADY_EXISTS;
    }

    // Create initial metadata
    log_metadata_t meta;
    meta.total_items = 0;
    meta.head_item_sequence_id = 0;
    meta.tail_item_sequence_id = 0;
    meta.next_id = 0;

    // Write metadata to storage
    // Determine operation type

    // Serialize metadata
    std::string serialized_meta;
    serialize_metadata(meta, serialized_meta);

    // Create record with serialized metadata
    auto record_ptr = std::make_unique<txservice::EloqStringRecord>();
    record_ptr->SetEncodedBlob(
        reinterpret_cast<const unsigned char *>(serialized_meta.c_str()),
        serialized_meta.size());

    // Create and execute upsert request
    txservice::UpsertTxRequest upsert_req(&vector_index_meta_table,
                                          std::move(tx_key),
                                          std::move(record_ptr),
                                          txservice::OperationType::Insert,
                                          nullptr,
                                          nullptr);
    txm->Execute(&upsert_req);
    upsert_req.Wait();

    // Check for errors
    if (upsert_req.ErrorCode() != txservice::TxErrorCode::NO_ERROR)
    {
        LOG(ERROR) << "Upsert request for log object " << log_name
                   << " failed with error "
                   << static_cast<int>(upsert_req.ErrorCode());
        return LogError::STORAGE_ERROR;
    }

    return LogError::SUCCESS;
}

/**
 * @brief Checks whether a log with the given name exists in storage.
 *
 * Queries the metadata record for the named log and returns true if the record
 * is present with status `Normal`. Returns false if the record is missing or
 * if a storage/read error occurs.
 *
 * @param log_name Name of the log to check.
 * @return true if the log metadata exists and is `Normal`; false otherwise.
 */
bool LogObject::exists(const std::string &log_name,
                       txservice::TransactionExecution *txm)
{
    // Create key for metadata
    std::string meta_key = get_metadata_key(log_name);
    txservice::EloqStringKey key_obj(meta_key.data(), meta_key.size());
    txservice::TxKey tx_key(&key_obj);

    // Create record to read into
    txservice::EloqStringRecord record;

    // Create and execute read request
    txservice::ReadTxRequest read_req(&vector_index_meta_table,
                                      0,
                                      &tx_key,
                                      &record,
                                      false,
                                      false,
                                      false,
                                      0,
                                      false,
                                      false,
                                      false,
                                      nullptr,
                                      nullptr,
                                      nullptr);
    txm->Execute(&read_req);
    read_req.Wait();

    // Check errors and record status
    if (read_req.ErrorCode() != txservice::TxErrorCode::NO_ERROR)
    {
        return false;  // Handle error
    }
    auto result = read_req.Result();
    return result.first == txservice::RecordStatus::Normal;
}

/**
 * @brief Appends one or more log items to the named log.
 *
 * Assigns consecutive sequence IDs to the provided items (starting at the log's
 * current next_id), persists each item and updates the log metadata
 * (total_items, next_id, tail_item_sequence_id). Requires an active transaction
 * execution context.
 *
 * @param log_name Name of the log to append to.
 * @param items Vector of log items to append; each item's sequence_id will be
 * set in-place.
 * @param[out] log_id Set to the sequence_id of the last appended item on
 * success.
 * @param[out] log_count Set to the updated total number of items in the log on
 * success.
 *
 * @return LogError::INVALID_PARAMETER if the transaction execution pointer is
 * null.
 * @return LogError::LOG_NOT_FOUND if the log metadata does not exist.
 * @return LogError::STORAGE_ERROR on any storage read/write failure.
 * @return LogError::SUCCESS if items were appended (or if the input vector is
 * empty).
 */
LogError LogObject::append_log(const std::string &log_name,
                               std::vector<log_item_t> &items,
                               uint64_t &log_id,
                               uint64_t &log_count,
                               txservice::TransactionExecution *txm)
{
    assert(!items.empty());
    // Require non-null transaction execution context
    if (txm == nullptr)
    {
        return LogError::INVALID_PARAMETER;
    }

    if (items.empty())
    {
        return LogError::SUCCESS;  // Nothing to append
    }

    // Read current metadata to get next sequence ID
    std::string meta_key = get_metadata_key(log_name);
    txservice::TxKey meta_tx_key =
        txservice::EloqStringKey::Create(meta_key.c_str(), meta_key.size());

    txservice::EloqStringRecord meta_record;
    txservice::ReadTxRequest meta_read_req(&vector_index_meta_table,
                                           0,
                                           &meta_tx_key,
                                           &meta_record,
                                           true,
                                           false,
                                           false,
                                           0,
                                           false,
                                           false,
                                           false,
                                           nullptr,
                                           nullptr,
                                           nullptr);
    txm->Execute(&meta_read_req);
    meta_read_req.Wait();

    if (meta_read_req.ErrorCode() != txservice::TxErrorCode::NO_ERROR)
    {
        return LogError::STORAGE_ERROR;
    }

    auto meta_result = meta_read_req.Result();
    if (meta_result.first != txservice::RecordStatus::Normal)
    {
        return LogError::LOG_NOT_FOUND;
    }

    // Deserialize metadata
    const char *meta_data = meta_record.EncodedBlobData();
    size_t meta_data_size = meta_record.EncodedBlobSize();
    log_metadata_t meta = deserialize_metadata(meta_data, meta_data_size);

    // Assign sequence IDs to items
    uint64_t current_seq_id = meta.next_id;
    for (auto &item : items)
    {
        item.sequence_id = current_seq_id++;
    }
    // No need to acqurie write intent on log items since we've already put wi
    // on meta data key, which means there won't be any concurrent write to the
    // same log.
    for (size_t i = 0; i < items.size(); ++i)
    {
        // Serialize log item
        std::string serialized_item;
        serialize_log_item(items[i], serialized_item);

        // Create record with serialized log item
        auto item_record_ptr = std::make_unique<txservice::EloqStringRecord>();
        item_record_ptr->SetEncodedBlob(
            reinterpret_cast<const unsigned char *>(serialized_item.c_str()),
            serialized_item.size());

        // Create and execute upsert request
        std::string item_key = get_log_item_key(log_name, items[i].sequence_id);
        txservice::TxKey item_tx_key =
            txservice::EloqStringKey::Create(item_key.c_str(), item_key.size());
        // TODO(liunyl): impl batch upsert request
        txservice::UpsertTxRequest item_upsert_req(
            &vector_index_meta_table,
            std::move(item_tx_key),
            std::move(item_record_ptr),
            txservice::OperationType::Upsert,
            nullptr,
            nullptr);
        txm->Execute(&item_upsert_req);
        item_upsert_req.Wait();

        if (item_upsert_req.ErrorCode() != txservice::TxErrorCode::NO_ERROR)
        {
            return LogError::STORAGE_ERROR;
        }
    }

    // Update metadata
    meta.total_items += items.size();
    meta.next_id = current_seq_id;

    // Update head and tail sequence IDs
    assert(items.back().sequence_id == current_seq_id - 1);
    meta.tail_item_sequence_id = items.back().sequence_id;

    // Write updated metadata
    std::string serialized_meta;
    serialize_metadata(meta, serialized_meta);

    auto meta_record_ptr = std::make_unique<txservice::EloqStringRecord>();
    meta_record_ptr->SetEncodedBlob(
        reinterpret_cast<const unsigned char *>(serialized_meta.c_str()),
        serialized_meta.size());

    txservice::UpsertTxRequest meta_upsert_req(&vector_index_meta_table,
                                               std::move(meta_tx_key),
                                               std::move(meta_record_ptr),
                                               txservice::OperationType::Update,
                                               nullptr,
                                               nullptr);
    txm->Execute(&meta_upsert_req);
    meta_upsert_req.Wait();

    if (meta_upsert_req.ErrorCode() != txservice::TxErrorCode::NO_ERROR)
    {
        return LogError::STORAGE_ERROR;
    }

    log_id = meta.next_id - 1;
    log_count = meta.total_items;

    return LogError::SUCCESS;
}

/**
 * @brief Removes a log and all its stored items.
 *
 * Reads the log's metadata to determine the stored sequence ID range, deletes
 * every log item with sequence IDs from head_item_sequence_id through
 * tail_item_sequence_id (inclusive), then deletes the metadata record.
 *
 * @param log_name Logical name/identifier of the log to remove.
 * @return LogError::INVALID_PARAMETER if the transaction execution context is
 * null.
 * @return LogError::LOG_NOT_FOUND if the log metadata does not exist.
 * @return LogError::STORAGE_ERROR on any storage/read/write/delete failure.
 * @return LogError::SUCCESS if the log and all its items were removed
 * successfully.
 */
LogError LogObject::remove_log(const std::string &log_name,
                               txservice::TransactionExecution *txm)
{
    // Require non-null transaction execution context
    if (txm == nullptr)
    {
        return LogError::INVALID_PARAMETER;
    }

    // Read metadata to get sequence ID range
    std::string meta_key = get_metadata_key(log_name);
    txservice::TxKey meta_tx_key =
        txservice::EloqStringKey::Create(meta_key.c_str(), meta_key.size());

    txservice::EloqStringRecord meta_record;
    txservice::ReadTxRequest meta_read_req(&vector_index_meta_table,
                                           0,
                                           &meta_tx_key,
                                           &meta_record,
                                           true,
                                           false,
                                           false,
                                           0,
                                           false,
                                           false,
                                           false,
                                           nullptr,
                                           nullptr,
                                           nullptr);
    txm->Execute(&meta_read_req);
    meta_read_req.Wait();

    if (meta_read_req.ErrorCode() != txservice::TxErrorCode::NO_ERROR)
    {
        return LogError::STORAGE_ERROR;
    }

    auto meta_result = meta_read_req.Result();
    if (meta_result.first != txservice::RecordStatus::Normal)
    {
        return LogError::LOG_NOT_FOUND;
    }

    // Deserialize metadata
    const char *meta_data = meta_record.EncodedBlobData();
    size_t meta_data_size = meta_record.EncodedBlobSize();
    log_metadata_t meta = deserialize_metadata(meta_data, meta_data_size);

    // Delete all log items in the range
    for (uint64_t seq_id = meta.head_item_sequence_id;
         seq_id <= meta.tail_item_sequence_id;
         ++seq_id)
    {
        std::string item_key = get_log_item_key(log_name, seq_id);
        txservice::TxKey item_tx_key =
            txservice::EloqStringKey::Create(item_key.c_str(), item_key.size());

        auto item_record_ptr = std::make_unique<txservice::EloqStringRecord>();
        txservice::UpsertTxRequest item_delete_req(
            &vector_index_meta_table,
            std::move(item_tx_key),
            std::move(item_record_ptr),
            txservice::OperationType::Delete,
            nullptr,
            nullptr);
        txm->Execute(&item_delete_req);
        item_delete_req.Wait();

        if (item_delete_req.ErrorCode() != txservice::TxErrorCode::NO_ERROR)
        {
            return LogError::STORAGE_ERROR;
        }
    }

    // Delete metadata
    auto meta_record_ptr = std::make_unique<txservice::EloqStringRecord>();
    txservice::UpsertTxRequest meta_delete_req(&vector_index_meta_table,
                                               std::move(meta_tx_key),
                                               std::move(meta_record_ptr),
                                               txservice::OperationType::Delete,
                                               nullptr,
                                               nullptr);
    txm->Execute(&meta_delete_req);
    meta_delete_req.Wait();

    if (meta_delete_req.ErrorCode() != txservice::TxErrorCode::NO_ERROR)
    {
        return LogError::STORAGE_ERROR;
    }

    return LogError::SUCCESS;
}

/**
 * @brief Truncates a log by deleting log items from the head up to a target
 * sequence id.
 *
 * Deletes every log item with sequence id in [head_item_sequence_id, to_id]
 * (inclusive), updates the log's metadata (head_item_sequence_id and
 * total_items), and returns the updated total item count via log_count.
 *
 * @param to_id Target sequence id to truncate up to (inclusive). If greater
 * than the current tail sequence id it will be clamped to the tail. If less
 * than the current head sequence id the call fails with INVALID_PARAMETER.
 * If set to UINT64_MAX, the log will be truncated up to the current tail and
 * updated to_id will be set to the current tail sequence id.
 * @param log_count Out parameter set to the remaining total_items after
 * truncation.
 *
 * Note: txm (transaction execution) must be non-null.
 *
 * @return LogError::SUCCESS on success.
 * @return LogError::INVALID_PARAMETER if txm is null or to_id <
 * head_item_sequence_id.
 * @return LogError::LOG_NOT_FOUND if the named log does not exist.
 * @return LogError::STORAGE_ERROR on underlying storage read/write/delete
 * failures.
 */
LogError LogObject::truncate_log(const std::string &log_name,
                                 uint64_t &to_id,
                                 uint64_t &log_count,
                                 txservice::TransactionExecution *txm)
{
    // Require non-null transaction execution context
    if (txm == nullptr)
    {
        return LogError::INVALID_PARAMETER;
    }

    // Read metadata to get sequence ID range
    std::string meta_key = get_metadata_key(log_name);
    txservice::TxKey meta_tx_key =
        txservice::EloqStringKey::Create(meta_key.c_str(), meta_key.size());

    txservice::EloqStringRecord meta_record;
    txservice::ReadTxRequest meta_read_req(&vector_index_meta_table,
                                           0,
                                           &meta_tx_key,
                                           &meta_record,
                                           true,
                                           false,
                                           false,
                                           0,
                                           false,
                                           false,
                                           false,
                                           nullptr,
                                           nullptr,
                                           nullptr);
    txm->Execute(&meta_read_req);
    meta_read_req.Wait();

    if (meta_read_req.ErrorCode() != txservice::TxErrorCode::NO_ERROR)
    {
        return LogError::STORAGE_ERROR;
    }

    auto meta_result = meta_read_req.Result();
    if (meta_result.first != txservice::RecordStatus::Normal)
    {
        return LogError::LOG_NOT_FOUND;
    }

    // Deserialize metadata
    const char *meta_data = meta_record.EncodedBlobData();
    size_t meta_data_size = meta_record.EncodedBlobSize();
    log_metadata_t meta = deserialize_metadata(meta_data, meta_data_size);

    if (to_id > meta.tail_item_sequence_id)
    {
        to_id = meta.tail_item_sequence_id;
    }

    if (meta.total_items == 0)
    {
        log_count = 0;
        return LogError::SUCCESS;
    }

    if (to_id < meta.head_item_sequence_id)
    {
        return LogError::INVALID_PARAMETER;
    }

    // Delete all log items in the range
    for (uint64_t seq_id = meta.head_item_sequence_id; seq_id <= to_id;
         ++seq_id)
    {
        std::string item_key = get_log_item_key(log_name, seq_id);
        txservice::TxKey item_tx_key =
            txservice::EloqStringKey::Create(item_key.c_str(), item_key.size());

        auto item_record_ptr = std::make_unique<txservice::EloqStringRecord>();
        txservice::UpsertTxRequest item_delete_req(
            &vector_index_meta_table,
            std::move(item_tx_key),
            std::move(item_record_ptr),
            txservice::OperationType::Delete,
            nullptr,
            nullptr);
        txm->Execute(&item_delete_req);
        item_delete_req.Wait();

        if (item_delete_req.ErrorCode() != txservice::TxErrorCode::NO_ERROR)
        {
            return LogError::STORAGE_ERROR;
        }
    }

    // Update metadata
    meta.total_items -= to_id - meta.head_item_sequence_id + 1;
    meta.head_item_sequence_id = to_id + 1;

    std::string serialized_meta;
    serialize_metadata(meta, serialized_meta);

    // Delete metadata
    auto meta_record_ptr = std::make_unique<txservice::EloqStringRecord>();
    meta_record_ptr->SetEncodedBlob(
        reinterpret_cast<const unsigned char *>(serialized_meta.c_str()),
        serialized_meta.size());
    txservice::UpsertTxRequest meta_upsert_req(&vector_index_meta_table,
                                               std::move(meta_tx_key),
                                               std::move(meta_record_ptr),
                                               txservice::OperationType::Update,
                                               nullptr,
                                               nullptr);
    txm->Execute(&meta_upsert_req);
    meta_upsert_req.Wait();

    if (meta_upsert_req.ErrorCode() != txservice::TxErrorCode::NO_ERROR)
    {
        return LogError::STORAGE_ERROR;
    }

    log_count = meta.total_items;
    if (to_id == UINT64_MAX)
    {
        to_id = meta.tail_item_sequence_id;
    }

    return LogError::SUCCESS;
}

/**
 * @brief Reads log entries from the head up to a specified sequence ID into a
 * vector.
 *
 * Reads the log's metadata to determine the valid sequence range, then
 * batch-reads each log item from the current head sequence ID through `to_id`
 * (inclusive), deserializes them, and appends them to `items`.
 *
 * @param to_id Upper bound sequence ID (inclusive). If greater than the log's
 * tail sequence ID it is clamped to the tail. Must be >= head sequence ID.
 * @param items Output vector that will be populated with the deserialized log
 * items in ascending sequence order.
 *
 * The transaction execution context (`txm`) must be provided and is used for
 * all storage reads; it is intentionally omitted from @param documentation as a
 * service.
 *
 * @return LogError::SUCCESS on success.
 * @return LogError::INVALID_PARAMETER if `txm` is null or `to_id` is less than
 * the log's head sequence ID.
 * @return LogError::LOG_NOT_FOUND if the named log does not exist.
 * @return LogError::STORAGE_ERROR on storage/read failures.
 */
LogError LogObject::scan_log(const std::string &log_name,
                             uint64_t to_id,
                             std::vector<log_item_t> &items,
                             txservice::TransactionExecution *txm)
{
    // Require non-null transaction execution context
    if (txm == nullptr)
    {
        return LogError::INVALID_PARAMETER;
    }

    // Read metadata to get sequence ID range
    std::string meta_key = get_metadata_key(log_name);
    txservice::EloqStringKey meta_key_obj(meta_key.data(), meta_key.size());
    txservice::TxKey meta_tx_key(&meta_key_obj);

    txservice::EloqStringRecord meta_record;
    txservice::ReadTxRequest meta_read_req(&vector_index_meta_table,
                                           0,
                                           &meta_tx_key,
                                           &meta_record,
                                           false,
                                           false,
                                           false,
                                           0,
                                           false,
                                           false,
                                           false,
                                           nullptr,
                                           nullptr,
                                           nullptr);
    txm->Execute(&meta_read_req);
    meta_read_req.Wait();

    if (meta_read_req.ErrorCode() != txservice::TxErrorCode::NO_ERROR)
    {
        return LogError::STORAGE_ERROR;
    }

    auto meta_result = meta_read_req.Result();
    if (meta_result.first != txservice::RecordStatus::Normal)
    {
        return LogError::LOG_NOT_FOUND;
    }

    // Deserialize metadata
    const char *meta_data = meta_record.EncodedBlobData();
    size_t meta_data_size = meta_record.EncodedBlobSize();
    log_metadata_t meta = deserialize_metadata(meta_data, meta_data_size);

    if (meta.total_items == 0)
    {
        return LogError::SUCCESS;
    }

    if (to_id < meta.head_item_sequence_id)
    {
        return LogError::INVALID_PARAMETER;
    }

    if (to_id > meta.tail_item_sequence_id)
    {
        to_id = meta.tail_item_sequence_id;
    }

    // Scan log items in the range by batch read
    std::vector<txservice::ScanBatchTuple> scan_batch_tuples;
    std::vector<txservice::EloqStringRecord> scan_batch_records;
    size_t item_count = to_id - meta.head_item_sequence_id + 1;
    scan_batch_tuples.reserve(item_count);
    scan_batch_records.reserve(item_count);
    for (uint64_t seq_id = meta.head_item_sequence_id; seq_id <= to_id;
         seq_id++)
    {
        std::string item_key = get_log_item_key(log_name, seq_id);
        txservice::TxKey item_tx_key =
            txservice::EloqStringKey::Create(item_key.c_str(), item_key.size());
        scan_batch_records.emplace_back();
        scan_batch_tuples.emplace_back(std::move(item_tx_key),
                                       &scan_batch_records.back());
    }

    txservice::BatchReadTxRequest batch_read_req(&vector_index_meta_table,
                                                 0,
                                                 scan_batch_tuples,
                                                 false,
                                                 false,
                                                 false,
                                                 nullptr,
                                                 nullptr,
                                                 nullptr);
    txm->Execute(&batch_read_req);
    batch_read_req.Wait();

    if (batch_read_req.ErrorCode() != txservice::TxErrorCode::NO_ERROR)
    {
        return LogError::STORAGE_ERROR;
    }

    // Deserialize log items
    for (size_t i = 0; i < scan_batch_tuples.size(); ++i)
    {
        assert(scan_batch_tuples[i].status_ == txservice::RecordStatus::Normal);
        const char *item_data = scan_batch_records[i].EncodedBlobData();
        size_t item_data_size = scan_batch_records[i].EncodedBlobSize();
        log_item_t item = deserialize_log_item(item_data, item_data_size);
        items.push_back(item);
    }

    return LogError::SUCCESS;
}

/**
 * @brief Retrieve metadata (statistics) for a named log.
 *
 * Reads the stored metadata record for the given log name within the provided
 * transaction execution context and returns the deserialized metadata.
 * If the metadata record is not found or a storage/read error occurs, a
 * default-constructed log_metadata_t (all fields zero) is returned.
 *
 * @param log_name Name of the log to query.
 * @return log_metadata_t Deserialized log metadata on success; default metadata
 * on error or if not found.
 */
log_metadata_t LogObject::get_stats(const std::string &log_name,
                                    txservice::TransactionExecution *txm)
{
    // Read metadata to get sequence ID range
    std::string meta_key = get_metadata_key(log_name);
    txservice::EloqStringKey meta_key_obj(meta_key.data(), meta_key.size());
    txservice::TxKey meta_tx_key(&meta_key_obj);

    txservice::EloqStringRecord meta_record;
    txservice::ReadTxRequest meta_read_req(&vector_index_meta_table,
                                           0,
                                           &meta_tx_key,
                                           &meta_record,
                                           false,
                                           false,
                                           false,
                                           0,
                                           false,
                                           false,
                                           false,
                                           nullptr,
                                           nullptr,
                                           nullptr);
    txm->Execute(&meta_read_req);
    meta_read_req.Wait();

    if (meta_read_req.ErrorCode() != txservice::TxErrorCode::NO_ERROR)
    {
        return log_metadata_t();
    }

    auto meta_result = meta_read_req.Result();
    if (meta_result.first != txservice::RecordStatus::Normal)
    {
        return log_metadata_t();
    }

    const char *meta_data = meta_record.EncodedBlobData();
    size_t meta_data_size = meta_record.EncodedBlobSize();
    log_metadata_t meta = deserialize_metadata(meta_data, meta_data_size);

    return meta;
}

// ===== SHARDED LOG IMPLEMENTATIONS =====

LogError LogObject::create_sharded_logs(const std::string &base_log_name,
                                        uint32_t num_shards,
                                        txservice::TransactionExecution *txm)
{
    // Require non-null transaction execution context
    if (txm == nullptr)
    {
        return LogError::INVALID_PARAMETER;
    }

    // Validate parameters
    if (num_shards == 0)
    {
        return LogError::INVALID_PARAMETER;
    }

    // Create all shard logs sequentially within transaction
    for (uint32_t shard_id = 0; shard_id < num_shards; ++shard_id)
    {
        std::string shard_log_name =
            get_shard_log_name(base_log_name, shard_id);
        LogError result = create_log(shard_log_name, txm);
        if (result != LogError::SUCCESS)
        {
            return result;
        }
    }

    return LogError::SUCCESS;
}

LogError LogObject::remove_sharded_logs(const std::string &base_log_name,
                                        uint32_t num_shards,
                                        txservice::TransactionExecution *txm)
{
    // Require non-null transaction execution context
    if (txm == nullptr)
    {
        return LogError::INVALID_PARAMETER;
    }

    // Validate parameters
    if (num_shards == 0)
    {
        return LogError::INVALID_PARAMETER;
    }

    // Remove all shard logs sequentially within transaction
    for (uint32_t shard_id = 0; shard_id < num_shards; ++shard_id)
    {
        std::string shard_log_name =
            get_shard_log_name(base_log_name, shard_id);
        LogError result = remove_log(shard_log_name, txm);
        if (result != LogError::SUCCESS)
        {
            return result;
        }
    }

    return LogError::SUCCESS;
}

LogError LogObject::append_log_sharded(const std::string &base_log_name,
                                       const std::string &shard_key,
                                       uint32_t num_shards,
                                       std::vector<log_item_t> &items,
                                       uint64_t &log_id,
                                       uint64_t &log_count,
                                       txservice::TransactionExecution *txm)
{
    // Require non-null transaction execution context
    if (txm == nullptr)
    {
        return LogError::INVALID_PARAMETER;
    }

    // Validate parameters
    if (num_shards == 0)
    {
        return LogError::INVALID_PARAMETER;
    }

    // Determine target shard
    uint32_t shard_id = get_shard_id(shard_key, num_shards);
    std::string shard_log_name = get_shard_log_name(base_log_name, shard_id);

    // Use existing single log append logic
    return append_log(shard_log_name, items, log_id, log_count, txm);
}

LogError LogObject::truncate_all_sharded_logs(
    const std::string &base_log_name,
    uint32_t num_shards,
    uint64_t &total_log_count,
    txservice::TransactionExecution *txm)
{
    // Require non-null transaction execution context
    if (txm == nullptr)
    {
        return LogError::INVALID_PARAMETER;
    }

    // Validate parameters
    if (num_shards == 0)
    {
        return LogError::INVALID_PARAMETER;
    }

    total_log_count = 0;

    // Truncate all shards sequentially within transaction
    for (uint32_t shard_id = 0; shard_id < num_shards; ++shard_id)
    {
        std::string shard_log_name =
            get_shard_log_name(base_log_name, shard_id);
        uint64_t shard_log_count = 0;
        uint64_t shard_to_id = UINT64_MAX;

        LogError result =
            truncate_log(shard_log_name, shard_to_id, shard_log_count, txm);
        if (result != LogError::SUCCESS)
        {
            return result;
        }

        total_log_count += shard_log_count;
    }

    return LogError::SUCCESS;
}

LogError LogObject::scan_sharded_log(const std::string &base_log_name,
                                     uint32_t num_shards,
                                     std::vector<log_item_t> &items,
                                     txservice::TransactionExecution *txm)
{
    // Require non-null transaction execution context
    if (txm == nullptr)
    {
        return LogError::INVALID_PARAMETER;
    }

    // Validate parameters
    if (num_shards == 0)
    {
        return LogError::INVALID_PARAMETER;
    }

    // Scan all shards sequentially within transaction
    std::vector<std::vector<log_item_t>> all_shard_items;
    all_shard_items.resize(num_shards);
    size_t total_items_count = 0;
    for (uint32_t shard_id = 0; shard_id < num_shards; ++shard_id)
    {
        std::string shard_log_name =
            get_shard_log_name(base_log_name, shard_id);
        std::vector<log_item_t> &shard_items = all_shard_items[shard_id];
        LogError result =
            scan_log(shard_log_name, UINT64_MAX, shard_items, txm);
        if (result != LogError::SUCCESS)
        {
            return result;
        }
        total_items_count += shard_items.size();
    }

    // Merge into the output vector
    items.reserve(total_items_count);
    for (auto &shard_items : all_shard_items)
    {
        items.insert(items.end(),
                     std::make_move_iterator(shard_items.begin()),
                     std::make_move_iterator(shard_items.end()));
    }

    return LogError::SUCCESS;
}

LogError LogObject::exists_sharded(const std::string &base_log_name,
                                   uint32_t num_shards,
                                   txservice::TransactionExecution *txm)
{
    // Require non-null transaction execution context
    if (txm == nullptr)
    {
        return LogError::INVALID_PARAMETER;
    }

    // Validate parameters
    if (num_shards == 0)
    {
        return LogError::INVALID_PARAMETER;
    }

    // Check if all shards exist
    for (uint32_t shard_id = 0; shard_id < num_shards; ++shard_id)
    {
        std::string shard_log_name =
            get_shard_log_name(base_log_name, shard_id);
        if (!exists(shard_log_name, txm))
        {
            return LogError::LOG_NOT_FOUND;
        }
    }

    return LogError::SUCCESS;
}

}  // namespace EloqVec
