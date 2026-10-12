# ADR 0002: 设备端代码生成走 LLVM IR → NVPTX 后端 → PTX

## Status

Accepted（已拍板）

## Context

hacc 需要为 `__global__` / `__device__` 函数生成**设备端二进制**（PTX，可选 cubin）
供 coolrt 经 `coolModuleLoadData` 载入。可选路线：

1. **委托 nvcc / NVRTC**：把设备源码交给 nvcc 产出 PTX/cubin。
   - 优点：最快落地、产物真实。
   - 缺点：依赖 CUDA Toolkit；与 ADR 0001（自建前端）定位冲突；hacc 沦为包装器。
2. **直出 LLVM IR → NVPTX 后端 → PTX**：hacc 自己把 EDG 的 AST/IL 降为 LLVM IR，
   用 LLVM 的 NVPTX 目标生成 PTX。
   - 优点：不依赖 nvcc（仅需 LLVM + 驱动 PTX JIT）；与 EDG 前端形成完整自研链路；可控、可观测。
   - 缺点：实现量大；需正确处理 CUDA 地址空间、参数 ABI、NVVM intrinsic。
3. **NVRTC 运行时编译**：主机侧运行时编译设备源码字符串。
   - 优点：无需离线编译；缺点：仍是 NVIDIA 组件，且把编译推迟到运行期。

## Decision

采用 **路线 2：EDG AST/IL → LLVM IR（`nvptx64-nvidia-cuda`）→ LLVM NVPTX 后端 → PTX**。

- 目标三元组 `nvptx64-nvidia-cuda`；地址空间映射 `global=1, shared=3, constant=4, local=5, param=0`。
- 内置变量经 **NVVM intrinsic** 落地（`llvm.nvvm.read.ptx.sreg.*`、`llvm.nvvm.barrier0`、`llvm.nvvm.membar.*`）。
- 内核入口使用 `ptx_kernel` 调用约定与 `.visible .entry`；参数按 CUDA 参数 ABI 落入 `param` 地址空间。
- `-arch=sm_XY` → NVPTX `-mcpu=sm_XY`（并选择 PTX 版本）。
- 可选 `--emit=cubin`：对 PTX 调用 `ptxas` 汇编为 cubin。
- **不实现 PTX 级优化器**：优化交给 LLVM（opt pipeline）与 `ptxas`。

## Consequences

Positive:

- **不依赖 nvcc / CUDA Toolkit**：仅需 LLVM 与 NVIDIA 驱动（驱动做 PTX JIT），与 coolrt 的
  “仅依赖驱动”取向一致。
- **完整自研链路**：前端（EDG）+ 中间表示（LLVM IR）+ 后端（NVPTX）可控、可观测（可导出 IR/PTX）。
- **复用 LLVM 优化**：直接受益于 LLVM 的优化与 NVPTX 目标成熟度。

Negative / trade-offs:

- **实现量大**：需实现 EDG IL/AST 到 LLVM IR 的降级，覆盖地址空间、参数 ABI、intrinsic 映射。
- **工具链依赖 LLVM**：构建需可用的 LLVM 开发库，且需固定版本。
- **调试困难**：IR 降级 bug 需在 IR/PTX 层定位（以 golden 与 `ptxas` 校验缓解）。

## Alternatives Considered

| Option | Pros | Cons |
| --- | --- | --- |
| 委托 nvcc | 最快、产物真实 | 依赖 Toolkit；与自建前端定位冲突；沦为包装器 |
| NVRTC 运行期编译 | 无需离线编译 | 仍是 NVIDIA 组件；编译推迟到运行期 |
| 本决策：LLVM IR → NVPTX → PTX | 无 nvcc 依赖；全链路自研可控；复用 LLVM 优化 | 实现量大；LLVM 依赖与版本锁定；IR 层调试 |
