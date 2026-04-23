#include "runtime.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_dma.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_pe.h>

extern "C" {
#include <samples/common.h>
#include "pe_common.h"
}

DOCA_LOG_REGISTER(XSCHED::DPU_UP::RUNTIME);

namespace dpu_up
{

namespace
{

#define MAX_INFLIGHT_TASKS_DEFAULT 64
#define DMA_BUFFER_SIZE_DEFAULT 64

#define EXIT_ON_FAILURE(_expression_) \
    do { \
        doca_error_t _status_ = (_expression_); \
        if (_status_ != DOCA_SUCCESS) return _status_; \
    } while (0)

enum class RunMode {
    kBacklog,
    kSerial,
};

struct RuntimeConfig {
    size_t max_inflight_tasks = MAX_INFLIGHT_TASKS_DEFAULT;
    size_t dma_buffer_size = DMA_BUFFER_SIZE_DEFAULT;
    RunMode run_mode = RunMode::kBacklog;
};

struct RuntimeState {
    struct pe_sample_state_base base {};
    struct doca_dma *dma = nullptr;
    struct doca_ctx *dma_ctx = nullptr;
    RuntimeConfig config {};
    std::vector<bool> slot_in_use {};
    uint64_t submitted_tasks = 0;
    uint64_t completed_tasks = 0;
};

std::once_flag g_log_once;
std::once_flag g_runtime_once;

static uint8_t expected_value_for_slot(size_t slot)
{
    return static_cast<uint8_t>((slot % 251) + 1);
}

static size_t sanitize_positive_env(const char *name, size_t default_value)
{
    const int64_t parsed = GetEnvInt64OrDefault(name, static_cast<int64_t>(default_value));
    if (parsed <= 0) return default_value;
    return static_cast<size_t>(parsed);
}

static RunMode get_run_mode_from_env()
{
    const char *value = std::getenv("DPU_UP_RUN_MODE");
    if (value == nullptr || value[0] == '\0') return RunMode::kBacklog;
    if (std::strcmp(value, "serial") == 0) return RunMode::kSerial;
    return RunMode::kBacklog;
}

static RuntimeConfig LoadRuntimeConfig()
{
    RuntimeConfig config;
    config.max_inflight_tasks = sanitize_positive_env("DPU_UP_MAX_INFLIGHT_TASKS", MAX_INFLIGHT_TASKS_DEFAULT);
    config.dma_buffer_size = sanitize_positive_env("DPU_UP_DMA_BUFFER_SIZE", DMA_BUFFER_SIZE_DEFAULT);
    config.run_mode = get_run_mode_from_env();
    return config;
}

static void dma_memcpy_completed_callback(struct doca_dma_task_memcpy *dma_task,
                                          union doca_data task_user_data,
                                          union doca_data ctx_user_data)
{
    auto *state = static_cast<RuntimeState *>(ctx_user_data.ptr);
    const size_t slot = static_cast<size_t>(task_user_data.u64 - 1);
    const uint8_t expected_value = expected_value_for_slot(slot);

    state->base.num_completed_tasks++;
    state->completed_tasks++;
    if (slot < state->slot_in_use.size()) state->slot_in_use[slot] = false;

    (void)process_completed_dma_memcpy_task(dma_task, expected_value);
    (void)dma_task_free(dma_task);
}

static void dma_memcpy_error_callback(struct doca_dma_task_memcpy *dma_task,
                                      union doca_data task_user_data,
                                      union doca_data ctx_user_data)
{
    auto *state = static_cast<RuntimeState *>(ctx_user_data.ptr);
    const size_t slot = static_cast<size_t>(task_user_data.u64 - 1);
    struct doca_task *task = doca_dma_task_memcpy_as_task(dma_task);

    state->base.num_completed_tasks++;
    state->completed_tasks++;
    if (slot < state->slot_in_use.size()) state->slot_in_use[slot] = false;

