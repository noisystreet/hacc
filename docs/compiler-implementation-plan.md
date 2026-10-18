# hacc 编译器实现方案

> 状态：**规划稿（待评审）**。本文档给出 hacc 的总体实施方案；四项关键决策分别记录在
> [ADR 0001](adr/0001-edg-cpp-frontend.md) ～ [ADR 0004](adr/0004-cuda-cpp-subset-scope.md)。

## 1. 背景与定位

coolrt 的架构把「**不做 CUDA C++ 编译前端**」列为 Non-Goal：运行时只接收**已编译好的**内核映像
（PTX/cubin），由 `coolModuleLoadData` / `coolModuleGetFunction` 载入、`coolLaunchKernel` 启动
（见 coolruntime `docs/adr/0004-kernel-module-api-placement.md`）。

这带来一个与 cudart 的**语义断层**：

| | cudart 模型 | coolrt 模型 |
| --- | --- | --- |
| 编译期 | nvcc 前端把 `__global__` 编译为设备码，并生成**主机桩** | 无前端 |
| 启动 | `cudaLaunchKernel(func=桩地址, grid, block, args, smem, stream)`；`<<<>>>` 语法糖展开即此 | `coolLaunchKernel(kernel=模块函数句柄, ...)` |
| 映像注册 | 链接期 `fatbin` + 运行期隐式自注册（`__cudaRegisterFatBinary`） | 调用方**显式**持有 `coolKernel_t` |

要让「**用 CUDA 的写法写的源码**」以接近 cudart 的语义在 coolrt 上运行，必须补上这一层：
一个能把 `__global__` 函数编译成设备码、并生成**主机桩 + 注册代码**的前端。**hacc 就是这一层。**

hacc 的产出 = 一份可被普通 C++ 编译器编译、并链接 **`libcoolrt`**（静态 `libcoolrt_static.a` /
共享 `libcoolrt.so`，即 `-lcoolrt`）的**主机目标文件** +
内嵌的**设备映像**（PTX/cubin）。它是 coolrt 的**编译前端**，而非 cudart 的复刻。

## 2. 目标与非目标

### 目标

1. 接受 **CUDA C++ 子集**源码（`.cu`），支持 `__global__` / `__device__` / `__host__`、
   `<<<grid, block, smem, stream>>>` 启动语法、内置维度变量与 `dim3` 等（范围见 §9）。
2. 产出：
   - **设备端**：`nvptx64` 目标的 **PTX**（**一期到此为止**；cubin 属后续阶段，由**直接调用 `ptxas`** 产出）；
   - **主机端**：普通 C++ 源码/目标文件，含**主机桩**与**内核注册**，`<<<>>>` 改写为 `coolLaunchKernel` 调用。
3. 在**编程模型语义**上对齐 cudart：`<<<>>>` 展开、参数按值物化、`stream=NULL` 即默认流、
   启动错误经 last error 上报。
4. 与 coolrt **零运行时改动**地衔接（基于现有 `coolModuleLoadData` / `coolModuleGetFunction` /
   `coolLaunchKernel`，见 §7）。
5. **教学可读**：流水线各阶段（前端 IL、LLVM IR、PTX、主机桩）可分别导出观测（`--emit=` / `-keep`）。

### 非目标

- 不复刻 nvcc：不支持分离编译 / RDC、动态并行（`<<<>>>` in device）、`__managed__`、
  设备端异常/RTTI/协程、设备端 STL、CUDA 库（cuBLAS 等）。
- 不做 PTX 级优化器：优化尽量交给 LLVM（`ptxas` 级优化随后续 cubin 阶段引入）。
- 不实现 cudart 运行时的其余部分（那是 coolrt 的职责）；hacc 只负责**编译**与**注册桥接**。

### 分期：一期到 PTX，后续再接 ptxas

- **一期（首个交付）**：整条链路**到 PTX 为止**——`foo.cu` 经前端/设备后端产出 PTX，主机桩、惰性注册与
  `<<<>>>` 端到端照常交付；PTX 交由**驱动 PTX JIT** 在 `coolModuleLoadData` 时载入运行。**不集成 `ptxas`、不产出 cubin**。
- **后续阶段**：新增 `--emit=cubin`，通过**直接调用 `ptxas` 子进程**（PTX → cubin）实现，**设备后端无需改动**；
  仅在需绕过驱动 PTX JIT 或与 nvcc 产物对照时启用。
- 一期测试仍**允许调用 `ptxas` 仅作 golden 校验**（验证 PTX 可汇编性），但**不产出、不内嵌 cubin**。

