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
#pragma once

#include <list>
#include <memory>
#include <string>
#include <string_view>

#include "vector_type.h"

namespace EloqVec
{
// ============================================================================
// Predicate Expression for Search Filtering
// ============================================================================
enum class PredicateOp : uint8_t
{
    // Comparison operators
    // Equal
    EQ = 0,
    // Not equal
    NE = 1,
    // Greater than
    GT = 2,
    // Greater than or equal
    GE = 3,
    // Less than
    LT = 4,
    // Less than or equal
    LE = 5,
    // IN operator
    IN = 6,
    // Logical operators
    // Logical AND
    AND = 7,
    // Logical OR
    OR = 8,
    // Logical NOT
    NOT = 9,
    // Unknown
    UNKNOWN = 255
};

/**
 * @brief Base class for predicate nodes
 */
class PredicateNode
{
public:
    using Uptr = std::unique_ptr<PredicateNode>;
    static constexpr std::string_view ID_FIELD_NAME = "id";

    PredicateNode() = default;
    explicit PredicateNode(PredicateOp op) : op_(op)
    {
    }

    PredicateNode(const PredicateNode &) = delete;
    PredicateNode(PredicateNode &&) noexcept = default;
    PredicateNode &operator=(const PredicateNode &) = delete;
    PredicateNode &operator=(PredicateNode &&) noexcept = default;

    virtual ~PredicateNode() = default;

    virtual bool IsLeaf() const = 0;

    PredicateOp op_;
};

/**
 * @brief Parent node for predicate expression
 */
class ParentPredNode : public PredicateNode
{
public:
    using Uptr = std::unique_ptr<ParentPredNode>;
    ParentPredNode() = default;
    explicit ParentPredNode(PredicateOp op) : PredicateNode(op)
    {
    }
    ParentPredNode(const ParentPredNode &) = delete;
    ParentPredNode(ParentPredNode &&) noexcept = default;
    ParentPredNode &operator=(const ParentPredNode &) = delete;
    ParentPredNode &operator=(ParentPredNode &&) noexcept = default;
    ~ParentPredNode() = default;

    bool IsLeaf() const override
    {
        return false;
    }

    std::list<PredicateNode::Uptr> children_;
};

/**
 * @brief Leaf node for predicate expression
 */
class LeafPredNode : public PredicateNode
{
public:
    using Uptr = std::unique_ptr<LeafPredNode>;
    LeafPredNode() = default;
    explicit LeafPredNode(PredicateOp op) : PredicateNode(op)
    {
    }
    LeafPredNode(const LeafPredNode &) = delete;
    LeafPredNode(LeafPredNode &&) noexcept = default;
    LeafPredNode &operator=(const LeafPredNode &) = delete;
    LeafPredNode &operator=(LeafPredNode &&) noexcept = default;
    ~LeafPredNode() = default;

    bool IsLeaf() const override
    {
        return true;
    }

    std::string field_name_;
    // Value of the predicate node with binary encoding
    std::vector<char> value_;
};

/**
 * @brief Predicate expression for filtering vectors
 */
class PredicateExpression
{
public:
    PredicateExpression() = default;

    /**
     * @brief Parse a JSON-based filter expression
     * @param json_str JSON filter view string (e.g., {"age": {"$gt": 18}})
     * @param schema Metadata schema for field validation and type conversion
     * @return true if parse succeeded, false otherwise
     */
    bool Parse(const std::string_view &json_str,
               const VectorRecordMetadata &schema);

    /**
     * @brief Evaluate the predicate against metadata
     * @param metadata Metadata field values
     * @param offsets The offsets of each field in the metadata
     * @param schema Metadata schema for type information
     * @return true if predicate is satisfied, false otherwise
     */
    bool Evaluate(const std::vector<char> &metadata,
                  const std::vector<size_t> &offsets,
                  const VectorRecordMetadata &schema) const;

    /**
     * @brief Get the root node of the predicate tree
     */
    const PredicateNode &Root() const
    {
        return *root_node_;
    }

private:
    /**
     * @brief Evaluate a single predicate node
     * @param node Predicate node
     * @param metadata Metadata field values
     * @param offsets The offsets of each field in the metadata
     * @param schema Metadata schema for type information
     * @return true if predicate is satisfied, false otherwise
     */
    bool EvaluateNode(const PredicateNode &node,
                      const std::vector<char> &metadata,
                      const std::vector<size_t> &offsets,
                      const VectorRecordMetadata &schema) const;

    PredicateNode::Uptr root_node_{nullptr};
};

}  // namespace EloqVec