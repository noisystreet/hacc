# M2 方案：主机路径（桩 + 内嵌映像 + `<<<>>>` 改写）

> 状态：**方案细化中（待评审）**（2026-10）。
> 上游：[compiler-implementation-plan.md](compiler-implementation-plan.md) §6.2（主机端）、§7（与 coolrt 的 ABI 衔接）、§10（里程碑 M2）。
> 前置：M1.2 已完成（`.cu` → EDG IL → LLVM IR → PTX，见 [m1.2-il-to-llvm-plan.md](m1.2-il-to-llvm-plan.md)）。
> 相关：[ADR 0003](adr/0003-host-stub-fatbin-registration.md)（主机桩 + 内嵌映像注册）、
> [ADR 0004](adr/0004-cuda-cpp-subset-scope.md)（子集范围）。
> 环境事实：本机有 GPU（RTX 4060 Laptop），PTX 经驱动 JIT 可载入 → e2e 可真验证。

## 1. 目标与范围

**目标**：让「用 CUDA 写法写的源码」（`__global__` + `<<<>>>`）经 hacc 编译后，**在 coolrt 上跑通**——
即补齐「主机路径」：`<<<>>>` 改写、主机桩、内嵌设备映像、惰性注册、主机编译与链接。

**M2 范围（最小切片）**：

- 内核形态：**无参 / 标量参**的 `__global__` 函数（与 M1.2 设备侧一致）。
- 必须支持：`kernel<<<grid, block>>>(args...)`（含 `smem` / `stream` 可选参数）。
- 产物：**可执行文件**（默认，对齐 nvcc「一步到可执行」）。
- 验证：e2e——vectorAdd 在 coolrt 上跑通并校验结果。

**不在 M2 范围**：`__shared__`/`__constant__`（M3）、向量类型（M3）、原子/`__threadfence`（M4）、
cubin/`ptxas`（M6）、分离编译/RDC、动态并行（Non-Goal）。

## 2. 调研结论（现状与约束）

### 2.1 coolrt 现有 ABI（零改动衔接）

| 用途 | coolrt 符号 | 语义 |
| --- | --- | --- |
| 载入映像 | `coolModuleLoadData(coolModule_t*, const void* image)` | 接受 **PTX 文本**，驱动 JIT |
| 取内核句柄 | `coolModuleGetFunction(coolKernel_t*, coolModule_t, const char* name)` | 按符号名 |
| 启动 | `coolLaunchKernel(coolKernel_t, dim3, dim3, void** args, size_t smem, coolStream_t)` | **`void** args` 直传**，不做类型推导 |
| 维度/句柄 | `dim3`、`coolKernel_t`、`coolModule_t`、`coolStream_t`（`NULL` = 默认流） | — |

> **关键**：`coolLaunchKernel` 的 `args` 是 `void**`（每个元素指向一个实参），与 `cuLaunchKernel` 的
> `kernelParams` 语义一致。故主机侧需把每个实参**物化为具名临时**并取地址。

### 2.2 `<<<>>>` 是**非 C++ 语法**

- EDG 原生不认识 `<<<>>>`（方案 §5.3）。**必须在预处理/词法层改写**为普通函数调用形式，再交 EDG 解析。
- 方案 §5.3 已定方向：改写为 `haccLaunch(kernel, grid, block, smem, stream, args...)` 形式。

### 2.3 `coolfe++` 现状

- 目前**只产 EDG IL**（`-o` 二进制 + `--emit-il-text` 文本），**不产 host 源**。
- 调 `cpfe` 的方式见 `src/frontend/edg_invoke.cpp`（分两趟：二进制趟 + 文本趟）。

### 2.4 主机侧不需要 EDG 代码生成（关键简化）

- 方案 §3.1 曾列「方案 C：用 EDG 的 C 生成后端产 C，再喂 clang」，但**已否决**（引入 clang 依赖、C 往返丢信息）。
- **本方案采用更简路径**：host 源 = **改写后的源码**（`<<<>>>` 已改写为 `haccLaunch(...)`）——
  - `__global__` / `__device__` / `threadIdx` 等由**运行时头**（`include/coolcc/hacc_runtime.h` + COOLRuntime 头）提供宏/声明；
  - **但 `__global__` 函数体必须剔除**（只留声明）——函数体属设备代码，host 侧编译会因 `threadIdx` 等未定义而失败（**见 §8.2 实测**）；
  - 主机侧就是**普通 C++**，直接交系统 `g++` 编译。
