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
1. 启动服务端（默认监听 `0.0.0.0:8063`）：
   ```shell
   ./build/remote_server --address=0.0.0.0 --port=8063
   ```
2. 运行 gRPC 客户端上传任务，可附带额外文件并指定回传的结果文件：
   ```shell
   ./build/remote_client \
     --target=10.0.0.2:8063 \
     --file=./application/cpu/app \
     --save_path=tasks/app.bin \
     --extra_files=script:./scripts/setup.sh:tasks/setup.sh,data:./inputs/input.dat:tasks/input.dat \
     --extra_dirs=data:./dataset:tasks/dataset \
     --server_result_path=tasks/output.log \
     --device=0
   ```
   - `--extra_files` 使用 `type:本地路径[:服务端路径]` 语法（以逗号分隔多个条目），服务端会校验路径并只允许写在其工作目录中。
   - `--extra_dirs` 使用 `type:本地目录[:服务端目录]` 语法，客户端会递归上传该目录下的所有文件并保留相对路径。
   - `--server_result_path` 表示任务完成后需要从服务器返回的文件路径（位于服务器工作目录内）；若省略，则默认回传任务的标准输出。`--result_path` 仍可用但已弃用，仅作兼容别名。
3. 如需整合资源申请和任务执行，可使用集成客户端：
   ```shell
   ./build/intergrated_client <controller_host:port> <local_app> <remote_app_path> [device_type] [server_result_path]
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

## 四、TODO

1  任务执行器基类和各个算力任务的继承类的编写 已完成 \
2  执行过程中命令的检查，确保只能在当前目录下进行操作 \
3  不应该用户指定在服务端存储的文件路径 \
4  脚本和数据传递，以及需要能传递结果的数据结构 \
5  更安全的版本 fork + exec 避免使用system 已完成
