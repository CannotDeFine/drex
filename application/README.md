# Application Tests

## gpu_up

Start `xserver`:

```bash
cd /home/cdf/xsched
./output/bin/xserver UP 50000
```

Start `remote_server`:

```bash
cd /home/cdf/drex
./build/server/remote_server
```

Submit `fg` first and `bg` second.

## 2:1

```bash
cd /home/cdf/drex
./build/client/remote_client \
  --target=127.0.0.1:8063 \
  --src_dir=/home/cdf/drex/application/gpu_up \
  --workspace_subdir=gpu_up_fg \
  --command="chmod +x run.sh && GPU_UP_RESET_SYNC=1 ./run.sh fg 1 smoke" \
  --utilization=2
```

```bash
cd /home/cdf/drex
./build/client/remote_client \
  --target=127.0.0.1:8063 \
  --src_dir=/home/cdf/drex/application/gpu_up \
  --workspace_subdir=gpu_up_bg \
  --command="chmod +x run.sh && ./run.sh bg 1 smoke" \
  --utilization=1
```

## 2:0

```bash
cd /home/cdf/drex
./build/client/remote_client \
  --target=127.0.0.1:8063 \
  --src_dir=/home/cdf/drex/application/gpu_up \
  --workspace_subdir=gpu_up_fg \
  --command="chmod +x run.sh && GPU_UP_RESET_SYNC=1 ./run.sh fg 1 smoke" \
  --utilization=2
```

```bash
cd /home/cdf/drex
./build/client/remote_client \
  --target=127.0.0.1:8063 \
  --src_dir=/home/cdf/drex/application/gpu_up \
  --workspace_subdir=gpu_up_bg \
  --command="chmod +x run.sh && ./run.sh bg 1 smoke" \
  --utilization=0
```

## gpu_dynamic_up

Start `xserver` with PUP:

```bash
cd /home/cdf/xsched
./output/bin/xserver PUP 50000
```

Start `remote_server`:

```bash
cd /home/cdf/drex
./build/server/remote_server
```

Submit two long-running inference tasks with initial utilization `2:1`:

```bash
cd /home/cdf/drex
./build/client/remote_client \
  --target=127.0.0.1:8063 \
  --src_dir=/home/cdf/drex/application/gpu_dynamic_up \
  --workspace_subdir=gpu_dyn_fg \
  --command="chmod +x run.sh && ./run.sh 1 fg 180 10" \
  --utilization=2
```

```bash
cd /home/cdf/drex
./build/client/remote_client \
  --target=127.0.0.1:8063 \
  --src_dir=/home/cdf/drex/application/gpu_dynamic_up \
  --workspace_subdir=gpu_dyn_bg \
  --command="chmod +x run.sh && ./run.sh 1 bg 180 10" \
  --utilization=1
```

Update the running ratio to `3:1`:

```bash
cd /home/cdf/drex
./build/client/remote_client \
  --target=127.0.0.1:8063 \
  --workspace_subdir=gpu_dyn_fg \
  --update_utilization \
  --utilization=3
```

```bash
cd /home/cdf/drex
./build/client/remote_client \
  --target=127.0.0.1:8063 \
  --workspace_subdir=gpu_dyn_bg \
  --update_utilization \
  --utilization=1
```

Each task prints windowed throughput every 10 seconds. Compare the windows before and after the update.