- **好处**：host 侧不依赖 EDG 的代码生成能力，`coolfe++` 只需「源码改写 + 调 cpfe 产 IL」两件事。

## 3. 方案设计

### 3.1 总体流水线（M2 扩展后）

```
input.cu
   │
   ├─(1) 源码改写（coolfe++ 内，纯文本）──▶ input.hacc.cpp   # <<<>>> → haccLaunch(...)
   │                                          │
   ├─(2) cpfe（对改写后源码）──▶ input.cil    # EDG IL（设备侧）
   │                                          │
   │                                          ▼
   │                                    coolicc --from-il ──▶ input.ptx
   │                                          │
   └─(3) 主机编译（g++）◀──────────────────────┘
          input.hacc.cpp + input.ptx（内嵌为字节数组）+ coolrt
                    │
                    ▼
              可执行文件
```

### 3.2 `<<<>>>` 改写（M2.1，核心）

**改写规则**（词法层，纯文本变换）：

```cpp
// 输入
kernel<<<grid, block>>>(a, b);
kernel<<<grid, block, smem>>>(a, b);
kernel<<<grid, block, smem, stream>>>(a, b);

// 输出
haccLaunch(kernel, grid, block, 0, nullptr, a, b);
haccLaunch(kernel, grid, block, smem, nullptr, a, b);
haccLaunch(kernel, grid, block, smem, stream, a, b);
```

**实现要点**：

1. **词法扫描**：识别 `<<<` ... `>>>` 配对（注意与 `<<` / `>>` 移位运算符区分——`<<<` 是三个 `<`）。
2. **参数切分**：`<<<...>>>` 内按顶层逗号切分（2/3/4 个参数）。
3. **实参透传**：`(...)` 内的实参原样透传（`haccLaunch` 是可变参数模板/宏，见 §3.4）。
4. **可测性**：改写是**纯文本 → 纯文本**，可独立单测（输入片段 → 期望输出片段），无需 EDG/LLVM。

> **风险**：`<<<` 与模板 `a << <b` 的歧义。子集内要求 `<<<` 仅用于启动语法（与 CUDA 一致）；
> 若出现歧义，报错（fail loud），不静默误改。

### 3.3 主机桩 + 内嵌映像（M2.2）

对每个 `__global__` 函数 `foo`，生成（示意，最终以 `hacc_runtime.h` 收敛）：

```cpp
// 1) 内嵌设备映像（PTX 字节）
static const unsigned char __cool_image_foo[] = { /* PTX 文本字节 */ };

// 2) 句柄槽位 + 注册描述（惰性解析为 coolKernel_t）
static coolKernel_t __cool_kernel_foo = nullptr;
static const hacc::KernelRegistration __cool_reg_foo = {
    "foo", __cool_image_foo, sizeof(__cool_image_foo), &__cool_kernel_foo };
```

**映像内嵌方式**：PTX 文本 → C 字节数组。由 `coolfe++`（或 driver）生成一个 `.cpp` 片段，
或直接用 `xxd -i` 风格。**一期用文本字节数组**（可读、可调试）。

### 3.4 `hacc_runtime.h`（公共逻辑收敛）

```cpp
namespace hacc {
struct KernelRegistration {
    const char* name; const void* image; std::size_t imageSize; coolKernel_t* slot;
};
// 首次使用时载入模块并取函数；失败返回 nullptr（不抛异常，符合 C ABI 语义）
coolKernel_t resolve(const KernelRegistration& reg);
}

// 启动跳板：把实参物化为具名临时，取地址成 void*[]，调 coolLaunchKernel。
// grid/block 用模板（接受 int / dim3，对齐 CUDA <<<g,b>>> 的隐式转换，见 §8.4）。
template <typename G, typename B, typename... Args>
void haccLaunch(const hacc::KernelRegistration& reg, G grid, B block, size_t smem,
                coolStream_t stream, Args... args);
```

**`haccLaunch` 语义**（对齐 cudart）：

- 每个实参在**完整表达式**内物化为具名临时（模板参数 `args...` 按值传入，本身即具名临时），取地址成 `void* argv[]`。
- `stream` 为 `nullptr` 即默认流。
- **丢弃** `coolLaunchKernel` 返回码（last error 由 coolrt A 层现有机制覆盖，见 §2.1 与 ADR 0003）。
- 第一个参数是**注册描述**（`__cool_reg_foo`），`haccLaunch` 内部 `resolve` 为 `coolKernel_t`。
- **完整原型见 §8.5**（已实测编译链接通过）。

