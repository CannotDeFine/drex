#include "npu_task.h"

grpc::Status NpuExecutor::Execute(TaskResult* result) {
  std::string output;
  grpc::Status status = RunTaskAndCapture(&output);
  if (!status.ok()) {
    result->set_status(remote_service::kFailed);
    result->set_message(status.error_message());
    result->clear_result();
    result->set_length(0);
    return status;
  }

  result->set_status(remote_service::kSuccess);
  result->set_result(output);
  result->set_length(static_cast<int64_t>(output.size()));
  result->set_message("Task executed successfully.");

  return grpc::Status::OK;
}
