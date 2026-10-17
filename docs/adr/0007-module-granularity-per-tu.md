# ADR 0007: 模块粒度为编译单元（修订 ADR 0003 的「每内核一份映像 + 各自载入」）

## Status

Accepted（已拍板，2026-10）

> 本条**修订** [ADR 0003](0003-host-stub-fatbin-registration.md) 中「对每个 `__global__` 生成内嵌映像 +
> 各自惰性 `coolModuleLoadData`」的部分。ADR 0003 的其余决策（主机桩 + 惰性注册 + `<<<>>>` 改写 +
> 零 coolrt 改动 + 保留纯编译模式）**全部继续有效**。
> 按下方「实施步骤」修复 [known-issues KI-1](../known-issues.md)。

## Context

ADR 0003 定的是「每个 `__global__` 函数各生成一份内嵌映像 + 各自的 `KernelRegistration` +
各自惰性解析」，落地后表现为（`src/host/stub_gen.cpp` + `include/coolcc/hacc_runtime.h`）：

```cpp
// 每个内核名一份
static const unsigned char __cool_image_<name>[] = { /* 完整 ptx_text 再渲染一遍 */ };
static coolModule_t      __cool_module_<name> = nullptr;
static std::once_flag    __cool_once_<name>;

// resolve()：每个 registration 各做一次
coolModuleLoadData(&module, reg.image);   // ← 同一份 PTX 被载入 N 次
coolModuleGetFunction(&kernel, module, reg.name);
```

N 个内核 → N 次 `coolModuleLoadData` → 驱动创建 **N 个 module 实例**。而 PTX 中的 `.global`
变量属于 module 作用域，因此每个实例各持一份**独立副本**。

### 后果（实测，非推测）

1. **设备全局变量不跨内核共享**：kernel A 写入的 `__device__` 变量，kernel B 读不到，
   且**不报错、不崩溃**。M4.4 的 e2e 首版即因此失败（`writer` 累加到 128，`reader` 读出 0）。
   详见 [known-issues KI-1](../known-issues.md)。
2. **二进制体积随内核数线性膨胀**：`__cool_image_<name>` 把完整 PTX 重复渲染 N 份。
3. **`haccMemcpyToSymbolAll` 被迫遍历所有内核**：`hacc_runtime.h` 自己的注释已写明
   「由于每个内核是独立模块，同一 `__constant__` 符号在每个模块里各有一份；故需遍历**所有**
   内核注册」——M3.2 已为这个决策付过一次代价。

> 注意：这与 CUDA 语义**不一致**。在 CUDA 中设备全局属于模块（= 一个编译单元/cubin），
> 多个 kernel 共享；cuModuleLoad 一次即全部可见。

## Decision

**把模块粒度从「内核」上移到「编译单元（TU）」**：同一份 PTX 只生成一个映像、只载入一个 module；
内核只做 `coolModuleGetFunction`。

### 数据结构

```cpp
// 每个编译单元（= 一份 PTX）一个
struct ModuleRegistration {
    const char*   name;        // 诊断用（TU / 产物名）
    const void*   image;       // 内嵌设备映像（PTX 文本字节）——只渲染一次
    std::size_t   imageSize;
    coolModule_t* module;      // 惰性解析后填充
    std::once_flag* once;      // 每 module 一个
};

// 每个 __global__ 一个
struct KernelRegistration {
    const char*         name;    // coolModuleGetFunction 按名取
    ModuleRegistration* owner;   // 所属 TU 的模块
    coolKernel_t*       slot;    // 惰性解析后缓存
};
```

`resolve(kernel_reg)` = `resolve_module(*reg.owner)`（`call_once` + 一次 `coolModuleLoadData`）
+ `coolModuleGetFunction(&kernel, module, reg.name)`。

### 不变项（继续遵守 ADR 0003）

- **零 coolrt 改动**：仍只用 `coolModuleLoadData` / `coolModuleGetFunction` /
  `coolLaunchKernel` / `coolMemcpyToSymbol`，不新增运行时符号、不改其 ABI。
- 主机桩 + 惰性注册 + `<<<>>>` → `haccLaunch` 改写；`--emit=ptx|cubin` 纯编译模式保留。
- last error 仍由 coolrt A 层承担，生成代码不介入。

### 生命周期

module **进程级不卸载**（不主动调 `coolModuleUnload`）。理由：设备全局是跨 kernel 的长期状态，
在静态析构阶段卸载会与其他静态对象的析构顺序产生竞态；资源由驱动在进程退出时回收。
若将来需要显式释放，应提供独立 API 而非挂在静态析构上。

