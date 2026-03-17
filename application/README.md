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
