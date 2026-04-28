#ifndef TASK_RUNTIME_CONTROL_H
#define TASK_RUNTIME_CONTROL_H

#include <string>
#include <vector>

#include <sys/types.h>

struct UtilizationUpdateResult {
    bool success = false;
    std::string message;
    std::vector<pid_t> pids;
};

void RegisterTaskProcessGroup(const std::string &workspace_subdir, pid_t pgid);
void UnregisterTaskProcessGroup(const std::string &workspace_subdir, pid_t pgid);
UtilizationUpdateResult UpdateTaskProcessUtilization(const std::string &workspace_subdir, int utilization);

#endif // TASK_RUNTIME_CONTROL_H
