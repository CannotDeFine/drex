#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdarg.h>

namespace gpu_up::utils {

namespace detail {

inline void LogMessage(const char *level, const char *fmt, ...) {
    std::fprintf(stderr, "[%s] ", level);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

} // namespace detail

} // namespace gpu_up::utils

#define LIKELY(expr) __builtin_expect(!!(expr), 1)
#define UNLIKELY(expr) __builtin_expect(!!(expr), 0)

#define INFO(fmt, ...) gpu_up::utils::detail::LogMessage("INFO", fmt, ##__VA_ARGS__)
#define WARN(fmt, ...) gpu_up::utils::detail::LogMessage("WARN", fmt, ##__VA_ARGS__)
#define ERRO(fmt, ...)                                                                                                           \
    do {                                                                                                                         \
        gpu_up::utils::detail::LogMessage("ERROR", fmt, ##__VA_ARGS__);                                                         \
        std::abort();                                                                                                            \
    } while (0)

#define ASSERT(cond, fmt, ...)                                                                                                  \
    do {                                                                                                                         \
        if (UNLIKELY(!(cond))) {                                                                                                 \
            gpu_up::utils::detail::LogMessage("ERROR", fmt, ##__VA_ARGS__);                                                     \
            std::abort();                                                                                                        \
        }                                                                                                                        \
    } while (0)

#define FLUSH_XLOG() std::fflush(stderr)

#define EXEC_TIME(unit, block)                                                                                                   \
    ([&]() -> int64_t {                                                                                                          \
        const auto _start = std::chrono::steady_clock::now();                                                                   \
        block;                                                                                                                   \
        const auto _end = std::chrono::steady_clock::now();                                                                     \
        return std::chrono::duration_cast<std::chrono::unit>(_end - _start).count();                                            \
    }())
