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
cmake_minimum_required(VERSION 3.12)
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

add_compile_definitions(ELOQ_MODULE_ELOQKV)

# Option to build as library for converged binary
option(BUILD_ELOQKV_AS_LIBRARY "Build eloqkv as library instead of executable" OFF)

set(WITH_DATA_STORE "ELOQDSS_ROCKSDB" CACHE STRING "Which key-value storage to use")
set_property(CACHE WITH_DATA_STORE PROPERTY STRINGS "DYNAMODB" "BIGTABLE" "ELOQDSS_ROCKSDB_CLOUD_S3" "ELOQDSS_ELOQSTORE" "ELOQDSS_ROCKSDB")
option(INI_USE_HEAP "Whether parse ini on heap" ON)

if(INI_USE_HEAP)
    add_definitions(-DINI_USE_STACK=0)
    add_definitions(-DINI_ALLOW_REALLOC=1)
endif()

IF((CMAKE_BUILD_TYPE STREQUAL "") OR (CMAKE_BUILD_TYPE MATCHES "Debug") OR (CMAKE_BUILD_TYPE MATCHES "DEBUG"))
    message(NOTICE "WITH_FAULT_INJECT: ON")
    add_definitions(-DWITH_FAULT_INJECT)
ELSE()
    message(NOTICE "WITH_FAULT_INJECT: OFF")
    remove_definitions(-DWITH_FAULT_INJECT)
ENDIF()

option(LINK_SO "Whether examples are linked dynamically" ON)

option(BRPC_WITH_GLOG "With glog" ON)

option(ABSL_PROPAGATE_CXX_STD "ABSL PROPAGATE CXX STD" ON)

option(WITH_ASAN "Enable memory sanitize" OFF)
message(NOTICE "WITH ASAN: ${WITH_ASAN}")

option(WITH_LOG_SERVICE "Compile with log service or not." ON)
message("WITH_LOG_SERVICE: " ${WITH_LOG_SERVICE})

if(WITH_LOG_SERVICE)
    add_compile_definitions(WITH_LOG_SERVICE=1)
endif()

option(OPEN_LOG_SERVICE "Compile with open log service." ON)
message("OPEN_LOG_SERVICE: " ${OPEN_LOG_SERVICE})

if(OPEN_LOG_SERVICE)
    add_compile_definitions(OPEN_LOG_SERVICE=1)
endif()

option(EXT_TX_PROC_ENABLED "Allows runtime threads to move forward the tx service." ON)

if(EXT_TX_PROC_ENABLED)
    message("EXT_TX_PROC_ENABLED: " ${EXT_TX_PROC_ENABLED})
    add_compile_definitions(EXT_TX_PROC_ENABLED)
endif()

option(ELOQ_MODULE_ENABLED "Enable EloqModule" ON)
message("ELOQ_MODULE_ENABLED: " ${ELOQ_MODULE_ENABLED})

if(ELOQ_MODULE_ENABLED)
    add_compile_definitions(ELOQ_MODULE_ENABLED)
endif()

if(ELOQ_MODULE_ENABLED AND NOT EXT_TX_PROC_ENABLED)
    message(FATAL_ERROR "ELOQ_MODULE_ENABLED requires EXT_TX_PROC_ENABLED to be ON.")
endif()

option(DISABLE_CKPT_REPORT "Enable DISABLE_CKPT_REPORT" OFF)
message(STATUS "DISABLE_CKPT_REPORT : ${DISABLE_CKPT_REPORT}")

if(DISABLE_CKPT_REPORT)
    add_definitions(-DDISABLE_CKPT_REPORT)
endif()

option(DISABLE_CODE_LINE_IN_LOG "Enable DISABLE_CODE_LINE_IN_LOG" OFF)
message(STATUS "DISABLE_CODE_LINE_IN_LOG : ${DISABLE_CODE_LINE_IN_LOG}")

if(DISABLE_CODE_LINE_IN_LOG)
    add_definitions(-DDISABLE_CODE_LINE_IN_LOG)
endif()

option(ENABLE_CPU_PROFILING "Enable CPU profiling" OFF)
message("ENABLE_CPU_PROFILING: ${ENABLE_CPU_PROFILING}")

if(ENABLE_CPU_PROFILING)
    find_path(GPERFTOOLS_INCLUDE_DIR NAMES gperftools/heap-profiler.h)
    find_library(GPERFTOOLS_LIBRARIES NAMES profiler)
    include_directories(${GPERFTOOLS_INCLUDE_DIR})
    add_compile_definitions(BRPC_ENABLE_CPU_PROFILER)
endif()

