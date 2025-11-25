#ifndef REMOTE_SERVICE_IMPLEMENT_H
#define REMOTE_SERVICE_IMPLEMENT_H

#include <grpcpp/grpcpp.h>

#include "remote_service.grpc.pb.h"
#include "remote_service.pb.h"

using grpc::ServerReader;
using remote_service::TaskManage;
using remote_service::TaskRequest;
using remote_service::TaskResult;

class RemoteServiceImplement final : public TaskManage::Service {
  public:
    grpc::Status TaskSubmission(grpc::ServerContext *context,
                                ServerReader<TaskRequest> *request,
                                TaskResult *result) override;
    ~RemoteServiceImplement() override = default;

  private:
    // Streams input files from the client and writes them to disk.
    grpc::Status DownloadFile(ServerReader<TaskRequest> *request,
                              TaskResult *result, TaskRequest *last_request);
};

#endif
