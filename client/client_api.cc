#include "client_api.h"

#include "client_logging.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>

#include <unistd.h>

#include <sys/time.h>

namespace {

namespace fs = std::filesystem;
constexpr size_t kChunkSize = 1024 * 1024;
using TaskStream = grpc::ClientReaderWriter<remote_service::TaskRequest, remote_service::TaskResponse>;

std::string HumanReadableBytes(int64_t bytes) {
    if (bytes <= 0) {
        return "0 B";
    }
    static const char *kUnits[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < (sizeof(kUnits) / sizeof(kUnits[0]))) {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream oss;
    if (unit == 0) {
        oss << static_cast<int64_t>(value) << ' ' << kUnits[unit];
    } else {
        oss << std::fixed << std::setprecision(1) << value << ' ' << kUnits[unit];
    }
    return oss.str();
}

std::atomic<bool> g_client_interrupt_requested{false};

extern "C" void HandleClientInterrupt(int signo) {
    if (signo == SIGINT) {
        g_client_interrupt_requested.store(true, std::memory_order_relaxed);
    }
}

class ScopedClientCancellation {
  public:
    explicit ScopedClientCancellation(grpc::ClientContext *context) : context_(context) {
        g_client_interrupt_requested.store(false, std::memory_order_relaxed);

        // Mirror local Ctrl+C into the RPC so the server can cancel the remote
        // task instead of leaving it running after the client exits.
        struct sigaction action {};
        action.sa_handler = HandleClientInterrupt;
        sigemptyset(&action.sa_mask);
        sigaction(SIGINT, &action, &previous_action_);

        if (context_ != nullptr) {
            watcher_ = std::thread([this]() {
                while (!stop_.load(std::memory_order_relaxed)) {
                    if (g_client_interrupt_requested.load(std::memory_order_relaxed)) {
                        LogClientWarn("interrupt received, cancelling remote task");
                        context_->TryCancel();
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            });
        }
    }

    ~ScopedClientCancellation() {
        stop_.store(true, std::memory_order_relaxed);
        if (watcher_.joinable()) {
            watcher_.join();
        }
        sigaction(SIGINT, &previous_action_, nullptr);
    }

  private:
    grpc::ClientContext *context_ = nullptr;
    struct sigaction previous_action_ {};
    std::atomic<bool> stop_{false};
    std::thread watcher_;
};

struct UploadProgress {
    int64_t bytes_sent = 0;
    int64_t total_bytes = -1;
    int next_percent = 10;

    bool enabled() const { return total_bytes >= 0; }

    void Report(bool force) {
        if (!enabled() || total_bytes <= 0) {
            return;
        }

        const int percent = total_bytes == 0 ? 100 : static_cast<int>((bytes_sent * 100) / total_bytes);
        if (!force && percent < next_percent && bytes_sent != total_bytes) {
            return;
        }

        LogClientInfo("upload progress: " + HumanReadableBytes(bytes_sent) + " / " + HumanReadableBytes(total_bytes) + " (" +
                      std::to_string(percent) + "%)");
        while (percent >= next_percent) {
            next_percent += 10;
        }
    }
};

bool SendTaskConfig(TaskStream *stream, const std::string &workspace_subdir, const std::string &command, const std::string &output_subdir,
                    bool enable_pty, const XSchedConfig *xsched_config) {
    // The config message leads the stream so the server can resolve paths and
    // prepare the workspace before file chunks start arriving.
    remote_service::TaskRequest config_request;
    auto *config = config_request.mutable_config();
    config->set_workspace_subdir(workspace_subdir);
    config->set_command(command);
    config->set_output_subdir(output_subdir);
    config->set_enable_pty(enable_pty);
    if (xsched_config != nullptr && xsched_config->enabled) {
        config->set_xsched_enabled(true);
        if (xsched_config->xsched_utilization >= 0) {
            config->set_xsched_utilization(xsched_config->xsched_utilization);
        }
    }
    return stream->Write(config_request);
}

grpc::Status StreamWorkspaceFiles(TaskStream *stream, const std::vector<UploadFileSpec> &files, UploadProgress *progress) {
    std::vector<char> buffer(kChunkSize);

    for (const auto &file : files) {
        std::ifstream infile(file.local_path, std::ios::binary);
        if (!infile.is_open()) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Cannot open file: " + file.local_path);
        }

        std::error_code ec;
        int64_t declared_size = -1;
        const uintmax_t on_disk = fs::file_size(file.local_path, ec);
        if (!ec) {
            declared_size = static_cast<int64_t>(on_disk);
        }

        bool first_chunk = true;
        while (infile) {
            infile.read(buffer.data(), buffer.size());
            const std::streamsize bytes_read = infile.gcount();
            if (bytes_read <= 0) {
                break;
            }

            remote_service::TaskRequest chunk_request;
            auto *chunk = chunk_request.mutable_file_chunk();
            // The server rebuilds each file directly from stream metadata, so
            // every chunk carries its relative path and file boundary flags.
            chunk->set_data(buffer.data(), bytes_read);
            chunk->set_relative_path(file.relative_path);
            chunk->set_file_start(first_chunk);
            if (first_chunk && declared_size >= 0) {
                chunk->set_file_size(declared_size);
            }
            if (infile.eof()) {
                chunk->set_file_end(true);
            }

            progress->bytes_sent += bytes_read;
            progress->Report(false);
            if (!stream->Write(chunk_request)) {
                return grpc::Status(grpc::StatusCode::ABORTED, "Failed to stream file contents to the server.");
            }
            first_chunk = false;
        }
    }

    progress->Report(true);
    return grpc::Status::OK;
}

void DrainServerResponses(TaskStream *stream, bool enable_progress, bool raw_terminal_output, remote_service::TaskResult *final_result) {
    remote_service::TaskResponse response;
    bool log_line_start = true;

    // Printed locally while the server is quiet so the terminal does not look stuck.
    const bool enable_idle_indicator = enable_progress && ::isatty(fileno(stderr));
    std::atomic<bool> stop_idle{false};
    std::atomic<int64_t> last_log_us{0};
    auto now_us = []() -> int64_t {
        using clock = std::chrono::steady_clock;
        return std::chrono::duration_cast<std::chrono::microseconds>(clock::now().time_since_epoch()).count();
    };
    last_log_us.store(now_us(), std::memory_order_relaxed);
    std::atomic<bool> idle_line_visible{false};

    auto clear_idle_line = [&]() {
        if (!enable_idle_indicator) {
            return;
        }
        if (!idle_line_visible.exchange(false)) {
            return;
        }
        std::cerr << '\r' << std::string(96, ' ') << '\r';
        std::cerr.flush();
    };

    std::thread idle_thread;
    if (enable_idle_indicator) {
        idle_thread = std::thread([&]() {
            int64_t last_print_sec = -1;
            while (!stop_idle.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                if (stop_idle.load(std::memory_order_relaxed)) {
                    break;
                }

                const int64_t idle_us = now_us() - last_log_us.load(std::memory_order_relaxed);
                if (idle_us < 1200 * 1000) {
                    continue;
                }
                const int64_t idle_sec = idle_us / 1'000'000;
                if (idle_sec == last_print_sec) {
                    continue;
                }
                last_print_sec = idle_sec;
                idle_line_visible.store(true, std::memory_order_relaxed);
                std::cerr << "\r    [client] waiting for remote output... (idle " << idle_sec << "s)";
                std::cerr.flush();
            }
        });
    }

    auto print_log = [&](const std::string &data) {
        if (data.empty()) {
            return;
        }
        last_log_us.store(now_us(), std::memory_order_relaxed);
        clear_idle_line();
        if (raw_terminal_output || !enable_progress) {
            std::cout.write(data.data(), static_cast<std::streamsize>(data.size()));
            std::cout.flush();
            return;
        }

        static constexpr const char *kPrefix = "\033[34m[From server]\033[0m ";
        static constexpr const char *kRawPrefix = "[From server] ";
        static constexpr const char *kStagePrefix = "\033[34m[From server]\033[0m \033[33m";
        static constexpr const char *kColorReset = "\033[0m";
        std::string out;
        out.reserve(data.size() + 64);

        size_t pos = 0;
        while (pos < data.size()) {
            bool stage_line = false;
            if (log_line_start && data[pos] == '\n') {
                out.push_back('\n');
                ++pos;
                continue;
            }
            if (log_line_start) {
                // Server-generated status lines already include the prefix;
                // task stdout/stderr lines do not.
                const bool already_prefixed = data.compare(pos, std::char_traits<char>::length(kRawPrefix), kRawPrefix) == 0;
                const bool completion_line = !already_prefixed &&
                                             data.compare(pos, std::strlen("Command finished."), "Command finished.") == 0;
                if (already_prefixed) {
                    out.append(kStagePrefix);
                    pos += std::char_traits<char>::length(kRawPrefix);
                    stage_line = true;
                } else if (completion_line) {
                    out.append(kStagePrefix);
                    stage_line = true;
                } else {
                    out.append(kPrefix);
                }
                log_line_start = false;
            }
            const size_t newline = data.find('\n', pos);
            if (newline == std::string::npos) {
                out.append(data, pos, std::string::npos);
                if (stage_line) {
                    out.append(kColorReset);
                }
                break;
            }
            out.append(data, pos, newline - pos + 1);
            if (stage_line) {
                out.append(kColorReset);
            }
            log_line_start = true;
            pos = newline + 1;
        }

        std::cout.write(out.data(), static_cast<std::streamsize>(out.size()));
        std::cout.flush();
    };

    while (stream->Read(&response)) {
        if (response.has_log_chunk()) {
            print_log(response.log_chunk().data());
        } else if (response.has_result()) {
            *final_result = response.result();
        }
    }

    stop_idle.store(true, std::memory_order_relaxed);
    if (idle_thread.joinable()) {
        idle_thread.join();
    }
    clear_idle_line();
}

} // namespace

bool CollectDirectoryFiles(const std::string &root_dir, std::vector<UploadFileSpec> *files) {
    if (files == nullptr) {
        return false;
    }
    files->clear();

    std::error_code ec;
    if (!fs::exists(root_dir, ec) || !fs::is_directory(root_dir, ec)) {
        std::cerr << "Directory does not exist: " << root_dir << std::endl;
        return false;
    }

    fs::path base = fs::canonical(root_dir, ec);
    if (ec) {
        std::cerr << "Failed to canonicalize directory: " << root_dir << std::endl;
        return false;
    }

    for (const auto &entry : fs::recursive_directory_iterator(base, fs::directory_options::follow_directory_symlink)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        fs::path rel = fs::relative(entry.path(), base, ec);
        if (ec) {
            continue;
        }
        UploadFileSpec spec;
        spec.local_path = entry.path().string();
        spec.relative_path = rel.generic_string();
        files->push_back(std::move(spec));
    }

    if (files->empty()) {
        std::cerr << "No files found under: " << root_dir << std::endl;
        return false;
    }
    return true;
}

bool ComputeUploadStats(const std::vector<UploadFileSpec> &files, UploadStats *stats) {
    if (stats == nullptr) {
        return false;
    }
    stats->file_count = files.size();
    stats->total_bytes = 0;
    for (const auto &file : files) {
        std::error_code ec;
        uintmax_t file_size = fs::file_size(file.local_path, ec);
        if (ec) {
            std::cerr << "Failed to stat file: " << file.local_path << std::endl;
            return false;
        }
        stats->total_bytes += static_cast<int64_t>(file_size);
    }
    return true;
}

grpc::Status ValidateUploadFiles(const std::vector<UploadFileSpec> &files) {
    for (const auto &file : files) {
        std::error_code ec;
        if (!fs::exists(file.local_path, ec) || !fs::is_regular_file(file.local_path, ec)) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Local file missing or not readable: " + file.local_path);
        }
    }
    return grpc::Status::OK;
}

RemoteServiceClient::RemoteServiceClient(std::shared_ptr<grpc::Channel> channel) : stub_(remote_service::TaskManage::NewStub(channel)) {}

grpc::Status RemoteServiceClient::TaskSubmit(const std::vector<UploadFileSpec> &files, const std::string &workspace_subdir, const std::string &command,
                                             TaskSubmitReport *report, const UploadStats *upload_stats, bool enable_pty,
                                             const XSchedConfig *xsched_config) {
    if (files.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "No files specified for upload.");
    }
    if (report == nullptr) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "TaskSubmitReport pointer must not be null.");
    }
    if (command.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Command must not be empty.");
    }

    const std::string effective_output_subdir = (fs::path(workspace_subdir) / "output").generic_string();

    grpc::ClientContext context;
    ScopedClientCancellation cancellation(&context);
    std::shared_ptr<TaskStream> stream(stub_->TaskSubmission(&context));

    timeval start, end;
    gettimeofday(&start, nullptr);

    if (!SendTaskConfig(stream.get(), workspace_subdir, command, effective_output_subdir, enable_pty, xsched_config)) {
        stream->WritesDone();
        stream->Finish();
        return grpc::Status(grpc::StatusCode::ABORTED, "Failed to send task configuration to the server.");
    }

    UploadProgress progress;
    if (upload_stats != nullptr) {
        progress.total_bytes = upload_stats->total_bytes;
    }
    const bool enable_progress = progress.enabled();

    const grpc::Status upload_status = StreamWorkspaceFiles(stream.get(), files, &progress);

    stream->WritesDone();

    if (enable_progress) {
        LogClientInfo("executing command on server (output -> " + effective_output_subdir + ")");
    }

    remote_service::TaskResult final_result;
    DrainServerResponses(stream.get(), enable_progress, enable_pty, &final_result);

    grpc::Status status = stream->Finish();

    gettimeofday(&end, nullptr);
    report->elapsed_seconds = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1'000'000.0;
    report->bytes_sent = progress.bytes_sent;
    report->response = final_result;

    if (!status.ok()) {
        if (final_result.status() == remote_service::kFailed && !final_result.message().empty()) {
            return grpc::Status(status.error_code(), final_result.message());
        }
        return status;
    }
    if (!upload_status.ok()) {
        return upload_status;
    }

    return grpc::Status::OK;
}

grpc::Status RemoteServiceClient::UpdateUtilization(const std::string &workspace_subdir, int utilization,
                                                    remote_service::UpdateUtilizationResponse *response) {
    if (workspace_subdir.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "workspace_subdir must not be empty.");
    }
    if (utilization < 0 || utilization > 100) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "utilization must be in [0, 100].");
    }
    if (response == nullptr) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "UpdateUtilizationResponse pointer must not be null.");
    }

    remote_service::UpdateUtilizationRequest request;
    request.set_workspace_subdir(workspace_subdir);
    request.set_utilization(utilization);
    grpc::ClientContext context;
    return stub_->UpdateUtilization(&context, request, response);
}
