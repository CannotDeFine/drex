#ifndef SERVER_LOGGING_H
#define SERVER_LOGGING_H

#include <filesystem>
#include <string>

void InitializeServerLogging(const std::filesystem::path &workspace_root);
void LogServerInfo(const std::string &message);
void LogServerDebug(const std::string &message);
void LogServerWarn(const std::string &message);
void LogServerError(const std::string &message);

#endif
