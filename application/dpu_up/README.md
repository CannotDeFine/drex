# DPU UP Test

This directory contains a self-contained DPU DMA throughput test modeled after
`application/gpu_up`.

What is included here:

- the `up_fg` and `up_bg` test programs
- the local DOCA DMA runtime they depend on
- local CMake and Makefile build files
- local run scripts for direct local runs and remote-task runs

What is still required from the machine:

- DOCA with `doca-common` and `doca-dma`

## Build

```bash
cd /home/cdf/drex/application/dpu_up
make build
```

## Run Locally

```bash
cd /home/cdf/drex/application/dpu_up
make run-base
```

## Run As Remote Tasks

Foreground task:

```bash
bash ./run.sh fg
```

Background task:

```bash
bash ./run.sh bg
```

For xsched utilization validation on DOCA, use the dedicated low-smoothing mode:

```bash
bash ./run.sh fg-util
bash ./run.sh bg-util
```

This validation mode is tailored to the DOCA block-level queue model used by
xsched. It switches the workload to:

- `DPU_UP_RUN_MODE=serial`
- `DPU_UP_MAX_INFLIGHT_TASKS=1`
- `DPU_UP_DMA_BUFFER_SIZE=4096`
- `DPU_UP_WARMUP_MS=200`
- `DPU_UP_WARMUP_CNT=10`

so that utilization effects are easier to observe in completed task counts.

If you want a scheduler-focused demo that makes the effect show up directly in
application-visible completed work, use the synthetic DOCA-command mode:

```bash
bash ./run.sh fg-demo
bash ./run.sh bg-demo
```

This mode uses xsched's DOCA queue path directly, but replaces real DMA work
with synthetic synchronizable commands whose cost is controlled by sleep time.
That makes completed command counts track scheduler budget much more closely
than the real DMA throughput benchmark.

When running roles manually in separate terminals, start `fg` first.
`run.sh fg` resets the `ProcessSync` shared memory automatically.

Notes for remote execution:

- the server can inject xsched-related environment variables before starting the
  task
- the task builds using only files inside `application/dpu_up`
- `run.sh` uses the global scheduler by default, so start `xserver` first if
  your environment expects it
- `run.sh` also defaults to `XSCHED_AUTO_XQUEUE_THRESHOLD=2` and
  `XSCHED_AUTO_XQUEUE_BATCH_SIZE=1` to reduce in-flight smoothing during UP
  policy experiments, and sets `XSCHED_AUTO_XQUEUE_TIMESLICE=100000` to make
  utilization weights easier to observe; override them explicitly if you want
  different queue launch depth or timeslice behavior

This task does not hardcode utilization in `run.sh`.
When launching through `remote_client` or `integrated_client`, pass it with:

```bash
--utilization=<0-100>
```

When running manually, export the environment variable before starting each
role:

```bash
export XSCHED_AUTO_XQUEUE_UTILIZATION=2
bash ./run.sh fg
```

Default workload knobs are fixed in code:

```cpp
#define MAX_INFLIGHT_TASKS 64
#define WARMUP_CNT 20
#define MEASURE_SEC 8
```

Results are written under:

```bash
/home/cdf/drex/application/dpu_up/results
```

Only these two files are kept and overwritten on each run:

```bash
/home/cdf/drex/application/dpu_up/results/up_fg.thpt
/home/cdf/drex/application/dpu_up/results/up_bg.thpt
/home/cdf/drex/application/dpu_up/results/up_fg.util.thpt
/home/cdf/drex/application/dpu_up/results/up_bg.util.thpt
/home/cdf/drex/application/dpu_up/results/up_fg.demo.txt
/home/cdf/drex/application/dpu_up/results/up_bg.demo.txt
```

The `.thpt` files store DMA task throughput in `tasks/s`.
The `.demo.txt` files store `completed_commands elapsed_seconds commands_per_second`.

To validate xsched utilization directly from scheduler-side budget summaries,
save `xserver` output to a log file and run:

```bash
bash ./scripts/check_up_summary.sh /path/to/xserver.log
```

The script reads the latest two `[UP_SUMMARY]` lines, compares observed
`budget_us` ratio with the logged `util` ratio, and prints `PASS` or `FAIL`.
