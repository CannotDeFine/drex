#ifndef GPU_TASK_H
#define GPU_TASK_H

#include "task_executor.h"

class GpuExecutor : public TaskExecutor {
  public:
    explicit GpuExecutor(const TaskConfig &config) : TaskExecutor(config) {}
    ~GpuExecutor() override = default;

    grpc::Status Execute(TaskResult *result) override;
};

#endif
