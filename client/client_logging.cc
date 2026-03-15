#include "client_logging.h"

#include <memory>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace {

std::shared_ptr<spdlog::logger> g_logger;

} // namespace

void InitializeClientLogging(const std::string &logger_name) {
    if (g_logger != nullptr) {
        return;
    }

    auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    g_logger = std::make_shared<spdlog::logger>(logger_name, sink);
    g_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    g_logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(g_logger);
}

void LogClientInfo(const std::string &message) {
    if (g_logger != nullptr) {
        g_logger->info(message);
    }
}

void LogClientWarn(const std::string &message) {
    if (g_logger != nullptr) {
        g_logger->warn(message);
    }
}

void LogClientError(const std::string &message) {
    if (g_logger != nullptr) {
        g_logger->error(message);
    }
}
