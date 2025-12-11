# 算力设备的远程过程调用

## 一、构建
编译安装protobuf
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
编译安装gRPC
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
编译安装bRPC
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

编译项目
```shell
./compile.sh
```

## 二、文件结构
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
└── tools
````

## 三、How to use
```shell
./build/remote_client \
  --target=server_ip:port \
  --src_dir=/path/to/local/folder \
  --workspace_subdir=father_folder \
  --command="command to build your project"
```
