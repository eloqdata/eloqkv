/**
 *    Copyright (C) 2025 EloqData Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under either of the following two licenses:
 *    1. GNU Affero General Public License, version 3, as published by the Free
 *    Software Foundation.
 *    2. GNU General Public License as published by the Free Software
 *    Foundation; version 2 of the License.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License or GNU General Public License for more
 *    details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    and GNU General Public License V2 along with this program.  If not, see
 *    <http://www.gnu.org/licenses/>.
 *
 */
#include "predicate.h"

#include <cassert>
#include <nlohmann/json.hpp>

#include "vector_type.h"
#include "vector_util.h"

namespace EloqVec
{
// ============================================================================
// PredicateExpression Implementation
// ============================================================================
// Helper for value comparisons
/**
 * @brief Compare two values
 * @param lhs_ptr Pointer to the left-hand side value
 * @param lhs_size Size of the left-hand side value
 * @param rhs_ptr Pointer to the right-hand side value
 * @param rhs_size Size of the right-hand side value(0 means same with lhs_size)
 * @param op Operator to compare
 * @return true if the values are equal, false otherwise
 */
static bool CompareValues(const char *lhs_ptr,
                          size_t lhs_size,
                          const char *rhs_ptr,
                          size_t rhs_size,
                          PredicateOp op)
{
    size_t cmp_size = rhs_size == 0 ? lhs_size : std::min(lhs_size, rhs_size);
    int cmp = std::memcmp(lhs_ptr, rhs_ptr, cmp_size);
    switch (op)
    {
    case PredicateOp::EQ:
    {
        return rhs_size == 0 ? cmp == 0 : (cmp == 0 && lhs_size == rhs_size);
    }
    case PredicateOp::NE:
    {
        return rhs_size == 0 ? cmp != 0 : (cmp != 0 || lhs_size != rhs_size);
    }
    case PredicateOp::GT:
    {
        return rhs_size == 0 ? cmp > 0
                             : (cmp > 0 || (cmp == 0 && lhs_size > rhs_size));
    }
    case PredicateOp::GE:
    {
        return rhs_size == 0 ? cmp >= 0
                             : (cmp >= 0 || (cmp == 0 && lhs_size > rhs_size));
    }
    case PredicateOp::LT:
    {
        return rhs_size == 0 ? cmp < 0
                             : (cmp < 0 || (cmp == 0 && lhs_size < rhs_size));
    }
    case PredicateOp::LE:
    {
        return rhs_size == 0 ? cmp <= 0
                             : (cmp <= 0 || (cmp == 0 && lhs_size < rhs_size));
    }
    default:
        return false;
    }
}

// Parse operator string to PredicateOp enum
PredicateOp ParseJSONOperator(const std::string &op)
{
    if (op == "$eq")
    {
        return PredicateOp::EQ;
    }
    if (op == "$ne")
    {
        return PredicateOp::NE;
    }
    if (op == "$gt")
    {
        return PredicateOp::GT;
    }
    if (op == "$gte")
    {
        return PredicateOp::GE;
    }
    if (op == "$lt")
    {
        return PredicateOp::LT;
    }
    if (op == "$lte")
    {
        return PredicateOp::LE;
    }
    if (op == "$in")
    {
        return PredicateOp::IN;
    }
    return PredicateOp::UNKNOWN;
}

PredicateNode::Uptr ParseINOperator(const std::string &field_name,
                                    const nlohmann::json &field_value,
                                    const MetadataFieldType &field_type)
{
    // convert to OR of multiple EQ predicates
    if (!field_value.is_array() || field_value.empty())
    {
        return nullptr;
    }

    ParentPredNode::Uptr or_node =
        std::make_unique<ParentPredNode>(PredicateOp::OR);
    for (const auto &item : field_value)
    {
        LeafPredNode::Uptr eq_node =
            std::make_unique<LeafPredNode>(PredicateOp::EQ);
        if (!ParseJSONFieldValue(item, field_type, eq_node->value_))
        {
            return nullptr;
        }
        eq_node->field_name_ = field_name;
        or_node->children_.emplace_back(std::move(eq_node));
    }
    return or_node;
}

PredicateNode::Uptr ParseMultiValueForSingleField(
    const std::string &field_name,
    const nlohmann::json &field_value,
    const MetadataFieldType &field_type)
{
    ParentPredNode::Uptr and_node =
        std::make_unique<ParentPredNode>(PredicateOp::AND);
    for (auto v_it = field_value.begin(); v_it != field_value.end(); ++v_it)
    {
        const std::string &op_str = v_it.key();
        const nlohmann::json &value_json = v_it.value();
        PredicateOp op = ParseJSONOperator(op_str);
        if (op == PredicateOp::UNKNOWN)
        {
            return nullptr;
        }
        else if (op == PredicateOp::IN)
        {
            PredicateNode::Uptr in_node =
                ParseINOperator(field_name, value_json, field_type);
            if (!in_node)
            {
                return nullptr;
            }
            and_node->children_.emplace_back(std::move(in_node));
        }
        else
        {
            // Create leaf node for other operators
            LeafPredNode::Uptr leaf_node = std::make_unique<LeafPredNode>(op);
            if (!ParseJSONFieldValue(value_json, field_type, leaf_node->value_))
            {
                return nullptr;
            }
            leaf_node->field_name_ = field_name;
            and_node->children_.emplace_back(std::move(leaf_node));
        }
    }
    return and_node;
}

PredicateNode::Uptr ParseSingleField(const std::string &field_name,
                                     const nlohmann::json &field_value,
                                     const VectorRecordMetadata &schema)
{
    if (field_value.empty() || !field_value.is_object())
    {
        return nullptr;
    }

    // Determine field type
    MetadataFieldType field_type;
    if (field_name.compare(PredicateNode::ID_FIELD_NAME) == 0)
    {
        field_type = MetadataFieldType::Int64;
    }
    // Verify field exists in schema
    else if (!schema.HasMetadataField(field_name))
    {
        return nullptr;
    }
    else
    {
        field_type = schema.GetFieldType(field_name);
    }

    if (field_value.size() > 1)
    {
        // multiple values for single field means implicit AND
        return ParseMultiValueForSingleField(
            field_name, field_value, field_type);
    }

    const std::string &op_str = field_value.begin().key();
    const nlohmann::json &value_json = field_value.begin().value();

    PredicateOp op = ParseJSONOperator(op_str);
    if (op == PredicateOp::UNKNOWN)
    {
        return nullptr;
    }
    else if (op == PredicateOp::IN)
    {
        return ParseINOperator(field_name, value_json, field_type);
    }

    // Create leaf node for other operators
    LeafPredNode::Uptr leaf_node = std::make_unique<LeafPredNode>(op);
    if (!ParseJSONFieldValue(value_json, field_type, leaf_node->value_))
    {
        return nullptr;
    }
    leaf_node->field_name_ = field_name;
    return leaf_node;
}

PredicateNode::Uptr ParseJSONObject(const nlohmann::json &obj,
                                    const VectorRecordMetadata &schema)
{
    if (!obj.is_object())
    {
        return nullptr;
    }

    // Check for logical operators
    if (obj.contains("$and"))
    {
        if (!obj["$and"].is_array())
        {
            return nullptr;
        }

        ParentPredNode::Uptr and_node =
            std::make_unique<ParentPredNode>(PredicateOp::AND);
        for (const auto &item : obj["$and"])
        {
            PredicateNode::Uptr child_node = ParseJSONObject(item, schema);
            if (!child_node)
            {
                return nullptr;
            }
            and_node->children_.emplace_back(std::move(child_node));
        }
        return and_node;
    }

    if (obj.contains("$or"))
    {
        if (!obj["$or"].is_array())
        {
            return nullptr;
        }

        ParentPredNode::Uptr or_node =
            std::make_unique<ParentPredNode>(PredicateOp::OR);
        for (const auto &item : obj["$or"])
        {
            PredicateNode::Uptr child_node = ParseJSONObject(item, schema);
            if (!child_node)
            {
                return nullptr;
            }
            or_node->children_.emplace_back(std::move(child_node));
        }
        return or_node;
    }

    if (obj.contains("$not"))
    {
        ParentPredNode::Uptr not_node =
            std::make_unique<ParentPredNode>(PredicateOp::NOT);
        PredicateNode::Uptr child_node = ParseJSONObject(obj["$not"], schema);
        if (!child_node)
        {
            return nullptr;
        }
        not_node->children_.emplace_back(std::move(child_node));
        return not_node;
    }

    // Parse field comparisons
    // Multiple fields at same level = implicit AND
    if (obj.size() > 1)
    {
        ParentPredNode::Uptr and_node =
            std::make_unique<ParentPredNode>(PredicateOp::AND);
        for (auto it = obj.begin(); it != obj.end(); ++it)
        {
            PredicateNode::Uptr child_node =
                ParseSingleField(it.key(), it.value(), schema);
            if (!child_node)
            {
                return nullptr;
            }
            and_node->children_.emplace_back(std::move(child_node));
        }
        return and_node;
    }

    // Single field comparison
    if (obj.empty())
    {
        return nullptr;
    }

    auto it = obj.begin();
    return ParseSingleField(it.key(), it.value(), schema);
}

bool PredicateExpression::Parse(const std::string_view &json_str,
                                const VectorRecordMetadata &schema)
{
    if (json_str.empty())
    {
        return false;
    }

    try
    {
        nlohmann::json j = nlohmann::json::parse(json_str);
        root_node_ = ParseJSONObject(j, schema);
        return root_node_ != nullptr;
    }
    catch (const std::exception &e)
    {
        // Parse error
        return false;
    }
}

bool PredicateExpression::Evaluate(const std::vector<char> &metadata,
                                   const std::vector<size_t> &offsets,
                                   const VectorRecordMetadata &schema) const
{
    assert(root_node_ != nullptr);
    return EvaluateNode(*root_node_, metadata, offsets, schema);
}

bool PredicateExpression::EvaluateNode(const PredicateNode &node,
                                       const std::vector<char> &metadata,
                                       const std::vector<size_t> &offsets,
                                       const VectorRecordMetadata &schema) const
{
    // Handle logical operators
    switch (node.op_)
    {
    case PredicateOp::AND:
    {
        const ParentPredNode &and_node =
            static_cast<const ParentPredNode &>(node);
        if (and_node.children_.size() < 2)
        {
            return false;
        }
        bool result = true;
        for (auto it = and_node.children_.begin();
             it != and_node.children_.end() && result;
             ++it)
        {
            const PredicateNode &child = **it;
            result = result && EvaluateNode(child, metadata, offsets, schema);
        }
        return result;
    }
    case PredicateOp::OR:
    {
        const ParentPredNode &or_node =
            static_cast<const ParentPredNode &>(node);
        if (or_node.children_.size() < 2)
        {
            return false;
        }
        bool result = false;
        for (auto it = or_node.children_.begin();
             it != or_node.children_.end() && !result;
             ++it)
        {
            const PredicateNode &child = **it;
            result = result || EvaluateNode(child, metadata, offsets, schema);
        }
        return result;
    }
    case PredicateOp::NOT:
    {
        const ParentPredNode &not_node =
            static_cast<const ParentPredNode &>(node);
        if (not_node.children_.size() != 1)
        {
            return false;
        }
        return !EvaluateNode(
            *not_node.children_.front(), metadata, offsets, schema);
    }
    default:
        break;
    }

    // Handle comparison operators (leaf nodes)
    const LeafPredNode &leaf_node = static_cast<const LeafPredNode &>(node);
    assert(leaf_node.IsLeaf());
    // Get field type from schema
    assert(schema.HasMetadataField(leaf_node.field_name_));
    size_t field_index = schema.GetFieldIndex(leaf_node.field_name_);
    assert(field_index < offsets.size());
    // binary format of the field value, with length prefix for string fields.
    const size_t start_idx = offsets[field_index];
    const char *metadata_ptr = metadata.data() + start_idx;
    size_t metadata_size = field_index == offsets.size() - 1
                               ? metadata.size() - start_idx
                               : offsets[field_index + 1] - start_idx;
    const char *value_ptr = leaf_node.value_.data();
    size_t value_size = 0;
    if (schema.GetFieldType(leaf_node.field_name_) == MetadataFieldType::String)
    {
        size_t len_size = sizeof(size_t);
        if (metadata_size < len_size || leaf_node.value_.size() < len_size)
        {
            return false;
        }
        metadata_ptr = metadata_ptr + len_size;
        metadata_size = metadata_size - len_size;
        value_ptr = value_ptr + len_size;
        value_size = leaf_node.value_.size() - len_size;
    }
    return CompareValues(
        metadata_ptr, metadata_size, value_ptr, value_size, leaf_node.op_);
}

}  // namespace EloqVec