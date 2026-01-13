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
#include <brpc/acceptor.h>
#include <brpc/server.h>
#include <gflags/gflags.h>
#include <glog/logging.h>

#include <sstream>
#include <string>

#if BRPC_WITH_GLOG
#include "glog_error_logging.h"
#endif

#include "INIReader.h"
#include "data_substrate.h"
#include "eloqkv_ascii_logo.h"
#include "redis_service.h"

DEFINE_string(config, "", "Configuration");
constexpr char VERSION[] = "1.0.2";

// EloqKV flags - these are converted to tx flags
DEFINE_string(ip, "127.0.0.1", "Redis IP");
DEFINE_int32(port, 6379, "Redis Port");
DEFINE_string(ip_port_list, "", "redis server cluster ip port list");
DEFINE_string(standby_ip_port_list,
              "",
              "Standby nodes ip:port list of servers."
              "Different standby nodes in the same group is separated by '|'."
              "If there is no standby nodes of the server, just leave empty "
              "text on the position."
              "eg:'xx|xx,xx,,xx|xx|xx' ");
DEFINE_string(voter_ip_port_list,
              "",
              "Voter nodes ip:port list of servers."
              "Different nodes in the same group is separated by '|'."
              "If there is no voter nodes of the group, just leave empty "
              "text on the position."
              "eg:'xx|xx,xx,,xx|xx|xx' ");

// Global variable defined in redis_service.cpp
extern brpc::Acceptor *EloqKV::server_acceptor;
extern std::string EloqKV::redis_ip_port;

void PrintHelloText()
{
    std::cout << EloqKV::asscii_logo << std::endl;
    std::cout << "* Welcome to use EloqKV(v" << VERSION << ")." << std::endl;
    std::cout << "* Running logs will be written to the following path:"
              << std::endl;
    std::cout << FLAGS_log_dir << std::endl;
    std::cout << "* The above log path can be specified by arg --log_dir."
              << std::endl;
    std::cout << "* You can also run with [--help] for all available flags."
              << std::endl;
    std::cout << std::endl;
}

// Helper function to check if an eloqkv flag exists and is set
static bool IsEloqkvFlagSet(const char *flag_name)
{
    gflags::CommandLineFlagInfo flag_info;
    bool flag_found = gflags::GetCommandLineFlagInfo(flag_name, &flag_info);
    return flag_found && !flag_info.is_default;
}

// Helper function to update ports in ip:port list by adding delta
static std::string UpdatePortsInList(const std::string &ip_port_list,
                                     int port_delta)
{
    if (ip_port_list.empty() || port_delta == 0)
    {
        return ip_port_list;
    }

    std::string result;
    std::istringstream stream(ip_port_list);
    std::string token;
    bool first = true;

    // Handle comma-separated node groups
    while (std::getline(stream, token, ','))
    {
        if (!first)
        {
            result += ',';
        }
        first = false;

        std::istringstream group_stream(token);
        std::string node_token;
        bool first_node = true;

        // Handle pipe-separated nodes within a group
        while (std::getline(group_stream, node_token, '|'))
        {
            if (!first_node)
            {
                result += '|';
            }
            first_node = false;

            size_t colon_pos = node_token.find(':');
            if (colon_pos != std::string::npos)
            {
                std::string ip = node_token.substr(0, colon_pos);
                std::string port_str = node_token.substr(colon_pos + 1);
                try
                {
                    int port = std::stoi(port_str);
                    port += port_delta;
                    result += ip + ":" + std::to_string(port);
                }
                catch (const std::exception &)
                {
                    // If port parsing fails, keep original
                    result += node_token;
                }
            }
            else
            {
                // No colon found, keep original
                result += node_token;
            }
        }
    }

    return result;
}

