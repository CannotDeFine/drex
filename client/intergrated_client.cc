#include "client_api.h"
#include "resource_control.pb.h"

#include <brpc/channel.h>
#include <butil/logging.h>
#include <grpcpp/grpcpp.h>

#include <arpa/inet.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct NodeInfo {
    std::string ip;
    int port = 0;
};

std::string Uint32ToIp(uint32_t ip_uint32) {
    uint32_t ip = ntohl(ip_uint32);
    std::ostringstream oss;
    oss << ((ip >> 24) & 0xFF) << "." << ((ip >> 16) & 0xFF) << "."
        << ((ip >> 8) & 0xFF) << "." << (ip & 0xFF);
    return oss.str();
}

NodeInfo ApplyResourceAndGetNode(const std::string &controller_addr, const std::string &resource_type,
                                 int resource_count) {
    brpc::Channel channel;
    brpc::ChannelOptions options;
    options.protocol = "baidu_std";
    options.timeout_ms = 5000;
    options.max_retry = 3;

    if (channel.Init(controller_addr.c_str(), "", &options) != 0) {
        throw std::runtime_error("Failed to initialize brpc channel.");
    }

    hcp::ResourceControlService_Stub stub(&channel);

    hcp::ApplyResourceRequest apply_request;
    hcp::ApplyResourceResponse apply_response;
    brpc::Controller apply_cntl;
    apply_request.set_type(resource_type);
    apply_request.set_nums(resource_count);
    stub.apply_resource(&apply_cntl, &apply_request, &apply_response, nullptr);

    if (apply_cntl.Failed() || apply_response.status().errcode() != 0) {
        throw std::runtime_error(std::string("Apply resource failed: ") +
                                 (apply_cntl.Failed() ? apply_cntl.ErrorText() : apply_response.status().errmsg()));
    }

    const std::string task_id = apply_response.task_id();

    hcp::QueryResourceRequest query_request;
    hcp::QueryResourceResponse query_response;
    brpc::Controller query_cntl;
    query_request.set_task_id(task_id);
    stub.query_resource(&query_cntl, &query_request, &query_response, nullptr);

    if (query_cntl.Failed() || query_response.status().errcode() != 0) {
        throw std::runtime_error(std::string("Query resource failed: ") +
                                 (query_cntl.Failed() ? query_cntl.ErrorText() : query_response.status().errmsg()));
    }

    if (query_response.rinfos_size() <= 0 || !query_response.rinfos(0).has_node()) {
        throw std::runtime_error("No node information returned.");
    }

    const hcp::NodeId &node = query_response.rinfos(0).node();
    NodeInfo info;
    info.ip = Uint32ToIp(node.ip());
    info.port = node.port();
    return info;
}

int ExecuteTask(const std::string &target, const std::vector<UploadFileSpec> &files, const std::string &workspace_subdir,
                const std::string &command, const std::string &local_output_dir) {
    RemoteServiceClient client(grpc::CreateChannel(target, grpc::InsecureChannelCredentials()));
    RemoteServiceClient::TaskSubmitReport report;
    grpc::Status status = client.TaskSubmit(files, workspace_subdir, command, &report);
    if (!status.ok()) {
        std::cerr << "RPC failed: " << status.error_message() << std::endl;
        return 1;
    }

    if (report.response.status() != remote_service::kSuccess) {
        std::cerr << "Server execution failed: " << report.response.message() << std::endl;
        return 1;
    }

    std::error_code ec;
    std::filesystem::create_directories(local_output_dir, ec);
    if (ec) {
        std::cerr << "Failed to create local output directory: " << local_output_dir << std::endl;
        return 1;
    }

    std::filesystem::path archive_path = std::filesystem::path(local_output_dir) / "server_output.tar.gz";
    std::ofstream outfile(archive_path, std::ios::binary | std::ios::trunc);
    if (!outfile.is_open()) {
        std::cerr << "Failed to save server outputs to " << archive_path << std::endl;
        return 1;
    }
    const std::string &archive = report.response.output_archive();
    outfile.write(archive.data(), static_cast<std::streamsize>(archive.size()));
    if (!outfile.good()) {
        std::cerr << "Failed to write server output archive." << std::endl;
        return 1;
    }

    std::cout << "Upload completed" << std::endl;
    std::cout << "Server message: " << report.response.message() << std::endl;
    std::cout << "Saved server outputs to: " << archive_path << std::endl;
    std::cout << "Elapsed: " << report.elapsed_seconds << " seconds" << std::endl;

    return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 8) {
        std::cerr << "Usage: " << argv[0]
                  << " <controller_addr(host:port)> <src_dir> <workspace_subdir> <command> <local_output_dir> <resource_type> <resource_count>"
                  << std::endl;
        return 1;
    }

    const std::string controller_addr = argv[1];
    const std::string src_dir = argv[2];
    const std::string workspace_subdir = argv[3];
    const std::string command = argv[4];
    const std::string local_output_dir = argv[5];
    const std::string resource_type = argv[6];

    int resource_count = 0;
    try {
        resource_count = std::stoi(argv[7]);
    } catch (const std::exception &ex) {
        std::cerr << "Invalid resource_count: " << ex.what() << std::endl;
        return 1;
    }

    if (command.empty()) {
        std::cerr << "Command must not be empty." << std::endl;
        return 1;
    }

    if (resource_type.empty()) {
        std::cerr << "Resource type must not be empty." << std::endl;
        return 1;
    }

    if (resource_count <= 0) {
        std::cerr << "Resource count must be positive." << std::endl;
        return 1;
    }

    std::vector<UploadFileSpec> files;
    if (!CollectDirectoryFiles(src_dir, &files)) {
        return 1;
    }

    grpc::Status validation = ValidateUploadFiles(files);
    if (!validation.ok()) {
        std::cerr << validation.error_message() << std::endl;
        return 1;
    }

    try {
        NodeInfo node = ApplyResourceAndGetNode(controller_addr, resource_type, resource_count);
        const std::string target = node.ip + ":" + std::to_string(static_cast<int>(node.port));
        return ExecuteTask(target, files, workspace_subdir, command, local_output_dir);
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << std::endl;
        return 1;
    }
}
