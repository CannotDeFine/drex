#include "task_runtime_control.h"

#include "server_logging.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>

namespace fs = std::filesystem;

namespace {

std::mutex g_task_mu;
std::unordered_map<std::string, pid_t> g_workspace_to_pgid;

bool IsNumericName(const std::string &name) {
    return !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

bool ReadProcessGroup(pid_t pid, pid_t *pgid) {
    if (pgid == nullptr) {
        return false;
    }

    std::ifstream stat_file(fs::path("/proc") / std::to_string(pid) / "stat");
    std::string stat;
    if (!std::getline(stat_file, stat)) {
        return false;
    }

    const size_t close_paren = stat.rfind(')');
    if (close_paren == std::string::npos || close_paren + 2 >= stat.size()) {
        return false;
    }

    std::istringstream fields(stat.substr(close_paren + 2));
    char state = '\0';
    long ppid = 0;
    long pgrp = 0;
    fields >> state >> ppid >> pgrp;
    if (!fields) {
        return false;
    }

    *pgid = static_cast<pid_t>(pgrp);
    return true;
}

std::vector<pid_t> FindPidsInProcessGroup(pid_t pgid) {
    std::vector<pid_t> pids;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator("/proc", ec)) {
        if (ec) {
            break;
        }
        const std::string name = entry.path().filename().string();
        if (!IsNumericName(name)) {
            continue;
        }

        const pid_t pid = static_cast<pid_t>(std::strtol(name.c_str(), nullptr, 10));
        pid_t process_group = -1;
        if (!ReadProcessGroup(pid, &process_group)) {
            continue;
        }
        if (process_group == pgid) {
            pids.push_back(pid);
        }
    }
    std::sort(pids.begin(), pids.end());
    return pids;
}

std::string GetXcliPath() {
    const char *env = std::getenv("XSCHED_XCLI");
    if (env != nullptr && env[0] != '\0') {
        return env;
    }
    return "/home/wyl/xsched/output/bin/xcli";
}

bool RunXcliHint(pid_t pid, int utilization, std::string *error) {
    const std::string xcli = GetXcliPath();
    if (!fs::exists(xcli)) {
        if (error != nullptr) {
            *error = "xcli not found: " + xcli;
        }
        return false;
    }

    const std::string pid_arg = std::to_string(static_cast<long long>(pid));
    const std::string util_arg = std::to_string(utilization);
    const pid_t child = fork();
    if (child < 0) {
        if (error != nullptr) {
            *error = std::string("fork failed: ") + std::strerror(errno);
        }
        return false;
    }
    if (child == 0) {
        execl(xcli.c_str(), "xcli", "hint", "--pid", pid_arg.c_str(), "-u", util_arg.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }

    int status = 0;
    if (waitpid(child, &status, 0) < 0) {
        if (error != nullptr) {
            *error = std::string("waitpid failed: ") + std::strerror(errno);
        }
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (error != nullptr) {
            *error = "xcli hint failed for pid " + pid_arg + " with status " + std::to_string(status);
        }
        return false;
    }
    return true;
}

} // namespace

void RegisterTaskProcessGroup(const std::string &workspace_subdir, pid_t pgid) {
    if (workspace_subdir.empty() || pgid <= 0) {
        return;
    }
    std::lock_guard<std::mutex> guard(g_task_mu);
    g_workspace_to_pgid[workspace_subdir] = pgid;
    LogServerDebug("registered task process group: workspace='" + workspace_subdir + "' pgid=" + std::to_string(pgid));
}

void UnregisterTaskProcessGroup(const std::string &workspace_subdir, pid_t pgid) {
    if (workspace_subdir.empty()) {
        return;
    }
    std::lock_guard<std::mutex> guard(g_task_mu);
    auto it = g_workspace_to_pgid.find(workspace_subdir);
    if (it != g_workspace_to_pgid.end() && (pgid <= 0 || it->second == pgid)) {
        g_workspace_to_pgid.erase(it);
        LogServerDebug("unregistered task process group: workspace='" + workspace_subdir + "'");
    }
}

UtilizationUpdateResult UpdateTaskProcessUtilization(const std::string &workspace_subdir, int utilization) {
    UtilizationUpdateResult result;
    if (workspace_subdir.empty()) {
        result.message = "workspace_subdir must not be empty";
        return result;
    }
    if (utilization < 0 || utilization > 100) {
        result.message = "utilization must be in [0, 100]";
        return result;
    }

    pid_t pgid = -1;
    {
        std::lock_guard<std::mutex> guard(g_task_mu);
        auto it = g_workspace_to_pgid.find(workspace_subdir);
        if (it == g_workspace_to_pgid.end()) {
            result.message = "no running task found for workspace_subdir: " + workspace_subdir;
            return result;
        }
        pgid = it->second;
    }

    std::vector<pid_t> pids = FindPidsInProcessGroup(pgid);
    if (pids.empty()) {
        result.message = "no live process found in task process group: " + std::to_string(pgid);
        return result;
    }

    std::vector<pid_t> updated;
    std::string last_error;
    for (pid_t pid : pids) {
        std::string error;
        if (RunXcliHint(pid, utilization, &error)) {
            updated.push_back(pid);
        } else {
            last_error = error;
        }
    }

    if (updated.empty()) {
        result.message = last_error.empty() ? "failed to update utilization for task process group" : last_error;
        return result;
    }

    result.success = true;
    result.pids = std::move(updated);
    result.message = "utilization updated for workspace_subdir='" + workspace_subdir + "'";
    return result;
}