void ConvertEloqkvFlagsToTxFlags(INIReader *config_reader)
{
    // Check for eloqkv 'ip' flag and convert to 'tx_ip'
    if (IsEloqkvFlagSet("ip"))
    {
        std::string eloqkv_ip;
        if (GFLAGS_NAMESPACE::GetCommandLineOption("ip", &eloqkv_ip))
        {
            // Only set tx_ip if it hasn't been explicitly set
            if (CheckCommandLineFlagIsDefault("tx_ip"))
            {
                GFLAGS_NAMESPACE::SetCommandLineOption("tx_ip",
                                                       eloqkv_ip.c_str());
                LOG(INFO) << "Converted eloqkv flag 'ip' to 'tx_ip': "
                          << eloqkv_ip;
            }
            else
            {
                LOG(WARNING)
                    << "EloqKV flag 'ip' is set but 'tx_ip' is also set. "
                    << "Using 'tx_ip' value and ignoring 'ip'.";
            }
        }
    }
    else if (config_reader != nullptr && config_reader->HasValue("local", "ip"))
    {
        std::string eloqkv_ip = config_reader->Get("local", "ip", FLAGS_ip);
        // Only set tx_ip if it hasn't been explicitly set
        if (CheckCommandLineFlagIsDefault("tx_ip"))
        {
            GFLAGS_NAMESPACE::SetCommandLineOption("tx_ip", eloqkv_ip.c_str());
            LOG(INFO) << "Converted eloqkv config 'ip' to 'tx_ip': "
                      << eloqkv_ip;
        }
    }

    // Check for eloqkv 'port' flag and convert to 'eloqkv_port' and 'tx_port'
    if (IsEloqkvFlagSet("port"))
    {
        std::string eloqkv_port_str;
        if (GFLAGS_NAMESPACE::GetCommandLineOption("port", &eloqkv_port_str))
        {
            try
            {
                int eloqkv_port = std::stoi(eloqkv_port_str);
                int tx_port_value = eloqkv_port + 10000;

                // Only set eloqkv_port if it hasn't been explicitly set
                if (CheckCommandLineFlagIsDefault("eloqkv_port"))
                {
                    GFLAGS_NAMESPACE::SetCommandLineOption(
                        "eloqkv_port", eloqkv_port_str.c_str());
                    LOG(INFO)
                        << "Converted eloqkv flag 'port' to 'eloqkv_port': "
                        << eloqkv_port;
                }
                else
                {
                    LOG(WARNING)
                        << "EloqKV flag 'port' is set but 'eloqkv_port' is "
                           "also set. "
                        << "Using 'eloqkv_port' value and ignoring 'port'.";
                }

                // Only set tx_port if it hasn't been explicitly set
                if (CheckCommandLineFlagIsDefault("tx_port"))
                {
                    GFLAGS_NAMESPACE::SetCommandLineOption(
                        "tx_port", std::to_string(tx_port_value).c_str());
                    LOG(INFO) << "Set 'tx_port' to: " << tx_port_value
                              << " (port + 10000)";
                }
                else
                {
                    LOG(WARNING) << "EloqKV flag 'port' is set but 'tx_port' "
                                    "is also set. "
                                 << "Using 'tx_port' value and ignoring "
                                    "calculated value.";
                }
            }
            catch (const std::exception &e)
            {
                LOG(ERROR) << "Failed to parse eloqkv 'port' flag value: "
                           << eloqkv_port_str << ", error: " << e.what();
            }
        }
    }
    else if (config_reader != nullptr &&
             config_reader->HasValue("local", "port"))
    {
        int eloqkv_port =
            config_reader->GetInteger("local", "port", FLAGS_port);
        int tx_port_value = eloqkv_port + 10000;

        // Only set eloqkv_port if it hasn't been explicitly set
        if (CheckCommandLineFlagIsDefault("eloqkv_port"))
        {
            GFLAGS_NAMESPACE::SetCommandLineOption(
                "eloqkv_port", std::to_string(eloqkv_port).c_str());
            LOG(INFO) << "Converted eloqkv config 'port' to 'eloqkv_port': "
                      << eloqkv_port;
        }

        // Only set tx_port if it hasn't been explicitly set
        if (CheckCommandLineFlagIsDefault("tx_port"))
        {
            GFLAGS_NAMESPACE::SetCommandLineOption(
                "tx_port", std::to_string(tx_port_value).c_str());
            LOG(INFO) << "Set 'tx_port' to: " << tx_port_value
                      << " (port + 10000)";
        }
    }

    // Convert eloqkv ip_port_list to tx_ip_port_list (with ports +10000)
    if (IsEloqkvFlagSet("ip_port_list"))
    {
        std::string eloqkv_ip_port_list;
        if (GFLAGS_NAMESPACE::GetCommandLineOption("ip_port_list",
                                                   &eloqkv_ip_port_list))
        {
            // Only convert if tx_ip_port_list hasn't been explicitly set
            if (CheckCommandLineFlagIsDefault("tx_ip_port_list"))
            {
                std::string updated_list =
                    UpdatePortsInList(eloqkv_ip_port_list, 10000);
                GFLAGS_NAMESPACE::SetCommandLineOption("tx_ip_port_list",
                                                       updated_list.c_str());
                LOG(INFO)
                    << "Converted eloqkv 'ip_port_list' to 'tx_ip_port_list' "
                       "with ports incremented by 10000";
            }
            else
            {
                LOG(WARNING) << "EloqKV flag 'ip_port_list' is set but "
                                "'tx_ip_port_list' is also set. "
                             << "Using 'tx_ip_port_list' value and ignoring "
                                "'ip_port_list'.";
            }
        }
    }
    else if (config_reader != nullptr &&
             config_reader->HasValue("cluster", "ip_port_list"))
    {
        std::string eloqkv_ip_port_list =
            config_reader->Get("cluster", "ip_port_list", "");
        // Only convert if tx_ip_port_list hasn't been explicitly set
        if (CheckCommandLineFlagIsDefault("tx_ip_port_list"))
        {
            std::string updated_list =
                UpdatePortsInList(eloqkv_ip_port_list, 10000);
            GFLAGS_NAMESPACE::SetCommandLineOption("tx_ip_port_list",
                                                   updated_list.c_str());
            LOG(INFO) << "Converted eloqkv config 'ip_port_list' to "
                         "'tx_ip_port_list' with ports incremented by 10000";
        }
        else
        {
            LOG(WARNING) << "EloqKV config 'ip_port_list' is set but "
                            "'tx_ip_port_list' is also set. "
                         << "Using 'tx_ip_port_list' value and ignoring "
                            "'ip_port_list'.";
        }
    }

    // Convert eloqkv standby_ip_port_list to tx_standby_ip_port_list (with
    // ports +10000)
    if (IsEloqkvFlagSet("standby_ip_port_list"))
    {
        std::string eloqkv_standby_ip_port_list;
        if (GFLAGS_NAMESPACE::GetCommandLineOption(
                "standby_ip_port_list", &eloqkv_standby_ip_port_list))
        {
            // Only convert if tx_standby_ip_port_list hasn't been explicitly
            // set
            if (CheckCommandLineFlagIsDefault("tx_standby_ip_port_list"))
            {
                std::string updated_list =
                    UpdatePortsInList(eloqkv_standby_ip_port_list, 10000);
                GFLAGS_NAMESPACE::SetCommandLineOption(
                    "tx_standby_ip_port_list", updated_list.c_str());
                LOG(INFO) << "Converted eloqkv 'standby_ip_port_list' to "
                             "'tx_standby_ip_port_list' with ports incremented "
                             "by 10000";
            }
            else
            {
                LOG(WARNING) << "EloqKV flag 'standby_ip_port_list' is set but "
                                "'tx_standby_ip_port_list' is also set. "
                             << "Using 'tx_standby_ip_port_list' value and "
                                "ignoring 'standby_ip_port_list'.";
            }
        }
    }
    else if (config_reader != nullptr &&
             config_reader->HasValue("cluster", "standby_ip_port_list"))
    {
        std::string eloqkv_standby_ip_port_list =
            config_reader->Get("cluster", "standby_ip_port_list", "");
        // Only convert if tx_standby_ip_port_list hasn't been explicitly set
        if (CheckCommandLineFlagIsDefault("tx_standby_ip_port_list"))
        {
            std::string updated_list =
                UpdatePortsInList(eloqkv_standby_ip_port_list, 10000);
            GFLAGS_NAMESPACE::SetCommandLineOption("tx_standby_ip_port_list",
                                                   updated_list.c_str());
            LOG(INFO)
                << "Converted eloqkv config 'standby_ip_port_list' to "
                   "'tx_standby_ip_port_list' with ports incremented by 10000";
        }
        else
        {
            LOG(WARNING)
                << "EloqKV config 'standby_ip_port_list' is set but "
                   "'tx_standby_ip_port_list' is also set. "
                << "Using 'tx_standby_ip_port_list' value and ignoring "
                   "'standby_ip_port_list'.";
        }
    }

    // Convert eloqkv voter_ip_port_list to tx_voter_ip_port_list (with ports
    // +10000)
    if (IsEloqkvFlagSet("voter_ip_port_list"))
    {
        std::string eloqkv_voter_ip_port_list;
        if (GFLAGS_NAMESPACE::GetCommandLineOption("voter_ip_port_list",
                                                   &eloqkv_voter_ip_port_list))
        {
            // Only convert if tx_voter_ip_port_list hasn't been explicitly set
            if (CheckCommandLineFlagIsDefault("tx_voter_ip_port_list"))
            {
                std::string updated_list =
                    UpdatePortsInList(eloqkv_voter_ip_port_list, 10000);
                GFLAGS_NAMESPACE::SetCommandLineOption("tx_voter_ip_port_list",
                                                       updated_list.c_str());
                LOG(INFO) << "Converted eloqkv 'voter_ip_port_list' to "
                             "'tx_voter_ip_port_list' with ports incremented "
                             "by 10000";
            }
            else
            {
                LOG(WARNING) << "EloqKV flag 'voter_ip_port_list' is set but "
                                "'tx_voter_ip_port_list' is also set. "
                             << "Using 'tx_voter_ip_port_list' value and "
                                "ignoring 'voter_ip_port_list'.";
            }
        }
    }
    else if (config_reader != nullptr &&
             config_reader->HasValue("cluster", "voter_ip_port_list"))
    {
        std::string eloqkv_voter_ip_port_list =
            config_reader->Get("cluster", "voter_ip_port_list", "");
        // Only convert if tx_voter_ip_port_list hasn't been explicitly set
        if (CheckCommandLineFlagIsDefault("tx_voter_ip_port_list"))
        {
            std::string updated_list =
                UpdatePortsInList(eloqkv_voter_ip_port_list, 10000);
            GFLAGS_NAMESPACE::SetCommandLineOption("tx_voter_ip_port_list",
                                                   updated_list.c_str());
            LOG(INFO)
                << "Converted eloqkv config 'voter_ip_port_list' to "
                   "'tx_voter_ip_port_list' with ports incremented by 10000";
        }
        else
        {
            LOG(WARNING) << "EloqKV config 'voter_ip_port_list' is set but "
                            "'tx_voter_ip_port_list' is also set. "
                         << "Using 'tx_voter_ip_port_list' value and ignoring "
                            "'voter_ip_port_list'.";
        }
    }
}

