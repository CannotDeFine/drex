
#ifndef TASK_EXECUTE_H
#define TASK_EXECUTE_H

#include <filesystem>
#include <string>

#include <grpcpp/grpcpp.h>
#include <remote_service.grpc.pb.h>

class TaskExecutor {
  public:
    TaskExecutor(const remote_service::TaskConfig &config, const std::filesystem::path &workspace_root,
                 grpc::ServerReaderWriter<remote_service::TaskResponse, remote_service::TaskRequest> *stream);

    grpc::Status Execute(remote_service::TaskResult *result) const;

  private:
    grpc::Status RunCommand(std::string *combined_output) const;
    grpc::Status WriteTerminalOutput(const std::string &output, std::filesystem::path *output_dir) const;
    grpc::Status CreateOutputArchive(const std::filesystem::path &output_dir, std::string *archive_data) const;
    void EmitLogChunk(const std::string &chunk) const;

    remote_service::TaskConfig config_;
    std::filesystem::path workspace_root_;
    grpc::ServerReaderWriter<remote_service::TaskResponse, remote_service::TaskRequest> *stream_;
};

#endif
