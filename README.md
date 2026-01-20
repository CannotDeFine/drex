
# rpc-work: Remote Execution RPC Toolkit (gRPC + bRPC)

`rpc-work` is a small toolchain for **"upload workspace → run on remote machine → fetch outputs"**.

It provides:

- `remote_server` (gRPC): accepts a streamed upload, runs a command on the server, and returns a `tar.gz` of the output directory.
- `remote_client` (gRPC): uploads a local directory, streams server logs to stdout, then extracts the returned output archive locally.
- `resource_client` (bRPC): talks to a resource controller to apply/query resources and prints node endpoints.
- `intergrated_client` (bRPC + gRPC): applies for resources via the controller, then submits the task to the returned node.

## Build prerequisites

This repo is built with CMake and links against system-installed dependencies:

- Protobuf (`protoc`)
- gRPC (C++), plus `grpc_cpp_plugin`
- Abseil (`absl`)
- bRPC (and its deps: gflags, leveldb, OpenSSL, etc.)
- System libs: Threads, OpenSSL, CURL

The CMake config defaults `CMAKE_PREFIX_PATH` to `/usr/local` (see [CMakeLists.txt](CMakeLists.txt)).

For example installation steps (protobuf / gRPC / bRPC), see [usage.md](usage.md).

## Build

From the repo root:

```bash
./compile.sh
```

Notes:

- `compile.sh` recreates `build/` each time.
- The build is `Release` with `-std=c++17`.

After build, binaries are in `build/`:

- `build/remote_server`
- `build/remote_client`
- `build/resource_client`
- `build/intergrated_client`

## Quick start

### 1) Start the server

On the server machine:

```bash
./build/remote_server --address=0.0.0.0 --port=8063
```

Flags (from source):

- `--address` (default: `0.0.0.0`)
- `--port` (default: `8063`)

Server-side workspace:

- The server writes under `rpc-work/workspace/` on the server host.
- It locks `workspace_subdir` to avoid concurrent collisions.
- It cleans up the uploaded workspace and output directory after the request finishes.

### 2) Submit a task with `remote_client`

On the client machine:

```bash
./build/remote_client \
  --target=SERVER_IP:8063 \
  --src_dir=/path/to/local/workspace \
  --workspace_subdir=uploaded_task \
  --command="bash -lc 'mkdir -p output && echo hello > output/hello.txt'"
```

Flags (from source):

- `--target` (default: `127.0.0.1:8063`): gRPC server endpoint
- `--src_dir` (default: `./task`): local directory to upload (must contain at least one file)
- `--workspace_subdir` (default: `uploaded_task`): subdirectory under server workspace
- `--command` (required): command executed on the server *inside* the uploaded workspace directory
- `--pty` (default: `false`): run the remote command under a PTY (helps when stdout is buffered; may slow very chatty output)

Outputs:

- By default, the server can archive `<workspace_subdir>/output/` and return it as a `tar.gz`.
- Some deployments disable output archiving and only stream terminal logs; in that mode, clients will report success without restoring an `output/` directory.

### 3) Query/apply resources with `resource_client`

`resource_client` uses a simple `--key=value` argument parser:

```bash
./build/resource_client \
  --controller=CONTROLLER_HOST:PORT \
  --type=GPU \
  --count=1
```

It prints the returned nodes as `IP` and `Port`.

### 4) Apply resources and submit a task with `intergrated_client`

`intergrated_client` supports **positional arguments** (legacy) or a **--key=value** style.

Positional arguments:

```bash
./build/intergrated_client \
  <controller_addr(host:port)> \
  <src_dir> \
  <workspace_subdir> \
  <command> \
  <local_output_dir> \
  <resource_type> \
  <resource_count> \
  [--mem=<mem_req>] [--cores=<core_req>] [--pty]
```

Flags style (defaults: `--count=1`, `--local_output_dir=.`):

```bash
./build/intergrated_client \
  --controller=<host:port> \
  --src_dir=<dir> \
  --workspace_subdir=<name> \
  --command=<cmd> \
  --type=<resource_type> \
  [--count=<n>] [--local_output_dir=<dir>] [--mem=<mem_req>] [--cores=<core_req>] [--pty]
```

Example:

```bash
./build/intergrated_client \
  controller_host:port \
  /path/to/local/workspace \
  uploaded_task \
  "bash -lc 'make -j && mkdir -p output && cp -r build output/'" \
  /tmp/rpc-work-output \
  GPU \
  1
```

Output behavior:

- `intergrated_client` saves the server archive as `server_output.tar.gz` under `<local_output_dir>`.
- It also extracts that archive locally using `tar` into `<local_output_dir>`.
  - If the server does not return an output archive, it will only stream terminal output and exit successfully.

## Protocols

- gRPC service: [proto/remote_service.proto](proto/remote_service.proto)
- Resource controller (bRPC): [proto/resource_control.proto](proto/resource_control.proto)

Protobuf notes are in [tools/proto.md](tools/proto.md).

## Repo layout

- `proto/`: protobuf / gRPC definitions
- `server/`: gRPC server implementation
- `client/`: gRPC client + bRPC clients
- `tools/`: documentation
- `workspace/`: server-side working directory root (created/used by the server)
- `application/`: demos and integration scripts

## Troubleshooting

- **CMake cannot find gRPC/Protobuf/absl**: ensure they are installed where CMake can find them (default search path is `/usr/local`).
- **`grpc_cpp_plugin` or `protoc` not found**: make sure they are in `PATH`.
- **Client extraction fails**: `remote_client` uses the system `tar` command to extract the returned archive (`tar -xzf - -C ...`).