## 3. 关键决策（对应 ADR）

| # | 决策点 | 结论 | ADR |
| --- | --- | --- | --- |
| D1 | 输入语言与兼容目标 | 解析**真实 CUDA C++ 子集**（`.cu`），语义对齐 cudart | [0004](adr/0004-cuda-cpp-subset-scope.md) |
| D2 | 前端实现策略 | 基于 **2026-09-30 开源的 EDG C++ 前端** 做词法/语法/语义 | [0001](adr/0001-edg-cpp-frontend.md) |
| D3 | 设备端代码生成 | **EDG AST/IL → LLVM IR → NVPTX 后端 → PTX**（不依赖 nvcc） | [0002](adr/0002-llvm-nvptx-device-backend.md) |
| D4 | 与 coolrt 对接 | 生成**主机桩 + 内嵌映像注册**，`<<<>>>` → `coolLaunchKernel` | [0003](adr/0003-host-stub-fatbin-registration.md) |

> 补充工程决策（已拍板，详见 §13）：**集成阶段顺序 = 先 IL（阶段 A）后回调（阶段 B）**（[ADR 0001](adr/0001-edg-cpp-frontend.md)）；
> **默认产物 = 一步到可执行**（对齐 nvcc，内部串联主机编译器与链接器，[ADR 0003](adr/0003-host-stub-fatbin-registration.md)）；
> **last error = 依赖 coolrt A 层现有机制（零改动）**（[ADR 0003](adr/0003-host-stub-fatbin-registration.md)）；
> **设备产物分期 = 一期只到 PTX，cubin 后续由直接调用 `ptxas` 产出**（见 §2「分期」，[ADR 0002](adr/0002-llvm-nvptx-device-backend.md)）。

## 4. 总体架构与编译流水线

```text
                       foo.cu  (CUDA C++ 子集)
                          │
              ┌───────────┴────────────┐
              │  hacc driver         │  选项解析 / 编排 / 子进程管理
              │  (src/driver, support) │  -arch=sm_XY  -O  --emit=  -o
              └───────────┬────────────┘
                          │
         ┌────────────────┴─────────────────┐
         │ coolfe++：EDG CUDA 前端           │  src/frontend
         │  词法/语法/语义 + CUDA 扩展识别     │  → 标记 __global__/__device__/<<<>>>
         │  → EDG AST / IL                    │
         └───────┬───────────────────┬───────┘
                 │                   │
         ┌───────┴────────┐   ┌──────┴───────────────────────┐
         │ 设备端路径      │   │ 主机端路径                    │
         │ src/device      │   │ src/host                      │
         │ AST/IL → LLVM IR│   │ 保留 host 代码；               │
         │ (nvptx64 target)│   │ 为每个 __global__ 生成主机桩； │
         │ → NVPTX 后端    │   │ <<<>>> 改写为 coolLaunchKernel │
         │ → .ptx          │   │ 生成内嵌映像 + 注册数据        │
         │ (→ .cubin 后续) │   │                               │
         └───────┬────────┘   └──────┬───────────────────────┘
                 │ .ptx (cubin 后续)   │ 生成的 .cpp
                 └────────┬───────────┘
                          │  主机编译器 (g++ 默认 / --host-cxx 可切 clang++，非 nvcc)
                          │  + include/coolcc/hacc_runtime.h
                          v
                    host.o  (含内嵌映像 + 注册)  ──链接──▶ libcoolrt
                          │
                          v
                     可执行文件
```

要点：

- **两条独立代码路径**（device / host），在主机编译器处汇合；设备映像以字节数组内嵌进主机目标文件。
- **`<<<>>>` 只存在于主机侧**：在主机 TU 中被改写成对 `coolLaunchKernel` 的调用。
- 主机侧编译**不使用 nvcc**，只用系统 C++ 编译器 + coolrt 的 C ABI 头 + compiler 附带的运行时头。
- **`coolfe++` / `coolicc` 为独立可执行文件**：driver 以子进程方式编排（对标 nvcc 经 `/bin/sh -c` 调度 `cudafe++`/`cicc`），降低耦合、便于独立验证。

### 4.1 nvcc ↔ hacc 子工具/组件对位

hacc 的工具/组件与 nvcc 工具链一一对位（子工具清单见《deep_dive_into_cuda》chapter_02_toolchain）：

