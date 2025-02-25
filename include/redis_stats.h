#pragma once
#include <cstdint>

namespace EloqKV
{
class RedisStats
{
public:
    static void ExposeBVar();
    static void HideBVar();

    static void IncrConnReceived();
    static void IncrConnRejected();
    static void IncrConnClosed();
    static void IncrBlockClient();
    static void DecrBlockClient();

    static void IncrReadCommand();
    static void IncrWriteCommand();
    static void IncrMultiObjectCommand();
    static void IncrCmdPerSec();

    static int64_t GetConnReceivedCount();
    static int64_t GetConnRejectedCount();
    static int64_t GetConnectingCount();
    static int64_t GetBlockedClientsCount();

    static int64_t GetTotalCommandsCount();
    static int64_t GetReadCommandsCount();
    static int64_t GetWriteCommandsCount();
    static int64_t GetMultiObjectCommandsCount();
    static int64_t GetCommandsPerSecond();

    static uint64_t GetStartNs()
    {
        return ns_start_;
    }

protected:
    static uint64_t ns_start_;
    static bool is_exposed_;
};
}  // namespace EloqKV
