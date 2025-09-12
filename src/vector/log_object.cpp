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

#include <cstring>

#include "eloq_string_key_record.h"
#include "tx_execution.h"
#include "tx_request.h"

namespace EloqVec
{

// Helper functions that don't need transaction execution context

void LogObject::serialize_metadata(const log_metadata_t &meta,
                                   std::string &result)
{
    // Reserve space for fixed-size data (4 uint64_t fields)
    result.reserve(sizeof(uint64_t) * 4 + result.size());

    // Serialize list_count (uint64_t)
    result.append(reinterpret_cast<const char *>(&meta.list_count),
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

log_metadata_t LogObject::deserialize_metadata(const std::string &data)
{
    log_metadata_t meta;
    size_t offset = 0;

    // Deserialize list_count (uint64_t)
    if (offset + sizeof(uint64_t) <= data.size())
    {
        std::memcpy(&meta.list_count, data.data() + offset, sizeof(uint64_t));
        offset += sizeof(uint64_t);
    }

    // Deserialize head_item_sequence_id (uint64_t)
    if (offset + sizeof(uint64_t) <= data.size())
    {
        std::memcpy(&meta.head_item_sequence_id,
                    data.data() + offset,
                    sizeof(uint64_t));
        offset += sizeof(uint64_t);
    }

    // Deserialize tail_item_sequence_id (uint64_t)
    if (offset + sizeof(uint64_t) <= data.size())
    {
        std::memcpy(&meta.tail_item_sequence_id,
                    data.data() + offset,
                    sizeof(uint64_t));
        offset += sizeof(uint64_t);
    }

    // Deserialize next_id (uint64_t)
    if (offset + sizeof(uint64_t) <= data.size())
    {
        std::memcpy(&meta.next_id, data.data() + offset, sizeof(uint64_t));
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

log_item_t LogObject::deserialize_log_item(const std::string &data)
{
    log_item_t item;
    size_t offset = 0;

    // Deserialize operation_type (uint8_t)
    if (offset + sizeof(uint8_t) <= data.size())
    {
        uint8_t op_type;
        std::memcpy(&op_type, data.data() + offset, sizeof(uint8_t));
        item.operation_type = static_cast<LogOperationType>(op_type);
        offset += sizeof(uint8_t);
    }

    // Deserialize key (string): length + content
    if (offset + sizeof(uint64_t) <= data.size())
    {
        uint64_t key_len;
        std::memcpy(&key_len, data.data() + offset, sizeof(uint64_t));
        offset += sizeof(uint64_t);

        if (offset + key_len <= data.size())
        {
            item.key.assign(data.data() + offset, key_len);
            offset += key_len;
        }
    }

    // Deserialize value (string): length + content
    if (offset + sizeof(uint64_t) <= data.size())
    {
        uint64_t value_len;
        std::memcpy(&value_len, data.data() + offset, sizeof(uint64_t));
        offset += sizeof(uint64_t);

        if (offset + value_len <= data.size())
        {
            item.value.assign(data.data() + offset, value_len);
            offset += value_len;
        }
    }

    // Deserialize timestamp (uint64_t)
    if (offset + sizeof(uint64_t) <= data.size())
    {
        std::memcpy(&item.timestamp, data.data() + offset, sizeof(uint64_t));
        offset += sizeof(uint64_t);
    }

    // Deserialize sequence_id (uint64_t)
    if (offset + sizeof(uint64_t) <= data.size())
    {
        std::memcpy(&item.sequence_id, data.data() + offset, sizeof(uint64_t));
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

// Public API implementations

LogError LogObject::create_log(const std::string &log_name,
                               txservice::TransactionExecution *txm)
{
    // Require non-null transaction execution context
    if (txm == nullptr)
    {
        return LogError::INVALID_PARAMETER;
    }

    // Check if log already exists
    txservice::TableName table_name_obj(std::string_view("log_table"),
                                        txservice::TableType::Primary,
                                        txservice::TableEngine::InternalHash);

    // Create key for metadata
    std::string meta_key = get_metadata_key(log_name);
    txservice::EloqStringKey key_obj(meta_key.data(), meta_key.size());
    txservice::TxKey tx_key(&key_obj);

    // Create record to read into
    txservice::EloqStringRecord record;

    // Create and execute read request
    txservice::ReadTxRequest read_req(&table_name_obj,
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
                                      txm);
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
    meta.list_count = 0;
    meta.head_item_sequence_id = 0;
    meta.tail_item_sequence_id = 0;
    meta.next_id = 0;

    // Write metadata to storage
    // Determine operation type
    txservice::OperationType op_type =
        (rec_status == txservice::RecordStatus::Deleted)
            ? txservice::OperationType::Insert
            : txservice::OperationType::Update;

    // Serialize metadata
    std::string serialized_meta;
    serialize_metadata(meta, serialized_meta);

    // Create record with serialized metadata
    auto record_ptr = std::make_unique<txservice::EloqStringRecord>();
    record_ptr->SetEncodedBlob(
        reinterpret_cast<const unsigned char *>(serialized_meta.c_str()),
        serialized_meta.size());

    // Create and execute upsert request
    txservice::UpsertTxRequest upsert_req(&table_name_obj,
                                          std::move(tx_key),
                                          std::move(record_ptr),
                                          op_type,
                                          nullptr,
                                          nullptr);
    txm->Execute(&upsert_req);
    upsert_req.Wait();

    // Check for errors
    if (upsert_req.ErrorCode() != txservice::TxErrorCode::NO_ERROR)
    {
        return LogError::STORAGE_ERROR;
    }

    return LogError::SUCCESS;
}

bool LogObject::exists(const std::string &log_name,
                       txservice::TransactionExecution *txm)
{
    // Create table name for log storage
    txservice::TableName table_name_obj(std::string_view("log_table"),
                                        txservice::TableType::Primary,
                                        txservice::TableEngine::InternalHash);

    // Create key for metadata
    std::string meta_key = get_metadata_key(log_name);
    txservice::EloqStringKey key_obj(meta_key.data(), meta_key.size());
    txservice::TxKey tx_key(&key_obj);

    // Create record to read into
    txservice::EloqStringRecord record;

    // Create and execute read request
    txservice::ReadTxRequest read_req(&table_name_obj,
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
                                      txm);
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

LogError LogObject::append_log(const std::string& log_name, std::vector<log_item_t>& items,
                              txservice::TransactionExecution* txm)
{
    assert(!items.empty());
    // Require non-null transaction execution context
    if (txm == nullptr) {
        return LogError::INVALID_PARAMETER;
    }
    
    if (items.empty()) {
        return LogError::SUCCESS; // Nothing to append
    }
    
    // Create table name for log storage
    txservice::TableName table_name_obj(
        std::string_view("log_table"), 
        txservice::TableType::Primary, 
        txservice::TableEngine::InternalHash);
    
    // Read current metadata to get next sequence ID
    std::string meta_key = get_metadata_key(log_name);
    txservice::EloqStringKey meta_key_obj(meta_key.data(), meta_key.size());
    txservice::TxKey meta_tx_key(&meta_key_obj);
    
    txservice::EloqStringRecord meta_record;
    txservice::ReadTxRequest meta_read_req(
        &table_name_obj, 0, &meta_tx_key, &meta_record,
        true, false, false, 0, false, false, false,
        nullptr, nullptr, txm);
    txm->Execute(&meta_read_req);
    meta_read_req.Wait();
    
    if (meta_read_req.ErrorCode() != txservice::TxErrorCode::NO_ERROR) {
        return LogError::STORAGE_ERROR;
    }
    
    auto meta_result = meta_read_req.Result();
    if (meta_result.first != txservice::RecordStatus::Normal) {
        return LogError::LOG_NOT_FOUND;
    }
    
    // Deserialize metadata
    std::string meta_data = meta_record.ToString();
    log_metadata_t meta = deserialize_metadata(meta_data);
    
    // Assign sequence IDs to items
    uint64_t current_seq_id = meta.next_id;
    for (auto& item : items) {
        item.sequence_id = current_seq_id++;
    }
    // No need to acqurie write intent on log items since we've already put wi on 
    // meta data key, which means there won't be any concurrent write to the same log.
    for (size_t i = 0; i < items.size(); ++i) {
        // Serialize log item
        std::string serialized_item;
        serialize_log_item(items[i], serialized_item);
        
        // Create record with serialized log item
        auto item_record_ptr = std::make_unique<txservice::EloqStringRecord>();
        item_record_ptr->SetEncodedBlob(
            reinterpret_cast<const unsigned char*>(serialized_item.c_str()), 
            serialized_item.size());
        
        // Create and execute upsert request
        std::string item_key = get_log_item_key(log_name, items[i].sequence_id);
        txservice::EloqStringKey item_key_obj(item_key.data(), item_key.size());
        txservice::TxKey item_tx_key(&item_key_obj);
        txservice::UpsertTxRequest item_upsert_req(
            &table_name_obj,
            std::move(item_tx_key),
            std::move(item_record_ptr),
            txservice::OperationType::Upsert,
            nullptr, nullptr);
        txm->Execute(&item_upsert_req);
        item_upsert_req.Wait();
        
        if (item_upsert_req.ErrorCode() != txservice::TxErrorCode::NO_ERROR) {
            return LogError::STORAGE_ERROR;
        }
    }
    
    // Update metadata
    meta.list_count += items.size();
    meta.next_id = current_seq_id;
    
    // Update head and tail sequence IDs
    assert(items.back().sequence_id == current_seq_id - 1);
    meta.tail_item_sequence_id = items.back().sequence_id;
    
    // Write updated metadata
    std::string serialized_meta;
    serialize_metadata(meta, serialized_meta);
    
    auto meta_record_ptr = std::make_unique<txservice::EloqStringRecord>();
    meta_record_ptr->SetEncodedBlob(
        reinterpret_cast<const unsigned char*>(serialized_meta.c_str()), 
        serialized_meta.size());
    
    txservice::UpsertTxRequest meta_upsert_req(
        &table_name_obj,
        std::move(meta_tx_key),
        std::move(meta_record_ptr),
        txservice::OperationType::Update,
        nullptr, nullptr);
    txm->Execute(&meta_upsert_req);
    meta_upsert_req.Wait();
    
    if (meta_upsert_req.ErrorCode() != txservice::TxErrorCode::NO_ERROR) {
        return LogError::STORAGE_ERROR;
    }
    
    return LogError::SUCCESS;
}

LogError LogObject::remove_log(const std::string& log_name, 
                              txservice::TransactionExecution* txm)
{
    // Require non-null transaction execution context
    if (txm == nullptr) {
        return LogError::INVALID_PARAMETER;
    }
    
    // Check if log exists
    if (!exists(log_name, txm)) {
        return LogError::LOG_NOT_FOUND;
    }
    
    // Create table name for log storage
    txservice::TableName table_name_obj(
        std::string_view("log_table"), 
        txservice::TableType::Primary, 
        txservice::TableEngine::InternalHash);
    
    // Read metadata to get sequence ID range
    std::string meta_key = get_metadata_key(log_name);
    txservice::EloqStringKey meta_key_obj(meta_key.data(), meta_key.size());
    txservice::TxKey meta_tx_key(&meta_key_obj);
    
    txservice::EloqStringRecord meta_record;
    txservice::ReadTxRequest meta_read_req(
        &table_name_obj, 0, &meta_tx_key, &meta_record,
        true, false, false, 0, false, false, false,
        nullptr, nullptr, txm);
    txm->Execute(&meta_read_req);
    meta_read_req.Wait();
    
    if (meta_read_req.ErrorCode() != txservice::TxErrorCode::NO_ERROR) {
        return LogError::STORAGE_ERROR;
    }
    
    auto meta_result = meta_read_req.Result();
    if (meta_result.first != txservice::RecordStatus::Normal) {
        return LogError::LOG_NOT_FOUND;
    }
    
    // Deserialize metadata
    std::string meta_data = meta_record.ToString();
    log_metadata_t meta = deserialize_metadata(meta_data);
    
    // Delete all log items in the range
    for (uint64_t seq_id = meta.head_item_sequence_id; 
         seq_id <= meta.tail_item_sequence_id; 
         ++seq_id) {
        
        std::string item_key = get_log_item_key(log_name, seq_id);
        txservice::EloqStringKey item_key_obj(item_key.data(), item_key.size());
        txservice::TxKey item_tx_key(&item_key_obj);
        
        auto item_record_ptr = std::make_unique<txservice::EloqStringRecord>();
        txservice::UpsertTxRequest item_delete_req(
            &table_name_obj,
            std::move(item_tx_key),
            std::move(item_record_ptr),
            txservice::OperationType::Delete,
            nullptr, nullptr);
        txm->Execute(&item_delete_req);
        item_delete_req.Wait();
        
        if (item_delete_req.ErrorCode() != txservice::TxErrorCode::NO_ERROR) {
            return LogError::STORAGE_ERROR;
        }
    }
    
    // Delete metadata
    auto meta_record_ptr = std::make_unique<txservice::EloqStringRecord>();
    txservice::UpsertTxRequest meta_delete_req(
        &table_name_obj,
        std::move(meta_tx_key),
        std::move(meta_record_ptr),
        txservice::OperationType::Delete,
        nullptr, nullptr);
    txm->Execute(&meta_delete_req);
    meta_delete_req.Wait();
    
    if (meta_delete_req.ErrorCode() != txservice::TxErrorCode::NO_ERROR) {
        return LogError::STORAGE_ERROR;
    }
    
    return LogError::SUCCESS;
}

}  // namespace EloqVec
