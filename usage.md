# 算力设备的远程过程调用

## 一、构建
先准备系统依赖：

```shell
sudo apt install -y build-essential cmake libssl-dev libleveldb-dev libgflags-dev
```

初始化子模块并构建项目私有依赖前缀：

```shell
git submodule update --init --recursive
./scripts/build_deps.sh
```

再编译项目：

```shell
./compile.sh
```

如果不想用默认的 `3rdparty/install`，可以在两步里都传同一个前缀：

```shell
RPC_THIRD_PARTY_PREFIX=/path/to/prefix ./scripts/build_deps.sh
RPC_THIRD_PARTY_PREFIX=/path/to/prefix ./compile.sh
```
## 二、How to use

```shell
./build/remote_client \
  --target=server_ip:port \
  --src_dir=/path/to/local/folder \
  --workspace_subdir=father_folder \
  --command="command to build your project"
```
