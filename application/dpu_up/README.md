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

When running roles manually in separate terminals, start `fg` first.
`run.sh fg` resets the `ProcessSync` shared memory automatically.

Notes for remote execution:

- the server can inject xsched-related environment variables before starting the
  task
- the task builds using only files inside `application/dpu_up`
- `run.sh` uses the global scheduler by default, so start `xserver` first if
  your environment expects it

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
```

Both files store DMA task throughput in `tasks/s`.
