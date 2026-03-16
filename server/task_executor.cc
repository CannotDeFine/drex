
#include "task_executor.h"

#include "server_logging.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <sstream>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <thread>
#include <vector>

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

int ParseEnvMillis(const char *name, int default_ms) {
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
}

bool SetNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

std::string QuoteForLog(const std::string &value) {
    return "'" + value + "'";
}

std::string JoinPaths(const std::vector<fs::path> &paths) {
    std::ostringstream oss;
    for (size_t i = 0; i < paths.size(); ++i) {
        if (i != 0) {
            oss << ", ";
        }
        oss << paths[i].filename().string();
    }
    return oss.str();
}

std::string FindCommandInPath(const std::string &name) {
    const char *path_env = std::getenv("PATH");
    if (path_env == nullptr || path_env[0] == '\0') {
        return "<PATH unset>";
    }

    std::stringstream ss(path_env);
    std::string item;
    while (std::getline(ss, item, ':')) {
        if (item.empty()) {
            continue;
        }
        fs::path candidate = fs::path(item) / name;
        std::error_code ec;
        if (fs::exists(candidate, ec)) {
            return candidate.string();
        }
    }
    return "<not found>";
}

std::string BuildCommandForExecution(const std::string &command, bool want_pty) {
    if (want_pty) {
        return command;
    }

    // In pipe mode, stdbuf keeps common CLI tools line-buffered so output
    // reaches the client sooner without requiring PTY mode.
    const std::string stdbuf_path = FindCommandInPath("stdbuf");
    if (stdbuf_path == "<not found>" || stdbuf_path == "<PATH unset>") {
        return command;
    }

    return ShellEscape(stdbuf_path) + " -oL -eL /bin/sh -c " + ShellEscape(command);
}

void ConfigureChildEnvironment(bool want_pty) {
    setenv("PYTHONUNBUFFERED", "1", 0);
    if (want_pty) {
        setenv("TERM", "xterm-256color", 0);
    }
}

void ConfigureXSchedEnvironment(const remote_service::TaskConfig &config) {
    if (!config.xsched_enabled()) {
        return;
    }

    // Remote tasks should register with the global xsched server. Global
    // policy and timeslice belong to xserver; the task only contributes its
    // per-xqueue utilization hint.
    setenv("XSCHED_SCHEDULER", "GLB", 1);
    setenv("XSCHED_AUTO_XQUEUE", "ON", 1);
    unsetenv("XSCHED_POLICY");
    if (config.xsched_utilization() >= 0) {
        setenv("XSCHED_AUTO_XQUEUE_UTILIZATION", std::to_string(config.xsched_utilization()).c_str(), 1);
    }
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
    // The current server model only streams terminal output; it does not
    // archive task artifacts back to the client.

    std::error_code ec;
    fs::create_directories(run_dir, ec);
    if (ec) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to prepare workspace directory for execution!");
    }

    LogServerInfo("starting command execution");
    LogServerDebug("starting command in '" + run_dir.string() + "'");
    LogExecutionContext(run_dir);

    std::string combined_output;
    grpc::Status run_status = RunCommand(&combined_output);
    if (!run_status.ok()) {
        LogServerWarn("command failed in '" + run_dir.string() + "': " + run_status.error_message());
        return run_status;
    }

    EmitLogChunk("Command finished.\n");

    result->set_status(remote_service::kSuccess);
    result->set_message("Task executed successfully.");
    result->clear_output_archive();
    result->set_archive_size(0);

    LogServerInfo("command finished successfully");
    LogServerDebug("command finished successfully in '" + run_dir.string() + "'");

    return grpc::Status::OK;
}

