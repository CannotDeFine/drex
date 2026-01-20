#include "resource_control.pb.h"

#include <brpc/channel.h>
#include <butil/logging.h>

#include <iostream>
#include <stdexcept>
#include <string>

struct ResourceRequest {
    std::string controller_addr;
    std::string resource_type;
    int resource_count = 0;
    int mem_req = -1;
    int core_req = -1;
};

bool ParseArguments(int argc, char **argv, ResourceRequest *req) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0]
                  << " --controller=<host:port> --type=<resource_type> --count=<resource_count>"
                     " [--mem=<mem_req>] [--cores=<core_req>]"
                  << std::endl;
        return false;
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        const std::string controller_prefix = "--controller=";
        const std::string type_prefix = "--type=";
        const std::string count_prefix = "--count=";
        const std::string mem_prefix = "--mem=";
        const std::string cores_prefix = "--cores=";

        if (arg.rfind(controller_prefix, 0) == 0) {
            req->controller_addr = arg.substr(controller_prefix.size());
        } else if (arg.rfind(type_prefix, 0) == 0) {
            req->resource_type = arg.substr(type_prefix.size());
        } else if (arg.rfind(count_prefix, 0) == 0) {
            try {
                req->resource_count = std::stoi(arg.substr(count_prefix.size()));
            } catch (const std::exception &ex) {
                LOG(ERROR) << "Invalid --count value: " << ex.what();
                return false;
            }
        } else if (arg.rfind(mem_prefix, 0) == 0) {
            try {
                req->mem_req = std::stoi(arg.substr(mem_prefix.size()));
            } catch (const std::exception &ex) {
                LOG(ERROR) << "Invalid --mem value: " << ex.what();
                return false;
            }
        } else if (arg.rfind(cores_prefix, 0) == 0) {
            try {
                req->core_req = std::stoi(arg.substr(cores_prefix.size()));
            } catch (const std::exception &ex) {
                LOG(ERROR) << "Invalid --cores value: " << ex.what();
                return false;
            }
        } else {
            LOG(ERROR) << "Unknown argument: " << arg;
            return false;
        }
    }

    if (req->controller_addr.empty()) {
        LOG(ERROR) << "--controller must be provided.";
        return false;
    }

    if (req->resource_type.empty()) {
        LOG(ERROR) << "--type must be provided.";
        return false;
    }

    if (req->resource_count <= 0) {
        LOG(ERROR) << "--count must be a positive integer.";
        return false;
    }

    if (req->mem_req == 0 || req->core_req == 0) {
        LOG(ERROR) << "--mem/--cores must be positive integers when provided.";
        return false;
    }

    if (req->mem_req < -1 || req->core_req < -1) {
        LOG(ERROR) << "--mem/--cores must be positive integers when provided.";
        return false;
    }

    return true;
}

bool CheckStep(const std::string &step_name, const brpc::Controller &cntl, const hcp::Status &status) {
    if (cntl.Failed()) {
        LOG(ERROR) << step_name << " RPC failed: " << cntl.ErrorText();
        return false;
    }
    if (status.errcode() != 0) {
        LOG(ERROR) << step_name << " failed: " << status.errmsg();
        return false;
    }
    return true;
}

int main(int argc, char **argv) {
    ResourceRequest req;
    if (!ParseArguments(argc, argv, &req)) {
        return -1;
    }

    brpc::Channel channel;
    brpc::ChannelOptions options;
    options.protocol = "baidu_std";
    options.timeout_ms = 5000;
    options.max_retry = 3;

    if (channel.Init(req.controller_addr.c_str(), "", &options) != 0) {
        LOG(ERROR) << "Init Channel Failed";
        return -1;
    }

    hcp::ResourceControlService_Stub stub(&channel);

    // Step 1: apply for resource.
    hcp::ApplyResourceRequest apply_req;
    hcp::ApplyResourceResponse apply_res;
    brpc::Controller cntl1;
    apply_req.set_type(req.resource_type);
    apply_req.set_nums(req.resource_count);
    if (req.mem_req > 0) {
        apply_req.set_memreq(req.mem_req);
    }
    if (req.core_req > 0) {
        apply_req.set_corereq(req.core_req);
    }
    stub.apply_resource(&cntl1, &apply_req, &apply_res, nullptr);

    if (!CheckStep("apply_resource", cntl1, apply_res.status())) {
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

    if (!CheckStep("query_resource", cntl2, query_res.status())) {
        return -1;
    }

    // Step 3: print node addresses.
    for (int i = 0; i < query_res.rinfos_size(); ++i) {
        const hcp::ResourceInfo &info = query_res.rinfos(i);
        if (info.has_node()) {
            const hcp::NodeId &node = info.node();
            std::string ip_str = node.ip();
            std::cout << "Resource[" << i << "]: IP=" << ip_str
                      << ", Port=" << node.port() << std::endl;
        }
    }

    return 0;
}
