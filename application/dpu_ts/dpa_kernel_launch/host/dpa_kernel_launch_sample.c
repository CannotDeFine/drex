/* DPA UP time-slice microbenchmark: host-side driver for short kernel launches. */

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <pthread.h>
#include <signal.h>

#include <doca_error.h>
#include <doca_log.h>

#include "xsched/xqueue.h"
#include "dpa_common.h"

DOCA_LOG_REGISTER(KERNEL_LAUNCH::SAMPLE);

static volatile sig_atomic_t g_stop_requested = 0;

static void handle_stop_signal(int signo)
{
	(void)signo;
	g_stop_requested = 1;
}

struct signal_guard {
	bool installed;
	struct sigaction old_int;
	struct sigaction old_term;
};

static void install_stop_signal_handlers(struct signal_guard *guard)
{
	struct sigaction sa;
	sa.sa_handler = handle_stop_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;

	guard->installed = (sigaction(SIGINT, &sa, &guard->old_int) == 0) &&
			   (sigaction(SIGTERM, &sa, &guard->old_term) == 0);
}

static void restore_stop_signal_handlers(const struct signal_guard *guard)
{
	if (!guard->installed)
		return;
	(void)sigaction(SIGINT, &guard->old_int, NULL);
	(void)sigaction(SIGTERM, &guard->old_term, NULL);
}

extern doca_dpa_func_t hello_world;

static const uint32_t k_runtime_ms_default = 8000;
static const uint32_t k_warmup_ms_default = 1000;
static const uint32_t k_iters_default = 2000;
static const uint32_t k_max_inflight_default = 128;

