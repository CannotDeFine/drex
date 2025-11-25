#include "remote_service.grpc.pb.h"
#include "remote_service.pb.h"
#include "resource_control.pb.h"

#include <brpc/channel.h>
#include <butil/logging.h>
#include <grpcpp/grpcpp.h>

#include <arpa/inet.h>
#include <sys/time.h>

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

using grpc::Channel;
using grpc::ClientContext;
using grpc::ClientWriter;
using grpc::Status;
using remote_service::ReqDeviceType;
using remote_service::TaskManage;
using remote_service::TaskRequest;
using remote_service::TaskResult;

constexpr size_t kChunkSize = 1024 * 1024;

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

NodeInfo ApplyResourceAndGetNode(const std::string &controller_addr) {
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
    apply_request.set_type("CPU");
    apply_request.set_nums(1);
    stub.apply_resource(&apply_cntl, &apply_request, &apply_response, nullptr);

    if (apply_cntl.Failed() || apply_response.status().errcode() != 0) {
        throw std::runtime_error(std::string("Apply resource failed: ") +
                                 (apply_cntl.Failed()
                                      ? apply_cntl.ErrorText()
                                      : apply_response.status().errmsg()));
    }

    const std::string task_id = apply_response.task_id();

    hcp::QueryResourceRequest query_request;
    hcp::QueryResourceResponse query_response;
    brpc::Controller query_cntl;
    query_request.set_task_id(task_id);
    stub.query_resource(&query_cntl, &query_request, &query_response, nullptr);

    if (query_cntl.Failed() || query_response.status().errcode() != 0) {
        throw std::runtime_error(std::string("Query resource failed: ") +
                                 (query_cntl.Failed()
                                      ? query_cntl.ErrorText()
                                      : query_response.status().errmsg()));
    }

    if (query_response.rinfos_size() <= 0 ||
        !query_response.rinfos(0).has_node()) {
        throw std::runtime_error("No node information returned.");
    }

    const hcp::NodeId &node = query_response.rinfos(0).node();
    NodeInfo info;
    info.ip = Uint32ToIp(node.ip());
    info.port = node.port();
    return info;
}

class RemoteServiceClient {
  public:
    explicit RemoteServiceClient(std::shared_ptr<Channel> channel)
        : stub_(TaskManage::NewStub(channel)) {}

    void TaskSubmit(const std::string &local_file, const std::string &save_path,
                    ReqDeviceType device_type, const std::string &result_path);

  private:
    std::unique_ptr<TaskManage::Stub> stub_;
};

void RemoteServiceClient::TaskSubmit(const std::string &local_file,
                                     const std::string &save_path,
                                     ReqDeviceType device_type,
                                     const std::string &result_path) {
    std::ifstream infile(local_file, std::ios::binary);
    if (!infile.is_open()) {
        std::cerr << "Cannot open file: " << local_file << std::endl;
        return;
    }

    ClientContext context;
    TaskResult response;
    std::unique_ptr<ClientWriter<TaskRequest>> writer(
        stub_->TaskSubmission(&context, &response));

    std::vector<char> buffer(kChunkSize);
    bool first_chunk = true;
    int64_t total_bytes = 0;

    std::error_code ec;
    int64_t declared_size = -1;
    const uintmax_t on_disk = std::filesystem::file_size(local_file, ec);
    if (!ec) {
        declared_size = static_cast<int64_t>(on_disk);
    }

    timeval start, end;
    gettimeofday(&start, nullptr);

    while (infile) {
        infile.read(buffer.data(), buffer.size());
        const std::streamsize bytes_read = infile.gcount();
        if (bytes_read <= 0) {
            break;
        }

        TaskRequest request;
        request.set_task(buffer.data(), bytes_read);
        request.set_path(save_path);
        request.set_file_type(remote_service::kFileExecutable);
        request.set_file_start(first_chunk);
        if (first_chunk && declared_size >= 0) {
            request.set_file_size(declared_size);
        }
        if (infile.eof()) {
            request.set_file_end(true);
        }
        if (first_chunk) {
            request.set_device_type(device_type);
            request.set_ip_address("integrated-client");
            if (!result_path.empty()) {
                request.set_result_path(result_path);
            }
            first_chunk = false;
        }

        total_bytes += bytes_read;
        if (!writer->Write(request)) {
            std::cerr << "Stream write failed (connection closed by server)."
                      << std::endl;
            break;
        }
    }

    writer->WritesDone();
    Status status = writer->Finish();

    gettimeofday(&end, nullptr);
    const double elapsed = (end.tv_sec - start.tv_sec) +
                           (end.tv_usec - start.tv_usec) / 1'000'000.0;

    if (!status.ok()) {
        std::cerr << "RPC failed: " << status.error_message() << std::endl;
    }

    if (response.status() != remote_service::kSuccess) {
        std::cerr << "Server execution failed: " << response.message()
                  << std::endl;
    }

    std::cout << "Upload completed" << std::endl;
    std::cout << "Client sent: " << total_bytes << " bytes" << std::endl;
    std::cout << "Server received: " << response.length() << " bytes"
              << std::endl;
    std::cout << "Server message: " << response.message() << std::endl;
    if (!response.result().empty()) {
        std::cout << "Server output (" << response.result().size()
                  << " bytes):" << std::endl;
        std::cout << response.result() << std::endl;
    }
    std::cout << "Elapsed: " << elapsed << " seconds" << std::endl;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0]
                  << " <controller_addr(host:port)> <local_file> <remote_path> "
                     "[device_type (0-5)] [result_path]"
                  << std::endl;
        return 1;
    }

    const std::string controller_addr = argv[1];
    const std::string local_file = argv[2];
    const std::string remote_path = argv[3];
    int device_arg = 0;
    if (argc >= 5) {
        device_arg = std::stoi(argv[4]);
    }
    std::string result_path;
    if (argc >= 6) {
        result_path = argv[5];
    }

    try {
        NodeInfo node = ApplyResourceAndGetNode(controller_addr);
        const std::string target =
            node.ip + ":" + std::to_string(static_cast<int>(node.port));

        RemoteServiceClient client(
            grpc::CreateChannel(target, grpc::InsecureChannelCredentials()));
        client.TaskSubmit(local_file, remote_path,
                          static_cast<ReqDeviceType>(device_arg), result_path);
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << std::endl;
        return 1;
    }

    return 0;
}
