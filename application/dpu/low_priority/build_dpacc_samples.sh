#!/bin/bash

# Build DPA device (kernel) code via dpacc.

set -e

# Args: <project_build_dir> <device_src_files_abs> <sample_name> <program_name>

PROJECT_BUILD_DIR=$1
DPA_KERNELS_DEVICE_SRC=$2
SAMPLE_NAME=$3
SAMPLE_PROGRAM_NAME=$4

# DOCA Configurations
DOCA_DIR="/opt/mellanox/doca"
DOCA_INCLUDE="${DOCA_DIR}/include"
DOCA_TOOLS="${DOCA_DIR}/tools"
DOCA_DPACC="${DOCA_TOOLS}/dpacc"

HOST_CC_FLAGS="-Wno-deprecated-declarations -Werror -Wall -Wextra"
DEVICE_CC_FLAGS="-Wno-deprecated-declarations -Werror -Wall -Wextra"

# DOCA DPA APP Configuration
# This variable name passed to DPACC with --app-name parameter and it's token must be identical to the
# struct doca_dpa_app parameter passed to doca_dpa_set_app(), i.e.
# doca_error_t doca_dpa_set_app(..., struct doca_dpa_app *${DPA_APP_NAME});
DPA_APP_NAME="dpa_sample_app"

# Build directory for the DPA device (kernel) code
SAMPLE_DEVICE_BUILD_DIR="${PROJECT_BUILD_DIR}/${SAMPLE_NAME}/device/build_dpacc"

rm -rf ${SAMPLE_DEVICE_BUILD_DIR}
mkdir -p ${SAMPLE_DEVICE_BUILD_DIR}

# Compile the DPA (kernel) device source code using the DPACC
$DOCA_DPACC $DPA_KERNELS_DEVICE_SRC \
	-o ${SAMPLE_DEVICE_BUILD_DIR}/${SAMPLE_PROGRAM_NAME}.a \
	-hostcc=gcc \
	-hostcc-options="${HOST_CC_FLAGS}" \
	--devicecc-options="${DEVICE_CC_FLAGS}" \
	--app-name="${DPA_APP_NAME}" \
	-device-libs="-L${DOCA_INCLUDE} -ldoca_dpa_dev_comm" \
	-ldpa \
	-flto \
