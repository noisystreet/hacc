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
