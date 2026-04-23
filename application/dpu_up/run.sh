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
    export XSCHED_AUTO_XQUEUE_THRESHOLD="${XSCHED_AUTO_XQUEUE_THRESHOLD:-2}"
    export XSCHED_AUTO_XQUEUE_BATCH_SIZE="${XSCHED_AUTO_XQUEUE_BATCH_SIZE:-1}"
    export XSCHED_AUTO_XQUEUE_TIMESLICE="${XSCHED_AUTO_XQUEUE_TIMESLICE:-100000}"
}

if [[ -z "${role}" ]]; then
    echo "Usage: $0 <fg|bg|fg-util|bg-util|fg-race|bg-race|fg-demo|bg-demo>" >&2
    exit 1
fi

case "${role}" in
    fg|bg|fg-util|bg-util|fg-race|bg-race|fg-demo|bg-demo)
        ;;
    *)
        echo "Invalid role: ${role}. Expected fg, bg, fg-util, bg-util, fg-race, bg-race, fg-demo, or bg-demo." >&2
        exit 1
        ;;
esac

validation_mode=0
base_role="${role}"
result_suffix=""
if [[ "${role}" == "fg-util" || "${role}" == "bg-util" ]]; then
    validation_mode=1
    base_role="${role%-util}"
    result_suffix=".util"
    export DPU_UP_RUN_MODE="${DPU_UP_RUN_MODE:-serial}"
    export DPU_UP_MAX_INFLIGHT_TASKS="${DPU_UP_MAX_INFLIGHT_TASKS:-1}"
    export DPU_UP_DMA_BUFFER_SIZE="${DPU_UP_DMA_BUFFER_SIZE:-4096}"
    export DPU_UP_WARMUP_MS="${DPU_UP_WARMUP_MS:-200}"
    export DPU_UP_WARMUP_CNT="${DPU_UP_WARMUP_CNT:-10}"
    export DPU_UP_MEASURE_SEC="${DPU_UP_MEASURE_SEC:-15}"
fi

race_mode=0
if [[ "${role}" == "fg-race" || "${role}" == "bg-race" ]]; then
    race_mode=1
    base_role="${role%-race}"
    result_suffix=".race"
    export DPU_UP_RUN_MODE="${DPU_UP_RUN_MODE:-backlog}"
    export DPU_UP_MAX_INFLIGHT_TASKS="${DPU_UP_MAX_INFLIGHT_TASKS:-1}"
    export DPU_UP_DMA_BUFFER_SIZE="${DPU_UP_DMA_BUFFER_SIZE:-65536}"
    export DPU_UP_RACE_WARMUP_TASKS="${DPU_UP_RACE_WARMUP_TASKS:-200}"
    export DPU_UP_RACE_TASKS="${DPU_UP_RACE_TASKS:-5000}"
fi

if [[ "${validation_mode}" == "1" ]]; then
    echo "Running xsched validation mode: role=${base_role}, mode=${DPU_UP_RUN_MODE}, inflight=${DPU_UP_MAX_INFLIGHT_TASKS}, dma_bytes=${DPU_UP_DMA_BUFFER_SIZE}" >&2
fi

if [[ "${race_mode}" == "1" ]]; then
    echo "Running xsched race mode: role=${base_role}, mode=${DPU_UP_RUN_MODE}, inflight=${DPU_UP_MAX_INFLIGHT_TASKS}, dma_bytes=${DPU_UP_DMA_BUFFER_SIZE}, race_tasks=${DPU_UP_RACE_TASKS}" >&2
fi

demo_mode=0
if [[ "${role}" == "fg-demo" || "${role}" == "bg-demo" ]]; then
    demo_mode=1
    base_role="${role%-demo}"
    result_suffix=".demo"
    export XSCHED_AUTO_XQUEUE_THRESHOLD="${XSCHED_AUTO_XQUEUE_THRESHOLD:-1}"
    export XSCHED_AUTO_XQUEUE_BATCH_SIZE="${XSCHED_AUTO_XQUEUE_BATCH_SIZE:-1}"
    export XSCHED_AUTO_XQUEUE_TIMESLICE="${XSCHED_AUTO_XQUEUE_TIMESLICE:-100000}"
    export DPU_UP_DEMO_WARMUP_CMDS="${DPU_UP_DEMO_WARMUP_CMDS:-200}"
    export DPU_UP_DEMO_MEASURE_SEC="${DPU_UP_DEMO_MEASURE_SEC:-8}"
    export DPU_UP_DEMO_CMD_US="${DPU_UP_DEMO_CMD_US:-2000}"
    export DPU_UP_DEMO_BACKLOG="${DPU_UP_DEMO_BACKLOG:-1}"
fi

if [[ "${demo_mode}" == "1" ]]; then
    echo "Running xsched demo mode: role=${base_role}, cmd_us=${DPU_UP_DEMO_CMD_US}, backlog=${DPU_UP_DEMO_BACKLOG}, measure_sec=${DPU_UP_DEMO_MEASURE_SEC}" >&2
fi

mkdir -p "${RESULT_DIR}"

if [[ "${base_role}" == "fg" || "${DPU_UP_RESET_SYNC:-0}" == "1" ]]; then
    ipcrm --shmem-key 0xbeef 2>/dev/null || true
fi

if [[ ! -x "${BUILD_DIR}/up_fg" || ! -x "${BUILD_DIR}/up_bg" ]]; then
    bash "${TESTCASE_ROOT}/scripts/build.sh"
fi

if [[ "${demo_mode}" == "1" && ( ! -x "${BUILD_DIR}/up_fg_demo" || ! -x "${BUILD_DIR}/up_bg_demo" ) ]]; then
    bash "${TESTCASE_ROOT}/scripts/build.sh"
fi

configure_xsched_preload

case "${base_role}" in
    fg)
        if [[ "${demo_mode}" == "1" ]]; then
            rm -f "${RESULT_DIR}/up_fg${result_suffix}.txt"
            exec "${BUILD_DIR}/up_fg_demo" "${RESULT_DIR}/up_fg${result_suffix}.txt"
        fi
        if [[ "${race_mode}" == "1" ]]; then
            rm -f "${RESULT_DIR}/up_fg${result_suffix}.txt"
            exec "${BUILD_DIR}/up_fg_race" "${RESULT_DIR}/up_fg${result_suffix}.txt"
        fi
        rm -f "${RESULT_DIR}/up_fg${result_suffix}.thpt"
        exec "${BUILD_DIR}/up_fg" "${RESULT_DIR}/up_fg${result_suffix}.thpt"
        ;;
    bg)
        if [[ "${demo_mode}" == "1" ]]; then
            rm -f "${RESULT_DIR}/up_bg${result_suffix}.txt"
            exec "${BUILD_DIR}/up_bg_demo" "${RESULT_DIR}/up_bg${result_suffix}.txt"
        fi
        if [[ "${race_mode}" == "1" ]]; then
            rm -f "${RESULT_DIR}/up_bg${result_suffix}.txt"
            exec "${BUILD_DIR}/up_bg_race" "${RESULT_DIR}/up_bg${result_suffix}.txt"
        fi
        rm -f "${RESULT_DIR}/up_bg${result_suffix}.thpt"
        exec "${BUILD_DIR}/up_bg" "${RESULT_DIR}/up_bg${result_suffix}.thpt"
        ;;
esac
