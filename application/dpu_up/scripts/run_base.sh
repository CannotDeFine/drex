#!/bin/bash
set -euo pipefail

TESTCASE_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
RESULT_DIR="${TESTCASE_ROOT}/results"

mkdir -p "${RESULT_DIR}"
ipcrm --shmem-key 0xbeef 2>/dev/null || true
rm -f "${RESULT_DIR}/up_fg.thpt" "${RESULT_DIR}/up_bg.thpt"

bash "${TESTCASE_ROOT}/scripts/build.sh"

bash "${TESTCASE_ROOT}/run.sh" fg &
FG_PID=$!
echo "FG_PID: ${FG_PID}"

sleep 1
bash "${TESTCASE_ROOT}/run.sh" bg &
BG_PID=$!
echo "BG_PID: ${BG_PID}"

trap 'kill -9 ${FG_PID} ${BG_PID} 2>/dev/null || true' SIGINT SIGTERM
wait "${FG_PID}" "${BG_PID}"

ipcrm --shmem-key 0xbeef 2>/dev/null || true
