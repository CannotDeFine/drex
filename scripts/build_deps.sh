#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THIRD_PARTY_DIR="$ROOT_DIR/3rdparty"
PREFIX="${RPC_THIRD_PARTY_PREFIX:-$THIRD_PARTY_DIR/install}"
BUILD_DIR="$THIRD_PARTY_DIR/build"
JOBS="${JOBS:-$(nproc)}"

require_dir() {
    local path="$1"
    local hint="$2"
    if [[ ! -d "$path" ]]; then
        echo "Missing dependency source: $path"
        echo "$hint"
        exit 1
    fi
}

configure_build_install() {
    local name="$1"
    local src_dir="$2"
    shift 2
    local build_subdir="$BUILD_DIR/$name"
    cmake -S "$src_dir" -B "$build_subdir" "$@"
    cmake --build "$build_subdir" -j"$JOBS"
    cmake --install "$build_subdir"
}

mkdir -p "$BUILD_DIR" "$PREFIX"

require_dir "$THIRD_PARTY_DIR/protobuf" "Run: git submodule update --init --recursive"
require_dir "$THIRD_PARTY_DIR/grpc" "Run: git submodule update --init --recursive"
require_dir "$THIRD_PARTY_DIR/brpc" "Run: git submodule update --init --recursive"

git submodule update --init 3rdparty/protobuf 3rdparty/grpc 3rdparty/brpc
# This project only consumes a small slice of gRPC's optional third_party tree.
# Pulling the full recursive graph makes bootstrapping much slower and adds fragile test-only deps.
git -C "$THIRD_PARTY_DIR/grpc" submodule update --init \
    third_party/abseil-cpp \
    third_party/cares/cares \
    third_party/re2 \
    third_party/zlib

export PATH="$PREFIX/bin:$PATH"
export LD_LIBRARY_PATH="$PREFIX/lib:$PREFIX/lib64:${LD_LIBRARY_PATH:-}"

configure_build_install \
    absl \
    "$THIRD_PARTY_DIR/grpc/third_party/abseil-cpp" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_STANDARD=17 \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DABSL_PROPAGATE_CXX_STD=ON \
    -DCMAKE_INSTALL_PREFIX="$PREFIX"

configure_build_install \
    cares \
    "$THIRD_PARTY_DIR/grpc/third_party/cares/cares" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCARES_SHARED=ON \
    -DCARES_STATIC=OFF \
    -DCMAKE_INSTALL_PREFIX="$PREFIX"

configure_build_install \
    re2 \
    "$THIRD_PARTY_DIR/grpc/third_party/re2" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_STANDARD=17 \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=ON \
    -DCMAKE_INSTALL_PREFIX="$PREFIX"

configure_build_install \
    zlib \
    "$THIRD_PARTY_DIR/grpc/third_party/zlib" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=ON \
    -DCMAKE_INSTALL_PREFIX="$PREFIX"

configure_build_install \
    protobuf \
    "$THIRD_PARTY_DIR/protobuf" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_STANDARD=17 \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -Dprotobuf_ABSL_PROVIDER=package \
    -DBUILD_SHARED_LIBS=ON \
    -Dprotobuf_BUILD_TESTS=OFF \
    -DCMAKE_PREFIX_PATH="$PREFIX"

configure_build_install \
    grpc \
    "$THIRD_PARTY_DIR/grpc" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_STANDARD=17 \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DgRPC_INSTALL=ON \
    -DgRPC_BUILD_TESTS=OFF \
    -DgRPC_ABSL_PROVIDER=package \
    -DgRPC_CARES_PROVIDER=package \
    -DgRPC_PROTOBUF_PROVIDER=package \
    -DgRPC_RE2_PROVIDER=package \
    -DgRPC_SSL_PROVIDER=package \
    -DgRPC_ZLIB_PROVIDER=package \
    -DCMAKE_PREFIX_PATH="$PREFIX"

# bRPC still relies on a few system libraries such as OpenSSL, gflags, and leveldb.
# The protobuf-related paths are pinned to the private prefix so we do not accidentally mix versions.
configure_build_install \
    brpc \
    "$THIRD_PARTY_DIR/brpc" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_STANDARD=17 \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_PREFIX_PATH="$PREFIX" \
    -DProtobuf_DIR="$PREFIX/lib/cmake/protobuf" \
    -DProtobuf_INCLUDE_DIR="$PREFIX/include" \
    -DProtobuf_LIBRARIES="$PREFIX/lib/libprotobuf.so" \
    -DProtobuf_PROTOC_EXECUTABLE="$PREFIX/bin/protoc"

echo "Dependencies installed under: $PREFIX"
