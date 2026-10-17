# M4 方案：子集 S1（原子 / `__threadfence` / 设备全局 `__device__` / `__restrict__`）

> 状态：**进行中**（2026-10）。M4.0 / M4.1 / M4.2 / M4.3 已完成；M4.4–M4.6 规划中。本文为 M4 的**实施方案**，落地后逐节回写「已完成」。
> 上游：[compiler-implementation-plan.md](compiler-implementation-plan.md) §9（子集范围 S1）、§10（里程碑 M4）。
> 前置：M3 已完成（子集 S0 全部落地，见 [m3-subset-s0-plan.md](m3-subset-s0-plan.md)）。
> 相关：[ADR 0004](adr/0004-cuda-cpp-subset-scope.md)（子集范围 S1）、
> [cuda-attributes.md](../tools/il_spike/cuda-attributes.md)（属性契约）、
> [TODO.md](TODO.md)（B4 跨项目协调）。
> 环境事实：本机有 GPU（RTX 4060 Laptop），PTX 经驱动 JIT 可载入 → e2e 可真验证。

## 1. 目标与范围

**目标**：补齐 **S1 子集**（[ADR 0004](adr/0004-cuda-cpp-subset-scope.md) §S1）——内存语义与原子，
使「S1 级 CUDA 源码」可编译运行。

**M4 范围**：

| 项 | 内容 | 依赖 |
| --- | --- | --- |
| **M4.0 属性契约扩展** | `cool_restrict`（→ `noalias`）、`cool_device` 变量（→ addrspace 1）纳入契约与探针 | 无 |
| **M4.1 设备全局 `__device__` 变量** | 文件作用域 `__device__` 变量 → LLVM `addrspace(1)` 全局；内核内可见 | 无（纯设备侧） |
| **M4.2 `__restrict__`** | 指针参数 `__restrict__` → LLVM `noalias` 参数属性 | 无（纯设备侧） |
| **M4.3 `__threadfence*`** | `__threadfence` / `_block` / `_system` → `llvm.nvvm.membar.*` | **coolrt 需新增内建声明** |
| **M4.4 原子内建** | `atomicAdd` / `atomicSub` / `atomicExch` / `atomicMin` / `atomicMax` / `atomicCAS` 等 → NVVM atomic | **coolrt 需新增内建声明** |
| **M4.5 指针地址空间传播** | 指向 global / shared / local 的指针区分与地址空间传播 | 依赖 M4.1 |
| **M4.6 samples + 边界用例** | `samples/` 新增原子/`__threadfence` 示例；边界用例 | 依赖上述 |

**不在 M4 范围**：模板 / `__launch_bounds__` / 数学函数（S2）、cubin/`ptxas`（M6）、
分离编译/RDC、动态并行、`__managed__`（Non-Goal）。

> **`cool*` 主机 API 混用**（S1 最后一项）：`cudaMalloc/cudaMemcpy` 等 `cool*` 主机 API 的混用
> 属**主机侧**能力，M2 已具备 `coolMalloc`/`coolMemcpy` 等（见 [m2-host-path-plan.md](m2-host-path-plan.md)）。
> M4 只需在 samples/e2e 中**验证混用可行**，无需新增编译器能力（见 §3.6）。

## 2. 调研结论（现状与约束）

### 2.1 设备后端现状（`src/device/il_to_llvm.cpp`）

- **类型映射**（`map_type`）：覆盖 `tk_void` / `tk_integer` / `tk_float` / `tk_pointer` / `tk_typeref` /
  `tk_struct`（含向量）/ `tk_array`。**`tk_pointer` 一律返回 `PointerType::get(ctx, 0)`（地址空间 0）**——
  **未做地址空间传播**（S1 的「指向 global/shared/local 的指针区分」落点，见 §3.5）。
- **属性识别**：`has_attribute(attrs, name)` 已存在，当前用于 `cool_global`（识别内核）、
  `cool_shared`（内核内共享内存）、`cool_constant`（文件作用域常量）。**未处理** `cool_device`（变量）、
  `cool_restrict`。
- **全局变量**：文件作用域 `__constant__` 已发射（`addrspace(4)`，`seed_constant_globals`）；
  **文件作用域 `__device__` 变量尚未发射**。
- **内建函数**：`try_translate_builtin_call` **只认 `__syncthreads`**（→ `llvm.nvvm.barrier0`）；
  其余函数调用 **fail loud**（`不支持的函数调用（M1.2.4 仅覆盖内建函数 __syncthreads）`）。
