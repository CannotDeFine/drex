#include <absl/flags/flag.h>
#include <absl/flags/parse.h>

#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "client_api.h"

ABSL_FLAG(std::string, target, "127.0.0.1:8063", "Server address.");
ABSL_FLAG(std::string, src_dir, "./task", "Local directory to upload to the server.");
ABSL_FLAG(std::string, workspace_subdir, "uploaded_task", "Subdirectory under the server workspace for the uploaded folder.");
ABSL_FLAG(std::string, command, "", "Command to execute inside the uploaded workspace directory.");
ABSL_FLAG(bool, pty, false, "Run the remote command under a PTY (improves interactivity for buffered stdout). May slow very chatty output.");

namespace fs = std::filesystem;

namespace {

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

int RunClientWorkflow(const std::vector<UploadFileSpec> &files, const UploadStats &stats, const std::string &target_str,
                      const std::string &workspace_subdir, const std::string &command, const std::string &restore_dir, bool enable_pty) {
    RemoteServiceClient client(grpc::CreateChannel(target_str, grpc::InsecureChannelCredentials()));
    RemoteServiceClient::TaskSubmitReport report;
    grpc::Status status = client.TaskSubmit(files, workspace_subdir, command, &report, &stats, enable_pty);
    if (!status.ok()) {
        PrintStageFailure(2, "RPC failed during upload/execution: " + status.error_message());
        return 1;
    }

    if (report.response.status() != remote_service::kSuccess) {
        PrintStageFailure(3, "Server reported failure: " + report.response.message());
        return 1;
    }

    fs::path restore_path = restore_dir.empty() ? fs::path(".") : fs::path(restore_dir);
    std::cout << "[4/4] Restoring outputs to: " << restore_path << std::endl;
    const std::string &archive = report.response.output_archive();
    if (archive.empty()) {
        std::cout << "\nExecution success." << std::endl;
        std::cout << "Exit code: 0" << std::endl;
        std::cout << "Total time: " << FormatSeconds(report.elapsed_seconds) << " s" << std::endl;
        std::cout << "Note: server did not return an output archive." << std::endl;
        return 0;
    }
    if (!ExtractArchiveTo(archive, restore_path)) {
        PrintStageFailure(4, "Failed to extract outputs to " + restore_path.string());
        return 1;
    }

    std::cout << "\nExecution success." << std::endl;
    std::cout << "Exit code: 0" << std::endl;
    std::cout << "Total time: " << FormatSeconds(report.elapsed_seconds) << " s" << std::endl;

    return 0;
}

} // namespace

int main(int argc, char **argv) {
    absl::ParseCommandLine(argc, argv);
    const std::string target_str = absl::GetFlag(FLAGS_target);
    const std::string src_dir = absl::GetFlag(FLAGS_src_dir);
    const std::string workspace_subdir = absl::GetFlag(FLAGS_workspace_subdir);
    const std::string command = absl::GetFlag(FLAGS_command);
    const bool enable_pty = absl::GetFlag(FLAGS_pty);

    if (src_dir.empty()) {
        std::cerr << "Use --src_dir to select a directory to upload." << std::endl;
        return 1;
    }
    if (command.empty()) {
        std::cerr << "Use --command to specify how to execute the uploaded workspace." << std::endl;
        return 1;
    }

    std::cout << "[1/4] Validating local workspace..." << std::endl;

    std::vector<UploadFileSpec> files;
    if (!CollectDirectoryFiles(src_dir, &files)) {
        PrintStageFailure(1, "Failed to enumerate local directory: " + src_dir);
        return 1;
    }

    grpc::Status validation = ValidateUploadFiles(files);
    if (!validation.ok()) {
        PrintStageFailure(1, "Validation failed: " + validation.error_message());
        return 1;
    }

    UploadStats stats;
    if (!ComputeUploadStats(files, &stats)) {
        PrintStageFailure(1, "Failed to compute workspace statistics.");
        return 1;
    }

    std::cout << "    -> Found " << stats.file_count << " files, total " << FormatBytes(stats.total_bytes) << std::endl;
    std::cout << "[2/4] Uploading workspace (" << stats.file_count << " files, " << FormatBytes(stats.total_bytes) << ")..." << std::endl;

    return RunClientWorkflow(files, stats, target_str, workspace_subdir, command, src_dir, enable_pty);
}
