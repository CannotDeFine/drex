#ifndef CPU_TASK_H
#define CPU_TASK_H

#include "task_executor.h"

class CpuExecutor : public TaskExecutor {
  public:
    explicit CpuExecutor(const TaskConfig &config) : TaskExecutor(config) {}
    ~CpuExecutor() override = default;

    grpc::Status Execute(TaskResult *result) override;
};

#endif