grpc::Status TaskExecutor::StartChildProcess(ChildProcess *child) const {
    if (child == nullptr) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "ChildProcess pointer must not be null!");
    }

    const bool want_pty = config_.enable_pty() && getenv("RPC_WORK_DISABLE_PTY") == nullptr;
    const std::string exec_command = BuildCommandForExecution(config_.command(), want_pty);
    if (want_pty) {
        int master_fd = -1;
        const pid_t pid = forkpty(&master_fd, nullptr, nullptr, nullptr);
        if (pid < 0) {
            return grpc::Status(grpc::StatusCode::INTERNAL, std::string("forkpty failed: ") + std::strerror(errno));
        }
        if (pid == 0) {
            (void)setpgid(0, 0);
            // Put the task in its own process group so cancellation and timeout
            // can terminate the whole tree with killpg().
            ConfigureChildEnvironment(true);
            ConfigureXSchedEnvironment(config_);
            if (chdir((workspace_root_ / config_.workspace_subdir()).c_str()) != 0) {
                _exit(127);
            }
            execl("/bin/sh", "sh", "-c", exec_command.c_str(), static_cast<char *>(nullptr));
            _exit(127);
        }

        child->pid = pid;
        child->read_fd = master_fd;
        child->use_pty = true;
        if (!SetNonBlocking(child->read_fd)) {
            close(child->read_fd);
            kill(pid, SIGKILL);
            (void)waitpid(pid, nullptr, 0);
            return grpc::Status(grpc::StatusCode::INTERNAL, std::string("Failed to configure PTY fd: ") + std::strerror(errno));
        }
        return grpc::Status::OK;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return grpc::Status(grpc::StatusCode::INTERNAL, std::string("Failed to create pipe: ") + std::strerror(errno));
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return grpc::Status(grpc::StatusCode::INTERNAL, std::string("Fork failed: ") + std::strerror(errno));
    }
    if (pid == 0) {
        (void)setpgid(0, 0);
        // Put the task in its own process group so cancellation and timeout can
        // terminate the whole tree with killpg().
        ConfigureChildEnvironment(false);
        ConfigureXSchedEnvironment(config_);
        if (chdir((workspace_root_ / config_.workspace_subdir()).c_str()) != 0) {
            _exit(127);
        }
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0 || dup2(pipefd[1], STDERR_FILENO) < 0) {
            _exit(127);
        }
        close(pipefd[1]);

        execl("/bin/sh", "sh", "-c", exec_command.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }

    close(pipefd[1]);
    child->pid = pid;
    child->read_fd = pipefd[0];
    child->use_pty = false;
    if (!SetNonBlocking(child->read_fd)) {
        close(child->read_fd);
        kill(pid, SIGKILL);
        (void)waitpid(pid, nullptr, 0);
        return grpc::Status(grpc::StatusCode::INTERNAL, std::string("Failed to configure pipe fd: ") + std::strerror(errno));
    }
    return grpc::Status::OK;
}

grpc::Status TaskExecutor::MonitorChildProcess(const ChildProcess &child, std::string *combined_output) const {
    constexpr int kPtyPollTimeoutMs = 30;
    constexpr int kPipePollTimeoutMs = 200;
    constexpr size_t kPtyReadSize = 1024;
    constexpr size_t kPipeReadSize = 4096;

    std::vector<char> buffer(child.use_pty ? kPtyReadSize : kPipeReadSize);
    int status = 0;
    std::atomic<bool> cancelled{false};

    const int idle_timeout_ms = ParseEnvMillis("RPC_WORK_IDLE_TIMEOUT_MS", -1);
    auto last_output_tp = std::chrono::steady_clock::now();
    bool saw_remote_output = false;

    EmitLogChunk("[From server] Command started, waiting for output...\n");

    auto kill_child_group = [&](int sig) {
        (void)killpg(child.pid, sig);
    };

    std::atomic<bool> stop_cancel_thread{false};
    std::thread cancel_thread;
    if (context_ != nullptr) {
        cancel_thread = std::thread([&]() {
            while (!stop_cancel_thread.load(std::memory_order_relaxed) && !context_->IsCancelled()) {
                usleep(1000);
            }

            if (stop_cancel_thread.load(std::memory_order_relaxed)) {
                return;
            }
            // gRPC cancellation only aborts the RPC. The subprocess must be
            // stopped explicitly on the server side.
            cancelled.store(true, std::memory_order_relaxed);
            kill_child_group(SIGTERM);
            usleep(2000 * 1000);
            kill_child_group(SIGKILL);
        });
    }

    bool child_exited = false;
    int wait_status = 0;
    auto child_exit_tp = std::chrono::steady_clock::time_point{};
    for (;;) {
        if (!child_exited) {
            const pid_t w = waitpid(child.pid, &wait_status, WNOHANG);
            if (w == child.pid) {
                child_exited = true;
                child_exit_tp = std::chrono::steady_clock::now();
            }
        }

        pollfd pfd;
        pfd.fd = child.read_fd;
        pfd.events = POLLIN | POLLHUP;
        pfd.revents = 0;

        const int prc = poll(&pfd, 1, child.use_pty ? kPtyPollTimeoutMs : kPipePollTimeoutMs);
        if (prc < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(child.read_fd);
            stop_cancel_thread.store(true, std::memory_order_relaxed);
            if (cancel_thread.joinable()) {
                cancel_thread.join();
            }
            return grpc::Status(grpc::StatusCode::INTERNAL, std::string("poll() failed: ") + std::strerror(errno));
        }

        if (prc == 0) {
            if (!child_exited) {
                const auto now = std::chrono::steady_clock::now();
                const int idle_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now - last_output_tp).count());

                if (idle_timeout_ms >= 0 && idle_ms >= idle_timeout_ms) {
                    EmitLogChunk("\nNo output for " + std::to_string(idle_ms) + " ms; task did not exit. Terminating process group...\n");
                    kill_child_group(SIGTERM);
                    usleep(2000 * 1000);
                    kill_child_group(SIGKILL);
                    stop_cancel_thread.store(true, std::memory_order_relaxed);
                    if (cancel_thread.joinable()) {
                        cancel_thread.join();
                    }
                    (void)waitpid(child.pid, &status, 0);
                    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                                        "Task timed out due to inactivity (no terminal output).\n"
                                        "Hint: set RPC_WORK_IDLE_TIMEOUT_MS=-1 to disable, or increase it.");
                }

            } else {
                const auto drain_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - child_exit_tp).count();
                if (drain_ms >= 50) {
                    break;
                }
            }
            continue;
        }

        if (pfd.revents & (POLLIN | POLLHUP)) {
            bool reached_end_of_stream = false;
            for (;;) {
                const ssize_t bytes_read = read(child.read_fd, buffer.data(), buffer.size());
                if (bytes_read > 0) {
                    combined_output->append(buffer.data(), static_cast<size_t>(bytes_read));
                    EmitLogChunk(std::string(buffer.data(), static_cast<size_t>(bytes_read)));
                    last_output_tp = std::chrono::steady_clock::now();
                    saw_remote_output = true;
                    continue;
                }
                if (bytes_read == 0) {
                    reached_end_of_stream = true;
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                if (child.use_pty && errno == EIO) {
                    // PTY masters often report EIO once the slave side is closed.
                    reached_end_of_stream = true;
                    break;
                }
                close(child.read_fd);
                stop_cancel_thread.store(true, std::memory_order_relaxed);
                if (cancel_thread.joinable()) {
                    cancel_thread.join();
                }
                return grpc::Status(grpc::StatusCode::INTERNAL, std::string("Failed to read task output: ") + std::strerror(errno));
            }

            if (reached_end_of_stream) {
                break;
            }
            continue;
        }
    }

    close(child.read_fd);
    stop_cancel_thread.store(true, std::memory_order_relaxed);
    if (cancel_thread.joinable()) {
        cancel_thread.join();
    }

    if (!child_exited) {
        if (waitpid(child.pid, &status, 0) < 0) {
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
            std::string message = "Task failed with exit code: " + std::to_string(exit_code);
            if (!combined_output->empty()) {
                constexpr size_t kTailLimit = 4096;
                const size_t start = combined_output->size() > kTailLimit ? combined_output->size() - kTailLimit : 0;
                message += "\n--- remote output tail ---\n";
                message.append(combined_output->data() + start, combined_output->size() - start);
            }
            return grpc::Status(grpc::StatusCode::INTERNAL, message);
        }
        return grpc::Status::OK;
    }
    if (WIFSIGNALED(status)) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "Task terminated by signal: " + std::to_string(WTERMSIG(status)));
    }
    return grpc::Status(grpc::StatusCode::INTERNAL, "Task ended abnormally!");
}

