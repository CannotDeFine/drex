add_executable(remote_server
    server.cc
    server_logging.cc
    remote_service_implement.cc
    task_executor.cc
)
target_include_directories(remote_server PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(remote_server
    PRIVATE
        project_options
        project_paths
        remote_grpc_proto
        spdlog::spdlog_header_only
        ${ABSL_LIBRARIES}
        util
)
