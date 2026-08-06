NPROC := $(shell nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)

SRCS := $(shell find src -name '*.cpp' -o -name '*.h' -o -name '*.c')

.PHONY: all build clean test perf asan tidy

all: build test

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
	ctest --test-dir build-asan --output-on-failure

test:
	ctest --test-dir build --output-on-failure $(if $(FILTER),-R $(FILTER))

# The timed cases in perf/. Minutes long, so it is not part of 'make'.
# DIFFICULTY=all adds the slow ones, SAVE=1 records the times to compare to.
perf: build
	python3 scripts/perf.py $(if $(DIFFICULTY),--difficulty $(DIFFICULTY)) \
		$(if $(FILTER),--filter $(FILTER)) $(if $(SAVE),--save)

clean:
	rm -rf build build-asan
