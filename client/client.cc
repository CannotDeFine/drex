#include <absl/flags/flag.h>
#include <absl/flags/parse.h>
#include <absl/strings/ascii.h>
#include <absl/strings/str_split.h>
#include <absl/strings/string_view.h>
#include <grpcpp/grpcpp.h>
#include <sys/time.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "remote_service.grpc.pb.h"
#include "remote_service.pb.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::ClientWriter;
using grpc::Status;
using remote_service::ReqDeviceType;
using remote_service::TaskManage;
using remote_service::TaskRequest;
using remote_service::TaskResult;

ABSL_FLAG(std::string, target, "127.0.0.1:8063", "Server address.");
ABSL_FLAG(std::string, file, "/home/cdf/rpc-work/application/cpu/app",
          "Local executable to submit.");
ABSL_FLAG(std::string, save_path, "./server_task.bin",
          "Destination path for the primary executable on server.");
ABSL_FLAG(std::string, extra_files, "",
          "Comma separated list of extra files: type:local[:remote].");
ABSL_FLAG(std::string, extra_dirs, "",
          "Comma separated list of directories: type:local_dir[:remote_dir].");
ABSL_FLAG(int, device, 0,
          "Device type (0=CPU, 1=DPU, 2=FPGA, 3=GPU, 4=NPU, 5=OFA).");
ABSL_FLAG(std::string, result_path, "",
          "Server-side path of result file to download after execution.");

namespace {

namespace fs = std::filesystem;

constexpr size_t kChunkSize = 1024 * 1024;
constexpr int kExecCount = 1;

struct UploadFileSpec {
    std::string local_path;
    std::string remote_path;
    remote_service::TaskFileType file_type = remote_service::kFileUnknown;
};

struct DirectorySpec {
    std::string local_dir;
    std::string remote_dir;
    remote_service::TaskFileType file_type = remote_service::kFileUnknown;
};

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

bool ParseDirectorySpec(absl::string_view spec, DirectorySpec* out_spec) {
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

void ExpandDirectory(const DirectorySpec& dir_spec,
                     std::vector<UploadFileSpec>* files) {
    std::error_code ec;
    if (!fs::exists(dir_spec.local_dir, ec) ||
        !fs::is_directory(dir_spec.local_dir, ec)) {
        std::cerr << "Directory does not exist: " << dir_spec.local_dir
                  << std::endl;
        return;
    }

    fs::path base = fs::canonical(dir_spec.local_dir, ec);
    if (ec) {
        std::cerr << "Failed to canonicalize directory: " << dir_spec.local_dir
                  << std::endl;
        return;
    }

    for (const auto& entry :
         fs::recursive_directory_iterator(base,
                                           fs::directory_options::follow_directory_symlink)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        fs::path rel = fs::relative(entry.path(), base, ec);
        if (ec) {
            continue;
        }
        fs::path remote_root = dir_spec.remote_dir.empty()
                                   ? base.filename()
                                   : fs::path(dir_spec.remote_dir);
        fs::path remote = remote_root / rel;

        UploadFileSpec spec;
        spec.local_path = entry.path().string();
        spec.remote_path = remote.generic_string();
        spec.file_type = dir_spec.file_type;
        files->push_back(std::move(spec));
    }
}

} // namespace

class RemoteServiceClient {
  public:
    explicit RemoteServiceClient(std::shared_ptr<Channel> channel)
        : stub_(TaskManage::NewStub(channel)) {}

    void TaskSubmit(const std::vector<UploadFileSpec> &files,
                    ReqDeviceType device_type, const std::string &result_path);