    DOCA_LOG_ERR("DMA task failed with status %s", doca_error_get_descr(doca_task_get_status(task)));
    (void)dma_task_free(dma_task);
}

static doca_error_t create_dma(RuntimeState *state)
{
    union doca_data ctx_user_data = {0};

    EXIT_ON_FAILURE(doca_dma_create(state->base.device, &state->dma));
    state->dma_ctx = doca_dma_as_ctx(state->dma);
    EXIT_ON_FAILURE(doca_pe_connect_ctx(state->base.pe, state->dma_ctx));

    ctx_user_data.ptr = state;
    EXIT_ON_FAILURE(doca_ctx_set_user_data(state->dma_ctx, ctx_user_data));
    EXIT_ON_FAILURE(doca_dma_task_memcpy_set_conf(state->dma,
                                                  dma_memcpy_completed_callback,
                                                  dma_memcpy_error_callback,
                                                  static_cast<uint32_t>(state->config.max_inflight_tasks)));
    return DOCA_SUCCESS;
}

static doca_error_t submit_task_for_slot(RuntimeState *state, size_t slot)
{
    struct doca_buf *source = nullptr;
    struct doca_buf *destination = nullptr;
    struct doca_dma_task_memcpy *task = nullptr;
    union doca_data user_data = {0};

    const size_t dma_buffer_size = state->config.dma_buffer_size;
    uint8_t *source_addr = state->base.buffer + slot * 2 * dma_buffer_size;
    uint8_t *destination_addr = source_addr + dma_buffer_size;
    const uint8_t expected_value = expected_value_for_slot(slot);

    std::memset(source_addr, expected_value, dma_buffer_size);
    std::memset(destination_addr, 0, dma_buffer_size);

    EXIT_ON_FAILURE(doca_buf_inventory_buf_get_by_data(state->base.inventory,
                                                       state->base.mmap,
                                                       source_addr,
                                                       dma_buffer_size,
                                                       &source));
    EXIT_ON_FAILURE(doca_buf_inventory_buf_get_by_addr(state->base.inventory,
                                                       state->base.mmap,
                                                       destination_addr,
                                                       dma_buffer_size,
                                                       &destination));

    user_data.u64 = slot + 1;
    doca_error_t result = doca_dma_task_memcpy_alloc_init(state->dma, source, destination, user_data, &task);
    if (result != DOCA_SUCCESS) {
        (void)doca_buf_dec_refcount(source, nullptr);
        (void)doca_buf_dec_refcount(destination, nullptr);
        return result;
    }

    result = doca_task_submit(doca_dma_task_memcpy_as_task(task));
    if (result != DOCA_SUCCESS) {
        (void)dma_task_free(task);
        return result;
    }

    state->slot_in_use[slot] = true;
    state->submitted_tasks++;
    return DOCA_SUCCESS;
}

class RuntimeSession {
public:
    RuntimeSession()
    {
        Init();
    }

    ~RuntimeSession()
    {
        Cleanup();
    }

    uint64_t RunForDurationNs(uint64_t duration_ns)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!ready_) return 0;

        ResetWindowState();

        auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(duration_ns);
        const bool ok = (state_.config.run_mode == RunMode::kSerial)
            ? RunSerial(deadline)
            : RunBacklog(deadline);
        if (!ok) return 0;

        const uint64_t completed_before_drain = state_.completed_tasks;

        DrainSubmittedTasks();
        return completed_before_drain;
    }
    uint64_t RunForTaskCount(uint64_t task_count)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!ready_ || task_count == 0) return 0;

        ResetWindowState();

        uint64_t to_submit = task_count;
        while (to_submit > 0) {
            bool submitted_any = false;
            for (size_t slot = 0; slot < state_.slot_in_use.size() && to_submit > 0; ++slot) {
                if (state_.slot_in_use[slot]) continue;

                doca_error_t result = submit_task_for_slot(&state_, slot);
                if (result != DOCA_SUCCESS) {
                    DOCA_LOG_ERR("submit_task_for_slot failed: %s", doca_error_get_descr(result));
                    return 0;
                }
                submitted_any = true;
                --to_submit;
            }

            if (to_submit == 0) break;

            int progressed = doca_pe_progress(state_.base.pe);
            if (progressed == 0 && !submitted_any) {
                std::this_thread::yield();
            }
        }

        DrainSubmittedTasks();
        return state_.completed_tasks;
    }

