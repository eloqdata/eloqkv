#pragma once

#include <string>
#include <string_view>
#include <utility>

#include "eloq_string.h"
#include "namespace/prefix.h"

namespace EloqKV
{

constexpr std::string_view kDefaultNamespace = "default";

std::string &GetCurrentNamespace();
#define current_namespace GetCurrentNamespace()

inline std::string ComposeNamespaceKey(std::string_view ns,
                                       std::string_view key)
{
    if (ns.empty() || ns == kDefaultNamespace)
    {
        return std::string(key);
    }
    std::string ns_key;
    ns_key.reserve(ns.size() + key.size());
    ns_key.append(ns);
    ns_key.append(key);
    return ns_key;
}

inline std::string ComposeNamespaceKeyNext(std::string_view ns)
{
    if (ns.empty() || ns == kDefaultNamespace)
    {
        return "";
    }
    return NamespacePrefix::MakePrefixNext(ns);
}

inline std::string ApplyNamespace(std::string_view key)
{
    return ComposeNamespaceKey(current_namespace, key);
}

inline EloqString CreateEloqStringFromNamespace(std::string_view key)
{
    std::string ns_key = ApplyNamespace(key);
    return EloqString(ns_key.data(), ns_key.size());
}

struct NamespaceGuard
{
    std::string old_ns;
    explicit NamespaceGuard(std::string_view ns)
        : old_ns(std::move(current_namespace))
    {
        current_namespace = ns;
    }
    ~NamespaceGuard()
    {
        current_namespace = std::move(old_ns);
    }

    NamespaceGuard(const NamespaceGuard &) = delete;
    NamespaceGuard &operator=(const NamespaceGuard &) = delete;
    NamespaceGuard(NamespaceGuard &&) = delete;
    NamespaceGuard &operator=(NamespaceGuard &&) = delete;
};

}  // namespace EloqKV
