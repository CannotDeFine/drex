#!/bin/bash

mkdir -p ./output
nvcc -o ./output/app.cubin app.cu

export XSCHED_POLICY=GBL
export XSCHED_AUTO_XQUEUE=ON
export XSCHED_AUTO_XQUEUE_PRIORITY=0
export XSCHED_AUTO_XQUEUE_LEVEL=1
export XSCHED_AUTO_XQUEUE_THRESHOLD=4
export XSCHED_AUTO_XQUEUE_BATCH_SIZE=2
export LD_LIBRARY_PATH=/home/cdf/reconfigurable-os/xsched-artifacts/sys/xsched/output/lib:$LD_LIBRARY_PATH

./output/app.cubin