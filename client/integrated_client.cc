#include "client_api.h"
#include "client_logging.h"
#include "resource_control.pb.h"

#include <brpc/channel.h>
#include <butil/logging.h>
#include <grpcpp/grpcpp.h>

#include <filesystem>
#include <algorithm>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct ResourceAllocation {
    std::string ip;
    int port = 0;
    std::string cgroup_path;
    std::vector<int> cpu_cores;
};

struct ProgramOptions {
    std::string controller_addr;
    std::string src_dir;
    std::string workspace_subdir;
    std::string command;
    std::string local_output_dir;
    std::string resource_type;
    int resource_count = 0;
    int mem_req = -1;
    int core_req = -1;
    int pid = -1;
    bool pty = false;
    XSchedConfig xsched;
};

void PrintUsage(const char *prog_name) {
    std::cerr << "Usage: " << prog_name
              << " <controller_addr(host:port)> <src_dir> <workspace_subdir> <command> <local_output_dir> <resource_type> <resource_count>"
                 " [--mem=<mem_req>] [--cores=<core_req>] [--pid=<pid>] [--pty] [--utilization=<0-100>]"
              << "\n   or: " << prog_name
              << " --controller=<host:port> --src_dir=<dir> --workspace_subdir=<name> --command=<cmd> --type=<resource_type>"
                 " [--count=<n>] [--local_output_dir=<dir>] [--mem=<mem_req>] [--cores=<core_req>] [--pid=<pid>] [--pty]"
                 " [--utilization=<0-100>]"
              << std::endl;
}

