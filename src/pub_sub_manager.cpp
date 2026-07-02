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
#include "pub_sub_manager.h"

#include <vector>

#include "absl/strings/string_view.h"
#include "redis_string_match.h"

namespace EloqKV
{
namespace
{
absl::string_view ToAbslStringView(std::string_view value)
{
    return absl::string_view(value.data(), value.size());
}
}  // namespace

void PubSubManager::Subscribe(const std::vector<std::string_view> &chans,
                              EloqKV::RedisConnectionContext *client)
{
    std::unique_lock lk(pub_sub_mu_);
    for (auto chan : chans)
    {
        SubscribeChannel(chan, client);

        brpc::RedisReply *output = client->GetOutput();
        output->SetArray(3);
        (*output)[0].SetString("subscribe");
        (*output)[1].SetString(butil::StringPiece(chan.data(), chan.size()));
        (*output)[2].SetInteger(client->SubscriptionsCount());

        client->FlushOutput();

        DLOG(INFO) << "client: " << client << " subscribed to chan: " << chan
                   << ", total subscribed: " << client->SubscriptionsCount();
    }
}

/**
 * Require that lock has been acquired.
 * @param chan
 * @param client
 */
bool PubSubManager::SubscribeChannel(std::string_view chan,
                                     EloqKV::RedisConnectionContext *client)
{
    auto [channel_it, _] = pub_sub_channels_.try_emplace(std::string(chan));
    // clients that subscribed to `channel`
    absl::flat_hash_set<RedisConnectionContext *> &clients = channel_it->second;
    bool success = clients.emplace(client).second;
    if (success)
    {
        // insert the channel into client's subscribed_channels
        client->SubscribeChannel(chan);
    }
    return success;
}

void PubSubManager::Unsubscribe(const std::vector<std::string_view> &chans,
                                EloqKV::RedisConnectionContext *client)
{
    std::unique_lock lk(pub_sub_mu_);
    if (chans.empty())
    {
        int unsubscribed = 0;
        // unsubscribe from all the channels
        for (auto &[chan, clients] : pub_sub_channels_)
        {
            auto cli_it = clients.find(client);
            if (cli_it != clients.end())
            {
                DLOG(INFO) << "unsubscribe client: " << client
                           << " from chan: " << chan
                           << ", socket: " << client->socket;

                clients.erase(client);
                // remove the channel from client's subscribed_channels
                client->UnsubscribeChannel(chan);

                // write response into client
                brpc::RedisReply *output = client->GetOutput();
                output->SetArray(3);
                (*output)[0].SetString("unsubscribe");
                (*output)[1].SetString(
                    butil::StringPiece(chan.data(), chan.size()));
                (*output)[2].SetInteger(client->SubscriptionsCount());

                client->FlushOutput();

                unsubscribed++;
            }
        }

        if (unsubscribed == 0)
        {
            // write an empty response into client
            brpc::RedisReply *output = client->GetOutput();
            output->SetArray(3);
            (*output)[0].SetString("unsubscribe");
            (*output)[1].SetNullString();
            (*output)[2].SetInteger(client->SubscriptionsCount());

            client->FlushOutput();
        }

        // remove the channel if no client is subscribed to it
        absl::erase_if(pub_sub_channels_,
                       [](const auto &item)
                       {
                           if (item.second.empty())
                           {
                               DLOG(INFO) << "erase channel: " << item.first;
                           }
                           return item.second.empty();
                       });
        DLOG(INFO) << "after unsubscribe, channel size: "
                   << pub_sub_channels_.size();
    }
    else
    {
        for (auto chan : chans)
        {
            UnsubscribeChannel(chan, client);

            // write response into client
            brpc::RedisReply *output = client->GetOutput();
            output->SetArray(3);
            (*output)[0].SetString("unsubscribe");
            (*output)[1].SetString(
                butil::StringPiece(chan.data(), chan.size()));
            (*output)[2].SetInteger(client->SubscriptionsCount());

            client->FlushOutput();
        }
    }
}

/**
 * Require that lock has been acquired.
 * @param chan
 * @param client
 * @return
 */
bool PubSubManager::UnsubscribeChannel(std::string_view chan,
                                       EloqKV::RedisConnectionContext *client)
{
    // unsubscribe from the specified channel
    auto it = pub_sub_channels_.find(ToAbslStringView(chan));
    if (it != pub_sub_channels_.end())
    {
        auto &clients = it->second;

        if (clients.find(client) != clients.end())
        {
            DLOG(INFO) << "unsubscribe subscriber: " << client
                       << " from chan: " << chan
                       << ", socket: " << client->socket;

            clients.erase(client);

            // remove the channel if no subscriber is subscribed to it
            if (clients.empty())
            {
                DLOG(INFO) << "erase channel: " << it->first;
                pub_sub_channels_.erase(it);
            }

            // remove the channel from client's subscribed_channels
            client->UnsubscribeChannel(chan);

            return true;
        }
    }
    DLOG(INFO) << "client: " << client << " not subscribed to channel: " << chan
               << ", do nothing";
    return false;
}

void PubSubManager::UnsubscribeAll(EloqKV::RedisConnectionContext *client)
{
    std::unique_lock lk(pub_sub_mu_);
    for (auto &[chan, clients] : pub_sub_channels_)
    {
        DLOG(INFO) << "erase client: " << client << " from channel: " << chan
                   << ", socket: " << client->socket;
        clients.erase(client);
    }
    absl::erase_if(pub_sub_channels_,
                   [](const auto &item)
                   {
                       if (item.second.empty())
                       {
                           DLOG(INFO) << "erase channel: " << item.first;
                       }
                       return item.second.empty();
                   });
    DLOG(INFO) << "after unsubscribe, channel size: "
               << pub_sub_channels_.size();

    for (auto &[pattern, clients] : pattern_subs_)
    {
        DLOG(INFO) << "erase client: " << client << " from pattern: " << pattern
                   << ", socket: " << client->socket;
        clients.erase(client);
    }
    absl::erase_if(pattern_subs_,
                   [](const auto &item)
                   {
                       if (item.second.empty())
                       {
                           DLOG(INFO) << "erase pattern: " << item.first;
                       }
                       return item.second.empty();
                   });
    DLOG(INFO) << "after unsubscribe, pattern size: " << pattern_subs_.size();
}

void PubSubManager::PSubscribe(const std::vector<std::string_view> &patterns,
                               EloqKV::RedisConnectionContext *client)
{
    std::unique_lock lk(pub_sub_mu_);
    for (auto pattern : patterns)
    {
        SubscribePattern(pattern, client);

        brpc::RedisReply *output = client->GetOutput();
        output->SetArray(3);
        (*output)[0].SetString("psubscribe");
        (*output)[1].SetString(
            butil::StringPiece(pattern.data(), pattern.size()));
        (*output)[2].SetInteger(client->SubscriptionsCount());

        client->FlushOutput();

        DLOG(INFO) << "client: " << client
                   << " subscribed to pattern: " << pattern
                   << ", total subscribed: " << client->SubscriptionsCount();
    }
}

/**
 * Require that lock has been acquired.
 * @param pattern
 * @param client
 * @return
 */
bool PubSubManager::SubscribePattern(std::string_view pattern,
                                     EloqKV::RedisConnectionContext *client)
{
    auto [pattern_it, _] = pattern_subs_.try_emplace(std::string(pattern));
    // clients that subscribed to `pattern`
    absl::flat_hash_set<RedisConnectionContext *> &clients = pattern_it->second;
    bool success = clients.emplace(client).second;
    if (success)
    {
        // insert the pattern into client's subscribed_patterns
        client->SubscribePattern(pattern);
    }
    return success;
}

void PubSubManager::PUnsubscribe(const std::vector<std::string_view> &patterns,
                                 EloqKV::RedisConnectionContext *client)
{
    std::unique_lock lk(pub_sub_mu_);
    if (patterns.empty())
    {
        int unsubscribed = 0;
        // unsubscribe from all the channels
        for (auto &[pattern, clients] : pattern_subs_)
        {
            auto cli_it = clients.find(client);
            if (cli_it != clients.end())
            {
                DLOG(INFO) << "punsubscribe client: " << client
                           << " from pattern: " << pattern
                           << ", socket: " << client->socket;

                clients.erase(client);
                // remove the channel from client's subscribed_patterns
                client->UnsubscribePattern(pattern);

                // write response into client
                brpc::RedisReply *output = client->GetOutput();
                output->SetArray(3);
                (*output)[0].SetString("punsubscribe");
                (*output)[1].SetString(
                    butil::StringPiece(pattern.data(), pattern.size()));
                (*output)[2].SetInteger(client->SubscriptionsCount());

                client->FlushOutput();

                unsubscribed++;
            }
        }

        if (unsubscribed == 0)
        {
            // write an empty response into client
            brpc::RedisReply *output = client->GetOutput();
            output->SetArray(3);
            (*output)[0].SetString("punsubscribe");
            (*output)[1].SetNullString();
            (*output)[2].SetInteger(client->SubscriptionsCount());

            client->FlushOutput();
        }

        // remove the channel if no client is subscribed to it
        absl::erase_if(pattern_subs_,
                       [](const auto &item)
                       {
                           if (item.second.empty())
                           {
                               DLOG(INFO) << "erase pattern: " << item.first;
                           }
                           return item.second.empty();
                       });
        DLOG(INFO) << "after punsubscribe, pattern size: "
                   << pattern_subs_.size();
    }
    else
    {
        for (auto pattern : patterns)
        {
            UnsubscribePattern(pattern, client);

            // write response into client
            brpc::RedisReply *output = client->GetOutput();
            output->SetArray(3);
            (*output)[0].SetString("punsubscribe");
            (*output)[1].SetString(
                butil::StringPiece(pattern.data(), pattern.size()));
            (*output)[2].SetInteger(client->SubscriptionsCount());

            client->FlushOutput();
        }
    }
}

/**
 * Require that lock has been acquired.
 * @param pattern
 * @param client
 * @return
 */
bool PubSubManager::UnsubscribePattern(std::string_view pattern,
                                       EloqKV::RedisConnectionContext *client)
{
    // unsubscribe from the specified pattern
    auto it = pattern_subs_.find(ToAbslStringView(pattern));
    if (it != pattern_subs_.end())
    {
        auto &clients = it->second;

        if (clients.find(client) != clients.end())
        {
            DLOG(INFO) << "unsubscribe subscriber: " << client
                       << " from pattern: " << pattern
                       << ", socket: " << client->socket;

            clients.erase(client);

            // remove the channel if no subscriber is subscribed to it
            if (clients.empty())
            {
                DLOG(INFO) << "erase pattern: " << it->first;
                pattern_subs_.erase(it);
            }

            // remove the channel from client's subscribed_channels
            client->UnsubscribePattern(pattern);

            return true;
        }
    }
    DLOG(INFO) << "client: " << client
               << " not subscribed to pattern: " << pattern << ", do nothing";
    return false;
}

int PubSubManager::Publish(std::string_view chan, std::string_view msg)
{
    std::unique_lock lk(pub_sub_mu_);
    int received = 0;
    auto it = pub_sub_channels_.find(ToAbslStringView(chan));
    if (it != pub_sub_channels_.end())
    {
        auto clients = it->second;
        for (auto &client : clients)
        {
            DLOG(INFO) << "publish message: " << msg
                       << " through channel: " << chan
                       << ", to client: " << client;
            // write message into client
            brpc::RedisReply *output = client->GetOutput();
            output->SetArray(3);
            (*output)[0].SetString("message");
            (*output)[1].SetString(
                butil::StringPiece(chan.data(), chan.size()));
            (*output)[2].SetString(butil::StringPiece(msg.data(), msg.size()));

            client->FlushOutput();
            received++;
        }
    }
    // check pattern subscriptions
    for (auto &[pattern, clients] : pattern_subs_)
    {
        if (stringmatchlen(
                pattern.data(), pattern.size(), chan.data(), chan.size(), 0))
        {
            for (auto client : clients)
            {
                DLOG(INFO) << "publish message: " << msg
                           << " through channel: " << chan
                           << ", by pattern: " << pattern
                           << ", to client: " << client;
                // write message into client
                brpc::RedisReply *output = client->GetOutput();
                output->SetArray(4);
                (*output)[0].SetString("pmessage");
                (*output)[1].SetString(
                    butil::StringPiece(pattern.data(), pattern.size()));
                (*output)[2].SetString(
                    butil::StringPiece(chan.data(), chan.size()));
                (*output)[3].SetString(
                    butil::StringPiece(msg.data(), msg.size()));

                client->FlushOutput();
                received++;
            }
        }
    }
    return received;
}

}  // namespace EloqKV
