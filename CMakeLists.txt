cmake_minimum_required(VERSION 3.16)

project(DReX C CXX)
include(cmake/Dependencies.cmake)

add_library(project_options INTERFACE)
target_compile_features(project_options INTERFACE cxx_std_17)

add_library(project_paths INTERFACE)
target_include_directories(project_paths SYSTEM INTERFACE
    "${BRPC_INCLUDE_PATH}"
    "${GFLAGS_INCLUDE_PATH}"
    "${LEVELDB_INCLUDE_PATH}"
    "${RPC_THIRD_PARTY_PREFIX}/include"
)

add_subdirectory(proto)
add_subdirectory(server)
add_subdirectory(client)
