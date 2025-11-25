#ifndef TASK_EXECUTE_H
#define TASK_EXECUTE_H

#include <string>

#include <grpcpp/grpcpp.h>
#include <remote_service.grpc.pb.h>

using remote_service::TaskRequest;
using remote_service::TaskResult;

class TaskExecutor {
 public:
  TaskExecutor(const TaskRequest& request);
  virtual ~TaskExecutor();

 virtual grpc::Status Execute(TaskResult* result) = 0;

 protected:
  grpc::Status RunTaskAndCapture(std::string* output) const;
  TaskRequest request_;
};

#endif
