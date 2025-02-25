#pragma once
#include <memory>  // make_unique
#include <string>

#include "cass/include/cassandra.h"
#include "tx_service/include/catalog_factory.h"  // TableSchema
#include "tx_service/include/tx_key.h"
#include "tx_service/include/tx_record.h"
#include "tx_service/include/type.h"

namespace EloqDS
{
class CassHandlerTyped
{
public:
    constexpr static cass_byte_t neg_inf_packed_key_{0x00};
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

    // Payload's fields is saved as independent fields or one blob field in
    // Cassandra Table.
    static uint16_t PayloadColumnCount(
        const txservice::TableName &table,
        const txservice::RecordSchema *record_schema);

    // Field names of payload separated by "," in Cassandra Table.
    // (Should be end with ",")
    static std::string PayloadColumnList(
        const txservice::TableName &table,
        const txservice::RecordSchema *record_schema);

    // Part field names(specified by key_part_name) of payload separated by ","
    // in Cassandra Table. (Should be end with ",")
    static std::string PayloadPartColumnList(
        const txservice::TableName &table,
        const txservice::RecordSchema *record_schema);

    // Pairs of payload's field name with datatype name separated by
    // "," in Cassandra Table. (Should be end with ",")
    static std::string PayloadColumnListWithType(
        const txservice::TableName &table,
        const txservice::RecordSchema *record_schema);

    static bool HasAutoIncreCol(const txservice::TableName &table,
                                const txservice::TableSchema *table_schema);

    // TxKey is saved as one blob field in cassandra table.
    // (Notice: ensure key bytes are mem-comparable if scan cass table with
    // key.)
    template <typename KeyT>
    static void BindStmtForKey(CassStatement *stmt,
                               size_t index,
                               const KeyT *key);

    template <typename KeyT>
    static void ParseKeyFromBlob(KeyT *key,
                                 const cass_byte_t *buf,
                                 size_t buf_len);

    static void BindBlobFieldForUnpackInfo(CassStatement *stmt,
                                           size_t index,
                                           const txservice::TxRecord *rec);
    static void ParseUnpackInfoFromBlob(txservice::TxRecord *rec,
                                        const cass_byte_t *buf,
                                        size_t buf_len);

    // Bind cass statement for payload fields, like
    // (non_key_column ..., unpack_info). Used by putall().
    static void BindStmtForPayload(CassStatement *stmt,
                                   size_t index,
                                   const txservice::TxRecord *rec,
                                   const txservice::RecordSchema *rec_schema,
                                   const txservice::TableName &table);

    static bool ParsePayloadFromCassRow(
        const CassRow *row,
        size_t row_index,
        txservice::TxRecord *rec,
        const txservice::RecordSchema *rec_schema,
        const txservice::TableName &table);

    static txservice::TxRecord::Uptr ParsePayloadFromCassRow(
        const CassRow *row,
        size_t row_index,
        const txservice::RecordSchema *rec_schema,
        const txservice::TableName &table);

    // Bind payload as one blob field to cass statement. (used by mvcc_archives)
    static void BindBlobFieldForPayload(CassStatement *stmt,
                                        size_t index,
                                        const txservice::TxRecord *rec,
                                        const txservice::TableName &table);
    static void ParsePayloadFromBlob(txservice::TxRecord *rec,
                                     const cass_byte_t *buf,
                                     size_t buf_len,
                                     const txservice::TableName &table);

    // Append key into cass collection field of cass statement.
    template <typename KeyT>
    static void AddKeyBytesIntoCollection(CassCollection *collection,
                                          const KeyT *key);

    static txservice::TxKey NewTxKey(const txservice::TableName &table,
                                     const cass_byte_t *buf = nullptr,
                                     size_t len = 0);
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
void CassHandlerTyped::BindStmtForKey(CassStatement *stmt,
                                      size_t index,
                                      const KeyT *key)
{
    std::string_view key_str = key->KVSerialize();
    cass_statement_bind_bytes(
        stmt,
        index,
        reinterpret_cast<const cass_byte_t *>(key_str.data()),
        key_str.size());
}

template <typename KeyT>
void CassHandlerTyped::ParseKeyFromBlob(KeyT *key,
                                        const cass_byte_t *buf,
                                        size_t buf_len)
{
    key->KVDeserialize(reinterpret_cast<const char *>(buf), buf_len);
}

// Append key into cass collection field of cass statement.
template <typename KeyT>
void CassHandlerTyped::AddKeyBytesIntoCollection(CassCollection *collection,
                                                 const KeyT *key)
{
    std::string_view key_str = key->KVSerialize();
    cass_collection_append_bytes(
        collection,
        reinterpret_cast<const cass_byte_t *>(key_str.data()),
        key_str.size());
}
}  // namespace EloqDS
