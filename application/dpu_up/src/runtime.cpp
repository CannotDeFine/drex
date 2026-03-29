#include "runtime.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

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

#define MAX_INFLIGHT_TASKS 64
#define DMA_BUFFER_SIZE 64
#define BUFFER_SIZE (DMA_BUFFER_SIZE * 2 * MAX_INFLIGHT_TASKS)
#define BUF_INVENTORY_SIZE (MAX_INFLIGHT_TASKS * 2)

#define EXIT_ON_FAILURE(_expression_) \
    do { \
        doca_error_t _status_ = (_expression_); \
        if (_status_ != DOCA_SUCCESS) return _status_; \
    } while (0)

struct RuntimeState {
    struct pe_sample_state_base base {};
    struct doca_dma *dma = nullptr;
    struct doca_ctx *dma_ctx = nullptr;
    std::array<bool, MAX_INFLIGHT_TASKS> slot_in_use {};
    uint64_t submitted_tasks = 0;
    uint64_t completed_tasks = 0;
};

std::once_flag g_log_once;
std::once_flag g_runtime_once;

static uint8_t expected_value_for_slot(size_t slot)
{
    return static_cast<uint8_t>((slot % 251) + 1);
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
                                                  MAX_INFLIGHT_TASKS));
    return DOCA_SUCCESS;
}

static doca_error_t submit_task_for_slot(RuntimeState *state, size_t slot)
{
    struct doca_buf *source = nullptr;
    struct doca_buf *destination = nullptr;
    struct doca_dma_task_memcpy *task = nullptr;
    union doca_data user_data = {0};

    uint8_t *source_addr = state->base.buffer + slot * 2 * DMA_BUFFER_SIZE;
    uint8_t *destination_addr = source_addr + DMA_BUFFER_SIZE;
    const uint8_t expected_value = expected_value_for_slot(slot);

    std::memset(source_addr, expected_value, DMA_BUFFER_SIZE);
    std::memset(destination_addr, 0, DMA_BUFFER_SIZE);

    EXIT_ON_FAILURE(doca_buf_inventory_buf_get_by_data(state->base.inventory,
                                                       state->base.mmap,
                                                       source_addr,
                                                       DMA_BUFFER_SIZE,
                                                       &source));
    EXIT_ON_FAILURE(doca_buf_inventory_buf_get_by_addr(state->base.inventory,
                                                       state->base.mmap,
                                                       destination_addr,
                                                       DMA_BUFFER_SIZE,
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
        if (!FillToBacklog(deadline)) return 0;

        while (std::chrono::steady_clock::now() < deadline) {
            int progressed = doca_pe_progress(state_.base.pe);
            if (progressed == 0) {
                std::this_thread::yield();
            }
            if (!FillToBacklog(deadline)) return 0;
        }

        while (state_.completed_tasks < state_.submitted_tasks) {
            int progressed = doca_pe_progress(state_.base.pe);
            if (progressed == 0) {
                std::this_thread::yield();
            }
        }

        return state_.completed_tasks;
    }

private:
    void ResetWindowState()
    {
        state_.base.num_completed_tasks = 0;
        state_.submitted_tasks = 0;
        state_.completed_tasks = 0;
        state_.slot_in_use.fill(false);
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
        state_.base.buffer_size = BUFFER_SIZE;
        state_.base.buf_inventory_size = BUF_INVENTORY_SIZE;

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
