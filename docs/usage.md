# Usage

This document covers build requirements and the main command-line entrypoints in
`DReX`.

## Requirements

System dependencies:

* CMake `>= 3.16`
* A C++17-capable compiler
* OpenSSL development files
* gflags development files
* leveldb development files

Example packages on Ubuntu:

```bash
sudo apt install -y build-essential cmake libssl-dev libleveldb-dev libgflags-dev
```

Verified environment:

* CMake `3.22.1`
* GCC / G++ `11.4.0`
* OpenSSL `3.0.2`

Bundled dependency versions:

* Protobuf `3.25.7`
* gRPC `1.74.0`
* bRPC `1.14.1`
* spdlog `1.17.0`

## Build

Build third-party dependencies first, then build the project:

```bash
git submodule update --init --recursive
./scripts/build_deps.sh
./compile.sh
```

If you want to use a custom third-party install prefix, use the same
`RPC_THIRD_PARTY_PREFIX` value for both commands:

```bash
RPC_THIRD_PARTY_PREFIX=/path/to/prefix ./scripts/build_deps.sh
RPC_THIRD_PARTY_PREFIX=/path/to/prefix ./compile.sh
```

Build outputs:

* `build/server/remote_server`
* `build/client/remote_client`
* `build/client/resource_client`
* `build/client/integrated_client`

## Components

`remote_server`

* Receives uploaded workspaces
* Runs commands on the remote node
* Streams command output back to the client

`remote_client`

* Uploads a local directory to `remote_server`
* Starts a command in the uploaded workspace

`resource_client`

* Talks to the controller through bRPC
* Requests resources and prints assigned nodes

`integrated_client`

* Requests resources first
* Then runs the same remote execution flow as `remote_client`

## remote_server

Start the server from the repository root:

```bash
./build/server/remote_server --address=0.0.0.0 --port=8063
```

Flags:

* `--address`: listening address, default `0.0.0.0`
* `--port`: listening port, default `8063`

Notes:

* Uploaded workspaces are created under `workspace/`
* Server logs are written to the terminal and `workspace/server.log`

## remote_client

Use `remote_client` to upload a local directory and run a command on the remote
server.

Example:

```bash
./build/client/remote_client \
  --target=127.0.0.1:8063 \
  --src_dir=/path/to/workspace \
  --workspace_subdir=my_task \
  --command="chmod +x run.sh && ./run.sh"
```

Main flags:

* `--target`: server address, default `127.0.0.1:8063`
* `--src_dir`: local directory to upload
* `--workspace_subdir`: subdirectory created under `workspace/`, default `uploaded_task`
* `--command`: command to run inside the uploaded workspace
* `--pty`: run the command under a PTY, default off
* `--utilization`: xsched utilization hint in `[0, 100]`, default disabled with `-1`
* `--update_utilization`: update utilization for a running task identified by `--workspace_subdir`


Runtime utilization update example:

```bash
./build/client/remote_client \
  --target=127.0.0.1:8063 \
  --workspace_subdir=my_task \
  --update_utilization \
  --utilization=3
```

Notes:

* Non-PTY mode is the default and is usually more reliable for batch jobs
* `Ctrl+C` on the client cancels the request and asks the server to stop the task
* Server output is streamed back to the client terminal

## resource_client

Use `resource_client` to request resources from the controller and print the
assigned nodes.

Example:

```bash
./build/client/resource_client \
  --controller=controller_host:port \
  --type=GPU \
  --count=1
```

Supported flags:

* `--controller`: controller address in `host:port` form
* `--type`: resource type
* `--count`: number of resources to request
* `--mem`: optional memory requirement
* `--cores`: optional core requirement

This client only supports the explicit flag form.

## integrated_client

Use `integrated_client` when you want to request resources and submit a remote
task in one command.

Flag form:

```bash
./build/client/integrated_client \
  --controller=controller_host:port \
  --src_dir=/path/to/workspace \
  --workspace_subdir=my_task \
  --command="chmod +x run.sh && ./run.sh" \
  --type=GPU \
  --count=1
```

Positional form:

```bash
./build/client/integrated_client \
  <controller_addr> \
  <src_dir> \
  <workspace_subdir> \
  <command> \
  <local_output_dir> \
  <resource_type> \
  <resource_count>
```

Supported flags:

* `--controller`: controller address
* `--src_dir`: local directory to upload
* `--workspace_subdir`: remote workspace subdirectory
* `--command`: command to run
* `--type`: resource type
* `--count`: resource count, default `1` in flag mode
* `--local_output_dir`: accepted for compatibility, default `.` in flag mode
* `--mem`: optional memory requirement
* `--cores`: optional core requirement
* `--pty`: run the command under a PTY
* `--utilization`: xsched utilization hint in `[0, 100]`

Notes:

* `integrated_client` shares the same upload and remote execution behavior as `remote_client`
* `local_output_dir` is still parsed, but the current flow does not restore output archives

## Current Behavior

The current execution flow is:

* Upload workspace
* Run command remotely
* Stream terminal output back to the client

The current flow does not:

* restore output archives on the client
* depend on archive extraction as part of normal execution

## Troubleshooting

* If a build fails because dependencies are missing, rerun `./scripts/build_deps.sh`
* If streamed output appears in bursts, prefer the default non-PTY mode unless PTY is required
* If you need to stop a running task, use `Ctrl+C` on the client and wait briefly for cleanup
