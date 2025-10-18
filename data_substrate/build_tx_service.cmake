SET(TX_SERVICE_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tx_service)
SET(METRICS_SERVICE_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/eloq_metrics)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-parentheses -Wno-error")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DFAULT_INJECTOR")

option(BRPC_WITH_GLOG "With glog" ON)

option(EXT_TX_PROC_ENABLED "Allows external threads to move forward the tx service." ON)

if(EXT_TX_PROC_ENABLED)
    add_definitions(-DEXT_TX_PROC_ENABLED)
endif()

option(ELOQ_MODULE_ENABLED "Register the tx service as an ELOQ module." OFF)
message(NOTICE "Tx service ELOQ_MODULE_ENABLED : ${ELOQ_MODULE_ENABLED}")

if(ELOQ_MODULE_ENABLED)
    add_definitions(-DELOQ_MODULE_ENABLED)
endif()

option(FORK_HM_PROCESS "Whether fork host manager process" OFF)
message(NOTICE "FORK_HM_PROCESS : ${FORK_HM_PROCESS}")

option(STATISTICS "Whether enable table statistics" OFF)
message(NOTICE "STATISTICS : ${STATISTICS}")

find_package(Protobuf REQUIRED)
find_package(GFLAGS REQUIRED)
find_package(MIMALLOC REQUIRED)

find_path(GFLAGS_INCLUDE_PATH gflags/gflags.h)
find_library(GFLAGS_LIBRARY NAMES gflags libgflags)

if((NOT GFLAGS_INCLUDE_PATH) OR(NOT GFLAGS_LIBRARY))
    message(FATAL_ERROR "Fail to find gflags")
endif()

execute_process(
    COMMAND bash -c "grep \"namespace [_A-Za-z0-9]\\+ {\" ${GFLAGS_INCLUDE_PATH}/gflags/gflags_declare.h | head -1 | awk '{print $2}' | tr -d '\n'"
    OUTPUT_VARIABLE GFLAGS_NS
)

if(${GFLAGS_NS} STREQUAL "GFLAGS_NAMESPACE")
    execute_process(
        COMMAND bash -c "grep \"#define GFLAGS_NAMESPACE [_A-Za-z0-9]\\+\" ${GFLAGS_INCLUDE_PATH}/gflags/gflags_declare.h | head -1 | awk '{print $3}' | tr -d '\n'"
        OUTPUT_VARIABLE GFLAGS_NS
    )
else()
    add_compile_definitions(OVERRIDE_GFLAGS_NAMESPACE)
endif()

if(SMALL_RANGE)
    add_compile_definitions(SMALL_RANGE)
endif()

if(FORK_HM_PROCESS)
    add_compile_definitions(FORK_HM_PROCESS)
endif()

if(STATISTICS)
    add_definitions(-DSTATISTICS)
endif()

if(CMAKE_COMPILER_IS_GNUCC)
    find_path(BRPC_INCLUDE_PATH NAMES brpc/stream.h)
    find_library(BRPC_LIB NAMES brpc)

    if((NOT BRPC_INCLUDE_PATH) OR(NOT BRPC_LIB))
        message(FATAL_ERROR "Fail to find brpc")
    endif()

    if(BRPC_WITH_GLOG)
        message(NOTICE "TX BRPC WITH GLOG")
        find_path(GLOG_INCLUDE_PATH NAMES glog/logging.h)
        find_library(GLOG_LIB NAMES glog VERSION ">=0.6.0" REQUIRED)

        if((NOT GLOG_INCLUDE_PATH) OR(NOT GLOG_LIB))
            message(FATAL_ERROR "Fail to find glog")
        endif()

        include_directories(${GLOG_INCLUDE_PATH})
        set(LINK_LIB ${LINK_LIB} ${GLOG_LIB})
    endif()

    find_path(LEVELDB_INCLUDE_PATH NAMES leveldb/db.h)
    find_library(LEVELDB_LIB NAMES leveldb)

    if((NOT LEVELDB_INCLUDE_PATH) OR(NOT LEVELDB_LIB))
        message(FATAL_ERROR "Fail to find leveldb")
    endif()
endif()

