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
ABSL_FLAG(int32_t, utilization, -1, "Initial xsched utilization for the task xqueue (0-100), or new value for --update_utilization.");
ABSL_FLAG(bool, update_utilization, false, "Update utilization of a running remote task identified by --workspace_subdir.");

namespace {

struct ClientOptions {
    std::string target;
    std::string src_dir;
    std::string workspace_subdir;
    std::string command;
    bool enable_pty = false;
    bool update_utilization = false;
    XSchedConfig xsched;
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
    opts.update_utilization = absl::GetFlag(FLAGS_update_utilization);
    opts.xsched.xsched_utilization = absl::GetFlag(FLAGS_utilization);
    opts.xsched.enabled = opts.xsched.xsched_utilization >= 0;
    return opts;
}

bool ValidateClientOptions(const ClientOptions &opts) {
    if (opts.workspace_subdir.empty()) {
        LogClientError("use --workspace_subdir to identify the remote task workspace");
        return false;
    }
    if (opts.update_utilization) {
        if (opts.xsched.xsched_utilization < 0 || opts.xsched.xsched_utilization > 100) {
            LogClientError("--update_utilization requires --utilization in [0, 100]");
            return false;
        }
        return true;
    }
    if (opts.src_dir.empty()) {
        LogClientError("use --src_dir to select a directory to upload");
        return false;
    }
    if (opts.command.empty()) {
        LogClientError("use --command to specify how to execute the uploaded workspace");
        return false;
    }
    if (opts.xsched.xsched_utilization < -1 || opts.xsched.xsched_utilization > 100) {
        LogClientError("--utilization must be in [0, 100]");
        return false;
    }
    return true;
}

bool PrepareWorkspace(const ClientOptions &opts, std::vector<UploadFileSpec> *files, UploadStats *stats) {
    // Collect and validate everything locally before opening the RPC so obvious
    // input errors fail fast on the client side.
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
    if (opts.xsched.enabled) {
        LogClientInfo("xsched enabled: utilization=" + std::to_string(opts.xsched.xsched_utilization));
    }
    return true;
}


int RunUpdateUtilization(const ClientOptions &opts) {
    RemoteServiceClient client(grpc::CreateChannel(opts.target, grpc::InsecureChannelCredentials()));
    remote_service::UpdateUtilizationResponse response;
    grpc::Status status = client.UpdateUtilization(opts.workspace_subdir, opts.xsched.xsched_utilization, &response);
    if (!status.ok()) {
        LogClientError("failed to update utilization: " + status.error_message());
        return 1;
    }
    if (!response.success()) {
        LogClientError("server rejected utilization update: " + response.message());
        return 1;
    }

    std::ostringstream oss;
    oss << "utilization updated: workspace=" << opts.workspace_subdir
        << " utilization=" << opts.xsched.xsched_utilization;
    if (response.pids_size() > 0) {
        oss << " pids=";
        for (int i = 0; i < response.pids_size(); ++i) {
            if (i != 0) {
                oss << ',';
            }
            oss << response.pids(i);
        }
    }
    LogClientInfo(oss.str());
    return 0;
}

int RunClientWorkflow(const ClientOptions &opts, const std::vector<UploadFileSpec> &files, const UploadStats &stats) {
    // The upload/execution details live in client_api.cc; this entry point only
    // prepares inputs and interprets the final task result.
    RemoteServiceClient client(grpc::CreateChannel(opts.target, grpc::InsecureChannelCredentials()));
    RemoteServiceClient::TaskSubmitReport report;
    grpc::Status status = client.TaskSubmit(files, opts.workspace_subdir, opts.command, &report, &stats, opts.enable_pty, &opts.xsched);
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
    if (opts.update_utilization) {
        return RunUpdateUtilization(opts);
    }

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
