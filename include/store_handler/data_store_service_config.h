#pragma once
/**
 * @file data_store_service_config.h
 * @brief Configuration file for the Data Store Service.
 *
 * This file is in the INI format and includes the following sections:
 * - version: Indicates the version of the configuration file format.
 * - shard: Defines one or more shards.
 * - node_list: Defines the list of nodes for each node group, where each node
 *   entry includes a host/port in a single string (e.g., 127.0.0.1:7000).
 * - sharding: Defines the algorithm (e.g., hashing, prefix-based) that maps
 *   arbitrary keys (of any length) to a node group.
 *
 * The exact mapping mechanism depends on the specified algorithm
 * in the `sharding` section, along with any parameters needed for it.
 */

/*
;------------------------------------------------------------------
; Example configuration file: data_store_service_config.ini
;------------------------------------------------------------------

[version]
topology_version=1

[this_node]
; the node
node=127.0.0.1:16386

[shard]
; Number of shards
shard_count=2

[node_list_shard0]
shard_status=read_write
; Node list for the first shard
node0=127.0.0.1:16386
node1=127.0.0.2:16386

[node_list_shard1]
shard_status=read_only
; Node list for the second shard
node0=192.168.1.10:16386
node1=192.168.1.11:16386

[sharding]
; Defines how keys are mapped to node groups.
; Example options: "consistent-hashing", "prefix-based", "range-based", etc.
algorithm=consistent-hashing

; Example parameters for a consistent hashing approach
hash_function=murmur
virtual_node_count=128

; If you use a different approach, for example prefix-based:
;   algorithm=prefix-based
;   prefix_map=user:0,cart:1,session:0
;
; For a range-based approach, you might define numeric or string boundaries
here.
*/

#include <brpc/channel.h>

#include <cassert>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#include "INIReader.h"
#include "data_store_service_util.h"
#include "ds_request.pb.h"

namespace EloqDS
{

class ShardingAlgorithm
{
public:
    ShardingAlgorithm() = default;
    virtual ~ShardingAlgorithm() = default;

    virtual uint32_t KeyToShardId(
        const std::string &key, const std::map<uint32_t, DSShard> &shards) const
    {
        return 0;
    }

    virtual void Load(const INIReader &reader);

    virtual void Save(std::ofstream &ofs) const;

    std::string GetName() const
    {
        return name_;
    }

private:
    std::string name_{"hash-key"};
};

class Topology
{
public:
    Topology() = default;
    ~Topology() = default;

    /**
     * @brief Copy constructor for Topology.
     * @param topology The Topology object to copy from.
     */
    Topology(const Topology &topology)
        : shard_count_(topology.shard_count_),
          shards_(topology.shards_),
          topology_version_(topology.topology_version_)
    {
    }

    /**
     * @brief Copy assignment operator for Topology.
     * @param topology The Topology object to copy from.
     * @return Reference to this object.
     */
    Topology &operator=(const Topology &topology)
    {
        if (this == &topology)
        {
            return *this;
        }

        shard_count_ = topology.shard_count_;
        shards_ = topology.shards_;
        topology_version_ = topology.topology_version_;

        return *this;
    }

    /**
     * @brief Initialize the topology with a single node.
     * @param node The node to initialize with.
     */
    void InitWithSingleNode(const DSSNode &node);

    /**
     * @brief Load the topology from the specified INI file.
     * @param reader The INIReader object.
     */
    void Load(const INIReader &reader);

    /**
     * @brief Save the topology to the specified output stream.
     * @param ofs The output stream to save to.
     */
    void Save(std::ofstream &ofs) const;

    /**
     * @brief Update the topology with the specified shards and version.
     * @param shards_map The map of shards.
     * @param topology_version The version of the topology.
     */
    void Update(const std::map<uint32_t, DSShard> &shards_map,
                uint64_t topology_version);

    /**
     * @brief Fetch the shard status for the specified node group.
     * @param shard_id The node group ID.
     * @return The shard status.
     */
    DSShardStatus FetchDSShardStatus(uint32_t shard_id) const;

    /**
     * @brief Update the shard status for the specified node group.
     * @param shard_id The node group ID.
     * @param status The new shard status.
     */
    void UpdateDSShardStatus(uint32_t shard_id, DSShardStatus status);

    /**
     * @brief Get the number of shards.
     * @return The number of shards.
     */
    int GetShardCount() const;

