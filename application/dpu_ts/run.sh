#!/bin/bash
set -euo pipefail

TESTCASE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
ROLE="${1:-}"
SYNC_DIR="${DPA_TS_SYNC_DIR:-/dev/shm/dpa_ts_sync}"

if [[ -z "${ROLE}" ]]; then
    echo "Usage: $0 <fg|bg|fg-token|bg-token>"
    exit 1
fi

if [[ "${ROLE}" != "fg" && "${ROLE}" != "bg" && "${ROLE}" != "fg-token" && "${ROLE}" != "bg-token" ]]; then
    echo "Unknown role: ${ROLE}"
    exit 1
fi

BASE_ROLE="${ROLE%-token}"
TOKEN_MODE=0
if [[ "${ROLE}" == *"-token" ]]; then
    TOKEN_MODE=1
fi

if [[ "${DPA_TS_SKIP_BUILD:-0}" == "0" ]]; then
    bash "${TESTCASE_ROOT}/scripts/build.sh" >/dev/null
fi

if [[ "${BASE_ROLE}" == "fg" && "${DPA_TS_RESET_SYNC:-1}" != "0" ]]; then
    rm -rf "${SYNC_DIR}"
fi

mkdir -p "${SYNC_DIR}"
touch "${SYNC_DIR}/${BASE_ROLE}.ready"

while [[ ! -f "${SYNC_DIR}/fg.ready" || ! -f "${SYNC_DIR}/bg.ready" ]]; do
    sleep 0.05
done

if [[ "${BASE_ROLE}" == "fg" && ! -f "${SYNC_DIR}/start" ]]; then
    touch "${SYNC_DIR}/start"
fi

while [[ ! -f "${SYNC_DIR}/start" ]]; do
    sleep 0.01
done

export XSCHED_BUILD_DIR="${XSCHED_BUILD_DIR:-/home/cdf/xsched/build}"
export LD_LIBRARY_PATH="${XSCHED_BUILD_DIR}/platforms/doca28:${XSCHED_BUILD_DIR}/preempt:${LD_LIBRARY_PATH:-}"
export LD_PRELOAD="${XSCHED_BUILD_DIR}/platforms/doca28/libshimdoca28.so${LD_PRELOAD:+:$LD_PRELOAD}"

export XLOG_LEVEL="${XLOG_LEVEL:-WARN}"
export XSCHED_SCHEDULER="${XSCHED_SCHEDULER:-GLB}"
export XSCHED_AUTO_XQUEUE="${XSCHED_AUTO_XQUEUE:-ON}"
export XSCHED_AUTO_XQUEUE_LEVEL="${XSCHED_AUTO_XQUEUE_LEVEL:-1}"
export XSCHED_AUTO_XQUEUE_THRESHOLD="${XSCHED_AUTO_XQUEUE_THRESHOLD:-64}"
export XSCHED_AUTO_XQUEUE_BATCH_SIZE="${XSCHED_AUTO_XQUEUE_BATCH_SIZE:-32}"
export XSCHED_AUTO_XQUEUE_TIMESLICE="${XSCHED_AUTO_XQUEUE_TIMESLICE:-100000}"

if [[ "${BASE_ROLE}" == "fg" ]]; then
    export XSCHED_AUTO_XQUEUE_UTILIZATION="${XSCHED_AUTO_XQUEUE_UTILIZATION:-2}"
else
    export XSCHED_AUTO_XQUEUE_UTILIZATION="${XSCHED_AUTO_XQUEUE_UTILIZATION:-1}"
fi

if [[ "${TOKEN_MODE}" == "1" ]]; then
    export DPA_TS_TOKEN_MODE="${DPA_TS_TOKEN_MODE:-1}"
    export XSCHED_AUTO_XQUEUE_COMMAND_QUOTA="${XSCHED_AUTO_XQUEUE_COMMAND_QUOTA:-1}"
    export DPA_TS_WARMUP_MS="${DPA_TS_WARMUP_MS:-300}"
    export DPA_TS_RUNTIME_MS="${DPA_TS_RUNTIME_MS:-3000}"
    export DPA_TS_KERNEL_ITERS="${DPA_TS_KERNEL_ITERS:-2000}"
    export DPA_TS_MAX_INFLIGHT="${DPA_TS_MAX_INFLIGHT:-1}"
else
    export DPA_TS_WARMUP_MS="${DPA_TS_WARMUP_MS:-1000}"
    export DPA_TS_RUNTIME_MS="${DPA_TS_RUNTIME_MS:-8000}"
    export DPA_TS_KERNEL_ITERS="${DPA_TS_KERNEL_ITERS:-2000}"
    export DPA_TS_MAX_INFLIGHT="${DPA_TS_MAX_INFLIGHT:-2}"
fi

export DPA_TS_ROLE="${ROLE}"
export DPA_TS_OUTPUT_PATH="${DPA_TS_OUTPUT_PATH:-${TESTCASE_ROOT}/results/${ROLE}.result.txt}"

mkdir -p "${TESTCASE_ROOT}/results"

echo "Running DPA time-slice test: role=${ROLE}, util=${XSCHED_AUTO_XQUEUE_UTILIZATION}, runtime_ms=${DPA_TS_RUNTIME_MS}, warmup_ms=${DPA_TS_WARMUP_MS}, iters=${DPA_TS_KERNEL_ITERS}, inflight=${DPA_TS_MAX_INFLIGHT}, timeslice_us=${XSCHED_AUTO_XQUEUE_TIMESLICE}"
exec "${TESTCASE_ROOT}/dpa_kernel_launch/output/doca_dpa_kernel_launch"