- **参数**：标量 / 指针 / 聚合（向量）已支持；**未处理参数属性**（`__restrict__` → `noalias`）。

### 2.2 属性契约（`tools/il_spike/cuda-attributes.md`，已定）

| 源码限定符 | 属性名 | LLVM 地址空间 | 阶段 |
| --- | --- | --- | --- |
| `__shared__` | `cool_shared` | **3** | S0（已完成） |
| `__constant__` | `cool_constant` | **4** | S0（已完成） |
| `__device__`（变量） | `cool_device` | **1** | **S1（M4.1）** |
| `__restrict__` | `cool_restrict` | —（→ `noalias`） | **S1（M4.2）** |
| 显式 | `cool_address_space(n)` | `n ∈ {0,1,3,4,5}` | 已定 |

> `cool_restrict` / `cool_device`（变量）的**未知属性传递**已在 M4.0 补探针并实测通过
> （`verify_attrs.sh` exit 0；`cool_restrict` 挂在参数实体上，见 cuda-attributes.md §6）。

### 2.3 coolrt 现状（`coolruntime/include/`）

| 项 | 现状 | M4 影响 |
| --- | --- | --- |
| `__device__` 变量宏 | ✅ 有（`device_builtins.h`：`#define __device__ __attribute__((cool_device))`） | M4.1 可直接用 |
| `__restrict__` 宏 | ✅ 有（`device_builtins.h`：`#define __restrict__ __attribute__((cool_restrict))`，仅 `__COOLCC__` 下；M4.0 已补） | M4.2 可直接用 |
| 原子内建（`atomicAdd` 等） | ❌ **无声明**（`device_builtins.h` 仅 `__syncthreads`） | **M4.4 需 coolrt 新增声明** |
| `__threadfence*` | ❌ **无声明** | **M4.3 需 coolrt 新增声明** |
| `coolMalloc` / `coolMemcpy` 等主机 API | ✅ 有（`cool_runtime_api.h`） | M4.6 混用验证可直接用 |

> **关键约束**：M4.3 / M4.4 涉及 **coolrt 侧改动**（新增内建声明），属**跨项目**。
> 需与 coolrt 维护者对齐（类似 TODO B4 的跨项目协调）。
>
> **分工原则**（沿用 `device_builtins.h` 头注释）：限定符宏（`__device__` 等）→ 映射为
> `__attribute__((cool_*))`，由 hacc 前端识别；内建变量/函数 → 由 hacc 设备端（`coolicc`）
> **按名字**映射到 NVVM intrinsic。故 coolrt 只需**声明**（供 `.cu` 编译通过），
> 语义映射在 hacc 侧。

## 3. 方案设计

### 3.1 M4.0 属性契约扩展（纯文档 + 探针）——**已完成**

1. **`cuda-attributes.md`**：`cool_restrict`（§2）与 `cool_device` 变量（§2）已提升为 S1 落地项；
   §5「非属性项」补 `__threadfence*`（→ `llvm.nvvm.membar.*`）与原子内建（→ `llvm.nvvm.atomic.*`）映射说明。
2. **探针**（`tools/il_spike/inputs/kernel_probe_attr2.cpp`）：新增 `cool_device` **变量**用例
   （`d_global_var`）与 `cool_restrict` **指针参数**用例（`cool_restrict_probe`）；
   `verify_attrs.sh` 的 `EXPECTED` 补 `cool_restrict`。
3. **`device_builtins.h`**（coolrt）：补 `__restrict__` 宏（`#define __restrict__ __attribute__((cool_restrict))`，
   仅 `__COOLCC__` 下展开；主机侧不覆盖，保持标准关键字原义）。

**验证（已实跑）**：`tools/il_spike/verify_attrs.sh` → **exit 0**，7 个属性名全部 OK：

```text
OK   attribute in IL: cool_shared
OK   attribute in IL: cool_global
OK   attribute in IL: cool_device
OK   attribute in IL: cool_host
OK   attribute in IL: cool_constant
OK   attribute in IL: cool_restrict
OK   attribute in IL: address_space
OK   IL contains 'kind: unrecognized' entries
```

### 3.2 M4.1 设备全局 `__device__` 变量（纯设备侧）——✅ 已完成

**设备侧**（`il_to_llvm`）：

