# GPU UP Test

This directory contains a self-contained port of the CUDA Fig. 7 UP
(utilization-partition) throughput test.

What is included here:

- the `up_fg` and `up_bg` test programs
- the TensorRT helper code they depend on
- local CMake and Makefile build files
- local run scripts for direct local runs and remote-task runs

What is still required from the machine:

- CUDA
- TensorRT

## Model files

The test expects a model prefix, not a directory. By default the scripts use:

```bash
application/gpu_up/models/resnet152
```

That means the following files should exist:

```bash
application/gpu_up/models/resnet152.onnx
application/gpu_up/models/resnet152.engine
```

The `.engine` file is optional. If it does not exist, TensorRT will build it
from the `.onnx` file on first run.

You can override the model prefix with:

```bash
export GPU_UP_MODEL_PREFIX=/abs/path/to/resnet152
```

## Build

```bash
cd application/gpu_up
make
```

## Run Locally

Direct foreground/background run:

```bash
cd application/gpu_up
./scripts/run_base.sh
```

Optional arguments:

```bash
./scripts/run_base.sh <device_name> <batch_size>
```

## Run As Remote Tasks

This is the intended layout for the remote execution server: upload this
directory as the task workspace, then execute one role per task.

Foreground task:

```bash
chmod +x run.sh && ./run.sh fg
```

Background task:

```bash
chmod +x run.sh && ./run.sh bg
```

Optional arguments:

```bash
./run.sh <fg|bg> [batch_size] [device_name]
```

Notes for remote execution:

- the server can inject xsched-related environment variables before starting the
  task
- the task uses only files inside `application/gpu_up`
- put `resnet152.onnx` under `application/gpu_up/models` before uploading
- if you want to reset the shared sync state before starting a fresh pair, use:

```bash
GPU_UP_RESET_SYNC=1 ./run.sh fg
```

Results are written under:

```bash
application/gpu_up/results
```

Build outputs are generated under:

```bash
application/gpu_up/build
```