option(VECTOR_INDEX_ENABLED "Enable vector index" OFF)
message("VECTOR_INDEX_ENABLED: ${VECTOR_INDEX_ENABLED}")
# Add compile definitions based on options
if(WITH_DATA_STORE STREQUAL "DYNAMODB")
    set(KV_STORAGE_VAL 1 CACHE STRING "dynamodb" FORCE)
    add_compile_definitions(DATA_STORE_TYPE_DYNAMODB)
    find_package(AWSSDK REQUIRED COMPONENTS dynamodb)
elseif(WITH_DATA_STORE STREQUAL "ROCKSDB")
    add_compile_definitions(DATA_STORE_TYPE_ROCKSDB)
elseif(WITH_DATA_STORE STREQUAL "BIGTABLE")
    set(KV_STORAGE_VAL 2 CACHE STRING "big table" FORCE)
    add_compile_definitions(DATA_STORE_TYPE_BIGTABLE)
    find_package(google_cloud_cpp_bigtable REQUIRED)
elseif(WITH_DATA_STORE STREQUAL "ELOQDSS_ROCKSDB_CLOUD_S3")
    set(KV_STORAGE_VAL 3 CACHE STRING "eloq_ds" FORCE)
    add_compile_definitions(DATA_STORE_TYPE_ELOQDSS_ROCKSDB_CLOUD_S3)
elseif(WITH_DATA_STORE STREQUAL "ELOQDSS_ROCKSDB")
    set(KV_STORAGE_VAL 3 CACHE STRING "eloq_ds" FORCE)
    add_compile_definitions(DATA_STORE_TYPE_ELOQDSS_ROCKSDB)
elseif(WITH_DATA_STORE STREQUAL "ELOQDSS_ELOQSTORE")
    set(KV_STORAGE_VAL 3 CACHE STRING "eloq_ds" FORCE)
    add_compile_definitions(DATA_STORE_TYPE_ELOQDSS_ELOQSTORE)
    option(WITH_TXSERVICE "Whether compile eloqstore with txservice" ON)
    set(ELOQSTORE_PARENT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/store_handler/eloq_data_store_service CACHE PATH "EloqStore parent directory")
    INCLUDE(${CMAKE_CURRENT_SOURCE_DIR}/store_handler/eloq_data_store_service/build_eloq_store.cmake)
else()
    message(FATAL_ERROR "Unset WITH_DATA_STORE")
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

if(NOT THRIFT_LIB)
    set(THRIFT_LIB "")
endif()

find_library(THRIFTNB_LIB NAMES thriftnb)

if(NOT THRIFTNB_LIB)
    set(THRIFTNB_LIB "")
endif()

find_path(BRPC_INCLUDE_PATH NAMES brpc/server.h)

if(LINK_SO)
    find_library(BRPC_LIB NAMES brpc)
else()
    find_library(BRPC_LIB NAMES libbrpc.a brpc)
endif()

if((NOT BRPC_INCLUDE_PATH) OR(NOT BRPC_LIB))
    message(FATAL_ERROR "Fail to find brpc")
endif()

include_directories(${BRPC_INCLUDE_PATH})

find_path(GFLAGS_INCLUDE_PATH gflags/gflags.h)
find_library(GFLAGS_LIBRARY NAMES gflags libgflags)

if((NOT GFLAGS_INCLUDE_PATH) OR(NOT GFLAGS_LIBRARY))
    message(FATAL_ERROR "Fail to find gflags")
endif()

include_directories(${GFLAGS_INCLUDE_PATH})

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    include(CheckFunctionExists)
    CHECK_FUNCTION_EXISTS(clock_gettime HAVE_CLOCK_GETTIME)

    if(NOT HAVE_CLOCK_GETTIME)
        set(DEFINE_CLOCK_GETTIME "-DNO_CLOCK_GETTIME_IN_MAC")
    endif()
endif()

set(CMAKE_CXX_FLAGS "${DEFINE_CLOCK_GETTIME} -DGFLAGS_NS=${GFLAGS_NS} -g")

# set(CMAKE_CXX_FLAGS "${CMAKE_CPP_FLAGS} -DNDEBUG -O2 -D__const__= -pipe -W -Wall -Wno-unused-parameter -fPIC -fno-omit-frame-pointer")
if(WITH_ASAN)
    # Add compile flags
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fno-omit-frame-pointer")

    # Add link flags for executables and shared libraries
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
endif()

find_path(LEVELDB_INCLUDE_PATH NAMES leveldb/db.h)
find_library(LEVELDB_LIB NAMES leveldb)

if((NOT LEVELDB_INCLUDE_PATH) OR(NOT LEVELDB_LIB))
    message(FATAL_ERROR "Fail to find leveldb")
endif()

