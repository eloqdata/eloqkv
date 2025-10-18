SET (LOG_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/log_service)
SET(TX_LOG_PROTOS_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tx_service/tx-log-protos)

set(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS} -Wno-error")

option(BRPC_WITH_GLOG "With glog" ON)

find_path(BRPC_INCLUDE_PATH NAMES brpc/stream.h)
find_library(BRPC_LIB NAMES brpc)
if ((NOT BRPC_INCLUDE_PATH) OR (NOT BRPC_LIB))
    message(FATAL_ERROR "Fail to find brpc")
endif()
include_directories(${BRPC_INCLUDE_PATH})

find_path(BRAFT_INCLUDE_PATH NAMES braft/raft.h)
find_library(BRAFT_LIB NAMES braft)
if ((NOT BRAFT_INCLUDE_PATH) OR (NOT BRAFT_LIB))
    message (FATAL_ERROR "Fail to find braft")
endif()
include_directories(${BRAFT_INCLUDE_PATH})

find_path(GFLAGS_INCLUDE_PATH gflags/gflags.h)
find_library(GFLAGS_LIBRARY NAMES gflags libgflags)
if((NOT GFLAGS_INCLUDE_PATH) OR (NOT GFLAGS_LIBRARY))
    message(FATAL_ERROR "Fail to find gflags")
endif()
include_directories(${GFLAGS_INCLUDE_PATH})

if(BRPC_WITH_GLOG)
    message(NOTICE "log service brpc with glog")
    find_path(GLOG_INCLUDE_PATH NAMES glog/logging.h)
    find_library(GLOG_LIB NAMES glog VERSION ">=0.6.0" REQUIRED)
    if((NOT GLOG_INCLUDE_PATH) OR (NOT GLOG_LIB))
        message(FATAL_ERROR "Fail to find glog")
    endif()
    include_directories(${GLOG_INCLUDE_PATH})
    set(LOG_LIB ${LOG_LIB} ${GLOG_LIB})
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

#find_path(GPERFTOOLS_INCLUDE_DIR NAMES gperftools/heap-profiler.h)
#find_library(GPERFTOOLS_LIBRARIES NAMES tcmalloc_and_profiler)
#if (GPERFTOOLS_INCLUDE_DIR AND GPERFTOOLS_LIBRARIES)
#    set(CMAKE_CXX_FLAGS "-DBRPC_ENABLE_CPU_PROFILER")
#    include_directories(${GPERFTOOLS_INCLUDE_DIR})
#else ()
#    set (GPERFTOOLS_LIBRARIES "")
#endif ()

#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CMAKE_CPP_FLAGS} -DGFLAGS_NS=${GFLAGS_NS} -DNDEBUG -O2 -D__const__= -pipe -W -Wall -Wno-unused-parameter -fPIC -fno-omit-frame-pointer")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # require at least gcc 4.8
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 4.8)
        message(FATAL_ERROR "GCC is too old, please install a newer version supporting C++11")
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # require at least clang 3.3
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 3.3)
        message(FATAL_ERROR "Clang is too old, please install a newer version supporting C++11")
    endif()
else()
    message(WARNING "You are using an unsupported compiler! Compilation has only been tested with Clang and GCC.")
endif()


#if(CMAKE_VERSION VERSION_LESS "3.1.3")
#    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
#        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17")
#    endif()
#    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
#        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17")
#    endif()
#else()
#    set(CMAKE_CXX_STANDARD 17)
#    set(CMAKE_CXX_STANDARD_REQUIRED ON)
#endif()

find_path(LEVELDB_INCLUDE_PATH NAMES leveldb/db.h)
find_library(LEVELDB_LIB NAMES leveldb)
if ((NOT LEVELDB_INCLUDE_PATH) OR (NOT LEVELDB_LIB))
    message(FATAL_ERROR "Fail to find leveldb")
endif()
include_directories(${LEVELDB_INCLUDE_PATH})

find_path(ROCKSDB_INCLUDE_PATH NAMES rocksdb/db.h)

if (NOT ROCKSDB_INCLUDE_PATH)
message(FATAL_ERROR "Fail to find RocksDB include path")
endif ()
message(STATUS "ROCKSDB_INCLUDE_PATH: ${ROCKSDB_INCLUDE_PATH}")

find_library(ROCKSDB_LIB NAMES rocksdb)
if (NOT ROCKSDB_LIB)
  message(FATAL_ERROR "Fail to find RocksDB lib path")
endif ()
message(STATUS "ROCKSDB_LIB: ${ROCKSDB_LIB}")

include_directories(${ROCKSDB_INCLUDE_PATH})

set(LOG_LIB
        ${LOG_LIB}
        ${ROCKSDB_LIB}
        )

# one shipping thread is enough for rocksdb version log state
set(LOG_SHIPPING_THREADS_NUM 1)

add_compile_definitions(LOG_SHIPPING_THREADS_NUM=${LOG_SHIPPING_THREADS_NUM})

# Add for compatiable reason
add_compile_definitions(LOG_STATE_TYPE_RKDB)

set(LOG_INCLUDE_DIR
   ${LOG_SOURCE_DIR}/include
   ${TX_LOG_PROTOS_SOURCE_DIR}
   )

set(LOG_LIB
    ${LOG_LIB}
    ${CMAKE_THREAD_LIBS_INIT}
    ${GFLAGS_LIBRARY}
    ${PROTOBUF_LIBRARY}
    ${GPERFTOOLS_LIBRARIES}
    ${LEVELDB_LIB}
    ${BRAFT_LIB}
    ${BRPC_LIB}
    dl
    z
    )

find_package(Protobuf REQUIRED)


ADD_LIBRARY(logservice STATIC
    ${LOG_SOURCE_DIR}/src/log_server.cpp
    ${LOG_SOURCE_DIR}/src/log_state_rocksdb_impl.cpp
    ${LOG_SOURCE_DIR}/src/fault_inject.cpp
    ${LOG_SOURCE_DIR}/src/INIReader.cpp
    ${LOG_SOURCE_DIR}/src/ini.c
    ${TX_LOG_PROTOS_SOURCE_DIR}/log.pb.cc
    ${LOG_SOURCE_DIR}/src/open_log_service.cpp
    ${LOG_SOURCE_DIR}/src/open_log_task.cpp
    )

target_include_directories(logservice PUBLIC ${LOG_INCLUDE_DIR})
target_link_libraries(logservice PUBLIC ${LOG_LIB} ${PROTOBUF_LIBRARIES})
