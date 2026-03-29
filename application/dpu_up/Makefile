BUILD_DIR := $(CURDIR)/build

.PHONY: all
all: build

.PHONY: build
build:
	bash ./scripts/build.sh

.PHONY: build-inner
build-inner: $(BUILD_DIR)/CMakeCache.txt
	cmake --build "$(BUILD_DIR)" -- -j$(shell nproc)

$(BUILD_DIR)/CMakeCache.txt:
	cmake -S "$(CURDIR)" -B "$(BUILD_DIR)"

.PHONY: run-base
run-base: build
	bash ./scripts/run_base.sh

.PHONY: clean
clean:
	rm -rf "$(BUILD_DIR)" "$(CURDIR)/results"
