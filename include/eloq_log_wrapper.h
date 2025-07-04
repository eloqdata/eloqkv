/**
 *    Copyright (C) 2025 EloqData Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under either of the following two licenses:
 *    1. GNU Affero General Public License, version 3, as published by the Free
 *    Software Foundation.
 *    2. GNU General Public License as published by the Free Software
 *    Foundation; version 2 of the License.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License or GNU General Public License for more
 *    details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    and GNU General Public License V2 along with this program.  If not, see
 *    <http://www.gnu.org/licenses/>.
 *
 */
#pragma once

#include "log_agent.h"
#include "txlog.h"

namespace EloqKV
{
class EloqLogAgent : public txservice::TxLog
{
public:
    EloqLogAgent() = delete;
    EloqLogAgent(const EloqLogAgent &rhs) = delete;

    EloqLogAgent(const uint32_t log_group_replica_num)
        : log_agent_(), log_group_replica_num_(log_group_replica_num)
    {
    }

    ~EloqLogAgent() = default;

    void WriteLog(uint32_t log_group_id,
                  brpc::Controller *cntl,
                  const ::txlog::LogRequest &log_record,
                  ::txlog::LogResponse &log_response,
                  google::protobuf::Closure &done) override
    {
        log_agent_.WriteLog(
            log_group_id, cntl, &log_record, &log_response, &done);
    }

    void CheckMigrationIsFinished(
        uint32_t log_group_id,
        brpc::Controller *cntl,
        const ::txlog::CheckMigrationIsFinishedRequest &request,
        ::txlog::CheckMigrationIsFinishedResponse &response,
        google::protobuf::Closure &done) override
    {
        log_agent_.CheckMigrationIsFinished(
            log_group_id, cntl, &request, &response, &done);
    }

    ::txlog::CheckClusterScaleStatusResponse::Status CheckClusterScaleStatus(
        uint32_t log_group_id, const std::string &id) override
    {
        return log_agent_.CheckClusterScaleStatus(log_group_id, id);
    }

    void UpdateCheckpointTs(uint32_t cc_node_group_id,
                            int64_t term,
                            uint64_t checkpoint_timestamp) override
    {
        log_agent_.UpdateCheckpointTs(
            cc_node_group_id, term, checkpoint_timestamp);
    }

    void RemoveCcNodeGroup(uint32_t cc_node_group_id, int64_t term) override
    {
        log_agent_.RemoveCcNodeGroup(cc_node_group_id, term);
    }

    void ReplayLog(uint32_t cc_node_group_id,
                   int64_t term,
                   const std::string &source_ip,
                   uint16_t source_port,
                   int log_group,
                   uint64_t start_ts,
                   std::atomic<bool> &interrupt) override
    {
        log_agent_.ReplayLog(cc_node_group_id,
                             term,
                             source_ip,
                             source_port,
                             log_group,
                             start_ts,
                             interrupt);
    }

    txservice::RecoverTxStatus RecoverTx(uint64_t tx_number,
                                         int64_t tx_term,
                                         uint64_t write_lock_ts,
                                         uint32_t cc_ng_id,
                                         int64_t cc_ng_term,
                                         const std::string &source_ip,
                                         uint16_t source_port) override
    {
        ::txlog::RecoverTxResponse_TxStatus status =
            log_agent_.RecoverTx(tx_number,
                                 tx_term,
                                 write_lock_ts,
                                 cc_ng_id,
                                 cc_ng_term,
                                 source_ip,
                                 source_port,
                                 GetLogGroupId(tx_number));

        switch (status)
        {
        case ::txlog::RecoverTxResponse_TxStatus_Alive:
            return txservice::RecoverTxStatus::Alive;
        case ::txlog::RecoverTxResponse_TxStatus_Committed:
            return txservice::RecoverTxStatus::Committed;
        case ::txlog::RecoverTxResponse_TxStatus_NotCommitted:
            return txservice::RecoverTxStatus::NotCommitted;
        default:
            return txservice::RecoverTxStatus::RecoverError;
        }
    }

    void TransferLeader(uint32_t log_group_id, uint32_t leader_idx) override
    {
        log_agent_.TransferLeader(log_group_id, leader_idx);
    }

    uint32_t LogGroupCount() override
    {
        return log_agent_.LogGroupCount();
    }

    uint32_t LogGroupReplicaNum() override
    {
        return log_agent_.LogGroupReplicaNum();
    }

    uint32_t GetLogGroupId(uint64_t tx_number) override
    {
        uint32_t log_group_idx = tx_number % log_agent_.LogGroupCount();
        return log_agent_.LogGroupId(log_group_idx);
    }

    void RefreshLeader(uint32_t log_group_id) override
    {
        log_agent_.RefreshLeader(log_group_id);
    }

    void RequestRefreshLeader(uint32_t log_group_id) override
    {
        log_agent_.RequestRefreshLeader(log_group_id);
    }

    /**
     * @brief Initializae the log agent by setup the log request stub using
     * ip_list and port_list.
     */
    void Init(std::vector<std::string> &ip_list,
              std::vector<uint16_t> &port_list,
              const uint32_t start_log_group_id) override
    {
        log_agent_.Init(
            ip_list, port_list, start_log_group_id, log_group_replica_num_);
    }

    void UpdateLeaderCache(uint32_t lg_id, uint32_t node_id) override
    {
        log_agent_.UpdateLeaderCache(lg_id, node_id);
    }

private:
    ::txlog::LogAgent log_agent_;
    const uint32_t log_group_replica_num_;
};
}  // namespace EloqKV
