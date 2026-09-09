#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <string>
#include <string_view>

#include "pub_sub_manager.h"
#include "redis_connection_context.h"

namespace
{
// No listener or database is needed: the production PubSubManager and
// FlushOutput write to a real nonblocking brpc socket, including queued writes.
class Subscriber
{
public:
    Subscriber()
    {
        int fds[2];
        REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds) == 0);
        peer_ = fds[0];
        brpc::SocketOptions options;
        options.fd = fds[1];
        brpc::SocketId id;
        REQUIRE(brpc::Socket::Create(options, &id) == 0);
        REQUIRE(brpc::Socket::Address(id, &socket_) == 0);
        context.socket = socket_.get();
        context.pub_sub_mgr = &manager;
    }

    ~Subscriber()
    {
        // Remove the manager's borrowed pointer before releasing the socket.
        manager.UnsubscribeAll(&context);
        context.subscribed_channels.clear();
        context.subscribed_patterns.clear();
        socket_->SetFailed();
        close(peer_);
    }

    void ExpectBytes(const std::string &expected)
    {
        std::string received;
        while (received.size() < expected.size())
        {
            pollfd fd{peer_, POLLIN, 0};
            int ready;
            do
            {
                ready = poll(&fd, 1, 5000);
            } while (ready < 0 && errno == EINTR);
            REQUIRE(ready == 1);
            char bytes[16384];
            ssize_t count = read(peer_, bytes, sizeof(bytes));
            if (count < 0 && (errno == EAGAIN || errno == EINTR))
            {
                continue;
            }
            REQUIRE(count > 0);
            received.append(bytes, count);
        }
        REQUIRE(received == expected);
    }

    EloqKV::PubSubManager manager;
    EloqKV::RedisConnectionContext context;

private:
    int peer_{-1};
    brpc::SocketUniquePtr socket_;
};

std::string Bulk(std::string_view value)
{
    return "$" + std::to_string(value.size()) + "\r\n" + std::string(value) +
           "\r\n";
}
}  // namespace

TEST_CASE("Pubsub replies survive source cleanup and connection reuse",
          "[pubsub][memory]")
{
    Subscriber subscriber;
    auto &context = subscriber.context;
    subscriber.manager.Subscribe({"channel"}, &context);
    subscriber.ExpectBytes("*3\r\n" + Bulk("subscribe") + Bulk("channel") +
                           ":1\r\n");
    REQUIRE(context.output.is_nil());

    subscriber.manager.PSubscribe({"chan*"}, &context);
    subscriber.ExpectBytes("*3\r\n" + Bulk("psubscribe") + Bulk("chan*") +
                           ":2\r\n");
    REQUIRE(context.output.is_nil());

    // Each publish emits two replies. The first reply's source arena is
    // reused while its large serialized payload may still be queued.
    std::string payload(1024 * 1024, 'x');
    payload[27] = '\0';
    for (int iteration = 0; iteration < 32; ++iteration)
    {
        payload.back() = 'a' + iteration % 26;
        REQUIRE(subscriber.manager.Publish("channel", payload) == 2);
        REQUIRE(context.output.is_nil());
        subscriber.ExpectBytes("*3\r\n" + Bulk("message") + Bulk("channel") +
                               Bulk(payload) + "*4\r\n" + Bulk("pmessage") +
                               Bulk("chan*") + Bulk("channel") + Bulk(payload));
    }

    subscriber.manager.Unsubscribe({"channel"}, &context);
    subscriber.ExpectBytes("*3\r\n" + Bulk("unsubscribe") + Bulk("channel") +
                           ":1\r\n");
    subscriber.manager.PUnsubscribe({"chan*"}, &context);
    subscriber.ExpectBytes("*3\r\n" + Bulk("punsubscribe") + Bulk("chan*") +
                           ":0\r\n");
    REQUIRE(context.output.is_nil());
}

TEST_CASE("Failed pubsub writes discard their source reply", "[pubsub][memory]")
{
    Subscriber subscriber;
    auto &context = subscriber.context;
    REQUIRE(context.socket->SetFailed() == 0);
    for (int iteration = 0; iteration < 4; ++iteration)
    {
        context.output.SetArray(3);
        context.output[0].SetString("message");
        context.output[1].SetString("channel");
        context.output[2].SetString(std::string(1024 * 1024, 'x'));
        REQUIRE_FALSE(context.FlushOutput());
        REQUIRE(context.output.is_nil());
    }
}
