
#include "task_executor.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

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

} // namespace

TaskExecutor::TaskExecutor(const remote_service::TaskConfig &config, const fs::path &workspace_root,
                           grpc::ServerReaderWriter<remote_service::TaskResponse, remote_service::TaskRequest> *stream)
    : config_(config), workspace_root_(workspace_root), stream_(stream) {}

grpc::Status TaskExecutor::Execute(remote_service::TaskResult *result) const {
    if (config_.command().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Command must not be empty!");
    }

    fs::path run_dir = (workspace_root_ / config_.workspace_subdir()).lexically_normal();
    fs::path output_dir = (workspace_root_ / config_.output_subdir()).lexically_normal();

    std::error_code ec;
    fs::create_directories(run_dir, ec);
    if (ec) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to prepare workspace directory for execution!");
    }

    std::string combined_output;
    grpc::Status run_status = RunCommand(&combined_output);
    if (!run_status.ok()) {
        return run_status;
    }

    grpc::Status write_status = WriteTerminalOutput(combined_output, &output_dir);
    if (!write_status.ok()) {
        return write_status;
    }

    std::string archive_data;
    grpc::Status archive_status = CreateOutputArchive(output_dir, &archive_data);
    if (!archive_status.ok()) {
        return archive_status;
    }

    result->set_status(remote_service::kSuccess);
    result->set_message("Task executed successfully.");
    result->set_output_archive(archive_data);
    result->set_archive_size(static_cast<int64_t>(archive_data.size()));

    return grpc::Status::OK;
}

grpc::Status TaskExecutor::RunCommand(std::string *combined_output) const {
    if (combined_output == nullptr) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Output buffer must not be null!");
    }
    combined_output->clear();

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return grpc::Status(grpc::StatusCode::INTERNAL, std::string("Failed to create pipe: ") + std::strerror(errno));
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return grpc::Status(grpc::StatusCode::INTERNAL, std::string("Fork failed: ") + std::strerror(errno));
    } else if (pid == 0) {
        if (chdir((workspace_root_ / config_.workspace_subdir()).c_str()) != 0) {
            _exit(127);
        }
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0 || dup2(pipefd[1], STDERR_FILENO) < 0) {
            _exit(127);
        }
        close(pipefd[1]);
        execl("/bin/sh", "sh", "-c", config_.command().c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }

    close(pipefd[1]);
    char buffer[4096];
    ssize_t bytes_read = 0;
    while ((bytes_read = read(pipefd[0], buffer, sizeof(buffer))) > 0) {
        combined_output->append(buffer, static_cast<size_t>(bytes_read));
        EmitLogChunk(std::string(buffer, static_cast<size_t>(bytes_read)));
    }
    close(pipefd[0]);

    if (bytes_read < 0) {
        return grpc::Status(grpc::StatusCode::INTERNAL, std::string("Failed to read task output: ") + std::strerror(errno));
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to wait for task process!");
    }

    if (WIFEXITED(status)) {
        const int exit_code = WEXITSTATUS(status);
        if (exit_code != 0) {
            return grpc::Status(grpc::StatusCode::INTERNAL, "Task failed with exit code: " + std::to_string(exit_code));
        }
    } else if (WIFSIGNALED(status)) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "Task terminated by signal: " + std::to_string(WTERMSIG(status)));
    } else {
        return grpc::Status(grpc::StatusCode::INTERNAL, "Task ended abnormally!");
    }

    return grpc::Status::OK;
}

grpc::Status TaskExecutor::WriteTerminalOutput(const std::string &output, fs::path *output_dir) const {
    if (output_dir == nullptr) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Output directory pointer must not be null!");
    }

    std::error_code ec;
    fs::create_directories(*output_dir, ec);
    if (ec) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to create output directory on server!");
    }

    fs::path log_path = *output_dir / "terminal_output.txt";
    std::ofstream log_file(log_path, std::ios::binary | std::ios::trunc);
    if (!log_file.is_open()) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to create terminal output file!");
    }
    log_file.write(output.data(), static_cast<std::streamsize>(output.size()));
    if (!log_file.good()) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to write terminal output file!");
    }
    return grpc::Status::OK;
}

grpc::Status TaskExecutor::CreateOutputArchive(const fs::path &output_dir, std::string *archive_data) const {
    if (archive_data == nullptr) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Archive buffer must not be null!");
    }
    archive_data->clear();

    fs::path parent_dir = output_dir.parent_path();
    fs::path entry_name = output_dir.filename();
    if (entry_name.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Output directory name is invalid!");
    }

    fs::path archive_path = workspace_root_ / (entry_name.string() + ".tar.gz");

    std::string command =
        "tar -czf " + ShellEscape(archive_path.string()) + " -C " + ShellEscape(parent_dir.string()) + " " + ShellEscape(entry_name.generic_string());
    int ret = std::system(command.c_str());
    if (ret != 0) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to archive output directory!");
    }

    std::ifstream archive_file(archive_path, std::ios::binary);
    if (!archive_file.is_open()) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to open archive for reading!");
    }
    archive_data->assign(std::istreambuf_iterator<char>(archive_file), std::istreambuf_iterator<char>());
    if (!archive_file.good() && !archive_file.eof()) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to read archive data!");
    }

    std::error_code remove_ec;
    fs::remove(archive_path, remove_ec);

    return grpc::Status::OK;
}

void TaskExecutor::EmitLogChunk(const std::string &chunk) const {
    if (stream_ == nullptr || chunk.empty()) {
        return;
    }
    remote_service::TaskResponse response;
    response.mutable_log_chunk()->set_data(chunk);
    stream_->Write(response);
}
