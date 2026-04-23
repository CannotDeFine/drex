# DPU Time-Slice Test

This task is a DPA microbenchmark specialized for validating the `UP` policy time-slice effect in xsched.

It launches many short DPA kernels and counts how many kernels complete inside the same fixed measurement window.
Unlike the previous DMA benchmark, this test uses `doca_dpa_kernel_launch_update_add()` with a shared completion
event so that the result is more directly tied to scheduler launch budget.

## Build

```bash
cd /home/cdf/drex/application/dpu_ts
make build
```

## Local Run

Start xsched with the `UP` policy first:

```bash
/home/cdf/xsched/build/service/xserver UP 50000
```

Then run both roles:

```bash
cd /home/cdf/drex/application/dpu_ts
make run-base
```

Or run them separately:

```bash
bash ./run.sh fg
bash ./run.sh bg
```

## Important Env Vars

- `XSCHED_AUTO_XQUEUE_UTILIZATION`: `fg` defaults to `2`, `bg` defaults to `1`
- `XSCHED_AUTO_XQUEUE_TIMESLICE`: defaults to `100000`
- `DPA_TS_WARMUP_MS`: defaults to `300` for `*-token`, otherwise `1000`
- `DPA_TS_RUNTIME_MS`: defaults to `3000` for `*-token`, otherwise `8000`
- `DPA_TS_KERNEL_ITERS`: defaults to `2000`
- `DPA_TS_MAX_INFLIGHT`: defaults to `1` for `*-token`, otherwise `2`
- `DPA_TS_SKIP_BUILD`: set to `1` only when the binary is already built and you want to skip recompilation

## Output

Each role prints one terminal summary line:

```text
RESULT role=fg completed=... submitted=... runtime_s=... throughput=... kernels/s iters=... inflight=...
```

The same line is also written to:

- `results/fg.result.txt`
- `results/bg.result.txt`
