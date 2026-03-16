#!/bin/bash
set -euo pipefail

dev_name=${1:-local}
batch_size=${2:-1}

TESTCASE_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
RESULT_DIR="${TESTCASE_ROOT}/results"
MODEL_PREFIX=${GPU_UP_MODEL_PREFIX:-"${TESTCASE_ROOT}/models/resnet152"}
BUILD_DIR="${TESTCASE_ROOT}/build"

mkdir -p "${RESULT_DIR}"

if [[ ! -f "${MODEL_PREFIX}.onnx" ]]; then
    echo "Missing model file: ${MODEL_PREFIX}.onnx" >&2
    echo "Set GPU_UP_MODEL_PREFIX or place the model under ${TESTCASE_ROOT}/models" >&2
    exit 1
fi

if [[ ! -x "${BUILD_DIR}/up_fg" || ! -x "${BUILD_DIR}/up_bg" ]]; then
    bash "${TESTCASE_ROOT}/scripts/build.sh"
fi

ipcrm --shmem-key 0xbeef 2>/dev/null || true
rm -rf /dev/shm/__IPC_SHM__*

"${BUILD_DIR}/up_fg" \
    "${MODEL_PREFIX}" \
    "${batch_size}" \
    "${RESULT_DIR}/up_base_${dev_name}.thpt" &
FG_PID=$!
echo "FG_PID: ${FG_PID}"

sleep 2
"${BUILD_DIR}/up_bg" \
    "${MODEL_PREFIX}" \
    "${batch_size}" \
    "${RESULT_DIR}/up_base_${dev_name}.thpt" &
BG_PID=$!
echo "BG_PID: ${BG_PID}"

trap 'kill -9 ${FG_PID} ${BG_PID} 2>/dev/null || true' SIGINT SIGTERM
wait "${FG_PID}" "${BG_PID}"

ipcrm --shmem-key 0xbeef 2>/dev/null || true
rm -rf /dev/shm/__IPC_SHM__*
