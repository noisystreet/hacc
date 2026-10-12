# hacc developer entry point.
# Quality gate used by pre-commit and CI: `make check`.

BUILD_DIR ?= build
BUILD_TYPE ?= Debug
CMAKE ?= cmake
CTEST ?= ctest
CLANG_FORMAT ?= clang-format
CLANG_TIDY ?= clang-tidy

SRC_DIRS := src tests samples include
FMT_FILES := $(shell find $(SRC_DIRS) -type f \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.hpp' \) 2>/dev/null)

.PHONY: all configure build test fmt fmt-check lint check clean

all: build

configure:
	$(CMAKE) -S . -B $(BUILD_DIR) \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DCMAKE_EXPORT_COMPILE_COMMANDS=ON

build: configure
	$(CMAKE) --build $(BUILD_DIR) -j

test: build
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure

fmt:
	@command -v $(CLANG_FORMAT) >/dev/null 2>&1 && \
		$(CLANG_FORMAT) -i $(FMT_FILES) || \
		echo "clang-format not found; skipping"

fmt-check:
	@command -v $(CLANG_FORMAT) >/dev/null 2>&1 && \
		$(CLANG_FORMAT) --dry-run --Werror $(FMT_FILES) || \
		echo "clang-format not found; skipping"

lint: configure
	@command -v $(CLANG_TIDY) >/dev/null 2>&1 && \
		$(CLANG_TIDY) -p $(BUILD_DIR) $(FMT_FILES) || \
		echo "clang-tidy not found; skipping"

check: fmt-check build test

clean:
	rm -rf $(BUILD_DIR)
