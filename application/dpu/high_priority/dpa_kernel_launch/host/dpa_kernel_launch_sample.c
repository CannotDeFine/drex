/* DPA flow demo: host-side driver for kernel launch. */

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

/* Kernel entry */
extern doca_dpa_func_t hello_world;

/* Initial simple knobs: fixed workload & per-task timing prints. */
static const uint32_t k_m_tasks = 200000;
static const uint32_t k_n_iters_in_kernel = 3000000;
static const uint32_t k_sleep_min_ms = 30;
static const uint32_t k_sleep_max_ms = 50;
static const uint32_t k_max_inflight_default = 1;
static const uint32_t k_verbose = 0;
static uint64_t get_time_ns(void);

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

static uint64_t get_time_ns(void)
{
	struct timespec ts;
	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void sleep_ms_range(uint32_t min_ms, uint32_t max_ms)
{
	uint32_t sleep_ms;
	if (max_ms < min_ms)
		max_ms = min_ms;
	if (max_ms == 0)
		return;

	sleep_ms = min_ms;
	if (max_ms > min_ms)
		sleep_ms = min_ms + (uint32_t)(rand() % (int)(max_ms - min_ms + 1));

	(void)usleep((useconds_t)sleep_ms * 1000);
}

struct inflight_record {
	uint32_t task_id;
	uint64_t comp_val;
	uint64_t start_ns;
};

struct completion_worker_ctx {
	struct doca_sync_event *comp_event;
	struct inflight_record *records;
	uint32_t total_tasks;
	uint32_t sleep_min_ms;
	uint32_t sleep_max_ms;
	uint32_t max_inflight;
	uint32_t next_ready;
	uint32_t next_to_wait;
	uint32_t completed;
	bool accepting;
	doca_error_t worker_result;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
};

static void *completion_worker(void *arg)
{
	struct completion_worker_ctx *ctx = (struct completion_worker_ctx *)arg;

	for (;;) {
		pthread_mutex_lock(&ctx->mutex);
		while (ctx->next_to_wait == ctx->next_ready && ctx->accepting) {
			struct timespec ts;
			(void)clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_nsec += 200 * 1000000L;
			if (ts.tv_nsec >= 1000000000L) {
				ts.tv_sec += 1;
				ts.tv_nsec -= 1000000000L;
			}
			(void)pthread_cond_timedwait(&ctx->cond, &ctx->mutex, &ts);
		}
		if (ctx->next_to_wait == ctx->next_ready && !ctx->accepting) {
			pthread_mutex_unlock(&ctx->mutex);
			break;
		}
		uint32_t record_idx = ctx->next_to_wait++;
		pthread_mutex_unlock(&ctx->mutex);

		const struct inflight_record *rec = &ctx->records[record_idx];
		doca_error_t wait_res = doca_sync_event_wait_gt(ctx->comp_event,
				    rec->comp_val - 1,
				    SYNC_EVENT_MASK_FFS);
		if (wait_res != DOCA_SUCCESS) {
			pthread_mutex_lock(&ctx->mutex);
			ctx->worker_result = wait_res;
			ctx->accepting = false;
			pthread_cond_broadcast(&ctx->cond);
			pthread_mutex_unlock(&ctx->mutex);
			break;
		}

		uint64_t task_end_ns = get_time_ns();
		uint64_t task_ms = (task_end_ns - rec->start_ns) / 1000000ULL;
		printf("Task %u completed in %lld ms\n", rec->task_id, (long long)task_ms);

		pthread_mutex_lock(&ctx->mutex);
		ctx->completed++;
		bool should_sleep = ctx->completed < ctx->total_tasks;
		pthread_cond_broadcast(&ctx->cond);
		pthread_mutex_unlock(&ctx->mutex);

		if (should_sleep)
			sleep_ms_range(ctx->sleep_min_ms, ctx->sleep_max_ms);
	}

	return NULL;
}

/* Run M tasks; each task launches the DPA kernel once and waits for completion. */
doca_error_t kernel_launch(struct dpa_resources *resources)
{
	struct signal_guard sig_guard = {0};
	install_stop_signal_handlers(&sig_guard);
	g_stop_requested = 0;

	const uint32_t m_tasks = getenv_u32_or_default("DPA_DEMO_TASKS", k_m_tasks);
	const uint32_t n_iters_in_kernel = getenv_u32_or_default("DPA_DEMO_ITERS", k_n_iters_in_kernel);
	const uint32_t sleep_min_ms = getenv_u32_or_default("DPA_DEMO_SLEEP_MIN_MS", k_sleep_min_ms);
	const uint32_t sleep_max_ms = getenv_u32_or_default("DPA_DEMO_SLEEP_MAX_MS", k_sleep_max_ms);
	uint32_t max_inflight = getenv_u32_or_default("DPA_DEMO_INFLIGHT", k_max_inflight_default);
	const uint32_t verbose = k_verbose;
	const unsigned int num_dpa_threads = 1;
	doca_error_t result = DOCA_SUCCESS;
	uint32_t tasks_completed = 0;

	if (max_inflight == 0)
		max_inflight = 1;

	srand((unsigned int)time(NULL));

	struct doca_sync_event *wait_event = NULL;
	struct doca_sync_event *comp_event = NULL;
	struct inflight_record *records = NULL;
	struct completion_worker_ctx worker_ctx;
	bool worker_started = false;
	bool worker_sync_ready = false;
	pthread_t worker_thread;

	result = create_doca_dpa_wait_sync_event(resources->doca_dpa, resources->doca_device, &wait_event);
	if (result != DOCA_SUCCESS)
		goto cleanup;
	result = create_doca_dpa_completion_sync_event(resources->doca_dpa,
				   resources->doca_device,
				   &comp_event,
				   NULL);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	records = calloc(m_tasks, sizeof(*records));
	if (records == NULL) {
		result = DOCA_ERROR_NO_MEMORY;
		goto cleanup;
	}

	worker_ctx.comp_event = comp_event;
	worker_ctx.records = records;
	worker_ctx.total_tasks = m_tasks;
	worker_ctx.sleep_min_ms = sleep_min_ms;
	worker_ctx.sleep_max_ms = sleep_max_ms;
	worker_ctx.max_inflight = max_inflight;
	worker_ctx.next_ready = 0;
	worker_ctx.next_to_wait = 0;
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

	for (uint32_t task_id = 0; task_id < m_tasks; task_id++) {
		if (g_stop_requested)
			break;

		pthread_mutex_lock(&worker_ctx.mutex);
		while ((worker_ctx.next_ready - worker_ctx.completed) >= max_inflight &&
		       worker_ctx.worker_result == DOCA_SUCCESS &&
		       !g_stop_requested) {
			struct timespec ts;
			(void)clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_nsec += 200 * 1000000L;
			if (ts.tv_nsec >= 1000000000L) {
				ts.tv_sec += 1;
				ts.tv_nsec -= 1000000000L;
			}
			(void)pthread_cond_timedwait(&worker_ctx.cond, &worker_ctx.mutex, &ts);
		}
		if (worker_ctx.worker_result != DOCA_SUCCESS) {
			pthread_mutex_unlock(&worker_ctx.mutex);
			result = worker_ctx.worker_result;
			goto cleanup;
		}
		pthread_mutex_unlock(&worker_ctx.mutex);
		if (g_stop_requested)
			break;

		uint64_t task_start_ns = get_time_ns();
		uint64_t wait_thresh = 4;
		uint64_t comp_event_val = 10 + (uint64_t)task_id;

		result = doca_dpa_kernel_launch_update_set(resources->doca_dpa,
			   wait_event,
			   wait_thresh,
			   comp_event,
			   comp_event_val,
			   num_dpa_threads,
			   &hello_world,
			   (uint64_t)n_iters_in_kernel,
			   (uint64_t)task_id,
			   (uint64_t)verbose);
		if (result != DOCA_SUCCESS)
			goto cleanup;

		result = doca_sync_event_update_set(wait_event, wait_thresh + 1);
		if (result != DOCA_SUCCESS)
			goto cleanup;

		records[task_id].task_id = task_id;
		records[task_id].comp_val = comp_event_val;
		records[task_id].start_ns = task_start_ns;

		pthread_mutex_lock(&worker_ctx.mutex);
		worker_ctx.next_ready++;
		pthread_cond_broadcast(&worker_ctx.cond);
		pthread_mutex_unlock(&worker_ctx.mutex);
	}

	pthread_mutex_lock(&worker_ctx.mutex);
	worker_ctx.accepting = false;
	pthread_cond_broadcast(&worker_ctx.cond);
	pthread_mutex_unlock(&worker_ctx.mutex);

	goto cleanup;

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
	if ((worker_started || worker_sync_ready) &&
	    worker_ctx.worker_result != DOCA_SUCCESS &&
	    result == DOCA_SUCCESS)
		result = worker_ctx.worker_result;
	if (records != NULL)
		free(records);

destroy_events:
	if (comp_event != NULL)
		(void)doca_sync_event_destroy(comp_event);
	if (wait_event != NULL)
		(void)doca_sync_event_destroy(wait_event);

	return result;
}
