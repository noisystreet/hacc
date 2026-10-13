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