| nvcc 子工具 | 角色 | hacc 对应 |
| --- | --- | --- |
| `nvcc` | 驱动/编排：选项解析、子进程调度、临时文件管理 | `hacc` driver（`src/driver`、`src/support`） |
| `cudafe++` | CUDA 前端：解析 CUDA 语法、分离 host/device、生成主机桩 | **`coolfe++`**（`src/frontend`，基于 EDG C++） |
| `cicc` | device 代码 → PTX | **`coolicc`**（`src/device`，EDG AST/IL → LLVM IR → NVPTX → PTX） |
| `ptxas` | PTX → SASS cubin | **不实现**，直接调用外部 `ptxas`（`--emit=cubin`，后续阶段） |
| `nvlink` | device 链接（RDC） | 一期不涉及（RDC 为 Non-Goal） |
| `fatbinary` | 打包 cubin + PTX 为容器 | 不用 fatbin 容器；改为**内嵌字节数组 + 惰性注册**（见 [ADR 0003](adr/0003-host-stub-fatbin-registration.md)） |
| `gcc` / `g++` | 主机编译与链接 | 系统 C++ 编译器子进程（**非** nvcc） |
| `crt/host_runtime.h` + `*.stub.c` | 主机桩与运行期注册 | hacc 随附头 `include/coolcc/hacc_runtime.h` |

> **分工说明**：CUDA 专有语法（execution space 关键字、`<<<>>>`、host/device 分离、主机桩生成）
> 由 **`cudafe++`（EDG）** 承担；`cicc` 是 **LLVM/NVVM 的 device 编译器**，把 device 代码降到
> LLVM IR 并发射 PTX。hacc 对应为 **`coolfe++`**（前端）与 **`coolicc`**（设备端，落在 `src/device`），
> 实现方案见 [ADR 0002](adr/0002-llvm-nvptx-device-backend.md)。

## 5. coolfe++：基于 EDG 的 CUDA 前端（D2）

`coolfe++` 是 hacc 的 **CUDA 前端组件**，对标 nvcc 工具链中的 `cudafe++`：基于 **EDG C++ 前端**
完成词法/语法/语义，并在其上识别 CUDA 扩展（execution space 关键字、`__shared__`/`__constant__`、
`<<<>>>`）、分离 host/device 代码、生成主机桩。命名遵循本生态「对标 NVIDIA 三层」的约定
（`nv`→`ha`、`cuda*`→`cool*`、`nvcc`→`hacc`），故 `cudafe++` → `coolfe++`。

> **形态已定**：`coolfe++` 做成**独立可执行文件**（对标 `cudafe++` 的子进程形态），由 `hacc` driver
> 以子进程方式编排调用——降低与 driver / `coolicc` 的耦合，便于独立开发与验证；与
> [ADR 0001](adr/0001-edg-cpp-frontend.md) 阶段 A（IL 文件、进程隔离）取向一致。
> **中间产物**：划分与命名**参考 nvcc**，一期只保留基本集（见 §6.3）；与 EDG 的 IL / 回调接口衔接点
> 固定为「EDG AST/IL → hacc 中间表示」，具体接口在 EDG 接入阶段确定。

### 5.1 为什么是 EDG

EDG C++ 前端是工业级 C/C++（含各标准版本）前端，NVIDIA / Microsoft 等历史上均使用过；
**2026-09-30 以 Apache-2.0 WITH LLVM-exception 在 `github.com/edgcpp/compiler` 开源**，仓库自带
IL 读写工具与示例后端（C / 源到源）。选它的理由：能真正解析真实 C++（模板、重载、命名空间），
避免从零手写 C++ 解析器；许可证宽松，可商用/嵌入。

### 5.2 集成形态（两种，分阶段）

EDG 的模型是「前端 + 后端」：前端做词法/语法/语义，**由后端选择输出形式**。hacc 采用：

- **阶段 A（bring-up，松耦合）**：以 **IL 文件**为接口。
  `coolfe++` 调用 `edgcpfe`（或链接其前端库）产出 **EDG IL**，再用 EDG 的 IL 读取工具消费 IL，
  交 `coolicc` 转换为 LLVM IR。优点是**进程隔离、IL 可保存比对、便于教学**与增量调试。
- **阶段 B（生产，紧耦合）**：以 **后端回调接口**为接口。`coolfe++` 链接 EDG 前端库并实现其后端接口，
  前端在处理每个声明/语句时回调，直接构建 LLVM IR。优点：单进程、诊断更精准、避免 IL 往返。