> **改写与桩的衔接**：`<<<>>>` 改写为 `haccLaunch(__cool_reg_foo, grid, block, smem, stream, args...)`，
> 其中 `__cool_reg_foo` 由桩生成（§3.3）。故**改写需知道内核名 → 注册描述名**的映射（`foo` → `__cool_reg_foo`）。

### 3.5 driver 编排（M2.3）

`hacc` driver 扩展流水线：

```
hacc input.cu -o a.out
  ├─ coolfe++ input.cu --emit-host=input.hacc.cpp -o input.cil   # 改写 + IL
  ├─ coolicc --from-il -o input.ptx input.cil                    # IL → PTX
  ├─ 生成 input.hacc.cpp 的桩段（内嵌 PTX 字节 + 注册描述，并入同一 TU）  # 桩
  └─ g++ input.hacc.cpp -I<coolrt> -lcoolrt -o a.out
```

> **桩并入 host 源**（同一 TU）——注册描述 `static` 可见性要求（**见 §8.3 实测**）。

**新增选项**：`--emit=exe`（默认）/ `--emit=ptx`（纯设备，M1.2 已有）/ `-keep`（保留中间产物）。

## 4. 分步实施（建议）

| 步骤 | 内容 | 验证 | 难度 |
| --- | --- | --- | --- |
| **M2.0** | 源码改写器（纯文本，独立模块 `src/host/launch_rewrite.{hpp,cpp}`）：① `<<<>>>` → `haccLaunch(...)`；② 剔除 `__global__` 函数体（只留声明） | 单测：片段 → 期望输出 | 中 |
| **M2.1** | `coolfe++` 产 host 源（改写后源码）+ 仍产 IL | golden：host 源文本比对 | 中 |
| **M2.2** | `hacc_runtime.h`（`KernelRegistration` + `resolve` + `haccLaunch`）+ 桩生成 | 单测 + 编译通过 | 中 |
| **M2.3** | driver 编排主机编译（调 `g++` + 链接 coolrt） | 产出可执行文件 | 中 |
| **M2.4** | e2e：vectorAdd 在 coolrt 上跑通 | **e2e（需 GPU，本机有）** | 高 |

> 每步独立可验证；M2.0/M2.1 是纯文本变换（最易测），M2.4 是端到端（最难，但本机有 GPU 可真跑）。

## 5. 风险与缓解

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| `<<<` 与模板/移位歧义 | 中 | 子集内限定 `<<<` 仅用于启动；歧义报错（fail loud） |
| 实参物化的求值顺序/生命周期 | 中 | 具名临时 + 完整表达式；单测覆盖 |
| 内核名 → 注册描述名映射 | 低 | 确定性命名（`foo` → `__cool_reg_foo`） |
| `-arch` 与设备不匹配 | 中 | 由 `coolGetDeviceProperties` 决定默认 arch（后续） |
| 主机编译器/链接器差异 | 低 | 用系统 `g++`；CI 固定环境 |
| PTX JIT 不可用 | 低 | 本机有 GPU；无 GPU 环境跳过 e2e |

## 6. 待确认（实施前）

> **§8 的 M2.0 spike 已解决第 2/4 项**（host 源不需 EDG；`haccLaunch` 用模板），并新增 2 项修正（剔除函数体、桩并入同 TU）。

1. **`<<<>>>` 改写的实现层**：`coolfe++` 内做（调 cpfe 前），还是独立工具？——**倾向 `coolfe++` 内**（与 IL 产出同源）。
2. ~~**host 源是否需 EDG 参与**~~：**已定——不需要**（§8.1 实测：改写后源码 + 剔除函数体，`g++` 直接编译链接通过）。
3. **映像内嵌格式**：PTX 文本字节数组（一期）vs 二进制。**倾向文本**（可读）。
4. ~~**`haccLaunch` 形态**~~：**已定——可变参数模板**（§8.5 原型已实测通过）。
5. **符号命名**：`extern "C"` 名字直接采用；否则固定 mangling（与设备侧 PTX 入口名一致）。

## 7. 与 M1.2 的衔接

- **设备侧不变**：M1.2 的 `.cu` → IL → PTX 链路**原样复用**。
- **新增主机侧**：`<<<>>>` 改写 + 桩 + 内嵌映像 + 主机编译。
- **driver 扩展**：M1.2.5 的 `pipeline.{hpp,cpp}` 增加主机编译阶段。