    /**
     * @brief Returns all shards
     *        Each node has a host string and port integer.
     */
    const std::map<uint32_t, DSShard> &GetAllShards() const;

    /**
     * @brief Returns the primary node of a specific group index.
     * @param groupIndex The group index.
     * @return The primary node of the group.
     */
    const DSSNode &GetPrimaryNode(uint32_t groupIndex) const;

    /**
     * @brief Returns the shard for a specific shard index.
     * @param shard_id The shard index.
     * @return The shard.
     */
    const DSShard &GetShard(uint32_t shard_id) const;

    /**
     * @brief Returns the node groups for a node.
     * @param host The host of the node.
     * @param port The port of the node.
     * @return The shards for the node.
     */
    std::vector<uint32_t> GetShardsForNode(const DSSNode &node) const;

    /**
     * @brief Update the node group with the specified node.
     */
    void UpdatePrimaryNode(uint32_t shard_id, const DSSNode &node);

    /**
     * @brief Check if this node is the owner of the node group.
     */
    bool IsOwnerOfShard(int shard_id);

    /**
     * @brief Get the topology version.
     */
    uint64_t GetTopologyVersion() const
    {
        return topology_version_;
    }

    /**
     * @brief Update the shard members for the specified shard.
     * @param shard_id The shard ID.
     * @param shard_version The shard version.
     * @param nodes The new shard members.
     */
    bool UpdateShardMembers(uint32_t shard_id,
                            uint64_t shard_version,
                            std::vector<DSSNode> &nodes);

    /**
     * @brief Add a shared member to the specified shard.
     * @param shard_id The shard ID.
     * @param new_node The new node to add.
     */
    void AddShardMember(uint32_t shard_id, const DSSNode &new_node);

    /**
     * @brief Remove the specified shard member.
     * @param shard_id The shard ID.
     * @param node The node to remove.
     */
    void RemoveShardMember(uint32_t shard_id, const DSSNode &node);

    /**
     * @brief Get the shard version for the specified shard.
     * @param shard_id The shard ID.
     * @return The shard version.
     */
    uint64_t FetchDSShardVersion(uint32_t shard_id) const;

    /**
     * @brief Update the shard version for the specified shard.
     * @param shard_id The shard ID.
     * @param version The new shard version.
     */
    void UpdateDSShardVersion(uint32_t shard_id, uint64_t version);

    /**
     * @brief Parse a "host:port" string.
     * @param host_port The host:port string.
     * @return The parsed DSSNode object.
     */
    DSSNode ParseHostPort(const std::string &host_port) const;

private:
    // Fields parsed from the INI file
    int shard_count_{0};
    std::map<uint32_t, DSShard> shards_;
    uint64_t topology_version_{1};
};

class DataStoreServiceClusterManager
{
public:
    DataStoreServiceClusterManager() = default;

    /**
     * @brief Copy constructor for DataStoreServiceConfig.
     * @param config The DataStoreServiceConfig object to copy from.
     */
    DataStoreServiceClusterManager(const DataStoreServiceClusterManager &config)
        : this_node_(config.this_node_),
          topology_(config.topology_),
          sharding_algorithm_(config.sharding_algorithm_
                                  ? CreateShardingAlgorithm(
                                        config.sharding_algorithm_->GetName())
                                  : nullptr)
    {
    }

    /**
     * @brief Copy assignment operator for DataStoreServiceConfig.
     * @param config The DataStoreServiceConfig object to copy from.
     * @return Reference to this object.
     */
    DataStoreServiceClusterManager &operator=(
        const DataStoreServiceClusterManager &config)
    {
        if (this == &config)
        {
            return *this;
        }

        std::unique_lock<std::shared_mutex> my_lk(mutex_);
        std::shared_lock<std::shared_mutex> other_lk(config.mutex_);
        this_node_ = config.this_node_;
        topology_ = config.topology_;
        sharding_algorithm_ =
            config.sharding_algorithm_
                ? CreateShardingAlgorithm(config.sharding_algorithm_->GetName())
                : nullptr;
        return *this;
    }

    /**
     * @brief Initialize the data store service config.
     * @param local_ip Local IP address.
     * @param local_service_port Local service port.
     */
    void Initialize(const std::string &local_ip, int local_service_port);

    /**
     * @brief Loads the configuration from the specified INI file.
     * @param filename Path to the .ini file.
     * @return True on success, false on failure.
     */
    bool Load(const std::string &filename);

