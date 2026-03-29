#!/bin/bash
set -euo pipefail

role=${1:-}

TESTCASE_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
RESULT_DIR="${TESTCASE_ROOT}/results"
BUILD_DIR="${TESTCASE_ROOT}/build"
XSCHED_ROOT=${XSCHED_ROOT:-/home/cdf/xsched}
XSCHED_BUILD_DIR=${XSCHED_BUILD_DIR:-${XSCHED_ROOT}/build}

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
    export LD_LIBRARY_PATH="$(prepend_path "${XSCHED_BUILD_DIR}/platforms/doca28" "${LD_LIBRARY_PATH:-}")"
    export LD_LIBRARY_PATH="$(prepend_path "${XSCHED_BUILD_DIR}/preempt" "${LD_LIBRARY_PATH:-}")"
    export LD_PRELOAD="${XSCHED_BUILD_DIR}/platforms/doca28/libshimdoca28.so${LD_PRELOAD:+:${LD_PRELOAD}}"
    export XSCHED_SCHEDULER=GLB
}

if [[ -z "${role}" ]]; then
    echo "Usage: $0 <fg|bg> [device_name]" >&2
    exit 1
fi

if [[ "${role}" != "fg" && "${role}" != "bg" ]]; then
    echo "Invalid role: ${role}. Expected fg or bg." >&2
    exit 1
fi

mkdir -p "${RESULT_DIR}"

if [[ "${role}" == "fg" || "${DPU_UP_RESET_SYNC:-0}" == "1" ]]; then
    ipcrm --shmem-key 0xbeef 2>/dev/null || true
fi

if [[ ! -x "${BUILD_DIR}/up_fg" || ! -x "${BUILD_DIR}/up_bg" ]]; then
    bash "${TESTCASE_ROOT}/scripts/build.sh"
fi

configure_xsched_preload

case "${role}" in
    fg)
        rm -f "${RESULT_DIR}/up_fg.thpt"
        exec "${BUILD_DIR}/up_fg" "${RESULT_DIR}/up_fg.thpt"
        ;;
    bg)
        rm -f "${RESULT_DIR}/up_bg.thpt"
        exec "${BUILD_DIR}/up_bg" "${RESULT_DIR}/up_bg.thpt"
        ;;
esac
