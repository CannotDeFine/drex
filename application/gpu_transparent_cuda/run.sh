#!/bin/bash
set -euo pipefail

task_name=${1:-task}
duration_sec=${2:-120}
window_sec=${3:-5}

TESTCASE_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
BUILD_DIR="${TESTCASE_ROOT}/build"
RESULT_DIR="${TESTCASE_ROOT}/results"
XSCHED_ROOT=${XSCHED_ROOT:-/home/cdf/xsched}
XSCHED_LIB_DIR=${XSCHED_LIB_DIR:-${XSCHED_ROOT}/output/lib}
XSCHED_CUDA_LIB=${XSCHED_CUDA_LIB:-/usr/lib/x86_64-linux-gnu/libcuda.so.1}

prepend_path() {
    local path="$1"
    local current="${2:-}"
    if [[ -z "${path}" ]]; then
        printf '%s' "${current}"
    elif [[ -z "${current}" ]]; then
        printf '%s' "${path}"
    else
        printf '%s:%s' "${path}" "${current}"
    fi
}

configure_xsched_preload() {
    local lib_dir=""
    if [[ -n "${XSCHED_LIB_DIR}" && -f "${XSCHED_LIB_DIR}/libshimcuda.so" ]]; then
        lib_dir="${XSCHED_LIB_DIR}"
    elif [[ -f "${XSCHED_ROOT}/output/lib/libshimcuda.so" ]]; then
        lib_dir="${XSCHED_ROOT}/output/lib"
    fi

    if [[ -z "${lib_dir}" ]]; then
        echo "WARNING: xsched CUDA shim directory not found under ${XSCHED_ROOT}; running without shim." >&2
        return
    fi

    export LD_LIBRARY_PATH="$(prepend_path "${lib_dir}" "${LD_LIBRARY_PATH:-}")"
    export XSCHED_CUDA_LIB
    export XSCHED_AUTO_XQUEUE_LEVEL=${XSCHED_AUTO_XQUEUE_LEVEL:-1}
    export XSCHED_AUTO_XQUEUE_THRESHOLD=${XSCHED_AUTO_XQUEUE_THRESHOLD:-16}
    export XSCHED_AUTO_XQUEUE_BATCH_SIZE=${XSCHED_AUTO_XQUEUE_BATCH_SIZE:-8}
}

if [[ "${GPU_TRANSPARENT_CUDA_FORCE_REBUILD:-1}" == "1" ]]; then
    make -C "${TESTCASE_ROOT}" clean >/dev/null 2>&1 || true
fi

mkdir -p "${RESULT_DIR}"

if [[ "${GPU_TRANSPARENT_CUDA_FORCE_REBUILD:-1}" == "1" || ! -x "${BUILD_DIR}/transparent_cuda_loop" ]]; then
    make -C "${TESTCASE_ROOT}"
fi

if [[ "${GPU_TRANSPARENT_CUDA_ENABLE_XSCHED:-1}" == "1" ]]; then
    configure_xsched_preload
else
    unset XSCHED_SCHEDULER
    unset XSCHED_AUTO_XQUEUE
    unset XSCHED_AUTO_XQUEUE_UTILIZATION
    unset XSCHED_AUTO_XQUEUE_PRIORITY
    unset XSCHED_AUTO_XQUEUE_TIMESLICE
fi

exec "${BUILD_DIR}/transparent_cuda_loop" \
    "${RESULT_DIR}/transparent_cuda_${task_name}.csv" \
    "${duration_sec}" \
    "${window_sec}"
