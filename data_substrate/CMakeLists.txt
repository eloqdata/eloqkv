cmake_minimum_required(VERSION 3.8)
project(data_substrate)

# Set library name
set(DATA_SUBSTRATE_LIB "data_substrate" CACHE STRING "Data substrate library name")
set(METRICS_LIB "eloq-metrics" CACHE STRING "Metrics library name")

# Options from original eloq CMakeLists
set(WITH_DATA_STORE "ELOQDSS_ROCKSDB" CACHE STRING "Which key-value storage to use")
set_property(CACHE WITH_DATA_STORE PROPERTY STRINGS "DYNAMODB" "BIGTABLE" "ELOQDSS_ROCKSDB_CLOUD_S3" "ELOQDSS_ELOQSTORE" "ELOQDSS_ROCKSDB")

option(WITH_LOG_SERVICE "Compile with built-in log service." ON)
option(OPEN_LOG_SERVICE "Compile with open log service." ON)
option(SMALL_RANGE "Whether enable small range" OFF)
option(STATISTICS "Whether enable table statistics" ON)
option(EXT_TX_PROC_ENABLED "Allows external threads to move forward the tx service." ON)
option(ELOQ_MODULE_ENABLED "Register the tx service as an ELOQ module." OFF)


message(NOTICE "WITH_DATA_STORE: ${WITH_DATA_STORE}")
message(NOTICE "WITH_LOG_SERVICE: ${WITH_LOG_SERVICE}")
message(NOTICE "OPEN_LOG_SERVICE: ${OPEN_LOG_SERVICE}")
option(USE_ONE_ELOQDSS_PARTITION_ENABLED "Whether use one partition for kv store" OFF)
message(NOTICE "USE_ONE_ELOQDSS_PARTITION_ENABLED : ${USE_ONE_ELOQDSS_PARTITION_ENABLED}")

if(USE_ONE_ELOQDSS_PARTITION_ENABLED)
    add_compile_definitions(USE_ONE_ELOQDSS_PARTITION)
endif()
# Add compile definitions based on options
if(WITH_DATA_STORE STREQUAL "DYNAMODB")
    set(KV_STORAGE_VAL 1 CACHE STRING "dynamodb" FORCE)
    add_compile_definitions(DATA_STORE_TYPE_DYNAMODB)
    find_package(AWSSDK REQUIRED COMPONENTS dynamodb)
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

# Find RocksDB if needed
if(WITH_DATA_STORE STREQUAL "ELOQDSS_ROCKSDB")
    find_path(ROCKSDB_INCLUDE_PATH NAMES rocksdb/db.h)
    if(NOT ROCKSDB_INCLUDE_PATH)
        message(FATAL_ERROR "Fail to find RocksDB include path")
    endif()
    message(STATUS "ROCKSDB_INCLUDE_PATH: ${ROCKSDB_INCLUDE_PATH}")
    include_directories(${ROCKSDB_INCLUDE_PATH})

    find_library(ROCKSDB_LIB NAMES rocksdb)
    if(NOT ROCKSDB_LIB)
        message(FATAL_ERROR "Fail to find RocksDB lib path")
    endif()
    message(STATUS "ROCKSDB_LIB: ${ROCKSDB_LIB}")
    set(ROCKSDB_LIBRARIES ${ROCKSDB_LIBRARIES} ${ROCKSDB_LIB})
endif()

# Handle RocksDB Cloud S3/GCS configurations
if((WITH_DATA_STORE STREQUAL "ROCKSDB_CLOUD_S3") OR (WITH_DATA_STORE STREQUAL "ROCKSDB_CLOUD_GCS") 
          OR (WITH_DATA_STORE STREQUAL "ELOQDSS_ROCKSDB_CLOUD_S3") OR (WITH_DATA_STORE STREQUAL "ELOQDSS_ROCKSDB_CLOUD_GCS"))
    if((WITH_DATA_STORE STREQUAL "ROCKSDB_CLOUD_S3") OR (WITH_DATA_STORE STREQUAL "ELOQDSS_ROCKSDB_CLOUD_S3"))
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
        set(ROCKSDB_INCLUDE_PATH ${ROCKSDB_INCLUDE_PATH} ${AWS_S3_INCLUDE_PATH})

        set(ROCKSDB_LIBRARIES ${ROCKSDB_LIBRARIES} ${AWS_CORE_LIB})
        set(ROCKSDB_LIBRARIES ${ROCKSDB_LIBRARIES} ${AWS_S3_LIB})

        find_library(ROCKSDB_CLOUD_LIB NAMES rocksdb-cloud-aws)
    endif()

    # Common RocksDB Cloud setup
    find_path(ROCKSDB_CLOUD_INCLUDE_PATH NAMES rocksdb/db.h PATH_SUFFIXES "rocksdb_cloud_header")
    if(NOT ROCKSDB_CLOUD_INCLUDE_PATH)
        message(FATAL_ERROR "Fail to find RocksDB Cloud include path")
    endif()
    message(STATUS "ROCKSDB_CLOUD_INCLUDE_PATH: ${ROCKSDB_CLOUD_INCLUDE_PATH}")
    set(ROCKSDB_INCLUDE_PATH ${ROCKSDB_INCLUDE_PATH} ${ROCKSDB_CLOUD_INCLUDE_PATH})
    include_directories(${ROCKSDB_INCLUDE_PATH})

    if(NOT ROCKSDB_CLOUD_LIB)
        message(FATAL_ERROR "Fail to find RocksDB Cloud lib path")
    endif()
    message(STATUS "ROCKSDB_CLOUD_LIB: ${ROCKSDB_CLOUD_LIB}")
    set(ROCKSDB_LIBRARIES ${ROCKSDB_LIBRARIES} ${ROCKSDB_CLOUD_LIB})
