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
#include <glog/logging.h>
#include <limits.h>

#include <cstddef>
#include <filesystem>
#include <iomanip>
#include <string>

DECLARE_string(log_file_name_prefix);

inline void CustomPrefix(std::ostream &s,
                         const google::LogMessageInfo &l,
                         void *)
{
    s << "["                                   //
      << std::setw(4) << 1900 + l.time.year()  // YY
      << '-'                                   // -
      << std::setw(2) << 1 + l.time.month()    // MM
      << '-'                                   // -
      << std::setw(2) << l.time.day()          // DD
      << 'T'                                   // T
      << std::setw(2) << l.time.hour()         // hh
      << ':'                                   // :
      << std::setw(2) << l.time.min()          // mm
      << ':'                                   // :
      << std::setw(2) << l.time.sec()          // ss
      << '.'                                   // .
      << std::setfill('0') << std::setw(6)     //
      << l.time.usec()                         // usec
      << " " << l.severity[0] << " "
      << "" << l.thread_id << "] "
#ifndef DISABLE_CODE_LINE_IN_LOG
      << "[" << l.filename << ':' << l.line_number << "]";
#else
        ;
#endif
};

inline void InitGoogleLogging(char **argv)
{
    // If `GLOG_logtostderr` is specified then log to `stderr` only .
    //
    // NOTE: This is for cases where disk space needs protection, like when
    // deployed in the cloud.
    if (FLAGS_logtostderr && FLAGS_log_dir.empty())
    {
        FLAGS_alsologtostderr = false;
        FLAGS_logtostdout = false;
    }
    else
    {
        // Log to `stderr` and `GLOG_log_dir/logfiles`.
        //
        // NOTE: If `GLOG_log_dir` is not specified then it will be default to
        // `path/to/eloqkv/logs`
        if (FLAGS_log_dir.empty())
        {
            // Get the absolute path of the bin directory
            char bin_path[PATH_MAX];
            ssize_t len = readlink("/proc/self/exe", bin_path, PATH_MAX);
            std::filesystem::path fullPath(std::string(bin_path, len));
            std::filesystem::path dir_path =
                fullPath.parent_path().parent_path();
            FLAGS_log_dir = dir_path.string() + "/logs";
        }

        if (!std::filesystem::exists(FLAGS_log_dir))
        {
            std::filesystem::create_directories(FLAGS_log_dir);
        }

        // Log to stderr and logfiles
        // FLAGS_alsologtostderr = true;

        // NOTE: Enable this will log to `stdout` instead of logfiles.
        FLAGS_logtostdout = false;

        // NOTE: Enable this will log to `stderr` instead of logfiles.
        FLAGS_logtostderr = false;

        // Log INFO/WARNING/ERROR/FATAL
        FLAGS_minloglevel = 0;

        // stderrthreshold (log messages at or above this level are copied to
        // stderr in addition to logfiles.) default: 2.
        FLAGS_stderrthreshold = google::GLOG_FATAL;

        // Don't buffer anything. NOTE: If `logtostderr` or `logtostdout` is
        // `true` then glog will force this value to -1.
        FLAGS_logbuflevel = -1;

        FLAGS_log_file_header = false;

        auto log_file_name_prefix = std::getenv("GLOG_log_file_name_prefix");
        FLAGS_log_file_name_prefix = log_file_name_prefix == NULL
                                         ? FLAGS_log_file_name_prefix
                                         : log_file_name_prefix;
        auto log_file_prefix =
            FLAGS_log_dir + "/" + FLAGS_log_file_name_prefix + ".";

        // Configure log destinations.
        google::SetLogDestination(google::INFO,
                                  (log_file_prefix + "INFO.").c_str());
        google::SetLogDestination(google::WARNING,
                                  (log_file_prefix + "WARNING.").c_str());
        google::SetLogDestination(google::ERROR,
                                  (log_file_prefix + "ERROR.").c_str());

        // Configure symlink for logfiles.
        google::SetLogSymlink(google::INFO, FLAGS_log_file_name_prefix.c_str());
        google::SetLogSymlink(google::WARNING,
                              FLAGS_log_file_name_prefix.c_str());
        google::SetLogSymlink(google::ERROR,
                              FLAGS_log_file_name_prefix.c_str());
    }
    google::InitGoogleLogging(argv[0], &CustomPrefix);
}