int main(int argc, char *argv[])
{
    using namespace EloqKV;
    google::SetVersionString(VERSION);
    google::ParseCommandLineFlags(&argc, &argv, true);

#if BRPC_WITH_GLOG
    InitGoogleLogging(argv);
#endif
    FLAGS_stderrthreshold = google::GLOG_FATAL;
    if (!FLAGS_alsologtostderr)
    {
        PrintHelloText();
        std::cout << "Starting EloqKV Server..." << std::endl;
    }

    std::string config_file = FLAGS_config;
    INIReader config_reader(config_file);
    if (!config_file.empty() && config_reader.ParseError() != 0)
    {
        std::cout << "Failed to parse config file: " << config_file
                  << std::endl;
        return -1;
    }

    // Convert eloqkv flags to tx flags
    ConvertEloqkvFlagsToTxFlags(&config_reader);

    // Step 1: Initialize DataSubstrate
    if (!DataSubstrate::Instance().Init(config_file))
    {
        LOG(ERROR) << "Failed to initialize DataSubstrate.";
        return -1;
    }

    // Step 2: Initialize and register EloqKv engine
    LOG(INFO) << "Starting EloqKV Server ...";
    DataSubstrate::Instance().EnableEngine(txservice::TableEngine::EloqKv);
    brpc::Server server;
    brpc::ServerOptions server_options;
    auto redis_service_impl =
        std::make_unique<EloqKV::RedisServiceImpl>(config_file, VERSION);
    if (!redis_service_impl->Init(server))
    {
        LOG(ERROR) << "Failed to start EloqKV server.";
        redis_service_impl->Stop();
        DataSubstrate::Instance().Shutdown();
#if BRPC_WITH_GLOG
        google::ShutdownGoogleLogging();
#endif
        return -1;
    }

    // Step 3: Start DataSubstrate
    if (!DataSubstrate::Instance().Start())
    {
        LOG(ERROR) << "Failed to start DataSubstrate.";
        redis_service_impl->Stop();
        DataSubstrate::Instance().Shutdown();
#if BRPC_WITH_GLOG
        google::ShutdownGoogleLogging();
#endif
        return -1;
    }

    // Step 4: Start Redis service
    EloqKV::RedisServiceImpl *redis_service_ptr = redis_service_impl.get();
    if (!redis_service_ptr->Start(server))
    {
        LOG(ERROR) << "Failed to start Redis service.";
        redis_service_ptr->Stop();
        DataSubstrate::Instance().Shutdown();
#if BRPC_WITH_GLOG
        google::ShutdownGoogleLogging();
#endif
        return -1;
    }
    std::string n_bthreads;
    GFLAGS_NAMESPACE::GetCommandLineOption("bthread_concurrency", &n_bthreads);
    server_options.num_threads = std::stoi(n_bthreads);
    // Notice: redis_service_impl will be deleted in server's destructor.
    server_options.redis_service = redis_service_impl.release();
    server_options.has_builtin_services = false;
    if (server.Start(redis_ip_port.c_str(), &server_options) != 0)
    {
        LOG(ERROR) << "Failed to start EloqKV server.";
        redis_service_ptr->Stop();
        DataSubstrate::Instance().Shutdown();
#if BRPC_WITH_GLOG
        google::ShutdownGoogleLogging();
#endif
        return -1;
    }

    if (!FLAGS_alsologtostderr)
    {
        std::cout << "EloqKV Server Started, listening on " << redis_ip_port
                  << std::endl;
    }
    LOG(INFO) << "==== EloqKV Server Started, listening on " << redis_ip_port
              << "====";

    EloqKV::server_acceptor = server.GetAcceptor();

    server.RunUntilAskedToQuit();

    if (!FLAGS_alsologtostderr)
    {
        std::cout << "\nEloqKV Server Stopping..." << std::endl;
    }
    DataSubstrate::Instance().Shutdown();
    redis_service_ptr->Stop();

    if (!FLAGS_alsologtostderr)
    {
        std::cout << "EloqKV Server Stopped." << std::endl;
    }
    LOG(INFO) << "EloqKV Server Stopped.";

#if BRPC_WITH_GLOG
    google::ShutdownGoogleLogging();
#endif
    return 0;
}
