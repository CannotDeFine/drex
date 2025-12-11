cmake_minimum_required(VERSION 3.16)

project(RemoteService C CXX)
set(CMAKE_PREFIX_PATH "/usr/local") 

# Dependencies
# system_libaries
find_package(CURL REQUIRED)
find_package(gRPC REQUIRED)
find_package(Protobuf REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(absl REQUIRED)

# find brpc
find_path(BRPC_INCLUDE_PATH brpc/server.h)
find_library(BRPC_LIB NAMES brpc libbrpc.a)
# find gflags
find_path(GFLAGS_INCLUDE_PATH gflags/gflags.h)
find_library(GFLAGS_LIBRARY NAMES gflags libgflags)
# find levledb
find_path(LEVELDB_INCLUDE_PATH NAMES leveldb/db.h)
find_library(LEVELDB_LIB NAMES leveldb)


# find grpc_cpp_plugin
find_program(_GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin)
# find protoc
find_program(_PROTOBUF_PROTOC protoc)

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


include_directories("${CMAKE_CURRENT_BINARY_DIR}")
include_directories("${remote_proto_path}")
include_directories("${resource_control_path}")
include_directories("${BRPC_INCLUDE_PATH}")
include_directories("${GFLAGS_INCLUDE_PATH}")
include_directories("${LEVELDB_INCLUDE_PATH}")

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
    gRPC::grpc++
    protobuf::libprotobuf
    gRPC::grpc++_reflection
)

# server 
add_executable(remote_server
    server/server.cc
    server/remote_service_implement.cc
    server/task_executor.cc
)
target_link_libraries(remote_server
    remote_grpc_proto
    
    ${ABSL_LIBRARIES}
)

# client
add_executable(remote_client
    client/client.cc
    client/client_api.cc
)
target_link_libraries(remote_client
    remote_grpc_proto
    ${ABSL_LIBRARIES}
)

# resource client
set(BRPC_DEPS
    ${CMAKE_THREAD_LIBS_INIT}
    ${GFLAGS_LIBRARY}
    ${PROTOBUF_LIBRARIES}
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
    ${BRPC_LIB}
    ${BRPC_DEPS}
    ${ABSL_LIBRARIES}
)

add_executable(intergrated_client
    client/intergrated_client.cc
    client/client_api.cc
    ${resource_control_proto_hdrs}
    ${resource_control_proto_srcs}
)
target_link_libraries(intergrated_client
    remote_grpc_proto
    ${BRPC_LIB}
    ${BRPC_DEPS}
    ${ABSL_LIBRARIES}
)
