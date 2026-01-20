
#include "remote_service_implement.h"

#include "task_executor.h"

#include <filesystem>
#include <fstream>
#include <mutex>
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
    grpc::Status download_status = DownloadWorkspace(context, stream, workspace_root, &config, &workspace_guard);
    if (!download_status.ok()) {
        if (download_status.error_code() == grpc::StatusCode::CANCELLED) {
            return download_status;
        }
        SendFailure(stream, download_status.error_message());
        return download_status;
    }

    TaskExecutor executor(context, config, workspace_root, stream);
    TaskResult result;
    grpc::Status exec_status = executor.Execute(&result);
    if (!exec_status.ok()) {
        if (exec_status.error_code() == grpc::StatusCode::CANCELLED) {
            return exec_status;
        }
        SendFailure(stream, exec_status.error_message());
        return exec_status;
    }

    SendResult(stream, result);
    return grpc::Status::OK;
}

grpc::Status RemoteServiceImplement::DownloadWorkspace(grpc::ServerContext *context,
                                                       grpc::ServerReaderWriter<TaskResponse, TaskRequest> *stream,
                                                       const fs::path &workspace_root, TaskConfig *config_out, WorkspaceLockGuard *lock_guard) {
    TaskRequest incoming;
    std::ofstream outfile;
    fs::path current_file_path;
    bool received_config = false;
    TaskConfig config_buffer;
    fs::path workspace_subdir_path;

    auto set_failure = [&](const grpc::Status &status) { return status; };

    while (stream->Read(&incoming)) {
        if (context != nullptr && context->IsCancelled()) {
            return set_failure(grpc::Status(grpc::StatusCode::CANCELLED, "Client cancelled request."));
        }
        if (incoming.has_config()) {
            if (received_config) {
                return set_failure(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Duplicate TaskConfig payload!"));
            }
            config_buffer = incoming.config();
            const std::string workspace_subdir = DefaultIfEmpty(config_buffer.workspace_subdir(), "uploaded_task");
            const std::string default_output = (fs::path(workspace_subdir) / "output").generic_string();
            const std::string output_subdir = DefaultIfEmpty(config_buffer.output_subdir(), default_output);

            workspace_subdir_path = LexicallyNormalized(workspace_root, workspace_subdir);
            fs::path output_subdir_path = LexicallyNormalized(workspace_root, output_subdir);

            if (!IsSubPath(workspace_root, workspace_subdir_path) || !IsSubPath(workspace_root, output_subdir_path)) {
                return set_failure(grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "Workspace or output directory escapes server workspace!"));
            }

            config_buffer.set_workspace_subdir(fs::relative(workspace_subdir_path, workspace_root).generic_string());
            config_buffer.set_output_subdir(fs::relative(output_subdir_path, workspace_root).generic_string());
            if (lock_guard != nullptr) {
                grpc::Status lock_status = lock_guard->Acquire(config_buffer.workspace_subdir());
                if (!lock_status.ok()) {
                    return set_failure(lock_status);
                }
                lock_guard->RegisterCleanupTargets(config_buffer.workspace_subdir(), config_buffer.output_subdir());
            }
            received_config = true;
            continue;
        }

        if (!incoming.has_file_chunk()) {
            return set_failure(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "TaskRequest payload is missing!"));
        }

        if (!received_config) {
            return set_failure(grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "TaskConfig must be sent before file chunks!"));
        }

        const auto &chunk = incoming.file_chunk();
        if (chunk.relative_path().empty()) {
            return set_failure(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "File chunk missing relative path!"));
        }

        fs::path target_path = LexicallyNormalized(workspace_subdir_path, chunk.relative_path());
        if (!IsSubPath(workspace_subdir_path, target_path)) {
            return set_failure(grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "File path escapes uploaded workspace!"));
        }

        const bool need_new_file = !outfile.is_open() || chunk.file_start() || target_path != current_file_path;
        if (need_new_file) {
            if (outfile.is_open()) {
                outfile.close();
            }
            std::error_code dir_ec;
            fs::create_directories(target_path.parent_path(), dir_ec);
            if (dir_ec) {
                return set_failure(grpc::Status(grpc::StatusCode::INTERNAL, "Failed to prepare workspace directory!"));
            }
            outfile.open(target_path, std::ios::binary | std::ios::trunc);
            if (!outfile.is_open()) {
                return set_failure(grpc::Status(grpc::StatusCode::INTERNAL, "Failed to open file for writing!"));
            }
            current_file_path = target_path;
        }

        if (!chunk.data().empty()) {
            outfile.write(chunk.data().data(), chunk.data().size());
            if (!outfile.good()) {
                return set_failure(grpc::Status(grpc::StatusCode::INTERNAL, "Failed to write file chunk!"));
            }
        }

        if (chunk.file_end() && outfile.is_open()) {
            outfile.close();
            current_file_path.clear();
        }
    }

    if (context != nullptr && context->IsCancelled()) {
        return set_failure(grpc::Status(grpc::StatusCode::CANCELLED, "Client cancelled request."));
    }

    if (!received_config) {
        return set_failure(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "TaskConfig payload is missing!"));
    }

    if (outfile.is_open()) {
        outfile.close();
    }

    config_out->CopyFrom(config_buffer);
    return grpc::Status::OK;
}
