
#include "task_executor.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <thread>

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

std::string SanitizeFilenameComponent(std::string input) {
    for (char &c : input) {
        if (c == '/' || c == '\\' || c == ' ') {
            c = '_';
        }
    }
    if (input.empty()) {
        return "_";
    }
    return input;
}

} // namespace

TaskExecutor::TaskExecutor(grpc::ServerContext *context, const remote_service::TaskConfig &config, const fs::path &workspace_root,
                           grpc::ServerReaderWriter<remote_service::TaskResponse, remote_service::TaskRequest> *stream)
    : context_(context), config_(config), workspace_root_(workspace_root), stream_(stream) {}

grpc::Status TaskExecutor::Execute(remote_service::TaskResult *result) const {
    if (config_.command().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Command must not be empty!");
    }

    fs::path run_dir = (workspace_root_ / config_.workspace_subdir()).lexically_normal();
    // NOTE: We intentionally do not create/archive output_subdir anymore.
    // This server is configured to only stream terminal output back to the client.

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

    EmitLogChunk("\nCommand finished. (output archive disabled)\n");

    result->set_status(remote_service::kSuccess);
    result->set_message("Task executed successfully.");
    result->clear_output_archive();
    result->set_archive_size(0);

    return grpc::Status::OK;
}

grpc::Status TaskExecutor::RunCommand(std::string *combined_output) const {
    if (combined_output == nullptr) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Output buffer must not be null!");
    }
    combined_output->clear();

    const bool want_pty = config_.enable_pty() && getenv("RPC_WORK_DISABLE_PTY") == nullptr;
    int read_fd = -1;
    pid_t pid = -1;

    if (want_pty) {
        // Use forkpty instead of `script` to avoid extra wrapper processes that may
        // create new sessions/process groups and escape killpg().
        int master_fd = -1;
        pid = forkpty(&master_fd, nullptr, nullptr, nullptr);
        if (pid < 0) {
            return grpc::Status(grpc::StatusCode::INTERNAL, std::string("forkpty failed: ") + std::strerror(errno));
        }
        if (pid == 0) {
            (void)setpgid(0, 0);
            if (chdir((workspace_root_ / config_.workspace_subdir()).c_str()) != 0) {
                _exit(127);
            }
            execl("/bin/sh", "sh", "-c", config_.command().c_str(), static_cast<char *>(nullptr));
            _exit(127);
        }
        read_fd = master_fd;
    } else {
        int pipefd[2];
        if (pipe(pipefd) != 0) {
            return grpc::Status(grpc::StatusCode::INTERNAL, std::string("Failed to create pipe: ") + std::strerror(errno));
        }
        pid = fork();
        if (pid < 0) {
            close(pipefd[0]);
            close(pipefd[1]);
            return grpc::Status(grpc::StatusCode::INTERNAL, std::string("Fork failed: ") + std::strerror(errno));
        } else if (pid == 0) {
            (void)setpgid(0, 0);
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
        read_fd = pipefd[0];
    }

    // Best-effort ensure a dedicated process group exists even if setpgid in child raced.
    (void)setpgid(pid, pid);

    char buffer[4096];
    int status = 0;
    std::atomic<bool> cancelled{false};

    const auto parse_env_ms = [](const char *name, int default_ms) -> int {
        const char *value = std::getenv(name);
        if (value == nullptr || value[0] == '\0') {
            return default_ms;
        }
        char *end = nullptr;
        long parsed = std::strtol(value, &end, 10);
        if (end == value || *end != '\0') {
            return default_ms;
        }
        if (parsed < 0) {
            return -1;
        }
        if (parsed > 24L * 60L * 60L * 1000L) {
            return 24 * 60 * 60 * 1000;
        }
        return static_cast<int>(parsed);
    };

    // Optional watchdog: if the task stops producing output and never exits (e.g., stuck cleanup),
    // you can enable a kill+timeout by setting RPC_WORK_IDLE_TIMEOUT_MS to a non-negative value.
    // Default is disabled.
    const int idle_timeout_ms = parse_env_ms("RPC_WORK_IDLE_TIMEOUT_MS", -1);
    const int idle_log_every_ms = 1000;
    auto last_output_tp = std::chrono::steady_clock::now();
    auto last_idle_log_tp = last_output_tp;

    auto kill_child_group = [&](int sig) {
        // Best-effort; ignore errors if process already exited.
        (void)killpg(pid, sig);
    };

    std::atomic<bool> stop_cancel_thread{false};
    std::thread cancel_thread;
    if (context_ != nullptr) {
        cancel_thread = std::thread([&]() {
            // Tight-ish loop to react quickly without blocking output reads.
            while (!stop_cancel_thread.load(std::memory_order_relaxed) && !context_->IsCancelled()) {
                usleep(1000); // 1ms
            }

            if (stop_cancel_thread.load(std::memory_order_relaxed)) {
                return;
            }
            cancelled.store(true, std::memory_order_relaxed);
            kill_child_group(SIGTERM);
            // Give the process a short grace period, then hard kill.
            usleep(2000 * 1000);
            kill_child_group(SIGKILL);
        });
    }

    // Read output while the child is running.
    // IMPORTANT: do not rely solely on EOF, because background processes may inherit stdout/stderr
    // and keep the pipe/pty open after the main command exits, which would otherwise hang forever.
    bool child_exited = false;
    int wait_status = 0;
    for (;;) {
        if (!child_exited) {
            const pid_t w = waitpid(pid, &wait_status, WNOHANG);
            if (w == pid) {
                child_exited = true;
            }
        }

        pollfd pfd;
        pfd.fd = read_fd;
        pfd.events = POLLIN | POLLHUP;
        pfd.revents = 0;

        const int timeout_ms = 200;
        const int prc = poll(&pfd, 1, timeout_ms);
        if (prc < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(read_fd);
            stop_cancel_thread.store(true, std::memory_order_relaxed);
            if (cancel_thread.joinable()) {
                cancel_thread.join();
            }
            return grpc::Status(grpc::StatusCode::INTERNAL, std::string("poll() failed: ") + std::strerror(errno));
        }

        if (prc == 0) {
            // No output available right now.
            if (!child_exited) {
                const auto now = std::chrono::steady_clock::now();
                const int idle_ms = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - last_output_tp).count());

                if (idle_timeout_ms >= 0 && idle_ms >= idle_timeout_ms) {
                    EmitLogChunk("\nNo output for " + std::to_string(idle_ms) +
                                 " ms; task did not exit. Terminating process group...\n");
                    kill_child_group(SIGTERM);
                    usleep(2000 * 1000);
                    kill_child_group(SIGKILL);
                    stop_cancel_thread.store(true, std::memory_order_relaxed);
                    if (cancel_thread.joinable()) {
                        cancel_thread.join();
                    }
                    // Reap child.
                    (void)waitpid(pid, &status, 0);
                    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                                        "Task timed out due to inactivity (no terminal output).\n"
                                        "Hint: set RPC_WORK_IDLE_TIMEOUT_MS=-1 to disable, or increase it.");
                }

                const int since_last_log_ms = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - last_idle_log_tp).count());
                if (idle_timeout_ms >= 0) {
                    if (since_last_log_ms >= idle_log_every_ms && idle_ms >= idle_log_every_ms) {
                        EmitLogChunk("waiting for task process to exit... (idle " + std::to_string(idle_ms / 1000) + "s)\n");
                        last_idle_log_tp = now;
                    }
                }
            }
            if (child_exited) {
                break;
            }
            continue;
        }

        if (pfd.revents & (POLLIN | POLLHUP)) {
            const ssize_t bytes_read = read(read_fd, buffer, sizeof(buffer));
            if (bytes_read > 0) {
                combined_output->append(buffer, static_cast<size_t>(bytes_read));
                EmitLogChunk(std::string(buffer, static_cast<size_t>(bytes_read)));
                last_output_tp = std::chrono::steady_clock::now();
                continue;
            }
            if (bytes_read == 0) {
                break; // EOF
            }
            if (errno == EINTR) {
                continue;
            }
            close(read_fd);
            stop_cancel_thread.store(true, std::memory_order_relaxed);
            if (cancel_thread.joinable()) {
                cancel_thread.join();
            }
            return grpc::Status(grpc::StatusCode::INTERNAL, std::string("Failed to read task output: ") + std::strerror(errno));
        }
    }

    close(read_fd);

    stop_cancel_thread.store(true, std::memory_order_relaxed);
    if (cancel_thread.joinable()) {
        cancel_thread.join();
    }

    if (!child_exited) {
        if (waitpid(pid, &status, 0) < 0) {
            return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to wait for task process!");
        }
    } else {
        status = wait_status;
    }

    if (cancelled.load(std::memory_order_relaxed)) {
        return grpc::Status(grpc::StatusCode::CANCELLED, "Client cancelled request.");
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

    const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    const std::string key = SanitizeFilenameComponent(config_.workspace_subdir());
    const std::string base = key + "_" + SanitizeFilenameComponent(entry_name.string()) + "_" + std::to_string(static_cast<long long>(getpid())) +
                             "_" + std::to_string(static_cast<long long>(now_us));
    fs::path archive_path = workspace_root_ / (base + ".tar.gz");

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
