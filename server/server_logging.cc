#include "server_logging.h"

#include <mutex>
#include <vector>

#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace {

std::once_flag g_logger_once;
std::shared_ptr<spdlog::logger> g_logger;

} // namespace

void InitializeServerLogging(const std::filesystem::path &workspace_root) {
    std::call_once(g_logger_once, [&]() {
        std::vector<spdlog::sink_ptr> sinks;
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

        std::error_code ec;
        std::filesystem::create_directories(workspace_root, ec);
        if (!ec) {
            const auto log_path = (workspace_root / "server.log").string();
            sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_path, true));
        }

        g_logger = std::make_shared<spdlog::logger>("remote_server", sinks.begin(), sinks.end());
        g_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
        g_logger->set_level(spdlog::level::info);
        g_logger->flush_on(spdlog::level::warn);
        spdlog::set_default_logger(g_logger);
    });
}

std::shared_ptr<spdlog::logger> GetServerLogger() {
    return g_logger;
}

void LogServerInfo(const std::string &message) {
    if (g_logger != nullptr) {
        g_logger->info(message);
    }
}

void LogServerDebug(const std::string &message) {
    if (g_logger != nullptr) {
        g_logger->debug(message);
    }
}

void LogServerWarn(const std::string &message) {
    if (g_logger != nullptr) {
        g_logger->warn(message);
    }
}

void LogServerError(const std::string &message) {
    if (g_logger != nullptr) {
        g_logger->error(message);
    }
}
