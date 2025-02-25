# Copyright (c) 2006, 2010, Oracle and/or its affiliates. All rights reserved.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335 USA
cmake_minimum_required(VERSION 3.8)
project(eloqkv C CXX)

# Install dependencies to compile redis_cli:
# With apt:
# sudo apt-get install libreadline-dev
# sudo apt-get install ncurses-dev
# With yum:
# sudo yum install readline-devel
# sudo yum install ncurses-devel

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)

set(METRICS_LIB "eloq-metrics" CACHE STRING "metrics library name.")

option(INI_USE_HEAP "Whether parse ini on heap" ON)

if (INI_USE_HEAP)
    add_definitions(-DINI_USE_STACK=0)
    add_definitions(-DINI_ALLOW_REALLOC=1)
endif()

IF ( (CMAKE_BUILD_TYPE STREQUAL "") OR (CMAKE_BUILD_TYPE MATCHES "Debug"))
  message(NOTICE "WITH_FAULT_INJECT: ON")
  add_definitions(-DWITH_FAULT_INJECT)
ELSE()
  message(NOTICE "WITH_FAULT_INJECT: OFF")
  remove_definitions(-DWITH_FAULT_INJECT)
ENDIF()

option(LINK_SO "Whether examples are linked dynamically" ON)

option(BRPC_WITH_GLOG "With glog" ON)

option(ABSL_PROPAGATE_CXX_STD "ABSL PROPAGATE CXX STD" ON)

# KV data store backend for redis server, Cassandra, DynamoDB, RocksDB Cloud
set(WITH_KV_STORE "CASSANDRA" CACHE STRING "The KV data store for Monograph Redis")
set_property(CACHE WITH_KV_STORE PROPERTY STRINGS "CASSANDRA" "DYNAMODB" "ROCKSDB")
message(NOTICE "With DATA_STORE: ${WITH_KV_STORE}")

# Set the cloud storage backend for RocksDB Cloud
if (WITH_KV_STORE STREQUAL "ROCKSDB")
  set(ROCKSDB_CLOUD_FS  "" CACHE STRING "The RocksDB backend file system, S3 or GCS")
  set_property(CACHE ROCKSDB_CLOUD_FS PROPERTY STRINGS "S3" "GCS")
   message(NOTICE "With ROCKSDB_CLOUD_FS: ${ROCKSDB_CLOUD_FS}")
endif ()

# Add compile flags for KV stores
if (WITH_KV_STORE STREQUAL "CASSANDRA")
   add_compile_definitions(KV_DATA_STORE_TYPE=1)
elseif (WITH_KV_STORE STREQUAL "DYNAMODB")
   add_compile_definitions(KV_DATA_STORE_TYPE=2)
elseif (WITH_KV_STORE STREQUAL "ROCKSDB")
   add_compile_definitions(KV_DATA_STORE_TYPE=3)
   if (ROCKSDB_CLOUD_FS STREQUAL "S3")
     add_compile_definitions(ROCKSDB_CLOUD_FS_TYPE=1)
   elseif (ROCKSDB_CLOUD_FS STREQUAL "GCS")
     add_compile_definitions(ROCKSDB_CLOUD_FS_TYPE=2)
   endif ()
endif ()

option(WITH_LOG_SERVICE "Compile with built-in log service." OFF)
message("WITH_LOG_SERVICE: " ${WITH_LOG_SERVICE})
if (WITH_LOG_SERVICE)
   add_compile_definitions(WITH_LOG_SERVICE=1)
endif()

option(RANGE_PARTITION_ENABLED "Whether enable range partition" OFF)
message(NOTICE "RANGE_PARTITION_ENABLED : ${RANGE_PARTITION_ENABLED}")
if (RANGE_PARTITION_ENABLED)
    add_definitions(-DRANGE_PARTITION_ENABLED)
endif()

option(EXT_TX_PROC_ENABLED "Allows runtime threads to move forward the tx service." ON)
if (EXT_TX_PROC_ENABLED)
    message("EXT_TX_PROC_ENABLED: " ${EXT_TX_PROC_ENABLED})
    add_compile_definitions(EXT_TX_PROC_ENABLED)
endif()

option(DISABLE_CKPT_REPORT "Enable DISABLE_CKPT_REPORT" OFF)
message(STATUS "DISABLE_CKPT_REPORT : ${DISABLE_CKPT_REPORT}")
if (DISABLE_CKPT_REPORT)
    add_definitions(-DDISABLE_CKPT_REPORT)
