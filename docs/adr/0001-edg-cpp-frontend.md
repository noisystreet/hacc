# ADR 0001: 采用开源的 EDG C++ 前端作为 hacc 前端

## Status

Accepted（已拍板）

## Context

hacc 的目标是解析**真实 CUDA C++ 子集**（`__global__` / `<<<>>>` / `dim3` / 内置变量等），
而非自研简化语言。这意味着需要一个**工业级 C++ 前端**（词法 / 语法 / 语义 / 模板实例化 / 重载解析）。

可选路线：

1. **从零手写递归下降前端**：教学透明、无外部依赖，但只覆盖极小子集，C++ 解析成本极高。
2. **基于 Clang/LLVM 库（libTooling）**：成熟，但需处理 Clang AST 与 CUDA 语义、并承担 Clang 版本耦合。
3. **基于 EDG C++ 前端**：极高兼容性的商用级前端。

背景事实：EDG（Edison Design Group）的 C++ 前端于 **2026-09-30 开源**，
仓库 `github.com/edgcpp/compiler`，许可证 **Apache-2.0 WITH LLVM-exception**，
由 The C++ Alliance（Boost 的非营利母体）托管；仓库自带 **EDG IL**（中间语言）读写工具、
源到源后端（C / C++）与 demangler。EDG 历史上被 NVIDIA、Microsoft、MathWorks、Veracode 等用于
编译器、静态分析与工具的 C/C++ 前端，**是仅剩的与 gcc/clang/MSVC 并列的四大 C++ 前端之一**。

## Decision

采用 **EDG C++ 前端**作为 hacc 的解析与语义分析前端。

- hacc 在其之上做 **CUDA 扩展识别层**（execution space 关键字、`__shared__` / `__constant__`、
  `<<<>>>` 语法糖改写），EDG 负责标准 C++ 部分。
- 前端与后端的衔接点固定为「**EDG AST / IL → hacc 中间表示（LLVM IR）**」，
  分两阶段落地：
  - **阶段 A（bring-up）**：以 **IL 文件**为接口（`edgcpfe` 产出 IL，hacc 用 IL 读取工具消费）。
    进程隔离、IL 可保存比对、便于教学与调试。
  - **阶段 B（生产）**：链接 EDG 前端库并实现其**后端回调接口**，单进程内直接构建 LLVM IR。
- **顺序已拍板：先阶段 A（IL）后阶段 B（回调）**。先以 IL 跑通端到端（M0 spike 需先验证 EDG IL 是否保留
  CUDA 相关属性：地址空间、`__shared__`/`__constant__`、同步点）；若 IL 表达力不足，则**提前切换**到阶段 B。
- **M0 IL spike 结论（已核验，2026-10）**：EDG IL 有一等的属性表示，但默认丢弃未知属性；
  **已定采用「构建期开启 `RECORD_UNRECOGNIZED_ATTRIBUTES`」路线（零 EDG 源码补丁）**，
  使 CUDA 属性以 `__attribute__((…))` 承载并随 IL 传递，**阶段 A 内成立，不触发切阶段 B**。
  细节见 [`../tools/il_spike/README.md`](../tools/il_spike/README.md) 与方案 §5.2。
- 通过**适配层**隔离 EDG 具体 API，避免其版本变化扩散到设备端代码生成。

## Consequences

Positive:

- **真实 C++ 支持**：模板、重载、命名空间由成熟前端处理，hacc 只聚焦 CUDA 扩展与代码生成。
- **兼容性高**：EDG 以“比其他前端更互相兼容”著称，利于解析用户真实 `.cu` 代码。
- **许可证宽松**：Apache-2.0 WITH LLVM-exception 允许嵌入与商用。
- **自带 IL 工具**：阶段 A 可用 IL 做进程隔离与黄金文件回归。

Negative / trade-offs:

- **集成复杂度高**：前端庞大，后端/IL 接口需在 EDG 接入阶段摸清并固定版本。
- **非英文社区资料有限**：EDG 长期闭源，公开集成经验少于 Clang。
- **EDG 采用方式已定**：**不 vendor**，由 CMake 按需拉取并固定 commit（可用 `-DHACC_EDG_ROOT` 指向本地副本），
  见 [ADR 0005](0005-licensing-and-edg-distribution.md)。

## Alternatives Considered

| Option | Pros | Cons |
| --- | --- | --- |
| 从零手写前端 | 教学透明、无依赖 | 只能极小子集；C++ 解析成本不可接受 |
| Clang/LLVM libTooling | 生态成熟、资料多 | 与 Clang 版本强耦合；CUDA 语义需自建；许可同源但更重 |
| 本决策：EDG 前端 | 工业级、许可宽松、自带 IL | 集成复杂、公开资料少、依赖需按需拉取（不 vendor） |