1. **文件作用域 `__device__` 变量**（带 `cool_device` 属性）→ 发射 LLVM 全局变量：
   `@<name> = addrspace(1) global <T> <init>`（`ExternalLinkage` + **原名**，供运行时按名解析，
   与 `__constant__` 一致）。
   - `isConstant=false`（避免 LLVM 把 load 折叠成 initializer）。
   - 初始化：`__device__ int x = 5;` → 常量初始化；无初始化 → `zeroinitializer`。
2. **内核内引用**：经 `KernelContext` 并入 `locals`（类似 `seed_constant_globals`，新增
   `seed_device_globals`），使内核内引用可解析。
3. **内核内 `__device__` 变量**：CUDA 允许函数内 `__device__`？——**不允许**（`__device__` 变量仅文件作用域），
   报错（fail loud）。

**验证**：golden PTX 校验 `.global` 段 + `ld.global` / `st.global`；e2e 用设备全局变量做累加。

> **已完成（M4.1）**：`il_to_llvm.cpp` 新增 `kCoolDeviceAttributeName` → `addrspace(1)` 全局
> （`ExternalLinkage` + 原名、`isConstant=false`）；`initk_static` 时用 `translate_constant` 取初值
> （其余零初始化）。`KernelContext` 增 `device_globals`，`seed_file_scope_globals` 同时 seed
> `__constant__` / `__device__`；内核内 `__device__` 声明 fail loud。
> 验证：`tests/golden/e2e/dev_global.cu`（`__device__ int g_counter = 100`），golden 校验
> `.visible .global ... g_counter = 100` + `ld.global.u32` / `st.global.u32`；e2e 在 RTX 4060 上
> 跑通（`g_counter == 105`）。`make check` 73/73 全绿。

**风险**：`__device__` 变量的**主机侧可见性**——CUDA 中 `__device__` 变量主机侧不可直接访问
（需 `cudaMemcpyFromSymbol`）。M4.1 一期**只做设备侧**，主机侧符号访问延后（见 §6 待确认）。

### 3.3 M4.2 `__restrict__`（纯设备侧）——✅ 已完成

**设备侧**（`il_to_llvm`）：

1. **指针参数带 `cool_restrict` 属性** → 给 LLVM 参数加 `noalias` 属性
   （`llvm::Attribute::NoAlias`）。
2. **非指针参数带 `__restrict__`**：CUDA 语义下 `__restrict__` 仅对指针有意义，非指针报错或忽略
   （建议 fail loud，与「未覆盖构造一律 fail loud」一致）。

**验证**：golden PTX 校验参数带 `noalias`（IR 层校验更直接，可加 IR 片段单测）。

> **已完成（M4.2）**：`il_to_llvm.cpp` 新增 `kCoolRestrictAttributeName` → `apply_param_noalias`
> 给对应参数加 `llvm::Attribute::NoAlias`。EDG 的属性位置有**两条路径**（`param-type.attributes`
> 用于 `int * __restrict__ a`；合成 `tk_typeref`/`for_type_attributes` 节点的
> `source_corresp.attributes` 用于 `int __restrict__ v`），故 `param_has_restrict` 两路都查；
> 非指针形参带 `__restrict__` → fail loud（带源位置）。
> 为在 IR 层可校验，新增 `emit_ir_from_il` API 与 `coolicc --emit=llvm-ir`（PTX 层看不到 `noalias`）；
> `emit_ptx_from_il` 与它共用抽出的 `build_ir_from_il`（同时拆出 `emit_file_scope_globals`
> 以控制圈复杂度）。
> 验证：`tests/golden/e2e/restrict_param.cu` → IR 含
> `define ptx_kernel void @axpy(ptr noalias %0, ptr noalias %1)`；`--emit=ptx` 仍产出
> `.visible .entry axpy`；`diag_restrict_non_pointer.cu` 报错且带行号；e2e 在 RTX 4060 上跑通
> （`restrict OK`）。`make check` 81/81 全绿。

**风险**：`noalias` 是**优化提示**，不影响正确性；但若前端误判（如把非指针标 `noalias`）可能引入
错误优化——故**严格限定仅指针参数**。

### 3.4 M4.3 `__threadfence*`（跨项目）——✅ 已完成

**coolrt 侧**（`device_builtins.h`，仅 `__COOLCC__` 下）：

```c
void __threadfence(void);
void __threadfence_block(void);
void __threadfence_system(void);
```