endif()

option(DISABLE_CODE_LINE_IN_LOG "Enable DISABLE_CODE_LINE_IN_LOG" OFF)
message(STATUS "DISABLE_CODE_LINE_IN_LOG : ${DISABLE_CODE_LINE_IN_LOG}")
if (DISABLE_CODE_LINE_IN_LOG)
    add_definitions(-DDISABLE_CODE_LINE_IN_LOG)
endif()

option(ENABLE_CPU_PROFILING "Enable CPU profiling" OFF)
message("ENABLE_CPU_PROFILING: ${ENABLE_CPU_PROFILING}")

if (ENABLE_CPU_PROFILING)
    find_path(GPERFTOOLS_INCLUDE_DIR NAMES gperftools/heap-profiler.h)
    find_library(GPERFTOOLS_LIBRARIES NAMES profiler)
    include_directories(${GPERFTOOLS_INCLUDE_DIR})
    add_compile_definitions(BRPC_ENABLE_CPU_PROFILER)
endif()

add_definitions("-Wall -g")

# execute_process(
# COMMAND bash -c "find ${PROJECT_SOURCE_DIR}/../.. -type d -regex \".*output/include$\" | head -n1 | xargs dirname | tr -d '\n'"
# OUTPUT_VARIABLE OUTPUT_PATH
# )
# set(CMAKE_PREFIX_PATH ${OUTPUT_PATH})
include(FindThreads)
include(FindProtobuf)

# Search for libthrift* by best effort. If it is not found and brpc is
# compiled with thrift protocol enabled, a link error would be reported.
find_library(THRIFT_LIB NAMES thrift)

if (NOT THRIFT_LIB)
    set(THRIFT_LIB "")
endif ()

find_library(THRIFTNB_LIB NAMES thriftnb)

if (NOT THRIFTNB_LIB)
    set(THRIFTNB_LIB "")
endif ()

find_path(BRPC_INCLUDE_PATH NAMES brpc/server.h)

if (LINK_SO)
    find_library(BRPC_LIB NAMES brpc)
else ()
    find_library(BRPC_LIB NAMES libbrpc.a brpc)
endif ()

if ((NOT BRPC_INCLUDE_PATH) OR (NOT BRPC_LIB))
    message(FATAL_ERROR "Fail to find brpc")
endif ()

include_directories(${BRPC_INCLUDE_PATH})

find_path(GFLAGS_INCLUDE_PATH gflags/gflags.h)
find_library(GFLAGS_LIBRARY NAMES gflags libgflags)

if ((NOT GFLAGS_INCLUDE_PATH) OR (NOT GFLAGS_LIBRARY))
    message(FATAL_ERROR "Fail to find gflags")
endif ()

include_directories(${GFLAGS_INCLUDE_PATH})

if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    include(CheckFunctionExists)
    CHECK_FUNCTION_EXISTS(clock_gettime HAVE_CLOCK_GETTIME)

    if (NOT HAVE_CLOCK_GETTIME)
        set(DEFINE_CLOCK_GETTIME "-DNO_CLOCK_GETTIME_IN_MAC")
    endif ()
endif ()

set(CMAKE_CPP_FLAGS "${DEFINE_CLOCK_GETTIME} -DGFLAGS_NS=${GFLAGS_NS} -g")
#set(CMAKE_CXX_FLAGS "${CMAKE_CPP_FLAGS} -DNDEBUG -O2 -D__const__= -pipe -W -Wall -Wno-unused-parameter -fPIC -fno-omit-frame-pointer")

find_path(LEVELDB_INCLUDE_PATH NAMES leveldb/db.h)
find_library(LEVELDB_LIB NAMES leveldb)

if ((NOT LEVELDB_INCLUDE_PATH) OR (NOT LEVELDB_LIB))
    message(FATAL_ERROR "Fail to find leveldb")
endif ()

include_directories(${LEVELDB_INCLUDE_PATH})

if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(OPENSSL_ROOT_DIR
            "/usr/local/opt/openssl" # Homebrew installed OpenSSL
            )
endif ()

find_package(OpenSSL)
include_directories(${OPENSSL_INCLUDE_DIR})

if (WITH_KV_STORE STREQUAL "DYNAMODB")
  find_package(AWSSDK REQUIRED COMPONENTS dynamodb)
endif ()

