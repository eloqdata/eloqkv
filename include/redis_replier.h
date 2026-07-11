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

#include <brpc/redis_reply.h>

#include <stack>

#include "output_handler.h"

namespace EloqKV
{
class RedisReplier : public OutputHandler
{
public:
    explicit RedisReplier(brpc::RedisReply *reply) : cur_reply_(reply)
    {
    }

    void OnBool(bool b) override;
    void OnString(std::string_view str) override;
    void OnInt(int64_t val) override;
    void OnArrayStart(unsigned len) override;
    void OnArrayEnd() override;
    void OnNil() override;
    // Emit a RESP null array (*-1). Distinct from OnNil(), which emits a
    // null bulk string ($-1).
    void OnNullArray();
    void OnStatus(std::string_view str) override;
    void OnError(std::string_view str) override;
    void OnFormatError(const char *fmt, ...) override;

private:
    // Every method could be called in array, check and update the reply array.
    void UpdateArray();

    // Redis reply and its next index to set result to if it's an array
    using CmdReply = std::pair<brpc::RedisReply *, int>;

    // Nested arrays are possible in RESP. For example, the result of a multi
    // transaction containing `lrange` command.
    std::stack<CmdReply, std::vector<CmdReply>> cmd_reply_stack_;
    // the RedisReply to set, if in array, cur_reply_ is the reply at last
    // CmdReply's index
    brpc::RedisReply *cur_reply_;
};

}  // namespace EloqKV