static uint32_t getenv_u32_or_default(const char *name, uint32_t default_value)
{
	const char *value = getenv(name);
	char *end = NULL;
	unsigned long parsed;

	if (value == NULL || value[0] == '\0')
		return default_value;

	errno = 0;
	parsed = strtoul(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0' || parsed > UINT_MAX)
		return default_value;

	return (uint32_t)parsed;
}

static const char *getenv_str_or_default(const char *name, const char *default_value)
{
	const char *value = getenv(name);
	return (value == NULL || value[0] == '\0') ? default_value : value;
}

static uint64_t get_time_ns(void)
{
	struct timespec ts;
	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

struct completion_worker_ctx {
	struct doca_sync_event *comp_event;
	uint64_t submitted;
	uint64_t completed;
	bool accepting;
	doca_error_t worker_result;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
};

static doca_error_t submit_short_kernel(struct dpa_resources *resources,
					 struct doca_sync_event *comp_event,
					 uint32_t iters,
					 uint64_t kernel_id)
{
	return doca_dpa_kernel_launch_update_set(resources->doca_dpa,
						 NULL,
						 0,
						 comp_event,
						 kernel_id + 1,
						 1,
						 &hello_world,
						 (uint64_t)iters,
						 kernel_id,
						 0ULL);
}

static void *completion_worker(void *arg)
{
	struct completion_worker_ctx *ctx = (struct completion_worker_ctx *)arg;
	uint64_t waited = 0;

	for (;;) {
		pthread_mutex_lock(&ctx->mutex);
		while (waited >= ctx->submitted && ctx->accepting) {
			struct timespec ts;
			(void)clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_nsec += 200 * 1000000L;
			if (ts.tv_nsec >= 1000000000L) {
				ts.tv_sec += 1;
				ts.tv_nsec -= 1000000000L;
			}
			(void)pthread_cond_timedwait(&ctx->cond, &ctx->mutex, &ts);
		}
		if (waited >= ctx->submitted && !ctx->accepting) {
			pthread_mutex_unlock(&ctx->mutex);
			break;
		}
		pthread_mutex_unlock(&ctx->mutex);

		doca_error_t wait_res = doca_sync_event_wait_gt(ctx->comp_event, waited, SYNC_EVENT_MASK_FFS);
		if (wait_res != DOCA_SUCCESS) {
			pthread_mutex_lock(&ctx->mutex);
			ctx->worker_result = wait_res;
			ctx->accepting = false;
			pthread_cond_broadcast(&ctx->cond);
			pthread_mutex_unlock(&ctx->mutex);
			break;
		}

		waited += 1;

		pthread_mutex_lock(&ctx->mutex);
		ctx->completed = waited;
		pthread_cond_broadcast(&ctx->cond);
		pthread_mutex_unlock(&ctx->mutex);
	}

	return NULL;
}

static doca_error_t drive_for_duration(struct dpa_resources *resources,
					 struct completion_worker_ctx *worker_ctx,
					 struct doca_sync_event *comp_event,
					 uint64_t duration_ns,
					 uint32_t iters,
					 uint32_t max_inflight,
					 bool token_mode,
					 XQueueHandle *xqueue_handle)
{
	const uint64_t deadline_ns = get_time_ns() + duration_ns;
	doca_error_t result = DOCA_SUCCESS;

	while (!g_stop_requested) {
		pthread_mutex_lock(&worker_ctx->mutex);
		while ((worker_ctx->submitted - worker_ctx->completed) >= max_inflight &&
		       worker_ctx->worker_result == DOCA_SUCCESS &&
		       !g_stop_requested) {
			struct timespec ts;
			(void)clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_nsec += 200 * 1000000L;
			if (ts.tv_nsec >= 1000000000L) {
				ts.tv_sec += 1;
				ts.tv_nsec -= 1000000000L;
			}
			(void)pthread_cond_timedwait(&worker_ctx->cond, &worker_ctx->mutex, &ts);
		}
		if (worker_ctx->worker_result != DOCA_SUCCESS) {
			result = worker_ctx->worker_result;
			pthread_mutex_unlock(&worker_ctx->mutex);
			return result;
		}
		if (get_time_ns() >= deadline_ns) {
			pthread_mutex_unlock(&worker_ctx->mutex);
			break;
		}

		if (token_mode && xqueue_handle != NULL && *xqueue_handle != 0) {
			while (!g_stop_requested && get_time_ns() < deadline_ns) {
				XResult token_res = XQueueTryAcquireTokens(*xqueue_handle, 1);
				if (token_res == kXSchedSuccess)
					break;
				usleep(100);
			}
			if (g_stop_requested || get_time_ns() >= deadline_ns) {
				pthread_mutex_unlock(&worker_ctx->mutex);
				break;
			}
		}

		const uint64_t kernel_id = worker_ctx->submitted;
		worker_ctx->submitted += 1;
		pthread_cond_broadcast(&worker_ctx->cond);
		pthread_mutex_unlock(&worker_ctx->mutex);

		result = submit_short_kernel(resources, comp_event, iters, kernel_id);
		if (result != DOCA_SUCCESS)
			return result;

		if (token_mode && xqueue_handle != NULL && *xqueue_handle == 0) {
			XQueueHandle discovered = 0;
			if (XQueueGetByHwQueue((HwQueueHandle)resources->doca_dpa, &discovered) == kXSchedSuccess)
				*xqueue_handle = discovered;
		}
	}

	return DOCA_SUCCESS;
}

static doca_error_t drain_to_target(struct completion_worker_ctx *worker_ctx, uint64_t target)
{
	pthread_mutex_lock(&worker_ctx->mutex);
	while (worker_ctx->completed < target &&
	       worker_ctx->worker_result == DOCA_SUCCESS &&
	       !g_stop_requested) {
		struct timespec ts;
		(void)clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 200 * 1000000L;
		if (ts.tv_nsec >= 1000000000L) {
			ts.tv_sec += 1;
			ts.tv_nsec -= 1000000000L;
		}
		(void)pthread_cond_timedwait(&worker_ctx->cond, &worker_ctx->mutex, &ts);
	}
	doca_error_t result = worker_ctx->worker_result;
	pthread_mutex_unlock(&worker_ctx->mutex);
	return result;
}

static bool discover_xqueue_handle(struct dpa_resources *resources, XQueueHandle *xqueue_handle)
{
	uint64_t deadline_ns = get_time_ns() + 1000000000ULL;

	if (xqueue_handle == NULL)
		return false;

	while (get_time_ns() < deadline_ns) {
		XQueueHandle discovered = 0;
		if (XQueueGetByHwQueue((HwQueueHandle)resources->doca_dpa, &discovered) == kXSchedSuccess &&
		    discovered != 0) {
			*xqueue_handle = discovered;
			return true;
		}
		usleep(1000);
	}

	return false;
}

static void write_result_if_needed(const char *path, const char *line)
{
	if (path == NULL || path[0] == '\0')
		return;

	FILE *fp = fopen(path, "w");
	if (fp == NULL)
		return;
	(void)fprintf(fp, "%s\n", line);
	(void)fclose(fp);
}

doca_error_t kernel_launch(struct dpa_resources *resources)
{
	struct signal_guard sig_guard = {0};
	struct doca_sync_event *comp_event = NULL;
	struct completion_worker_ctx worker_ctx;
	pthread_t worker_thread;
	bool worker_started = false;
	bool worker_sync_ready = false;
	doca_error_t result = DOCA_SUCCESS;

	const char *role = getenv_str_or_default("DPA_TS_ROLE", "worker");
	const char *result_path = getenv_str_or_default("DPA_TS_OUTPUT_PATH", "");
	const uint32_t warmup_ms = getenv_u32_or_default("DPA_TS_WARMUP_MS", k_warmup_ms_default);
	const uint32_t runtime_ms = getenv_u32_or_default("DPA_TS_RUNTIME_MS", k_runtime_ms_default);
	const uint32_t iters = getenv_u32_or_default("DPA_TS_KERNEL_ITERS", k_iters_default);
	uint32_t max_inflight = getenv_u32_or_default("DPA_TS_MAX_INFLIGHT", k_max_inflight_default);
	const bool token_mode = getenv_u32_or_default("DPA_TS_TOKEN_MODE", 0) != 0;
	XQueueHandle xqueue_handle = 0;
	uint64_t measure_base_completed = 0;
	uint64_t measure_base_submitted = 0;
	uint64_t measure_completed = 0;
	uint64_t measure_submitted = 0;
	uint64_t measure_start_ns = 0;
	uint64_t measure_end_ns = 0;

	if (max_inflight == 0)
		max_inflight = 1;

	install_stop_signal_handlers(&sig_guard);
	g_stop_requested = 0;

	result = create_doca_dpa_completion_sync_event(resources->doca_dpa,
						       resources->doca_device,
						       &comp_event,
						       NULL);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	worker_ctx.comp_event = comp_event;
	worker_ctx.submitted = 0;
	worker_ctx.completed = 0;
	worker_ctx.accepting = true;
	worker_ctx.worker_result = DOCA_SUCCESS;
	if (pthread_mutex_init(&worker_ctx.mutex, NULL) != 0) {
		result = DOCA_ERROR_UNEXPECTED;
		goto cleanup;
	}
	worker_sync_ready = true;
	if (pthread_cond_init(&worker_ctx.cond, NULL) != 0) {
		result = DOCA_ERROR_UNEXPECTED;
		goto cleanup;
	}
	if (pthread_create(&worker_thread, NULL, completion_worker, &worker_ctx) != 0) {
		result = DOCA_ERROR_UNEXPECTED;
		goto cleanup;
	}
	worker_started = true;

	if (token_mode) {
		(void)discover_xqueue_handle(resources, &xqueue_handle);
	}

	if (warmup_ms != 0) {
		result = drive_for_duration(resources,
					    &worker_ctx,
					    comp_event,
					    (uint64_t)warmup_ms * 1000000ULL,
					    iters,
					    max_inflight,
					    token_mode,
					    &xqueue_handle);
		if (result != DOCA_SUCCESS)
			goto cleanup;

		result = drain_to_target(&worker_ctx, worker_ctx.submitted);
		if (result != DOCA_SUCCESS)
			goto cleanup;
	}

	pthread_mutex_lock(&worker_ctx.mutex);
	measure_base_completed = worker_ctx.completed;
	measure_base_submitted = worker_ctx.submitted;
	pthread_mutex_unlock(&worker_ctx.mutex);

	measure_start_ns = get_time_ns();
	result = drive_for_duration(resources,
				    &worker_ctx,
				    comp_event,
				    (uint64_t)runtime_ms * 1000000ULL,
				    iters,
				    max_inflight,
				    token_mode,
				    &xqueue_handle);
	measure_end_ns = get_time_ns();
	if (result != DOCA_SUCCESS)
		goto cleanup;

	pthread_mutex_lock(&worker_ctx.mutex);
	measure_completed = worker_ctx.completed;
	measure_submitted = worker_ctx.submitted;
	pthread_mutex_unlock(&worker_ctx.mutex);

	{
		const uint64_t completed_in_window = measure_completed - measure_base_completed;
		const uint64_t submitted_in_window = measure_submitted - measure_base_submitted;
		const double elapsed_s = (double)(measure_end_ns - measure_start_ns) / 1e9;
		const double throughput = elapsed_s > 0.0 ? (double)completed_in_window / elapsed_s : 0.0;
		char result_line[512];

		(void)snprintf(result_line,
			       sizeof(result_line),
			       "RESULT role=%s completed=%llu submitted=%llu runtime_s=%.6f throughput=%.2f kernels/s iters=%u inflight=%u xq=%s",
			       role,
			       (unsigned long long)completed_in_window,
			       (unsigned long long)submitted_in_window,
			       elapsed_s,
			       throughput,
			       iters,
			       max_inflight,
			       xqueue_handle != 0 ? "ready" : "missing");
		printf("%s\n", result_line);
		fflush(stdout);
		write_result_if_needed(result_path, result_line);
	}

cleanup:
	restore_stop_signal_handlers(&sig_guard);
	if (worker_sync_ready) {
		pthread_mutex_lock(&worker_ctx.mutex);
		worker_ctx.accepting = false;
		pthread_cond_broadcast(&worker_ctx.cond);
		pthread_mutex_unlock(&worker_ctx.mutex);
	}
	if (worker_started)
		(void)pthread_join(worker_thread, NULL);
	if (worker_sync_ready) {
		pthread_cond_destroy(&worker_ctx.cond);
		pthread_mutex_destroy(&worker_ctx.mutex);
	}
	if (comp_event != NULL)
		(void)doca_sync_event_destroy(comp_event);

	return result;
}
