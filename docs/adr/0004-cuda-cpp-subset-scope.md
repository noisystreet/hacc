# ADR 0004: CUDA C++ 子集范围与分期

## Status

Accepted（已拍板）

## Context

hacc 的目标是解析**真实 CUDA C++ 子集**而非自研语言（见 [ADR 0001](0001-edg-cpp-frontend.md)），
但“支持真实 CUDA C++”是一个巨大的谱系。需要一个**明确、可交付、可测试**的子集边界，
与 coolrt 已有的能力（内存 / 流 / 事件 / 模块与内核）以及对齐 cudart 的编程模型目标相匹配。

约束与依据：

- coolrt 公开 ABI 覆盖：版本 / 错误 / 设备 / 内存 / 流 / 事件 / 模块与内核
  （`coolMalloc` / `coolMemcpy` / `coolStream*` / `coolModuleLoadData` / `coolLaunchKernel` 等）。
- 关联 Non-Goal（coolruntime）：不做 CUDA C++ 编译前端；内核映像由外部工具产出。
- hacc 侧 Non-Goal：不做分离编译 / RDC、动态并行、设备端异常/RTTI/协程、CUDA 库。

## Decision

hacc 支持的 CUDA C++ 子集按**三阶段**推进，边界如下。

### S0 核心（首个可交付）

- 函数标注：`__global__` / `__device__` / `__host__`。
- 启动语法：`<<<grid, block, sharedMem, stream>>>` 与默认参数形式。
- 内置变量：`threadIdx` / `blockIdx` / `blockDim` / `gridDim`（`dim3`）、`warpSize`。
- 类型：`dim3`；内建向量类型 `float2/3/4`、`int2/3/4`、`double2` 等。
- 存储限定：`__shared__`、`__constant__`、设备全局 `__device__` 变量。
- 同步：`__syncthreads()`。
- 语言子集：普通函数调用、算术/逻辑/位运算、控制流、数组与指针、`extern "C"` 内核。
- 构建：`-arch=sm_XY`、`-I`/`-D`、`--emit=ptx|cubin`、主机桩 + 注册 + 链接。

### S1 内存与原子

- `__threadfence()` / `__threadfence_block()` / `__threadfence_system()`。
- 原子内建：`atomicAdd` / `atomicSub` / `atomicExch` / `atomicMin` / `atomicMax` / `atomicCAS` 等。
- `__restrict__`；指向 global / shared / local 的指针区分与地址空间传播。
- 主机侧与 coolrt `cool*` 内存/流 API 混用。

### S2 进阶（可选）

- 内核与 `__device__` 函数**模板**。
- `__launch_bounds__` 提示；内建数学函数（`__fmaf`、`sinf`、`sqrtf` 等）。
- `--emit=cubin` + `ptxas` 集成；`cudaFuncGetAttributes` 类元数据（若 coolrt 后续提供对应能力）。

### 明确排除（Non-Goals）

- 分离编译 / RDC；跨 TU 设备函数链接。
- 动态并行（device 侧 `<<<>>>`）。
- `__managed__`；统一内存高级特性。
- 设备端 C++ 异常 / RTTI / 协程 / 虚拟函数 / 函数指针。
- CUDA 库（cuBLAS/cuFFT/…）、多 GPU / Peer / Graph 编程。

## Consequences

Positive:

- **边界清晰、可测试**：每阶段有明确的 e2e 与 golden 用例，便于逐格交付与回归。
- **与 coolrt 能力对齐**：S0/S1 恰好落在现有 ABI 能支撑的范围内，无需运行时改动。
- **教学可读**：S0 即可跑通 `vectorAdd` 级别的完整示例，读者可对照 cudart 理解编程模型。

Negative / trade-offs:

- **兼容性有限**：真实工程 CUDA 代码常依赖 RDC、模板、异常等，超出子集即无法编译，需明确报错。
- **后续扩展有增量成本**：S2 的模板与数学库需要额外的 IR 降级与 intrinsic 映射。
- **边界维护**：子集定义需随 cudart 演进与用户反馈持续维护。

## Alternatives Considered

| Option | Pros | Cons |
| --- | --- | --- |
| 全量 CUDA C++ | 兼容性最好 | 不现实：等同重写 nvcc |
| 极简子集（仅标量内核 + `<<<>>>`） | 最快落地 | 无法支撑 `__shared__`/向量/原子等基本教学场景 |
| 本决策：S0/S1/S2 分期子集 | 边界清晰、与运行时能力对齐、可逐阶段交付 | 兼容性有限、需持续维护边界 |
