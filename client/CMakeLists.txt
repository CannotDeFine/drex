set(BRPC_DEPS
    ${CMAKE_THREAD_LIBS_INIT}
    ${GFLAGS_LIBRARY}
    ${PROTOBUF_LIB_TARGET}
    ${LEVELDB_LIB}
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
    dl
)

add_executable(remote_client
    client.cc
    client_logging.cc
    client_api.cc
)
target_include_directories(remote_client PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(remote_client
    PRIVATE
        project_options
        project_paths
        remote_grpc_proto
        spdlog::spdlog_header_only
        ${ABSL_LIBRARIES}
)

add_executable(resource_client
    resource_client.cc
)
target_include_directories(resource_client PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(resource_client
    PRIVATE
        project_options
        project_paths
        resource_control_proto
        ${BRPC_LIB}
        ${BRPC_DEPS}
        ${ABSL_LIBRARIES}
)

add_executable(integrated_client
    client_logging.cc
    integrated_client.cc
    client_api.cc
)
target_include_directories(integrated_client PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(integrated_client
    PRIVATE
        project_options
        project_paths
        remote_grpc_proto
        resource_control_proto
        spdlog::spdlog_header_only
        ${BRPC_LIB}
        ${BRPC_DEPS}
        ${ABSL_LIBRARIES}
)