endif()

# Compile proto file for data store service
if((WITH_DATA_STORE STREQUAL "ELOQDSS_ROCKSDB_CLOUD_S3") OR (WITH_DATA_STORE STREQUAL "ELOQDSS_ROCKSDB_CLOUD_GCS") OR (WITH_DATA_STORE STREQUAL "ELOQDSS_ROCKSDB") OR (WITH_DATA_STORE STREQUAL "ELOQDSS_ELOQSTORE"))
    set(DS_PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/store_handler/eloq_data_store_service)
    message(NOTICE "data store service proto dir: ${DS_PROTO_DIR}")
    set(PROTO_SRC ${DS_PROTO_DIR})
    set(PROTO_NAME ds_request)
    execute_process(
        COMMAND protoc ./${PROTO_NAME}.proto --cpp_out=./
        WORKING_DIRECTORY ${PROTO_SRC}
    )
endif()

# Add compile definitions based on options
if(WITH_LOG_SERVICE)
    add_compile_definitions(WITH_LOG_SERVICE)
endif()

if(OPEN_LOG_SERVICE)
    add_compile_definitions(OPEN_LOG_SERVICE=1)
endif()

if(EXT_TX_PROC_ENABLED)
    add_compile_definitions(EXT_TX_PROC_ENABLED)
endif()

if(ELOQ_MODULE_ENABLED)
    add_compile_definitions(ELOQ_MODULE_ENABLED)
endif()

if(SMALL_RANGE)
    add_compile_definitions(SMALL_RANGE)
endif()

if(STATISTICS)
    add_compile_definitions(STATISTICS)
endif()


# Include build files for subcomponents
INCLUDE(build_tx_service.cmake)
INCLUDE(build_eloq_metrics.cmake)

if(WITH_LOG_SERVICE)
    if(OPEN_LOG_SERVICE)
        INCLUDE(build_log_service.cmake)
    else()
        INCLUDE(build_eloq_log_service.cmake)
    endif()
endif()

# Find mimalloc
find_package(MIMALLOC REQUIRED)
include_directories(${MIMALLOC_INCLUDE_DIR})

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/tx_service/include
    ${CMAKE_CURRENT_SOURCE_DIR}/tx_service/include/cc
    ${CMAKE_CURRENT_SOURCE_DIR}/tx_service/include/remote
    ${CMAKE_CURRENT_SOURCE_DIR}/tx_service/include/fault
    ${CMAKE_CURRENT_SOURCE_DIR}/tx_service/include/store
    ${CMAKE_CURRENT_SOURCE_DIR}/tx_service/tx-log-protos
    ${CMAKE_CURRENT_SOURCE_DIR}/eloq_metrics/include
    ${CMAKE_CURRENT_SOURCE_DIR}/store_handler
)

if(WITH_LOG_SERVICE)
    if(OPEN_LOG_SERVICE)
        include_directories(${CMAKE_CURRENT_SOURCE_DIR}/log_service/include)
    else()
        include_directories(${CMAKE_CURRENT_SOURCE_DIR}/eloq_log_service/include)
    endif()
endif()

# Collect all source files for the library
SET(DATA_SUBSTRATE_SOURCES
    # Core data substrate files
    core/src/data_substrate.cpp
    core/src/log_init.cpp
    core/src/metrics_init.cpp
    core/src/storage_init.cpp
    core/src/tx_service_init.cpp
)

