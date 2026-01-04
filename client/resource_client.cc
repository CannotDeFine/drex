#include "resource_control.pb.h"

#include <brpc/channel.h>
#include <butil/logging.h>

#include <arpa/inet.h>

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

// Converts a uint32 IP value to dotted decimal string.
std::string Uint32ToIP(uint32_t ip_uint32) {
    uint32_t ip = ntohl(ip_uint32);
    std::ostringstream oss;
    oss << ((ip >> 24) & 0xFF) << "." << ((ip >> 16) & 0xFF) << "."
        << ((ip >> 8) & 0xFF) << "." << (ip & 0xFF);
    return oss.str();
}

int main(int argc, char **argv) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0]
                  << " --controller=<host:port> --type=<resource_type> --count=<resource_count>" << std::endl;
        return -1;
    }

    std::string controller_addr;
    std::string resource_type;
    int resource_count = 0;

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        const std::string controller_prefix = "--controller=";
        const std::string type_prefix = "--type=";
        const std::string count_prefix = "--count=";

        if (arg.rfind(controller_prefix, 0) == 0) {
            controller_addr = arg.substr(controller_prefix.size());
        } else if (arg.rfind(type_prefix, 0) == 0) {
            resource_type = arg.substr(type_prefix.size());
        } else if (arg.rfind(count_prefix, 0) == 0) {
            const std::string value = arg.substr(count_prefix.size());
            try {
                resource_count = std::stoi(value);
            } catch (const std::exception &ex) {
                LOG(ERROR) << "Invalid --count value: " << ex.what();
                return -1;
            }
        } else {
            LOG(ERROR) << "Unknown argument: " << arg;
            return -1;
        }
    }

    if (controller_addr.empty()) {
        LOG(ERROR) << "--controller must be provided.";
        return -1;
    }

    if (resource_type.empty()) {
        LOG(ERROR) << "--type must be provided.";
        return -1;
    }

    if (resource_count <= 0) {
        LOG(ERROR) << "--count must be a positive integer.";
        return -1;
    }

    brpc::Channel channel;
    brpc::ChannelOptions options;
    options.protocol = "baidu_std";
    options.timeout_ms = 5000;
    options.max_retry = 3;

    if (channel.Init(controller_addr.c_str(), "", &options) != 0) {
        LOG(ERROR) << "Init Channel Failed";
        return -1;
    }

    hcp::ResourceControlService_Stub stub(&channel);

    // Step 1: apply for resource.
    hcp::ApplyResourceRequest apply_req;
    hcp::ApplyResourceResponse apply_res;
    brpc::Controller cntl1;
    apply_req.set_type(resource_type);
    apply_req.set_nums(resource_count);
    stub.apply_resource(&cntl1, &apply_req, &apply_res, nullptr);

    if (cntl1.Failed() || apply_res.status().errcode() != 0) {
        LOG(ERROR) << "Apply Resource failed: "
                   << (cntl1.Failed() ? cntl1.ErrorText()
                                      : apply_res.status().errmsg());
        return -1;
    }

    std::string task_id = apply_res.task_id();
    LOG(INFO) << "Apply success, task_id = " << task_id;

    // Step 2: query resource details.
    hcp::QueryResourceRequest query_req;
    hcp::QueryResourceResponse query_res;

    brpc::Controller cntl2;
    query_req.set_task_id(task_id);
    stub.query_resource(&cntl2, &query_req, &query_res, nullptr);

    if (cntl2.Failed() || query_res.status().errcode() != 0) {
        LOG(ERROR) << "query_resource failed: "
                   << (cntl2.Failed() ? cntl2.ErrorText()
                                      : query_res.status().errmsg());
        return -1;
    }

    // Step 3: print node addresses.
    for (int i = 0; i < query_res.rinfos_size(); ++i) {
        const hcp::ResourceInfo &info = query_res.rinfos(i);
        if (info.has_node()) {
            const hcp::NodeId &node = info.node();
            std::string ip_str = Uint32ToIP(node.ip());
            std::cout << "Resource[" << i << "]: IP=" << ip_str
                      << ", Port=" << node.port() << std::endl;
        }
    }

    return 0;
}
