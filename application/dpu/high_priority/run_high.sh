#!/bin/bash
set -euo pipefail

TESTCASE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
XSCHED_BUILD_DIR="${XSCHED_BUILD_DIR:-/home/cdf/xsched/build}"
BUILD_DIR="${TESTCASE_ROOT}/dpa_kernel_launch"

cd "${BUILD_DIR}"
bash ./build_output.sh >/dev/null

export XSCHED_BUILD_DIR
export LD_LIBRARY_PATH="${XSCHED_BUILD_DIR}/platforms/doca28:${XSCHED_BUILD_DIR}/preempt:${LD_LIBRARY_PATH:-}"
export LD_PRELOAD="${XSCHED_BUILD_DIR}/platforms/doca28/libshimdoca28.so${LD_PRELOAD:+:$LD_PRELOAD}"

export XLOG_LEVEL="${XLOG_LEVEL:-WARN}"
export XSCHED_SCHEDULER="${XSCHED_SCHEDULER:-GLB}"
export XSCHED_AUTO_XQUEUE="${XSCHED_AUTO_XQUEUE:-ON}"
export XSCHED_AUTO_XQUEUE_LEVEL="${XSCHED_AUTO_XQUEUE_LEVEL:-1}"
export XSCHED_AUTO_XQUEUE_THRESHOLD="${XSCHED_AUTO_XQUEUE_THRESHOLD:-64}"
export XSCHED_AUTO_XQUEUE_BATCH_SIZE="${XSCHED_AUTO_XQUEUE_BATCH_SIZE:-32}"
export XSCHED_AUTO_XQUEUE_PRIORITY="${XSCHED_AUTO_XQUEUE_PRIORITY:-5}"

export DPA_DEMO_PRINT_ELAPSED_MS="${DPA_DEMO_PRINT_ELAPSED_MS:-1}"
export DPA_DEMO_PRINT_TASK_MS="${DPA_DEMO_PRINT_TASK_MS:-0}"
export DPA_DEMO_HEARTBEAT_MS="${DPA_DEMO_HEARTBEAT_MS:-0}"
export DPA_DEMO_STDOUT_FLUSH_MS="${DPA_DEMO_STDOUT_FLUSH_MS:-200}"

if [[ "${DPA_DEMO_USE_SCRIPT_PTY:-0}" != "0" ]] && command -v script >/dev/null 2>&1; then
    exec script -q -e -c "./output/doca_dpa_kernel_launch" /dev/null
fi

exec ./output/doca_dpa_kernel_launch
