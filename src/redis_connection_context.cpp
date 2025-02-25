#include "redis_connection_context.h"

#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include "redis_service.h"
#include "tx_util.h"

namespace EloqKV
{
RedisConnectionContext::~RedisConnectionContext()
{
    // Fixme(zkl): risk of data race for subscribed_channels
    if (!subscribed_channels.empty() || !subscribed_patterns.empty())
    {
        DLOG(INFO) << "Connection: " << this
                   << " has subscribed channels or patterns, "
                      "unsubscribe them all";
        pub_sub_mgr->UnsubscribeAll(this);
    }

    if (txm)
    {
        AbortTx(txm);
    }

    RedisStats::IncrConnClosed();
}

void RedisConnectionContext::SubscribeChannel(std::string_view chan)
{
    assert(!subscribed_channels.contains(chan));
    subscribed_channels.emplace(chan);
}

void RedisConnectionContext::UnsubscribeChannel(std::string_view chan)
{
    assert(subscribed_channels.contains(chan));
    subscribed_channels.erase(chan);
}

void RedisConnectionContext::SubscribePattern(std::string_view pattern)
{
    assert(!subscribed_patterns.contains(pattern));
    subscribed_patterns.emplace(pattern);
}

void RedisConnectionContext::UnsubscribePattern(std::string_view pattern)
{
    assert(subscribed_patterns.contains(pattern));
    subscribed_patterns.erase(pattern);
}

int RedisConnectionContext::SubscriptionsCount() const
{
    return subscribed_channels.size() + subscribed_patterns.size();
}

brpc::RedisReply *RedisConnectionContext::GetOutput()
{
    return &output;
}

bool RedisConnectionContext::FlushOutput()
{
    butil::IOBufAppender appender;
    output.SerializeTo(&appender);
    butil::IOBuf sendbuf;
    appender.move_to(sendbuf);
    CHECK(!sendbuf.empty());
    brpc::Socket::WriteOptions wopt;
    wopt.ignore_eovercrowded = true;

    if (socket->Write(&sendbuf, &wopt) != 0)
    {
        LOG(WARNING) << "Fail to send reply to client: " << this;
        return false;
    }

    return true;
}

// Cache cursor content and return a hash key to fetch next.
uint64_t RedisConnectionContext::CacheScanCursor(const std::string_view cursor)
{
    // FNV-1a hash algorithm.
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < cursor.size(); ++i)
    {
        hash ^= cursor[i];
        hash *= 1099511628211ULL;
    }
    auto [iter, emplaced] =
        scan_cursors.emplace(hash, std::make_pair(cursor, 1));
    if (!emplaced)
    {
        iter->second.first = cursor;
        iter->second.second++;
    }

    scan_cursor_list.push_back(&(*iter));
    if (scan_cursor_list.size() > 100)
    {
        auto &pr = scan_cursor_list.front();

        if (pr->second.second > 1)
        {
            pr->second.second--;
        }
        else
        {
            scan_cursors.erase(pr->first);
        }

        scan_cursor_list.pop_front();
    }
    return hash;
}

std::pair<bool, const std::string *> RedisConnectionContext::FindScanCursor(
    uint64_t cursor_id) const
{
    auto iter = scan_cursors.find(cursor_id);
    if (iter != scan_cursors.end())
    {
        const std::string &cursor_content = iter->second.first;
        return {true, &cursor_content};
    }
    return {false, nullptr};
}

}  // namespace EloqKV