    /**
     * @brief Saves the current configuration state to the specified INI file.
     * @param filename Path to the .ini file to save.
     * @return True on success, false on failure.
     */
    bool Save(const std::string &filename) const;

    void Update(const std::string &sharding_algorithm,
                const std::map<uint32_t, DSShard> &ng_nodes_map,
                uint64_t topology_version);

    DSShardStatus FetchDSShardStatus(uint32_t shard_id) const;
    void UpdateDSShardStatus(uint32_t shard_id, DSShardStatus status);

    /**
     * @return Number of shards (from [shard] section).
     */
    int GetShardCount() const;

    /**
     * @brief Get this node's host and port.
     */
    DSSNode GetThisNode() const;

    /**
     * @brief Returns all node groups parsed from the file.
     *        Each node has a host string and port integer.
     */
    std::map<uint32_t, DSShard> GetAllShards() const;

    /**
     * @brief Returns the node groups of this node.
     */
    std::vector<uint32_t> GetShardsForThisNode() const;

    /**
     * @return Sharding algorithm (from [sharding] section).
     */
    const ShardingAlgorithm *GetShardingAlgorithm() const;

    /**
     * @brief Update the primary node of a specific node group.
     */
    void UpdatePrimaryNode(uint32_t shard_id, const DSSNode &node);

    /**
     * @brief Returns if this node is the owner of a specific node group.
     */
    bool IsOwnerOfShard(int shard_id, uint64_t *shard_version = nullptr);

    /**
     * @brief Returns the shard members for the specified shard.
     */
    uint64_t GetTopologyVersion() const;

    bool UpdateShardMembers(uint32_t shard_id,
                            uint64_t shard_version,
                            std::vector<DSSNode> &nodes,
                            DSShardStatus *status,
                            std::string config_file_path = "");

    const DSShard GetShard(uint32_t shard_id);

    void ReplaceShardMembers(uint32_t shard_id,
                             const std::vector<DSSNode *> old_nodes,
                             const std::vector<DSSNode *> new_nodes,
                             uint64_t shard_version);

    void AddShardMember(uint32_t shard_id, const DSSNode &new_node);

    void RemoveShardMember(uint32_t shard_id, const DSSNode &removed_node);

    uint64_t FetchDSShardVersion(uint32_t shard_id) const;

    void UpdateDSShardVersion(uint32_t shard_id, uint64_t shard_version);

    void SetThisNode(const std::string &ip, uint16_t port);

    /**
     * @brief Append the key of this node to the specified string stream.
     */
    void AppendThisNodeKey(std::stringstream &ss) const;

    DSSNode GetDSSNodeByKey(const std::string &key) const;

    const uint32_t GetShardIdByKey(const std::string &key);

    bool SwitchShardToReadOnly(uint32_t shard_id, DSShardStatus expected);
    bool SwitchShardToReadWrite(uint32_t shard_id, DSShardStatus expected);
    bool SwitchShardToClosed(uint32_t shard_id, DSShardStatus expected);

    void PrepareShardingError(uint32_t req_shard_id,
                              uint32_t shard_id,
                              ::EloqDS::remote::CommonResult *result);

    void HandleShardingError(const ::EloqDS::remote::CommonResult &result);

    std::shared_ptr<brpc::Channel> GetDataStoreServiceChannelByShardId(
        uint32_t shard_id);
    std::shared_ptr<brpc::Channel> UpdateDataStoreServiceChannelByShardId(
        uint32_t shard_id);
    std::shared_ptr<brpc::Channel> GetDataStoreServiceChannel(
        const DSSNode &node);
    std::shared_ptr<brpc::Channel> UpdateDataStoreServiceChannel(
        const DSSNode &node);

private:
    std::unique_ptr<ShardingAlgorithm> CreateShardingAlgorithm(
        const std::string &algorithm = "hash-key");

    bool SaveInternal(const std::string &filename) const;

    // Mutex for protecting the configuration
    mutable std::shared_mutex mutex_;

    // this node
    DSSNode this_node_;

    // topology
    Topology topology_;

    // shading algorithm
    std::unique_ptr<ShardingAlgorithm> sharding_algorithm_;

    // channel to other DSS service
    std::map<DSSNode, std::shared_ptr<brpc::Channel>> node_channel_map_;
};
}  // namespace EloqDS
