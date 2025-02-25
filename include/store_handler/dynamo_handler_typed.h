#pragma once
#include <aws/core/Aws.h>
#include <aws/dynamodb/model/AttributeValue.h>  // Attribute
#include <aws/dynamodb/model/PutRequest.h>      // PutRequest
#include <aws/dynamodb/model/QueryRequest.h>
#include <aws/dynamodb/model/QueryResult.h>

#include <memory>  // make_unique
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "tx_service/include/catalog_factory.h"  // TableSchema
#include "tx_service/include/tx_key.h"
#include "tx_service/include/tx_record.h"
#include "tx_service/include/type.h"

namespace EloqDS
{
class DynamoHandlerTyped
{
public:
    constexpr static unsigned char neg_inf_packed_key_{0x00};
    inline static const std::string mysql_seq_string_{"./mysql/sequences"};
    inline static const txservice::TableName sequence_table_name_{
        mysql_seq_string_.data(),
        mysql_seq_string_.size(),
        txservice::TableType::Primary};

    static void SetSequenceTableSchema(
        const txservice::TableSchema *tbl_schema);
    static const txservice::TableSchema *GetSequenceTableSchema();

    static const std::unordered_map<
        uint,
        std::pair<txservice::TableName, txservice::SecondaryKeySchema>>
        *GetIndexes(const txservice::TableSchema *table_schema);

    // Pairs of payload's field name with datatype name separated by
    // "," in Cassandra Table. (Should be end with ",")
    static std::string PayloadColumnListWithType(
        const txservice::TableName &table,
        const txservice::RecordSchema *record_schema);

    // TxKey is saved as one blob field in cassandra table.
    // (Notice: ensure key bytes are mem-comparable if scan cass table with
    // key.)
    template <typename KeyT>
    static void BindDynamoReqForKey(Aws::DynamoDB::Model::AttributeValue &sk,
                                    const KeyT &key);

    static int predecessor(unsigned char *const packed_tuple, const uint len)
    {
        assert(packed_tuple != nullptr);

        int changed = 0;
        unsigned char *p = packed_tuple + len - 1;
        for (; p > packed_tuple; p--)
        {
            changed++;
            if (*p != (unsigned char) (0x00))
            {
                *p = *p - 1;
                break;
            }
            *p = 0xFF;
        }
        return changed;
    }

    template <typename KeyT>
    static void BindDynamoReqForKeyPredecessor(
        Aws::DynamoDB::Model::AttributeValue &sk, const KeyT &key);

    static void BindBlobFieldForUnpackInfo(
        Aws::DynamoDB::Model::PutRequest &put_req,
        const txservice::TxRecord *rec);
    static void ParseUnpackInfoFromDynamoRow(
        txservice::TxRecord *rec,
        const Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue> &row);

    // Bind dynamo request for payload fields, like
    // (non_key_column ..., unpack_info). Used by putall().
    static void BindDynamoReqForPayload(
        Aws::DynamoDB::Model::PutRequest &put_req,
        const txservice::TxRecord *rec,
        const txservice::RecordSchema *rec_schema,
        const txservice::TableName &table);
    static void ParsePayloadFromDynamoRow(
        const Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue> &row,
        txservice::TxRecord *rec,
        const txservice::RecordSchema *rec_schema,
        const txservice::TableName &table);

    static txservice::TxRecord::Uptr ParsePayloadFromDynamoRow(
        const Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue> &row,
        const txservice::RecordSchema *rec_schema,
        const txservice::TableName &table);

    // Append key into Dynamo Aws::DynamoDB::Model::AttributeValue.
    template <typename KeyT>
    static void AddKeyBytesIntoCollection(
        Aws::DynamoDB::Model::AttributeValue &attr_val, const KeyT &key);

    static txservice::TxRecord::Uptr NewTxRecord(
        const txservice::TableName &table);

    static const txservice::TxKey *NegInfKey(const txservice::TableName &table);
    static const txservice::TxKey *PosInfKey(const txservice::TableName &table);

    static void ColumnsToScanWhenBuildSk(
        const txservice::TableSchema *table_schema,
        const std::vector<txservice::TableName> &new_indexes_name,
        bool &full_column_scan,
        std::unordered_set<std::string_view> &columns);
};

template <typename KeyT>
void DynamoHandlerTyped::BindDynamoReqForKey(
    Aws::DynamoDB::Model::AttributeValue &sk, const KeyT &key)
{
    std::string_view key_str = key.KVSerialize();
    sk.SetB(Aws::Utils::ByteBuffer(
        reinterpret_cast<const unsigned char *>(key_str.data()),
        key_str.size()));
}

template <typename KeyT>
void DynamoHandlerTyped::AddKeyBytesIntoCollection(
    Aws::DynamoDB::Model::AttributeValue &attr_val, const KeyT &key)
{
    std::string_view key_str = key.KVSerialize();
    attr_val.AddBItem(reinterpret_cast<const unsigned char *>(key_str.data()),
                      key_str.size());
}

template <typename KeyT>
void DynamoHandlerTyped::BindDynamoReqForKeyPredecessor(
    Aws::DynamoDB::Model::AttributeValue &sk, const KeyT &key)
{
    assert(false);

    std::string_view predecessor_key_str = key.KVSerialize();
    auto data_ptr =
        reinterpret_cast<const unsigned char *>(predecessor_key_str.data());
    predecessor(const_cast<unsigned char *>(data_ptr),
                predecessor_key_str.size());

    sk.SetB(Aws::Utils::ByteBuffer(
        reinterpret_cast<const unsigned char *>(predecessor_key_str.data()),
        predecessor_key_str.size()));
}

}  // namespace EloqDS
