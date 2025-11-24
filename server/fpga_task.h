#ifndef FPGA_TASK_H
#define FPGA_TASK_H

#include "task_executor.h"

class FpgaExecutor : public TaskExecutor {
 public:
  explicit FpgaExecutor(const TaskRequest& request) : TaskExecutor(request) {}
  ~FpgaExecutor() override = default;

  grpc::Status Execute(TaskResult* result) override;
};

#endif