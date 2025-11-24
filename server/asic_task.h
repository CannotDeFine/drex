#ifndef ASIC_TASK_H
#define ASIC_TASK_H

#include "task_executor.h"

class OfaExecutor : public TaskExecutor {
 public:
  explicit OfaExecutor(const TaskRequest& request) : TaskExecutor(request) {}
  ~OfaExecutor() override = default;

  grpc::Status Execute(TaskResult* result) override;
};

#endif