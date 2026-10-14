# M3 方案：子集 S0 完成（`__shared__` / `__constant__` / 向量类型 / `dim3`）

> 状态：**进行中（M3.0 已完成）**（2026-10）。M3.0 向量类型（聚合参数 + 成员访问）已实现并真跑通过。
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

### 3.2 M3.0 向量类型（承接 B3-b）——**已完成**

**Spike 结论（聚合参数 ABI）**：用 LLVM C++ API 建 `void k(float4 v, float* out)` 内核发射 PTX，
确认 NVPTX 后端对 struct 参数按 **byval** 发射：

```
.visible .entry k(
	.param .align 4 .b8 k_param_0[16],   ← struct float4 按 byval（16 字节）
	.param .u64 k_param_1
)
```

**含义**：`il_to_llvm` 把向量类型映射为 LLVM `<N x T>`（**选型见 §8**：PTX 用 `ld.param.v4.f32`
向量化加载，对齐 16），NVPTX 后端自动按 byval 发射——**无需手动拆解**。主机侧 `haccLaunch` 的
`&args` 传 struct 地址，与 byval 语义一致。

**三处协同（已落地）**：

1. **coolrt**（`device_builtins.h`）：补全 `float2/3/4`、`int2/4`、`uint2/4`、`double2/4`，
   **带正确对齐**（`float2` align 8、`float4` align 16、`float3` 无 padding align 4，与 CUDA 一致）。
   提交 `f3cfbdd`。
2. **`il_to_llvm`**：
   - `map_type` 新增 `tk_struct`/`tk_class`/`tk_union` 分支（`map_struct_type`：遍历 `field_list`
     逐成员递归，仅支持无位域 POD 聚合）；**向量类型（成员全同标量、2/3/4 个）→ LLVM `<N x T>`**
     （对齐 §8 选型），其余 POD 聚合 → `StructType`；
   - 新增**普通结构体成员访问**（`try_translate_struct_field`：向量类型 load 后 `extractelement`，
     struct 走 `CreateStructGEP` + load，按名字在 `field_list` 定位成员索引）。
3. **测试**：golden `golden_frontend_vector_type` + `golden_device_il_to_ptx_vector_type[_vec]`
   （校验 `ld.param.v4.f32` + `.param .align 16 .b8 add4_param_0[16]`）；e2e
   `e2e_hacc_vector_type_run[_build]`（`float4` 内核参数 + 成员访问，校验 `out == 110`）。
   **本机 RTX 4060 真跑通过**（`vectorType OK: 110.000000`）。

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
| **M3.0** | 向量类型 + 聚合参数 ABI | **coolrt 新增类型** | golden + e2e | 高 | ✅ **已完成** |
| **M3.2** | `__constant__`（+ 主机侧拷贝） | **coolrt 新增 API** | golden + e2e | 中 |
| **M3.5** | samples + 边界用例 | 上述 | samples 全绿 | 低 |

> **推荐顺序**：先做**无 coolrt 依赖**的 M3.1 / M3.3 / M3.4（可立即交付），
> 再与 coolrt 对齐后做 M3.0 / M3.2（跨项目）。

## 5. 风险与缓解

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| ~~聚合参数 PTX ABI 不确定~~ | ~~高（M3.0 核心）~~ | **已消除（§8 spike）**：byval / `<N x T>` 均跑通，选 `<N x T>` |
| coolrt 无向量类型 / `__constant__` API | 中（M3.0/M3.2 阻塞） | 跨项目对齐；M3.2 可降级为「仅设备侧」 |
| `__shared__` 动态索引的地址空间传播 | 中 | GEP + addrspace 正确性单测 |
| `dim3` 与 `uint3` 关系不清 | 低 | 明确：设备侧 `dim3` 用于参数，内建变量用 `uint3` |
| 诊断源位置获取 | 低 | 从 IL `source_corresp` 取行列 |

## 6. 待确认（实施前）

1. **coolrt 是否新增向量类型**？（M3.0 前置）——需与 coolrt 维护者对齐。
2. **coolrt 是否新增 `coolMemcpyToSymbol`**？（M3.2 前置）——若无，M3.2 降级。
3. ~~**聚合参数 PTX ABI**：byval vs 拆解？——需 spike。~~ **已解决（§8 spike）**：byval struct 与
   `<4 x float>` 均端到端跑通；**选 `<N x T>` 向量类型**（`ld.param.vN.f32` 向量化，对齐明确）。
