#ifndef REMOTE_CLIENT_API_H
#define REMOTE_CLIENT_API_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <absl/strings/string_view.h>
#include <grpcpp/grpcpp.h>

#include "remote_service.grpc.pb.h"
#include "remote_service.pb.h"

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

bool CollectExtraFiles(absl::string_view flag_value, std::vector<UploadFileSpec> *files);
bool CollectExtraDirectories(absl::string_view flag_value, std::vector<UploadFileSpec> *files);
grpc::Status ValidateUploadFiles(const std::vector<UploadFileSpec> &files);

class RemoteServiceClient {
  public:
    explicit RemoteServiceClient(std::shared_ptr<grpc::Channel> channel);

    struct TaskSubmitReport {
        remote_service::TaskResult response;
        int64_t bytes_sent = 0;
        double elapsed_seconds = 0.0;
    };

    grpc::Status TaskSubmit(const std::vector<UploadFileSpec> &files, remote_service::ReqDeviceType device_type,
                            const std::string &entry_path, const std::string &result_path, TaskSubmitReport *report);

  private:
    std::unique_ptr<remote_service::TaskManage::Stub> stub_;
};

#endif // REMOTE_CLIENT_API_H
