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

#include <string>

#if BRPC_WITH_GLOG
#include "glog_error_logging.h"
#endif

#include "eloqkv_ascii_logo.h"
#include "redis_service.h"

DEFINE_string(config, "", "Configuration");
constexpr char VERSION[] = "0.8.24";

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
    LOG(INFO) << "Starting EloqKV Server ...";
    brpc::Server server;
    brpc::ServerOptions server_options;
    auto redis_service_impl =
        std::make_unique<EloqKV::RedisServiceImpl>(config_file, VERSION);
    if (!redis_service_impl->Init(server))
    {
        LOG(ERROR) << "Failed to start EloqKV server.";
        redis_service_impl->Stop();
#if BRPC_WITH_GLOG
        google::ShutdownGoogleLogging();
#endif
        return -1;
    }
    EloqKV::RedisServiceImpl *redis_service_ptr = redis_service_impl.get();
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