**顺序已拍板：先阶段 A（IL）后阶段 B（回调）**。**一期只做基本集 = 阶段 A（IL 文件接口）**：
以 IL 跑通端到端；**阶段 B（后端回调）延后**，仅作「IL 表达力不足」时后备，一期不实现。
M0 先做 **IL spike**，判据（基本版）：最小 `.cu`（含 `__global__` / `<<<>>>` / `threadIdx` /
`__syncthreads`）产出的 EDG IL 须保留四大关键属性——① 地址空间（global/shared/constant/local）
② `__shared__` / `__constant__` ③ 同步点（`__syncthreads`） ④ 内核 / 执行空间标记；
**任一缺失即判「IL 表达力不足」，触发切阶段 B**（不在本期承诺）。

**M0 IL spike 结论（已核验，2026-10）**：EDG IL 具备一等的属性表示（`attribute@…` 实体 +
实体 `source_corresp.attributes:` 引用），但**默认只保留 EDG 已知属性名**，未知属性被丢弃。
**已定：一期采用「构建期开启 `RECORD_UNRECOGNIZED_ATTRIBUTES`」路线（零 EDG 源码补丁）**——
开启后未知属性（含名字/参数）随 IL 传递并挂到实体上，CUDA 存储/执行限定符以
`__attribute__((…))` 承载即可在**阶段 A 内成立**，故**不触发切阶段 B**。
（注意：EDG 把 `.c` 源当 C++ 编译，须用 `CMAKE_CXX_FLAGS` 注入；该变量为全局量，阶段 B 可运行期置真。）
细节与「CUDA 属性清单」见 [`tools/il_spike/README.md`](../tools/il_spike/README.md)；
`<<<>>>` 启动语法属源码改写（coolfe++ 扩展层），非属性问题。

> 具体回调/IL 的**符号名与头文件（`fe_*.h` 等）须在 EDG 接入阶段对照 EDG 源码确认**；
> 本方案不预先假定其 API 名称，仅固定「AST/IL → 中间表示」这一衔接点。EDG 的获取方式见 [ADR 0005](adr/0005-licensing-and-edg-distribution.md)。

### 5.3 CUDA 扩展的识别

EDG 原生不认识 `__global__` / `<<<>>>`。hacc 在其之上做**扩展层**：

- 用 EDG 的 `__attribute__`/关键字兼容机制，或预处理期把 `__global__` 等映射为可识别的属性宏，
  使前端把它们当作函数标注而非语法错误（具体机制在 EDG 接入阶段确定）。
- 关键字的识别结果（execution space、`__shared__`、`__constant__`）存入 hacc 自有的**语义标注层**，
  供设备端代码生成消费。
- `<<<...>>>` 是**非 C++ 语法**：需在**预处理/词法层**改写（例如改写为 `haccLaunch(kernel, grid, block, smem, stream, args...)` 形式），
  再交由 EDG 正常解析；由主机端代码生成负责最终展开（§6）。

## 6. 设备端与主机端代码生成

### 6.1 设备端（coolicc）：EDG AST/IL → LLVM IR → NVPTX → PTX（D3）

`coolicc` 是 hacc 的**设备端代码生成组件**，对标 nvcc 工具链中的 `cicc`：把 `coolfe++` 产出的
device 侧 AST/IL 降到 **LLVM IR**，经 LLVM 优化后由 **NVPTX 后端**发射 **PTX**。
**形态已定：`coolicc` 同样做成独立可执行文件**，由 `hacc` driver 编排调用；中间产物形态仍待后续确定。

> **M1.2 细化方案**（EDG IL → LLVM IR 的选型、翻译核心、分步实施）见
> [m1.2-il-to-llvm-plan.md](m1.2-il-to-llvm-plan.md)。**M1.1（LLVM IR → PTX）已实现**（`coolicc` 接入 LLVM/NVPTX）。

- **目标三元组** `nvptx64-nvidia-cuda`，DataLayout 与 CUDA 地址空间映射：
  `global=1, shared=3, constant=4, local=5, param=0`（与 LLVM NVPTX 约定一致）。
- 内置变量经 **NVVM intrinsic** 落地：
  `threadIdx.x` → `llvm.nvvm.read.ptx.sreg.tid.x`；`blockIdx`/`blockDim`/`gridDim` 类似；
  `__syncthreads()` → `llvm.nvvm.barrier0`；`__threadfence()` → `llvm.nvvm.membar.*`。
- 内核入口 → `ptx_kernel` 调用约定 + `.visible .entry <name>`；内核参数按 CUDA 参数 ABI 落为
  `param` 地址空间参数。
