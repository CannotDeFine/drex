#ifndef GPU_TASK_H
#define GPU_TASK_H

#include "task_executor.h"

class GpuExecutor : public TaskExecutor {
 public:
  explicit GpuExecutor(const TaskRequest& request) : TaskExecutor(request) {}
  ~GpuExecutor() override = default;

  grpc::Status Execute(TaskResult* result) override;
};

#endif