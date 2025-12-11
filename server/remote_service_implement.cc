#include "remote_service_implement.h"
#include "task_factory.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

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

grpc::Status ResolveTaskFilePath(const std::string &requested_path, const fs::path &canonical_workspace_root, fs::path *resolved_path) {
    fs::path candidate_path;
    if (requested_path.empty()) {
        candidate_path = canonical_workspace_root / "server_task.bin";
    } else {
        fs::path requested(requested_path);
        candidate_path = requested.is_absolute() ? requested : canonical_workspace_root / requested;
    }

    std::error_code ec;
    fs::path canonical_target = fs::weakly_canonical(candidate_path, ec);
    if (ec) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Failed to resolve task file path!");
    }

    if (!IsSubPath(canonical_workspace_root, canonical_target)) {
        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "Task file path must stay within the server workspace!");
    }

    *resolved_path = canonical_target;
    return grpc::Status::OK;
}

} // namespace

grpc::Status RemoteServiceImplement::TaskSubmission(grpc::ServerContext *context, ServerReader<TaskRequest> *request, TaskResult *result) {
    TaskConfig task_config;

    grpc::Status download_status = DownloadFile(request, result, &task_config);
    if (!download_status.ok()) {
        result->set_status(remote_service::kFailed);
        result->set_message(download_status.error_message());
        result->clear_result();
        result->set_length(0);
        return download_status;
    }

    auto task = TaskFactory::Create(task_config);
    if (!task) {
        result->set_status(remote_service::kFailed);
        result->set_message("Unknown device type!");
        result->clear_result();
        result->set_length(0);
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Unknown device type!");
    }

    grpc::Status exec_status = task->Execute(result);
    if (!exec_status.ok()) {
        return exec_status;
    }

    return grpc::Status::OK;
}

grpc::Status RemoteServiceImplement::DownloadFile(ServerReader<TaskRequest> *request, TaskResult *result, TaskConfig *config_out) {
    TaskRequest task_request;
    std::ofstream outfile;
    fs::path current_file_path;
    std::error_code workspace_ec;
    fs::path workspace_root = fs::weakly_canonical(fs::current_path(), workspace_ec);
    if (workspace_ec) {
        result->set_status(remote_service::kFailed);
        result->set_message("Failed to determine server workspace!");
        result->clear_result();
        result->set_length(0);
        return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to determine server workspace!");
    }

    auto set_failure = [&](const grpc::Status &status) {
        result->set_status(remote_service::kFailed);
        result->set_message(status.error_message());
        result->clear_result();
        result->set_length(0);
        return status;
    };

    const fs::path default_filename = workspace_root / "server_task.bin";
    fs::path target_path = default_filename;
    int64_t total_received = 0;
    bool received_config = false;
    TaskConfig config_buffer;

    auto sanitize_result_path = [&](const std::string &input, std::string *output) -> grpc::Status {
        if (input.empty()) {
            output->clear();
            return grpc::Status::OK;
        }
        fs::path resolved;
        grpc::Status st = ResolveTaskFilePath(input, workspace_root, &resolved);
        if (!st.ok()) {
            return st;
        }
        *output = resolved.string();
        return grpc::Status::OK;
    };

    while (request->Read(&task_request)) {
        if (task_request.has_config()) {
            if (received_config) {
                return set_failure(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Duplicate TaskConfig payload!"));
            }
            received_config = true;
            config_buffer = task_request.config();

            fs::path resolved_entry;
            grpc::Status entry_status = ResolveTaskFilePath(config_buffer.entry_path(), workspace_root, &resolved_entry);
            if (!entry_status.ok()) {
                return set_failure(entry_status);
            }
            config_buffer.set_entry_path(resolved_entry.string());

            std::string resolved_result;
            grpc::Status result_status = sanitize_result_path(config_buffer.result_path(), &resolved_result);
            if (!result_status.ok()) {
                return set_failure(result_status);
            }
            config_buffer.set_result_path(resolved_result);
            continue;
        }

        if (!task_request.has_file_chunk()) {
            return set_failure(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "TaskRequest payload is missing!"));
        }

        if (!received_config) {
            return set_failure(grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "TaskConfig must be sent before file chunks!"));
        }

        const auto &chunk = task_request.file_chunk();
        if (!chunk.path().empty()) {
            grpc::Status path_status = ResolveTaskFilePath(chunk.path(), workspace_root, &target_path);
            if (!path_status.ok()) {
                return set_failure(path_status);
            }
        } else if (!outfile.is_open()) {
            target_path = default_filename;
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
                return set_failure(grpc::Status(grpc::StatusCode::INTERNAL, "Can not open file!"));
            }
            current_file_path = target_path;
        }

        if (!chunk.data().empty()) {
            outfile.write(chunk.data().data(), chunk.data().size());
            if (!outfile.good()) {
                return set_failure(grpc::Status(grpc::StatusCode::INTERNAL, "Failed to write to file!"));
            }
            total_received += static_cast<int64_t>(chunk.data().size());
        }

        if (chunk.file_end() && outfile.is_open()) {
            outfile.close();
            current_file_path.clear();
        }
    }

    if (!received_config) {
        return set_failure(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "TaskConfig payload is missing!"));
    }

    if (outfile.is_open()) {
        outfile.close();
    }

    result->set_length(total_received);
    config_out->CopyFrom(config_buffer);

    return grpc::Status::OK;
}