# Add RocksDB deps
if(WITH_KV_STORE STREQUAL "ROCKSDB")
  if (ROCKSDB_CLOUD_FS MATCHES "S3|GCS")
    if (ROCKSDB_CLOUD_FS STREQUAL "S3")
        find_path(AWS_CORE_INCLUDE_PATH aws/core/Aws.h)
        if((NOT AWS_CORE_INCLUDE_PATH))
          message(FATAL_ERROR "Fail to find aws/core include path")
        endif()
        message(STATUS "aws/core include path: ${AWS_CORE_INCLUDE_PATH}")

        find_library(AWS_CORE_LIB aws-cpp-sdk-core)
        if((NOT AWS_CORE_LIB ))
          message(FATAL_ERROR "Fail to find aws-cpp-sdk-core lib")
        endif()
        message(STATUS "aws-cpp-sdk-core library: ${AWS_CORE_LIB}")

        find_path(AWS_KINESIS_INCLUDE_PATH aws/kinesis/KinesisClient.h)
        if((NOT AWS_KINESIS_INCLUDE_PATH))
          message(FATAL_ERROR "Fail to find aws/kinesis include path")
        endif()
        message(STATUS "aws/kinesis include path: ${AWS_KINESIS_INCLUDE_PATH}")

        find_library(AWS_KINESIS_LIB aws-cpp-sdk-kinesis)
        if((NOT AWS_KINESIS_LIB))
          message(FATAL_ERROR "Fail to find aws-cpp-sdk-kinesis lib")
        endif()
        message(STATUS "aws-cpp-sdk-kinesis library: ${AWS_KINESIS_LIB}")

        find_path(AWS_KINESIS_INCLUDE_PATH aws/kinesis/KinesisClient.h)
        if((NOT AWS_KINESIS_INCLUDE_PATH))
          message(FATAL_ERROR "Fail to find aws/kinesis include path")
        endif()
        message(STATUS "aws/kinesis include path: ${AWS_KINESIS_INCLUDE_PATH}")

        find_library(AWS_KINESIS_LIB aws-cpp-sdk-kinesis)
        if((NOT AWS_KINESIS_LIB))
          message(FATAL_ERROR "Fail to find aws-cpp-sdk-kinesis lib")
        endif()
        message(STATUS "aws-cpp-sdk-kinesis library: ${AWS_KINESIS_LIB}")

        find_path(AWS_S3_INCLUDE_PATH aws/s3/S3Client.h)
        if((NOT AWS_S3_INCLUDE_PATH))
          message(FATAL_ERROR "Fail to find aws/s3 include path")
        endif()
        message(STATUS "aws/s3 include path: ${AWS_S3_INCLUDE_PATH}")

        find_library(AWS_S3_LIB aws-cpp-sdk-s3)
        if((NOT AWS_S3_LIB ))
          message(FATAL_ERROR "Fail to find aws-cpp-sdk-s3 lib")
        endif()
        message(STATUS "aws-cpp-sdk-s3 library: ${AWS_S3_LIB}")

        set(ROCKSDB_INCLUDE_PATH ${ROCKSDB_INCLUDE_PATH} ${AWS_CORE_INCLUDE_PATH})
        set(ROCKSDB_INCLUDE_PATH ${ROCKSDB_INCLUDE_PATH} ${AWS_KINESIS_INCLUDE_PATH})
        set(ROCKSDB_INCLUDE_PATH ${ROCKSDB_INCLUDE_PATH} ${AWS_S3_INCLUDE_PATH})

        set(ROCKSDB_LIBRARIES ${ROCKSDB_LIBRARIES} ${AWS_CORE_LIB})
        set(ROCKSDB_LIBRARIES ${ROCKSDB_LIBRARIES} ${AWS_KINESIS_LIB})
        set(ROCKSDB_LIBRARIES ${ROCKSDB_LIBRARIES} ${AWS_S3_LIB})

        find_library(ROCKSDB_CLOUD_LIB NAMES rocksdb-cloud-aws)
      elseif (ROCKSDB_CLOUD_FS STREQUAL "GCS")
        find_path(GCP_CS_INCLUDE_PATH google/cloud/storage/client.h)
        if((NOT GCP_CS_INCLUDE_PATH))
          message(FATAL_ERROR "Fail to find google/cloud/storage include path")
        endif()
        message(STATUS "google/cloud/storage include path: ${GCP_CS_INCLUDE_PATH}")

        find_library(GCP_COMMON_LIB google_cloud_cpp_common)
        if((NOT GCP_COMMON_LIB))
          message(FATAL_ERROR "Fail to find google_cloud_cpp_common lib")
        endif()
        message(STATUS "google_cloud_cpp_common library: ${GCP_COMMON_LIB}")

        find_library(GCP_CS_LIB google_cloud_cpp_storage)
        if((NOT GCP_CS_LIB))
          message(FATAL_ERROR "Fail to find google_cloud_cpp_storage lib")
        endif()
        message(STATUS "google_cloud_cpp_storage library: ${GCP_CS_LIB}")

        set(ROCKSDB_LIBRARIES ${ROCKSDB_LIBRARIES} ${GCP_COMMON_LIB})
        set(ROCKSDB_LIBRARIES ${ROCKSDB_LIBRARIES} ${GCP_CS_LIB})

        find_library(ROCKSDB_CLOUD_LIB NAMES rocksdb-cloud-gcp)
      endif ()

      find_path(ROCKSDB_CLOUD_INCLUDE_PATH NAMES rocksdb/db.h PATH_SUFFIXES "rocksdb_cloud_header")
      if (NOT ROCKSDB_CLOUD_INCLUDE_PATH)
        message(FATAL_ERROR "Fail to find RocksDB Cloud include path")
      endif ()
      message(STATUS "ROCKSDB_CLOUD_INCLUDE_PATH: ${ROCKSDB_CLOUD_INCLUDE_PATH}")
      set(ROCKSDB_INCLUDE_PATH ${ROCKSDB_INCLUDE_PATH} ${ROCKSDB_CLOUD_INCLUDE_PATH})
      include_directories(${ROCKSDB_INCLUDE_PATH})

      if (NOT ROCKSDB_CLOUD_LIB)
        message(FATAL_ERROR "Fail to find RocksDB Cloud lib path")
      endif ()
      message(STATUS "ROCKSDB_CLOUD_LIB: ${ROCKSDB_CLOUD_LIB}")
      set(ROCKSDB_LIBRARIES ${ROCKSDB_LIBRARIES} ${ROCKSDB_CLOUD_LIB})
    else ()
      find_path(ROCKSDB_INCLUDE_PATH NAMES rocksdb/db.h)
      if (NOT ROCKSDB_INCLUDE_PATH)
    	message(FATAL_ERROR "Fail to find RocksDB include path")
      endif ()
      message(STATUS "ROCKSDB_INCLUDE_PATH: ${ROCKSDB_INCLUDE_PATH}")
      include_directories(${ROCKSDB_INCLUDE_PATH})

      find_library(ROCKSDB_LIB NAMES rocksdb)
      if (NOT ROCKSDB_LIB)
    	  message(FATAL_ERROR "Fail to find RocksDB lib path")
      endif ()
      message(STATUS "ROCKSDB_LIB: ${ROCKSDB_LIB}")
      set(ROCKSDB_LIBRARIES ${ROCKSDB_LIBRARIES} ${ROCKSDB_LIB})

    endif()
