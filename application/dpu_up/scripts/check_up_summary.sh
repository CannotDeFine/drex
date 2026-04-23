#!/bin/bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "Usage: $0 <xserver_log> [tolerance_fraction]" >&2
    echo "Example: $0 /tmp/xserver.log 0.15" >&2
    exit 1
fi

log_path="$1"
tolerance="${2:-0.15}"

if [[ ! -f "${log_path}" ]]; then
    echo "Log file not found: ${log_path}" >&2
    exit 1
fi

summaries=$(
    grep '\[UP_SUMMARY\]' "${log_path}" | tail -n 2 | awk '
        {
            xq = pid = util = switches = budget = ""
            for (i = 1; i <= NF; ++i) {
                if ($i ~ /^xq=/)       xq = substr($i, 4)
                if ($i ~ /^pid=/)      pid = substr($i, 5)
                if ($i ~ /^util=/)     util = substr($i, 6)
                if ($i ~ /^switches=/) switches = substr($i, 10)
                if ($i ~ /^budget_us=/) budget = substr($i, 11)
            }
            if (xq != "" && pid != "" && util != "" && switches != "" && budget != "") {
                print xq, pid, util, switches, budget
            }
        }
    '
)

line_count=$(printf '%s\n' "${summaries}" | sed '/^$/d' | wc -l)
if [[ "${line_count}" -ne 2 ]]; then
    echo "Expected 2 UP_SUMMARY lines in ${log_path}, found ${line_count}." >&2
    exit 1
fi

line_a=$(printf '%s\n' "${summaries}" | sed -n '1p')
line_b=$(printf '%s\n' "${summaries}" | sed -n '2p')

read -r xq1 pid1 util1 switches1 budget1 <<<"${line_a}"
read -r xq2 pid2 util2 switches2 budget2 <<<"${line_b}"

if (( util1 <= 0 || util2 <= 0 || budget1 <= 0 || budget2 <= 0 )); then
    echo "Invalid util or budget parsed from UP_SUMMARY." >&2
    exit 1
fi

if (( util2 > util1 )); then
    tmp="${xq1}"; xq1="${xq2}"; xq2="${tmp}"
    tmp="${pid1}"; pid1="${pid2}"; pid2="${tmp}"
    tmp="${util1}"; util1="${util2}"; util2="${tmp}"
    tmp="${switches1}"; switches1="${switches2}"; switches2="${tmp}"
    tmp="${budget1}"; budget1="${budget2}"; budget2="${tmp}"
fi

analysis=$(
    awk -v util1="${util1}" -v util2="${util2}" \
        -v budget1="${budget1}" -v budget2="${budget2}" \
        -v tol="${tolerance}" '
        function abs(x) { return x < 0 ? -x : x }
        BEGIN {
            observed_ratio = budget1 / budget2
            expected_ratio = util1 / util2
            norm1 = budget1 / util1
            norm2 = budget2 / util2
            deviation = abs(norm1 - norm2) / (norm1 > norm2 ? norm1 : norm2)
            status = deviation <= tol ? "PASS" : "FAIL"
            printf "%.6f %.6f %.6f %s\n", observed_ratio, expected_ratio, deviation, status
        }
    '
)

read -r observed_ratio expected_ratio deviation verdict <<<"${analysis}"

echo "UP summary check"
echo "log: ${log_path}"
echo "queue_a: xq=${xq1} pid=${pid1} util=${util1} switches=${switches1} budget_us=${budget1}"
echo "queue_b: xq=${xq2} pid=${pid2} util=${util2} switches=${switches2} budget_us=${budget2}"
echo "observed_budget_ratio=${observed_ratio}"
echo "expected_util_ratio=${expected_ratio}"
echo "normalized_budget_deviation=${deviation}"
echo "tolerance=${tolerance}"
echo "verdict=${verdict}"

if [[ "${verdict}" != "PASS" ]]; then
    exit 2
fi
