#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "bthread/moodycamelqueue.h"
#include "gflags/gflags.h"
#include "glog/logging.h"

DEFINE_string(output_file_dir,
              "",
              "The file directory path to store the aof data files");
DEFINE_uint32(thread_count,
              1,
              "The number of thread to parse records from rocksdb");

DEFINE_uint32(write_block_size, 1000000, "Block bytes size each write");
DEFINE_uint32(write_block_count, 1000, "Each thread writes block count");

namespace EloqKV
{

namespace Tools
{

struct ParseWorker
{
    ParseWorker(const std::string output_aof_path,
                uint32_t write_block_size,
                uint32_t write_block_count)
        : output_aof_path_(output_aof_path),
          block_size_(write_block_size),
          block_count_(write_block_count)
    {
        thd_ = std::thread([this] { Run(); });
    }

    void Terminate()
    {
        std::unique_lock<std::mutex> lk(entry_mtx_);
        terminated_ = true;
    }

    void Join()
    {
        thd_.join();
    }

    void Run()
    {
        std::ofstream outfile(output_aof_path_);
        if (!outfile.is_open())
        {
            LOG(INFO) << "Failed to open output file: " << output_aof_path_;
            return;
        }

        std::string cmds_str;
        // write 1MB data once
        const uint32_t WriteBlockSize = block_size_;
        cmds_str.resize(WriteBlockSize);

        for (uint i = 0; i < block_count_; i++)
        {
            outfile << cmds_str;
        }
        outfile.flush();
        outfile.close();

        LOG(INFO) << "Thread terminated, write count:" << block_count_;
    }

private:
    bool terminated_{false};
    std::mutex entry_mtx_;
    // std::condition_variable cv_;

    std::string output_aof_path_;
    std::thread thd_;

    uint32_t block_size_;
    uint32_t block_count_;
};

bool CheckPathExists(const std::string &db_path)
{
    std::error_code error_code;
    bool path_exists = std::filesystem::exists(db_path, error_code);
    if (error_code.value() != 0)
    {
        LOG(ERROR) << "unable to check rocksdb directory: " << db_path
                   << ", error code: " << error_code.value()
                   << ", error message: " << error_code.message();
        return false;
    }

    return path_exists;
}

}  // namespace Tools

}  // namespace EloqKV

int main(int argc, char *argv[])
{
    // using namespace EloqKV;
    // google::SetVersionString(VERSION);
    google::ParseCommandLineFlags(&argc, &argv, true);

    FLAGS_logtostdout = true;
    google::InitGoogleLogging(argv[0]);

    if (!EloqKV::Tools::CheckPathExists(FLAGS_output_file_dir))
    {
        LOG(INFO) << "Output file directory not exists: "
                  << FLAGS_output_file_dir;
        return -1;
    }

    LOG(INFO) << "Output file directory path:" << FLAGS_output_file_dir;
    LOG(INFO) << "Parse thread count:" << FLAGS_thread_count;
    LOG(INFO) << "Write block size:" << FLAGS_write_block_size;
    LOG(INFO) << "Write block count:" << FLAGS_write_block_count;
    LOG(INFO) << "====Begin====";

    std::vector<std::unique_ptr<EloqKV::Tools::ParseWorker>> workers;
    for (uint32_t i = 0; i < FLAGS_thread_count; i++)
    {
        const std::string output_fpath =
            FLAGS_output_file_dir + "/" + std::to_string(i) + ".tmp";
        workers.emplace_back(std::make_unique<EloqKV::Tools::ParseWorker>(
            output_fpath, FLAGS_write_block_size, FLAGS_write_block_count));
    }

    for (uint32_t idx = 0; idx < FLAGS_thread_count; idx++)
    {
        workers.at(idx)->Join();
    }

    LOG(INFO) << "====Finished====";
    return 0;
}
