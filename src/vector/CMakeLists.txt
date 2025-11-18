cmake_minimum_required(VERSION 3.8)
project(Vector CXX)

find_package(Threads)

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wno-parentheses -Wno-unused-parameter -fPIC")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g")

option(RUN_VECTOR_CTEST "Whether to run vector tests" ON)
message(NOTICE "RUN_VECTOR_CTEST : ${RUN_VECTOR_CTEST}")

# CURL
find_package(CURL REQUIRED)
include_directories(${CURL_INCLUDE_DIRS})
set(DYNAMIC_LIB ${DYNAMIC_LIB} ${CURL_LIBRARIES})

SET(INI_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/../ini.c ${CMAKE_CURRENT_SOURCE_DIR}/../INIReader.cpp)

set(VECTOR_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/hnsw_vector_index.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vector_handler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/log_object.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/predicate.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vector_type.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/cloud_manager.cpp
)

add_library(eloq_vector STATIC ${VECTOR_SOURCES} ${INI_SOURCES})

target_include_directories(eloq_vector PUBLIC ${VECTOR_INCLUDE_DIR})
target_link_libraries(eloq_vector PUBLIC txservice ${ABSEIL} ${DYNAMIC_LIB})

if(RUN_VECTOR_CTEST)
    enable_testing()
    add_subdirectory(tests)
endif()
