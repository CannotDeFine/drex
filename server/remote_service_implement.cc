#include "remote_service_implement.h"

#include "server_logging.h"
#include "task_executor.h"
#include "task_runtime_control.h"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <system_error>
#include <unordered_set>

using remote_service::TaskConfig;
using remote_service::TaskRequest;
using remote_service::TaskResponse;
using remote_service::TaskResult;

namespace fs = std::filesystem;

namespace {

const fs::path kServerSourceDir = fs::path(__FILE__).parent_path();
const fs::path kProjectRoot = kServerSourceDir.parent_path();
const fs::path kWorkspaceRoot = kProjectRoot / "workspace";
std::mutex g_workspace_lock_mu;
std::unordered_set<std::string> g_locked_subdirs;

bool IsSubPath(const fs::path &base, const fs::path &candidate) {
    auto base_it = base.begin();
    auto candidate_it = candidate.begin();
    for (; base_it != base.end(); ++base_it, ++candidate_it) {
        if (candidate_it == candidate.end() || *base_it != *candidate_it) {
            return false;
        }
    }
    return true;
}

fs::path LexicallyNormalized(const fs::path &base, const fs::path &suffix) {
    return (base / suffix).lexically_normal();
}

std::string DefaultIfEmpty(const std::string &value, const std::string &fallback) {
    return value.empty() ? fallback : value;
}

void SendResult(grpc::ServerReaderWriter<TaskResponse, TaskRequest> *stream, const TaskResult &result) {
    TaskResponse response;
    *response.mutable_result() = result;
    stream->Write(response);
}

void SendFailure(grpc::ServerReaderWriter<TaskResponse, TaskRequest> *stream, const std::string &message) {
    TaskResult fail_result;
    fail_result.set_status(remote_service::kFailed);
    fail_result.set_message(message);
    fail_result.clear_output_archive();
    fail_result.set_archive_size(0);
    SendResult(stream, fail_result);
}

struct ResolvedTaskPaths {
    std::string workspace_subdir;
    std::string output_subdir;
    fs::path workspace_path;
    fs::path output_path;
};

grpc::Status ResolveTaskPaths(const fs::path &workspace_root, const TaskConfig &config, ResolvedTaskPaths *paths) {
    if (paths == nullptr) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "ResolvedTaskPaths pointer must not be null.");
    }

    const std::string workspace_subdir = DefaultIfEmpty(config.workspace_subdir(), "uploaded_task");
    const std::string default_output = (fs::path(workspace_subdir) / "output").generic_string();
    const std::string output_subdir = DefaultIfEmpty(config.output_subdir(), default_output);

    const fs::path workspace_path = LexicallyNormalized(workspace_root, workspace_subdir);
    const fs::path output_path = LexicallyNormalized(workspace_root, output_subdir);
    // Reject any path that escapes the dedicated server workspace, even if the
    // client sends "../" components.
    if (!IsSubPath(workspace_root, workspace_path) || !IsSubPath(workspace_root, output_path)) {
        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "Workspace or output directory escapes server workspace!");
    }

    paths->workspace_subdir = fs::relative(workspace_path, workspace_root).generic_string();
    paths->output_subdir = fs::relative(output_path, workspace_root).generic_string();
    paths->workspace_path = workspace_path;
    paths->output_path = output_path;
    return grpc::Status::OK;
}

} // namespace

class WorkspaceLockGuard {
  public:
    explicit WorkspaceLockGuard(const fs::path &workspace_root);
    ~WorkspaceLockGuard();

    grpc::Status Acquire(const std::string &subdir_key);
    void RegisterCleanupTargets(const std::string &workspace_subdir, const std::string &output_subdir);
    void Release();

  private:
    void Cleanup();

    fs::path workspace_root_;
    std::string key_;
    std::string workspace_subdir_;
    std::string output_subdir_;
    bool locked_ = false;
    bool cleanup_registered_ = false;
};

WorkspaceLockGuard::WorkspaceLockGuard(const fs::path &workspace_root) : workspace_root_(workspace_root) {}

WorkspaceLockGuard::~WorkspaceLockGuard() {
    Cleanup();
    Release();
}

grpc::Status WorkspaceLockGuard::Acquire(const std::string &subdir_key) {
    if (locked_) {
        if (subdir_key == key_) {
            return grpc::Status::OK;
        }
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "Workspace lock already acquired.");
    }
    if (subdir_key.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "workspace_subdir must not be empty.");
    }

    std::lock_guard<std::mutex> guard(g_workspace_lock_mu);
    if (g_locked_subdirs.find(subdir_key) != g_locked_subdirs.end()) {
        return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "workspace_subdir is currently occupied, choose a unique subdirectory.");
    }
    g_locked_subdirs.insert(subdir_key);
    key_ = subdir_key;
    locked_ = true;
    return grpc::Status::OK;
}

