#include "namespace/manager.h"

#include "b255.h"
#include "token.h"

namespace EloqKV
{

// ==========================================
// MemoryNamespaceStorage Implementation
// ==========================================

bool MemoryNamespaceStorage::Add(std::string_view ns,
                                 const NamespaceToken &token)
{
    return rcu_state_.Update(
        [&](StorageState &new_state) -> bool
        {
            auto it_ns = new_state.ns_to_token.find(ns);
            if (it_ns != new_state.ns_to_token.end())
            {
                return it_ns->second == token;
            }
            if (new_state.token_to_ns.find(token) !=
                new_state.token_to_ns.end())
            {
                return false;
            }
            new_state.token_to_ns.emplace(token, ns);
            new_state.ns_to_token.emplace(ns, token);
            uint64_t id = ++new_state.next_id;
            new_state.ns_to_id.emplace(ns, b255prefix(id));
            return true;
        });
}

bool MemoryNamespaceStorage::Set(std::string_view ns,
                                 const NamespaceToken &token)
{
    return rcu_state_.Update(
        [&](StorageState &new_state) -> bool
        {
            auto it_token = new_state.token_to_ns.find(token);
            if (it_token != new_state.token_to_ns.end() &&
                it_token->second != ns)
            {
                return false;
            }

            auto it_ns = new_state.ns_to_token.find(ns);
            if (it_ns != new_state.ns_to_token.end())
            {
                if (it_ns->second == token)
                {
                    return true;
                }
                new_state.token_to_ns.erase(it_ns->second);
                new_state.ns_to_token[std::string(ns)] = token;
                new_state.token_to_ns.emplace(token, ns);
                return true;
            }

            new_state.token_to_ns.emplace(token, ns);
            new_state.ns_to_token.emplace(ns, token);
            uint64_t id = ++new_state.next_id;
            new_state.ns_to_id.emplace(ns, b255prefix(id));
            return true;
        });
}

bool MemoryNamespaceStorage::Del(std::string_view ns)
{
    return rcu_state_.Update(
        [&](StorageState &new_state) -> bool
        {
            auto it_ns = new_state.ns_to_token.find(ns);
            if (it_ns != new_state.ns_to_token.end())
            {
                new_state.token_to_ns.erase(it_ns->second);
                new_state.ns_to_id.erase(std::string(ns));
                new_state.ns_to_token.erase(std::string(ns));
                return true;
            }
            return false;
        });
}

NamespaceToken MemoryNamespaceStorage::GetToken(std::string_view ns)
{
    auto current_state = rcu_state_.Read();
    if (current_state)
    {
        auto it = current_state->ns_to_token.find(ns);
        if (it != current_state->ns_to_token.end())
        {
            return it->second;
        }
    }
    return NamespaceToken();
}

std::string MemoryNamespaceStorage::ByToken(const NamespaceToken &token,
                                            std::string &ns_id,
                                            uint64_t &epoch)
{
    ns_id = "";
    epoch = 1;
    auto current_state = rcu_state_.Read();
    if (current_state)
    {
        auto it = current_state->token_to_ns.find(token);
        if (it != current_state->token_to_ns.end())
        {
            auto it_id = current_state->ns_to_id.find(it->second);
            if (it_id != current_state->ns_to_id.end())
            {
                ns_id = it_id->second;
            }
            return it->second;
        }
    }
    return "";
}

std::map<NamespaceToken, std::string> MemoryNamespaceStorage::List()
{
    auto current_state = rcu_state_.Read();
    if (current_state)
    {
        return current_state->token_to_ns;
    }
    return {};
}

// ==========================================
// NamespaceManager Implementation
// ==========================================

NamespaceManager::NamespaceManager(std::unique_ptr<INamespaceStorage> storage)
    : storage_(std::move(storage))
{
}

bool NamespaceManager::Add(std::string_view ns, const NamespaceToken &token)
{
    if (storage_)
    {
        return storage_->Add(ns, token);
    }
    return false;
}

bool NamespaceManager::Set(std::string_view ns, const NamespaceToken &token)
{
    if (storage_)
    {
        bool ok = storage_->Set(ns, token);
        if (ok)
        {
            RemoveMetadata(ns);
        }
        return ok;
    }
    return false;
}

bool NamespaceManager::Del(std::string_view ns)
{
    if (storage_)
    {
        bool ok = storage_->Del(ns);
        if (ok)
        {
            RemoveMetadata(ns);
        }
        return ok;
    }
    return false;
}

std::string NamespaceManager::Get(std::string_view ns) const
{
    if (storage_)
    {
        return storage_->GetToken(ns).ToString();
    }
    return "";
}

std::string NamespaceManager::GetByToken(const NamespaceToken &token) const
{
    std::string ns_id;
    return GetByToken(token, ns_id);
}

std::string NamespaceManager::GetByToken(const NamespaceToken &token,
                                         std::string &ns_id) const
{
    if (storage_)
    {
        uint64_t dummy_epoch = 1;
        return storage_->ByToken(token, ns_id, dummy_epoch);
    }
    ns_id = "";
    return "";
}

std::map<std::string, std::string, std::less<>> NamespaceManager::List() const
{
    std::map<std::string, std::string, std::less<>> result;
    if (storage_)
    {
        for (auto &pair : storage_->List())
        {
            result.emplace(pair.first.ToString(), pair.second);
        }
    }
    return result;
}

std::shared_ptr<NamespaceMetadata> NamespaceManager::GetMetadataByToken(
    const NamespaceToken &token) const
{
    auto current_state = rcu_state_.Read();
    if (current_state)
    {
        auto it = current_state->token_metadata.find(token);
        if (it != current_state->token_metadata.end())
        {
            return it->second;
        }
    }

    if (storage_)
    {
        std::string ns_id;
        uint64_t epoch = 1;
        std::string ns_name = storage_->ByToken(token, ns_id, epoch);
        if (ns_name.empty())
        {
            return nullptr;
        }

        return rcu_state_.Update(
            [&](CacheState &new_state) -> std::shared_ptr<NamespaceMetadata>
            {
                auto it = new_state.token_metadata.find(token);
                if (it != new_state.token_metadata.end())
                {
                    return it->second;
                }

                auto meta = std::make_shared<NamespaceMetadata>();
                meta->ns_name = ns_name;
                meta->encoded_id = ns_id;
                meta->token = token;
                meta->epoch.store(epoch);

                new_state.token_metadata[token] = meta;
                new_state.ns_metadata[ns_name] = meta;
                return meta;
            });
    }

    return nullptr;
}

void NamespaceManager::RemoveMetadata(std::string_view ns_name)
{
    rcu_state_.Update(
        [&](CacheState &new_state)
        {
            auto it = new_state.ns_metadata.find(std::string(ns_name));
            if (it != new_state.ns_metadata.end())
            {
                new_state.token_metadata.erase(it->second->token);
                new_state.ns_metadata.erase(it);
            }
        });
}

}  // namespace EloqKV
