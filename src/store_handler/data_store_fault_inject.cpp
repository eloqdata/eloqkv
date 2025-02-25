#include "data_store_fault_inject.h"

#include <algorithm>
#include <csignal>
#include <string>

#include "butil/logging.h"

namespace EloqDS
{
// using namespace remote;
void FaultInject::TriggerAction(FaultEntry *entry)
{
    if (entry->start_strike_ >= 0 && entry->end_strike_ >= 0)
    {
        entry->count_strike_++;
        if (entry->start_strike_ < entry->count_strike_ ||
            entry->end_strike_ > entry->count_strike_)
        {
            return;
        }
    }

    for (auto str : entry->vctAction_)
    {
        std::string action, para;
        size_t pos = str.find('<');
        if (pos == std::string::npos)
        {
            pos = str.find('#');
            if (pos == std::string::npos)
            {
                action = str;
            }
            else
            {
                action = str.substr(0, pos);
                para = str.substr(pos + 1, str.size() - pos - 1);
            }
        }
        else
        {
            action = str.substr(0, pos);
            size_t pos2 = str.find('>');
            if (pos2 == std::string::npos)
            {
                LOG(ERROR) << "Error action parameters: name="
                           << entry->fault_name_ << ", action=" << str;
                abort();
            }

            para = str.substr(pos + 1, pos2 - pos - 1);
        }

        std::transform(action.begin(), action.end(), action.begin(), ::toupper);
        auto iter = action_name_to_enum_map.find(action);
        FaultAction fa =
            (iter == action_name_to_enum_map.end() ? FaultAction::NOOP
                                                   : iter->second);

        switch (fa)
        {
        case FaultAction::PANIC:
        {
            int retval;
            sigset_t new_mask;
            sigfillset(&new_mask);

            retval = kill(getpid(), SIGKILL);
            assert(retval == 0);
            retval = sigsuspend(&new_mask);
            fprintf(
                stderr, "sigsuspend returned %d errno %d \n", retval, errno);
            assert(false); /* With full signal mask, we should never return
                              here. */
            break;
        }
        case FaultAction::SLEEP:
        {
            int secs = 1;
            if (!para.empty())
            {
                secs = std::stoi(para);
            }

            sleep(secs);
            break;
        }
        default:
            break;
        }
    }
}

}  // namespace EloqDS
