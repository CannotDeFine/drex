#include "dpu_task.h"

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

grpc::Status DpuExecutor::Execute(TaskResult* result) {
  const std::string file_name = request_.path();
  if (access(file_name.c_str(), F_OK) != 0) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "Error: File not found!");
  }

  if (access(file_name.c_str(), X_OK) != 0) {
    struct stat st;
    if (stat(file_name.c_str(), &st) != 0) {
      return grpc::Status(grpc::StatusCode::INTERNAL,
                          "Failed to stat task file!");
    }
    if (chmod(file_name.c_str(), st.st_mode | S_IXUSR) != 0) {
      return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                          "Error: No execute permission!");
    }
  }

  pid_t pid = fork();
  if (pid < 0) {
    return grpc::Status(grpc::StatusCode::INTERNAL, "Error: Fork failed!");
  } else if (pid == 0) {
    char* argv[] = {const_cast<char*>(file_name.c_str()), nullptr};
    execv(file_name.c_str(), argv);
    _exit(127);
  } else {
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
      return grpc::Status(grpc::StatusCode::INTERNAL,
                          "Failed to wait for child process!");
    }

    if (WIFEXITED(status)) {
      int exit_code = WEXITSTATUS(status);
      if (exit_code != 0) {
        return grpc::Status(
            grpc::StatusCode::INTERNAL,
            "Task failed with exit code: " + std::to_string(exit_code));
      }
    } else if (WIFSIGNALED(status)) {
      int sig = WTERMSIG(status);
      return grpc::Status(grpc::StatusCode::INTERNAL,
                          "Task killed by signal: " + std::to_string(sig));
    } else {
      return grpc::Status(grpc::StatusCode::INTERNAL, "Task ended abnormally");
    }
  }

  result->set_status(remote_service::kSuccess);
  result->set_result("Task executed successfully.");

  return grpc::Status::OK;
}
