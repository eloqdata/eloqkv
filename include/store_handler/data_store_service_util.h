#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace EloqDS
{
// Node status in DSS
enum DSShardStatus
{
    // Node status is unknown, can be dead or alive, need to check
    Unknown = 0,
    // Node is online with read only mode
    ReadOnly = 1,
    // Node is online with read write mode
    ReadWrite = 2,
    // Node is closed
    Closed = 3,
};

// TODO(liunyl): define error code

struct DSSNode
{
public:
    std::string host_name_{""};
    uint16_t port_{0};

    bool operator<(const DSSNode &other) const
    {
        // Define ordering logic (e.g., by node_id, then host_name, then port)
        if (host_name_ != other.host_name_)
            return host_name_ < other.host_name_;
        return port_ < other.port_;
    }

    bool operator==(const DSSNode &other) const
    {
        return host_name_ == other.host_name_ && port_ == other.port_;
    }

    bool operator!=(const DSSNode &other) const
    {
        return !(*this == other);
    }

    DSSNode() = default;
    DSSNode(const std::string &host_name, uint16_t port)
        : host_name_(host_name), port_(port)
    {
    }

    DSSNode(const DSSNode &other)
        : host_name_(other.host_name_), port_(other.port_)
    {
    }

    DSSNode &operator=(const DSSNode &other)
    {
        if (this != &other)
        {
            host_name_ = other.host_name_;
            port_ = other.port_;
        }
        return *this;
    }
};

// For grouping nodes within a node group, we can store them in a vector
struct DSShard
{
    DSShard() = default;
    DSShard(const DSShard &other)
        : shard_id_(other.shard_id_),
          nodes_(other.nodes_),
          status_(other.status_),
          version_(other.version_)
    {
    }

    DSShard &operator=(const DSShard &other)
    {
        if (this != &other)
        {
            shard_id_ = other.shard_id_;
            nodes_ = other.nodes_;
            status_ = other.status_;
            version_ = other.version_;
        }
        return *this;
    }

    int32_t shard_id_{0};
    // host:port pairs
    std::vector<DSSNode> nodes_;
    DSShardStatus status_{DSShardStatus::Closed};
    uint64_t version_{1};
};
}  // namespace EloqDS
