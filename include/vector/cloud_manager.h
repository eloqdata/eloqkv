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

#include <curl/curl.h>

#include <string>

#include "vector_type.h"

namespace EloqVec
{

class CloudManager
{
public:
    explicit CloudManager(const CloudConfig &cloud_config)
        : cloud_config_(cloud_config)
    {
        curl_global_init(CURL_GLOBAL_ALL);
    }

    ~CloudManager()
    {
        curl_global_cleanup();
        ShutdownCloudService();
    }

    CloudManager(const CloudManager &) = delete;
    CloudManager(CloudManager &&) = delete;

    /**
     * @brief Connect to the cloud service
     *
     * @return True if the cloud service is connected successfully, false
     * otherwise
     */
    bool ConnectCloudService();

    /**
     * @brief Upload a file to the cloud service
     *
     * @param local_file_path Local file path
     * @param remote_file_path Remote file path
     * @return True if the file is uploaded successfully, false otherwise
     */
    bool UploadFile(const std::string &local_file_path,
                    const std::string &remote_file_path);
    /**
     * @brief Download a file from the cloud service
     *
     * @param remote_file_path Remote file path
     * @param local_file_path Local file path
     * @return True if the file is downloaded successfully, false otherwise
     */
    bool DownloadFile(const std::string &remote_file_path,
                      const std::string &local_file_path);
    /**
     * @brief Delete a file from the cloud service
     *
     * @param remote_file_path Remote file path
     * @return True if the file is deleted successfully, false otherwise
     */
    bool DeleteFile(const std::string &remote_file_path);

private:
    static size_t WriteCallback(void *contents,
                                size_t size,
                                size_t nmemb,
                                std::string *data);

    /**
     * @brief Find the executable of the cloud service
     *
     * @param name Name of the executable
     * @return The path of the executable
     */
    std::string FindExecutable(const std::string &name) const;

    /**
     * @brief Check if the port is available
     *
     * @param port Port number
     * @return True if the port is available, false otherwise
     */
    bool IsPortAvailable(int port) const;

    /**
     * @brief Start the cloud service
     *
     * @return True if the cloud service is started successfully, false
     * otherwise
     */
    bool StartCloudService();
    /**
     * @brief Shutdown the cloud service
     *
     * @return True if the cloud service is shutdown successfully, false
     * otherwise
     */
    bool ShutdownCloudService();
    /**
     * @brief Try to connect to the cloud service
     *
     * @return True if the cloud service is connected successfully, false
     * otherwise
     */
    bool TryConnectCloudService() const;
    /**
     * @brief Create a bucket in the cloud service
     *
     * @return True if the bucket is created successfully, false otherwise
     */
    bool CreateBucket() const;

    /**
     * @brief Perform a request to the cloud service
     *
     * @param url url of the request
     * @param data data of the request
     * @return True if the request is perform successfully, false otherwise
     */
    bool PerformRequest(const std::string &url, const std::string &data) const;

private:
    const std::string base_url_{"http://127.0.0.1:15572"};
    CloudConfig cloud_config_;
    // Cloud service PID
    pid_t cloud_service_pid_{0};
};

}  // namespace EloqVec