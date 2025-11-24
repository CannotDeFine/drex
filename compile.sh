#!/bin/bash
set -e  # 遇到错误立即退出

# 清理并创建构建目录
rm -rf build
mkdir build
pushd build

# 配置编译选项
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_STANDARD=17 \

# 并行编译
make -j$(nproc)

# 返回上级目录
popd