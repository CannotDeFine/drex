#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$ROOT_DIR/build"
PREFIX="${RPC_THIRD_PARTY_PREFIX:-$ROOT_DIR/3rdparty/install}"

if [[ ! -d "$PREFIX" ]]; then
  echo "Missing third-party install prefix: $PREFIX"
  echo "Run ./scripts/build_deps.sh first."
  exit 1
fi

export PATH="$PREFIX/bin:$PATH"
export LD_LIBRARY_PATH="$PREFIX/lib:$PREFIX/lib64:${LD_LIBRARY_PATH:-}"

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
pushd "$BUILD_DIR" >/dev/null

cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DRPC_THIRD_PARTY_PREFIX="$PREFIX"

cmake --build . -j"$(nproc)"

popd >/dev/null

ln -sfn build/compile_commands.json "$ROOT_DIR/compile_commands.json"
