#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <stdarg.h>

#include <errno.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <pthread.h>

namespace gpu_up::utils {

namespace detail {

struct SharedSyncState {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int count;
    int initialized;
};

inline SharedSyncState *AcquireSharedSyncState() {
    constexpr key_t kSyncKey = 0xbeef;
    int shmid = shmget(kSyncKey, sizeof(SharedSyncState), IPC_CREAT | 0666);
    if (shmid < 0) {
        throw std::runtime_error("shmget failed");
    }

    void *addr = shmat(shmid, nullptr, 0);
    if (addr == (void *)-1) {
        throw std::runtime_error("shmat failed");
    }

    auto *state = static_cast<SharedSyncState *>(addr);
    if (__sync_bool_compare_and_swap(&state->initialized, 0, 1)) {
        pthread_mutexattr_t mutex_attr;
        pthread_condattr_t cond_attr;
        pthread_mutexattr_init(&mutex_attr);
        pthread_condattr_init(&cond_attr);
        pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
        pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED);
        pthread_mutex_init(&state->mutex, &mutex_attr);
        pthread_cond_init(&state->cond, &cond_attr);
        state->count = 0;
        pthread_mutexattr_destroy(&mutex_attr);
        pthread_condattr_destroy(&cond_attr);
    }
    return state;
}

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

class ProcessSync {
  public:
    ProcessSync() : state_(detail::AcquireSharedSyncState()) {}

    void Sync(int target, const char *message) {
        pthread_mutex_lock(&state_->mutex);
        ++state_->count;
        if (message != nullptr) {
            detail::LogMessage("INFO", "%s (sync=%d)", message, state_->count);
        }
        pthread_cond_broadcast(&state_->cond);
        while (state_->count < target) {
            pthread_cond_wait(&state_->cond, &state_->mutex);
        }
        pthread_mutex_unlock(&state_->mutex);
    }

    int GetCnt() const {
        pthread_mutex_lock(&state_->mutex);
        const int value = state_->count;
        pthread_mutex_unlock(&state_->mutex);
        return value;
    }

  private:
    detail::SharedSyncState *state_ = nullptr;
};

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
