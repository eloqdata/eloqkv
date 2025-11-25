#!/bin/bash

set -ex

# Ensure noninteractive apt; keep TZ default
export DEBIAN_FRONTEND=noninteractive
export TZ=${TZ:-UTC}

needs_tz_config=false
if [ ! -f /etc/timezone ] || ! grep -qE '^(Etc/UTC|UTC)$' /etc/timezone; then
  needs_tz_config=true
fi
if [ ! -L /etc/localtime ] || [ "$(readlink -f /etc/localtime)" != "/usr/share/zoneinfo/Etc/UTC" ]; then
  needs_tz_config=true
fi

if $needs_tz_config; then
  echo 'tzdata tzdata/Areas select Etc' | sudo debconf-set-selections || true
  echo 'tzdata tzdata/Zones/Etc select UTC' | sudo debconf-set-selections || true
  echo 'Etc/UTC' | sudo tee /etc/timezone >/dev/null
  sudo ln -sf /usr/share/zoneinfo/Etc/UTC /etc/localtime
fi

# Install system packages
DEBIAN_FRONTEND=noninteractive sudo apt-get update
DEBIAN_FRONTEND=noninteractive sudo apt-get install -y --no-install-recommends \
    jq sudo vim wget curl apt-utils python3 python3-dev python3-pip python3-venv \
    python3-venv gdb libcurl4-openssl-dev build-essential libncurses5-dev \
    gnutls-dev bison zlib1g-dev ccache rsync cmake ninja-build libuv1-dev git \
    g++ make openjdk-11-jdk openssh-client openssh-server libssl-dev libgflags-dev \
    libleveldb-dev libsnappy-dev openssl lcov libbz2-dev liblz4-dev libzstd-dev \
    libboost-context-dev ca-certificates libc-ares-dev libc-ares2 m4 pkg-config \
    tar gcc redis tcl libreadline-dev ncurses-dev patchelf libprotobuf-dev \
    protobuf-compiler libjsoncpp-dev

# Install lua
mkdir -p $HOME/Downloads/lua && cd $HOME/Downloads/lua
curl -fsSL https://www.lua.org/ftp/lua-5.4.6.tar.gz | tar -xzf - --strip-components=1
make all && sudo make install
cd ../ && rm -rf lua

# Setup Python virtual environment and install dependencies
python3 -m venv $HOME/venv
source $HOME/venv/bin/activate
pip install --no-cache-dir --upgrade pip setuptools wheel
pip install --no-cache-dir setuptools==45.2.0 \
    cassandra-driver==3.28.0 \
    awscli==1.29.44 \
    boto3==1.28.36 \
    botocore==1.31.44 \
    mysql-connector-python==8.1.0 \
    psutil==5.9.5 \
    grpcio==1.60.0 \
    grpcio-tools==1.60.0

# Install Protobuf
mkdir -p $HOME/Downloads/protobuf && cd $HOME/Downloads/protobuf
curl -fsSL https://github.com/protocolbuffers/protobuf/archive/refs/tags/v21.12.tar.gz | tar -xzf - --strip-components=1
cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=yes \
    -Dprotobuf_BUILD_TESTS=OFF \
    -Dprotobuf_ABSL_PROVIDER=package \
    -S . -B cmake-out
cmake --build cmake-out -- -j $(nproc)
sudo cmake --build cmake-out --target install -- -j $(nproc)
sudo ldconfig
cd ../ && rm -rf protobuf

# Install glog
git clone https://github.com/eloqdata/glog.git glog
cd glog
cmake -S . -B build -G "Unix Makefiles"
cmake --build build -j6
sudo cmake --build build --target install
cd ../ && rm -rf glog

# Install liburing
git clone https://github.com/axboe/liburing.git liburing
cd liburing
git checkout tags/liburing-2.6
./configure --cc=gcc --cxx=g++
make -j4 && sudo make install
cd .. && rm -rf liburing

# Install brpc
git clone https://github.com/eloqdata/brpc.git brpc
cd brpc
mkdir build && cd build
cmake .. \
    -DWITH_GLOG=ON \
    -DIO_URING_ENABLED=ON \
    -DBUILD_SHARED_LIBS=ON
