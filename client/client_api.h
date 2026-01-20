#ifndef REMOTE_CLIENT_API_H
#define REMOTE_CLIENT_API_H
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "remote_service.grpc.pb.h"
#include "remote_service.pb.h"

struct UploadFileSpec {
    std::string local_path;
    std::string relative_path;
};

struct UploadStats {
    std::size_t file_count = 0;
    int64_t total_bytes = 0;
};

bool CollectDirectoryFiles(const std::string &root_dir, std::vector<UploadFileSpec> *files);
bool ComputeUploadStats(const std::vector<UploadFileSpec> &files, UploadStats *stats);
grpc::Status ValidateUploadFiles(const std::vector<UploadFileSpec> &files);

class RemoteServiceClient {
  public:
    explicit RemoteServiceClient(std::shared_ptr<grpc::Channel> channel);

    struct TaskSubmitReport {
        remote_service::TaskResult response;
        int64_t bytes_sent = 0;
        double elapsed_seconds = 0.0;
    };

    grpc::Status TaskSubmit(const std::vector<UploadFileSpec> &files, const std::string &workspace_subdir, const std::string &command,
                            TaskSubmitReport *report, const UploadStats *upload_stats = nullptr, bool enable_pty = false);

  private:
    std::unique_ptr<remote_service::TaskManage::Stub> stub_;
};

#endif // REMOTE_CLIENT_API_H
