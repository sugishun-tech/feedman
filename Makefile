BUILD_DIR ?= build
CMAKE_FLAGS ?= -DCMAKE_BUILD_TYPE=Release

.PHONY: all configure test benchmark run install clean
all: configure
	cmake --build $(BUILD_DIR) --parallel
configure:
	cmake -S . -B $(BUILD_DIR) $(CMAKE_FLAGS)
test: all
	ctest --test-dir $(BUILD_DIR) --output-on-failure
benchmark: all
	$(BUILD_DIR)/feedman_tests --benchmark
run: all
	$(BUILD_DIR)/feedman
install: all
	cmake --install $(BUILD_DIR)
clean:
	cmake --build $(BUILD_DIR) --target clean
