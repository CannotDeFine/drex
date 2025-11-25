#include "task_executor.h"

#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

TaskExecutor::TaskExecutor(const TaskRequest& request) : request_(request) {}
TaskExecutor::~TaskExecutor() = default;

grpc::Status TaskExecutor::RunTaskAndCapture(std::string* output) const {
  if (output == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "Output container must not be null!");
  }
  output->clear();

  const std::string file_name = request_.path();
  if (file_name.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "Task path is empty!");
  }

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

  int pipefd[2];
  if (pipe(pipefd) != 0) {
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        std::string("Failed to create pipe: ") +
                            std::strerror(errno));
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        std::string("Fork failed: ") + std::strerror(errno));
  } else if (pid == 0) {
    close(pipefd[0]);
    if (dup2(pipefd[1], STDOUT_FILENO) < 0 ||
        dup2(pipefd[1], STDERR_FILENO) < 0) {
      _exit(127);
    }
    close(pipefd[1]);
    char* argv[] = {const_cast<char*>(file_name.c_str()), nullptr};
    execv(file_name.c_str(), argv);
    _exit(127);
  }

  close(pipefd[1]);
  char buffer[4096];
  ssize_t bytes_read = 0;
  while ((bytes_read = read(pipefd[0], buffer, sizeof(buffer))) > 0) {
    output->append(buffer, static_cast<size_t>(bytes_read));
  }
  close(pipefd[0]);

  if (bytes_read < 0) {
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        std::string("Failed to read task output: ") +
                            std::strerror(errno));
  }

  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        "Failed to wait for child process!");
  }

  if (WIFEXITED(status)) {
    const int exit_code = WEXITSTATUS(status);
    if (exit_code != 0) {
      return grpc::Status(grpc::StatusCode::INTERNAL,
                          "Task failed with exit code: " +
                              std::to_string(exit_code));
    }
  } else if (WIFSIGNALED(status)) {
    const int sig = WTERMSIG(status);
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        "Task killed by signal: " + std::to_string(sig));
  } else {
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        "Task ended abnormally!");
  }

  return grpc::Status::OK;
}
