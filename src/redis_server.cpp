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
#include <brpc/ssl_options.h>
#include <gflags/gflags.h>
#include <glog/logging.h>

#include <cstdint>
#include <limits>
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
constexpr char VERSION[] = "1.3.2";

// EloqKV flags - these are converted to tx flags
DEFINE_string(ip, "127.0.0.1", "Redis IP");
DEFINE_int32(port, 6379, "Redis Port");
DEFINE_int32(admin_port,
             0,
             "Administrative Redis port; 0 disables the admin listener");
DEFINE_uint32(admin_maxclients,
              16,
              "Maximum connections accepted by the admin Redis listener");
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

namespace
{
/**
 * A non-owning Redis service view for the administrative listener.
 *
 * brpc::Server owns and deletes ServerOptions::redis_service, so the primary
 * RedisServiceImpl cannot be installed in two Server instances directly. The
 * primary Server owns the implementation; the administrative Server owns this
 * proxy and is always stopped and destroyed first.
 */
class RedisServiceProxy final : public brpc::RedisService
{
public:
    explicit RedisServiceProxy(EloqKV::RedisServiceImpl *service)
        : service_(service)
    {
    }

    std::unique_ptr<brpc::ConnectionContext> NewConnectionContext(
        brpc::Socket *socket) const override
    {
        return service_->NewConnectionContext(socket);
    }

    brpc::RedisCommandHandlerResult DispatchCommand(
        brpc::ConnectionContext *ctx,
        const std::vector<butil::StringPiece> &args,
        brpc::RedisReply *output,
        bool flush_batched) const override
    {
        return service_->DispatchCommand(ctx, args, output, flush_batched);
    }

private:
    EloqKV::RedisServiceImpl *service_;
};

void ConfigureRedisListener(brpc::ServerOptions *options,
                            EloqKV::RedisServiceImpl *redis_service,
                            size_t max_connections,
                            const char *listener_name)
{
    std::string n_bthreads;
    GFLAGS_NAMESPACE::GetCommandLineOption("bthread_concurrency", &n_bthreads);
    options->num_threads = std::stoi(n_bthreads);
    options->has_builtin_services = false;
    options->enabled_protocols = "redis";
    options->redis_max_connections = max_connections;

    if (!redis_service->IsTlsEnabled())
    {
        return;
    }

    options->force_ssl = true;
    brpc::ServerSSLOptions *ssl_options = options->mutable_ssl_options();
    ssl_options->default_cert.certificate = redis_service->GetTlsCertFile();
    ssl_options->default_cert.private_key = redis_service->GetTlsKeyFile();

    LOG(INFO) << "TLS enabled for " << listener_name
              << " Redis listener. Certificate: "
              << redis_service->GetTlsCertFile()
              << ", Key: " << redis_service->GetTlsKeyFile();
}

std::string RedisListenAddress(uint32_t port)
{
    const auto &network_config = DataSubstrate::Instance().GetNetworkConfig();
    const std::string ip =
        network_config.bind_all ? "0.0.0.0" : network_config.local_ip;
    return ip + ":" + std::to_string(port);
}
}  // namespace

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

    const int64_t configured_admin_port =
        IsEloqkvFlagSet("admin_port")
            ? FLAGS_admin_port
            : config_reader.GetInteger("local", "admin_port", FLAGS_admin_port);
    const int64_t configured_admin_maxclients =
        IsEloqkvFlagSet("admin_maxclients")
            ? FLAGS_admin_maxclients
            : config_reader.GetInteger(
                  "local", "admin_maxclients", FLAGS_admin_maxclients);
    if (configured_admin_port < 0 ||
        configured_admin_port > std::numeric_limits<uint16_t>::max())
    {
        LOG(ERROR) << "admin_port must be between 0 and "
                   << std::numeric_limits<uint16_t>::max();
        return -1;
    }
    if (configured_admin_maxclients <= 0 ||
        configured_admin_maxclients > std::numeric_limits<uint32_t>::max())
    {
        LOG(ERROR) << "admin_maxclients must be between 1 and "
                   << std::numeric_limits<uint32_t>::max();
        return -1;
    }
    const uint32_t admin_port = static_cast<uint32_t>(configured_admin_port);
    const uint32_t admin_maxclients =
        static_cast<uint32_t>(configured_admin_maxclients);

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
    // Declared after the primary Server so that its non-owning Redis service
    // proxy is destroyed before the primary Server deletes RedisServiceImpl.
    brpc::Server admin_server;
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
    if (admin_port != 0 && admin_port == redis_service_ptr->GetRedisPort())
    {
        LOG(ERROR) << "admin_port must differ from the primary Redis port";
        redis_service_ptr->Stop();
        DataSubstrate::Instance().Shutdown();
#if BRPC_WITH_GLOG
        google::ShutdownGoogleLogging();
#endif
        return -1;
    }

    // Notice: redis_service_impl will be deleted in server's destructor.
    server_options.redis_service = redis_service_impl.release();
    // This listener is exclusively RESP. Declaring it as such lets brpc
    // enforce maxclients immediately after accept, before any optional TLS
    // handshake, without applying the limit to EloqKV's other RPC servers.
    ConfigureRedisListener(&server_options,
                           redis_service_ptr,
                           redis_service_ptr->MaxConnectionCount(),
                           "primary");

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
    EloqKV::server_acceptor = server.GetAcceptor();

    std::string admin_ip_port;
    if (admin_port != 0)
    {
        admin_ip_port = RedisListenAddress(admin_port);
        brpc::ServerOptions admin_server_options;
        // The proxy exposes exactly the same service behavior while keeping
        // ownership and connection admission independent between listeners.
        admin_server_options.redis_service =
            new RedisServiceProxy(redis_service_ptr);
        ConfigureRedisListener(&admin_server_options,
                               redis_service_ptr,
                               admin_maxclients,
                               "administrative");
        if (admin_server.Start(admin_ip_port.c_str(), &admin_server_options) !=
            0)
        {
            LOG(ERROR) << "Failed to start the administrative Redis listener "
                       << "on " << admin_ip_port;
            server.Stop(0);
            server.Join();
            EloqKV::server_acceptor = nullptr;
            redis_service_ptr->Stop();
            DataSubstrate::Instance().Shutdown();
#if BRPC_WITH_GLOG
            google::ShutdownGoogleLogging();
#endif
            return -1;
        }
    }

    if (!FLAGS_alsologtostderr)
    {
        std::cout << "EloqKV Server Started, listening on " << redis_ip_port
                  << std::endl;
    }
    LOG(INFO) << "==== EloqKV Server Started, listening on " << redis_ip_port
              << "====";
    if (admin_port != 0)
    {
        if (!FLAGS_alsologtostderr)
        {
            std::cout << "Administrative Redis listener started on "
                      << admin_ip_port << std::endl;
        }
        LOG(INFO) << "==== Administrative Redis listener started on "
                  << admin_ip_port << ", maxclients=" << admin_maxclients
                  << " ====";
    }

    server.RunUntilAskedToQuit();

    // Stop the proxy listener before stopping the shared RedisServiceImpl.
    if (admin_server.IsRunning())
    {
        admin_server.Stop(0);
        admin_server.Join();
    }
    EloqKV::server_acceptor = nullptr;

    if (!FLAGS_alsologtostderr)
    {
        std::cout << "\nEloqKV Server Stopping..." << std::endl;
    }
    redis_service_ptr->Stop();
    DataSubstrate::Instance().Shutdown();

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
