#include "data_store_service_config.h"

#include <algorithm>  // for std::find, etc.
#include <cassert>
#include <cstdlib>  // for std::stoi
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "INIReader.h"
#include "glog/logging.h"

namespace EloqDS
{

void ShardingAlgorithm::Load(const INIReader &reader)
{
    name_ = reader.Get("sharding", "algorithm", "hash-key");
}

void ShardingAlgorithm::Save(std::ofstream &ofs) const
{
    ofs << "[sharding]\n";
    ofs << "algorithm=" << name_ << "\n\n";
}

void Topology::InitWithSingleNode(const DSSNode &node)
{
    DSShard shard;
    shard.shard_id_ = 0;
    shard.nodes_.push_back(node);
    shard.status_ = DSShardStatus::ReadWrite;
    shard.version_ = 1;
    shards_[0] = shard;
    shard_count_ = 1;
    topology_version_ = 1;
}

void Topology::Load(const INIReader &reader)
{
    // [version]
    topology_version_ = reader.GetInteger("version", "topology_version", 1);

    // [shard]
    shard_count_ =
        static_cast<int>(reader.GetInteger("shard", "shard_count", 0));
    DLOG(INFO) << "Shard count: " << shard_count_;

    // For each shard, e.g. [node_list_shard0], [node_list_shard1]...
    for (int i = 0; i < shard_count_; i++)
    {
        DSShard shard;
        shard.shard_id_ = i;  // store the shard ID

        // Construct the INI section name
        const std::string shardSection = "node_list_shard" + std::to_string(i);

        // Read shard status for this shard
        std::string shard_status = reader.Get(shardSection, "shard_status", "");
        if (shard_status == "read_write")
        {
            shard.status_ = DSShardStatus::ReadWrite;
        }
        else if (shard_status == "read_only")
        {
            shard.status_ = DSShardStatus::ReadOnly;
        }
        else if (shard_status == "closed")
        {
            shard.status_ = DSShardStatus::Closed;
        }
        else
        {
            LOG(ERROR) << "Unknown shard status: " << shard_status
                       << " for group " << i;
            std::abort();
        }

        shard.version_ = reader.GetInteger(shardSection, "shard_version", 1);
        int nodeIndex = 0;
        while (true)
        {
            // node0, node1, node2, ...
            std::string key = "node" + std::to_string(nodeIndex++);
            std::string nodeValue = reader.Get(shardSection, key, "");
            if (nodeValue.empty())
            {
                // no more nodes
                break;
            }

            // Parse into DSSNode
            DSSNode node = ParseHostPort(nodeValue);
            shard.nodes_.push_back(node);
            DLOG(INFO) << "Shard " << i << " node " << node.host_name_ << ":"
                       << node.port_;
        }

        // Store in the map
        shards_[i] = shard;
    }
}

void Topology::Save(std::ofstream &ofs) const
{
    // [version]
    ofs << "[version]\n";
    ofs << "topology_version=" << topology_version_ << "\n\n";

    // [shard]
    ofs << "[shard]\n";
    ofs << "shard_count=" << shard_count_ << "\n\n";

    // [node_list_shardX] and node entries
    for (int i = 0; i < shard_count_; i++)
    {
        // write shard status
        ofs << "[node_list_shard" << i << "]\n";
        std::string shard_status_str;
        switch (shards_.at(i).status_)
        {
        case DSShardStatus::ReadWrite:
            shard_status_str = "read_write";
            break;
        case DSShardStatus::ReadOnly:
            shard_status_str = "read_only";
            break;
        case DSShardStatus::Closed:
            shard_status_str = "closed";
            break;
        default:
            assert(false);
            shard_status_str = "unknown";
            break;
        }
        ofs << "shard_status=" << shard_status_str << "\n";

        // write shard version
        ofs << "shard_version=" << shards_.at(i).version_ << "\n";

        // Ensure shard i exists in map
        if (shards_.find(i) != shards_.end())
        {
            const auto &shard = shards_.at(i);
            assert(shard.nodes_.size() > 0);
            for (size_t idx = 0; idx < shard.nodes_.size(); ++idx)
            {
                const DSSNode &node = shard.nodes_[idx];
                // Write host_name_:port_
                ofs << "node" << idx << "=" << node.host_name_ << ":"
                    << node.port_ << "\n";
            }
        }
        ofs << "\n";
    }
}

void Topology::Update(const std::map<uint32_t, DSShard> &shards_map,
                      uint64_t topology_version)
{
    shard_count_ = shards_map.size();
    shards_.clear();
    shards_ = shards_map;
    topology_version_ = topology_version;
}

DSShardStatus Topology::FetchDSShardStatus(uint32_t shard_id) const
{
    auto it = shards_.find(shard_id);
    if (it != shards_.end())
    {
        return it->second.status_;
    }
    return DSShardStatus::Unknown;
}

void Topology::UpdateDSShardStatus(uint32_t shard_id, DSShardStatus status)
{
    auto it = shards_.find(shard_id);
    if (it != shards_.end())
    {
        it->second.status_ = status;
    }
}

int Topology::GetShardCount() const
{
    return shard_count_;
}

const std::map<uint32_t, DSShard> &Topology::GetAllShards() const
{
    return shards_;
}

const DSSNode &Topology::GetPrimaryNode(uint32_t shardIndex) const
{
    return shards_.at(shardIndex).nodes_.front();
}

std::vector<uint32_t> Topology::GetShardsForNode(const DSSNode &node) const
{
    std::vector<uint32_t> result;
    // Look through each shard to see if we have a matching node
    for (const auto &[sId, shard] : shards_)
    {
        for (const auto &other_node : shard.nodes_)
        {
            if (other_node.host_name_ == node.host_name_ &&
                other_node.port_ == node.port_)
            {
                // found a match
                result.push_back(sId);
            }
        }
    }
    return result;
}

void Topology::UpdatePrimaryNode(uint32_t shard_id, const DSSNode &node)
{
    // Find the shard and update the primary node
    auto it = shards_.find(shard_id);
    if (it != shards_.end())
    {
        // found if the node is in the shard, move it to the front if found
        auto &shard = it->second;
        auto it = std::find(shard.nodes_.begin(), shard.nodes_.end(), node);
        if (it != shard.nodes_.end())
        {
            shard.nodes_.erase(it);
            shard.nodes_.insert(shard.nodes_.begin(), node);
        }
        // else, add the node to the front
        else
        {
            shard.nodes_.insert(shard.nodes_.begin(), node);
        }
    }
    // else, add the shard and node
    else
    {
        DSShard shard;
        shard.shard_id_ = shard_id;
        shard.nodes_.push_back(node);
        shards_[shard_id] = shard;
    }
}

bool Topology::IsOwnerOfShard(int shard_id)
{
    auto it = shards_.find(shard_id);
    if (it == shards_.end())
    {
        return false;
    }
    return it->second.status_ != DSShardStatus::Closed;
}

bool Topology::UpdateShardMembers(uint32_t shard_id,
                                  uint64_t shard_version,
                                  std::vector<DSSNode> &nodes)
{
    auto it = shards_.try_emplace(shard_id);
    if (!it.second && it.first->second.version_ > shard_version)
    {
        return false;
    }
    it.first->second.nodes_ = nodes;
    it.first->second.version_ = shard_version;
    return true;
}

void Topology::AddShardMember(uint32_t shard_id, const DSSNode &new_node)
{
    auto it = shards_.find(shard_id);
    if (it == shards_.end())
    {
        DSShard shard;
        shard.shard_id_ = shard_id;
        shard.nodes_.push_back(new_node);
        shards_[shard_id] = shard;
        return;
    }

    for (auto &node : it->second.nodes_)
    {
        if (node == new_node)
        {
            return;
        }
    }

    it->second.nodes_.push_back(new_node);
}

const DSShard &Topology::GetShard(uint32_t shard_id) const
{
    return shards_.at(shard_id);
}

void Topology::RemoveShardMember(uint32_t shard_id, const DSSNode &remove_node)
{
    auto it = shards_.at(shard_id).nodes_.begin();
    for (; it != shards_.at(shard_id).nodes_.end(); ++it)
    {
        if (*it == remove_node)
        {
            shards_.at(shard_id).nodes_.erase(it);
            return;
        }
    }
}

uint64_t Topology::FetchDSShardVersion(uint32_t shard_id) const
{
    auto it = shards_.find(shard_id);
    if (it != shards_.end())
    {
        return it->second.version_;
    }
    return 0;
}

void Topology::UpdateDSShardVersion(uint32_t shard_id, uint64_t shard_version)
{
    auto it = shards_.find(shard_id);
    if (it != shards_.end())
    {
        it->second.version_ = shard_version;
    }
}

DSSNode Topology::ParseHostPort(const std::string &host_port) const
{
    DSSNode node;

    auto colon_pos = host_port.find(':');
    if (colon_pos == std::string::npos)
    {
        // If there's no colon, entire string is host
        node.host_name_ = host_port;
        node.port_ = 0;
        return node;
    }

    node.host_name_ = host_port.substr(0, colon_pos);
    std::string port_str = host_port.substr(colon_pos + 1);

    try
    {
        node.port_ = static_cast<uint16_t>(std::stoi(port_str));
    }
    catch (...)
    {
        // If stoi fails, fallback to 0
        node.port_ = 0;
    }

    return node;
}

void DataStoreServiceClusterManager::Initialize(const std::string &local_ip,
                                                int local_service_port)
{
    std::unique_lock<std::shared_mutex> lk(mutex_);
    this_node_.host_name_ = local_ip;
    this_node_.port_ = local_service_port;
    // init topology whith this node
    topology_.InitWithSingleNode(this_node_);
    // init sharding algorithm with default one
    sharding_algorithm_ = CreateShardingAlgorithm();
}

bool DataStoreServiceClusterManager::Load(const std::string &filename)
{
    std::unique_lock<std::shared_mutex> lk(mutex_);

    INIReader reader(filename);

    if (reader.ParseError() != 0)
    {
        LOG(ERROR) << "Failed to parse INI file: " << filename;
        return false;
    }

    // [this_node]
    this_node_ = topology_.ParseHostPort(reader.Get("this_node", "node", ""));

    // [topology]
    topology_.Load(reader);

    // [sharding]
    std::string sharding_algorithm = reader.Get("sharding", "algorithm", "");
    sharding_algorithm_ = CreateShardingAlgorithm(sharding_algorithm);
    sharding_algorithm_->Load(reader);

    return true;
}

bool DataStoreServiceClusterManager::Save(const std::string &filename) const
{
    std::shared_lock<std::shared_mutex> lk(mutex_);
    return SaveInternal(filename);
}

bool DataStoreServiceClusterManager::SaveInternal(
    const std::string &filename) const
{
    std::string tmp_filename = filename + ".tmp";
    std::ofstream ofs(tmp_filename, std::ofstream::out | std::ofstream::trunc);
    if (!ofs.is_open())
    {
        LOG(ERROR) << "Failed to open file for writing: " << filename;
        return false;
    }

    // [this_node]
    ofs << "[this_node]\n";
    ofs << "node=" << this_node_.host_name_ << ":" << this_node_.port_
        << "\n\n";

    // [topology]
    topology_.Save(ofs);

    // [sharding]
    sharding_algorithm_->Save(ofs);

    ofs.close();

    // Rename the temp file to the final file
    if (std::rename(tmp_filename.c_str(), filename.c_str()) != 0)
    {
        LOG(ERROR) << "Failed to rename temp file to final file: " << filename;
        return false;
    }

    return true;
}

void DataStoreServiceClusterManager::Update(
    const std::string &sharding_algorithm,
    const std::map<uint32_t, DSShard> &shards_map,
    uint64_t topology_version)
{
    std::unique_lock<std::shared_mutex> lk(mutex_);

    // Update the topology
    topology_.Update(shards_map, topology_version);

    // Update the sharding algorithm
    if (sharding_algorithm_ == nullptr ||
        sharding_algorithm_->GetName() != sharding_algorithm)
    {
        sharding_algorithm_ = CreateShardingAlgorithm(sharding_algorithm);
    }
}

DSShardStatus DataStoreServiceClusterManager::FetchDSShardStatus(
    uint32_t shard_id) const
{
    std::shared_lock<std::shared_mutex> lk(mutex_);
    return topology_.FetchDSShardStatus(shard_id);
}

void DataStoreServiceClusterManager::UpdateDSShardStatus(uint32_t shard_id,
                                                         DSShardStatus status)
{
    std::unique_lock<std::shared_mutex> lk(mutex_);
    topology_.UpdateDSShardStatus(shard_id, status);
}

int DataStoreServiceClusterManager::GetShardCount() const
{
    std::shared_lock<std::shared_mutex> lk(mutex_);
    return topology_.GetShardCount();
}

DSSNode DataStoreServiceClusterManager::GetThisNode() const
{
    std::shared_lock<std::shared_mutex> lk(mutex_);
    return this_node_;
}

std::map<uint32_t, DSShard> DataStoreServiceClusterManager::GetAllShards() const
{
    std::shared_lock<std::shared_mutex> lk(mutex_);
    return topology_.GetAllShards();
}

std::vector<uint32_t> DataStoreServiceClusterManager::GetShardsForThisNode()
    const
{
    std::shared_lock<std::shared_mutex> lk(mutex_);
    // Use the host and port of this_node_ to find the shards
    return topology_.GetShardsForNode(this_node_);
}

const ShardingAlgorithm *DataStoreServiceClusterManager::GetShardingAlgorithm()
    const
{
    std::shared_lock<std::shared_mutex> lk(mutex_);
    return sharding_algorithm_.get();
}

void DataStoreServiceClusterManager::UpdatePrimaryNode(uint32_t shard_id,
                                                       const DSSNode &node)
{
    std::unique_lock<std::shared_mutex> lk(mutex_);
    topology_.UpdatePrimaryNode(shard_id, node);
}

std::shared_ptr<brpc::Channel>
DataStoreServiceClusterManager::GetDataStoreServiceChannelByShardId(
    uint32_t shard_id)
{
    std::shared_lock<std::shared_mutex> lk(mutex_);
    const DSSNode &node = topology_.GetPrimaryNode(shard_id);

    auto it = node_channel_map_.find(node);
    if (it != node_channel_map_.end())
    {
        return it->second;
    }

    lk.unlock();
    std::unique_lock<std::shared_mutex> guard(mutex_);
    // double check to avoid double initialization
    it = node_channel_map_.find(node);
    if (it != node_channel_map_.end())
    {
        return it->second;
    }
    auto channel = std::make_shared<brpc::Channel>();
    if (channel->Init(node.host_name_.c_str(), node.port_, nullptr) != 0)
    {
        LOG(ERROR) << "Failed to update the cc node service channel"
                   << " shard_id: " << shard_id << " node: " << node.host_name_
                   << ":" << node.port_;
        return nullptr;
    }
    DLOG(INFO) << "DataStoreService channel updated to "
               << " shard_id: " << shard_id << " node: " << node.host_name_
               << ":" << node.port_;
    node_channel_map_[node] = channel;
    return channel;
}

std::shared_ptr<brpc::Channel>
DataStoreServiceClusterManager::GetDataStoreServiceChannel(const DSSNode &node)
{
    std::shared_lock<std::shared_mutex> lk(mutex_);

    auto it = node_channel_map_.find(node);
    if (it != node_channel_map_.end())
    {
        return it->second;
    }

    lk.unlock();
    std::unique_lock<std::shared_mutex> guard(mutex_);
    // double check to avoid double initialization
    it = node_channel_map_.find(node);
    if (it != node_channel_map_.end())
    {
        return it->second;
    }
    auto channel = std::make_shared<brpc::Channel>();
    if (channel->Init(node.host_name_.c_str(), node.port_, nullptr) != 0)
    {
        LOG(ERROR) << "Failed to update the cc node service channel.";
        return nullptr;
    }
    DLOG(INFO) << "DataStoreService channel updated to " << node.host_name_
               << ":" << node.port_;
    node_channel_map_[node] = channel;
    return channel;
}

std::shared_ptr<brpc::Channel>
DataStoreServiceClusterManager::UpdateDataStoreServiceChannelByShardId(
    uint32_t shard_id)
{
    std::unique_lock<std::shared_mutex> guard(mutex_);
    const DSSNode &node = topology_.GetPrimaryNode(shard_id);
    auto channel = std::make_shared<brpc::Channel>();
    if (channel->Init(node.host_name_.c_str(), node.port_, nullptr) != 0)
    {
        LOG(ERROR) << "Failed to update the cc node service channel.";
        return nullptr;
    }
    DLOG(INFO) << "DataStoreService channel updated to " << node.host_name_
               << ":" << node.port_;
    node_channel_map_[node] = channel;
    return channel;
}

std::shared_ptr<brpc::Channel>
DataStoreServiceClusterManager::UpdateDataStoreServiceChannel(
    const DSSNode &node)
{
    std::unique_lock<std::shared_mutex> guard(mutex_);

    auto channel = std::make_shared<brpc::Channel>();
    while (channel->Init(node.host_name_.c_str(), node.port_, nullptr) != 0)
    {
        LOG(ERROR) << "Failed to update the cc node service channel.";
        return nullptr;
    }
    DLOG(INFO) << "DataStoreService channel updated to " << node.host_name_
               << ":" << node.port_;
    node_channel_map_[node] = channel;
    return channel;
}

bool DataStoreServiceClusterManager::IsOwnerOfShard(int shard_id,
                                                    uint64_t *shard_version)
{
    std::shared_lock<std::shared_mutex> lk(mutex_);
    // bool is_owner = topology_.IsOwnerOfShard(shard_id);
    bool is_owner = topology_.GetPrimaryNode(shard_id) == this_node_;
    if (is_owner && shard_version)
    {
        *shard_version = topology_.FetchDSShardVersion(shard_id);
    }
    return is_owner;
}

uint64_t DataStoreServiceClusterManager::GetTopologyVersion() const
{
    std::shared_lock<std::shared_mutex> lk(mutex_);
    return topology_.GetTopologyVersion();
}

bool DataStoreServiceClusterManager::UpdateShardMembers(
    uint32_t shard_id,
    uint64_t shard_version,
    std::vector<DSSNode> &nodes,
    DSShardStatus *status,
    std::string config_file_path)
{
    std::unique_lock<std::shared_mutex> lk(mutex_);
    bool ret = topology_.UpdateShardMembers(shard_id, shard_version, nodes);
    if (ret && status != nullptr)
    {
        topology_.UpdateDSShardStatus(shard_id, *status);
    }
    if (ret && !config_file_path.empty())
    {
        SaveInternal(config_file_path);
    }
    return ret;
}

const DSShard DataStoreServiceClusterManager::GetShard(uint32_t shard_id)
{
    std::shared_lock<std::shared_mutex> lk(mutex_);
    return topology_.GetShard(shard_id);
}

void DataStoreServiceClusterManager::ReplaceShardMembers(
    uint32_t shard_id,
    const std::vector<DSSNode *> old_nodes,
    const std::vector<DSSNode *> new_nodes,
    uint64_t shard_version)
{
    std::unique_lock<std::shared_mutex> lk(mutex_);
    for (size_t i = 0; i < old_nodes.size(); ++i)
    {
        topology_.RemoveShardMember(shard_id, *old_nodes[i]);
    }
    for (size_t i = 0; i < new_nodes.size(); ++i)
    {
        topology_.AddShardMember(shard_id, *new_nodes[i]);
    }
    topology_.UpdateDSShardVersion(shard_id, shard_version);
}

void DataStoreServiceClusterManager::AddShardMember(uint32_t shard_id,
                                                    const DSSNode &new_node)
{
    std::unique_lock<std::shared_mutex> lk(mutex_);
    topology_.AddShardMember(shard_id, new_node);
}

void DataStoreServiceClusterManager::RemoveShardMember(
    uint32_t shard_id, const DSSNode &removed_node)
{
    std::unique_lock<std::shared_mutex> lk(mutex_);
    topology_.RemoveShardMember(shard_id, removed_node);
}

uint64_t DataStoreServiceClusterManager::FetchDSShardVersion(
    uint32_t shard_id) const
{
    std::shared_lock<std::shared_mutex> lk(mutex_);
    return topology_.FetchDSShardVersion(shard_id);
}

void DataStoreServiceClusterManager::UpdateDSShardVersion(
    uint32_t shard_id, uint64_t shard_version)
{
    std::unique_lock<std::shared_mutex> lk(mutex_);
    topology_.UpdateDSShardVersion(shard_id, shard_version);
}

void DataStoreServiceClusterManager::SetThisNode(const std::string &ip,
                                                 uint16_t port)
{
    std::unique_lock<std::shared_mutex> lk(mutex_);
    this_node_ = DSSNode{ip, port};
}

void DataStoreServiceClusterManager::AppendThisNodeKey(
    std::stringstream &ss) const
{
    std::shared_lock<std::shared_mutex> lk(mutex_);
    ss << this_node_.host_name_ << ":" << this_node_.port_;
}

DSSNode DataStoreServiceClusterManager::GetDSSNodeByKey(
    const std::string &key) const
{
    std::shared_lock<std::shared_mutex> lk(mutex_);
    uint32_t dss_shard_id =
        sharding_algorithm_->KeyToShardId(key, topology_.GetAllShards());
    return topology_.GetPrimaryNode(dss_shard_id);
}

const uint32_t DataStoreServiceClusterManager::GetShardIdByKey(
    const std::string &key)
{
    std::shared_lock<std::shared_mutex> lk(mutex_);
    return sharding_algorithm_->KeyToShardId(key, topology_.GetAllShards());
}

bool DataStoreServiceClusterManager::SwitchShardToClosed(uint32_t shard_id,
                                                         DSShardStatus expected)
{
    std::unique_lock<std::shared_mutex> lk(mutex_);
    auto shard_status = topology_.FetchDSShardStatus(shard_id);

    assert(expected == DSShardStatus::ReadOnly);
    if (shard_status != expected)
    {
        return false;
    }

    if (shard_status == DSShardStatus::Closed)
    {
        DLOG(INFO) << "SwitchToClosed, shard " << shard_id
                   << " already in closed mode";
        return true;
    }

    if (shard_status != DSShardStatus::ReadOnly)
    {
        DLOG(ERROR) << "Shard " << shard_id << " is not in read only mode"
                    << ", status: " << shard_status;
        return false;
    }

    topology_.UpdateDSShardStatus(shard_id, DSShardStatus::Closed);
    DLOG(INFO) << "SwitchToClosed, shard " << shard_id
               << " status: " << shard_status;
    return true;
}

bool DataStoreServiceClusterManager::SwitchShardToReadWrite(
    uint32_t shard_id, DSShardStatus expected)
{
    std::unique_lock<std::shared_mutex> lk(mutex_);
    auto shard_status = topology_.FetchDSShardStatus(shard_id);
    if (shard_status != expected)
    {
        return false;
    }
    if (shard_status == DSShardStatus::ReadWrite)
    {
        DLOG(INFO) << "SwitchToReadWrite, shard " << shard_id
                   << " already in read write mode";
        return true;
    }

    topology_.UpdateDSShardStatus(shard_id, DSShardStatus::ReadWrite);
    DLOG(INFO) << "SwitchToReadWrite, shard " << shard_id
               << " status: " << shard_status;
    return true;
}

bool DataStoreServiceClusterManager::SwitchShardToReadOnly(
    uint32_t shard_id, DSShardStatus expected)
{
    std::unique_lock<std::shared_mutex> lk(mutex_);
    auto shard_status = topology_.FetchDSShardStatus(shard_id);
    if (shard_status != expected)
    {
        return false;
    }

    if (shard_status == DSShardStatus::ReadOnly)
    {
        return true;
    }

    if (shard_status != DSShardStatus::ReadWrite)
    {
        DLOG(ERROR) << "Shard " << shard_id << " is not in ReadWrite mode"
                    << ", status: " << shard_status;
        return false;
    }

    topology_.UpdateDSShardStatus(shard_id, DSShardStatus::ReadOnly);
    DLOG(INFO) << "SwitchToReadOnly, shard " << shard_id
               << " status: " << shard_status;
    return true;
}

void DataStoreServiceClusterManager::PrepareShardingError(
    uint32_t req_shard_id,
    uint32_t shard_id,
    ::EloqDS::remote::CommonResult *result)
{
    std::shared_lock<std::shared_mutex> lk(mutex_);

    result->set_error_code(
        ::EloqDS::remote::DataStoreError::REQUESTED_NODE_NOT_OWNER);
    DLOG(INFO) << "=====PrepareShardingError";
    result->set_error_msg("Requested data not on local node.");
    auto *key_sharding_changed_message = result->mutable_new_key_sharding();
    if (req_shard_id != shard_id)
    {
        // Node group changed - send full cluster config
        key_sharding_changed_message->set_type(
            ::EloqDS::remote::KeyShardingErrorType::NodeGroupChanged);
        auto &shards = topology_.GetAllShards();
        auto *new_shards =
            key_sharding_changed_message->mutable_new_cluster_config();
        for (const auto &[shard_id, shard] : shards)
        {
            auto *new_shard = new_shards->add_shards();
            new_shard->set_shard_id(shard_id);
            for (const auto &node : shard.nodes_)
            {
                auto *new_node = new_shard->add_member_nodes();
                new_node->set_host_name(node.host_name_);
                new_node->set_port(node.port_);
            }
        }
    }
    else
    {
        // Same shard but primary changed - just send new primary
        key_sharding_changed_message->set_type(
            ::EloqDS::remote::KeyShardingErrorType::PrimaryNodeChanged);
        DSSNode primary_node = topology_.GetPrimaryNode(shard_id);
        auto *new_primary_node =
            key_sharding_changed_message->mutable_new_primary_node();
        new_primary_node->set_host_name(primary_node.host_name_);
        new_primary_node->set_port(primary_node.port_);
        key_sharding_changed_message->set_shard_id(shard_id);
    }
}

void DataStoreServiceClusterManager::HandleShardingError(
    const ::EloqDS::remote::CommonResult &result)
{
    assert(result.error_code() ==
           static_cast<uint32_t>(
               ::EloqDS::remote::DataStoreError::REQUESTED_NODE_NOT_OWNER));

    auto &new_key_sharding = result.new_key_sharding();
    auto error_type = new_key_sharding.type();
    if (error_type ==
        ::EloqDS::remote::KeyShardingErrorType::PrimaryNodeChanged)
    {
        uint32_t shard_id = new_key_sharding.shard_id();
        auto &primary_node = new_key_sharding.new_primary_node();
        DSSNode new_primary_node;
        new_primary_node.host_name_ = primary_node.host_name();
        new_primary_node.port_ = primary_node.port();
        UpdatePrimaryNode(shard_id, new_primary_node);
    }
    else
    {
        // the whole node group has changed
        auto &new_cluster_config = new_key_sharding.new_cluster_config();
        std::map<uint32_t, DSShard> new_shard_map;
        for (auto &shard : new_cluster_config.shards())
        {
            DSShard new_shard;
            new_shard.shard_id_ = shard.shard_id();
            for (auto &node : shard.member_nodes())
            {
                DSSNode new_node;
                new_node.host_name_ = node.host_name();
                new_node.port_ = node.port();
                new_shard.nodes_.push_back(new_node);
            }
            new_shard_map[new_shard.shard_id_] = std::move(new_shard);
        }
        uint64_t new_version = new_cluster_config.topology_version();
        const std::string &sharding_algorithm =
            new_cluster_config.sharding_algorithm();
        Update(sharding_algorithm, new_shard_map, new_version);
    }
}

std::unique_ptr<ShardingAlgorithm>
DataStoreServiceClusterManager::CreateShardingAlgorithm(
    const std::string &algorithm)
{
    // if (algorithm == "hash-key")
    return std::make_unique<ShardingAlgorithm>();
    // Add more sharding algorithms here
}

}  // namespace EloqDS
