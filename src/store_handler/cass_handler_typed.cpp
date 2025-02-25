#include "cass_handler_typed.h"

#include <memory>
#include <utility>

#include "eloq_key.h"
#include "redis_hash_object.h"
#include "redis_list_object.h"
#include "redis_object.h"  // RedisEloqObject
#include "redis_set_object.h"
#include "redis_string_object.h"
#include "redis_zset_object.h"
#include "tx_key.h"

// Store the serialized bytes of payload in one blob field of cass table.

namespace EloqDS
{

void CassHandlerTyped::SetSequenceTableSchema(
    const txservice::TableSchema *tbl_schema)
{
    assert(false);
}
const txservice::TableSchema *CassHandlerTyped::GetSequenceTableSchema()
{
    assert(false);
    return nullptr;
}

const std::unordered_map<
    uint,
    std::pair<txservice::TableName, txservice::SecondaryKeySchema>>
    *CassHandlerTyped::GetIndexes(const txservice::TableSchema *table_schema)
{
    assert(false);
    return nullptr;
}

uint16_t CassHandlerTyped::PayloadColumnCount(
    const txservice::TableName &table,
    const txservice::RecordSchema *record_schema)
{
    if (table.Type() == txservice::TableType::Primary)
    {
        return 1;  //___payload___
    }
    else
    {
        assert(table.Type() == txservice::TableType::Secondary ||
               table.Type() == txservice::TableType::UniqueSecondary);
        return 0;
    }
}

std::string CassHandlerTyped::PayloadColumnList(
    const txservice::TableName &table,
    const txservice::RecordSchema *record_schema)
{
    if (table.Type() == txservice::TableType::Primary)
    {
        return (" \"___payload___\", ");
    }
    else
    {
        assert(table.Type() == txservice::TableType::Secondary ||
               table.Type() == txservice::TableType::UniqueSecondary);
        return "";
    }
}

std::string CassHandlerTyped::PayloadPartColumnList(
    const txservice::TableName &table,
    const txservice::RecordSchema *record_schema)
{
    assert(false);
    return (" \"___payload___\", ");
}

std::string CassHandlerTyped::PayloadColumnListWithType(
    const txservice::TableName &table,
    const txservice::RecordSchema *record_schema)
{
    if (table.Type() == txservice::TableType::Primary)
    {
        return (" \"___payload___\" blob, ");
    }
    else
    {
        assert(table.Type() == txservice::TableType::Secondary ||
               table.Type() == txservice::TableType::UniqueSecondary);
        return "";
    }
}

bool CassHandlerTyped::HasAutoIncreCol(
    const txservice::TableName &table,
    const txservice::TableSchema *table_schema)
{
    assert(false);
    return false;
}

void CassHandlerTyped::BindBlobFieldForUnpackInfo(
    CassStatement *stmt, size_t index, const txservice::TxRecord *rec)
{
    // unpack info is null
    cass_statement_bind_null(stmt, index);
}

void CassHandlerTyped::ParseUnpackInfoFromBlob(txservice::TxRecord *rec,
                                               const cass_byte_t *buf,
                                               size_t buf_len)
{
    // unpack info is null
    return;
}

void CassHandlerTyped::BindStmtForPayload(
    CassStatement *stmt,
    size_t index,
    const txservice::TxRecord *rec,
    const txservice::RecordSchema *rec_schema,
    const txservice::TableName &table)
{
    assert(index == 0);
    if (table.IsBase())
    {
        auto *blob_rec = dynamic_cast<const txservice::BlobTxRecord *>(rec);
        if (blob_rec)
        {
            // Binds the record's "___payload___" column.
            cass_statement_bind_bytes(
                stmt,
                index,
                reinterpret_cast<const cass_byte_t *>(blob_rec->value_.data()),
                blob_rec->value_.size());
        }
        else
        {
            std::string payload_str;
            rec->Serialize(payload_str);
            // Binds the record's "___payload___" column.
            cass_statement_bind_bytes(
                stmt,
                index,
                reinterpret_cast<const cass_byte_t *>(payload_str.data()),
                payload_str.size());
        }
    }
    else
    {
        assert(table.Type() == txservice::TableType::Secondary ||
               table.Type() == txservice::TableType::UniqueSecondary);
        cass_statement_bind_null(stmt, index);
    }
}

bool CassHandlerTyped::ParsePayloadFromCassRow(
    const CassRow *row,
    size_t row_index,
    txservice::TxRecord *rec,
    const txservice::RecordSchema *rec_schema,
    const txservice::TableName &table)
{
    if (table.IsBase())
    {
        auto typed_rec = static_cast<EloqKV::RedisEloqObject *>(rec);

        const cass_byte_t *payload = NULL;
        size_t payload_len = 0;
        cass_value_get_bytes(
            cass_row_get_column(row, row_index), &payload, &payload_len);

        const char *payload_ptr = reinterpret_cast<const char *>(payload);
        int8_t obj_type_int = static_cast<int8_t>(*payload_ptr);
        EloqKV::RedisObjectType obj_type =
            static_cast<EloqKV::RedisObjectType>(obj_type_int);
        assert(typed_rec->ObjectType() == obj_type);

        size_t offset = 0;
        rec->Deserialize(payload_ptr, offset);
    }
    else
    {
        assert(table.Type() == txservice::TableType::Secondary ||
               table.Type() == txservice::TableType::UniqueSecondary);
    }
    return true;
}

txservice::TxRecord::Uptr CassHandlerTyped::ParsePayloadFromCassRow(
    const CassRow *row,
    size_t row_index,
    const txservice::RecordSchema *rec_schema,
    const txservice::TableName &table)
{
    if (table.IsBase())
    {
        const cass_byte_t *payload = NULL;
        size_t payload_len = 0;
        cass_value_get_bytes(
            cass_row_get_column(row, row_index), &payload, &payload_len);

        const char *payload_ptr = reinterpret_cast<const char *>(payload);
        size_t offset = 0;
        EloqKV::RedisEloqObject tmp_obj;
        return tmp_obj.DeserializeObject(payload_ptr, offset);
    }
    else
    {
        assert(table.Type() == txservice::TableType::Secondary ||
               table.Type() == txservice::TableType::UniqueSecondary);
        return nullptr;
    }
}

// Bind payload as one blob field to cass statement.
void CassHandlerTyped::BindBlobFieldForPayload(
    CassStatement *stmt,
    size_t index,
    const txservice::TxRecord *rec,
    const txservice::TableName &table)
{
    assert(false);
    if (table.IsBase() || table.IsUniqueSecondary())
    {
        std::string payload_str;
        rec->Serialize(payload_str);
        // Binds the record's "___payload___" or "___trailing_pk___" column.
        cass_statement_bind_bytes(
            stmt,
            index,
            reinterpret_cast<const cass_byte_t *>(payload_str.data()),
            payload_str.size());
    }
    else
    {
        cass_statement_bind_null(stmt, index);
    }
}
void CassHandlerTyped::ParsePayloadFromBlob(txservice::TxRecord *rec,
                                            const cass_byte_t *buf,
                                            size_t buf_len,
                                            const txservice::TableName &table)
{
    assert(false);

    size_t offset = 0;
    rec->Deserialize(reinterpret_cast<const char *>(buf), offset);
    assert(offset == buf_len);
}

txservice::TxKey CassHandlerTyped::NewTxKey(const txservice::TableName &table,
                                            const cass_byte_t *buf,
                                            size_t len)
{
    // Please implement this based different "KeyType".
    //
    std::unique_ptr<EloqKV::EloqKey> key = std::make_unique<EloqKV::EloqKey>();
    if (buf != nullptr)
    {
        key->KVDeserialize(reinterpret_cast<const char *>(buf), len);
    }

    return txservice::TxKey(std::move(key));
}

txservice::TxRecord::Uptr CassHandlerTyped::NewTxRecord(
    const txservice::TableName &table)
{
    // Please implement this based different "RecordType".
    //
    // return std::make_unique<RecordType>();

    assert(false);
    return nullptr;
}

const txservice::TxKey *CassHandlerTyped::NegInfKey(
    const txservice::TableName &table)
{
    // Please implement this based different "KeyType".
    //
    // return txservice::NegativeInfinity<KeyType>::Instance();

    assert(false);
    return nullptr;
}

const txservice::TxKey *CassHandlerTyped::PosInfKey(
    const txservice::TableName &table)
{
    // Please implement this based different "KeyType".
    //
    // return txservice::PositiveInfinity<KeyType>::Instance();

    assert(false);
    return nullptr;
}

void CassHandlerTyped::ColumnsToScanWhenBuildSk(
    const txservice::TableSchema *table_schema,
    const std::vector<txservice::TableName> &new_indexes_name,
    bool &full_column_scan,
    std::unordered_set<std::string_view> &sk_parts_name)
{
    assert(false);
    full_column_scan = true;
}

}  // namespace EloqDS