bool ParseArguments(int argc, char **argv, ProgramOptions *opts) {
    if (argc < 2) {
        PrintUsage(argv[0]);
        return false;
    }

    // Keep both the original positional form and the newer flag form so the
    // client remains compatible with existing scripts.
    const bool flag_mode = std::string(argv[1]).rfind("--", 0) == 0;
    if (!flag_mode) {
        if (argc < 8) {
            PrintUsage(argv[0]);
            return false;
        }

        opts->controller_addr = argv[1];
        opts->src_dir = argv[2];
        opts->workspace_subdir = argv[3];
        opts->command = argv[4];
        opts->local_output_dir = argv[5];
        opts->resource_type = argv[6];

        try {
            opts->resource_count = std::stoi(argv[7]);
        } catch (const std::exception &ex) {
            std::cerr << "Invalid resource_count: " << ex.what() << std::endl;
            return false;
        }
    } else {
        // Defaults for flags mode.
        opts->resource_count = 1;
        opts->local_output_dir = ".";
        opts->pid = static_cast<int>(::getpid());

        for (int i = 1; i < argc; ++i) {
            const std::string arg(argv[i]);
            if (arg == "--pty") {
                opts->pty = true;
                continue;
            }

            const auto ConsumeValue = [&](const std::string &prefix, std::string *out) -> bool {
                if (arg.rfind(prefix, 0) != 0) {
                    return false;
                }
                *out = arg.substr(prefix.size());
                return true;
            };

            std::string value;
            if (ConsumeValue("--controller=", &value)) {
                opts->controller_addr = value;
                continue;
            }
            if (ConsumeValue("--src_dir=", &value)) {
                opts->src_dir = value;
                continue;
            }
            if (ConsumeValue("--workspace_subdir=", &value)) {
                opts->workspace_subdir = value;
                continue;
            }
            if (ConsumeValue("--command=", &value)) {
                opts->command = value;
                continue;
            }
            if (ConsumeValue("--local_output_dir=", &value)) {
                opts->local_output_dir = value;
                continue;
            }
            if (ConsumeValue("--type=", &value)) {
                opts->resource_type = value;
                continue;
            }

            const std::string count_prefix = "--count=";
            if (arg.rfind(count_prefix, 0) == 0) {
                try {
                    opts->resource_count = std::stoi(arg.substr(count_prefix.size()));
                } catch (const std::exception &ex) {
                    std::cerr << "Invalid --count value: " << ex.what() << std::endl;
                    return false;
                }
                continue;
            }

            const std::string mem_prefix = "--mem=";
            if (arg.rfind(mem_prefix, 0) == 0) {
                try {
                    opts->mem_req = std::stoi(arg.substr(mem_prefix.size()));
                } catch (const std::exception &ex) {
                    std::cerr << "Invalid --mem value: " << ex.what() << std::endl;
                    return false;
                }
                continue;
            }

            const std::string cores_prefix = "--cores=";
            const std::string pid_prefix = "--pid=";
            if (arg.rfind(cores_prefix, 0) == 0) {
                try {
                    opts->core_req = std::stoi(arg.substr(cores_prefix.size()));
                } catch (const std::exception &ex) {
                    std::cerr << "Invalid --cores value: " << ex.what() << std::endl;
                    return false;
                }
                continue;
            }

            if (arg.rfind(pid_prefix, 0) == 0) {
                try {
                    opts->pid = std::stoi(arg.substr(pid_prefix.size()));
                } catch (const std::exception &ex) {
                    std::cerr << "Invalid --pid value: " << ex.what() << std::endl;
                    return false;
                }
                continue;
            }

            const std::string util_prefix = "--utilization=";
            if (arg.rfind(util_prefix, 0) == 0) {
                try {
                    opts->xsched.xsched_utilization = std::stoi(arg.substr(util_prefix.size()));
                } catch (const std::exception &ex) {
                    std::cerr << "Invalid --utilization value: " << ex.what() << std::endl;
                    return false;
                }
                continue;
            }

            std::cerr << "Unknown argument: " << arg << std::endl;
            PrintUsage(argv[0]);
            return false;
        }
    }

    if (opts->command.empty()) {
        std::cerr << "Command must not be empty." << std::endl;
        return false;
    }

    if (opts->local_output_dir.empty()) {
        std::cerr << "Local output dir must not be empty." << std::endl;
        return false;
    }

    if (opts->resource_type.empty()) {
        std::cerr << "Resource type must not be empty." << std::endl;
        return false;
    }

    if (opts->resource_count <= 0) {
        std::cerr << "Resource count must be positive." << std::endl;
        return false;
    }

    if (opts->mem_req == 0 || opts->core_req == 0) {
        std::cerr << "--mem/--cores must be positive integers when provided." << std::endl;
        return false;
    }
    if (opts->mem_req < -1 || opts->core_req < -1) {
        std::cerr << "--mem/--cores must be positive integers when provided." << std::endl;
        return false;
    }

    if (opts->pid == 0 || opts->pid < -1) {
        std::cerr << "--pid must be a positive integer when provided." << std::endl;
        return false;
    }

    if (opts->xsched.xsched_utilization < -1 || opts->xsched.xsched_utilization > 100) {
        std::cerr << "--utilization must be in [0, 100]." << std::endl;
        return false;
    }
    opts->xsched.enabled = opts->xsched.xsched_utilization >= 0;

    return true;
}

void PrintStageFailure(int stage, const std::string &message) {
    (void)stage;
    LogClientError(message);
}

std::string FormatBytes(int64_t bytes) {
    static const char *kUnits[] = {"B", "KB", "MB", "GB", "TB"};
    double size = static_cast<double>(bytes);
    size_t unit = 0;
    while (size >= 1024.0 && unit + 1 < std::size(kUnits)) {
        size /= 1024.0;
        ++unit;
    }
    std::ostringstream oss;
    if (unit == 0) {
        oss << static_cast<int64_t>(size) << ' ' << kUnits[unit];
    } else {
        oss << std::fixed << std::setprecision(2) << size << ' ' << kUnits[unit];
    }
    return oss.str();
}

std::string FormatSeconds(double seconds) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << seconds;
    return oss.str();
}