  private:
    std::unique_ptr<TaskManage::Stub> stub_;
};

void RemoteServiceClient::TaskSubmit(const std::vector<UploadFileSpec> &files,
                                     ReqDeviceType device_type,
                                     const std::string &result_path) {
    if (files.empty()) {
        std::cerr << "No files specified for upload." << std::endl;
        return;
    }

    ClientContext context;
    TaskResult response;
    std::unique_ptr<ClientWriter<TaskRequest>> writer(
        stub_->TaskSubmission(&context, &response));

    std::vector<char> buffer(kChunkSize);
    bool first_request = true;
    int64_t total_bytes = 0;
    bool write_failed = false;

    timeval start, end;
    gettimeofday(&start, nullptr);

    for (const auto &file : files) {
        std::ifstream infile(file.local_path, std::ios::binary);
        if (!infile.is_open()) {
            std::cerr << "Cannot open file: " << file.local_path << std::endl;
            continue;
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

            TaskRequest request;
            request.set_task(buffer.data(), bytes_read);
            request.set_path(file.remote_path);
            request.set_file_type(file.file_type);
            request.set_file_start(first_chunk);
            if (first_chunk && declared_size >= 0) {
                request.set_file_size(declared_size);
            }
            if (infile.eof()) {
                request.set_file_end(true);
            }
            if (first_request) {
                request.set_device_type(device_type);
                request.set_ip_address("client");
                if (!result_path.empty()) {
                    request.set_result_path(result_path);
                }
                first_request = false;
            }

            total_bytes += bytes_read;
            if (!writer->Write(request)) {
                std::cerr
                    << "Failed to write stream (connection closed by server)."
                    << std::endl;
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
    Status status = writer->Finish();

    gettimeofday(&end, nullptr);
    const double elapsed = (end.tv_sec - start.tv_sec) +
                           (end.tv_usec - start.tv_usec) / 1'000'000.0;

    if (!status.ok()) {
        std::cerr << "RPC failed: " << status.error_message() << std::endl;
    }

    if (response.status() != remote_service::kSuccess) {
        std::cerr << "Server execution failed: " << response.message()
                  << std::endl;
    }

    std::cout << "Upload completed" << std::endl;
    std::cout << "Client sent: " << total_bytes << " bytes" << std::endl;
    std::cout << "Server received: " << response.length() << " bytes"
              << std::endl;
    std::cout << "Server message: " << response.message() << std::endl;
    if (!response.result().empty()) {
        std::cout << "Server output (" << response.result().size()
                  << " bytes):" << std::endl;
        std::cout << response.result() << std::endl;
    }
    std::cout << "Elapsed: " << elapsed << " seconds" << std::endl;
}

int main(int argc, char **argv) {
    absl::ParseCommandLine(argc, argv);
    const std::string target_str = absl::GetFlag(FLAGS_target);
    const std::string file_path = absl::GetFlag(FLAGS_file);
    const std::string save_path = absl::GetFlag(FLAGS_save_path);
  const std::string extra_files_flag = absl::GetFlag(FLAGS_extra_files);
  const std::string extra_dirs_flag = absl::GetFlag(FLAGS_extra_dirs);
  const int device = absl::GetFlag(FLAGS_device);
  const std::string result_path = absl::GetFlag(FLAGS_result_path);

    if (file_path.empty()) {
        std::cerr << "Use --file to select executable file." << std::endl;
        return 1;
    }

    std::vector<UploadFileSpec> files;
    files.push_back({file_path, save_path, remote_service::kFileExecutable});

  if (!extra_files_flag.empty()) {
        for (absl::string_view spec :
             absl::StrSplit(extra_files_flag, ',', absl::SkipWhitespace())) {
            if (spec.empty()) {
                continue;
  }

  if (!extra_dirs_flag.empty()) {
      for (absl::string_view spec :
           absl::StrSplit(extra_dirs_flag, ',', absl::SkipWhitespace())) {
          if (spec.empty()) {
              continue;
          }
          DirectorySpec dir_spec;
          if (!ParseDirectorySpec(spec, &dir_spec)) {
              std::cerr << "Invalid directory spec: " << spec << std::endl;
              continue;
          }
          ExpandDirectory(dir_spec, &files);
      }
  }
            UploadFileSpec extra;
            if (!ParseExtraFileSpec(spec, &extra)) {
                std::cerr << "Invalid extra file spec: " << spec << std::endl;
                continue;
            }
            files.push_back(extra);
        }
    }

    RemoteServiceClient client(
        grpc::CreateChannel(target_str, grpc::InsecureChannelCredentials()));

    for (int i = 0; i < kExecCount; ++i) {
        timeval start, end;
        gettimeofday(&start, nullptr);
        client.TaskSubmit(files, static_cast<ReqDeviceType>(device),
                          result_path);
        gettimeofday(&end, nullptr);
        double elapsed = (end.tv_sec - start.tv_sec) +
                         (end.tv_usec - start.tv_usec) / 1'000'000.0;
        std::cout << elapsed << std::endl;
    }

    return 0;
}