endif ()

set(DYNAMIC_LIB
        ${CMAKE_THREAD_LIBS_INIT}
        ${GFLAGS_LIBRARY}
        ${PROTOBUF_LIBRARIES}
        ${GPERFTOOLS_LIBRARIES}
        ${LEVELDB_LIB}
        ${OPENSSL_CRYPTO_LIBRARY}
        ${OPENSSL_SSL_LIBRARY}
        ${THRIFT_LIB}
        ${THRIFTNB_LIB}
        dl
        )

set(DS_PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include/store_handler)
message(NOTICE "data store service proto dir: ${DS_PROTO_DIR}")
set(PROTO_SRC ${DS_PROTO_DIR})
set(PROTO_NAME ds_request)
execute_process(
	COMMAND protoc ./${PROTO_NAME}.proto --cpp_out=./
	WORKING_DIRECTORY ${PROTO_SRC}
)

if (BRPC_WITH_GLOG)
    find_path(GLOG_INCLUDE_PATH NAMES glog/logging.h)
    find_library(GLOG_LIB NAMES glog VERSION ">=0.6.0" REQUIRED)

    if ((NOT GLOG_INCLUDE_PATH) OR(NOT GLOG_LIB))
        message(FATAL_ERROR "Fail to find glog")
    endif ()

    include_directories(${GLOG_INCLUDE_PATH})
    set(DYNAMIC_LIB ${DYNAMIC_LIB} ${GLOG_LIB})
