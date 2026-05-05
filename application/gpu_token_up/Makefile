# BUILD_TYPE = Debug / Release
BUILD_TYPE ?= Release

# VERBOSE = ON / OFF
VERBOSE ?= OFF

WORK_PATH := $(shell pwd)
PROJECT_ROOT := $(abspath $(WORK_PATH)/../..)
BUILD_PATH := $(WORK_PATH)/build

.PHONY: all
all: build

.PHONY: build
build: $(BUILD_PATH)/CMakeCache.txt
	cmake --build $(BUILD_PATH) -- -j$(shell nproc)

$(BUILD_PATH)/CMakeCache.txt:
	$(MAKE) configure

.PHONY: configure
configure:
	cmake -B$(BUILD_PATH) \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DCMAKE_VERBOSE_MAKEFILE=$(VERBOSE) \
		-DCUDA_GEN_CODE=$(CUDA_GEN_CODE)

.PHONY: clean
clean:
	rm -rf $(BUILD_PATH)
