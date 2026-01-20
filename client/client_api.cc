#include "client_api.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#include <atomic>
#include <chrono>
#include <thread>

#include <unistd.h>

#include <sys/time.h>

namespace {

namespace fs = std::filesystem;
constexpr size_t kChunkSize = 1024 * 1024;

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
                                             TaskSubmitReport *report, const UploadStats *upload_stats, bool enable_pty) {
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
    std::shared_ptr<grpc::ClientReaderWriter<remote_service::TaskRequest, remote_service::TaskResponse>> stream(
        stub_->TaskSubmission(&context));

    timeval start, end;
    gettimeofday(&start, nullptr);

    remote_service::TaskRequest config_request;
    auto *config = config_request.mutable_config();
    config->set_workspace_subdir(workspace_subdir);
    config->set_command(command);
    config->set_output_subdir(effective_output_subdir);
    config->set_enable_pty(enable_pty);

    if (!stream->Write(config_request)) {
        stream->WritesDone();
        stream->Finish();
        return grpc::Status(grpc::StatusCode::ABORTED, "Failed to send task configuration to the server.");
    }

    std::vector<char> buffer(kChunkSize);
    int64_t total_bytes = 0;
    bool write_failed = false;
    const bool enable_progress = (upload_stats != nullptr && upload_stats->total_bytes >= 0);
    const int64_t total_expected_bytes = enable_progress ? upload_stats->total_bytes : -1;
    int next_progress = 10;
    auto report_progress = [&](bool force) {
        if (!enable_progress || total_expected_bytes <= 0) {
            return;
        }
        int percent = total_expected_bytes == 0 ? 100 : static_cast<int>((total_bytes * 100) / total_expected_bytes);
        if (force || percent >= next_progress || total_bytes == total_expected_bytes) {
            std::cout << "    -> Sent " << HumanReadableBytes(total_bytes) << " / " << HumanReadableBytes(total_expected_bytes) << " (" << percent
                      << "%)" << std::endl;
            while (percent >= next_progress) {
                next_progress += 10;
            }
        }
    };

    for (const auto &file : files) {
        std::ifstream infile(file.local_path, std::ios::binary);
        if (!infile.is_open()) {
            stream->WritesDone();
            stream->Finish();
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
            chunk->set_data(buffer.data(), bytes_read);
            chunk->set_relative_path(file.relative_path);
            chunk->set_file_start(first_chunk);
            if (first_chunk && declared_size >= 0) {
                chunk->set_file_size(declared_size);
            }
            if (infile.eof()) {
                chunk->set_file_end(true);
            }

            total_bytes += bytes_read;
            report_progress(false);
            if (!stream->Write(chunk_request)) {
                write_failed = true;
                break;
            }
            first_chunk = false;
        }

        if (write_failed) {
            break;
        }
    }

    report_progress(true);

    stream->WritesDone();

    if (enable_progress) {
        std::cout << "[3/4] Executing command on server: \"" << command << "\" (output -> " << effective_output_subdir << ")" << std::endl;
    }

    remote_service::TaskResult final_result;
    remote_service::TaskResponse response;
    bool log_line_start = true;

    // Local-only idle indicator: helps when the remote process is preempted or quiet.
    // Printed to stderr, single-line, no extra remote logs.
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
        // Clear the current stderr line without ANSI.
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
        if (!enable_progress) {
            std::cout.write(data.data(), static_cast<std::streamsize>(data.size()));
            std::cout.flush();
            return;
        }

        // Fast path: insert a prefix at the beginning of each line.
        static constexpr const char *kPrefix = "    [server] ";
        std::string out;
        out.reserve(data.size() + 32);

        size_t pos = 0;
        while (pos < data.size()) {
            if (log_line_start) {
                out.append(kPrefix);
                log_line_start = false;
            }
            const size_t newline = data.find('\n', pos);
            if (newline == std::string::npos) {
                out.append(data, pos, std::string::npos);
                break;
            }
            out.append(data, pos, newline - pos + 1);
            log_line_start = true;
            pos = newline + 1;
        }

        std::cout.write(out.data(), static_cast<std::streamsize>(out.size()));
        std::cout.flush();
    };

    while (stream->Read(&response)) {
        if (response.has_log_chunk()) {
            const std::string &data = response.log_chunk().data();
            print_log(data);
        } else if (response.has_result()) {
            final_result = response.result();
        }
    }

    stop_idle.store(true, std::memory_order_relaxed);
    if (idle_thread.joinable()) {
        idle_thread.join();
    }
    clear_idle_line();

    grpc::Status status = stream->Finish();

    gettimeofday(&end, nullptr);
    report->elapsed_seconds = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1'000'000.0;
    report->bytes_sent = total_bytes;
    report->response = final_result;

    if (!status.ok()) {
        return status;
    }
    if (write_failed) {
        return grpc::Status(grpc::StatusCode::ABORTED, "Failed to stream file contents to the server.");
    }

    return grpc::Status::OK;
}
