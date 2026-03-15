#include <absl/flags/flag.h>
#include <absl/flags/parse.h>

#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "client_api.h"
#include "client_logging.h"

ABSL_FLAG(std::string, target, "127.0.0.1:8063", "Server address.");
ABSL_FLAG(std::string, src_dir, "./task", "Local directory to upload to the server.");
ABSL_FLAG(std::string, workspace_subdir, "uploaded_task", "Subdirectory under the server workspace for the uploaded folder.");
ABSL_FLAG(std::string, command, "", "Command to execute inside the uploaded workspace directory.");
ABSL_FLAG(bool, pty, false, "Run the remote command under a PTY. Leave disabled for better reliability with batch commands.");

namespace {

struct ClientOptions {
    std::string target;
    std::string src_dir;
    std::string workspace_subdir;
    std::string command;
    bool enable_pty = false;
};

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

ClientOptions LoadClientOptions() {
    ClientOptions opts;
    opts.target = absl::GetFlag(FLAGS_target);
    opts.src_dir = absl::GetFlag(FLAGS_src_dir);
    opts.workspace_subdir = absl::GetFlag(FLAGS_workspace_subdir);
    opts.command = absl::GetFlag(FLAGS_command);
    opts.enable_pty = absl::GetFlag(FLAGS_pty);
    return opts;
}

bool ValidateClientOptions(const ClientOptions &opts) {
    if (opts.src_dir.empty()) {
        LogClientError("use --src_dir to select a directory to upload");
        return false;
    }
    if (opts.command.empty()) {
        LogClientError("use --command to specify how to execute the uploaded workspace");
        return false;
    }
    return true;
}

bool PrepareWorkspace(const ClientOptions &opts, std::vector<UploadFileSpec> *files, UploadStats *stats) {
    LogClientInfo("validating local workspace");

    if (!CollectDirectoryFiles(opts.src_dir, files)) {
        PrintStageFailure(1, "Failed to enumerate local directory: " + opts.src_dir);
        return false;
    }

    grpc::Status validation = ValidateUploadFiles(*files);
    if (!validation.ok()) {
        PrintStageFailure(1, "Validation failed: " + validation.error_message());
        return false;
    }

    if (!ComputeUploadStats(*files, stats)) {
        PrintStageFailure(1, "Failed to compute workspace statistics.");
        return false;
    }

    LogClientInfo("workspace ready: " + std::to_string(stats->file_count) + " files, total " + FormatBytes(stats->total_bytes));
    return true;
}

int RunClientWorkflow(const ClientOptions &opts, const std::vector<UploadFileSpec> &files, const UploadStats &stats) {
    RemoteServiceClient client(grpc::CreateChannel(opts.target, grpc::InsecureChannelCredentials()));
    RemoteServiceClient::TaskSubmitReport report;
    grpc::Status status = client.TaskSubmit(files, opts.workspace_subdir, opts.command, &report, &stats, opts.enable_pty);
    if (!status.ok()) {
        PrintStageFailure(2, "RPC failed during upload/execution: " + status.error_message());
        return 1;
    }

    if (report.response.status() != remote_service::kSuccess) {
        PrintStageFailure(3, "Server reported failure: " + report.response.message());
        return 1;
    }

    LogClientInfo("execution success");
    LogClientInfo("exit code: 0");
    LogClientInfo("total time: " + FormatSeconds(report.elapsed_seconds) + " s");

    return 0;
}

int RunClient(const ClientOptions &opts) {
    std::vector<UploadFileSpec> files;
    UploadStats stats;
    if (!PrepareWorkspace(opts, &files, &stats)) {
        return 1;
    }

    LogClientInfo("uploading workspace (" + std::to_string(stats.file_count) + " files, " + FormatBytes(stats.total_bytes) + ")");
    return RunClientWorkflow(opts, files, stats);
}

} // namespace

int main(int argc, char **argv) {
    absl::ParseCommandLine(argc, argv);
    InitializeClientLogging("remote_client");
    const ClientOptions opts = LoadClientOptions();
    if (!ValidateClientOptions(opts)) {
        return 1;
    }
    return RunClient(opts);
}