private:
    bool RunBacklog(const std::chrono::steady_clock::time_point &deadline)
    {
        if (!FillToBacklog(deadline)) return false;

        while (std::chrono::steady_clock::now() < deadline) {
            int progressed = doca_pe_progress(state_.base.pe);
            if (progressed == 0) {
                std::this_thread::yield();
            }
            if (!FillToBacklog(deadline)) return false;
        }
        return true;
    }

    bool RunSerial(const std::chrono::steady_clock::time_point &deadline)
    {
        while (std::chrono::steady_clock::now() < deadline) {
            if (state_.slot_in_use.empty()) return false;

            while (state_.slot_in_use[0]) {
                int progressed = doca_pe_progress(state_.base.pe);
                if (progressed == 0) {
                    std::this_thread::yield();
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    return true;
                }
            }

            doca_error_t result = submit_task_for_slot(&state_, 0);
            if (result != DOCA_SUCCESS) {
                DOCA_LOG_ERR("submit_task_for_slot failed: %s", doca_error_get_descr(result));
                return false;
            }

            const uint64_t target_completed = state_.submitted_tasks;
            while (state_.completed_tasks < target_completed) {
                int progressed = doca_pe_progress(state_.base.pe);
                if (progressed == 0) {
                    std::this_thread::yield();
                }
                if (std::chrono::steady_clock::now() >= deadline) break;
            }
        }
        return true;
    }

    void DrainSubmittedTasks()
    {
        while (state_.completed_tasks < state_.submitted_tasks) {
            int progressed = doca_pe_progress(state_.base.pe);
            if (progressed == 0) {
                std::this_thread::yield();
            }
        }
    }

    void ResetWindowState()
    {
        state_.base.num_completed_tasks = 0;
        state_.submitted_tasks = 0;
        state_.completed_tasks = 0;
        std::fill(state_.slot_in_use.begin(), state_.slot_in_use.end(), false);
        state_.base.available_buffer = state_.base.buffer;
    }

    bool FillToBacklog(const std::chrono::steady_clock::time_point &deadline)
    {
        for (size_t slot = 0; slot < state_.slot_in_use.size(); ++slot) {
            if (std::chrono::steady_clock::now() >= deadline) break;
            if (state_.slot_in_use[slot]) continue;

            doca_error_t result = submit_task_for_slot(&state_, slot);
            if (result != DOCA_SUCCESS) {
                DOCA_LOG_ERR("submit_task_for_slot failed: %s", doca_error_get_descr(result));
                return false;
            }
        }
        return true;
    }

    void Init()
    {
        state_.config = LoadRuntimeConfig();
        state_.slot_in_use.assign(state_.config.max_inflight_tasks, false);

        state_.base.buffer_size =
            static_cast<size_t>(state_.config.dma_buffer_size * 2 * state_.config.max_inflight_tasks);
        state_.base.buf_inventory_size =
            static_cast<size_t>(state_.config.max_inflight_tasks * 2);

        doca_error_t result = allocate_buffer(&state_.base);
        if (result != DOCA_SUCCESS) goto fail;

        result = open_device(&state_.base);
        if (result != DOCA_SUCCESS) goto fail;

        result = create_mmap(&state_.base);
        if (result != DOCA_SUCCESS) goto fail;

        result = create_buf_inventory(&state_.base);
        if (result != DOCA_SUCCESS) goto fail;

        result = create_pe(&state_.base);
        if (result != DOCA_SUCCESS) goto fail;

        result = create_dma(&state_);
        if (result != DOCA_SUCCESS) goto fail;

        result = doca_ctx_start(state_.dma_ctx);
        if (result != DOCA_SUCCESS) goto fail;

        ready_ = true;
        return;

fail:
        DOCA_LOG_ERR("DMA runtime init failed: %s", doca_error_get_descr(result));
        Cleanup();
    }

    void Cleanup()
    {
        if (state_.dma_ctx != nullptr) {
            (void)doca_ctx_stop(state_.dma_ctx);
            state_.dma_ctx = nullptr;
        }

        if (state_.dma != nullptr) {
            (void)doca_dma_destroy(state_.dma);
            state_.dma = nullptr;
        }

        pe_sample_base_cleanup(&state_.base);
        ready_ = false;
    }

    std::mutex mtx_;
    bool ready_ = false;
    RuntimeState state_ {};
};

std::unique_ptr<RuntimeSession> g_runtime;

RuntimeSession *GetRuntime()
{
    std::call_once(g_runtime_once, []() {
        g_runtime = std::make_unique<RuntimeSession>();
    });
    return g_runtime.get();
}

} // namespace

void InitDocaLogging()
{
    std::call_once(g_log_once, []() {
        struct doca_log_backend *sdk_log = nullptr;
        doca_error_t result = doca_log_backend_create_standard();
        if (result != DOCA_SUCCESS) return;

        result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
        if (result != DOCA_SUCCESS) return;

        (void)doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
    });
}

uint64_t RunForDurationNs(uint64_t duration_ns)
{
    InitDocaLogging();
    auto *runtime = GetRuntime();
    return runtime != nullptr ? runtime->RunForDurationNs(duration_ns) : 0;
}

uint64_t RunForTaskCount(uint64_t task_count)
{
    InitDocaLogging();
    auto *runtime = GetRuntime();
    return runtime != nullptr ? runtime->RunForTaskCount(task_count) : 0;
}

void ShutdownRuntime()
{
    g_runtime.reset();
}

int64_t GetEnvInt64OrDefault(const char *name, int64_t default_value)
{
    const char *value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') return default_value;
    char *end = nullptr;
    long long parsed = std::strtoll(value, &end, 10);
    if (end == value || *end != '\0') return default_value;
    return parsed;
}

} // namespace dpu_up
