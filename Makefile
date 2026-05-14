NPROC        := $(shell nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)
CLANG_TIDY   := $(shell command -v clang-tidy 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/clang-tidy)
CLANG_FORMAT := $(shell command -v clang-format 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/clang-format)
CC_SRCS      := $(wildcard src/*.cc)
ALL_SRCS     := $(wildcard src/*.cc src/*.h)

.PHONY: all build run clean test asan check tidy

all: build check run

# Real-file target: only re-runs cmake configure when CMakeLists.txt changes.
build/compile_commands.json: CMakeLists.txt
	cmake -B build -S . -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

build: build/compile_commands.json
	cmake --build build -j$(NPROC)

# check only needs compile_commands.json (no full compile required).
check: build/compile_commands.json
	$(CLANG_FORMAT) --dry-run --Werror $(ALL_SRCS)
	$(CLANG_TIDY) -p build --warnings-as-errors='*' $(CC_SRCS)

# tidy fixes in place: clang-tidy transforms first, clang-format cleans up after.
tidy: build/compile_commands.json
	$(CLANG_TIDY) -p build --fix $(CC_SRCS)
	$(CLANG_FORMAT) -i $(ALL_SRCS)

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
