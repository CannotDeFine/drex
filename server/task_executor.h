#ifndef TASK_EXECUTE_H
#define TASK_EXECUTE_H

#include <string>

#include <grpcpp/grpcpp.h>
#include <remote_service.grpc.pb.h>

using remote_service::TaskConfig;
using remote_service::TaskResult;

class TaskExecutor {
  public:
    TaskExecutor(const TaskConfig &config);
    virtual ~TaskExecutor();

    virtual grpc::Status Execute(TaskResult *result) = 0;

  protected:
    grpc::Status RunTaskAndCapture(std::string *output) const;
    grpc::Status ReadResultFile(std::string *data) const;
    grpc::Status PopulateSuccessResult(const std::string &command_output, TaskResult *result) const;
    TaskConfig config_;
};

#endif