bool ReleaseResource(const std::string &controller_addr, const std::string &task_id) {
    if (task_id.empty()) {
        return true;
    }

    brpc::Channel channel;
    brpc::ChannelOptions options;
    options.protocol = "baidu_std";
    options.timeout_ms = 5000;
    options.max_retry = 3;

    if (channel.Init(controller_addr.c_str(), "", &options) != 0) {
        return false;
    }

    hcp::ResourceControlService_Stub stub(&channel);
    hcp::ReleaseResourceRequest release_request;
    hcp::ReleaseResourceResponse release_response;
    brpc::Controller release_cntl;
    release_request.set_task_id(task_id);
    stub.release_resource(&release_cntl, &release_request, &release_response, nullptr);
    if (release_cntl.Failed()) {
        return false;
    }
    if (!release_response.has_status() || release_response.status().errcode() != 0) {
        return false;
    }
    return true;
}

std::string FormatCpuCores(const std::vector<int> &cpu_cores) {
    std::ostringstream oss;
    for (size_t i = 0; i < cpu_cores.size(); ++i) {
        if (i != 0) {
            oss << ",";
        }
        oss << cpu_cores[i];
    }
    return oss.str();
}

ResourceAllocation ApplyResourceAndGetAllocation(const std::string &controller_addr, const std::string &resource_type, int resource_count, int mem_req, int core_req,
                                 int pid, std::string *task_id_out) {
    brpc::Channel channel;
    brpc::ChannelOptions options;
    options.protocol = "baidu_std";
    options.timeout_ms = 5000;
    options.max_retry = 3;

    if (channel.Init(controller_addr.c_str(), "", &options) != 0) {
        throw std::runtime_error("Failed to initialize brpc channel.");
    }

    hcp::ResourceControlService_Stub stub(&channel);

    // First reserve resources, then query the controller for the concrete node
    // that should receive the uploaded workload.
    hcp::ApplyResourceRequest apply_request;
    hcp::ApplyResourceResponse apply_response;
    brpc::Controller apply_cntl;
    apply_request.set_type(resource_type);
    apply_request.set_nums(resource_count);
    if (mem_req > 0) {
        apply_request.set_memreq(mem_req);
    }
    if (core_req > 0) {
        apply_request.set_corereq(core_req);
    }
    if (pid > 0) {
        apply_request.set_pid(pid);
    }
    stub.apply_resource(&apply_cntl, &apply_request, &apply_response, nullptr);

    if (apply_cntl.Failed() || apply_response.status().errcode() != 0) {
        throw std::runtime_error(std::string("Apply resource failed: ") + (apply_cntl.Failed() ? apply_cntl.ErrorText() : apply_response.status().errmsg()));
    }

    const std::string task_id = apply_response.task_id();
    if (task_id_out != nullptr) {
        *task_id_out = task_id;
    }

    hcp::QueryResourceRequest query_request;
    hcp::QueryResourceResponse query_response;
    brpc::Controller query_cntl;
    query_request.set_task_id(task_id);
    stub.query_resource(&query_cntl, &query_request, &query_response, nullptr);

    if (query_cntl.Failed() || query_response.status().errcode() != 0) {
        throw std::runtime_error(std::string("Query resource failed: ") + (query_cntl.Failed() ? query_cntl.ErrorText() : query_response.status().errmsg()));
    }

    if (query_response.rinfos_size() <= 0 || !query_response.rinfos(0).has_node()) {
        throw std::runtime_error("No node information returned.");
    }

    const hcp::ResourceInfo &resource = query_response.rinfos(0);
    const hcp::NodeId &node = resource.node();
    ResourceAllocation info;
    info.ip = node.ip();
    info.port = node.port();
    if (resource.has_cgroup_path()) {
        info.cgroup_path = resource.cgroup_path();
    }
    for (int i = 0; i < resource.cpu_cores_size(); ++i) {
        info.cpu_cores.push_back(resource.cpu_cores(i));
    }
    return info;
}

