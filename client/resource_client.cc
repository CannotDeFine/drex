#include "resource_control.pb.h"

#include <brpc/channel.h>
#include <butil/logging.h>

#include <iostream>
#include <sstream>

// Converts a uint32 IP value to dotted decimal string.
std::string Uint32ToIP(uint32_t ip_uint32) {
    uint32_t ip = ntohl(ip_uint32);
    std::ostringstream oss;
    oss << ((ip >> 24) & 0xFF) << "."
        << ((ip >> 16) & 0xFF) << "."
        << ((ip >> 8) & 0xFF)  << "."
        << (ip & 0xFF);
    return oss.str();
}

int main () {
    brpc::Channel channel;
    brpc::ChannelOptions options;
    options.protocol = "baidu_std";
    options.timeout_ms = 5000;
    options.max_retry = 3;

    if (channel.Init("10.26.42.231:8000", "", &options) != 0) {
        LOG(ERROR) << "Init Channel Failed";
        return -1;
    }

    hcp::ResourceControlService_Stub stub(&channel);

    // Step 1: apply for resource.
    hcp::ApplyResourceRequest apply_req;
    hcp::ApplyResourceResponse apply_res;
    brpc::Controller cntl1;
    apply_req.set_type("CPU");
    apply_req.set_nums(1);
    stub.apply_resource(&cntl1, &apply_req, &apply_res, nullptr);

    if (cntl1.Failed() || apply_res.status().errcode() != 0) {
        LOG(ERROR) << "Apply Resource failed: "
                   << (cntl1.Failed() ? cntl1.ErrorText() : apply_res.status(). errmsg());
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
                   << (cntl2.Failed() ? cntl2.ErrorText() : query_res.status().errmsg());
        return -1;
    }

    // Step 3: print node addresses.
    for (int i = 0; i < query_res.rinfos_size(); ++i) {
        const hcp::ResourceInfo& info = query_res.rinfos(i);
        if (info.has_node()) {
            const hcp::NodeId& node = info.node();
            std::string ip_str = Uint32ToIP(node.ip());
            std::cout << "Resource[" << i << "]: IP=" << ip_str
                      << ", Port=" << node.port() << std::endl;
        }
    }

    return 0;

}