- 内建向量类型（`float4`/`int2`/`double3`…）映射为 LLVM 向量/结构类型。
- `-arch=sm_XY` → NVPTX 的 `-mcpu=sm_XY` + PTX 版本；**一期产物止于 PTX**（`--emit=ptx`，默认）；`--emit=cubin` 属后续阶段，由**直接调用 `ptxas`** 完成（§2「分期」）。
- **不追求设备端优化**：`-O` 一期透传给 LLVM（`opt` 级）；`ptxas` 级优化随后续 cubin 阶段引入。

### 6.2 主机端：桩 + 内嵌映像 + 注册（D4）

对每个 `__global__` 函数生成（示意，最终以编译器随附头 `include/coolcc/hacc_runtime.h` 收敛）：

```cpp
#include <cool/cool_runtime_api.h>
#include <coolcc/hacc_runtime.h>

// 1) 内嵌设备映像（一期为 PTX 字节）
static const unsigned char __cool_image_foo[] = { /* ... */ };

// 2) 句柄槽位 + 注册描述（惰性解析为 coolKernel_t）
static coolKernel_t __cool_kernel_foo = nullptr;
static const hacc::KernelRegistration __cool_reg_foo = {
    "foo", __cool_image_foo, sizeof(__cool_image_foo), &__cool_kernel_foo };

// 3) 调用点改写： foo<<<g, b, s, st>>>(a, b);
{
    int    __a0 = (a);            // 参数按值物化为具名临时
    double __a1 = (b);
    void*  __args[] = { &__a0, &__a1 };
    const dim3 __grid = (g), __block = (b);
    (void)coolLaunchKernel(hacc::resolve(__cool_reg_foo),
                           __grid, __block, __args, (s), (st));
}
```

编译器随附头 `hacc_runtime.h` 承担公共逻辑（保持生成代码最小）：

```cpp
namespace hacc {
struct KernelRegistration {
    const char* name; const void* image; std::size_t imageSize; coolKernel_t* slot;
};
// 首次使用时载入模块并取函数；失败返回 nullptr（不抛异常，符合 C ABI 语义）
coolKernel_t resolve(const KernelRegistration& reg);
}
```

要点与语义细节：

- **惰性注册**：首次启动该内核时用 `coolModuleLoadData` + `coolModuleGetFunction` 解析；
  使用 `std::call_once`/互斥做一次性保护（主机侧代码是普通 C++，**不受** coolrt R 层「无并发原语」约束）。
- **参数语义**：每个实参在启动的**完整表达式**内物化为具名临时（对齐 CUDA 的「按值传参」），
  避免临时量生命周期与求值顺序问题。
- **错误语义**：`<<<>>>` 无返回值。cudart 中启动失败写入 last error。hacc 生成代码**丢弃** `coolLaunchKernel`
  返回值；last error 由 coolrt A 层现有机制覆盖（见 §7 检查项 1，**零改动**）。
- **符号命名**：子集内要求内核名在同一 TU 内唯一；PTX 入口名与主机注册名使用同一确定性方案
  （`extern "C"` 名字直接采用；否则用固定 mangling），保证 `coolModuleGetFunction` 能按名取到。

> **M2 细化方案**（`<<<>>>` 改写、主机桩、内嵌映像、driver 编排、分步实施）见
> [m2-host-path-plan.md](m2-host-path-plan.md)。**M1.2（设备路径）已完成**（`.cu` → EDG IL → LLVM IR → PTX）。

### 6.3 中间产物与命名（一期，参考 nvcc）

一期的中间产物**参考 nvcc 的对应产物**划分与命名，**只保留基本集**；RDC/dlink、fatbin 容器、
cubin/SASS 等不产出（属后续阶段或 Non-Goal）。

| nvcc 产物 | 作用 | hacc 一期对应（参考） |
| --- | --- | --- |
| `.cpp4.ii` / `.cpp1.ii` | host / device 预处理 | 内部步骤，默认不单独保留 |
| `.cudafe1.cpp` | host 纯 C++ TU | `<stem>.hacc.cpp`（`coolfe++` 输出，**未拼桩**） |
| `.cudafe1.stub.c` | host 桩 + 注册 | `<stem>.hacc.stub.cpp`（独立的桩 TU） |
| （nvcc 无对应） | 桩 + host 合并 | `<stem>.hacc.gen.cpp`（主机编译的实际输入） |
| `.cudafe1.gpu` | device 中间代码 | `<stem>.hacc.il`（**EDG IL**，交 `coolicc`） |
| `.ptx` | PTX | `<stem>.ptx`（一期终产物） |
| `.sm_XY.cubin` | SASS | **不产出**（后续 `--emit=cubin`） |
| `.fatbin.c` / `_dlink.*` | 打包 / RDC 链接 | **不产出**（一期无 fatbin 容器、无 RDC） |

