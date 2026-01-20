#ifndef REMOTE_SERVICE_IMPLEMENT_H
#define REMOTE_SERVICE_IMPLEMENT_H

#include <filesystem>
#include <string>

#include <grpcpp/grpcpp.h>

#include "remote_service.grpc.pb.h"
#include "remote_service.pb.h"

class WorkspaceLockGuard;

class RemoteServiceImplement final : public remote_service::TaskManage::Service {
  public:
    grpc::Status TaskSubmission(grpc::ServerContext *context,
                                grpc::ServerReaderWriter<remote_service::TaskResponse, remote_service::TaskRequest> *stream) override;
    ~RemoteServiceImplement() override = default;

  private:
    grpc::Status DownloadWorkspace(grpc::ServerContext *context,
                                   grpc::ServerReaderWriter<remote_service::TaskResponse, remote_service::TaskRequest> *stream,
                                   const std::filesystem::path &workspace_root, remote_service::TaskConfig *config_out,
                                   WorkspaceLockGuard *lock_guard);
};

#endif
