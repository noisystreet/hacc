# ADR 0003: 生成主机桩 + 内嵌映像注册，`<<<>>>` 展开为 coolLaunchKernel

## Status

Accepted（已拍板）

## Context

coolrt 的启动接口是 **Driver 风格**的：

```c
coolError_t coolLaunchKernel(coolKernel_t kernel, dim3 grid, dim3 block, void** args,
                             size_t sharedMem, coolStream_t stream);
```

而 cudart 的编程模型是：

```c
cudaError_t cudaLaunchKernel(const void* func, dim3 grid, dim3 block, void** args,
                             size_t sharedMem, cudaStream_t stream);
// func 是 __global__ 函数的「主机桩地址」；<<<g,b,s,st>>>(...) 是它的语法糖
```

差异在于 **`func`（主机桩地址）**：cudart 由 nvcc 生成主机桩并借 `fatbin` + 运行期隐式注册
（`__cudaRegisterFatBinary`）把桩与设备映像关联；coolrt **没有** fatbin 注册机制，只提供
`coolModuleLoadData` + `coolModuleGetFunction` 显式取得 `coolKernel_t`。

hacc 需决定如何把「CUDA 写法」桥接到「coolrt 的显式模块 API」：

1. **生成主机桩 + 内嵌映像 + 注册**：为每个 `__global__` 生成主机侧注册代码，
   `<<<>>>` 改写为 `coolLaunchKernel` 调用。
2.（纯编译）**仅产出 PTX/cubin**：加载与启动由用户手动调用模块 API。
3. **为 coolrt 新增 `cudaLaunchKernel` / `__cudaRegisterFatBinary` 式 ABI**，让生成代码更像 cudart。

约束：

- 尽量**不改动 coolrt**（现有 ABI 已足够：`coolModuleLoadData` / `coolModuleGetFunction` / `coolLaunchKernel`）。
- 主机侧为普通 C++，**不受** coolrt R 层「无并发原语」约束，可用 `std::call_once` 等。

## Decision

采用**路线 1：生成主机桩 + 内嵌设备映像 + 惰性注册**，并**保持路线 2 作为可配置模式**。

- 对每个 `__global__` 函数生成：
  - 内嵌映像字节数组（PTX 或 cubin，随 `--emit` 决定）；
  - `coolKernel_t` 句柄槽位 + `KernelRegistration` 描述；
  - 一个**惰性解析**函数：首次启动时 `coolModuleLoadData` + `coolModuleGetFunction`，用
    `std::call_once`/互斥保护，只需一次。
- `<<<grid, block, smem, stream>>>(a, b)` 改写为：

  ```cpp
  void* __args[] = { &(a), &(b) };   // 参数按值物化为具名临时
  (void)coolLaunchKernel(resolve(reg), grid, block, __args, smem, stream);
  ```

- 公共逻辑收敛到编译器随附头 `include/coolcc/hacc_runtime.h`，使生成代码保持最小。
- **零 coolrt 改动**：注册完全建立在现有模块 API 之上，不新增运行时符号。
- `--emit=ptx|cubin` 等纯编译模式作为独立模式保留（路线 2），便于与 nvcc 产物对照与教学。
- **默认产物 = 一步到可执行**：`hacc foo.cu -o foo` 内部串联主机编译器与链接器，
  产出可直接运行的可执行文件，对齐 nvcc 的使用体验；`-c`（仅主机目标文件）、`--emit=ptx|cubin`、
  `-keep`（保留中间产物）作为可配置模式保留。

### 语义对齐要点

- **按值传参**：每个实参在启动的**完整表达式**内物化为具名临时，对齐 CUDA「kernel 参数按值」语义。
- **默认流**：`stream` 省略或为 `NULL` 时即默认流（与 `coolLaunchKernel` 语义一致）。
- **动态共享内存**：`<<<g,b,smem,st>>>` 第三参数映射到 `sharedMem`。
- **错误语义**：`<<<>>>` 无返回值；生成代码丢弃 `coolLaunchKernel` 返回码。**last error 归属已拍板**：
  依赖 **coolrt A 层现有机制（零改动）**——已查证 `coolLaunchKernel` 失败路径经
  `COOL_API_RETURN`/`COOL_API_GUARD` 写入线程局部 last error，故 `<<<>>>` 改写后
  `coolGetLastError()` / `coolPeekAtLastError()`（含 `cuda*` 别名）自动可读，hacc 生成代码不介入。

## Consequences

Positive:

- **语义对齐 cudart**：用户以 `__global__` + `<<<>>>` 编写的源码可近乎原样用 hacc 编译并跑在 coolrt 上。
- **零运行时改动**：不触碰 coolrt 现有 ABI，降低耦合与回归面。
- **可配置**：保留纯编译模式，便于教学对照与与 nvcc 产物比对。
- **注册惰性**：仅在首次使用内核时载入，避免未用内核的无谓开销与初始化顺序问题。

Negative / trade-offs:

- **每次链接需带上 hacc 随附的运行时头**（`hacc_runtime.h`）与 `libcoolruntime`。
- **命名约束**：子集内内核名需在同一 TU 内唯一，以确定性方案生成 PTX 入口名与注册名。
- **last error 已对齐（零改动）**：coolrt A 层已在 `coolLaunchKernel` 失败时写入线程局部 last error，
  故 `<<<>>>` 改写丢弃返回码后，`coolGetLastError()` / `coolPeekAtLastError()` 仍能反映启动失败；hacc 侧无需补齐。
- **参数物化实现需谨慎**：涉及完整表达式求值与临时量生命周期，需单元/端到端覆盖。

## Alternatives Considered

| Option | Pros | Cons |
| --- | --- | --- |
| 仅产出 PTX/cubin（手动加载启动） | 实现最简 | 不提供 `<<<>>>` 语义；用户需手写模块管理 |
| 为 coolrt 新增 cudart 式注册 ABI | 生成代码最接近 cudart | 需改动 coolrt，违背“零改动”取向；引入新 ABI 面 |
| 本决策：主机桩 + 内嵌映像惰性注册（保留纯编译模式） | 语义对齐；零运行时改动；可配置 | 需随附运行时头；命名约束（last error 已由 A 层覆盖） |