**设备侧**（`il_to_llvm`）：`try_translate_builtin_call` 扩展名字映射：

| 内建 | NVVM intrinsic |
| --- | --- |
| `__threadfence` | `llvm.nvvm.membar.gl` |
| `__threadfence_block` | `llvm.nvvm.membar.cta` |
| `__threadfence_system` | `llvm.nvvm.membar.sys` |

**验证**：golden PTX 校验 `membar.gl` / `membar.cta` / `membar.sys`；e2e 用 `__threadfence` 做
跨块可见性（可选，较难稳定验证，可只做 golden）。

> **已完成（M4.3）**：coolrt `device_builtins.h`（`__COOLCC__` 下）已声明三个无参内建；
> `il_to_llvm.cpp` 的 `try_translate_builtin_call` 改为**表驱动**（`kNoArgBuiltins`
> 映射 `__syncthreads` / 三个 fence → `void()` NVVM intrinsic），新增名字只需加一行。
> 验证：`tests/golden/e2e/threadfence.cu` → PTX 依次含 `membar.gl` / `membar.cta` / `membar.sys`
> （三条独立断言，避免「命中一个即通过」）；e2e 在 RTX 4060 上跑通（`threadfence OK: out[0]=2`）。
> 跨块可见性语义需多 block 竞争窗口，难以稳定复现，故强断言放在 PTX 层。`make check` 88/88 全绿。

### 3.5 M4.4 原子内建（跨项目）

**coolrt 侧**（`device_builtins.h`，仅 `__COOLCC__` 下）：声明原子内建（按 CUDA 签名，覆盖常用类型）：

```c
int   atomicAdd(int* address, int val);
unsigned int atomicAdd(unsigned int* address, unsigned int val);
float atomicAdd(float* address, float val);
int   atomicSub(int* address, int val);
int   atomicExch(int* address, int val);
int   atomicMin(int* address, int val);
int   atomicMax(int* address, int val);
int   atomicCAS(int* address, int compare, int val);
/* …按需扩展 unsigned / unsigned long long / float 等重载 */
```

**设备侧**（`il_to_llvm`）：`try_translate_builtin_call` 扩展名字映射到 NVVM atomic intrinsic：

| 内建 | NVVM intrinsic（示意） |
| --- | --- |
| `atomicAdd` | `llvm.nvvm.atomic.add.*` |
| `atomicSub` | `llvm.nvvm.atomic.sub.*` |
| `atomicExch` | `llvm.nvvm.atomic.exch.*` |
| `atomicMin` / `atomicMax` | `llvm.nvvm.atomic.min.*` / `.max.*` |
| `atomicCAS` | `llvm.nvvm.atomic.cas.*` |

> **注意**：NVVM atomic intrinsic 的**具体名字与签名**需以 LLVM 19 的 NVPTX 后端为准
> （`llvm.nvvm.atomic.*` 系列，含地址空间/类型后缀）。**M4.4 开工前需 spike 确认**
> （见 §6 待确认 1）。

**验证**：golden PTX 校验 `atom.global.add.*` 等；e2e 用 `atomicAdd` 做直方图/归约。

**风险**：原子 intrinsic 的**类型/地址空间后缀**较多，映射表需逐类型核对；建议先做 `int` / `float`
的 `atomicAdd`，再扩展。

### 3.6 M4.5 指针地址空间传播（依赖 M4.1）

**现状**：`map_type` 的 `tk_pointer` 一律返回 `addrspace(0)` 指针。S1 要求「指向 global / shared /
local 的指针区分与地址空间传播」。

**方案**（分两步，避免一次性大改）：

1. **最小可用**：M4.1 的 `__device__` 全局变量引用经 `locals` 解析为 `addrspace(1)` 全局指针，
   GEP / load / store 自然带 `addrspace(1)`（LLVM 不透明指针下，地址空间由**指针类型**承载）。
2. **完整传播**：若需区分「指向 shared 的指针」等，需在 `map_type` 的 `tk_pointer` 分支读取
   `cool_address_space(n)` 属性（或从被指类型推导），返回对应地址空间的 `PointerType`。
   **一期可先不做**，视 M4.1/M4.4 的实际需要决定（见 §6 待确认 2）。

**验证**：golden PTX 校验 `ld.global` / `st.global`（addrspace 1）与 `ld.shared`（addrspace 3）区分。

### 3.7 M4.6 samples + 边界用例

