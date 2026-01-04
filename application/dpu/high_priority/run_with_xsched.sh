#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
XSCHED_BUILD_DIR="${XSCHED_BUILD_DIR:-/home/cdf/xsched/build}"
XSERVER_BIN="${XSCHED_BUILD_DIR}/service/xserver"
if [[ ! -x "${XSERVER_BIN}" ]]; then
  echo "ERROR: cannot find xserver at ${XSERVER_BIN}." >&2
  exit 3
fi

started_xserver=0
XSERVER_PID=""
cleanup() {
  if [[ ${started_xserver} -eq 1 && -n "${XSERVER_PID}" ]]; then
    kill "${XSERVER_PID}" 2>/dev/null || true
  fi
}
trap cleanup EXIT

if ! pgrep -x xserver >/dev/null 2>&1; then
  echo "[high_priority] starting xserver (HPF 50000)" >&2
  "${XSERVER_BIN}" HPF 50000 >/tmp/xserver.log 2>&1 &
  XSERVER_PID=$!
  started_xserver=1
  sleep 1
fi

export XSCHED_BUILD_DIR
export XSCHED_SCHEDULER=GLB
export XSCHED_AUTO_XQUEUE=ON
export XSCHED_AUTO_XQUEUE_LEVEL=1
export XSCHED_AUTO_XQUEUE_THRESHOLD="${XSCHED_AUTO_XQUEUE_THRESHOLD:-64}"
export XSCHED_AUTO_XQUEUE_BATCH_SIZE="${XSCHED_AUTO_XQUEUE_BATCH_SIZE:-32}"
export XSCHED_AUTO_XQUEUE_PRIORITY="${XSCHED_AUTO_XQUEUE_PRIORITY:-1}"
export XSCHED_AUTO_XQUEUE_UTILIZATION=20

# Keep this queue busy but preemptible so xcli can show RDY/BLK flips.
export DPA_DEMO_PRINT_ELAPSED_MS=1
export DPA_DEMO_HEARTBEAT_MS=200

"${SCRIPT_DIR}/run_demo_xsched_preload.sh"
