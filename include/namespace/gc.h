#pragma once

#include <bthread/bthread.h>

#include <string>
#include <vector>

namespace EloqKV
{

class RedisServiceImpl;

class NamespaceGc
{
public:
    explicit NamespaceGc(RedisServiceImpl *server) : server_(server)
    {
    }
    ~NamespaceGc() = default;

    void Start();
    void Stop();

private:
    static void *DaemonRoutine(void *arg);
    void RunDaemon();
    std::vector<std::string> ScanGCRecords();
    bool CleanPrefixKeys(const std::string &old_prefix);
    void DeleteGCRecord(const std::string &gc_key);
    void SleepWithStopCheck(int hundred_ms_units);

private:
    RedisServiceImpl *server_;
    bthread_t gc_tid_{};
};

}  // namespace EloqKV