grpc::Status TaskExecutor::RunCommand(std::string *combined_output) const {
    if (combined_output == nullptr) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Output buffer must not be null!");
    }
    combined_output->clear();
    ChildProcess child;
    grpc::Status start_status = StartChildProcess(&child);
    if (!start_status.ok()) {
        return start_status;
    }

    // Best-effort ensure a dedicated process group exists even if setpgid in child raced.
    (void)setpgid(child.pid, child.pid);
    return MonitorChildProcess(child, combined_output);
}

void TaskExecutor::LogExecutionContext(const fs::path &run_dir) const {
    std::ostringstream oss;
    oss << "execution context: cwd=" << QuoteForLog(run_dir.string())
        << " path=" << QuoteForLog(std::getenv("PATH") != nullptr ? std::getenv("PATH") : "<unset>")
        << " shell=" << QuoteForLog(FindCommandInPath("sh"))
        << " bash=" << QuoteForLog(FindCommandInPath("bash"))
        << " g++=" << QuoteForLog(FindCommandInPath("g++"))
        << " taskset=" << QuoteForLog(FindCommandInPath("taskset"))
        << " nice=" << QuoteForLog(FindCommandInPath("nice"));
    LogServerDebug(oss.str());

    std::error_code ec;
    const fs::path script_path = run_dir / "run_low.sh";
    std::ostringstream file_oss;
    file_oss << "workspace files: ";
    std::vector<fs::path> entries;
    for (const auto &entry : fs::directory_iterator(run_dir, ec)) {
        if (ec) {
            break;
        }
        entries.push_back(entry.path());
    }
    if (ec) {
        file_oss << "<failed to enumerate: " << ec.message() << ">";
    } else {
        file_oss << JoinPaths(entries);
    }
    LogServerDebug(file_oss.str());

    std::ostringstream script_oss;
    script_oss << "script check: path=" << QuoteForLog(script_path.string())
               << " exists=" << (fs::exists(script_path) ? "true" : "false")
               << " executable=" << (::access(script_path.c_str(), X_OK) == 0 ? "true" : "false");
    LogServerDebug(script_oss.str());
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