4. **`dim3` 设备侧**：是否需与 `uint3` 合并？——需定。
5. **诊断格式**：具体输出格式（C6）——需定。

## 7. 与 M2 / B3 的衔接

- **设备侧**：M2 的标量链路**原样复用**；M3 扩展类型系统（向量/聚合）与存储限定符（shared/constant）。
- **主机侧**：M3.0 的聚合传参**复用** `haccLaunch`（B3-b 已确认主机侧可编译）；M3.2 需主机侧符号拷贝 API。
- **B3-b**：M3.0 落地时**同步闭环** B3-b（聚合/向量传参）。

## 8. M3.0 spike 实测结论（2026-10，已核验）

用 `coolicc --emit=ptx` + 手写 IR + 真实工具链（`g++` + coolrt + RTX 4060）验证了 M3.0 的
**核心不确定性——聚合参数 PTX ABI**。**结论：byval struct 与 `<4 x float>` 两种方案均端到端跑通**。

### 8.1 三种参数形态的 PTX 降级（`coolicc --emit=ptx -arch=sm_80`）

| 变体 | IR 参数 | PTX 参数形态 | 加载指令 |
| --- | --- | --- | --- |
| **byval struct** | `ptr byval(%struct.float4) align 4` | `.param .align 4 .b8 k_param_0[16]` | 逐字段 `ld.param.f32` |
| **拆解标量** | `float, float, float, float` | `.param .f32 ×4` | 4 个独立 `ld.param.f32` |
| **`<4 x float>`** | `<4 x float>` | `.param .align 16 .b8 k_param_0[16]` | **`ld.param.v4.f32`（向量化）** |

**关键观察**：
- **byval struct 与 `<4 x float>` 的 PTX 参数形态一致**（都是 `.b8[N]` 内联），差异仅在**对齐**
  （byval 用 `align 4`，向量用 `align 16`）。
- **`<4 x float>` 更优**：`ld.param.v4.f32` 一次加载 4 个 float（向量化），byval struct 是逐字段 GEP + load。
- **拆解标量**：参数个数膨胀（4 个 float → 4 个 `.param`），主机侧需拆解，**不推荐**。

### 8.2 端到端验证（成功）

| 方案 | 内核 | 主机侧传参 | 结果 |
| --- | --- | --- | --- |
| **byval struct** | `k_f4(ptr byval(%struct.float4), ptr)` | `void* argv[] = {&v, &d_out}` | `h = 3.5` ✅ |
| **`<4 x float>`** | `k_vec(<4 x float>, ptr)` | `void* argv[] = {&v, &d_out}` | `h = 7.25` ✅ |

- **主机侧 `void* argv[] = {&struct}`（传 `&struct`）** 与 **PTX `.param .b8[N]`（内联）** **完全匹配**——
  驱动按 `sizeof(struct)` 拷贝 N 字节，与 PTX 参数内联语义一致。
- **主机侧无需拆解**：直接传 `&struct` 即可（`haccLaunch` 的 `(void*)&args...` 天然支持）。

### 8.3 结论与选型

1. **聚合参数 ABI 可行**：主机侧 `&struct` ↔ PTX `.param .b8[N]` 匹配，**无需拆解**。
2. **推荐 `<N x T>` 向量类型**（而非 byval struct）：
   - PTX 用 `ld.param.vN.f32` 向量化加载，**更高效**；
   - 对齐明确（`align 16` for float4），与 CUDA 的 `float4` 对齐一致；
   - 与 LLVM 原生向量类型对应，`il_to_llvm` 映射直接（`tk_struct` → `<N x T>`）。
3. **对齐要求**：主机侧 struct 的对齐**必须与 PTX 一致**（`float4` → `align 16`）。
   coolrt 的向量类型定义需带 `__align__(16)`（或 `alignas(16)`）。
4. **`il_to_llvm` 映射**：`tk_struct`（向量类型）→ LLVM `<N x T>`；参数按值传（`<N x T>` 直接作参数类型）。

### 8.4 对 M3.0 的影响

- **M3.0 的 ABI 不确定性已消除**：byval / 向量两方案均验证可行，**选 `<N x T>`**。
- **coolrt 侧**：新增向量类型时**须带正确对齐**（`float4` → 16 字节对齐）。
- **主机侧**：`haccLaunch` **无需改动**（`&args` 已支持聚合）。
- **设备侧**：`il_to_llvm` 新增 `tk_struct` → `<N x T>` 映射 + 参数按值传。
