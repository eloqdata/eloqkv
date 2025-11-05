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
#include "cloud_manager.h"

#include <fcntl.h>
#include <glog/logging.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <nlohmann/json.hpp>

namespace EloqVec
{
size_t CloudManager::WriteCallback(void *contents,
                                   size_t size,
                                   size_t nmemb,
                                   std::string *data)
{
    size_t total_size = size * nmemb;
    data->append(static_cast<char *>(contents), total_size);
    return total_size;
}

bool CloudManager::ConnectCloudService()
{
    // Start the cloud service
    return StartCloudService() && TryConnectCloudService() && CreateBucket();
}

std::string CloudManager::FindExecutable(const std::string &name) const
{
    // Check if the name is an absolute path and exists
    if (name.find('/') != std::string::npos)
    {
        if (std::filesystem::exists(name) &&
            std::filesystem::is_regular_file(name))
        {
            return name;
        }
        return "";
    }

    // Find the executable in the PATH environment variable
    const char *path_env = getenv("PATH");
    if (!path_env)
    {
        return "";
    }

    std::string path_str(path_env);
    std::stringstream ss(path_str);
    std::string dir;
    while (std::getline(ss, dir, ':'))
    {
        if (dir.empty())
        {
            continue;
        }

        std::string full_path(dir);
        full_path.append("/").append(name);
        if (std::filesystem::exists(full_path) &&
            std::filesystem::is_regular_file(full_path))
        {
            // Check if the file is executable
            if (access(full_path.c_str(), X_OK) == 0)
            {
                return full_path;
            }
        }
    }
    return "";
}

bool CloudManager::IsPortAvailable(int port) const
{
    bool available = true;
    // Check if the port is in use using socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1)
    {
        return false;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, (struct sockaddr *) &addr, sizeof(addr)) != 0)
    {
        available = false;
    }
    close(sock);
    return available;
}

bool CloudManager::StartCloudService()
{
    std::string rclone_path = FindExecutable("rclone");
    if (rclone_path.empty())
    {
        LOG(ERROR) << "Failed to find rclone executable in PATH.";
        return false;
    }

    // Check if the port is already in use
    if (!IsPortAvailable(15572))
    {
        LOG(ERROR) << "Port 15572 is already in use";
        return false;
    }

    // Start the cloud service
    char *argv[] = {const_cast<char *>("rclone"),
                    const_cast<char *>("rcd"),
                    const_cast<char *>("--rc-no-auth"),
                    const_cast<char *>("--rc-addr=127.0.0.1:15572"),
                    const_cast<char *>("--transfers=16"),
                    const_cast<char *>("--checkers=16"),
                    const_cast<char *>("--s3-upload-concurrency=8"),
                    const_cast<char *>("--s3-chunk-size=8M"),
                    const_cast<char *>("--fast-list"),
                    const_cast<char *>("-v"),
                    nullptr};

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions,
                                     STDOUT_FILENO,
                                     "/tmp/vector_cloud_service.log",
                                     O_WRONLY | O_CREAT | O_APPEND,
                                     0644);
    posix_spawn_file_actions_addopen(&actions,
                                     STDERR_FILENO,
                                     "/tmp/vector_cloud_service.log",
                                     O_WRONLY | O_CREAT | O_APPEND,
                                     0644);
    pid_t pid;
    int ret = posix_spawn(
        &pid, rclone_path.c_str(), &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (ret != 0)
    {
        LOG(ERROR)
            << "Failed to start vector cloud service on port 15572: errno "
            << ret << " (" << strerror(ret) << ")";
        return false;
    }

    cloud_service_pid_ = pid;
    DLOG(INFO) << "Cloud service started on port 15572 with PID: " << pid;
    return true;
}