- **保留与观测（B7 已定稿）**：成功时默认只保留终产物，`-keep`（`--keep`）保留上述中间产物
  并列出；**失败时始终保留并列出清单**（便于定位断在哪一阶段）。`--keep-dir <d>` 指定
  中间产物目录（默认当前工作目录）；文件名基于**输入文件 stem**，与 `-o` 无关（同 nvcc）。
- **原则**：产物集合以「**最小可端到端**」为准——host 源 / 主机桩 / device 中间表示 / PTX。
- **衔接点（A5，一期基本集）**：`coolfe++` 与 `coolicc` 之间以 **EDG IL** 为交接物——
  `coolfe++` 产出 IL，`coolicc` 消费 IL 并降为 **LLVM IR** → NVPTX → PTX。契合
  [ADR 0001](adr/0001-edg-cpp-frontend.md) 阶段 A（IL 文件、进程隔离）；若 A2 的 IL spike
  判定 IL 表达力不足而切阶段 B，交接形态随之重定（不在本期承诺）。

## 7. 与 coolrt 的 ABI 衔接

hacc 生成的主机 TU 仅依赖 coolrt **现有**公开 ABI，无需运行时改动：

| 用途 | coolrt 符号 |
| --- | --- |
| 载入内嵌设备映像 | `coolModuleLoadData(coolModule_t*, const void* image)` |
| 取内核句柄 | `coolModuleGetFunction(coolKernel_t*, coolModule_t, const char* name)` |
| 启动 | `coolLaunchKernel(coolKernel_t, dim3, dim3, void**, size_t, coolStream_t)` |
| 维度/句柄类型 | `dim3`、`coolKernel_t`、`coolModule_t`、`coolStream_t`（`NULL` = 默认流） |
| 内存/流（用户主机代码） | `coolMalloc` / `coolMemcpy` / `coolStreamCreate` …（即 cudart 对应 `cool*`） |

**衔接检查项（实施前需确认）**：

1. **last error 对齐**：~~需确认 coolrt A 层是否在返回前记录 last error。~~ **已确认（零改动）**：
   `coolLaunchKernel` 失败路径经 `COOL_API_RETURN`/`COOL_API_GUARD` 调用 `setLastError()` 写入线程局部 last error
   （[coolruntime `src/api/kernel_api.cpp`](../../coolruntime/src/api/kernel_api.cpp)、
   [`src/core/error.hpp`](../../coolruntime/src/core/error.hpp)），并已由 `coolGetLastError`/`coolPeekAtLastError`
   对外提供（含 `cuda*` 别名）。故 `<<<>>>` 改写丢弃返回码后仍可经 `cudaGetLastError()` 取回，hacc 侧无需补齐。
2. **PTX JIT 可用性**：`coolModuleLoadData` 依赖驱动的 PTX JIT；目标环境若不可用，需回退到 cubin——
   该回退依赖后的 `--emit=cubin`（直接调用 `ptxas`），**不在本期范围内**。
3. **`-arch` 与真实设备**：`coolModuleLoadData` 载入的 PTX/cubin 的 `sm_XY` 需与设备匹配。
   **已定（B1）**：三级优先——显式 `-arch` → `coolGetDeviceProperties` 的 `major/minor` → 查不到设备时
   回退默认 **`sm_80`**（并提示，非错误）；编译期查设备为「尽力而为」，失败不报错（见 [TODO](TODO.md) B1）。

## 8. 目录结构

```text
hacc/
├── CMakeLists.txt            # 构建：libLLVM + EDG(按需拉取) + 自身（含 coolfe++ / coolicc 可执行）
├── Makefile                  # build / test / fmt / check（对齐 coolruntime）
├── README.md  AGENTS.md
├── include/coolcc/
│   └── hacc_runtime.h      # 随编译器分发：主机桩/注册支持（被生成的 host 代码 include）
├── src/
│   ├── driver/               # 命令行驱动、阶段编排、子进程
│   ├── frontend/             # coolfe++：EDG 集成、CUDA 扩展识别、语义标注
│   ├── device/               # coolicc：AST/IL → LLVM IR → NVPTX → PTX/cubin
│   ├── host/                 # 主机 TU 生成：桩、<<<>>> 改写、内嵌映像与注册
│   └── support/              # 诊断、选项、文件/临时文件、进程封装
├── tests/
│   ├── unit/                 # 各阶段单测（选项、mangling、IR 片段、桩生成）
│   ├── golden/               # 黄金文件：.cu → 期望 PTX/主机桩（可回归比对）
│   └── e2e/                  # 端到端：编译+链接+在 coolrt 上运行并校验结果
├── samples/                  # 可运行 CUDA 子集示例（vectorAdd 等）
└── docs/
    ├── compiler-implementation-plan.md
    └── adr/0001..0005-*.md
```

