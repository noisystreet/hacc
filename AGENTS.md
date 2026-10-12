# AGENTS.md

## Project Identity

- Project: hacc
- Purpose: coolrt 的**编译前端**，把 **CUDA C++ 子集**编译为设备端 PTX/cubin + 主机端桩与注册，
  在编程模型语义上对齐 cudart
- Tech stack: CMake、C++20；前端基于开源的 **EDG C++ 前端**（Apache-2.0 WITH LLVM-exception）；
  设备端代码生成基于 **LLVM / NVPTX**；CTest
- Target runtime/platform: Linux（x86_64）；产物链接 [coolrt](../coolruntime)；驱动需支持 PTX JIT

## Directory Overview

```text
hacc/
├── include/coolcc/    # 随编译器分发的运行时头（主机桩/注册支持，被生成的 host 代码 include）
├── src/
│   ├── driver/          # 命令行驱动、阶段编排、子进程
│   ├── frontend/        # coolfe++：EDG 集成、CUDA 扩展识别、语义标注
│   ├── device/          # coolicc：AST/IL → LLVM IR → NVPTX → PTX/cubin
│   ├── host/            # 主机 TU 生成：桩、<<<>>> 改写、内嵌映像与注册
│   └── support/         # 诊断、选项、文件/临时文件、进程封装
├── tests/               # unit/ golden/ e2e/
├── samples/             # 可运行 CUDA 子集示例
└── docs/                # 实施方案与 ADR
```

## Hard Constraints

- Module dependency direction（单向）：`driver` → `frontend` / `device` / `host` → `support`。下层不得反向依赖上层。
- **前端与后端的衔接点固定**为「EDG AST/IL → hacc 的 LLVM IR」。EDG 的具体 API 须经**适配层**
  隔离，不得让其类型/头文件泄漏到 `device/`、`host/`。
- 与 coolrt 的对接**只使用其公开 C ABI**（`coolModuleLoadData` / `coolModuleGetFunction` /
  `coolLaunchKernel` 等）；**默认不修改 coolrt**。若确需改动，须先记录 ADR。
- 生成的主机代码为**普通 C++**（不使用 nvcc），只 include coolrt 的 `include/cool/*.h` 与
  hacc 随附的 `include/coolcc/hacc_runtime.h`。
- 输出产物（PTX/cubin、生成的主机代码、链接产物）**可复现**：相同的输入与选项应产出相同结果。
- Architecture docs that require human approval before editing:
  - `docs/compiler-implementation-plan.md`、`docs/adr/**`、`README.md`。
- Security red lines:
  - Do not commit secrets, tokens, certificates, cookies, or private keys.
  - Do not vendor third-party source without preserving its license and attribution.

## Required Commands

```bash
make check      # fmt-check + build + test（提交前必跑）
make test       # 构建并运行全部测试
make lint       # clang-tidy（未安装时跳过）
```

## Coding Rules

- 优先遵循仓库既有风格；格式化由 `.clang-format` 统一。
- 公开前缀：hacc 自身工具/库符号用 `hacc`/`hacc::`；**生成的**主机代码符号用 `__cool_` 前缀，
  随附运行时头的符号置于 `hacc::` 命名空间。
- 优先 RAII（`std::unique_ptr` + 自定义 deleter）；使用固定宽度整型，避免隐式窄化。
- 编译器诊断面向教学：错误信息应包含阶段、源位置与成因，避免只抛底层 LLVM/EDG 原始信息。
- 行为变更必须有测试覆盖；对 golden 文件（PTX / 主机桩）的变更需显式说明。

## Documentation Rules

- 语言范围变更更新 [ADR 0004](docs/adr/0004-cuda-cpp-subset-scope.md) 与方案 §9。
- 架构/流水线变更更新 `docs/compiler-implementation-plan.md` 或新增 ADR。
- 用户可见变更更新 `CHANGELOG.md`。
