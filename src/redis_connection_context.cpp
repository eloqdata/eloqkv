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
#include "redis_connection_context.h"

#include <list>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "redis_command.h"
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

uint64_t RedisConnectionContext::CreateBucketScanCursor(
    std::string_view cursor_content,
    std::unique_ptr<txservice::BucketScanSavePoint> save_point)
{
    // FNV-1a hash algorithm.
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < cursor_content.size(); ++i)
    {
        hash ^= cursor_content[i];
        hash *= 1099511628211ULL;
    }

    auto [iter, inserted] =
        bucket_scan_cursors.try_emplace(db_id, hash, nullptr);
    if (inserted)
    {
        iter->second.second = std::move(save_point);
    }
    else
    {
        // update
        iter->second.first = hash;
        iter->second.second = std::move(save_point);
    }

    LOG(INFO) << "==CreateBucketScanCursor: new cursor id = " << hash
              << ", save point addr = " << iter->second.second.get()
              << ", cursors size = " << bucket_scan_cursors.size()
              << ", db id = " << db_id << ", this = this";

    return hash;
}

void RedisConnectionContext::RemoveBucketScanCursor()
{
    bucket_scan_cursors.erase(db_id);
}

uint64_t RedisConnectionContext::UpdateBucketScanCursor(
    std::string_view cursor_content)
{
    // FNV-1a hash algorithm.
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < cursor_content.size(); ++i)
    {
        hash ^= cursor_content[i];
        hash *= 1099511628211ULL;
    }

    bucket_scan_cursors.at(db_id).first = hash;
    return hash;
}

BucketScanSavePoint *RedisConnectionContext::FindBucketScanCursor(
    uint64_t cursor_id)
{
    auto iter = bucket_scan_cursors.find(db_id);
    if (iter == bucket_scan_cursors.end())
    {
        LOG(INFO) << "== FindBucketScanCursor: db id not found, db id = "
                  << db_id << ", bucket_scan_cursor size = "
                  << bucket_scan_cursors.size() << ", this = " << this;
        return nullptr;
    }

    if (iter->second.first != cursor_id)
    {
        LOG(INFO) << "== FindBucketScanCursor: cursor id mismatch";
        return nullptr;
    }

    return iter->second.second.get();
}

}  // namespace EloqKV