## 9. CUDA C++ 子集范围（D1，分期）

| 阶段 | 能力 |
| --- | --- |
| **S0 核心** | `__global__` / `__device__` / `__host__`；普通函数调用；算术与流程控制；内置变量 `threadIdx/blockIdx/blockDim/gridDim/warpSize`；`dim3`；内建向量类型 `float2/3/4`、`int2/3/4`、`double2`…；`<<<g,b,s,st>>>`；`__syncthreads()`；`__shared__`；`__constant__`；`-arch=sm_XY`；主机桩+注册+链接；`extern "C"` 内核 |
| **S1 内存与原子** | `__threadfence*`；原子操作 `atomicAdd` 等内建；`__restrict__`；指向 global/shared/local 的指针；设备全局 `__device__` 变量；`cudaMalloc/cudaMemcpy` 等 **`cool*`** 主机 API 混用 |
| **S2 进阶（可选）** | 内核/函数模板；`__launch_bounds__` 提示；内建数学函数（`__fmaf`、`sinf` 等经 NVVM/nvvm 库）；`--emit=cubin`（**直接调用 `ptxas`**） |

> 注：设备产物**一期只到 PTX**；`--emit=cubin` 与 `ptxas` 集成安排在**后续阶段**（见 §2「分期」与 §10）。

**明确排除**：分离编译/RDC、动态并行、`__managed__`、设备端异常/RTTI/协程/虚拟函数与函数指针、
CUDA 库、多 GPU/Peer/Graph 编程。

## 10. 里程碑

| 里程碑 | 交付物 | 验证 |
| --- | --- | --- |
| M0 骨架 | 目录/构建/驱动选项解析；`--version`、`-h`；EDG 按需拉取（§5.2 阶段 A IL）与最短链路打通，**spike 验证 IL 是否保留 CUDA 属性**（地址空间/`__shared__`/同步点） | `make check` 空跑；`hacc --version`；IL spike 结论 |
| M1 设备路径 | 最小内核（无参/标量参）→ PTX；内嵌 NVVM 维度变量与 `__syncthreads` | golden PTX；`ptxas` 可汇编性校验（仅校验，不产出 cubin） |
| M2 主机路径 | 主机桩 + 惰性注册 + `<<<>>>` 改写；端到端 `<<<>>>` 启动 | e2e：vectorAdd 在 coolrt 上跑通并校验结果（PTX 经驱动 JIT 载入） |
| M3 子集 S0 完成 | `__shared__`/`__constant__`/向量类型/dim3；诊断质量 | samples 全绿；边界用例 |
| M4 子集 S1 | 原子/`__threadfence`/设备全局/`__restrict__`（方案见 [m4-subset-s1-plan.md](m4-subset-s1-plan.md)） | 新增 e2e 与 golden |
| M5 稳定 | 诊断、`--emit`/`-keep` 观测输出、CI、文档 | CI 全绿；黄金文件回归 |
| M6（后续）cubin | `--emit=cubin`：**直接调用 `ptxas` 子进程**（PTX → cubin），设备后端不改动 | cubin 载入 e2e；与 PTX 产物对照 |

> **一期范围 = M0–M5**：设备产物为 **PTX**（主机桩/注册/`<<<>>>` 端到端同期交付，PTX 交驱动 PTX JIT 载入）；
> 一期测试可调用 `ptxas` 仅作 golden 校验，**不产出、不内嵌 cubin**。**M6（后续）**才引入 `--emit=cubin`（直接调用 `ptxas`）。

> **当前进度（2026-10）**：**M0–M4 已完成**；M5 的 `--emit` / `-keep` 观测输出（原 TODO 的 B7）
> **已完成**（`make check` **137/137**，真机零 SKIP）；仅 CI 形态（原 C5）仍待定。

## 11. 测试与验证策略

- **单元**：选项/mangling/地址空间映射/桩模板渲染等纯逻辑。
- **golden**：`.cu` → 期望 PTX 与主机桩文本，文本比对；对 PTX 还可用 `ptxas` 做**可汇编性校验**。
- **e2e**：真实编译 + 链接 coolrt + 运行 + 结果断言；参照 coolruntime 的 `test_kernel` 内嵌 PTX 思路，
  但此处由 hacc 从 `.cu` 生成。
