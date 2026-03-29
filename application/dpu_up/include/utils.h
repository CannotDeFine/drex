#pragma once

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <errno.h>
#include <pthread.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace dpu_up::utils {

namespace detail {

struct SharedSyncState {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int count;
    int initialized;
};

inline long GetThreadId() {
    static const thread_local long tid = syscall(SYS_gettid);
    return tid;
}

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
    const auto now = std::chrono::system_clock::now();
    const auto now_tt = std::chrono::system_clock::to_time_t(now);
    const auto now_tm = std::localtime(&now_tt);
    const auto now_us =
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count() % 1000000;

    std::fprintf(stderr,
                 "[%s @ T%ld @ %02d:%02d:%02d.%06ld] ",
                 level,
                 GetThreadId(),
                 now_tm->tm_hour,
                 now_tm->tm_min,
                 now_tm->tm_sec,
                 now_us);

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

} // namespace dpu_up::utils

#define XINFO(fmt, ...) dpu_up::utils::detail::LogMessage("INFO", fmt, ##__VA_ARGS__)
#define XWARN(fmt, ...) dpu_up::utils::detail::LogMessage("WARN", fmt, ##__VA_ARGS__)
#define XERRO(fmt, ...)                                                                                                         \
    do {                                                                                                                        \
        dpu_up::utils::detail::LogMessage("ERROR", fmt, ##__VA_ARGS__);                                                        \
        std::abort();                                                                                                           \
    } while (0)
