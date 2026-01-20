#include "client_api.h"
#include "resource_control.pb.h"

#include <brpc/channel.h>
#include <butil/logging.h>
#include <grpcpp/grpcpp.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct NodeInfo {
    std::string ip;
    int port = 0;
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
    bool pty = false;
};

void PrintUsage(const char *prog_name) {
    std::cerr << "Usage: " << prog_name
              << " <controller_addr(host:port)> <src_dir> <workspace_subdir> <command> <local_output_dir> <resource_type> <resource_count>"
                 " [--mem=<mem_req>] [--cores=<core_req>] [--pty]"
              << "\n   or: " << prog_name
              << " --controller=<host:port> --src_dir=<dir> --workspace_subdir=<name> --command=<cmd> --type=<resource_type>"
                 " [--count=<n>] [--local_output_dir=<dir>] [--mem=<mem_req>] [--cores=<core_req>] [--pty]"
              << std::endl;
}

// 解析并校验命令行参数
bool ParseArguments(int argc, char **argv, ProgramOptions *opts) {
    if (argc < 2) {
        PrintUsage(argv[0]);
        return false;
    }

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
            if (arg.rfind(cores_prefix, 0) == 0) {
                try {
                    opts->core_req = std::stoi(arg.substr(cores_prefix.size()));
                } catch (const std::exception &ex) {
                    std::cerr << "Invalid --cores value: " << ex.what() << std::endl;
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

    return true;
}

void PrintStageFailure(int stage, const std::string &message) {
    std::cerr << "[Stage " << stage << "] " << message << std::endl;
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

std::string ShellEscape(const std::string &input) {
    std::string escaped = "'";
    for (char c : input) {
        if (c == '\'') {
            escaped += "'\\''";
        } else {
            escaped.push_back(c);
        }
    }
    escaped.push_back('\'');
    return escaped;
}

bool ExtractArchiveTo(const std::string &archive_data, const fs::path &destination) {
    std::error_code ec;
    fs::create_directories(destination, ec);
    if (ec) {
        return false;
    }
    ec.clear();
    fs::path canonical_dest = fs::weakly_canonical(destination, ec);
    if (ec) {
        canonical_dest = destination;
    }
    const std::string command = "tar -xzf - -C " + ShellEscape(canonical_dest.string());
    FILE *pipe = ::popen(command.c_str(), "w");
    if (pipe == nullptr) {
        return false;
    }
    const size_t written = std::fwrite(archive_data.data(), 1, archive_data.size(), pipe);
    const int rc = ::pclose(pipe);
    return written == archive_data.size() && rc == 0;
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

NodeInfo ApplyResourceAndGetNode(const std::string &controller_addr, const std::string &resource_type, int resource_count, int mem_req, int core_req,
                                 std::string *task_id_out) {
    brpc::Channel channel;
    brpc::ChannelOptions options;
    options.protocol = "baidu_std";
    options.timeout_ms = 5000;
    options.max_retry = 3;

    if (channel.Init(controller_addr.c_str(), "", &options) != 0) {
        throw std::runtime_error("Failed to initialize brpc channel.");
    }

    hcp::ResourceControlService_Stub stub(&channel);

    // step 1: 申请资源
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
    stub.apply_resource(&apply_cntl, &apply_request, &apply_response, nullptr);

    if (apply_cntl.Failed() || apply_response.status().errcode() != 0) {
        throw std::runtime_error(std::string("Apply resource failed: ") + (apply_cntl.Failed() ? apply_cntl.ErrorText() : apply_response.status().errmsg()));
    }

    const std::string task_id = apply_response.task_id();
    if (task_id_out != nullptr) {
        *task_id_out = task_id;
    }

    // step 2: 查询资源分配结果
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

    const hcp::NodeId &node = query_response.rinfos(0).node();
    NodeInfo info;
    info.ip = node.ip();
    info.port = node.port();
    return info;
}

// 将任务打包上传到执行节点，并获取执行结果
int ExecuteTask(const std::string &target, const std::vector<UploadFileSpec> &files, const UploadStats &stats, const std::string &workspace_subdir,
                const std::string &command, const std::string &local_output_dir, bool enable_pty) {
    RemoteServiceClient client(grpc::CreateChannel(target, grpc::InsecureChannelCredentials()));
    RemoteServiceClient::TaskSubmitReport report;
    grpc::Status status = client.TaskSubmit(files, workspace_subdir, command, &report, &stats, enable_pty);
    if (!status.ok()) {
        PrintStageFailure(3, "RPC failed during upload/execution: " + status.error_message());
        return 1;
    }

    if (report.response.status() != remote_service::kSuccess) {
        PrintStageFailure(3, "Server reported failure: " + report.response.message());
        return 1;
    }

    const std::string &archive = report.response.output_archive();
    if (archive.empty()) {
        std::cout << "\nExecution success." << std::endl;
        std::cout << "Server message: " << report.response.message() << std::endl;
        std::cout << "Total time: " << FormatSeconds(report.elapsed_seconds) << " s" << std::endl;
        std::cout << "Note: server did not return an output archive." << std::endl;
        return 0;
    }

    std::error_code ec;
    fs::create_directories(local_output_dir, ec);
    if (ec) {
        PrintStageFailure(4, "Failed to create local output directory: " + local_output_dir);
        return 1;
    }

    fs::path archive_path = fs::path(local_output_dir) / "server_output.tar.gz";
    std::ofstream outfile(archive_path, std::ios::binary | std::ios::trunc);
    if (!outfile.is_open()) {
        PrintStageFailure(4, "Failed to save server outputs to " + archive_path.string());
        return 1;
    }
    outfile.write(archive.data(), static_cast<std::streamsize>(archive.size()));
    if (!outfile.good()) {
        PrintStageFailure(4, "Failed to write server output archive.");
        return 1;
    }

    std::cout << "[4/4] Restoring outputs to: " << local_output_dir << std::endl;
    if (!ExtractArchiveTo(archive, fs::path(local_output_dir))) {
        PrintStageFailure(4, "Failed to extract outputs to " + local_output_dir + " (archive is still saved as " + archive_path.string() + ")");
        return 1;
    }

    std::cout << "\nExecution success." << std::endl;
    std::cout << "Server message: " << report.response.message() << std::endl;
    std::cout << "Saved server outputs archive to: " << archive_path << std::endl;
    std::cout << "Total time: " << FormatSeconds(report.elapsed_seconds) << " s" << std::endl;

    return 0;
}

} // namespace

int main(int argc, char **argv) {
    ProgramOptions opts;
    if (!ParseArguments(argc, argv, &opts)) {
        return 1;
    }

    std::cout << "[1/4] Validating local workspace..." << std::endl;

    std::vector<UploadFileSpec> files;
    if (!CollectDirectoryFiles(opts.src_dir, &files)) {
        return 1;
    }

    grpc::Status validation = ValidateUploadFiles(files);
    if (!validation.ok()) {
        std::cerr << validation.error_message() << std::endl;
        return 1;
    }

    UploadStats stats;
    if (!ComputeUploadStats(files, &stats)) {
        PrintStageFailure(1, "Failed to compute workspace statistics.");
        return 1;
    }

    std::cout << "    -> Found " << stats.file_count << " files, total " << FormatBytes(stats.total_bytes) << std::endl;
    std::cout << "[2/4] Applying resources from controller..." << std::endl;

    try {
        std::string task_id;
        const NodeInfo node = ApplyResourceAndGetNode(opts.controller_addr, opts.resource_type, opts.resource_count, opts.mem_req, opts.core_req, &task_id);
        const std::string target = std::string("10.26.42.234") + ":" + std::to_string(static_cast<int>(node.port));
        std::cout << "[3/4] Uploading workspace and executing on: " << target << std::endl;
        const int rc = ExecuteTask(target, files, stats, opts.workspace_subdir, opts.command, opts.local_output_dir, opts.pty);
        (void)ReleaseResource(opts.controller_addr, task_id);
        return rc;
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << std::endl;
        return 1;
    }

    return 0;
}
