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

grpc::Status ResolveTaskFilePath(const std::string &requested_path,
                                 const fs::path &canonical_workspace_root,
                                 fs::path *resolved_path) {
    fs::path candidate_path;
    if (requested_path.empty()) {
        candidate_path = canonical_workspace_root / "server_task.bin";
    } else {
        fs::path requested(requested_path);
        candidate_path = requested.is_absolute()
                             ? requested
                             : canonical_workspace_root / requested;
    }

    std::error_code ec;
    fs::path canonical_target = fs::weakly_canonical(candidate_path, ec);
    if (ec) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "Failed to resolve task file path!");
    }

    if (!IsSubPath(canonical_workspace_root, canonical_target)) {
        return grpc::Status(
            grpc::StatusCode::PERMISSION_DENIED,
            "Task file path must stay within the server workspace!");
    }

    *resolved_path = canonical_target;
    return grpc::Status::OK;
}

} // namespace

grpc::Status
RemoteServiceImplement::TaskSubmission(grpc::ServerContext *context,
                                       ServerReader<TaskRequest> *request,
                                       TaskResult *result) {
    TaskRequest task_request;

    grpc::Status download_status = DownloadFile(request, result, &task_request);
    if (!download_status.ok()) {
        result->set_status(remote_service::kFailed);
        result->set_message(download_status.error_message());
        result->clear_result();
        result->set_length(0);
        return download_status;
    }

    auto task = TaskFactory::Create(task_request);
    if (!task) {
        result->set_status(remote_service::kFailed);
        result->set_message("Unknown device type!");
        result->clear_result();
        result->set_length(0);
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "Unknown device type!");
    }

    grpc::Status exec_status = task->Execute(result);
    if (!exec_status.ok()) {
        return exec_status;
    }

    return grpc::Status::OK;
}

grpc::Status
RemoteServiceImplement::DownloadFile(ServerReader<TaskRequest> *request,
                                     TaskResult *result,
                                     TaskRequest *last_request) {
    TaskRequest task_request;
    std::ofstream outfile;
    fs::path current_file_path;
    std::error_code workspace_ec;
    fs::path workspace_root =
        fs::weakly_canonical(fs::current_path(), workspace_ec);
    if (workspace_ec) {
        result->set_status(remote_service::kFailed);
        result->set_message("Failed to determine server workspace!");
        result->clear_result();
        result->set_length(0);
        return grpc::Status(grpc::StatusCode::INTERNAL,
                            "Failed to determine server workspace!");
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
    bool has_result_path = false;
    std::string resolved_result_path;

    while (request->Read(&task_request)) {
        last_request->CopyFrom(task_request);
        if (!task_request.result_path().empty() && !has_result_path) {
            fs::path resolved_result;
            grpc::Status result_path_status = ResolveTaskFilePath(
                task_request.result_path(), workspace_root, &resolved_result);
            if (!result_path_status.ok()) {
                return set_failure(result_path_status);
            }
            has_result_path = true;
            resolved_result_path = resolved_result.string();
        }

        if (!task_request.path().empty()) {
            grpc::Status path_status = ResolveTaskFilePath(
                task_request.path(), workspace_root, &target_path);
            if (!path_status.ok()) {
                return set_failure(path_status);
            }
        } else if (!outfile.is_open()) {
            target_path = default_filename;
        }

        const bool need_new_file = !outfile.is_open() ||
                                   task_request.file_start() ||
                                   target_path != current_file_path;

        if (need_new_file) {
            if (outfile.is_open()) {
                outfile.close();
            }
            std::error_code dir_ec;
            fs::create_directories(target_path.parent_path(), dir_ec);
            if (dir_ec) {
                return set_failure(
                    grpc::Status(grpc::StatusCode::INTERNAL,
                                 "Failed to prepare workspace directory!"));
            }
            outfile.open(target_path, std::ios::binary | std::ios::trunc);
            if (!outfile.is_open()) {
                return set_failure(grpc::Status(grpc::StatusCode::INTERNAL,
                                                "Can not open file!"));
            }
            current_file_path = target_path;
        }

        if (!task_request.task().empty()) {
            outfile.write(task_request.task().data(),
                          task_request.task().size());
            if (!outfile.good()) {
                return set_failure(grpc::Status(grpc::StatusCode::INTERNAL,
                                                "Failed to write to file!"));
            }
            total_received += static_cast<int64_t>(task_request.task().size());
        }

        if (task_request.file_end() && outfile.is_open()) {
            outfile.close();
            current_file_path.clear();
        }
    }

    if (outfile.is_open()) {
        outfile.close();
    }

    result->set_length(total_received);
    if (has_result_path) {
        last_request->set_result_path(resolved_result_path);
    }

    return grpc::Status::OK;
}
