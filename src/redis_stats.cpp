#include "redis_stats.h"

#include <butil/time.h>
#include <bvar/bvar.h>

#include <cassert>

namespace EloqKV
{
// The count of total received connections
bvar::Adder<uint64_t> *conn_received_count = nullptr;
// The count of total rejected connections due to max clients
bvar::Adder<uint64_t> *conn_rejected_count = nullptr;
// The count of total closed connections.
bvar::Adder<uint64_t> *conn_closed_count = nullptr;
// The count of blocked clients
bvar::Adder<uint64_t> *blocked_clients_count = nullptr;

// The count of total readonly commands
bvar::Adder<uint64_t> *cmd_read_count = nullptr;
// The count of total write commands
bvar::Adder<uint64_t> *cmd_write_count = nullptr;
// The count of MultiObjectTxCommand
bvar::Adder<uint64_t> *multi_cmd_count = nullptr;
// The count of command per second
bvar::WindowEx<bvar::Adder<int>, 1> *cmd_pre_sec;

uint64_t RedisStats::ns_start_{0};
bool RedisStats::is_exposed_{false};

void RedisStats::ExposeBVar()
{
    assert(conn_received_count == nullptr);
    conn_received_count = new bvar::Adder<uint64_t>("conn_received_count");
    conn_rejected_count = new bvar::Adder<uint64_t>("conn_rejected_count");
    conn_closed_count = new bvar::Adder<uint64_t>("conn_closed_count");
    blocked_clients_count = new bvar::Adder<uint64_t>("blocked_clients_count");

    cmd_read_count = new bvar::Adder<uint64_t>("cmd_read_count");
    cmd_write_count = new bvar::Adder<uint64_t>("cmd_write_count");
    multi_cmd_count = new bvar::Adder<uint64_t>("multi_cmd_count");
    // cmd_pre_sec = new bvar::WindowEx<bvar::Adder<int>, 1>("cmd_pre_sec");

    ns_start_ = butil::cpuwide_time_ns();
    is_exposed_ = true;
}

void RedisStats::HideBVar()
{
    if (!is_exposed_)
    {
        return;
    }
    assert(conn_received_count != nullptr);
    delete conn_received_count;
    conn_received_count = nullptr;
    delete conn_rejected_count;
    conn_rejected_count = nullptr;
    delete conn_closed_count;
    conn_closed_count = nullptr;
    delete blocked_clients_count;
    blocked_clients_count = nullptr;

    delete cmd_read_count;
    cmd_read_count = nullptr;
    delete cmd_write_count;
    cmd_write_count = nullptr;
    delete multi_cmd_count;
    multi_cmd_count = nullptr;
    //  delete cmd_pre_sec;
    // cmd_pre_sec = nullptr;

    ns_start_ = 0;
    is_exposed_ = false;
}

void RedisStats::IncrConnReceived()
{
    if (!is_exposed_)
        return;
    (*conn_received_count) << 1;
}

void RedisStats::IncrConnRejected()
{
    if (!is_exposed_)
        return;
    (*conn_rejected_count) << 1;
}

void RedisStats::IncrConnClosed()
{
    if (!is_exposed_)
        return;
    (*conn_closed_count) << 1;
}

void RedisStats::IncrBlockClient()
{
    if (!is_exposed_)
        return;
    (*blocked_clients_count) << 1;
}

void RedisStats::DecrBlockClient()
{
    if (!is_exposed_)
        return;
    (*blocked_clients_count) << -1;
}

void RedisStats::IncrReadCommand()
{
    if (!is_exposed_)
        return;
    (*cmd_read_count) << 1;
}

void RedisStats::IncrWriteCommand()
{
    if (!is_exposed_)
        return;
    (*cmd_write_count) << 1;
}

void RedisStats::IncrMultiObjectCommand()
{
    if (!is_exposed_)
        return;
    (*multi_cmd_count) << 1;
}

void RedisStats::IncrCmdPerSec()
{
    if (!is_exposed_)
        return;
    (*cmd_pre_sec) << 1;
}

int64_t RedisStats::GetConnReceivedCount()
{
    assert(is_exposed_);
    return conn_received_count->get_value();
}

int64_t RedisStats::GetConnRejectedCount()
{
    assert(is_exposed_);
    return conn_rejected_count->get_value();
}

int64_t RedisStats::GetConnectingCount()
{
    assert(is_exposed_);
    return conn_received_count->get_value() - conn_closed_count->get_value();
}

int64_t RedisStats::GetBlockedClientsCount()
{
    assert(is_exposed_);
    return blocked_clients_count->get_value();
}

int64_t RedisStats::GetTotalCommandsCount()
{
    assert(is_exposed_);
    return cmd_read_count->get_value() + cmd_write_count->get_value() +
           multi_cmd_count->get_value();
}

int64_t RedisStats::GetReadCommandsCount()
{
    assert(is_exposed_);
    return cmd_read_count->get_value();
}

int64_t RedisStats::GetWriteCommandsCount()
{
    assert(is_exposed_);
    return cmd_write_count->get_value();
}

int64_t RedisStats::GetMultiObjectCommandsCount()
{
    assert(is_exposed_);
    return multi_cmd_count->get_value();
}

int64_t RedisStats::GetCommandsPerSecond()
{
    assert(is_exposed_);
    return cmd_pre_sec->get_value();
}
}  // namespace EloqKV
