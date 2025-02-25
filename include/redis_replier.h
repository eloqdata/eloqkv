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
