set(remote_proto "${CMAKE_CURRENT_SOURCE_DIR}/remote_service.proto")
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

add_library(remote_grpc_proto
    ${remote_grpc_srcs}
    ${remote_grpc_hdrs}
    ${remote_proto_srcs}
    ${remote_proto_hdrs}
)
target_include_directories(remote_grpc_proto
    PUBLIC
        "${CMAKE_CURRENT_BINARY_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(remote_grpc_proto
    PUBLIC
        project_options
        project_paths
        gRPC::grpc++
        ${PROTOBUF_LIB_TARGET}
        gRPC::grpc++_reflection
)

set(resource_control_proto "${CMAKE_CURRENT_SOURCE_DIR}/resource_control.proto")
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

add_library(resource_control_proto
    ${resource_control_proto_hdrs}
    ${resource_control_proto_srcs}
)
target_include_directories(resource_control_proto
    PUBLIC
        "${CMAKE_CURRENT_BINARY_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(resource_control_proto
    PUBLIC
        project_options
        project_paths
        ${PROTOBUF_LIB_TARGET}
)