# Compile all protobuf files under txservice proto directory.
SET(PROTO_SRC ${TX_SERVICE_SOURCE_DIR}/include/proto)
FILE(GLOB PROTO_FILES RELATIVE ${PROTO_SRC} ${PROTO_SRC}/*.proto)

FOREACH(PROTO_FILE ${PROTO_FILES})
    STRING(REGEX REPLACE "[^/]proto" "" PROTO_NAME ${PROTO_FILE})
    LIST(APPEND PROTO_CC_FILES ${PROTO_SRC}/${PROTO_NAME}.pb.cc)
    ADD_CUSTOM_COMMAND(
        OUTPUT "${PROTO_SRC}/${PROTO_NAME}.pb.cc" "${PROTO_SRC}/${PROTO_NAME}.pb.h"
        DEPENDS ${PROTO_SRC}/${PROTO_FILE}
        COMMAND protoc ${PROTO_FILE} --proto_path=./ --cpp_out=./
        WORKING_DIRECTORY ${PROTO_SRC}
    )
ENDFOREACH(PROTO_FILE)

set(LOG_PROTO_SRC ${CMAKE_CURRENT_SOURCE_DIR}/tx_service/tx-log-protos)
set(LOG_PROTO_NAME log)
execute_process(
    COMMAND protoc ./${LOG_PROTO_NAME}.proto --cpp_out=./ --proto_path=./
    WORKING_DIRECTORY ${LOG_PROTO_SRC}
)

add_subdirectory(tx_service/abseil-cpp)

message(${TX_SERVICE_SOURCE_DIR})
set(INCLUDE_DIR
    ${TX_SERVICE_SOURCE_DIR}/include
    ${TX_SERVICE_SOURCE_DIR}/include/cc
    ${TX_SERVICE_SOURCE_DIR}/include/remote
    ${TX_SERVICE_SOURCE_DIR}/include/fault
    ${TX_SERVICE_SOURCE_DIR}/tx-log-protos
    ${METRICS_SERVICE_SOURCE_DIR}/include
    ${Protobuf_INCLUDE_DIR}
    ${MIMALLOC_INCLUDE_DIR})

if(CMAKE_COMPILER_IS_GNUCC)
    set(INCLUDE_DIR ${INCLUDE_DIR}
        ${BRPC_INCLUDE_PATH}
        ${GLOG_INCLUDE_PATH}
        ${GFLAGS_INCLUDE_PATH})
endif()

set(LINK_LIB ${LINK_LIB} ${PROTOBUF_LIBRARY})

set(LINK_LIB ${LINK_LIB}
    mimalloc
    absl::btree
    absl::flat_hash_map
    ${GFLAGS_LIBRARY}
    ${LEVELDB_LIB}
)

if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    set(LINK_LIB ${LINK_LIB}
        ${BRPC_LIB}
    )
endif()

SET(ELOQ_SOURCES
    ${TX_SERVICE_SOURCE_DIR}/src/tx_key.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/tx_execution.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/tx_operation.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/checkpointer.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/tx_trace.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/tx_start_ts_collector.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/tx_worker_pool.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/sharder.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/standby.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/catalog_key_record.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/range_record.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/range_bucket_key_record.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/cc/cc_entry.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/cc/cc_map.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/cc/cc_shard.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/cc/cc_handler_result.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/cc/local_cc_handler.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/cc/local_cc_shards.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/cc/non_blocking_lock.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/cc/cc_req_misc.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/cc/range_slice.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/cc/reader_writer_cntl.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/eloq_string_key_record.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/eloq_basic_catalog_factory.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/remote/remote_cc_handler.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/remote/remote_cc_request.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/remote/cc_node_service.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/remote/cc_stream_receiver.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/remote/cc_stream_sender.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/fault/log_replay_service.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/fault/cc_node.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/fault/fault_inject.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/dead_lock_check.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/tx_index_operation.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/sk_generator.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/data_sync_task.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/store/snapshot_manager.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/sequences/sequences.cpp
    ${TX_SERVICE_SOURCE_DIR}/tx-log-protos/log_agent.cpp
    ${TX_SERVICE_SOURCE_DIR}/tx-log-protos/log.pb.cc
    ${METRICS_SERVICE_SOURCE_DIR}/src/metrics.cc
)

set(INCLUDE_DIR ${INCLUDE_DIR} ${PROTO_SRC})
set(ELOQ_SOURCES ${ELOQ_SOURCES} ${PROTO_CC_FILES})

ADD_LIBRARY(txservice STATIC
    ${ELOQ_SOURCES})

target_include_directories(txservice PUBLIC ${INCLUDE_DIR})

target_link_libraries(txservice PUBLIC ${LINK_LIB} ${PROTOBUF_LIBRARIES})

# if(FORK_HM_PROCESS)
#     include(FetchContent)
#     # Import yaml-cpp library used by host manager
#     FetchContent_Declare(
#         yaml-cpp
#         GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
#         GIT_TAG yaml-cpp-0.7.0 # Can be a tag (yaml-cpp-x.x.x), a commit hash, or a branch name (master)
#     )
#     FetchContent_MakeAvailable(yaml-cpp)

#     SET(HOST_MANAGER_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tx_service/raft_host_manager)
#     set(HOST_MANAGER_INCLUDE_DIR
#         ${HOST_MANAGER_SOURCE_DIR}/include
#         ${TX_SERVICE_SOURCE_DIR}/tx-log-protos
#         ${OPENSSL_INCLUDE_DIR}
#         ${LOG_PROTO_SRC}
#         ${PROTO_SRC})

#     if(CMAKE_COMPILER_IS_GNUCC)
#         set(HOST_MANAGER_INCLUDE_DIR ${HOST_MANAGER_INCLUDE_DIR}
#             ${BRPC_INCLUDE_PATH}
#             ${BRAFT_INCLUDE_PATH}
#             ${GLOG_INCLUDE_PATH}
#             ${GFLAGS_INCLUDE_PATH})
#     endif()

#     set(HOST_MANAGER_LINK_LIB ${HOST_MANAGER_LINK_LIB} ${PROTOBUF_LIBRARIES})

#     if(CMAKE_COMPILER_IS_GNUCC)
#         find_path(BRAFT_INCLUDE_PATH NAMES braft/raft.h)
#         find_library(BRAFT_LIB NAMES braft)

#         if((NOT BRAFT_INCLUDE_PATH) OR(NOT BRAFT_LIB))
#             message(FATAL_ERROR "Fail to find braft")
#         endif()

#         set(HOST_MANAGER_LINK_LIB ${HOST_MANAGER_LINK_LIB}
#             ${GFLAGS_LIBRARY}
#             ${LEVELDB_LIB}
#             ${BRAFT_LIB}
#             ${BRPC_LIB}
#             ${OPENSSL_LIB})
#         find_path(GLOG_INCLUDE_PATH NAMES glog/logging.h)
#         find_library(GLOG_LIB NAMES glog VERSION ">=0.6.0" REQUIRED)

#         if((NOT GLOG_INCLUDE_PATH) OR(NOT GLOG_LIB))
#             message(FATAL_ERROR "Fail to find glog")
#         endif()

#         include_directories(${GLOG_INCLUDE_PATH})
#         set(HOST_MANAGER_LINK_LIB ${HOST_MANAGER_LINK_LIB} ${GLOG_LIB})
#     endif()

#     SET(RaftHM_SOURCES
#         ${HOST_MANAGER_SOURCE_DIR}/src/main.cpp
#         ${HOST_MANAGER_SOURCE_DIR}/src/raft_host_manager_service.cpp
#         ${HOST_MANAGER_SOURCE_DIR}/src/raft_host_manager.cpp
#         ${HOST_MANAGER_SOURCE_DIR}/src/ini.c
#         ${HOST_MANAGER_SOURCE_DIR}/src/INIReader.cpp
#         ${LOG_PROTO_SRC}/log_agent.cpp
#         ${LOG_PROTO_SRC}/${LOG_PROTO_NAME}.pb.cc
#         ${PROTO_CC_FILES}
#     )

#     include_directories(${HOST_MANAGER_INCLUDE_DIR})
#     MYSQL_ADD_EXECUTABLE(host_manager ${RaftHM_SOURCES} DESTINATION ${INSTALL_SBINDIR} COMPONENT Server)
#     target_link_libraries(host_manager ${HOST_MANAGER_LINK_LIB} yaml-cpp::yaml-cpp)

# endif()

if (FORK_HM_PROCESS)
    SET (HOST_MANAGER_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tx_service/raft_host_manager)
    set(HOST_MANAGER_INCLUDE_DIR
        ${HOST_MANAGER_SOURCE_DIR}/include
        ${TX_SERVICE_SOURCE_DIR}/tx-log-protos
        ${OPENSSL_INCLUDE_DIR}
        ${PROTO_SRC}
        ${LOG_PROTO_SRC})

    set(HOST_MANAGER_INCLUDE_DIR ${HOST_MANAGER_INCLUDE_DIR}
        ${BRPC_INCLUDE_PATH}
        ${BRAFT_INCLUDE_PATH}
        ${GLOG_INCLUDE_PATH}
        ${GFLAGS_INCLUDE_PATH})

    set(HOST_MANAGER_LINK_LIB ${HOST_MANAGER_LINK_LIB} ${PROTOBUF_LIBRARIES})

    find_path(BRAFT_INCLUDE_PATH NAMES braft/raft.h)
    find_library(BRAFT_LIB NAMES braft)
    if ((NOT BRAFT_INCLUDE_PATH) OR (NOT BRAFT_LIB))
        message (FATAL_ERROR "Fail to find braft")
    endif()
    set(HOST_MANAGER_LINK_LIB ${HOST_MANAGER_LINK_LIB}
        ${GFLAGS_LIBRARY}
        ${GPERFTOOLS_LIBRARIES}
        ${LEVELDB_LIB}
        ${BRAFT_LIB}
        ${BRPC_LIB}
        ${OPENSSL_LIB})
    find_path(GLOG_INCLUDE_PATH NAMES glog/logging.h)
    find_library(GLOG_LIB NAMES glog VERSION ">=0.6.0" REQUIRED)
    if((NOT GLOG_INCLUDE_PATH) OR (NOT GLOG_LIB))
        message(FATAL_ERROR "Fail to find glog")
    endif()
    include_directories(${GLOG_INCLUDE_PATH})
    set(HOST_MANAGER_LINK_LIB ${HOST_MANAGER_LINK_LIB} ${GLOG_LIB})

    SET(RaftHM_SOURCES
        ${HOST_MANAGER_SOURCE_DIR}/src/main.cpp
        ${HOST_MANAGER_SOURCE_DIR}/src/raft_host_manager_service.cpp
        ${HOST_MANAGER_SOURCE_DIR}/src/raft_host_manager.cpp
        ${HOST_MANAGER_SOURCE_DIR}/src/ini.c
        ${HOST_MANAGER_SOURCE_DIR}/src/INIReader.cpp
        ${PROTO_SRC}/${PROTO_NAME}.pb.cc
        ${LOG_PROTO_SRC}/log_agent.cpp
        ${LOG_PROTO_SRC}/${LOG_PROTO_NAME}.pb.cc
        )

    include(FetchContent)

    # Import yaml-cpp library used by host manager
    FetchContent_Declare(
            yaml-cpp
            GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
            GIT_TAG yaml-cpp-0.7.0 # Can be a tag (yaml-cpp-x.x.x), a commit hash, or a branch name (master)
    )
    FetchContent_MakeAvailable(yaml-cpp)
    set(HOST_MANAGER_LINK_LIB ${HOST_MANAGER_LINK_LIB} yaml-cpp::yaml-cpp)

    include_directories(${HOST_MANAGER_INCLUDE_DIR})
    add_executable(host_manager ${RaftHM_SOURCES})
    target_link_libraries(host_manager PRIVATE ${HOST_MANAGER_LINK_LIB} )

    set_target_properties(host_manager PROPERTIES
            BUILD_RPATH "$ORIGIN/../lib"
            INSTALL_RPATH "$ORIGIN/../lib"
            INSTALL_RPATH_USE_LINK_PATH TRUE)

    install(TARGETS host_manager RUNTIME DESTINATION bin)
endif()

