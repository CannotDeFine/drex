#!/bin/bash
set -euo pipefail

role=${1:-}
batch_size=${2:-1}
device_name=${3:-remote}

TESTCASE_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
MODEL_PREFIX=${GPU_UP_MODEL_PREFIX:-"${TESTCASE_ROOT}/models/resnet152"}
RESULT_DIR="${TESTCASE_ROOT}/results"
BUILD_DIR="${TESTCASE_ROOT}/build"
XSCHED_ROOT=${XSCHED_ROOT:-/home/cdf/xsched}
XSCHED_LIB_DIR=${XSCHED_LIB_DIR:-}
XSCHED_CUDA_LIB=${XSCHED_CUDA_LIB:-}

prepend_path() {
    local path="$1"
    local current="${2:-}"
    if [[ -z "${path}" ]]; then
        printf '%s' "${current}"
        return
    fi
    if [[ -z "${current}" ]]; then
        printf '%s' "${path}"
        return
    fi
    printf '%s:%s' "${path}" "${current}"
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
    # Let the shim resolve the real NVIDIA driver library explicitly instead of
    # relying on a plain libcuda.so soname lookup.
    if [[ -z "${XSCHED_CUDA_LIB}" ]]; then
        if [[ -f /usr/lib/x86_64-linux-gnu/libcuda.so.1 ]]; then
            XSCHED_CUDA_LIB=/usr/lib/x86_64-linux-gnu/libcuda.so.1
        elif [[ -f /usr/lib64/libcuda.so.1 ]]; then
            XSCHED_CUDA_LIB=/usr/lib64/libcuda.so.1
        elif [[ -f /usr/lib/wsl/lib/libcuda.so.1 ]]; then
            XSCHED_CUDA_LIB=/usr/lib/wsl/lib/libcuda.so.1
        fi
    fi

    if [[ -n "${XSCHED_CUDA_LIB}" ]]; then
        export XSCHED_CUDA_LIB
    else
        echo "[gpu_up] warning: XSCHED_CUDA_LIB is not set; shim may fail to forward CUDA calls" >&2
    fi
}

if [[ -z "${role}" ]]; then
    echo "Usage: $0 <fg|bg> [batch_size] [device_name]" >&2
    exit 1
fi

if [[ "${role}" != "fg" && "${role}" != "bg" ]]; then
    echo "Invalid role: ${role}. Expected fg or bg." >&2
    exit 1
fi

if [[ ! -f "${MODEL_PREFIX}.onnx" ]]; then
    echo "Missing model file: ${MODEL_PREFIX}.onnx" >&2
    echo "Set GPU_UP_MODEL_PREFIX or place the model under ${TESTCASE_ROOT}/models" >&2
    exit 1
fi

mkdir -p "${RESULT_DIR}"

if [[ "${GPU_UP_RESET_SYNC:-0}" == "1" ]]; then
    ipcrm --shmem-key 0xbeef 2>/dev/null || true
fi

if [[ ! -x "${BUILD_DIR}/up_fg" || ! -x "${BUILD_DIR}/up_bg" ]]; then
    bash "${TESTCASE_ROOT}/scripts/build.sh"
fi

configure_xsched_preload

case "${role}" in
    fg)
        exec "${BUILD_DIR}/up_fg" \
            "${MODEL_PREFIX}" \
            "${batch_size}" \
            "${RESULT_DIR}/up_fg_${device_name}.thpt"
        ;;
    bg)
        exec "${BUILD_DIR}/up_bg" \
            "${MODEL_PREFIX}" \
            "${batch_size}" \
            "${RESULT_DIR}/up_bg_${device_name}.thpt"
        ;;
esac
