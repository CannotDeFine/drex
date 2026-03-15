# Usage

This document describes the current runtime behavior of `rpc-work` and the supported command-line entrypoints.

## Build Prerequisites

Minimum supported toolchain:

- CMake `>= 3.16`
- A C++17-capable compiler
- OpenSSL development files
- gflags development files
- leveldb development files

Verified environment:

- CMake `3.22.1`
- GCC / G++ `11.4.0`
- OpenSSL `3.0.2`

Bundled dependency versions:

- Protobuf `3.25.7`
- gRPC `1.74.0`
- bRPC `1.14.1`
- spdlog `1.17.0`

Install the required system packages first:

```bash
sudo apt install -y build-essential cmake libssl-dev libleveldb-dev libgflags-dev
```

Then build the vendored dependencies and the project itself:

```bash
git submodule update --init --recursive
./scripts/build_deps.sh
./compile.sh
```

If you need a custom third-party prefix, use the same `RPC_THIRD_PARTY_PREFIX` for both steps:

```bash
RPC_THIRD_PARTY_PREFIX=/path/to/prefix ./scripts/build_deps.sh
RPC_THIRD_PARTY_PREFIX=/path/to/prefix ./compile.sh
```

## Start the Server

Run the server from the repository root:

```bash
./build/remote_server --address=0.0.0.0 --port=8063
```

Available flags:

- `--address`
  Listening address. Default: `0.0.0.0`
- `--port`
  Listening port. Default: `8063`

Behavior:

- Uploaded workspaces are materialized under `workspace/`.
- The server streams command output back to the client.
- The server logs its own runtime events to the terminal and to `workspace/server.log`.

## Run `remote_client`

Use `remote_client` to upload a local directory and run a command on the remote server.

```bash
./build/remote_client \
  --target=127.0.0.1:8063 \
  --src_dir=/path/to/workspace \
  --workspace_subdir=my_task \
  --command="chmod +x run.sh && ./run.sh"
```

Available flags:

- `--target`
  gRPC endpoint of `remote_server`. Default: `127.0.0.1:8063`
- `--src_dir`
  Local workspace directory to upload
- `--workspace_subdir`
  Remote subdirectory under `workspace/`
- `--command`
  Command executed inside the uploaded workspace
- `--pty`
  Enable PTY mode explicitly

Notes:

- The default mode is non-PTY.
- The client prints its own progress through structured logs.
- Server output is shown with a `[From server]` prefix when applicable.
- Local `Ctrl+C` cancels the remote gRPC request and asks the server to stop the task.

## Run `resource_client`

Use `resource_client` to request resources from the controller and print the assigned nodes.

```bash
./build/resource_client \
  --controller=controller_host:port \
  --type=GPU \
  --count=1
```

Supported flags:

- `--controller`
- `--type`
- `--count`
- `--mem`
- `--cores`

## Run `integrated_client`

Use `integrated_client` when you want to allocate resources first and then submit a task to the assigned node.

Positional form:

```bash
./build/integrated_client \
  <controller_addr> \
  <src_dir> \
  <workspace_subdir> \
  <command> \
  <local_output_dir> \
  <resource_type> \
  <resource_count> \
  [--mem=<mem_req>] [--cores=<core_req>] [--pty]
```

Flag form:

```bash
./build/integrated_client \
  --controller=<host:port> \
  --src_dir=<dir> \
  --workspace_subdir=<name> \
  --command=<cmd> \
  --type=<resource_type> \
  [--count=<n>] [--local_output_dir=<dir>] [--mem=<mem_req>] [--cores=<core_req>] [--pty]
```

Example:

```bash
./build/integrated_client \
  --controller=controller_host:port \
  --src_dir=/path/to/workspace \
  --workspace_subdir=my_task \
  --command="chmod +x run.sh && ./run.sh" \
  --type=GPU \
  --count=1
```

Notes:

- `integrated_client` shares the same remote execution behavior as `remote_client`.
- `local_output_dir` is still accepted by the current parser for compatibility, but the current execution flow does not restore output archives.

## Current Execution Model

The current project behavior is:

- Upload workspace
- Execute command remotely
- Stream terminal output back to the client

It does not currently:

- restore output archives on the client
- rely on archive extraction as part of the normal flow

## Troubleshooting

- If server output appears in bursts, restart `remote_server` after rebuilding and keep using the default non-PTY mode unless PTY is required.
- If a task must be cancelled, use `Ctrl+C` on the client side and allow the server a short moment to terminate the remote process group.
- If builds fail due to missing dependencies, rebuild the private prefix with `./scripts/build_deps.sh`.