bool PrepareWorkspace(const ProgramOptions &opts, std::vector<UploadFileSpec> *files, UploadStats *stats) {
    LogClientInfo("validating local workspace");

    if (!CollectDirectoryFiles(opts.src_dir, files)) {
        return false;
    }

    grpc::Status validation = ValidateUploadFiles(*files);
    if (!validation.ok()) {
        LogClientError(validation.error_message());
        return false;
    }

    if (!ComputeUploadStats(*files, stats)) {
        PrintStageFailure(1, "Failed to compute workspace statistics.");
        return false;
    }

    LogClientInfo("workspace ready: " + std::to_string(stats->file_count) + " files, total " + FormatBytes(stats->total_bytes));
    if (opts.xsched.enabled) {
        LogClientInfo("xsched enabled: utilization=" + std::to_string(opts.xsched.xsched_utilization));
    }
    return true;
}

std::string FormatNodeTarget(const ResourceAllocation &node) {
    if (node.ip.empty()) {
        throw std::runtime_error("Controller returned an empty node IP.");
    }
    return node.ip + ":" + std::to_string(node.port);
}

int ExecuteTask(const std::string &target, const std::vector<UploadFileSpec> &files, const UploadStats &stats, const std::string &workspace_subdir,
                const std::string &command, bool enable_pty, const XSchedConfig *xsched_config) {
    // Resource allocation chooses the node, but execution still goes through
    // the same remote execution client used by remote_client.
    RemoteServiceClient client(grpc::CreateChannel(target, grpc::InsecureChannelCredentials()));
    RemoteServiceClient::TaskSubmitReport report;
    grpc::Status status = client.TaskSubmit(files, workspace_subdir, command, &report, &stats, enable_pty, xsched_config);
    if (!status.ok()) {
        PrintStageFailure(3, "RPC failed during upload/execution: " + status.error_message());
        return 1;
    }

    if (report.response.status() != remote_service::kSuccess) {
        PrintStageFailure(3, "Server reported failure: " + report.response.message());
        return 1;
    }

    LogClientInfo("execution success");
    LogClientInfo("server message: " + report.response.message());
    LogClientInfo("total time: " + FormatSeconds(report.elapsed_seconds) + " s");

    return 0;
}

int RunIntegratedClient(const ProgramOptions &opts) {
    std::vector<UploadFileSpec> files;
    UploadStats stats;
    if (!PrepareWorkspace(opts, &files, &stats)) {
        return 1;
    }

    LogClientInfo("applying resources from controller");

    std::string task_id;
    // The integrated client is a thin orchestration layer: reserve resources,
    // submit the task to the chosen node, then release the reservation.
    const ResourceAllocation allocation = ApplyResourceAndGetAllocation(opts.controller_addr, opts.resource_type, opts.resource_count, opts.mem_req, opts.core_req, opts.pid, &task_id);
    const std::string target = FormatNodeTarget(allocation);
    LogClientInfo("uploading workspace and executing on " + target);
    if (!allocation.cgroup_path.empty()) {
        LogClientInfo("allocated cgroup: " + allocation.cgroup_path);
    }
    if (!allocation.cpu_cores.empty()) {
        LogClientInfo("allocated cpu cores: " + FormatCpuCores(allocation.cpu_cores));
    }

    const int rc = ExecuteTask(target, files, stats, opts.workspace_subdir, opts.command, opts.pty, &opts.xsched);
    if (!ReleaseResource(opts.controller_addr, task_id)) {
        LogClientWarn("failed to release allocated resources for task " + task_id);
    }
    return rc;
}

} // namespace

int main(int argc, char **argv) {
    InitializeClientLogging("integrated_client");
    ProgramOptions opts;
    if (!ParseArguments(argc, argv, &opts)) {
        return 1;
    }

    try {
        return RunIntegratedClient(opts);
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << std::endl;
        return 1;
    }
}