- **对照**（可选）：同一 `.cu` 分别经 hacc 与 nvcc 编译，比对运行结果（不比对二进制）。
- 环境不满足（无可用设备 / 无 PTX JIT）时，e2e 用例应可**跳过**而非失败（对齐 coolruntime 的测试取向）。

## 12. 风险与缓解

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| **EDG 集成复杂度高**（前端庞大、后端接口需摸清） | 高 | 先走 **IL 文件松耦合**（阶段 A）；接口点用适配层隔离；EDG 以**按需拉取 + 固定 commit** 锁版本（[ADR 0005](adr/0005-licensing-and-edg-distribution.md)） |
| **`<<<>>>` 非 C++ 语法需词法改写** | 中 | 在预处理器/词法层改写为普通函数调用形式，避免改 EDG 语法 |
| **NVPTX 地址空间/参数 ABI 细节** | 中 | 以最小内核起步，`ptxas` 校验 + golden 回归逐格推进 |
| **`-arch` 与设备/驱动 PTX JIT 不匹配** | 中 | 由 `coolGetDeviceProperties` 决定默认 arch；**后续阶段**提供 `--emit=cubin`（直接调用 `ptxas`）回退 |
| **与 cudart 语义细微差异**（last error、参数物化） | 中 | §7 检查项逐条闭合；e2e 覆盖错误路径 |
| **LLVM/EDG 版本漂移** | 低 | 固定 LLVM 与 EDG 版本；CI 锁定。**LLVM 用系统包并锁 19.x**（`find_package(LLVM 19 REQUIRED CONFIG)`，实测 19.1.7，含 NVPTX；见 [TODO](TODO.md) C1）；EDG 按固定 commit 拉取（[ADR 0005](adr/0005-licensing-and-edg-distribution.md)） |

## 13. 开放问题（均已拍板）

1. **许可证**：~~hacc 自身采用何种许可证？~~ **已定**：采用 **`Apache-2.0 WITH LLVM-exception`**
   （与 EDG/LLVM 同源），已落 [`LICENSE`](../LICENSE) 与 [`NOTICE`](../NOTICE)。见
   [ADR 0005](adr/0005-licensing-and-edg-distribution.md)。
2. **EDG 分发方式**：~~vendored 子模块，还是要求用户自备？~~ **已定**：**不 vendor**，由 CMake
   `FetchContent`/`ExternalProject` 按**固定 commit** 按需拉取（`--filter=blob:none`），
   并支持 `-DHACC_EDG_ROOT=/path` 指向本地副本。见 [ADR 0005](adr/0005-licensing-and-edg-distribution.md)。
3. **前端集成阶段顺序**：~~是否接受先 IL（阶段 A）再回调接口（阶段 B）？~~ **已定**：**先 IL（阶段 A）后回调（阶段 B）**。
   先以 EDG IL 跑通 e2e（M0 spike 验证 IL 是否保留 CUDA 相关属性），再迁到后端回调接口；若 IL 表达力不足则提前切换。
   见 [ADR 0001](adr/0001-edg-cpp-frontend.md)。
4. **默认产物**：~~默认 `--emit=ptx` + 主机目标文件，还是默认一步到可执行？~~ **已定**：**默认一步到可执行**
   （`hacc foo.cu -o foo`，内部串联主机编译器与链接器，对齐 nvcc）；`-c`/`--emit=ptx|cubin`/`-keep` 保留为可配置模式。
   见 [ADR 0003](adr/0003-host-stub-fatbin-registration.md)。
5. **last error 归属**：~~在 hacc 生成代码补齐，还是在 coolrt A 层补齐？~~ **已定**：**依赖 coolrt A 层现有机制（零改动）**。
   已查证 `coolLaunchKernel` 失败路径经 `COOL_API_RETURN`/`COOL_API_GUARD` 写入线程局部 last error，
   `<<<>>>` 改写后 `coolGetLastError()`/`coolPeekAtLastError()` 自动可读；hacc 生成代码不介入。见 [ADR 0003](adr/0003-host-stub-fatbin-registration.md)。
6. **设备产物分期**：~~一期是否直接做 cubin？~~ **已定**：**一期只到 PTX**——整条链路端到端交付，PTX 交驱动 PTX JIT 载入；
   `--emit=cubin` 属**后续阶段**，由**直接调用 `ptxas` 子进程**（PTX → cubin）产出，设备后端不改动；
   一期测试可调用 `ptxas` **仅作 golden 校验**，不产出、不内嵌 cubin。
