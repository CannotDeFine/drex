#!/bin/bash
set -euo pipefail

TESTCASE_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "${TESTCASE_ROOT}"

if [[ -f "${TESTCASE_ROOT}/build/CMakeCache.txt" ]]; then
    if ! grep -q "${TESTCASE_ROOT}" "${TESTCASE_ROOT}/build/CMakeCache.txt"; then
        make clean >/dev/null 2>&1
    fi
fi

build_log=$(mktemp "${TESTCASE_ROOT}/build.log.XXXXXX")
trap 'rm -f "${build_log}"' EXIT

if ! make build-inner >"${build_log}" 2>&1; then
    cat "${build_log}"
    exit 1
fi
