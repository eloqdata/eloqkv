# Ensure functions/modules are available
set(CASS_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/cass)
set(CASS_SRC_DIR "${CASS_ROOT_DIR}/src")
set(CASS_INCLUDE_DIR "${CASS_ROOT_DIR}/include")

# Ensure functions/modules are available
list(APPEND CMAKE_MODULE_PATH ${CASS_ROOT_DIR}/cmake)

set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

set(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS} -Wno-unused-local-typedefs -Wno-non-virtual-dtor -Wno-error")

#---------------
# Policies
#---------------

if(POLICY CMP0074)
  cmake_policy(SET CMP0074 NEW)
endif()

if (POLICY CMP0054)
  cmake_policy(SET CMP0054 NEW)
endif()

#---------------
# Options
#---------------

option(CASS_BUILD_EXAMPLES "Build examples" OFF)
option(CASS_BUILD_INTEGRATION_TESTS "Build integration tests" OFF)
option(CASS_BUILD_SHARED "Build shared library" OFF)
option(CASS_BUILD_STATIC "Build static library" ON)
option(CASS_BUILD_TESTS "Build tests" OFF)
option(CASS_BUILD_UNIT_TESTS "Build unit tests" OFF)
option(CASS_DEBUG_CUSTOM_ALLOC "Debug custom allocator" OFF)
option(CASS_INSTALL_HEADER "Install header file" OFF)
option(CASS_INSTALL_HEADER_IN_SUBDIR "Install header file under 'include/cassandra'" OFF)
option(CASS_INSTALL_PKG_CONFIG "Install pkg-config file(s)" OFF)
option(CASS_MULTICORE_COMPILATION "Enable multicore compilation" ON)
option(CASS_USE_BOOST_ATOMIC "Use Boost atomics library" OFF)
option(CASS_USE_KERBEROS "Use Kerberos" OFF)
option(CASS_USE_LIBSSH2 "Use libssh2 for integration tests" OFF)
option(CASS_USE_OPENSSL "Use OpenSSL" ON)
option(CASS_USE_STATIC_LIBS "Link static libraries when building executables" OFF)
option(CASS_USE_STD_ATOMIC "Use C++11 atomics library" ON)
option(CASS_USE_ZLIB "Use zlib" ON)
option(CASS_USE_TIMERFD "Use timerfd (Linux only)" ON)

# Determine which driver target should be used as a dependency
set(PROJECT_LIB_NAME_TARGET cassandra)
if(CASS_USE_STATIC_LIBS OR
   (WIN32 AND (CASS_BUILD_INTEGRATION_TESTS OR CASS_BUILD_UNIT_TESTS)))
  set(CASS_USE_STATIC_LIBS ON) # Not all driver internals are exported for test executable (e.g. CASS_EXPORT)
  set(CASS_BUILD_STATIC ON)
  set(PROJECT_LIB_NAME_TARGET cassandra_static)
endif()

# Ensure the driver is configured to build
if(NOT CASS_BUILD_SHARED AND NOT CASS_BUILD_STATIC)
  message(FATAL_ERROR "Driver is not Configured to Build: Ensure shared and/or static library is enabled")
endif()

if(CASS_DEBUG_CUSTOM_ALLOC AND CASS_USE_STATIC_LIBS)
  message(WARNING "Debugging the custom allocator while static linking the library can cause your application to fail")
endif()

#------------------------
# Dependencies
#------------------------

include(Dependencies)
include(ClangFormat)

#------------------------
# Project Version
#------------------------

file(STRINGS "${CASS_INCLUDE_DIR}/cassandra.h" _VERSION_PARTS
  REGEX "^#define[ \t]+CASS_VERSION_(MAJOR|MINOR|PATCH|SUFFIX)[ \t]+([0-9]+|\"([^\"]+)\")$")

foreach(part MAJOR MINOR PATCH SUFFIX)
  string(REGEX MATCH "CASS_VERSION_${part}[ \t]+([0-9]+|\"([^\"]+)\")"
    PROJECT_VERSION_${part} ${_VERSION_PARTS})
  # Extract version numbers
  if (PROJECT_VERSION_${part})
    string(REGEX REPLACE "CASS_VERSION_${part}[ \t]+([0-9]+|\"([^\"]+)\")" "\\1"
      PROJECT_VERSION_${part} ${PROJECT_VERSION_${part}})
  endif()
endforeach()

# Verify version parts
if(NOT PROJECT_VERSION_MAJOR AND NOT PROJECT_VERSION_MINOR)
  message(FATAL_ERROR "Unable to retrieve driver version from ${version_header_file}")
endif()

set(PROJECT_VERSION_STRING
  ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})
if(NOT PROJECT_VERSION_PATCH STREQUAL "")
  set(PROJECT_VERSION_STRING
    "${PROJECT_VERSION_STRING}.${PROJECT_VERSION_PATCH}")
endif()
if(NOT PROJECT_VERSION_SUFFIX STREQUAL "")
  string(REPLACE "\"" ""
    PROJECT_VERSION_SUFFIX ${PROJECT_VERSION_SUFFIX})
  set(PROJECT_VERSION_STRING
    "${PROJECT_VERSION_STRING}-${PROJECT_VERSION_SUFFIX}")
endif()

message(STATUS "Driver version: ${PROJECT_VERSION_STRING}")

#------------------------
# Determine atomic implementation
#------------------------

# Determine if std::atomic can be used for GCC, Clang, or MSVC
if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
  if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_COMPILER_IS_GNUCXX)
    # Version determined from: https://gcc.gnu.org/wiki/Atomic/GCCMM
    if(CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL "4.7" OR
        CMAKE_CXX_COMPILER_VERSION VERSION_GREATER "4.7")
      set(CASS_USE_STD_ATOMIC ON)
    endif()
  endif()
elseif("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
  # Version determined from: http://clang.llvm.org/cxx_status.html
  # 3.2 includes the full C++11 memory model, but 3.1 had atomic
  # support.
  if(CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL "3.1" OR
      CMAKE_CXX_COMPILER_VERSION VERSION_GREATER "3.1")
    set(CASS_USE_STD_ATOMIC ON)
  endif()
elseif("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
  # Version determined from https://msdn.microsoft.com/en-us/library/hh874894
  # VS2012+/VS 11.0+/WindowsSDK v8.0+
  if(MSVC_VERSION GREATER 1700 OR
      MSVC_VERSION EQUAL 1700)
    set(CASS_USE_STD_ATOMIC ON)
  endif()
endif()

if(CASS_USE_BOOST_ATOMIC)
  message(STATUS "Using boost::atomic implementation for atomic operations")
elseif(CASS_USE_STD_ATOMIC)
  message(STATUS "Using std::atomic implementation for atomic operations")
endif()

#------------------------
# Subdirectories
#------------------------

add_subdirectory(${CASS_SRC_DIR})
