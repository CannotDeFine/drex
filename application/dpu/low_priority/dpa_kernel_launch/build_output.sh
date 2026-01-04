#!/bin/bash
set -euo pipefail

# Build DOCA DPA kernel_launch demo into ./output (instead of ./build)

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${ROOT_DIR}/output"

mkdir -p "${BUILD_DIR}"

if [ ! -f "${BUILD_DIR}/build.ninja" ]; then
  meson setup "${BUILD_DIR}" "${ROOT_DIR}"
else
  meson setup --reconfigure "${BUILD_DIR}" "${ROOT_DIR}"
fi

ninja -C "${BUILD_DIR}"

echo "Built: ${BUILD_DIR}/doca_dpa_kernel_launch"
