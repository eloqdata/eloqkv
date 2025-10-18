SET (LOG_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/eloq_log_service)
SET(TX_LOG_PROTOS_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tx_service/tx-log-protos)

set(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS} -Wno-error")

option(BRPC_WITH_GLOG "With glog" ON)

set(WITH_LOG_STATE "ROCKSDB" CACHE STRING "The log state implementation")
set_property(CACHE WITH_LOG_STATE PROPERTY STRINGS "MEMORY" "ROCKSDB" "ROCKSDB_CLOUD_S3" "ROCKSDB_CLOUD_GCS")
message(NOTICE "WITH_LOG_STATE: ${WITH_LOG_STATE}")

# Add compile flags for LOG STATE TYPE
if(WITH_LOG_STATE STREQUAL "MEMORY")
  add_compile_definitions(LOG_STATE_TYPE_MEM)
elseif(WITH_LOG_STATE STREQUAL "ROCKSDB")
  add_compile_definitions(LOG_STATE_TYPE_RKDB)
  add_compile_definitions(LOG_STATE_TYPE_RKDB_ALL)
elseif(WITH_LOG_STATE STREQUAL "ROCKSDB_CLOUD_S3")
  add_compile_definitions(LOG_STATE_TYPE_RKDB_S3)
elseif(WITH_LOG_STATE STREQUAL "ROCKSDB_CLOUD_GCS")
  add_compile_definitions(LOG_STATE_TYPE_RKDB_GCS)
else()
  message(FATAL_ERROR "Unknown WITH_LOG_STATE: ${WITH_LOG_STATE}")
endif()


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

set(LOG_SHIPPING_THREADS_NUM 1)

# Add RocksDB include and libs for LOG_STATE
if (WITH_LOG_STATE MATCHES "ROCKSDB|ROCKSDB_CLOUD_S3|ROCKSDB_CLOUD_GCS")
  if (WITH_LOG_STATE MATCHES "ROCKSDB_CLOUD_S3|ROCKSDB_CLOUD_GCS")
    if (WITH_LOG_STATE STREQUAL "ROCKSDB_CLOUD_S3")
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
  
        set(ROCKSDB_LIB ${ROCKSDB_LIB} ${AWS_CORE_LIB})
        set(ROCKSDB_LIB ${ROCKSDB_LIB} ${AWS_KINESIS_LIB})
        set(ROCKSDB_LIB ${ROCKSDB_LIB} ${AWS_S3_LIB})
  
        find_library(ROCKSDB_CLOUD_LIB NAMES rocksdb-cloud-aws)

        add_compile_definitions(USE_AWS)
      elseif (WITH_LOG_STATE STREQUAL "ROCKSDB_CLOUD_GCS")
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

        set(ROCKSDB_LIB ${ROCKSDB_LIB} ${GCP_COMMON_LIB})
        set(ROCKSDB_LIB ${ROCKSDB_LIB} ${GCP_CS_LIB})
  
        find_library(ROCKSDB_CLOUD_LIB NAMES rocksdb-cloud-gcp)

        add_compile_definitions(USE_GCP)
      endif ()

      find_path(ROCKSDB_CLOUD_INCLUDE_PATH NAMES rocksdb/db.h PATH_SUFFIXES "rocksdb_cloud_header")
      if (NOT ROCKSDB_CLOUD_INCLUDE_PATH)
    	  message(FATAL_ERROR "Fail to find RocksDB Cloud include path")
      endif ()
      message(STATUS "ROCKSDB_CLOUD_INCLUDE_PATH: ${ROCKSDB_CLOUD_INCLUDE_PATH}")
      set(ROCKSDB_INCLUDE_PATH ${ROCKSDB_INCLUDE_PATH} ${ROCKSDB_CLOUD_INCLUDE_PATH})

      if (NOT ROCKSDB_CLOUD_LIB)
    	  message(FATAL_ERROR "Fail to find RocksDB Cloud lib path")
      endif ()
      message(STATUS "ROCKSDB_CLOUD_LIB: ${ROCKSDB_CLOUD_LIB}")
      set(ROCKSDB_LIB ${ROCKSDB_LIB} ${ROCKSDB_CLOUD_LIB})
    else ()
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
    endif ()

    include_directories(${ROCKSDB_INCLUDE_PATH})

    set(LOG_LIB
            ${LOG_LIB}
            ${ROCKSDB_LIB}
            )

    # one shipping thread is enough for rocksdb version log state
    set(LOG_SHIPPING_THREADS_NUM 1)
endif ()

add_compile_definitions(LOG_SHIPPING_THREADS_NUM=${LOG_SHIPPING_THREADS_NUM})

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
    ${LOG_SOURCE_DIR}/src/log_instance.cpp
    ${LOG_SOURCE_DIR}/src/log_server.cpp
    ${LOG_SOURCE_DIR}/src/log_state_rocksdb_impl.cpp
    ${LOG_SOURCE_DIR}/src/log_state_rocksdb_cloud_impl.cpp
    ${LOG_SOURCE_DIR}/src/log_state_memory_impl.cpp
    ${LOG_SOURCE_DIR}/src/fault_inject.cpp
    ${LOG_SOURCE_DIR}/src/INIReader.cpp
    ${LOG_SOURCE_DIR}/src/ini.c
    ${TX_LOG_PROTOS_SOURCE_DIR}/log.pb.cc
    )

target_include_directories(logservice PUBLIC ${LOG_INCLUDE_DIR})
target_link_libraries(logservice PUBLIC ${LOG_LIB} ${PROTOBUF_LIBRARIES})
