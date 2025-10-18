set(ELOQ_METRICS_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/eloq_metrics")
set(ELOQ_METRICS_SRC_DIR "${ELOQ_METRICS_ROOT_DIR}/src")
set(ELOQ_METRICS_INCLUDE_DIR "${ELOQ_METRICS_ROOT_DIR}/include")

option(ELOQ_METRICS_ENABLE_BENCHMARK "Whether enable google benchmark" OFF)
option(ELOQ_METRICS_WITH_GLOG "Whether use glog" ON)
option(ENABLE_ELOQ_METRICS_APP "Whether enable mono metrics app" OFF)

message(STATUS "ELOQ_METRICS_ENABLE_BENCHMARK ${ELOQ_METRICS_ENABLE_BENCHMARK}")
message(STATUS "ELOQ_METRICS_WITH_GLOG ${ELOQ_METRICS_WITH_GLOG}")
message(STATUS "ENABLE_ELOQ_METRICS_APP ${ENABLE_ELOQ_METRICS_APP}")
message(STATUS "ELOQ_METRICS_ROOT_DIR ${ELOQ_METRICS_ROOT_DIR}")

if(ELOQ_METRICS_WITH_GLOG)
    add_compile_definitions(WITH_GLOG=1)
endif()

if(POLICY CMP0074)
    cmake_policy(SET CMP0074 NEW)
endif()

if(POLICY CMP0054)
    cmake_policy(SET CMP0054 NEW)
endif()

find_package(prometheus-cpp CONFIG REQUIRED)

set(ELOQ_METRICS_TARGET_SOURCE_LIST
    ${ELOQ_METRICS_INCLUDE_DIR}/metrics.h
    ${ELOQ_METRICS_INCLUDE_DIR}/meter.h
    ${ELOQ_METRICS_INCLUDE_DIR}/metrics_collector.h
    ${ELOQ_METRICS_INCLUDE_DIR}/prometheus_collector.h
    ${ELOQ_METRICS_INCLUDE_DIR}/metrics_manager.h
    ${ELOQ_METRICS_INCLUDE_DIR}/metrics_registry_impl.h
    ${ELOQ_METRICS_SRC_DIR}/metrics.cc
    ${ELOQ_METRICS_SRC_DIR}/prometheus_collector.cc
    ${ELOQ_METRICS_SRC_DIR}/metrics_manager.cc
    ${ELOQ_METRICS_SRC_DIR}/metrics_registry_impl.cpp
)
add_library(${METRICS_LIB} STATIC ${ELOQ_METRICS_TARGET_SOURCE_LIST})

target_include_directories(${METRICS_LIB} PUBLIC ${ELOQ_METRICS_INCLUDE_DIR})

target_link_libraries(${METRICS_LIB} PUBLIC prometheus-cpp::pull)
