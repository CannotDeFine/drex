cmake_minimum_required(VERSION 3.16)

project(DReX C CXX)
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

# Dependencies
# system_libaries
# find_package(CURL REQUIRED)
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

# find brpc
find_path(BRPC_INCLUDE_PATH brpc/server.h HINTS ${_THIRD_PARTY_INCLUDE_HINTS})
find_library(BRPC_LIB NAMES brpc libbrpc.a HINTS ${_THIRD_PARTY_LIBRARY_HINTS})
# find gflags
find_path(GFLAGS_INCLUDE_PATH gflags/gflags.h HINTS ${_THIRD_PARTY_INCLUDE_HINTS})
find_library(GFLAGS_LIBRARY NAMES gflags libgflags HINTS ${_THIRD_PARTY_LIBRARY_HINTS})
# find levledb
find_path(LEVELDB_INCLUDE_PATH NAMES leveldb/db.h HINTS ${_THIRD_PARTY_INCLUDE_HINTS})
find_library(LEVELDB_LIB NAMES leveldb HINTS ${_THIRD_PARTY_LIBRARY_HINTS})


# find grpc_cpp_plugin
find_program(_GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin HINTS "${RPC_THIRD_PARTY_PREFIX}/bin")
# find protoc
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

# generate source and header 
# remote_service
set(remote_proto "${CMAKE_CURRENT_SOURCE_DIR}/proto/remote_service.proto")
get_filename_component(remote_proto_path "${remote_proto}" PATH)
set(remote_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/remote_service.pb.cc")
set(remote_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/remote_service.pb.h")
set(remote_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/remote_service.grpc.pb.cc")
set(remote_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/remote_service.grpc.pb.h")
add_custom_command(
    OUTPUT "${remote_proto_srcs}" "${remote_proto_hdrs}" "${remote_grpc_srcs}" "${remote_grpc_hdrs}"
    COMMAND ${_PROTOBUF_PROTOC}
    ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}/"
         --cpp_out "${CMAKE_CURRENT_BINARY_DIR}/"
         -I "${remote_proto_path}"
         --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
         "${remote_proto}"
    DEPENDS "${remote_proto}"
)

# resource_control
set(resource_control_proto "${CMAKE_CURRENT_SOURCE_DIR}/proto/resource_control.proto")
get_filename_component(resource_control_path "${resource_control_proto}" PATH)
set(resource_control_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/resource_control.pb.cc")
set(resource_control_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/resource_control.pb.h")
add_custom_command(
    OUTPUT "${resource_control_proto_hdrs}" "${resource_control_proto_srcs}"
    COMMAND ${_PROTOBUF_PROTOC}
    ARGS --cpp_out "${CMAKE_CURRENT_BINARY_DIR}/"
         -I "${resource_control_path}/"
         "${resource_control_proto}"
    DEPENDS "${resource_control_proto}"
)

add_library(project_options INTERFACE)
target_compile_features(project_options INTERFACE cxx_std_17)

add_library(project_paths INTERFACE)
# Generated protobuf headers live in the build directory, while local imports stay under proto/.
target_include_directories(project_paths INTERFACE
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${remote_proto_path}"
    "${resource_control_path}"
)
# Vendor and external library headers are marked as system headers to keep warnings focused on this repo.
target_include_directories(project_paths SYSTEM INTERFACE
    "${BRPC_INCLUDE_PATH}"
    "${GFLAGS_INCLUDE_PATH}"
    "${LEVELDB_INCLUDE_PATH}"
    "${RPC_THIRD_PARTY_PREFIX}/include"
)

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

# remote_grpc_proto
add_library(remote_grpc_proto
    ${remote_grpc_srcs}
    ${remote_grpc_hdrs}
    ${remote_proto_srcs}
    ${remote_proto_hdrs}
)
target_link_libraries(remote_grpc_proto
    PUBLIC
        project_options
        project_paths
        gRPC::grpc++
        ${PROTOBUF_LIB_TARGET}
        gRPC::grpc++_reflection
)

# server
add_executable(remote_server
    server/server.cc
    server/server_logging.cc
    server/remote_service_implement.cc
    server/task_executor.cc
)
target_link_libraries(remote_server
    PRIVATE
        project_options
        project_paths
        remote_grpc_proto
        spdlog::spdlog_header_only
        ${ABSL_LIBRARIES}
        util
)

# client
add_executable(remote_client
    client/client.cc
    client/client_logging.cc
    client/client_api.cc
)
target_link_libraries(remote_client
    PRIVATE
        project_options
        project_paths
        remote_grpc_proto
        spdlog::spdlog_header_only
        ${ABSL_LIBRARIES}
)

# resource client
set(BRPC_DEPS
    ${CMAKE_THREAD_LIBS_INIT}
    ${GFLAGS_LIBRARY}
    ${PROTOBUF_LIB_TARGET}
    ${LEVELDB_LIB}
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
    dl
)
add_executable(resource_client
    client/resource_client.cc
    ${resource_control_proto_hdrs}
    ${resource_control_proto_srcs}
)
target_link_libraries(resource_client
    PRIVATE
        project_options
        project_paths
        ${BRPC_LIB}
        ${BRPC_DEPS}
        ${ABSL_LIBRARIES}
)

add_executable(integrated_client
    client/client_logging.cc
    client/integrated_client.cc
    client/client_api.cc
    ${resource_control_proto_hdrs}
    ${resource_control_proto_srcs}
)
target_link_libraries(integrated_client
    PRIVATE
        project_options
        project_paths
        remote_grpc_proto
        spdlog::spdlog_header_only
        ${BRPC_LIB}
        ${BRPC_DEPS}
        ${ABSL_LIBRARIES}
)