### 多编译单元

- 一期（S1）产物是**单 `.cu` → 单 TU → 单 module**，语义与 CUDA 一致。
- 一期**不支持**跨 TU 引用同一个 `__device__` 变量：每个 TU 各自一个 module，各自的 `.global`
  是不同副本。该限制须在文档声明；若将来支持多 TU 链接，需要设备侧符号合并（类似
  `-rdc=true` / 设备 LTO），属独立决策，不在本 ADR 范围。

### 对 `__constant__` 符号的影响

`haccMemcpyToSymbolAll` 的 fan-out **不能完全删除**，但**基数从「内核数」降为「TU 数」**：
单 TU 时退化为一次 `coolMemcpyToSymbol`，多 TU 时仍需遍历各 module。
接口可保留（语义一致，仅参数含义由「内核注册表」变为「模块注册表」）。

## Consequences

Positive:

- **设备全局跨 kernel 共享**，与 CUDA 语义对齐，修复 KI-1。
- 内嵌映像只渲染一份，体积不再随内核数膨胀；模块载入开销从 N 次降为 1 次。
- `haccMemcpyToSymbolAll` 在单 TU 场景下退化为单次调用。

Negative / trade-offs:

- 改动落在**已交付**的 M2/M3 主机路径上（`stub_gen.cpp`、`hacc_runtime.h` 及其消费方），
  回归面需重点覆盖 M3.2 的 `__constant__` 符号 e2e。
- 生成代码的结构变化（多一个 `ModuleRegistration`），`-keep` 产物与既有对照夹具需同步更新。
- 多 TU 的设备全局共享仍不支持（明确记录为限制，见上）。

## Alternatives Considered

| Option | Pros | Cons |
| --- | --- | --- |
| 保持每内核一份 module，改用 `coolMemcpyFromSymbol`/`ToSymbol` 显式同步设备全局 | 不动现有架构 | 需要在每个 kernel 之间手工搬运全局状态，用户代码语义被污染；不可自动化 |
| 每个 `__device__` 变量不做模块级，改为「主机侧指针 + 显式 memcpy」 | 完全绕开模块语义 | 用户代码写法与 CUDA 差异过大，等于不支持 `__device__` 变量 |
| 每内核一份 module，但载入后在各 module 之间手工镜像 `.global` | 不改 ABI | 镜像时机/一致性无法保证（内核随时可能写），且需枚举符号，复杂度高于收益 |
| **本决策：模块粒度 = TU，一次载入** | 语义对齐；体积与载入开销同降；仍零 coolrt 改动 | 触已交付路径，需回归 M3.2 |

## 实施步骤（批准后执行，先红后绿）

1. **加回归测试并确认当前失败**：e2e `writer`（128 线程 `atomicAdd`）→ `reader`（拷出），
   断言 `out[0] == 128`；再加「两内核各写不同全局、交叉读取」用例。
2. `src/host/stub_gen.cpp`：生成 `ModuleRegistration`（每 TU 一个）+ `KernelRegistration`
   （每内核一个，指向 owner）；映像只渲染一次。
3. `include/coolcc/hacc_runtime.h`：`resolve` 拆为 `resolve_module` + 取函数；
   `haccMemcpyToSymbolAll` 改为遍历模块注册表。
4. 回归：`make check` 全绿（含 M3.2 `__constant__` 符号 e2e、M2 的 `<<<>>>` 改写用例）；
   确认生成主机桩中 PTX 只出现一次。
5. 关闭 [known-issues KI-1](../known-issues.md)，在其条目注明修复提交。

> **实施状态：已完成。** 步骤 1–5 全部落地；回归测试为
> `tests/golden/e2e/dev_global_shared.cu`（`e2e_hacc_dev_global_shared_run`），
> 修复前该用例三项断言全为「读到模块初值」（0），修复后为 `counter=128 a=11 b=22`。
> 实施中一处对本文结构的细化：`KernelRegistration` 保留了一个 per-kernel `once_flag`，
> 仅用于保护 `coolModuleGetFunction` 这一步（模块载入由 owner 的 once 保护），
> 避免两线程首次启动同一内核时并发写句柄槽位。

## References

- [ADR 0003](0003-host-stub-fatbin-registration.md)：主机桩 + 内嵌映像注册（本 ADR 修订其模块粒度）
- [known-issues KI-1](../known-issues.md)：设备全局不跨 kernel 共享
- `src/host/stub_gen.cpp`（`generate_stubs`）、`include/coolcc/hacc_runtime.h`（`resolve`、
  `haccMemcpyToSymbolAll`）
