#ifndef TASK_EXECUTE_H
#define TASK_EXECUTE_H

#include <remote_service.grpc.pb.h>
#include <grpcpp/grpcpp.h>

using remote_service::TaskRequest;
using remote_service::TaskResult;

class TaskExecutor {
public:
    TaskExecutor(const TaskRequest& request);
    virtual ~TaskExecutor();

    virtual grpc::Status Execute(TaskResult* result) = 0;
protected:
    TaskRequest request_;
};

#endif