#!/bin/bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/dpa_kernel_launch"
chmod +x ./build_output.sh
./build_output.sh >/dev/null

XSCHED_BUILD_DIR="${XSCHED_BUILD_DIR:-/home/cdf/xsched/build}"
export LD_LIBRARY_PATH="${XSCHED_BUILD_DIR}/platforms/doca28:${XSCHED_BUILD_DIR}/preempt:${LD_LIBRARY_PATH:-}"
export LD_PRELOAD="${XSCHED_BUILD_DIR}/platforms/doca28/libshimdoca28.so${LD_PRELOAD:+:$LD_PRELOAD}"

export XSCHED_SCHEDULER="${XSCHED_SCHEDULER:-GLB}"
if [[ "${XSCHED_SCHEDULER}" != "GLB" ]]; then
	echo "ERROR: only XSCHED_SCHEDULER=GLB is supported by this demo (APP mode removed)." >&2
	exit 2
fi

if ! pgrep -x xserver >/dev/null 2>&1; then
	echo "ERROR: xserver is not running. GLB mode requires xserver." >&2
	echo "Hint: start it in another terminal, e.g.:" >&2
	echo "  ${XSCHED_BUILD_DIR}/service/xserver HPF 50000" >&2
	exit 3
fi
export XLOG_LEVEL="${XLOG_LEVEL:-WARN}"
export XSCHED_AUTO_XQUEUE="${XSCHED_AUTO_XQUEUE:-ON}"
export XSCHED_AUTO_XQUEUE_PRIORITY="${XSCHED_AUTO_XQUEUE_PRIORITY:-5}"
export XSCHED_AUTO_XQUEUE_LEVEL="${XSCHED_AUTO_XQUEUE_LEVEL:-1}"
export XSCHED_AUTO_XQUEUE_THRESHOLD="${XSCHED_AUTO_XQUEUE_THRESHOLD:-64}"
export XSCHED_AUTO_XQUEUE_BATCH_SIZE="${XSCHED_AUTO_XQUEUE_BATCH_SIZE:-32}"

# Print monotonic elapsed time per line to make preemption visible (gaps in low-priority output).
export DPA_DEMO_PRINT_ELAPSED_MS="${DPA_DEMO_PRINT_ELAPSED_MS:-1}"

# Heartbeat makes preemption obvious even when completions pause.
export DPA_DEMO_HEARTBEAT_MS="${DPA_DEMO_HEARTBEAT_MS:-0}"
export DPA_DEMO_STDOUT_FLUSH_MS="${DPA_DEMO_STDOUT_FLUSH_MS:-200}"

# NOTE:
# The high-priority binary defaults to DPA_DEMO_ITERS=3000000 and DPA_DEMO_INFLIGHT=1.
# Forcing inflight > 1 will make "Task N completed in X ms" look increasing because X
# includes queueing time from submission to completion.
# Keep defaults unless you explicitly export DPA_DEMO_ITERS / DPA_DEMO_INFLIGHT.

# Optional: wrap in a local PTY (useful when running this script directly).
# When using remote_client, prefer its --pty flag instead of forcing PTY here.
if [[ "${DPA_DEMO_USE_SCRIPT_PTY:-0}" != "0" ]] && command -v script >/dev/null 2>&1; then
	exec script -q -e -c "./output/doca_dpa_kernel_launch" /dev/null
fi

exec ./output/doca_dpa_kernel_launch
