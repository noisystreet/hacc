# hacc developer entry point.
# Quality gate used by pre-commit and CI: `make check`.

BUILD_DIR ?= build
BUILD_TYPE ?= Debug
CMAKE ?= cmake
CTEST ?= ctest
CLANG_FORMAT ?= clang-format
CLANG_TIDY ?= clang-tidy
LIZARD ?= lizard

# lizard 阈值（见 whitelizard.txt 的历史债务白名单）
CCN_THRESHOLD ?= 10
FUNC_LENGTH_THRESHOLD ?= 200
LIZARD_PATHS := src include tests
LIZARD_WHITELIST := whitelizard.txt

SRC_DIRS := src tests samples include
FMT_FILES := $(shell find $(SRC_DIRS) -type f \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.hpp' \) 2>/dev/null)

.PHONY: all configure build test fmt fmt-check lint ccn check check-strict clean

all: build

configure:
	$(CMAKE) -S . -B $(BUILD_DIR) \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DCMAKE_EXPORT_COMPILE_COMMANDS=ON

build: configure
	$(CMAKE) --build $(BUILD_DIR) -j

test: build
	@$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure > $(BUILD_DIR)/ctest.log 2>&1 || \
		{ cat $(BUILD_DIR)/ctest.log; exit 1; }
	@tail -n 3 $(BUILD_DIR)/ctest.log
	@skips=$$(grep -c '\*\*\*Skipped' $(BUILD_DIR)/ctest.log || true); \
	if [ "$$skips" != "0" ]; then \
		echo ""; \
		echo "⚠  $$skips 个测试被 SKIP —— 「全部通过」不含这些用例。"; \
		echo "   本机有 GPU 时先查驱动状态（nvidia-smi 正常 ≠ CUDA 可用）："; \
		echo "     lsmod | grep nvidia_uvm; ls -l /dev/nvidia-uvm; grep -i uvm /proc/devices"; \
		echo "   要强制零 SKIP：make check-strict"; \
	fi

# 工具缺失时跳过（打印提示），存在时违规必须让目标失败（退出码非 0）。
fmt:
	@if command -v $(CLANG_FORMAT) >/dev/null 2>&1; then \
		$(CLANG_FORMAT) -i $(FMT_FILES); \
	else \
		echo "clang-format not found; skipping"; \
	fi

fmt-check:
	@if command -v $(CLANG_FORMAT) >/dev/null 2>&1; then \
		$(CLANG_FORMAT) --dry-run --Werror $(FMT_FILES); \
	else \
		echo "clang-format not found; skipping"; \
	fi

lint: configure
	@if command -v $(CLANG_TIDY) >/dev/null 2>&1; then \
		$(CLANG_TIDY) -p $(BUILD_DIR) $(FMT_FILES); \
	else \
		echo "clang-tidy not found; skipping"; \
	fi

# 圈复杂度（CCN）与函数长度门禁；历史债务见 $(LIZARD_WHITELIST)。
ccn:
	@if command -v $(LIZARD) >/dev/null 2>&1; then \
		$(LIZARD) $(LIZARD_PATHS) -l cpp \
			-C $(CCN_THRESHOLD) -L $(FUNC_LENGTH_THRESHOLD) \
			-W $(LIZARD_WHITELIST) -w; \
	else \
		echo "lizard not found; skipping"; \
	fi

check: fmt-check ccn build test

# 严格模式：任何 SKIP 都算失败。用于「有 GPU 的 CI」或本机复验 ——
# 平时 make check 允许 SKIP（无 GPU 的开发机上必须能跑），但那时"全绿"是缺省的。
check-strict: fmt-check ccn build
	@$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure > $(BUILD_DIR)/ctest-strict.log 2>&1 || \
		{ cat $(BUILD_DIR)/ctest-strict.log; exit 1; }
	@skips=$$(grep -c '\*\*\*Skipped' $(BUILD_DIR)/ctest-strict.log || true); \
	if [ "$$skips" != "0" ]; then \
		cat $(BUILD_DIR)/ctest-strict.log; \
		echo ""; \
		echo "✗  $$skips 个测试被 SKIP，而 check-strict 要求零 SKIP。"; \
		exit 1; \
	fi
	@tail -n 3 $(BUILD_DIR)/ctest-strict.log

clean:
	rm -rf $(BUILD_DIR)
