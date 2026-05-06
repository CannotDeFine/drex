NVCC ?= nvcc
NVCC_FLAGS ?= -O3 -std=c++14

BUILD_DIR := build
TARGET := $(BUILD_DIR)/transparent_cuda_loop
SRC := transparent_cuda_loop.cu

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	mkdir -p $(BUILD_DIR)
	$(NVCC) $(NVCC_FLAGS) -o $@ $<

clean:
	rm -rf $(BUILD_DIR) results

