NPROC := $(shell nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)

.PHONY: all build run clean test asan

all: build run

build:
	@if [ ! -f "build/CMakeCache.txt" ]; then \
		cmake -B build -S . -DCMAKE_EXPORT_COMPILE_COMMANDS=ON; \
	fi
	cmake --build build -j$(NPROC)

asan:
	@if [ ! -f "build-asan/CMakeCache.txt" ]; then \
		cmake -B build-asan -S . \
			-DCMAKE_BUILD_TYPE=Debug \
			-DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
			-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"; \
	fi
	cmake --build build-asan -j$(NPROC)
	cd build-asan && ctest --output-on-failure

run:
	./build/parse_test

test:
	cd build && ctest --output-on-failure

clean:
	rm -rf build build-asan
