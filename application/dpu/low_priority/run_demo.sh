#!/bin/bash
set -euo pipefail

cd ./dpa_kernel_launch
./build_output.sh
./output/doca_dpa_kernel_launch
