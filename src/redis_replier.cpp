#include "redis_replier.h"

namespace EloqKV
{
void RedisReplier::OnBool(bool b)
{
    int64_t res = b;
    cur_reply_->SetInteger(res);
    UpdateArray();
}

void RedisReplier::OnString(std::string_view str)
{
    cur_reply_->SetString(butil::StringPiece(str.data(), str.size()));
    UpdateArray();
}

void RedisReplier::OnInt(int64_t val)
{
    cur_reply_->SetInteger(val);
    UpdateArray();
}

void RedisReplier::OnArrayStart(unsigned int len)
{
    if (!cmd_reply_stack_.empty())
    {
        // increment the next index in current reply array
        auto &[cur_array, next] = cmd_reply_stack_.top();
        next++;
    }
    cur_reply_->SetArray(len);
    // cur_reply now is the new array
    cmd_reply_stack_.push({cur_reply_, 0});
    cur_reply_ = &(*cur_reply_)[0];
}

void RedisReplier::OnArrayEnd()
{
    assert(!cmd_reply_stack_.empty());
    cmd_reply_stack_.pop();
    if (!cmd_reply_stack_.empty())
    {
        // update cur_reply_ to next index in outer reply array
        auto &[cmd_reply, next] = cmd_reply_stack_.top();
        cur_reply_ = &(*cmd_reply)[next];
    }
}

void RedisReplier::OnNil()
{
    cur_reply_->SetNullString();
    UpdateArray();
}

void RedisReplier::OnStatus(std::string_view str)
{
    cur_reply_->SetStatus(butil::StringPiece(str.data(), str.size()));
    UpdateArray();
}

void RedisReplier::OnError(std::string_view str)
{
    cur_reply_->SetError(butil::StringPiece{str.data(), str.size()});
    UpdateArray();
}

void RedisReplier::OnFormatError(const char *fmt, ...)
{
    va_list args, cpy;
    va_start(args, fmt);

    char static_buf[1024], *buf = static_buf;
    size_t buflen = sizeof(static_buf);
    int bufstrlen;

    while (true)
    {
        va_copy(cpy, args);
        bufstrlen = vsnprintf(buf, buflen, fmt, cpy);
        assert(buflen >= 0);
        va_end(cpy);
        if (static_cast<size_t>(bufstrlen) >= buflen)
        {
            buflen *= 2;
            if (buf == static_buf)
            {
                buf = static_cast<char *>(malloc(buflen));
            }
            else
            {
                buf = static_cast<char *>(realloc(buf, buflen));
            }
        }
        else
        {
            break;
        }
    }

    OnString(std::string_view(buf, bufstrlen));

    if (buf != static_buf)
    {
        free(buf);
    }

    va_end(args);
}

void RedisReplier::UpdateArray()
{
    if (!cmd_reply_stack_.empty())
    {
        auto &[cmd_reply, next] = cmd_reply_stack_.top();
        next++;
        cur_reply_ = &(*cmd_reply)[next];
    }
}
}  // namespace EloqKV
