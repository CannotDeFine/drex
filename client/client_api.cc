#include "client_api.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <utility>

#include <sys/time.h>

#include <absl/strings/ascii.h>
#include <absl/strings/str_split.h>

namespace {

namespace fs = std::filesystem;

constexpr size_t kChunkSize = 1024 * 1024;

remote_service::TaskFileType ParseFileType(absl::string_view type_token) {
    const std::string type = absl::AsciiStrToLower(type_token);
    if (type == "exe" || type == "exec" || type == "bin") {
        return remote_service::kFileExecutable;
    }
    if (type == "script") {
        return remote_service::kFileScript;
    }
    if (type == "data" || type == "dataset") {
        return remote_service::kFileDataset;
    }
    if (type == "result" || type == "output") {
        return remote_service::kFileResult;
    }
    return remote_service::kFileUnknown;
}

bool ParseExtraFileSpec(absl::string_view spec, UploadFileSpec *out_spec) {
    std::vector<std::string> parts = absl::StrSplit(spec, ':');
    if (parts.size() < 2 || parts[1].empty()) {
        return false;
    }

    out_spec->file_type = ParseFileType(parts[0]);
    out_spec->local_path = parts[1];
    if (parts.size() >= 3 && !parts[2].empty()) {
        out_spec->remote_path = parts[2];
    } else {
        fs::path local(out_spec->local_path);
        out_spec->remote_path = local.filename().string();
    }

    return true;
}

bool ParseDirectorySpec(absl::string_view spec, DirectorySpec *out_spec) {
    std::vector<std::string> parts = absl::StrSplit(spec, ':');
    if (parts.size() < 2 || parts[1].empty()) {
        return false;
    }

    out_spec->file_type = ParseFileType(parts[0]);
    out_spec->local_dir = parts[1];
    if (parts.size() >= 3 && !parts[2].empty()) {
        out_spec->remote_dir = parts[2];
    } else {
        fs::path local(out_spec->local_dir);
        out_spec->remote_dir = local.filename().string();
    }

    return true;
}

bool ExpandDirectory(const DirectorySpec &dir_spec, std::vector<UploadFileSpec> *files) {
    std::error_code ec;
    if (!fs::exists(dir_spec.local_dir, ec) || !fs::is_directory(dir_spec.local_dir, ec)) {
        std::cerr << "Directory does not exist: " << dir_spec.local_dir << std::endl;
        return false;
    }

    fs::path base = fs::canonical(dir_spec.local_dir, ec);
    if (ec) {
        std::cerr << "Failed to canonicalize directory: " << dir_spec.local_dir << std::endl;
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
        fs::path remote_root = dir_spec.remote_dir.empty() ? base.filename() : fs::path(dir_spec.remote_dir);
        fs::path remote = remote_root / rel;

        UploadFileSpec spec;
        spec.local_path = entry.path().string();
        spec.remote_path = remote.generic_string();
        spec.file_type = dir_spec.file_type;
        files->push_back(std::move(spec));
    }
    return true;
}

} // namespace

bool CollectExtraFiles(absl::string_view flag_value, std::vector<UploadFileSpec> *files) {
    if (flag_value.empty()) {
        return true;
    }

    bool success = true;
    for (absl::string_view spec_token : absl::StrSplit(flag_value, ',', absl::SkipWhitespace())) {
        if (spec_token.empty()) {
            continue;
        }
        UploadFileSpec extra;
        if (!ParseExtraFileSpec(spec_token, &extra)) {
            std::cerr << "Invalid extra file spec: " << spec_token << std::endl;
            success = false;
            continue;
        }
        files->push_back(std::move(extra));
    }
    return success;
}

bool CollectExtraDirectories(absl::string_view flag_value, std::vector<UploadFileSpec> *files) {
    if (flag_value.empty()) {
        return true;
    }

    bool success = true;
    for (absl::string_view dir_token : absl::StrSplit(flag_value, ',', absl::SkipWhitespace())) {
        if (dir_token.empty()) {
            continue;
        }
        DirectorySpec dir_spec;
        if (!ParseDirectorySpec(dir_token, &dir_spec)) {
            std::cerr << "Invalid directory spec: " << dir_token << std::endl;
            success = false;
            continue;
        }
        if (!ExpandDirectory(dir_spec, files)) {
            success = false;
        }
    }
    return success;
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

grpc::Status RemoteServiceClient::TaskSubmit(const std::vector<UploadFileSpec> &files, remote_service::ReqDeviceType device_type,
                                             const std::string &entry_path, const std::string &result_path, TaskSubmitReport *report) {
    if (files.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "No files specified for upload.");
    }
    if (report == nullptr) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "TaskSubmitReport pointer must not be null.");
    }

    grpc::ClientContext context;
    remote_service::TaskResult response;
    std::unique_ptr<grpc::ClientWriter<remote_service::TaskRequest>> writer(stub_->TaskSubmission(&context, &response));

    timeval start, end;
    gettimeofday(&start, nullptr);

    remote_service::TaskRequest config_request;
    auto *config = config_request.mutable_config();
    config->set_device_type(device_type);
    config->set_ip_address("client");
    config->set_entry_path(entry_path);
    if (!result_path.empty()) {
        config->set_result_path(result_path);
    }

    if (!writer->Write(config_request)) {
        writer->WritesDone();
        writer->Finish();
        return grpc::Status(grpc::StatusCode::ABORTED, "Failed to send task configuration to the server.");
    }

    std::vector<char> buffer(kChunkSize);
    int64_t total_bytes = 0;
    bool write_failed = false;

    for (const auto &file : files) {
        std::ifstream infile(file.local_path, std::ios::binary);
        if (!infile.is_open()) {
            writer->WritesDone();
            writer->Finish();
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
            chunk->set_path(file.remote_path);
            chunk->set_file_type(file.file_type);
            chunk->set_file_start(first_chunk);
            if (first_chunk && declared_size >= 0) {
                chunk->set_file_size(declared_size);
            }
            if (infile.eof()) {
                chunk->set_file_end(true);
            }

            total_bytes += bytes_read;
            if (!writer->Write(chunk_request)) {
                write_failed = true;
                break;
            }
            first_chunk = false;
        }

        if (write_failed) {
            break;
        }
    }

    writer->WritesDone();
    grpc::Status status = writer->Finish();

    gettimeofday(&end, nullptr);
    report->elapsed_seconds = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1'000'000.0;
    report->bytes_sent = total_bytes;
    report->response = response;

    if (!status.ok()) {
        return status;
    }
    if (write_failed) {
        return grpc::Status(grpc::StatusCode::ABORTED, "Failed to stream file contents to the server.");
    }

    return grpc::Status::OK;
}