# Add store handler sources based on data store type
if(WITH_DATA_STORE STREQUAL "ELOQDSS_ROCKSDB" OR WITH_DATA_STORE STREQUAL "ELOQDSS_ROCKSDB_CLOUD_S3")
    SET(DATA_SUBSTRATE_SOURCES ${DATA_SUBSTRATE_SOURCES}
        store_handler/data_store_service_client.cpp
        store_handler/data_store_service_client_closure.cpp
        store_handler/data_store_service_scanner.cpp
        store_handler/store_util.cpp
        store_handler/eloq_data_store_service/ini.c
        store_handler/eloq_data_store_service/INIReader.cpp
        store_handler/eloq_data_store_service/thread_worker_pool.cpp
        store_handler/eloq_data_store_service/data_store_service.cpp
        store_handler/eloq_data_store_service/data_store_fault_inject.cpp
        store_handler/eloq_data_store_service/data_store_service_config.cpp
        store_handler/eloq_data_store_service/ds_request.pb.cc
        store_handler/eloq_data_store_service/rocksdb_config.cpp
        store_handler/eloq_data_store_service/rocksdb_data_store_common.cpp
    )
    
    if(WITH_DATA_STORE STREQUAL "ELOQDSS_ROCKSDB")
        SET(DATA_SUBSTRATE_SOURCES ${DATA_SUBSTRATE_SOURCES}
            store_handler/eloq_data_store_service/rocksdb_data_store.cpp
        )
    elseif(WITH_DATA_STORE STREQUAL "ELOQDSS_ROCKSDB_CLOUD_S3")
        SET(DATA_SUBSTRATE_SOURCES ${DATA_SUBSTRATE_SOURCES}
            store_handler/eloq_data_store_service/rocksdb_cloud_data_store.cpp
            store_handler/eloq_data_store_service/purger_event_listener.cpp
            store_handler/eloq_data_store_service/purger_sliding_window.cpp
        )
    endif()
elseif(WITH_DATA_STORE STREQUAL "ELOQDSS_ELOQSTORE")
    SET(DATA_SUBSTRATE_SOURCES ${DATA_SUBSTRATE_SOURCES}
        store_handler/data_store_service_client.cpp
        store_handler/data_store_service_client_closure.cpp
        store_handler/data_store_service_scanner.cpp
        store_handler/store_util.cpp
        store_handler/eloq_data_store_service/thread_worker_pool.cpp
        store_handler/eloq_data_store_service/data_store_service.cpp
        store_handler/eloq_data_store_service/data_store_fault_inject.cpp
        store_handler/eloq_data_store_service/data_store_service_config.cpp
        store_handler/eloq_data_store_service/eloq_store_data_store.cpp
        store_handler/eloq_data_store_service/ds_request.pb.cc
        store_handler/eloq_data_store_service/eloq_store_config.cpp
    )
endif()

# Build the static library
ADD_LIBRARY(${DATA_SUBSTRATE_LIB} STATIC ${DATA_SUBSTRATE_SOURCES})

# Link with dependencies
SET(DATA_SUBSTRATE_LINK_LIBS
    txservice
    ${METRICS_LIB}
)

if(WITH_LOG_SERVICE)
    SET(DATA_SUBSTRATE_LINK_LIBS ${DATA_SUBSTRATE_LINK_LIBS} logservice)
endif()

if(WITH_DATA_STORE STREQUAL "ELOQDSS_ROCKSDB" OR WITH_DATA_STORE STREQUAL "ELOQDSS_ROCKSDB_CLOUD_S3")
    SET(DATA_SUBSTRATE_LINK_LIBS ${DATA_SUBSTRATE_LINK_LIBS} ${ROCKSDB_LIBRARIES})
elseif(WITH_DATA_STORE STREQUAL "DYNAMODB")
    SET(DATA_SUBSTRATE_LINK_LIBS ${DATA_SUBSTRATE_LINK_LIBS} ${AWSSDK_LINK_LIBRARIES})
elseif(WITH_DATA_STORE STREQUAL "BIGTABLE")
    SET(DATA_SUBSTRATE_LINK_LIBS ${DATA_SUBSTRATE_LINK_LIBS} google-cloud-cpp::bigtable)
elseif(WITH_DATA_STORE STREQUAL "ELOQDSS_ELOQSTORE")
    SET(DATA_SUBSTRATE_LINK_LIBS ${DATA_SUBSTRATE_LINK_LIBS} eloqstore)
endif()

TARGET_LINK_LIBRARIES(${DATA_SUBSTRATE_LIB} ${DATA_SUBSTRATE_LINK_LIBS})

# Export include directories for other targets
TARGET_INCLUDE_DIRECTORIES(${DATA_SUBSTRATE_LIB} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/tx_service/include
    ${CMAKE_CURRENT_SOURCE_DIR}/eloq_metrics/include
    ${CMAKE_CURRENT_SOURCE_DIR}/store_handler
)