include_directories(${LEVELDB_INCLUDE_PATH})

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(OPENSSL_ROOT_DIR
        "/usr/local/opt/openssl" # Homebrew installed OpenSSL
    )
endif()

find_package(OpenSSL)
include_directories(${OPENSSL_INCLUDE_DIR})

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

if(BRPC_WITH_GLOG)
    find_path(GLOG_INCLUDE_PATH NAMES glog/logging.h)
    find_library(GLOG_LIB NAMES glog VERSION ">=0.6.0" REQUIRED)

    if((NOT GLOG_INCLUDE_PATH) OR(NOT GLOG_LIB))
        message(FATAL_ERROR "Fail to find glog")
    endif()

    include_directories(${GLOG_INCLUDE_PATH})
    set(DYNAMIC_LIB ${DYNAMIC_LIB} ${GLOG_LIB})
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
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
endif()

if(VECTOR_INDEX_ENABLED)
    add_compile_definitions(VECTOR_INDEX_ENABLED)
    # Add usearch dependency for vector indexing
    include(FetchContent)
    FetchContent_Declare(
        usearch
        GIT_REPOSITORY https://github.com/unum-cloud/usearch.git
        GIT_TAG        v2.21.0
    )
    FetchContent_MakeAvailable(usearch)
    set(USEARCH_INCLUDE_DIR
       ${usearch_SOURCE_DIR}/include
       ${usearch_SOURCE_DIR}/fp16/include
    )
    include_directories(${USEARCH_INCLUDE_DIR})
    
    # Add nlohmann/json dependency for vector indexing
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.12.0
    )
    FetchContent_MakeAvailable(nlohmann_json)
    set(NLOHMANN_JSON_INCLUDE_DIR ${nlohmann_json_SOURCE_DIR}/single_include)
    include_directories(${NLOHMANN_JSON_INCLUDE_DIR})
    
    # CURL
    find_package(CURL REQUIRED)
    include_directories(${CURL_INCLUDE_DIRS})
    set(DYNAMIC_LIB ${DYNAMIC_LIB} ${CURL_LIBRARIES})
endif()

# Build data_substrate library first
if(NOT BUILD_ELOQKV_AS_LIBRARY)
    # Build data_substrate library first (standalone mode)
    add_subdirectory(data_substrate)
else()
    # Use parent data_substrate (converged mode)
    message(STATUS "EloqKV: Using parent data_substrate")
endif()

