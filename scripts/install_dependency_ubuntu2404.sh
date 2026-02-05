#!/bin/bash
set -ex

DIR=$(realpath $0) && DIR=${DIR%/*}
cd /tmp/

bash $DIR/dep/ubuntu/00-system.sh
bash $DIR/dep/ubuntu/01-clone.sh &
bash $DIR/dep/ubuntu/02-python.sh &
wait

source $HOME/venv/bin/activate

cd lua
make all && sudo make install
cd .. && rm -rf lua

cd protobuf
cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=yes \
  -Dprotobuf_BUILD_TESTS=OFF \
  -Dprotobuf_ABSL_PROVIDER=package \
  -S . -B cmake-out
cmake --build cmake-out -- -j $(nproc)
sudo cmake --build cmake-out --target install -- -j $(nproc)
sudo ldconfig
cd .. && rm -rf protobuf

cd glog
cmake -S . -B build -G "Unix Makefiles"
cmake --build build -j6
sudo cmake --build build --target install
cd .. && rm -rf glog

cd liburing
./configure --cc=gcc --cxx=g++
make -j4 && sudo make install
cd .. && rm -rf liburing

cd brpc
mkdir build && cd build
cmake .. \
  -DWITH_GLOG=ON \
  -DIO_URING_ENABLED=ON \
  -DBUILD_SHARED_LIBS=ON
cmake --build . -j6
sudo cp -r ./output/include/* /usr/include/
sudo cp ./output/lib/* /usr/lib/
cd ../.. && rm -rf brpc

cd braft
sed -i 's/libbrpc.a//g' CMakeLists.txt
mkdir bld && cd bld
cmake .. -DBRPC_WITH_GLOG=ON
cmake --build . -j6
sudo cp -r ./output/include/* /usr/include/
sudo cp ./output/lib/* /usr/lib/
cd ../.. && rm -rf braft

cd mimalloc
mkdir bld && cd bld
cmake .. && make && sudo make install
cd ../.. && rm -rf mimalloc

cd cuckoofilter
sudo make install
cd .. && rm -rf cuckoofilter

cd aws
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
cd ../.. && rm -rf aws

cd rocksdb
USE_RTTI=1 PORTABLE=1 ROCKSDB_DISABLE_TCMALLOC=1 ROCKSDB_DISABLE_JEMALLOC=1 make -j8 shared_lib
sudo make install-shared
cd .. && sudo ldconfig && rm -rf rocksdb

cd prometheus-cpp
mkdir _build && cd _build
cmake .. -DBUILD_SHARED_LIBS=ON
cmake --build . -j6
sudo cmake --install .
cd ../.. && rm -rf prometheus-cpp

cd Catch2 && mkdir bld && cd bld
cmake .. \
  -DCMAKE_INSTALL_PREFIX=/usr/ \
  -DCATCH_BUILD_EXAMPLES=OFF \
  -DBUILD_TESTING=OFF
cmake --build . -j4
sudo cmake --install .
cd ../.. && rm -rf Catch2

cd abseil-cpp
sed -i 's/^#define ABSL_OPTION_USE_\(.*\) 2/#define ABSL_OPTION_USE_\1 0/' "absl/base/options.h"
cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DABSL_BUILD_TESTING=OFF \
  -DBUILD_SHARED_LIBS=yes \
  -S . -B cmake-out
cmake --build cmake-out -- -j $(nproc)
sudo cmake --build cmake-out --target install -- -j $(nproc)
sudo ldconfig
cd .. && rm -rf abseil-cpp

cd re2
cmake -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=ON \
  -DRE2_BUILD_TESTING=OFF \
  -S . -B cmake-out
cmake --build cmake-out -- -j $(nproc)
sudo cmake --build cmake-out --target install -- -j $(nproc)
sudo ldconfig
cd .. && rm -rf re2

cd grpc
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
cd .. && rm -rf grpc

cd crc32c
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
cd .. && rm -rf crc32c

cd json
cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=yes \
  -DBUILD_TESTING=OFF \
  -DJSON_BuildTests=OFF \
  -S . -B cmake-out
sudo cmake --build cmake-out --target install -- -j $(nproc)
sudo ldconfig
cd .. && rm -rf json

cd google-cloud-cpp
cmake -S . -B cmake-out \
  -DCMAKE_INSTALL_PREFIX="/usr/local" \
  -DBUILD_SHARED_LIBS=ON \
  -DBUILD_TESTING=OFF \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DGOOGLE_CLOUD_CPP_ENABLE_EXAMPLES=OFF \
  -DGOOGLE_CLOUD_CPP_ENABLE=bigtable,storage
cmake --build cmake-out -- -j $(nproc)
sudo cmake --build cmake-out --target install
cd .. && rm -rf google-cloud-cpp

cd FakeIt
sudo cp single_header/catch/fakeit.hpp /usr/include/catch2/fakeit.hpp
cd .. && rm -rf FakeIt

cd rocksdb-cloud
LIBNAME=librocksdb-cloud-aws USE_RTTI=1 USE_AWS=1 ROCKSDB_DISABLE_TCMALLOC=1 ROCKSDB_DISABLE_JEMALLOC=1 make shared_lib -j $(nproc)
LIBNAME=librocksdb-cloud-aws PREFIX=$(pwd)/output make install-shared
sudo mkdir -p /usr/local/include/rocksdb_cloud_header
sudo cp -r ./output/include/* /usr/local/include/rocksdb_cloud_header
sudo cp -r ./output/lib/* /usr/local/lib
make clean && rm -rf $(pwd)/output
LIBNAME=librocksdb-cloud-gcp USE_RTTI=1 USE_GCP=1 ROCKSDB_DISABLE_TCMALLOC=1 ROCKSDB_DISABLE_JEMALLOC=1 make shared_lib -j $(nproc)
LIBNAME=librocksdb-cloud-gcp PREFIX=$(pwd)/output make install-shared
sudo cp -r ./output/lib/* /usr/local/lib
cd .. && sudo ldconfig && rm -rf rocksdb-cloud