void WorkspaceLockGuard::RegisterCleanupTargets(const std::string &workspace_subdir, const std::string &output_subdir) {
    workspace_subdir_ = workspace_subdir;
    output_subdir_ = output_subdir;
    cleanup_registered_ = true;
}

void WorkspaceLockGuard::Release() {
    if (!locked_) {
        return;
    }
    std::lock_guard<std::mutex> guard(g_workspace_lock_mu);
    g_locked_subdirs.erase(key_);
    key_.clear();
    locked_ = false;
}

void WorkspaceLockGuard::Cleanup() {
    if (!cleanup_registered_) {
        return;
    }
    // Each submission owns a private subtree. Cleanup on every exit path keeps
    // later runs from inheriting stale files.
    auto remove_path = [&](const std::string &relative) {
        if (relative.empty()) {
            return;
        }
        std::error_code ec;
        fs::remove_all((workspace_root_ / relative).lexically_normal(), ec);
    };
    remove_path(workspace_subdir_);
    if (output_subdir_ != workspace_subdir_) {
        remove_path(output_subdir_);
    }
    cleanup_registered_ = false;
}

class WorkspaceDownloadSession {
  public:
    WorkspaceDownloadSession(grpc::ServerContext *context, grpc::ServerReaderWriter<TaskResponse, TaskRequest> *stream, const fs::path &workspace_root,
                             WorkspaceLockGuard *lock_guard)
        : context_(context), stream_(stream), workspace_root_(workspace_root), lock_guard_(lock_guard) {}

    grpc::Status Run(TaskConfig *config_out) {
        if (config_out == nullptr) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "TaskConfig pointer must not be null.");
        }

        TaskRequest incoming;
        while (stream_->Read(&incoming)) {
            if (context_ != nullptr && context_->IsCancelled()) {
                return grpc::Status(grpc::StatusCode::CANCELLED, "Client cancelled request.");
            }
            if (incoming.has_config()) {
                grpc::Status status = HandleConfig(incoming.config());
                if (!status.ok()) {
                    return status;
                }
                continue;
            }
            if (!incoming.has_file_chunk()) {
                return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "TaskRequest payload is missing!");
            }
            // After the initial config, the rest of the stream is treated as an
            // ordered sequence of file chunks for the uploaded workspace.
            grpc::Status status = HandleFileChunk(incoming.file_chunk());
            if (!status.ok()) {
                return status;
            }
        }

        if (context_ != nullptr && context_->IsCancelled()) {
            return grpc::Status(grpc::StatusCode::CANCELLED, "Client cancelled request.");
        }
        if (!received_config_) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "TaskConfig payload is missing!");
        }
        CloseCurrentFile();
        config_out->CopyFrom(config_);
        return grpc::Status::OK;
    }

  private:
    grpc::Status HandleConfig(const TaskConfig &config) {
        if (received_config_) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Duplicate TaskConfig payload!");
        }

        // Resolve and lock the workspace once, then reuse the normalized paths
        // for every subsequent file chunk.
        config_ = config;
        grpc::Status path_status = ResolveTaskPaths(workspace_root_, config_, &paths_);
        if (!path_status.ok()) {
            return path_status;
        }

        config_.set_workspace_subdir(paths_.workspace_subdir);
        config_.set_output_subdir(paths_.output_subdir);
        if (lock_guard_ != nullptr) {
            grpc::Status lock_status = lock_guard_->Acquire(config_.workspace_subdir());
            if (!lock_status.ok()) {
                return lock_status;
            }
            lock_guard_->RegisterCleanupTargets(config_.workspace_subdir(), config_.output_subdir());
        }

        received_config_ = true;
        return grpc::Status::OK;
    }

    grpc::Status HandleFileChunk(const remote_service::TaskFileChunk &chunk) {
        if (!received_config_) {
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "TaskConfig must be sent before file chunks!");
        }
        if (chunk.relative_path().empty()) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "File chunk missing relative path!");
        }

        const fs::path target_path = LexicallyNormalized(paths_.workspace_path, chunk.relative_path());
        if (!IsSubPath(paths_.workspace_path, target_path)) {
            return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "File path escapes uploaded workspace!");
        }

        grpc::Status open_status = EnsureOutputFile(target_path, chunk.file_start());
        if (!open_status.ok()) {
            return open_status;
        }

        if (!chunk.data().empty()) {
            outfile_.write(chunk.data().data(), chunk.data().size());
            if (!outfile_.good()) {
                return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to write file chunk!");
            }
        }

        if (chunk.file_end()) {
            CloseCurrentFile();
        }
        return grpc::Status::OK;
    }

    grpc::Status EnsureOutputFile(const fs::path &target_path, bool file_start) {
        const bool need_new_file = !outfile_.is_open() || file_start || target_path != current_file_path_;
        if (!need_new_file) {
            return grpc::Status::OK;
        }

        // The stream may switch between files at arbitrary chunk boundaries, so
        // reopen lazily whenever the target path changes.
        CloseCurrentFile();
        std::error_code dir_ec;
        fs::create_directories(target_path.parent_path(), dir_ec);
        if (dir_ec) {
            return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to prepare workspace directory!");
        }

        outfile_.open(target_path, std::ios::binary | std::ios::trunc);
        if (!outfile_.is_open()) {
            return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to open file for writing!");
        }
        current_file_path_ = target_path;
        return grpc::Status::OK;
    }

    void CloseCurrentFile() {
        if (outfile_.is_open()) {
            outfile_.close();
        }
        current_file_path_.clear();
    }

    grpc::ServerContext *context_ = nullptr;
    grpc::ServerReaderWriter<TaskResponse, TaskRequest> *stream_ = nullptr;
    fs::path workspace_root_;
    WorkspaceLockGuard *lock_guard_ = nullptr;
    TaskConfig config_;
    ResolvedTaskPaths paths_;
    std::ofstream outfile_;
    fs::path current_file_path_;
    bool received_config_ = false;
};

