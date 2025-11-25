#include "npu_task.h"

grpc::Status NpuExecutor::Execute(TaskResult *result) {
    std::string output;
    grpc::Status status = RunTaskAndCapture(&output);
    if (!status.ok()) {
        result->set_status(remote_service::kFailed);
        result->set_message(status.error_message());
        result->clear_result();
        result->set_length(0);
        return status;
    }

    status = PopulateSuccessResult(output, result);
    if (!status.ok()) {
        result->set_status(remote_service::kFailed);
        result->set_message(status.error_message());
        result->clear_result();
        result->set_length(0);
        return status;
    }

    return grpc::Status::OK;
}