SET(RESELOQ_RESOURCES
    src/redis/commands.c
    src/redis/zmalloc.c
    src/redis/monotonic.c
    src/redis/mt19937-64.c
    src/redis/siphash.c
    src/redis/dict.c
    src/redis/sha1.c
    src/eloq_algorithm.cpp
    src/eloqkv_catalog_factory.cpp
    src/eloqkv_key.cpp
    src/eloq_string.cpp
    src/lua_interpreter.cpp
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

if(VECTOR_INDEX_ENABLED)
    SET(RESELOQ_RESOURCES ${RESELOQ_RESOURCES}
        src/vector/hnsw_vector_index.cpp
        src/vector/vector_handler.cpp
        src/vector/log_object.cpp
        src/vector/vector_type.cpp
        src/vector/cloud_manager.cpp
        src/vector/predicate.cpp
    )
endif()

SET(INI_SOURCES src/ini.c src/INIReader.cpp)

if(NOT(WITH_DATA_STORE STREQUAL "ELOQDSS_ELOQSTORE"))
    SET(RESELOQ_RESOURCES ${RESELOQ_RESOURCES} ${INI_SOURCES})
endif()

SET(RESELOQ_LIBRARY ${RESELOQ_LIBRARY} data_substrate)

# crcspeed
SET(CRCSPEED_SOURCES crcspeed/crcspeed.c crcspeed/crc16speed.c crcspeed/crc64speed.c)
SET(RESELOQ_RESOURCES ${RESELOQ_RESOURCES} ${CRCSPEED_SOURCES})

# Set up base include directories
set(INCLUDE_DIRS
    ${PROJECT_SOURCE_DIR}/
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/include/redis
)

if(VECTOR_INDEX_ENABLED)
    set(INCLUDE_DIRS ${INCLUDE_DIRS} ${PROJECT_SOURCE_DIR}/include/vector)
endif()

# Apply all includes at once
include_directories(${INCLUDE_DIRS})

set(ABSEIL
    absl::btree
    absl::flat_hash_map
    absl::span
)

add_subdirectory(lua)
add_subdirectory(fpconv)

# Add library target for converged binary
if(BUILD_ELOQKV_AS_LIBRARY)
    # Build as static library for converged binary
    ADD_LIBRARY(eloqkv_lib STATIC ${RESELOQ_RESOURCES})
    TARGET_LINK_LIBRARIES(eloqkv_lib ${DYNAMIC_LIB} ${RESELOQ_LIBRARY} ${ABSEIL} lua fpconv)
    
    # Export include directories
    TARGET_INCLUDE_DIRECTORIES(eloqkv_lib PUBLIC
        ${PROJECT_SOURCE_DIR}/
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/include/redis
    )
    
    # Export the library for parent project
    set(ELOQKV_LIBRARY eloqkv_lib PARENT_SCOPE)
    set(ELOQKV_INCLUDE_DIRS 
        ${PROJECT_SOURCE_DIR}/
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/include/redis
        PARENT_SCOPE
    )
endif()

# add_executable(redis_press redis_press.cpp)
# Only build executable in standalone mode
if(NOT BUILD_ELOQKV_AS_LIBRARY)
    add_executable(eloqkv src/redis_server.cpp ${RESELOQ_RESOURCES})
    target_link_libraries(eloqkv ${DYNAMIC_LIB} ${RESELOQ_LIBRARY} ${ABSEIL} lua fpconv)
endif()

option(WITH_BOOST_STACKTRACE "With boost stack trace." OFF)
message("WITH_BOOST_STACKTRACE: " ${WITH_BOOST_STACKTRACE})

if(WITH_BOOST_STACKTRACE)
    # target_compile_options(eloqkv PRIVATE -fno-pie)
    # target_link_options(eloqkv PRIVATE -no-pie)
    find_library(BOOST_STACKTRACE_LIB boost_stacktrace_backtrace)
    target_link_libraries(eloqkv ${BOOST_STACKTRACE_LIB})
    add_compile_definitions(BOOST_STACKTRACE_LINK)
endif()

if(WITH_DATA_STORE STREQUAL "ROCKSDB" AND NOT BUILD_ELOQKV_AS_LIBRARY)
    add_executable(eloqkv_to_aof src/tools/eloqkv2aof/eloqkv2aof.cpp ${RESELOQ_RESOURCES})
    target_link_libraries(eloqkv_to_aof ${DYNAMIC_LIB} ${RESELOQ_LIBRARY} ${ABSEIL} lua fpconv)
    add_executable(eloqkv_to_rdb src/tools/eloqkv2rdb/eloqkv2rdb.cpp ${RESELOQ_RESOURCES})
    target_link_libraries(eloqkv_to_rdb ${DYNAMIC_LIB} ${RESELOQ_LIBRARY} ${ABSEIL} lua fpconv)
endif()

# add_executable(test_io_write src/tools/eloqkv2aof/test_io_write.cpp ${RESELOQ_RESOURCES})
# target_link_libraries(test_io_write ${DYNAMIC_LIB} ${RESELOQ_LIBRARY} ${ABSEIL} lua fpconv)

# set(AUX_LIB readline ncurses)
# add_executable(eloqkv-client src/redis_cli.cpp)
# target_link_libraries(eloqkv-client ${BRPC_LIB} ${DYNAMIC_LIB} ${AUX_LIB})
option(BUILD_WITH_TESTS "Run unit-tests" OFF)

if(${BUILD_WITH_TESTS})
    find_package(Catch2 REQUIRED)

    add_executable(object_serialize_deserialize_test ./tests/unit/mono/object_serialize_deserialize_test.cpp ${RESELOQ_RESOURCES})
    target_link_libraries(object_serialize_deserialize_test PRIVATE Catch2::Catch2WithMain ${ABSEIL} ${LOG_LIB} ${RESELOQ_LIBRARY} lua fpconv ${DYNAMIC_LIB})

    add_test(NAME serialize/deserialize_test COMMAND object_serialize_deserialize_test WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    enable_testing()

    if (VECTOR_INDEX_ENABLED)
        set(VECTOR_INCLUDE_DIR ${PROJECT_SOURCE_DIR}/include/vector)
        add_subdirectory(src/vector)
    endif()
endif()

if(NOT BUILD_ELOQKV_AS_LIBRARY)
    set_target_properties(eloqkv PROPERTIES
        BUILD_RPATH "$ORIGIN/../lib"
        INSTALL_RPATH "$ORIGIN/../lib"
        INSTALL_RPATH_USE_LINK_PATH TRUE)
    
    install(TARGETS eloqkv
        RUNTIME DESTINATION bin)
endif()

if(WITH_DATA_STORE STREQUAL "ROCKSDB" AND NOT BUILD_ELOQKV_AS_LIBRARY)
    install(TARGETS eloqkv_to_aof
        RUNTIME DESTINATION bin)
    install(TARGETS eloqkv_to_rdb
        RUNTIME DESTINATION bin)
endif()
