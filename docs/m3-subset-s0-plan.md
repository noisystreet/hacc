# M3 方案：子集 S0 完成（`__shared__` / `__constant__` / 向量类型 / `dim3`）

> 状态：**方案细化中（待评审）**（2026-10）。
> 上游：[compiler-implementation-plan.md](compiler-implementation-plan.md) §9（子集范围）、§10（里程碑 M3）。
> 前置：M2 已完成（主机路径端到端跑通，见 [m2-host-path-plan.md](m2-host-path-plan.md)）。
> 相关：[ADR 0004](adr/0004-cuda-cpp-subset-scope.md)（子集范围）、
> [ADR 0003](adr/0003-host-stub-fatbin-registration.md)（参数物化，B3-b 与 M3.0 同步）、
> [cuda-attributes.md](../tools/il_spike/cuda-attributes.md)（属性契约）。
> 环境事实：本机有 GPU（RTX 4060 Laptop），PTX 经驱动 JIT 可载入 → e2e 可真验证。

## 1. 目标与范围

**目标**：补齐 **S0 子集**（[ADR 0004](adr/0004-cuda-cpp-subset-scope.md) §S0）中 M1/M2 未覆盖的部分，
使「S0 级 CUDA 源码」可完整编译运行；并提升诊断质量。

**M3 范围**：

| 项 | 内容 | 依赖 |
| --- | --- | --- |
| **M3.0 向量类型** | `float2/3/4`、`int2/4`、`double2` 等内建向量类型；聚合参数传参（承接 B3-b） | **coolrt 需新增类型定义** |
| **M3.1 `__shared__`** | 静态共享内存变量 → LLVM `addrspace(3)`；内核内 `__shared__` 数组 | 纯设备侧（无 coolrt 改动） |
| **M3.2 `__constant__`** | 常量内存变量 → LLVM `addrspace(4)`；主机侧符号拷贝 | **coolrt 需新增 `coolMemcpyToSymbol` 类 API** |
| **M3.3 `dim3` 设备侧** | `dim3` 类型（struct of 3×uint）在设备侧的类型映射 | 纯设备侧 |
| **M3.4 诊断质量** | 统一诊断格式（阶段 / 源位置 / 成因，对应 TODO C6） | 纯前端/后端 |
| **M3.5 samples + 边界用例** | `samples/` 目录；边界用例（越界、类型不匹配等） | 依赖上述 |

**不在 M3 范围**：原子 / `__threadfence`（M4）、`__restrict__`（M4）、设备全局 `__device__` 变量（M4）、
模板 / `__launch_bounds__` / 数学函数（S2）、cubin/`ptxas`（M6）、分离编译/RDC、动态并行（Non-Goal）。

## 2. 调研结论（现状与约束）

### 2.1 设备后端现状（`src/device/il_to_llvm.cpp`）

- **类型映射**（`map_type`）：仅 `tk_void` / `tk_integer`（i8..i64）/ `tk_float`（float/double）/
  `tk_pointer`（不透明 ptr）/ `tk_typeref`（解引用）。**未覆盖** `tk_struct`（向量类型）、
  `tk_array`、`tk_enum` 等 → 遇未覆盖类型 **fail loud**。
- **属性识别**：`has_attribute(attrs, name)` 已存在，当前只用于 `cool_global`（识别内核）。
  **未处理变量属性**（`cool_shared` / `cool_constant` / `cool_device`）。
- **全局变量**：当前**不发射**任何全局变量（内核内 `__shared__` 声明也未处理）。
- **参数**：仅标量（`tk_integer` / `tk_float` / `tk_pointer`）；聚合参数 fail loud。

### 2.2 属性契约（`tools/il_spike/cuda-attributes.md`，已定）

| 源码限定符 | 属性名 | LLVM 地址空间 |
| --- | --- | --- |
| `__shared__` | `cool_shared` | **3** |
| `__constant__` | `cool_constant` | **4** |
| `__device__`（变量） | `cool_device` | **1**（M4） |
| 显式 | `cool_address_space(n)` | `n ∈ {0,1,3,4,5}` |

> `cool_shared` / `cool_constant` 的**带参未知属性传递**已在 M0 spike 验证（`verify_attrs.sh`）。

### 2.3 coolrt 现状（`coolruntime/include/`）

| 项 | 现状 | M3 影响 |
| --- | --- | --- |
| `dim3` | ✅ 有（`cool_types.h`） | M3.3 可直接用 |
| 向量类型（`float2/4` 等） | ❌ **无**（仅 `uint3`/`int3`，供内建变量） | **M3.0 需 coolrt 新增** |
| `coolMemcpyToSymbol` / `coolGetSymbolAddress` | ❌ **无** | **M3.2 需 coolrt 新增**（或 M3.2 降级为「仅设备侧声明」） |
| `coolLaunchKernel(..., sharedMem, ...)` | ✅ 有（动态共享内存） | M3.1 静态 `__shared__` 与之互补 |

> **关键约束**：M3.0 / M3.2 涉及 **coolrt 侧改动**（新增类型 / API），属**跨项目**。
> 需与 coolrt 维护者对齐（类似 TODO B4 的跨项目协调）。

## 3. 方案设计

### 3.1 M3.1 `__shared__`（建议先做，纯设备侧）

**设备侧**（`il_to_llvm`）：

1. **内核内 `__shared__` 变量声明**（`stmk_decl` + 变量带 `cool_shared` 属性）：
   - 发射 LLVM 全局变量：`@__shared_<name> = internal addrspace(3) global [N x T] zeroinitializer`；
   - 内核内引用该变量 → `addrspacecast` 或直接 `addrspace(3)` 指针。
