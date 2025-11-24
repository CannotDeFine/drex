#ifndef DPU_TASK_H
#define DPU_TASK_H

#include "task_executor.h"

class DpuExecutor : public TaskExecutor {
 public:
  explicit DpuExecutor(const TaskRequest& request) : TaskExecutor(request) {}
  ~DpuExecutor() override = default;

  grpc::Status Execute(TaskResult* result) override;
};

#endif