endif ()


# #######
if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(DYNAMIC_LIB ${DYNAMIC_LIB}
            pthread
            "-framework CoreFoundation"
            "-framework CoreGraphics"
            "-framework CoreData"
            "-framework CoreText"
            "-framework Security"
            "-framework Foundation"
            "-Wl,-U,_MallocExtension_ReleaseFreeMemory"
            "-Wl,-U,_ProfilerStart"
            "-Wl,-U,_ProfilerStop")
endif ()

if(WITH_KV_STORE STREQUAL "CASSANDRA")
    INCLUDE(build_cass_driver.cmake)
endif ()

add_subdirectory(tx_service/abseil-cpp)

INCLUDE(build_tx_service.cmake)
if (WITH_LOG_SERVICE)
INCLUDE(build_log_service.cmake)
endif ()
INCLUDE(build_eloq_metrics.cmake)

SET(RESMONOGRAPH_SOURCES
        src/redis/commands.c
        src/redis/zmalloc.c
        src/redis/monotonic.c
        src/redis/mt19937-64.c
        src/redis/siphash.c
        src/redis/dict.c
        src/redis/sha1.c
        src/eloq_algorithm.cpp
        src/eloq_catalog_factory.cpp
        src/eloq_key.cpp
        src/eloq_string.cpp
        src/ini.c
        src/INIReader.cpp
        src/lua_interpreter.cpp
        src/metrics_registry_impl.cpp
        src/pub_sub_manager.cpp
        src/redis_service.cpp
        src/redis_command.cpp
        src/redis_object.cpp
        src/redis_string_object.cpp
        src/redis_list_object.cpp
        src/redis_handler.cpp
        src/redis_hash_object.cpp
        src/redis_replier.cpp
        src/redis_errors.cpp
        src/redis_zset_object.cpp
        src/redis_set_object.cpp
        src/redis_string_match.cpp
        src/redis_connection_context.cpp
        src/redis_stats.cpp
        )
SET(RESMONOGRAPH_LIBRARY txservice ${METRICS_LIB})
if (WITH_LOG_SERVICE)
    SET(RESMONOGRAPH_LIBRARY ${RESMONOGRAPH_LIBRARY} logservice)
endif ()

# crcspeed
SET(CRCSPEED_SOURCES crcspeed/crcspeed.c crcspeed/crc16speed.c crcspeed/crc64speed.c)
SET(RESMONOGRAPH_SOURCES ${RESMONOGRAPH_SOURCES} ${CRCSPEED_SOURCES})

if (WITH_KV_STORE STREQUAL "CASSANDRA")
    # cass handler
    SET(RESMONOGRAPH_SOURCES
        ${RESMONOGRAPH_SOURCES}
        src/store_handler/cass_handler.cpp
        src/store_handler/cass_scanner.cpp
        src/store_handler/cass_handler_typed.cpp
    )
elseif (WITH_KV_STORE STREQUAL "DYNAMODB")
    SET(RESMONOGRAPH_SOURCES ${RESMONOGRAPH_SOURCES}
        src/store_handler/dynamo_handler.cpp
        src/store_handler/dynamo_scanner.cpp
        src/store_handler/dynamo_handler_typed.cpp
        )
elseif (WITH_KV_STORE STREQUAL "ROCKSDB")
    SET(RESMONOGRAPH_SOURCES ${RESMONOGRAPH_SOURCES}
        include/store_handler/ds_request.pb.cc
        src/store_handler/store_util.cpp
        src/store_handler/rocksdb_config.cpp
        src/store_handler/rocksdb_handler.cpp
        src/store_handler/rocksdb_scanner.cpp
        src/store_handler/rocksdb_cloud_data_store.cpp
        src/store_handler/data_store_service_config.cpp
        src/store_handler/data_store_service.cpp
        src/store_handler/data_store_service_client.cpp
        src/store_handler/data_store_service_scanner.cpp
        src/store_handler/data_store_fault_inject.cpp
        src/store_handler/thread_worker_pool.cpp
       )
endif ()


if (WITH_KV_STORE STREQUAL "CASSANDRA")
    # cass handler
    SET(RESMONOGRAPH_LIBRARY
        ${RESMONOGRAPH_LIBRARY}
        cassandra_static
    )
elseif (WITH_KV_STORE STREQUAL "DYNAMODB")
    SET(RESMONOGRAPH_LIBRARY ${RESMONOGRAPH_LIBRARY} ${AWSSDK_LINK_LIBRARIES})
