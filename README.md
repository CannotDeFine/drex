# 算力设备的远程过程调用

## 一、构建
1 依赖安装
```shell
# protobuf 3.25.7 安装
# 这里的absl可以用下面gRPC的使用的版本，将它安装到系统
mkdir build
cd build
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr/local \
  -Dprotobuf_ABSL_PROVIDER=package \
  -DBUILD_SHARED_LIBS=ON \
  -Dprotobuf_BUILD_TESTS=OFF \
  -DCMAKE_PREFIX_PATH=/usr/local
make -j$(nproc)
sudo make install
```
2 编译安装gRPC
```shell
git clone -b v1.74.0 --depth 1 https://github.com/grpc/grpc
cd grpc
git submodule update --init --recursive # or download by hand

# Install absl
mkdir -p "third_party/abseil-cpp/cmake/build"
pushd "third_party/abseil-cpp/cmake/build"
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=17 -DCMAKE_POSITION_INDEPENDENT_CODE=TRUE ../..
make -j $(nproc) 
sudo make install
popd

# Install c-ares
# If the distribution provides a new-enough version of c-ares,
# this section can be replaced with:
# apt-get install -y libc-ares-dev
mkdir -p "third_party/cares/cares/cmake/build"
pushd "third_party/cares/cares/cmake/build"
cmake -DCMAKE_BUILD_TYPE=Release ../..
make -j $(nproc) 
sudo make install
popd

# Install re2
mkdir -p "third_party/re2/cmake/build"
pushd "third_party/re2/cmake/build"
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=17 -DCMAKE_POSITION_INDEPENDENT_CODE=TRUE ../..
make -j $(nproc) 
sudo make install
popd

# Install zlib
mkdir -p "third_party/zlib/cmake/build"
pushd "third_party/zlib/cmake/build"
cmake -DCMAKE_BUILD_TYPE=Release ../..
make -j $(nproc) 
sudo make install
popd

# Install gRPC, we must install by package.
mkdir -p "cmake/build"
pushd "cmake/build"
cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_STANDARD=17 \
  -DgRPC_INSTALL=ON \
  -DgRPC_BUILD_TESTS=OFF \
  -DgRPC_CARES_PROVIDER=package \
  -DgRPC_ABSL_PROVIDER=package \
  -DgRPC_PROTOBUF_PROVIDER=package \
  -DgRPC_RE2_PROVIDER=package \
  -DgRPC_SSL_PROVIDER=package \
  -DgRPC_ZLIB_PROVIDER=package \
  ../..

# install grpc to system
make -j $(nproc)
sudo make install
popd
```
3 编译bRPC
```shell
mkdir build
cd build
cmake .. \
  -DProtobuf_INCLUDE_DIR=/usr/local/include \
  -DProtobuf_LIBRARIES=/usr/local/lib/libprotobuf.so \
  -DProtobuf_PROTOC_EXECUTABLE=/usr/local/bin/protoc \
  -DCMAKE_INSTALL_PREFIX=/usr/local \
  -DCMAKE_BUILD_TYPE=Release

make -j$(nproc)
sudo make install
```
4 安装第三方库
```shell
# curl install
sudo apt install libcurl4-openssl-dev
```
5 编译项目
```shell
./compile.sh
```

## 二、使用说明
为了方便部署，服务器会把所有上传的内容放进项目根目录下的 `workspace/`，因此建议始终从仓库根目录运行二进制文件，或在启动前手动创建好该目录。客户端在上传文件时会重新写入数据，原始可执行权限不会被保留，如有需要请在 `--command` 中自行调用 `chmod +x`。

1. 启动服务端（默认监听 `0.0.0.0:8063`）：
   ```shell
   ./build/remote_server --address=0.0.0.0 --port=8063
   ```
2. 运行 gRPC 客户端上传整个目录、指定执行命令以及输出目录：
   ```shell
   ./build/remote_client \
     --target=10.0.0.2:8063 \
     --src_dir=./workspace/my_job \
     --workspace_subdir=jobs/my_job \
     --command="./run.sh --config configs/train.yaml"
   ```
   - `--src_dir`：本地需要上传的目录，客户端会递归遍历并保持相对路径。
   - `--workspace_subdir`：服务器 `workspace/` 下的落盘子目录，所有文件会被一一同步。
   - `--command`：服务器在 `workspace/<workspace_subdir>` 中执行的命令，底层通过 `sh -c` 启动，可将多条命令串联。
   - 服务器端输出固定写在 `<workspace_subdir>/output`，执行结束后 `terminal_output.txt` 和其他结果会被打包并自动解压回本地 `--src_dir/output/`，无需额外参数。

客户端与服务器之间使用双向流式 RPC。上传阶段客户端持续发送文件块；执行阶段服务器会把标准输出和标准错误实时推送为 log chunk，客户端终端能够立即看到远程日志。任务完成后服务器把 `<workspace_subdir>/output` 打包为 `TaskResult` 附件，客户端直接把压缩包解压到 `--src_dir/output/`，因此本地工作目录会同步远端运行所生成的全部文件。
3. 如需整合资源申请和任务执行，可使用集成客户端：
   ```shell
   ./build/intergrated_client <controller_host:port> <src_dir> <workspace_subdir> <command> <local_output_dir>
   ```


## 三、文件结构
```shell
.
├── application     # for testing
├── build           # to compile
├── client          # client: leanr how to use it
├── CMakeLists.txt
├── compile.sh      # script
├── proto           # proto file
├── README.md
├── server          # the implement of the function
├── third_party     # third party tools
└── tools
````

## 四、How to use
```shell
./build/remote_client \
  --target=server_ip:port \
  --src_dir=/path/to/local/folder \
  --workspace_subdir=father_folder \
  --command="command to build your project"
```
