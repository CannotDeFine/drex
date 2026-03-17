# DReX

`DReX` is a small remote execution toolkit built on top of gRPC and bRPC.

It supports three main workflows:

- Upload a local workspace to a remote node and execute a command there.
- Request resources from a controller and inspect the assigned nodes.
- Combine resource allocation and remote execution in one client.

## Components

- `remote_server`
  Accepts uploaded workspaces, runs commands on the server, and streams terminal output back to the client.
- `remote_client`
  Uploads a workspace to `remote_server` and streams the server-side command output.
- `resource_client`
  Talks to the resource controller through bRPC.
- `integrated_client`
  Applies for resources first, then submits the task to the returned node.

## Repository Layout

- `client/`
  Client-side entrypoints and shared upload/streaming logic.
- `server/`
  gRPC server implementation, workspace handling, task execution, and server logging.
- `proto/`
  Protocol definitions for gRPC and bRPC services.
- `application/`
  Example workloads for CPU, GPU, and DPU scenarios.
- `scripts/`
  Build helpers for third-party dependencies.
- `3rdparty/`
  Vendored dependencies and their private install prefix.
- `docs/`
  Detailed usage documentation.

## Build

System prerequisites:

- CMake `>= 3.16`
- A C++17-capable compiler
  Verified with GCC `11.4.0`
- OpenSSL development files
  OpenSSL `3.0.x` is verified in the current environment
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

Build steps:

```bash
git submodule update --init --recursive
./scripts/build_deps.sh
./compile.sh
```

Artifacts:

- `build/server/remote_server`
- `build/client/remote_client`
- `build/client/resource_client`
- `build/client/integrated_client`

## Runtime Notes

- The server uses `workspace/` as its runtime working root.
- The current execution model streams terminal output only.
- Output archive restore is no longer part of the client flow.
- Clients default to non-PTY execution for better batch-job reliability.
- Use `--pty` only when you really need terminal-like behavior.

## Documentation

Detailed command usage and examples are in [`docs/usage.md`](docs/usage.md).