cmake --build . -j6
sudo cp -r ./output/include/* /usr/include/
sudo cp ./output/lib/* /usr/lib/
cd ../../ && rm -rf brpc

# Install braft
git clone https://github.com/eloqdata/braft.git braft
cd braft
sed -i 's/libbrpc.a//g' CMakeLists.txt
mkdir bld && cd bld
cmake .. -DBRPC_WITH_GLOG=ON
cmake --build . -j6
sudo cp -r ./output/include/* /usr/include/
sudo cp ./output/lib/* /usr/lib/
cd ../../ && rm -rf braft

# Install mimalloc
git clone https://github.com/eloqdata/mimalloc.git mimalloc
cd mimalloc
git checkout eloq-v2.1.2
mkdir bld && cd bld
cmake .. && make && sudo make install
cd ../../ && rm -rf mimalloc

# Install cuckoo filter
git clone https://github.com/eloqdata/cuckoofilter.git cuckoofilter
cd cuckoofilter
sudo make install
cd .. && rm -rf cuckoofilter

# Install AWSSDK
git clone --recurse-submodules https://github.com/aws/aws-sdk-cpp.git aws
cd aws
git checkout tags/1.11.446
mkdir bld && cd bld
cmake .. \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX=./output/ \
    -DENABLE_TESTING=OFF \
    -DBUILD_SHARED_LIBS=ON \
    -DFORCE_SHARED_CRT=OFF \
    -DBUILD_ONLY="dynamodb;sqs;s3;kinesis;kafka;transfer"
cmake --build . --config RelWithDebInfo -j6
cmake --install . --config RelWithDebInfo
sudo cp -r ./output/include/* /usr/include/
sudo cp -r ./output/lib/* /usr/lib/
cd ../../ && rm -rf aws

# Install rocksdb
git clone https://github.com/facebook/rocksdb.git
cd rocksdb
git checkout tags/v9.1.0
USE_RTTI=1 PORTABLE=1 ROCKSDB_DISABLE_TCMALLOC=1 ROCKSDB_DISABLE_JEMALLOC=1 make -j8 shared_lib
sudo make install-shared
cd ../ && sudo ldconfig && rm -rf rocksdb

# Install prometheus cpp client
git clone https://github.com/jupp0r/prometheus-cpp.git
cd prometheus-cpp
git checkout tags/v1.1.0
git submodule init && git submodule update
mkdir _build && cd _build
cmake .. -DBUILD_SHARED_LIBS=ON
cmake --build . -j6
sudo cmake --install .
cd ../../ && rm -rf prometheus-cpp

# Install Catch2
git clone -b v3.3.2 https://github.com/catchorg/Catch2.git
cd Catch2 && mkdir bld && cd bld
cmake .. \
    -DCMAKE_INSTALL_PREFIX=/usr/ \
    -DCATCH_BUILD_EXAMPLES=OFF \
    -DBUILD_TESTING=OFF
cmake --build . -j4
sudo cmake --install .
cd ../../ && rm -rf Catch2

# Install Abseil
mkdir -p $HOME/Downloads/abseil-cpp && cd $HOME/Downloads/abseil-cpp
curl -fsSL https://github.com/abseil/abseil-cpp/archive/20230802.0.tar.gz | tar -xzf - --strip-components=1
sed -i 's/^#define ABSL_OPTION_USE_\(.*\) 2/#define ABSL_OPTION_USE_\1 0/' "absl/base/options.h"
cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DABSL_BUILD_TESTING=OFF \
    -DBUILD_SHARED_LIBS=yes \
    -S . -B cmake-out
cmake --build cmake-out -- -j $(nproc)
sudo cmake --build cmake-out --target install -- -j $(nproc)
sudo ldconfig
cd ../ && rm -rf abseil-cpp

# Install RE2
mkdir -p $HOME/Downloads/re2 && cd $HOME/Downloads/re2
curl -fsSL https://github.com/google/re2/archive/2023-08-01.tar.gz | tar -xzf - --strip-components=1
cmake -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=ON \
    -DRE2_BUILD_TESTING=OFF \
    -S . -B cmake-out
cmake --build cmake-out -- -j $(nproc)
sudo cmake --build cmake-out --target install -- -j $(nproc)
sudo ldconfig
cd ../ && rm -rf re2

# Install gRPC
mkdir -p $HOME/Downloads/grpc && cd $HOME/Downloads/grpc
curl -fsSL https://codeload.github.com/grpc/grpc/tar.gz/refs/tags/v1.51.1 | tar -xzf - --strip-components=1
cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=yes \
    -DgRPC_INSTALL=ON \
    -DgRPC_BUILD_TESTS=OFF \
    -DgRPC_ABSL_PROVIDER=package \
    -DgRPC_CARES_PROVIDER=package \
    -DgRPC_PROTOBUF_PROVIDER=package \
    -DgRPC_RE2_PROVIDER=package \
    -DgRPC_SSL_PROVIDER=package \
    -DgRPC_ZLIB_PROVIDER=package \
    -S . -B cmake-out
cmake --build cmake-out -- -j $(nproc)
sudo cmake --build cmake-out --target install -- -j $(nproc)
sudo ldconfig
cd ../ && rm -rf grpc

# Install crc32c
mkdir -p $HOME/Downloads/crc32c && cd $HOME/Downloads/crc32c
curl -fsSL https://github.com/google/crc32c/archive/1.1.2.tar.gz | tar -xzf - --strip-components=1
cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=yes \
    -DCRC32C_BUILD_TESTS=OFF \
    -DCRC32C_BUILD_BENCHMARKS=OFF \
    -DCRC32C_USE_GLOG=OFF \
    -S . -B cmake-out
cmake --build cmake-out -- -j $(nproc)
sudo cmake --build cmake-out --target install -- -j $(nproc)
sudo ldconfig
cd ../ && rm -rf crc32c

# Install nlohmann_json
mkdir -p $HOME/Downloads/json && cd $HOME/Downloads/json
curl -fsSL https://github.com/nlohmann/json/archive/v3.11.2.tar.gz | tar -xzf - --strip-components=1
cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=yes \
    -DBUILD_TESTING=OFF \
    -DJSON_BuildTests=OFF \
    -S . -B cmake-out
sudo cmake --build cmake-out --target install -- -j $(nproc)
sudo ldconfig
cd ../ && rm -rf json

# Install Google Cloud CPP
mkdir -p $HOME/Downloads/google-cloud-cpp && cd $HOME/Downloads/google-cloud-cpp
curl -fsSL https://codeload.github.com/googleapis/google-cloud-cpp/tar.gz/refs/tags/v2.24.0 | tar -xzf - --strip-components=1
cmake -S . -B cmake-out \
    -DCMAKE_INSTALL_PREFIX="/usr/local" \
    -DBUILD_SHARED_LIBS=ON \
    -DBUILD_TESTING=OFF \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DGOOGLE_CLOUD_CPP_ENABLE_EXAMPLES=OFF \
    -DGOOGLE_CLOUD_CPP_ENABLE=bigtable,storage
cmake --build cmake-out -- -j $(nproc)
sudo cmake --build cmake-out --target install
cd ../ && rm -rf google-cloud-cpp

# Install Google Cloud CLI
DEBIAN_FRONTEND=noninteractive sudo apt-get install -y apt-transport-https ca-certificates gnupg curl sudo
echo "deb https://packages.cloud.google.com/apt cloud-sdk main" | sudo tee -a /etc/apt/sources.list.d/google-cloud-sdk.list
curl https://packages.cloud.google.com/apt/doc/apt-key.gpg | sudo apt-key add -
DEBIAN_FRONTEND=noninteractive sudo apt-get update && DEBIAN_FRONTEND=noninteractive sudo apt-get install -y google-cloud-cli

# Install FakeIt
git clone https://github.com/eranpeer/FakeIt.git
cd FakeIt
sudo cp single_header/catch/fakeit.hpp /usr/include/catch2/fakeit.hpp
cd ../ && rm -rf FakeIt

# Install RocksDB-Cloud
git clone https://github.com/eloqdata/rocksdb-cloud.git
cd rocksdb-cloud
git checkout main
LIBNAME=librocksdb-cloud-aws USE_RTTI=1 USE_AWS=1 ROCKSDB_DISABLE_TCMALLOC=1 ROCKSDB_DISABLE_JEMALLOC=1 make shared_lib -j $(nproc)
LIBNAME=librocksdb-cloud-aws PREFIX=$(pwd)/output make install-shared
sudo mkdir -p /usr/local/include/rocksdb_cloud_header
sudo cp -r ./output/include/* /usr/local/include/rocksdb_cloud_header
sudo cp -r ./output/lib/* /usr/local/lib
make clean && rm -rf $(pwd)/output
LIBNAME=librocksdb-cloud-gcp USE_RTTI=1 USE_GCP=1 ROCKSDB_DISABLE_TCMALLOC=1 ROCKSDB_DISABLE_JEMALLOC=1 make shared_lib -j $(nproc)
LIBNAME=librocksdb-cloud-gcp PREFIX=$(pwd)/output make install-shared
sudo cp -r ./output/lib/* /usr/local/lib
cd ../ && sudo ldconfig && rm -rf rocksdb-cloud

echo "All dependencies have been installed successfully!" 