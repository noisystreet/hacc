# Changelog

本项目遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 与语义化版本（SemVer）。

## [Unreleased]

### Added

- 项目骨架（M0）：CMake 构建、`Makefile` 质量入口（`make check`）、`.clang-format`、
  `.gitignore`、`.pre-commit-config.yaml`。
- 三个可执行骨架：`hacc`（驱动，对标 `nvcc`）、`coolfe++`（CUDA 前端，对标 `cudafe++`）、
  `coolicc`（设备端编译器，对标 `cicc`）；均支持 `--version` / `-h`。
- CTest 冒烟测试（unit + 三个 CLI `--version`）。
- 随编译器分发的运行时头占位 `include/coolcc/hacc_runtime.h`。
- **M1.1 设备后端（LLVM IR → PTX）**：`coolicc` 接入 LLVM/NVPTX（`-DHACC_WITH_LLVM=ON`，
  系统 LLVM 锁 19.x，见 C1），支持 `--emit=ptx` / `-arch=sm_XX` / `-o`；最小内核产出
  `.visible .entry`。新增 golden 测试 `golden_device_empty_kernel`。未启用 LLVM 时走占位实现。
- **M1.2.0 前端（`.cu` → EDG IL）**：`coolfe++` 接入 EDG 前端（`-DHACC_WITH_EDG=ON`），
  以子进程调用 `cpfe` 产出 ① 二进制 IL（`-o`，供 `coolicc` 用 `il_read` 读回）与
  ② 人类可读文本 IL（`--il_display`，编译期 dump，规避 `cdisp` 对含属性 IL 的崩溃，见方案 §7.1）。
  新增 `--cpfe` / `--edg-base-dir` / `-o` / `--emit-il-text[=F]` / `-I` / `-D` 选项；
  路径解析优先级：显式参数 > `$HACC_CPFE`/`$HACC_EDG_BASE` > 构建期默认。
  新增单元测试 `unit_edg_invoke` 与 golden 测试 `golden_frontend_empty_kernel[_text]`。
  未启用 EDG 时 `coolfe++` 仍可构建（运行期需显式指定 `cpfe`）。
- **M1.2.1 设备端 IL 读回（`.cil` → routine 列表）**：`coolicc` 新增 `--emit=il-list`，
  链接 EDG 前端库（`-DHACC_WITH_EDG=ON`）用 `il_read` 读回二进制 IL，列出 file scope 的 routine
  并标记内核（`cool_global` 属性）。新增 `src/device/il_read_backend.{hpp,cpp}`（EDG 适配层，
  EDG 类型不外泄）与未启用 EDG 时的 stub；新增 golden 测试 `golden_device_il_list_empty_kernel`。
- **M1.2.2 EDG IL → LLVM IR（最小内核）**：`coolicc` 新增 `--from-il`，把 EDG 二进制 IL（`.cil`）
  翻译为 LLVM IR 并发射 PTX。新增 `src/device/il_to_llvm.{hpp,cpp}`（EDG 适配层，同时 include
  EDG 与 LLVM 头，对外只暴露 hacc 类型）与未启用 EDG/LLVM 时的 stub；一期只处理带 `cool_global`
  属性的**无参内核**，建空 `Function`（调用约定 `ptx_kernel`，属性 `nvptx-kernel`）。
    新增 golden 测试 `golden_device_il_to_ptx_empty_kernel`（`.cu` → `.cil` → PTX）。
- **M1.2.3 EDG IL → LLVM IR（标量参数 + 简单语句）**：`coolicc --from-il` 的翻译器扩展到
  标量参数与简单语句——参数（`tk_routine` 的 `param_type_list` → LLVM 参数）、语句
  （`stmk_expr` / `stmk_if` / `stmk_block` / `stmk_return` / `stmk_decl` / `stmk_init`）、
  表达式（`enk_variable` / `enk_constant` / `enk_operation`：算术/比较/赋值/取址/解引用/转换）、
  类型（`tk_integer` i8..i64 / `tk_float` float/double / `tk_pointer` ptr）。未覆盖的构造一律
  报错（fail loud），不静默产出错误 IR。新增 golden 测试 `golden_frontend_scalar_kernel` 与
  `golden_device_il_to_ptx_scalar_kernel`（`add_kernel(int*, int, int)`，校验参数/算术/分支）。
  实测细节见 [edg-il-reference §5.5](docs/edg-il-reference.md)。

### Changed

- **EDG 改为非 ASan 构建，`coolicc` 不再带 ASan**：EDG 在 Linux+Debug 下默认开 ASan
  （`EDG_CPFE_USE_ASAN`），迫使链接方 `coolicc` 也带 `-fsanitize=address`（发布隐患）。
  现 `tools/il_spike/fetch_edg.sh` 传 `-DEDG_CPFE_USE_ASAN=OFF` 重建 EDG，
  `CMakeLists.txt` 去掉 `coolicc` 的 `-fsanitize=address`；`coolicc` 不再依赖 `libasan.so`。
- **lizard 门禁阈值收紧到 CCN 10**：`Makefile` 的 `CCN_THRESHOLD` 由 15 收紧到 10，
  并把 8 个 CCN>10 的函数拆分为职责单一的小函数（行为等价）：`il_to_llvm` 的
  `map_integer_type` / `translate_arith` / `emit_compare` / `build_kernel`，`nvptx_backend`
  的 `emit_ptx`，`edg_invoke` 的 `run_cpfe_once`，`device/main` 与 `frontend/main` 的
  `parse_args`。

### Fixed

- **M1.2.0 `coolfe++` 产出的二进制 IL 无效**：`cpfe` 的 `--il_display` 与 `-o <真实文件>` 不能在同一次
  调用中同时使用（否则 `-o` 写出「显示变体」IL，`il_read` 读回报 `invalid intermediate language file`）；
  且 `--il_display` 不带 `-o` 时会把默认名 `<input-stem>.cil` 的显示变体写到当前目录，可能覆盖二进制 IL。
  现改为**分两次 `cpfe` 调用**（二进制趟 + 文本趟，文本趟用 `-o <临时文件>` 抑制默认写）。
  详见 [方案 §2.4/§7.1](docs/m1.2-il-to-llvm-plan.md) 与 [spike README](tools/il_spike/README.md)。
