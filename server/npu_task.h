#ifndef NPU_TASK_H
#define NPU_TASK_H

#include "task_executor.h"

class NpuExecutor : public TaskExecutor {
  public:
    explicit NpuExecutor(const TaskRequest &request) : TaskExecutor(request) {}
    ~NpuExecutor() override = default;

    grpc::Status Execute(TaskResult *result) override;
};

#endif