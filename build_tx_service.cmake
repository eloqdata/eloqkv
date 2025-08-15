SET(TX_SERVICE_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tx_service)
SET(METRICS_SERVICE_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/eloq_metrics)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-parentheses -Wno-error")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DFAULT_INJECTOR")

option(BRPC_WITH_GLOG "With glog" ON)

option (FORK_HM_PROCESS "Whether fork host manager process" OFF)
message(NOTICE "FORK_HM_PROCESS : ${FORK_HM_PROCESS}")
if (FORK_HM_PROCESS)
    add_compile_definitions(FORK_HM_PROCESS)
endif()

option(SKIP_WRITE_LOG "Skip writing log" ON)

add_compile_definitions(ON_KEY_OBJECT)

if (SKIP_WRITE_LOG)
    add_compile_definitions(SKIP_WRITE_LOG)
endif ()

find_package(Protobuf REQUIRED)
find_package(GFLAGS REQUIRED)
find_package (MIMALLOC REQUIRED)
find_path(BRPC_INCLUDE_PATH NAMES brpc/stream.h)
find_library(BRPC_LIB NAMES brpc)

if((NOT BRPC_INCLUDE_PATH) OR(NOT BRPC_LIB))
    message(FATAL_ERROR "Fail to find brpc")
endif()

find_path(BRAFT_INCLUDE_PATH NAMES braft/raft.h)
find_library(BRAFT_LIB NAMES braft)

if((NOT BRAFT_INCLUDE_PATH) OR(NOT BRAFT_LIB))
    message(FATAL_ERROR "Fail to find braft")
endif()

if(BRPC_WITH_GLOG)
    find_path(GLOG_INCLUDE_PATH NAMES glog/logging.h)
    find_library(GLOG_LIB NAMES glog)

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

set(PROTO_SRC ${CMAKE_CURRENT_SOURCE_DIR}/tx_service/include/proto)
set(PROTO_NAME cc_request)
execute_process(
    COMMAND protoc ./${PROTO_NAME}.proto --cpp_out=./ --proto_path=./
    WORKING_DIRECTORY ${PROTO_SRC}
)

set(LOG_PROTO_SRC ${CMAKE_CURRENT_SOURCE_DIR}/tx_service/tx-log-protos)
set(LOG_PROTO_NAME log)
execute_process(
    COMMAND protoc ./${LOG_PROTO_NAME}.proto --cpp_out=./ --proto_path=./
    WORKING_DIRECTORY ${LOG_PROTO_SRC}
)

set(INCLUDE_DIR
    ${TX_SERVICE_SOURCE_DIR}/include
    ${TX_SERVICE_SOURCE_DIR}/include/cc
    ${TX_SERVICE_SOURCE_DIR}/include/remote
    ${TX_SERVICE_SOURCE_DIR}/include/fault
    ${TX_SERVICE_SOURCE_DIR}/tx-log-protos
    ${METRICS_SERVICE_SOURCE_DIR}/include
    ${Protobuf_INCLUDE_DIR})

set(INCLUDE_DIR ${INCLUDE_DIR}
    ${BRPC_INCLUDE_PATH}
    ${BRAFT_INCLUDE_PATH}
    ${GLOG_INCLUDE_PATH}
    ${GFLAGS_INCLUDE_PATH})

set(LINK_LIB ${LINK_LIB} ${PROTOBUF_LIBRARY})

set(LINK_LIB ${LINK_LIB}
    ${GFLAGS_LIBRARY}
    ${LEVELDB_LIB}
    ${BRAFT_LIB}
    ${BRPC_LIB}
)

SET(ELOQ_RESOURCES
    ${TX_SERVICE_SOURCE_DIR}/src/tx_key.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/tx_execution.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/tx_operation.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/checkpointer.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/tx_trace.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/tx_start_ts_collector.cpp
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
    ${TX_SERVICE_SOURCE_DIR}/src/remote/remote_cc_handler.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/remote/remote_cc_request.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/remote/cc_node_service.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/remote/cc_stream_receiver.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/remote/cc_stream_sender.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/fault/log_replay_service.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/fault/cc_node.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/fault/fault_inject.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/tx_worker_pool.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/dead_lock_check.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/tx_index_operation.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/sk_generator.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/data_sync_task.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/store/snapshot_manager.cpp
    ${TX_SERVICE_SOURCE_DIR}/src/sequences/sequences.cpp
    ${METRICS_SERVICE_SOURCE_DIR}/src/metrics.cc
)

set(INCLUDE_DIR ${INCLUDE_DIR} ${PROTO_SRC} ${LOG_PROTO_SRC})
set(ELOQ_RESOURCES ${ELOQ_RESOURCES} ${PROTO_SRC}/${PROTO_NAME}.pb.cc)
set(ELOQ_RESOURCES ${ELOQ_RESOURCES} ${LOG_PROTO_SRC}/log_agent.cpp)
set(ELOQ_RESOURCES ${ELOQ_RESOURCES} ${LOG_PROTO_SRC}/${LOG_PROTO_NAME}.pb.cc)


# ADD_CONVENIENCE_LIBRARY(txservice STATIC
# ${ELOQ_RESOURCES})
ADD_LIBRARY(txservice STATIC
    ${ELOQ_RESOURCES})

target_include_directories(txservice PUBLIC ${INCLUDE_DIR})

target_link_libraries(txservice PUBLIC mimalloc ${LINK_LIB} ${PROTOBUF_LIBRARIES})


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
