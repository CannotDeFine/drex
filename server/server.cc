#include <absl/flags/flag.h>
#include <absl/flags/parse.h>
#include <absl/log/initialize.h>
#include <absl/strings/str_format.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include <iostream>
#include <memory>
#include <string>

#include "remote_service_implement.h"
#include "server_logging.h"

using grpc::Server;
using grpc::ServerBuilder;

// Bind address for the remote execution service.
ABSL_FLAG(uint16_t, port, 8063, "Server port for the service");
ABSL_FLAG(std::string, address, "0.0.0.0", "Server address");

void RunServer(const std::string &address, uint16_t port) {
    const std::string server_address = absl::StrFormat("%s:%d", address, port);
    InitializeServerLogging("workspace");

    RemoteServiceImplement service;

    // Keep bootstrap logic minimal here; request handling lives in
    // RemoteServiceImplement and TaskExecutor.
    grpc::EnableDefaultHealthCheckService(true);
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();

    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<Server> server(builder.BuildAndStart());
    LogServerInfo("remote_server listening on " + server_address);
    server->Wait();
}

int main(int argc, char **argv) {
    absl::ParseCommandLine(argc, argv);
    absl::InitializeLog();
    RunServer(absl::GetFlag(FLAGS_address), absl::GetFlag(FLAGS_port));
    return 0;
}
