#pragma once

#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "namespace/token.h"
#include "rcu.h"

namespace EloqKV
{

struct NamespaceMetadata
{
    std::string ns_name;
    std::string encoded_id;
    NamespaceToken token;
    std::atomic<uint64_t> epoch{1};
};

class INamespaceStorage
{
public:
    virtual ~INamespaceStorage() = default;
    virtual NamespaceToken GetToken(std::string_view ns) = 0;
    virtual std::string ByToken(const NamespaceToken &token,
                                std::string &ns_id,
                                uint64_t &epoch) = 0;
    virtual bool Add(std::string_view ns, const NamespaceToken &token) = 0;
    virtual bool Set(std::string_view ns, const NamespaceToken &token) = 0;
    virtual bool Del(std::string_view ns) = 0;
    virtual std::map<NamespaceToken, std::string> List() = 0;
    virtual void StartGCDaemon()
    {
    }
    virtual void StopGCDaemon()
    {
    }
};

struct StorageState
{
    std::map<NamespaceToken, std::string> token_to_ns;
    std::map<std::string, NamespaceToken, std::less<>> ns_to_token;
    std::map<std::string, std::string, std::less<>> ns_to_id;
    uint64_t next_id{2};
};

class MemoryNamespaceStorage : public INamespaceStorage
{
public:
    MemoryNamespaceStorage() = default;
    ~MemoryNamespaceStorage() override = default;

    bool Add(std::string_view ns, const NamespaceToken &token) override;
    bool Set(std::string_view ns, const NamespaceToken &token) override;
    bool Del(std::string_view ns) override;
    NamespaceToken GetToken(std::string_view ns) override;
    std::string ByToken(const NamespaceToken &token,
                        std::string &ns_id,
                        uint64_t &epoch) override;
    std::map<NamespaceToken, std::string> List() override;

private:
    Rcu<StorageState> rcu_state_;
};

struct CacheState
{
    std::unordered_map<std::string, std::shared_ptr<NamespaceMetadata>>
        ns_metadata;
    std::map<NamespaceToken, std::shared_ptr<NamespaceMetadata>> token_metadata;
};

class NamespaceManager
{
public:
    NamespaceManager() = default;
    explicit NamespaceManager(std::unique_ptr<INamespaceStorage> storage);
    ~NamespaceManager() = default;

    NamespaceManager(const NamespaceManager &) = delete;
    NamespaceManager &operator=(const NamespaceManager &) = delete;
    NamespaceManager(NamespaceManager &&) = delete;
    NamespaceManager &operator=(NamespaceManager &&) = delete;

    bool Add(std::string_view ns, const NamespaceToken &token);
    bool Set(std::string_view ns, const NamespaceToken &token);
    bool Del(std::string_view ns);
    std::string Get(std::string_view ns) const;
    std::string GetByToken(const NamespaceToken &token) const;
    std::string GetByToken(const NamespaceToken &token,
                           std::string &ns_id) const;
    std::map<std::string, std::string, std::less<>> List() const;

    std::shared_ptr<NamespaceMetadata> GetMetadataByToken(
        const NamespaceToken &token) const;
    void RemoveMetadata(std::string_view ns_name);

    void StartGCDaemon()
    {
        if (storage_)
            storage_->StartGCDaemon();
    }
    void StopGCDaemon()
    {
        if (storage_)
            storage_->StopGCDaemon();
    }

private:
    std::unique_ptr<INamespaceStorage> storage_;
    mutable Rcu<CacheState> rcu_state_;
};

}  // namespace EloqKV
