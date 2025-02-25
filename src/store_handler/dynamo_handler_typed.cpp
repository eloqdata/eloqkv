#include "dynamo_handler_typed.h"

#include <utility>

#include "redis_hash_object.h"
#include "redis_list_object.h"
#include "redis_object.h"  // RedisEloqObject
#include "redis_set_object.h"
#include "redis_string_object.h"
#include "redis_zset_object.h"

namespace EloqDS
{

void DynamoHandlerTyped::SetSequenceTableSchema(
    const txservice::TableSchema *tbl_schema)
{
    assert(false);
}
const txservice::TableSchema *DynamoHandlerTyped::GetSequenceTableSchema()
{
    assert(false);
}

const std::unordered_map<
    uint,
    std::pair<txservice::TableName, txservice::SecondaryKeySchema>>
    *DynamoHandlerTyped::GetIndexes(const txservice::TableSchema *table_schema)
{
    assert(false);
    return nullptr;
}

void DynamoHandlerTyped::BindBlobFieldForUnpackInfo(
    Aws::DynamoDB::Model::PutRequest &put_req, const txservice::TxRecord *rec)
{
    // unpack info is null
    Aws::DynamoDB::Model::AttributeValue unpack_info;
    unpack_info.SetNull(true);
    put_req.AddItem("___unpack_info___", std::move(unpack_info));
}

void DynamoHandlerTyped::ParseUnpackInfoFromDynamoRow(
    txservice::TxRecord *rec,
    const Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue> &row)
{
    // unpack info is null
    return;
}

void DynamoHandlerTyped::BindDynamoReqForPayload(
    Aws::DynamoDB::Model::PutRequest &put_req,
    const txservice::TxRecord *rec,
    const txservice::RecordSchema *rec_schema,
    const txservice::TableName &table)
{
    if (table.Type() == txservice::TableType::Primary)
    {
        Aws::DynamoDB::Model::AttributeValue payload;
        auto *blob_rec = dynamic_cast<const txservice::BlobTxRecord *>(rec);
        if (blob_rec)
        {
            payload.SetB(
                Aws::Utils::ByteBuffer(reinterpret_cast<const unsigned char *>(
                                           blob_rec->value_.data()),
                                       blob_rec->value_.size()));
        }
        else
        {
            std::string payload_str;
            rec->Serialize(payload_str);
            payload.SetB(Aws::Utils::ByteBuffer(
                reinterpret_cast<const unsigned char *>(payload_str.data()),
                payload_str.size()));
        }
        // Binds the record's "___payload___" column.
        put_req.AddItem("___payload___", std::move(payload));
    }
    else
    {
        assert(table.Type() == txservice::TableType::Secondary ||
               table.Type() == txservice::TableType::UniqueSecondary);
    }
}

void DynamoHandlerTyped::ParsePayloadFromDynamoRow(
    const Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue> &row,
    txservice::TxRecord *rec,
    const txservice::RecordSchema *rec_schema,
    const txservice::TableName &table)
{
    if (table.IsBase())
    {
        auto typed_rec = static_cast<EloqKV::RedisEloqObject *>(rec);

        Aws::Utils::ByteBuffer payload_val = row.at("___payload___").GetB();

        const char *payload_ptr =
            reinterpret_cast<const char *>(payload_val.GetUnderlyingData());
        int8_t obj_type_int = static_cast<int8_t>(*payload_ptr);
        EloqKV::RedisObjectType obj_type =
            static_cast<EloqKV::RedisObjectType>(obj_type_int);
        assert(typed_rec->ObjectType() == obj_type);

        size_t offset = 0;
        rec->Deserialize(reinterpret_cast<const char *>(payload_ptr), offset);
    }
    else
    {
        assert(table.Type() == txservice::TableType::Secondary ||
               table.Type() == txservice::TableType::UniqueSecondary);
    }
}

txservice::TxRecord::Uptr DynamoHandlerTyped::ParsePayloadFromDynamoRow(
    const Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue> &row,
    const txservice::RecordSchema *rec_schema,
    const txservice::TableName &table)
{
    if (table.IsBase())
    {
        Aws::Utils::ByteBuffer payload_val = row.at("___payload___").GetB();

        const char *payload_ptr =
            reinterpret_cast<const char *>(payload_val.GetUnderlyingData());
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

txservice::TxRecord::Uptr DynamoHandlerTyped::NewTxRecord(
    const txservice::TableName &table)
{
    assert(false);
}

const txservice::TxKey *DynamoHandlerTyped::NegInfKey(
    const txservice::TableName &table)
{
    // Please implement this based different "KeyType".
    //
    // return txservice::NegativeInfinity<KeyType>::Instance();

    assert(false);
}

const txservice::TxKey *DynamoHandlerTyped::PosInfKey(
    const txservice::TableName &table)
{
    // Please implement this based different "KeyType".
    //
    // return txservice::PositiveInfinity<KeyType>::Instance();

    assert(false);
}

void DynamoHandlerTyped::ColumnsToScanWhenBuildSk(
    const txservice::TableSchema *table_schema,
    const std::vector<txservice::TableName> &new_indexes_name,
    bool &full_column_scan,
    std::unordered_set<std::string_view> &sk_parts_name)
{
    assert(false);
    full_column_scan = true;
}

}  // namespace EloqDS
