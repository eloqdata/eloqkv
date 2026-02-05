#!/bin/bash
set -ex

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

# Function to clone with retry
clone_repo() {
    local url=$1
    local dir=$2
    local branch=$3

    rm -rf "$dir"
    local cmd=(git clone --recurse-submodules)
    [ -n "$branch" ] && cmd+=(-b "$branch")
    cmd+=("$url" "$dir")

    run_with_retry "${cmd[@]}"
}

# Function to download tar and extract with retry
download_tar() {
    local url=$1
    local dir=$2

    rm -rf "$dir"
    mkdir -p "$dir"
    run_with_retry bash -c "curl -fsSL '$url' | tar -xzf - -C '$dir' --strip-components=1"
}

# Clone all required repositories and download tarballs in parallel
(download_tar https://www.lua.org/ftp/lua-5.4.6.tar.gz lua) &
(download_tar https://github.com/protocolbuffers/protobuf/archive/refs/tags/v21.12.tar.gz protobuf) &
(clone_repo https://github.com/eloqdata/glog.git glog) &
(clone_repo https://github.com/axboe/liburing.git liburing liburing-2.6) &
(clone_repo https://github.com/eloqdata/brpc.git brpc) &
(clone_repo https://github.com/eloqdata/braft.git braft) &
(clone_repo https://github.com/eloqdata/mimalloc.git mimalloc eloq-v2.1.2) &
(clone_repo https://github.com/eloqdata/cuckoofilter.git cuckoofilter) &
(clone_repo https://github.com/aws/aws-sdk-cpp.git aws 1.11.446) &
(clone_repo https://github.com/facebook/rocksdb.git rocksdb v9.1.0) &
(clone_repo https://github.com/jupp0r/prometheus-cpp.git prometheus-cpp v1.1.0) &
(clone_repo https://github.com/catchorg/Catch2.git Catch2 v3.3.2) &
(download_tar https://github.com/abseil/abseil-cpp/archive/20230802.0.tar.gz abseil-cpp) &
(download_tar https://github.com/google/re2/archive/2023-08-01.tar.gz re2) &
(download_tar https://codeload.github.com/grpc/grpc/tar.gz/refs/tags/v1.51.1 grpc) &
(download_tar https://github.com/google/crc32c/archive/1.1.2.tar.gz crc32c) &
(download_tar https://github.com/nlohmann/json/archive/v3.11.2.tar.gz json) &
(download_tar https://codeload.github.com/googleapis/google-cloud-cpp/tar.gz/refs/tags/v2.24.0 google-cloud-cpp) &
(clone_repo https://github.com/eranpeer/FakeIt.git FakeIt) &
(clone_repo https://github.com/eloqdata/rocksdb-cloud.git rocksdb-cloud main) &

wait