bool CloudManager::ShutdownCloudService()
{
    if (cloud_service_pid_ != 0)
    {
        kill(cloud_service_pid_, SIGTERM);
        // Wait for the process to exit with a timeout of 5 seconds
        for (int i = 0; i < 50; ++i)
        {
            if (kill(cloud_service_pid_, 0) != 0)
            {
                // The process has exited
                LOG(INFO) << "Cloud service stopped gracefully.";
                cloud_service_pid_ = 0;
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // If the process is not exited, send SIGKILL to force termination
        LOG(ERROR)
            << "Cloud service did not stop gracefully, forcing termination...";
        kill(cloud_service_pid_, SIGKILL);

        // Wait for the process to exit completely
        waitpid(cloud_service_pid_, nullptr, 0);
        cloud_service_pid_ = 0;
        LOG(INFO) << "Cloud service forcefully stopped.";
    }
    return true;
}

bool CloudManager::TryConnectCloudService() const
{
    // Try to connect to the cloud service
    const int max_retries = 10;
    int retries = 0;
    bool connected = false;
    std::string url(base_url_);
    url.append("/rc/noop");
    while (!connected)
    {
        connected = PerformRequest(url, "{}");
        if (!connected && retries++ == max_retries - 1)
        {
            LOG(ERROR) << "Failed to connect to cloud service";
            return false;
        }
        else if (!connected)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
    DLOG(INFO) << "Connected to cloud service successfully";
    return true;
}

bool CloudManager::CreateBucket() const
{
    // Make a directory in the cloud service
    if (cloud_config_.base_path_.empty())
    {
        LOG(ERROR) << "bucket name is empty";
        return false;
    }
    // check if the directory already exists using /operations/list
    size_t slash_pos = cloud_config_.base_path_.find_first_of('/');
    slash_pos = slash_pos == std::string::npos ? cloud_config_.base_path_.size()
                                               : slash_pos;
    std::string_view bucket_name(cloud_config_.base_path_.c_str(), slash_pos);
    nlohmann::json list_params;
    list_params.emplace("fs", cloud_config_.endpoint_ + ":");
    list_params.emplace("remote", bucket_name);
    std::string list_url(base_url_);
    list_url.append("/operations/list");
    if (PerformRequest(list_url, list_params.dump()))
    {
        // directory already exists
        DLOG(INFO) << "Bucket " << bucket_name << " already exists";
        return true;
    }

    // create directory using operations/mkdir
    nlohmann::json mkdir_params;
    mkdir_params.emplace("fs", cloud_config_.endpoint_ + ":");
    mkdir_params.emplace("remote", bucket_name);
    std::string mkdir_url(base_url_);
    mkdir_url.append("/operations/mkdir");
    if (!PerformRequest(mkdir_url, mkdir_params.dump()))
    {
        LOG(ERROR) << "Failed to create bucket " << bucket_name;
        return false;
    }
    DLOG(INFO) << "Bucket " << bucket_name << " created successfully";
    return true;
}

bool CloudManager::UploadFile(const std::string &local_file_path,
                              const std::string &remote_file_path)
{
    static const std::string upload_url(base_url_ + "/operations/copyfile");
    static const std::string remote_fs(cloud_config_.endpoint_ + ":" +
                                       cloud_config_.base_path_);

    nlohmann::json upload_params;
    upload_params.emplace("srcFs", "/");
    upload_params.emplace("srcRemote", local_file_path);
    upload_params.emplace("dstFs", remote_fs);
    upload_params.emplace("dstRemote", remote_file_path);

    if (!PerformRequest(upload_url, upload_params.dump()))
    {
        LOG(ERROR) << "Failed to upload file: " << local_file_path;

        return false;
    }
    DLOG(INFO) << "File uploaded successfully: " << local_file_path << " to "
               << remote_file_path;
    return true;
}

bool CloudManager::DownloadFile(const std::string &remote_file_path,
                                const std::string &local_file_path)
{
    static const std::string download_url(base_url_ + "/operations/copyfile");
    static const std::string remote_fs(cloud_config_.endpoint_ + ":" +
                                       cloud_config_.base_path_);

    nlohmann::json download_params;
    download_params.emplace("srcFs", remote_fs);
    download_params.emplace("srcRemote", remote_file_path);
    download_params.emplace("dstFs", "/");
    download_params.emplace("dstRemote", local_file_path);
    if (!PerformRequest(download_url, download_params.dump()))
    {
        LOG(ERROR) << "Failed to download file: " << remote_file_path;
        return false;
    }
    DLOG(INFO) << "File downloaded successfully: " << remote_file_path << " to "
               << local_file_path;
    return true;
}

bool CloudManager::DeleteFile(const std::string &remote_file_path)
{
    static const std::string delete_url(base_url_ + "/operations/deletefile");
    static const std::string remote_fs(cloud_config_.endpoint_ + ":" +
                                       cloud_config_.base_path_);

    nlohmann::json delete_params;
    delete_params.emplace("fs", remote_fs);
    delete_params.emplace("remote", remote_file_path);
    if (!PerformRequest(delete_url, delete_params.dump()))
    {
        LOG(ERROR) << "Failed to delete file: " << remote_file_path;
        return false;
    }
    DLOG(INFO) << "File deleted successfully: " << remote_file_path;
    return true;
}

bool CloudManager::PerformRequest(const std::string &url,
                                  const std::string &data) const
{
    CURL *curl = curl_easy_init();
    if (!curl)
    {
        LOG(ERROR) << "Failed to initialize curl";
        return false;
    }

    std::string response;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());

    CURLcode res = curl_easy_perform(curl);

    long response_code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK)
    {
        LOG(ERROR) << "Request failed with error: " << curl_easy_strerror(res);
        return false;
    }

    if (response_code != 200)
    {
        LOG(ERROR) << "Request failed with HTTP error: " << response_code
                   << " and response: " << response;
        return false;
    }
    return true;
}

}  // namespace EloqVec