- `samples/`：新增 `atomic_reduce.cu`（`atomicAdd` 归约）、`device_global.cu`（`__device__` 变量）、
  `threadfence.cu`（`__threadfence`，可选）。
- 边界用例（fail loud）：内核内 `__device__` 变量、非指针 `__restrict__`、未覆盖的原子类型。
- `tests/CMakeLists.txt` 为每个示例加 `sample_<name>_build` / `_run`（无设备按 77 SKIP）。

## 4. 分步实施（建议）

| 步骤 | 内容 | 依赖 | 验证 | 难度 |
| --- | --- | --- | --- | --- |
| **M4.0** | 属性契约扩展（`cool_restrict` / `cool_device` 变量）+ 探针 | 无 | `verify_attrs.sh` | 低 | ✅ **已完成** |
| **M4.1** | 设备全局 `__device__` 变量（addrspace 1） | 无 | golden + e2e | 中 |
| **M4.2** | `__restrict__` → `noalias` | 无 | golden / IR 单测 | 低 |
| **M4.3** | `__threadfence*` → `llvm.nvvm.membar.*` | **coolrt 声明** | golden | 低 |
| **M4.4** | 原子内建 → NVVM atomic | **coolrt 声明** | golden + e2e | 高 |
| **M4.5** | 指针地址空间传播（按需） | M4.1 | golden | 中 |
| **M4.6** | samples + 边界用例 | 上述 | samples 全绿 | 低 |

> **推荐顺序**：先做**无 coolrt 依赖**的 M4.0 / M4.1 / M4.2（可立即交付），
> 再与 coolrt 对齐后做 M4.3 / M4.4（跨项目）；M4.5 视需要；M4.6 收尾。

## 5. 风险与缓解

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| **coolrt 无原子 / `__threadfence` 声明** | 高（M4.3/M4.4 阻塞） | 跨项目对齐（TODO B4 同类）；先做无依赖项 |
| **NVVM atomic intrinsic 名字/签名不确定** | 高（M4.4 核心） | 开工前 spike（手写 IR + `coolicc --emit=ptx` 验证） |
| `__device__` 变量主机侧可见性 | 中 | 一期只做设备侧；主机侧符号访问延后 |
| 指针地址空间传播范围不清 | 中 | 先做最小可用（M4.1），完整传播按需 |
| `noalias` 误标引入错误优化 | 中 | 严格限定仅指针参数 |
| `__threadfence` e2e 难稳定验证 | 低 | 以 golden PTX 为主 |

## 6. 待确认（实施前）

1. **NVVM atomic intrinsic 的具体名字与签名**（LLVM 19 NVPTX）——需 spike 确认（M4.4 前置）。
2. **指针地址空间传播的范围**：一期是否需完整区分 global/shared/local 指针，还是仅 M4.1 的最小可用？
3. **coolrt 侧内建声明由谁改**：`device_builtins.h` 新增原子/`__threadfence` 声明，
   属跨项目改动，需与 coolrt 维护者对齐。（`__restrict__` 宏已由 M4.0 补齐，不再待确认。）
4. **`__device__` 变量的主机侧访问**：是否需 `coolMemcpyFromSymbol` 类 API（类似 M3.2 的
   `coolMemcpyToSymbol`）？一期是否延后？
5. **原子内建的类型覆盖范围**：一期覆盖哪些类型（`int` / `unsigned` / `float` / `unsigned long long`）？

## 7. 与 M3 / S0 的衔接

- **设备侧**：M3 的类型系统（向量/聚合/数组）与存储限定符（shared/constant）**原样复用**；
  M4 扩展存储限定符（device 全局）与内建函数（原子/`__threadfence`）。
- **主机侧**：M4.1 的 `__device__` 变量若需主机侧访问，复用 M3.2 的符号拷贝机制
  （`haccMemcpyToSymbolAll` / `stub_gen` 的符号描述）。
- **属性契约**：M4.0 扩展 `cuda-attributes.md`，与 M3 的 `cool_shared` / `cool_constant` 并列。

## 8. 变更规则

- 本文是 M4 的**活文档**：每步落地后回写「已完成」并补落地细节（对齐 M3 方案的做法）。
- 属性契约变更须同步 `cuda-attributes.md`、`verify_attrs.sh`、探针与本文档。
- 跨项目（coolrt）改动须在本文档与 [TODO.md](TODO.md) 记录协调状态。
