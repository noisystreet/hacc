# CUDA 属性清单（A2 决策 (a) 的配套契约）

本清单固定 **hacc 一期**把 CUDA 限定符/关键字映射为 **EDG 未知属性**（`__attribute__((…))`）的
**名字与语义**，作为 `coolfe++`（前置改写/识别）与 `coolicc`（消费 IL）之间的**稳定契约**。

- 背景与决策：见 [README.md](README.md)「决策：选 (a)」与
  [../../docs/compiler-implementation-plan.md](../../docs/compiler-implementation-plan.md) §5.2/§5.3。
- 机制：EDG 构建期开启 `RECORD_UNRECOGNIZED_ATTRIBUTES`，未知属性以
  `file-scope attribute@…`（`kind: unrecognized`、带 `name`/`arguments`）进入 IL，
  并挂到实体 `source_corresp.attributes:`。
- **命名前缀 `cool_`**：避免与 EDG 已知属性（`aligned`/`section`…）及未来标准属性冲突。
- **一期只覆盖 S0 核心子集**（见方案 §9）；S1/S2 项标注为「后续」。

## 1. 执行空间（函数限定符）

| CUDA 关键字 | 属性名 | 签名 | 适用实体 | 语义 / 下游消费 |
| --- | --- | --- | --- | --- |
| `__global__` | `cool_global` | 无参 | 函数 | 内核入口；`coolicc` 生成 `ptx_kernel` 约定 + `.visible .entry`；主机端生成桩/注册 |
| `__device__` | `cool_device` | 无参 | 函数、变量 | 设备侧函数/变量；不生成主机桩 |
| `__host__` | `cool_host` | 无参 | 函数 | 仅主机侧；不进入设备映像 |
| `__host__ __device__` | 同时带 `cool_host` + `cool_device` | 无参 | 函数 | 双端可见（两属性并存，顺序无关） |

> 组合规则：`__global__` 与 `__host__`/`__device__` 互斥（CUDA 语义）；`coolfe++` 负责诊断。

## 2. 存储空间（变量限定符）

| CUDA 关键字 | 属性名 | 签名 | 适用实体 | 语义 / 下游消费 |
| --- | --- | --- | --- | --- |
| `__shared__` | `cool_shared` | 无参 | 变量 | 共享内存；LLVM 地址空间 **3** |
| `__constant__` | `cool_constant` | 无参 | 变量 | 常量内存；LLVM 地址空间 **4** |
| `__device__`（变量） | `cool_device` | 无参 | 变量 | 设备全局；LLVM 地址空间 **1** |
| `__managed__` | — | — | — | **明确排除**（方案 §9） |
| `__restrict__` | `cool_restrict` | 无参 | 指针参数 | **S1（M4.2）**；别名提示（映射 LLVM `noalias`） |

## 3. 地址空间（显式）

| CUDA 概念 | 属性名 | 签名 | 语义 / 下游消费 |
| --- | --- | --- | --- |
| 显式地址空间 | `cool_address_space(n)` | `(ci)` 整数常量 | 直接指定 LLVM 地址空间号；`n ∈ {0,1,3,4,5}`（param/global/shared/constant/local）；**M4.5 已消费** |

> 与 §2 的关系：`cool_shared`/`cool_constant`/`cool_device` 是**语义化**写法，
> `cool_address_space(n)` 是**底层**写法；二者可互相推导，`coolicc` 优先取语义化属性。
> 探针当前用 EDG 已知的 `address_space(3)` 验证「带参未知属性」的传递（见 README）。

## 4. 启动与提示（S1/S2）

| CUDA 概念 | 属性名 | 签名 | 阶段 | 备注 |
| --- | --- | --- | --- | --- |
| `__launch_bounds__(maxThreads, minBlocks)` | `cool_launch_bounds` | `(ci)` 或 `(ci,ci)` | S2 | 映射 `.maxntid` / `.minnctapersm` |
| `__forceinline__` | `cool_forceinline` | 无参 | S1 | 映射 `alwaysinline` |
| `__noinline__` | `cool_noinline` | 无参 | S1 | 映射 `noinline` |

## 5. 非属性项（不走属性通道）

| CUDA 概念 | 处理方式 | 说明 |
| --- | --- | --- |
| `<<<grid, block, smem, stream>>>` | **词法/预处理层改写** | 非 C++ 语法；改写为 `haccLaunch(...)` 形式（方案 §5.3/§6.2），**不是属性** |
| `threadIdx` / `blockIdx` / `blockDim` / `gridDim` / `warpSize` | **内建变量** | 经 NVVM intrinsic 落地（方案 §6.1），非属性 |
| `dim3` | **类型** | 随编译器头提供，非属性 |
| `float2/3/4`、`int2/3/4`、`double2`… | **内建向量类型** | 映射 LLVM 向量/结构（方案 §6.1），非属性 |
| `__syncthreads()` | **内建函数调用** | IL 中即 routine 调用；`coolicc` 映射 `llvm.nvvm.barrier0`（方案 §6.1）。**如需在 IL 中显式标记同步点，另议**（当前不引入属性） |
| `__threadfence*()` | **内建函数调用** | S1（M4.3）；映射 `llvm.nvvm.membar.*`（`gl`/`cta`/`sys`） |
| `atomicAdd` 等原子内建 | **内建函数调用** | S1（M4.4）；映射 `llvm.nvvm.atomic.*`（按类型/地址空间后缀） |

## 6. 与验证脚本的对应

`tools/il_spike/verify_attrs.sh` 的 `EXPECTED` 与探针 `inputs/kernel_probe_attr2.cpp`
覆盖 §1–§3 的核心项：

| 清单项 | 探针是否覆盖 |
| --- | --- |
| `cool_global` / `cool_device` / `cool_host` | ✅ |
| `cool_shared` / `cool_constant` | ✅ |
| `cool_address_space(n)` | 以 EDG 已知 `address_space(3)` 代替（验证带参未知属性传递） |
| `cool_restrict` / `cool_launch_bounds` / `cool_forceinline` / `cool_noinline` | `cool_restrict` ✅（M4.0）；其余 ⬜ 后续（S2） |

## 7. 变更规则

- 本清单是 `coolfe++` ↔ `coolicc` 的**契约**：新增/改名/改签名需同步
  `verify_attrs.sh`、探针与本文档，并在方案 §5.3 记录。
- 升级到路线 (b)（正式注册属性族）时，本清单的**名字**保持不变，仅把 `kind: unrecognized`
  换成稳定 `ak_*`（见 README「决策」的升级路径）。