## 8. M2.0 spike 实测结论（2026-10，已核验）

用 Python 原型 + 真实工具链（`g++` + coolrt + RTX 4060）验证了 M2 的**核心不确定性**，
**结论：方案可行，端到端跑通**。实测发现 3 处需修正方案细节：

### 8.1 端到端验证（成功）

```
tid_kernel.cu → hacc → 真实 PTX → 内嵌字节数组 → g++ 编译链接 → 在 RTX 4060 上运行
                                                                  → result = 0 ✓
```

- 真实 PTX（M1.2 产出）内嵌为 C 字节数组；
- `coolMalloc` / `coolLaunchKernel` / `coolDeviceSynchronize` / `coolMemcpy` 全链路跑通；
- 内核启动成功，结果校验通过（`tid.x=0 + ctaid.x=0 = 0`）。

### 8.2 修正 1：host 侧**必须剔除 `__global__` 函数体**（关键）

- **原方案 §2.4 说「host 源 = 改写后源码本身」不准确**。
- **实测**：host 侧（无 `__COOLCC__`）编译 `__global__` 函数体时，`threadIdx` / `__syncthreads`
  未声明 → **编译失败**；即使补声明，函数体仍产生**未定义引用**（链接失败）。
- **修正**：host 源里 `__global__` 函数**只留声明**（剔除函数体）：
  ```cpp
  // 输入：__global__ void tid_kernel(int* out) { ... }
  // host 源：void tid_kernel(int* out);   // 只留声明
  ```
  函数体属设备代码，由 `coolicc` 从 IL 生成 PTX，**不在 host 侧编译**。
- **实现**：改写器需识别 `__global__` 函数边界（`{ ... }` 配对），剔除函数体。

### 8.3 修正 2：桩**必须与 host 源同 TU**

- **原方案 §3.5 说「生成 `input.image.cpp`」不准确**。
- **实测**：注册描述 `__cool_reg_foo` 若在独立 TU 且 `static`，host 源**看不到**（编译失败）。
- **修正**：桩（内嵌映像 + 注册描述）**并入 host 源**（同一 TU），或注册描述用非 `static` + `extern` 声明。
  **倾向并入**（简单、无跨 TU 符号管理）。

### 8.4 修正 3：`haccLaunch` 的 grid/block 需**模板化**

- **原方案 §3.4 写的是 `dim3` 参数**。
- **实测**：`<<<g, b>>>` 的 `g`/`b` 常为 `int`（如 `(n+255)/256`），`dim3` 是 C struct（无构造函数），
  **不能隐式转换** → 编译失败。
- **修正**：`haccLaunch` 的 grid/block 参数用**模板**，内部经 `to_dim3` 统一转 `dim3`：
  ```cpp
  inline dim3 to_dim3(unsigned int v) { return dim3{v, 1, 1}; }
  inline dim3 to_dim3(dim3 d) { return d; }
  template <typename G, typename B, typename... Args>
  void haccLaunch(const hacc::KernelRegistration& reg, G grid, B block,
                  std::size_t smem, coolStream_t stream, Args... args);
  ```

### 8.5 已验证的 `haccLaunch` 原型（可直接落地）

```cpp
template <typename G, typename B, typename... Args>
void haccLaunch(const hacc::KernelRegistration& reg, G grid, B block,
                std::size_t smem, coolStream_t stream, Args... args) {
    coolKernel_t k = hacc::resolve(reg);
    if (k == nullptr) return;                       // 失败静默（last error 由 coolrt 覆盖）
    void* argv[] = { (void*)&args... };             // 实参物化为具名临时（模板参数即临时）
    (void)coolLaunchKernel(k, hacc::to_dim3(grid), hacc::to_dim3(block), argv, smem, stream);
}
```

> **注**：`(void*)&args...` 中 `args` 是**按值传入的模板参数**，本身即具名临时，生命周期覆盖整个调用——
> 满足「实参按值物化」语义（§3.4）。无需额外 `auto __a0 = ...`。

### 8.6 对分步实施的影响

- **M2.0（改写器）**：需同时做两件事——① `<<<>>>` → `haccLaunch(...)`；② 剔除 `__global__` 函数体。
  两者都是**纯文本变换**，可独立单测。
- **M2.2（桩/运行时头）**：桩并入 host 源；`haccLaunch` 用 §8.5 原型。
- **M2.4（e2e）**：本机 GPU 已验证可跑通，**非跳过**。