elseif (WITH_KV_STORE STREQUAL "ROCKSDB")
    SET(RESMONOGRAPH_LIBRARY ${RESMONOGRAPH_LIBRARY} ${ROCKSDB_LIBRARIES})
endif ()

include_directories(
        ${PROJECT_SOURCE_DIR}/
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/include/redis
        ${PROJECT_SOURCE_DIR}/include/store_handler
        ${PROJECT_SOURCE_DIR}/tx_service/abseil-cpp
        ${PROJECT_SOURCE_DIR}/eloq_metrics/include
        ${PROJECT_SOURCE_DIR}/tx_service/include
        ${PROJECT_SOURCE_DIR}/tx_service/include/cc
        ${PROJECT_SOURCE_DIR}/tx_service/include/remote
        ${PROJECT_SOURCE_DIR}/tx_service/include/fault
        ${PROJECT_SOURCE_DIR}/tx_service/include/store
        ${PROJECT_SOURCE_DIR}/tx_service/tx-log-protos
if (WITH_LOG_SERVICE)
        ${PROJECT_SOURCE_DIR}/log_service/include
endif ()
)

set(ABSEIL
        absl::btree
        absl::flat_hash_map
        absl::span
        )

add_subdirectory(lua)
add_subdirectory(fpconv)

# add_executable(redis_press redis_press.cpp)
add_executable(eloqkv src/redis_server.cpp ${RESMONOGRAPH_SOURCES})
target_link_libraries(eloqkv ${DYNAMIC_LIB} ${RESMONOGRAPH_LIBRARY} ${ABSEIL} lua fpconv)

if (WITH_KV_STORE STREQUAL "ROCKSDB")
add_executable(eloqkv_to_aof src/tools/eloqkv2aof/eloqkv2aof.cpp ${RESMONOGRAPH_SOURCES})
target_link_libraries(eloqkv_to_aof ${DYNAMIC_LIB} ${RESMONOGRAPH_LIBRARY} ${ABSEIL} lua fpconv)
add_executable(eloqkv_to_rdb src/tools/eloqkv2rdb/eloqkv2rdb.cpp ${RESMONOGRAPH_SOURCES})
target_link_libraries(eloqkv_to_rdb ${DYNAMIC_LIB} ${RESMONOGRAPH_LIBRARY} ${ABSEIL} lua fpconv)
endif()
# add_executable(test_io_write src/tools/eloqkv2aof/test_io_write.cpp ${RESMONOGRAPH_SOURCES})
# target_link_libraries(test_io_write ${DYNAMIC_LIB} ${RESMONOGRAPH_LIBRARY} ${ABSEIL} lua fpconv)

#set(AUX_LIB readline ncurses)
#add_executable(eloqkv-client src/redis_cli.cpp)
#target_link_libraries(eloqkv-client ${BRPC_LIB} ${DYNAMIC_LIB} ${AUX_LIB})

#target_compile_options(redis_server PRIVATE -fsanitize=address -fno-omit-frame-pointer)
#target_link_options(redis_server PRIVATE -fsanitize=address -fno-omit-frame-pointer)

option(BUILD_WITH_TESTS "Run unit-tests" OFF)
if(${BUILD_WITH_TESTS})
find_package(Catch2 REQUIRED)

add_executable(object_serialize_deserialize_test ./tests/unit/mono/object_serialize_deserialize_test.cpp ${RESMONOGRAPH_SOURCES})
target_link_libraries(object_serialize_deserialize_test PRIVATE Catch2::Catch2WithMain ${ABSEIL} ${LOG_LIB} ${RESMONOGRAPH_LIBRARY} lua fpconv ${DYNAMIC_LIB})

add_test(NAME serialize/deserialize_test COMMAND object_serialize_deserialize_test WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
enable_testing()
endif ()

set_target_properties(eloqkv PROPERTIES
	BUILD_RPATH "$ORIGIN/../lib"
	INSTALL_RPATH "$ORIGIN/../lib"
        INSTALL_RPATH_USE_LINK_PATH TRUE)

install(TARGETS eloqkv
        RUNTIME DESTINATION bin)

if (WITH_KV_STORE STREQUAL "ROCKSDB")
install(TARGETS eloqkv_to_aof
        RUNTIME DESTINATION bin)
install(TARGETS eloqkv_to_rdb
        RUNTIME DESTINATION bin)
endif()
