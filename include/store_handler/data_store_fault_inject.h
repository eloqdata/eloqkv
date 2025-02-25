#pragma once

#include <cassert>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "butil/logging.h"

// TODO: merge the fault_inject files with tx_service.
// They share the same code, we should have common utils in future to reduce the
// duplicate code.
namespace EloqDS
{
enum struct FaultAction
{
    NOOP = 0,
    SLEEP,
    ERROR,
    FATAL,
    PANIC,
    INFI_LOOP,
    SUSPEND,
    RESUME,
    SKIP,
    RESET,
    STATUS,
    WAIT_UNTIL_TRIGGER,
    REMOTE,
    LOG_TRANSFER,
    CLEAN_PKMAP,
    NOTIFY_CHECKPOINTER
};
static std::unordered_map<std::string, FaultAction> action_name_to_enum_map{
    {"UNKNOWN", FaultAction::NOOP},
    {"SLEEP", FaultAction::SLEEP},
    {"ERROR", FaultAction::ERROR},
    {"FATAL", FaultAction::FATAL},
    {"PANIC", FaultAction::PANIC},
    {"INFI_LOOP", FaultAction::INFI_LOOP},
    {"SUSPEND", FaultAction::SUSPEND},
    {"RESUME", FaultAction::RESUME},
    {"SKIP", FaultAction::SKIP},
    {"RESET", FaultAction::RESET},
    {"STATUS", FaultAction::STATUS},
    {"WAIT_UNTIL_TRIGGER", FaultAction::WAIT_UNTIL_TRIGGER},
    {"REMOTE", FaultAction::REMOTE},
    {"LOG_TRANSFER", FaultAction::LOG_TRANSFER},
    {"CLEAN_PKMAP", FaultAction::CLEAN_PKMAP},
    {"NOTIFY_CHECKPOINTER", FaultAction::NOTIFY_CHECKPOINTER}};

class FaultEntry
{
public:
    FaultEntry(std::string fault_name, std::string paras)
        : fault_name_(fault_name)
    {
        // Parse parameters
        size_t pos1 = 0;
        while (pos1 < paras.size())
        {
            size_t pos2 = paras.find(';', pos1);
            if (pos2 == std::string::npos)
                pos2 = paras.size();
            else if (paras.find('<', pos1) < pos2)
            {
                // To parse remote action and ensure to get entire key value
                pos2 = paras.find('>', pos1);
                if (pos2 == std::string::npos)
                {
                    LOG(ERROR) << "Error parameters for fault inject: name="
                               << fault_name << ", parameters=" << paras;
                    abort();
                }
                pos2 = paras.find(';', pos2);
                if (pos2 == std::string::npos)
                    pos2 = paras.size();
            }

            // Split key and value
            std::string sbs = paras.substr(pos1, pos2 - pos1);
            size_t pos3 = sbs.find('=');
            assert(pos3 != std::string::npos);
            std::string key = sbs.substr(0, pos3);
            std::string val = sbs.substr(pos3 + 1);

            if (key.compare("db_name") == 0)
            {
                database_name_ = val;
            }
            else if (key.compare("table_name") == 0)
            {
                table_name_ = val;
            }
            else if (key.compare("start_strike") == 0)
            {
                start_strike_ = stoi(val);
            }
            else if (key.compare("end_strike") == 0)
            {
                end_strike_ = stoi(val);
            }
            else if (key.compare("action") == 0)
            {
                vctAction_.push_back(val);
            }
            else
            {
                map_para_.emplace(key, val);
            }

            pos1 = pos2 + 1;
        }
    }

    ~FaultEntry()
    {
    }

    std::string fault_name_;
    std::unordered_map<std::string, std::string> map_para_;
    std::vector<std::string> vctAction_;
    // advanced field, not used yet.
    std::string database_name_;
    std::string table_name_;
    int start_strike_ = -1;
    int end_strike_ = -1;
    int count_strike_ = 0;
};

class FaultInject
{
public:
    static FaultInject &Instance()
    {
        static FaultInject instance_;
        return instance_;
    }

    static FaultEntry *Entry(std::string fault_name)
    {
        FaultInject &fi = Instance();
        std::lock_guard<std::mutex> lk(fi.mux_);
        auto iter = fi.injected_fault_map_.find(fault_name);
        if (iter != fi.injected_fault_map_.end())
        {
            return &iter->second;
        }
        else
        {
            return nullptr;
        }
    }

    void TriggerAction(std::string fault_name)
    {
        FaultEntry *entry;
        {
            std::lock_guard<std::mutex> lk(mux_);
            auto iter = injected_fault_map_.find(fault_name);
            if (iter != injected_fault_map_.end())
            {
                entry = &iter->second;
            }
            else
            {
                return;
            }
        }

        TriggerAction(entry);
    }

    void TriggerAction(FaultEntry *entry);
    void InjectFault(std::string fault_name, std::string paras)
    {
        LOG(INFO) << "FaultInject name=" << fault_name << "  paras=" << paras;
        // To remove the pointed fault inject.
        if (paras.compare("remove") == 0)
        {
            std::lock_guard<std::mutex> lk(mux_);
            injected_fault_map_.erase(fault_name);
            return;
        }

        FaultEntry fentry(fault_name, paras);
        if (fault_name.compare("at_once") == 0)
        {
            // If fault name equal "at_once", run it at once
            TriggerAction(&fentry);
        }
        else
        {
            std::lock_guard<std::mutex> lk(mux_);
            injected_fault_map_.try_emplace(fault_name, fentry);
        }
    }

private:
    FaultInject()
    {
    }

    ~FaultInject()
    {
    }

    uint32_t node_id_;
    std::unordered_map<std::string, FaultEntry> injected_fault_map_;
    std::mutex mux_;
};

#ifdef WITH_FAULT_INJECT
#define ACTION_FAULT_INJECTOR(FaultName)                                       \
    FaultInject::Instance().TriggerAction(FaultName)
#define CODE_FAULT_INJECTOR(FaultName, code)                                   \
    {                                                                          \
        FaultEntry *fault_entry = FaultInject::Entry(FaultName);               \
        if (fault_entry != nullptr)                                            \
            code;                                                              \
    }
#define FAULT_INJECTOR_CONDITION_WRAP(FaultName, code)                         \
    (FaultInject::Entry(FaultName) ? true : false) || (code)
#else
#define ACTION_FAULT_INJECTOR(FaultName)
#define CODE_FAULT_INJECTOR(FaultName, code)
#define FAULT_INJECTOR_CONDITION_WRAP(FaultName, code) (code)
#endif
}  // namespace EloqDS
