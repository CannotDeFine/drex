set(RPC_THIRD_PARTY_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/install" CACHE PATH "Private prefix where bundled third-party dependencies are installed")
if(EXISTS "${RPC_THIRD_PARTY_PREFIX}")
    list(PREPEND CMAKE_PREFIX_PATH "${RPC_THIRD_PARTY_PREFIX}")
endif()
list(APPEND CMAKE_PREFIX_PATH "/usr/local")

set(_THIRD_PARTY_INCLUDE_HINTS
    "${RPC_THIRD_PARTY_PREFIX}/include"
)
set(_THIRD_PARTY_LIBRARY_HINTS
    "${RPC_THIRD_PARTY_PREFIX}/lib"
    "${RPC_THIRD_PARTY_PREFIX}/lib64"
)

set(CMAKE_BUILD_RPATH "${RPC_THIRD_PARTY_PREFIX}/lib;${RPC_THIRD_PARTY_PREFIX}/lib64")
set(CMAKE_INSTALL_RPATH "${RPC_THIRD_PARTY_PREFIX}/lib;${RPC_THIRD_PARTY_PREFIX}/lib64")

set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE_HO OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS_HO OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_PIC ON CACHE BOOL "" FORCE)
add_subdirectory(3rdparty/spdlog)

find_package(Protobuf CONFIG QUIET)
if(NOT Protobuf_FOUND)
    find_package(Protobuf REQUIRED)
endif()
find_package(gRPC CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(absl CONFIG REQUIRED)

set(PROTOBUF_LIB_TARGET protobuf::libprotobuf)
if(NOT TARGET protobuf::libprotobuf)
    if(TARGET protobuf::protobuf)
        set(PROTOBUF_LIB_TARGET protobuf::protobuf)
    elseif(DEFINED Protobuf_LIBRARIES)
        set(PROTOBUF_LIB_TARGET ${Protobuf_LIBRARIES})
    else()
        message(FATAL_ERROR "Protobuf was found, but no usable protobuf library target is available.")
    endif()
endif()

find_path(BRPC_INCLUDE_PATH brpc/server.h HINTS ${_THIRD_PARTY_INCLUDE_HINTS})
find_library(BRPC_LIB NAMES brpc libbrpc.a HINTS ${_THIRD_PARTY_LIBRARY_HINTS})
find_path(GFLAGS_INCLUDE_PATH gflags/gflags.h HINTS ${_THIRD_PARTY_INCLUDE_HINTS})
find_library(GFLAGS_LIBRARY NAMES gflags libgflags HINTS ${_THIRD_PARTY_LIBRARY_HINTS})
find_path(LEVELDB_INCLUDE_PATH NAMES leveldb/db.h HINTS ${_THIRD_PARTY_INCLUDE_HINTS})
find_library(LEVELDB_LIB NAMES leveldb HINTS ${_THIRD_PARTY_LIBRARY_HINTS})

find_program(_GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin HINTS "${RPC_THIRD_PARTY_PREFIX}/bin")
find_program(_PROTOBUF_PROTOC protoc HINTS "${RPC_THIRD_PARTY_PREFIX}/bin")

if(NOT BRPC_INCLUDE_PATH OR NOT BRPC_LIB)
    message(FATAL_ERROR "bRPC was not found. Build dependencies with ./scripts/build_deps.sh or set RPC_THIRD_PARTY_PREFIX.")
endif()

if(NOT GFLAGS_INCLUDE_PATH OR NOT GFLAGS_LIBRARY)
    message(FATAL_ERROR "gflags was not found. Install it system-wide or into ${RPC_THIRD_PARTY_PREFIX}.")
endif()

if(NOT LEVELDB_INCLUDE_PATH OR NOT LEVELDB_LIB)
    message(FATAL_ERROR "leveldb was not found. Install it system-wide or into ${RPC_THIRD_PARTY_PREFIX}.")
endif()

if(NOT _GRPC_CPP_PLUGIN_EXECUTABLE)
    message(FATAL_ERROR "grpc_cpp_plugin was not found. Build dependencies with ./scripts/build_deps.sh.")
endif()

if(NOT _PROTOBUF_PROTOC)
    message(FATAL_ERROR "protoc was not found. Build dependencies with ./scripts/build_deps.sh.")
endif()

set(ABSL_LIBRARIES
    absl::absl_check
    absl::absl_log
    absl::algorithm
    absl::base
    absl::bind_front
    absl::bits
    absl::btree
    absl::cleanup
    absl::cord
    absl::check
    absl::core_headers
    absl::debugging
    absl::die_if_null
    absl::dynamic_annotations
    absl::flags
    absl::flags_parse
    absl::flat_hash_map
    absl::flat_hash_set
    absl::function_ref
    absl::hash
    absl::layout
    absl::log
    absl::log_initialize
    absl::log_severity
    absl::memory
    absl::node_hash_map
    absl::node_hash_set
    absl::optional
    absl::span
    absl::status
    absl::statusor
    absl::strings
    absl::synchronization
    absl::time
    absl::type_traits
    absl::utility
    absl::variant
)