2. **文件作用域 `__shared__`**：CUDA 不允许（`__shared__` 仅函数内），报错。
3. **地址空间**：`addrspace(3)`（契约已定）。

**验证**：golden PTX 校验 `.shared` 段 + `st.shared` / `ld.shared` 指令；e2e 用共享内存做归约。

**风险**：`__shared__` 数组的**动态索引**（`sdata[tid]`）需 GEP + 地址空间正确传播。

### 3.2 M3.0 向量类型（承接 B3-b）

**三处协同**（与 B3-b 一致）：

1. **coolrt**：新增 `float2/3/4`、`int2/4`、`double2` 等（`device_builtins.h` 或新头）。
   - 对齐 CUDA：`struct float4 { float x, y, z, w; }`（POD，无构造）。
2. **`hacc_runtime.h`**：引入向量类型定义（主机侧生成代码需可见）。
3. **`il_to_llvm`**：
   - `tk_struct`（向量类型）→ LLVM `<N x T>` 或 struct（**需定**：CUDA 用 struct，PTX 参数 ABI 用 byval）；
   - **聚合参数 PTX ABI**：`byval` 或拆解为标量（**需 spike 验证**）。

**验证**：golden PTX（向量类型参数）；e2e（向量加法）。

**风险**：聚合参数的 PTX ABI 是**最大不确定性**——需 spike 确认 NVVM 对 struct 参数的降级方式。

### 3.3 M3.2 `__constant__`

**设备侧**：`__constant__` 变量 → `addrspace(4)` 全局变量。

**主机侧**：需 `coolMemcpyToSymbol` 类 API 把主机数据拷入常量内存。
- **若 coolrt 无此 API**：M3.2 **降级**为「仅设备侧声明 + 编译期初始化」（`__constant__ int x = 5;`），
  运行时拷贝延后到 coolrt 提供 API 后。

**验证**：golden PTX（`.const` 段）；e2e（若 coolrt 支持拷贝）。

### 3.4 M3.3 `dim3` 设备侧

- `dim3` 是 `cool_types.h` 的 struct（3×uint）；设备侧类型映射为 LLVM struct `{i32, i32, i32}`。
- 内建变量（`threadIdx` 等）当前用 `uint3`（`device_builtins.h`）——**需统一** `dim3` 与 `uint3` 的关系
  （CUDA 中 `threadIdx` 是 `uint3`，`dim3` 是主机侧类型；设备侧 `dim3` 用于内核参数）。

### 3.5 M3.4 诊断质量（对应 TODO C6）

- 统一诊断格式：`<阶段>: <源位置>: <成因>`（如 `coolicc: foo.cu:12: 不支持的运算符 eok_xxx`）。
- 当前 `fail()` 只给字符串，**无源位置**——需从 IL 的 `source_corresp` 取行列。

### 3.6 M3.5 samples + 边界用例

- `samples/`：`vectorAdd`（已有）、`shared_reduce`（共享内存归约）、`constant_pi`（常量内存）等。
- 边界用例：越界索引、类型不匹配、未覆盖构造的 fail loud 消息。

## 4. 分步实施（建议）

| 步骤 | 内容 | 依赖 | 验证 | 难度 |
| --- | --- | --- | --- | --- |
| **M3.1** | `__shared__`（静态共享内存 → addrspace(3)） | 无（纯设备侧） | golden PTX + e2e 归约 | 中 |
| **M3.3** | `dim3` 设备侧类型映射 | 无 | golden PTX | 低 |
| **M3.4** | 诊断格式（源位置） | 无 | 单测（错误消息） | 低 |
| **M3.0** | 向量类型 + 聚合参数 ABI | **coolrt 新增类型** | golden + e2e | 高 |
| **M3.2** | `__constant__`（+ 主机侧拷贝） | **coolrt 新增 API** | golden + e2e | 中 |
| **M3.5** | samples + 边界用例 | 上述 | samples 全绿 | 低 |

> **推荐顺序**：先做**无 coolrt 依赖**的 M3.1 / M3.3 / M3.4（可立即交付），
> 再与 coolrt 对齐后做 M3.0 / M3.2（跨项目）。

## 5. 风险与缓解

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| 聚合参数 PTX ABI 不确定 | 高（M3.0 核心） | **先 spike**：用 NVVM 试 struct 参数，确认 byval/拆解 |
| coolrt 无向量类型 / `__constant__` API | 中（M3.0/M3.2 阻塞） | 跨项目对齐；M3.2 可降级为「仅设备侧」 |
| `__shared__` 动态索引的地址空间传播 | 中 | GEP + addrspace 正确性单测 |
| `dim3` 与 `uint3` 关系不清 | 低 | 明确：设备侧 `dim3` 用于参数，内建变量用 `uint3` |
| 诊断源位置获取 | 低 | 从 IL `source_corresp` 取行列 |

## 6. 待确认（实施前）

1. **coolrt 是否新增向量类型**？（M3.0 前置）——需与 coolrt 维护者对齐。
2. **coolrt 是否新增 `coolMemcpyToSymbol`**？（M3.2 前置）——若无，M3.2 降级。
3. **聚合参数 PTX ABI**：byval vs 拆解？——需 spike。
4. **`dim3` 设备侧**：是否需与 `uint3` 合并？——需定。
5. **诊断格式**：具体输出格式（C6）——需定。

## 7. 与 M2 / B3 的衔接

- **设备侧**：M2 的标量链路**原样复用**；M3 扩展类型系统（向量/聚合）与存储限定符（shared/constant）。
- **主机侧**：M3.0 的聚合传参**复用** `haccLaunch`（B3-b 已确认主机侧可编译）；M3.2 需主机侧符号拷贝 API。
- **B3-b**：M3.0 落地时**同步闭环** B3-b（聚合/向量传参）。