grpc::Status RemoteServiceImplement::TaskSubmission(grpc::ServerContext *context,
                                                    grpc::ServerReaderWriter<TaskResponse, TaskRequest> *stream) {
    std::error_code ec;
    fs::path workspace_root = fs::weakly_canonical(kWorkspaceRoot, ec);
    if (ec) {
        SendFailure(stream, "Failed to determine server workspace!");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to determine server workspace!");
    }

    WorkspaceLockGuard workspace_guard(workspace_root);
    TaskConfig config;
    LogServerInfo("accepted task submission");
    LogServerDebug("task peer: " + std::string(context != nullptr ? context->peer() : "unknown-peer"));
    grpc::Status download_status = DownloadWorkspace(context, stream, workspace_root, &config, &workspace_guard);
    if (!download_status.ok()) {
        if (download_status.error_code() == grpc::StatusCode::CANCELLED) {
            LogServerWarn("task submission cancelled during upload");
            return download_status;
        }
        LogServerError("failed to download workspace: " + download_status.error_message());
        SendFailure(stream, download_status.error_message());
        return download_status;
    }

    {
        std::ostringstream oss;
        oss << "workspace ready: subdir='" << config.workspace_subdir() << "' output='" << config.output_subdir() << "'";
        LogServerDebug(oss.str());
    }

    TaskExecutor executor(context, config, workspace_root, stream);
    TaskResult result;
    grpc::Status exec_status = executor.Execute(&result);
    if (!exec_status.ok()) {
        if (exec_status.error_code() == grpc::StatusCode::CANCELLED) {
            LogServerWarn("task execution cancelled: workspace='" + config.workspace_subdir() + "'");
            return exec_status;
        }
        LogServerError("task execution failed: workspace='" + config.workspace_subdir() + "' error='" + exec_status.error_message() + "'");
        SendFailure(stream, exec_status.error_message());
        return exec_status;
    }

    LogServerInfo("task execution completed successfully: workspace='" + config.workspace_subdir() + "'");
    SendResult(stream, result);
    return grpc::Status::OK;
}

grpc::Status RemoteServiceImplement::DownloadWorkspace(grpc::ServerContext *context,
                                                       grpc::ServerReaderWriter<TaskResponse, TaskRequest> *stream,
                                                       const fs::path &workspace_root, TaskConfig *config_out, WorkspaceLockGuard *lock_guard) {
    WorkspaceDownloadSession session(context, stream, workspace_root, lock_guard);
    return session.Run(config_out);
}

grpc::Status RemoteServiceImplement::UpdateUtilization(grpc::ServerContext *context, const remote_service::UpdateUtilizationRequest *request,
                                                       remote_service::UpdateUtilizationResponse *response) {
    (void)context;
    if (request == nullptr || response == nullptr) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "UpdateUtilization request/response must not be null.");
    }

    const UtilizationUpdateResult result = UpdateTaskProcessUtilization(request->workspace_subdir(), request->utilization());
    response->set_success(result.success);
    response->set_message(result.message);
    for (pid_t pid : result.pids) {
        response->add_pids(static_cast<int>(pid));
    }

    if (!result.success) {
        LogServerWarn("update utilization failed: workspace='" + request->workspace_subdir() + "' error='" + result.message + "'");
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, result.message);
    }

    LogServerInfo("updated utilization: workspace='" + request->workspace_subdir() + "' utilization=" + std::to_string(request->utilization()));
    return grpc::Status::OK;
}
