#!/bin/bash
set -euo pipefail

TESTCASE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"

bash "${TESTCASE_ROOT}/scripts/build.sh" >/dev/null
DPA_TS_SKIP_BUILD=1 bash "${TESTCASE_ROOT}/run.sh" fg &
FG_PID=$!
sleep 0.2
DPA_TS_SKIP_BUILD=1 bash "${TESTCASE_ROOT}/run.sh" bg &
BG_PID=$!

wait "${FG_PID}"
wait "${BG_PID}"
