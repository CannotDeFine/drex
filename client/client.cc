#include <absl/flags/flag.h>
#include <absl/flags/parse.h>

#include <grpcpp/grpcpp.h>

#include <iostream>
#include <string>
#include <vector>

#include "client_api.h"

using remote_service::ReqDeviceType;

ABSL_FLAG(std::string, target, "127.0.0.1:8063", "Server address.");
ABSL_FLAG(std::string, file, "/home/cdf/rpc-work/application/cpu/app", "Local executable to submit.");
ABSL_FLAG(std::string, save_path, "./server_task.bin", "Destination path for the primary executable on server.");
ABSL_FLAG(std::string, extra_files, "", "Comma separated list of extra files: type:local[:remote].");
ABSL_FLAG(std::string, extra_dirs, "", "Comma separated list of directories: type:local_dir[:remote_dir].");
ABSL_FLAG(int, device, 0, "Device type (0=CPU, 1=DPU, 2=FPGA, 3=GPU, 4=NPU, 5=OFA).");
ABSL_FLAG(std::string, server_result_path, "", "Path inside the server workspace whose contents should be returned after execution.");
ABSL_FLAG(std::string, result_path, "", "[Deprecated] Alias for --server_result_path.");

namespace {

constexpr int kExecCount = 1;

int RunClientWorkflow(const std::vector<UploadFileSpec> &files, ReqDeviceType device, const std::string &target_str,
                      const std::string &server_result_path, int exec_count) {
    const std::string entry_path = files.front().remote_path.empty() ? "./server_task.bin" : files.front().remote_path;

    RemoteServiceClient client(grpc::CreateChannel(target_str, grpc::InsecureChannelCredentials()));

    for (int i = 0; i < exec_count; ++i) {
        RemoteServiceClient::TaskSubmitReport report;
        grpc::Status status = client.TaskSubmit(files, device, entry_path, server_result_path, &report);
        if (!status.ok()) {
            std::cerr << "RPC failed: " << status.error_message() << std::endl;
            return 1;
        }

        if (report.response.status() != remote_service::kSuccess) {
            std::cerr << "Server execution failed: " << report.response.message() << std::endl;
            return 1;
        }

        std::cout << "Upload completed" << std::endl;
        std::cout << "Client sent: " << report.bytes_sent << " bytes" << std::endl;
        std::cout << "Server received: " << report.response.length() << " bytes" << std::endl;
        std::cout << "Server message: " << report.response.message() << std::endl;
        if (!report.response.result().empty()) {
            std::cout << "Server output (" << report.response.result().size() << " bytes):" << std::endl;
            std::cout << report.response.result() << std::endl;
        }
        std::cout << "Elapsed: " << report.elapsed_seconds << " seconds" << std::endl;
        std::cout << report.elapsed_seconds << std::endl;
    }

    return 0;
}

} // namespace

// Entry point that parses CLI flags and runs the upload workflow.
int main(int argc, char **argv) {
    absl::ParseCommandLine(argc, argv);
    const std::string target_str = absl::GetFlag(FLAGS_target);
    const std::string file_path = absl::GetFlag(FLAGS_file);
    const std::string save_path = absl::GetFlag(FLAGS_save_path);
    const std::string extra_files_flag = absl::GetFlag(FLAGS_extra_files);
    const std::string extra_dirs_flag = absl::GetFlag(FLAGS_extra_dirs);
    const int device = absl::GetFlag(FLAGS_device);

    std::string server_result_path = absl::GetFlag(FLAGS_server_result_path);
    const std::string deprecated_result_path = absl::GetFlag(FLAGS_result_path);
    if (server_result_path.empty() && !deprecated_result_path.empty()) {
        std::cerr << "[WARN] --result_path is deprecated, please use --server_result_path instead." << std::endl;
        server_result_path = deprecated_result_path;
    }

    if (file_path.empty()) {
        std::cerr << "Use --file to select executable file." << std::endl;
        return 1;
    }

    std::vector<UploadFileSpec> files;
    files.push_back({file_path, save_path, remote_service::kFileExecutable});

    bool extras_ok = true;
    extras_ok &= CollectExtraDirectories(extra_dirs_flag, &files);
    extras_ok &= CollectExtraFiles(extra_files_flag, &files);
    if (!extras_ok) {
        return 1;
    }

    grpc::Status validation = ValidateUploadFiles(files);
    if (!validation.ok()) {
        std::cerr << validation.error_message() << std::endl;
        return 1;
    }

    return RunClientWorkflow(files, static_cast<ReqDeviceType>(device), target_str, server_result_path, kExecCount);
}
