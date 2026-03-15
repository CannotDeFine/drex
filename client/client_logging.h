#ifndef CLIENT_LOGGING_H
#define CLIENT_LOGGING_H

#include <string>

void InitializeClientLogging(const std::string &logger_name);
void LogClientInfo(const std::string &message);
void LogClientWarn(const std::string &message);
void LogClientError(const std::string &message);

#endif
