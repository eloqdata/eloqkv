#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "manager.h"

namespace EloqKV
{

constexpr std::string_view kNamespacePrefix = "n:";
constexpr std::string_view kNamespaceScanEnd = "n;";
constexpr std::string_view kTokenPrefix = "t:";
constexpr std::string_view kIdToNamePrefix = "i:";
constexpr std::string_view kEpochPrefix = "e:";
constexpr std::string_view kGcPrefix = "g:";
constexpr std::string_view kGcScanEnd = "g;";
constexpr std::string_view kNextIdKey = "next_id";

class RedisServiceImpl;
class NamespaceGc;

class NamespaceStorage : public INamespaceStorage
{
public:
    explicit NamespaceStorage(RedisServiceImpl *server);
    ~NamespaceStorage() override;

    NamespaceToken GetToken(std::string_view ns) override;
    std::string ByToken(const NamespaceToken &token,
                        std::string &ns_id,
                        uint64_t &epoch) override;
    bool Add(std::string_view ns, const NamespaceToken &token) override;
    bool Set(std::string_view ns, const NamespaceToken &token) override;
    bool Del(std::string_view ns) override;
    std::map<NamespaceToken, std::string> List() override;

    void StartGCDaemon() override;
    void StopGCDaemon() override;

private:
    RedisServiceImpl *server_;
    std::unique_ptr<NamespaceGc> gc_;
};

}  // namespace EloqKV
