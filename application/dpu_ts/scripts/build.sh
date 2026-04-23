#!/bin/bash
set -euo pipefail

TESTCASE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "${TESTCASE_ROOT}/dpa_kernel_launch"
bash ./build_output.sh
