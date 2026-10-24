# S2 计划：进阶子集（模板 / 数学内建 / `__launch_bounds__`）

> 状态：**计划中，未开工**（2026-10 立项）。本文件只记方案与实测现状，开工后按项更新状态。
> 上游：[ADR 0004](adr/0004-cuda-cpp-subset-scope.md) §S2 进阶（可选）。
> 相关：[已知缺陷 KI-4](known-issues.md)（递归深度限制）、[ADR 0007](adr/0007-module-granularity-per-tu.md)（设备符号不跨 TU）。
> 实测环境：RTX 4060 Laptop / sm_89 / 驱动 595.58.03 / `make check` 215/215。

## 1. 目标与范围

ADR 0004 的 S2 列了三块，本计划的判断是**它们的成本与收益差异极大，不应作为整体排期**：

| 子项 | 决策 | 一句话理由 |
| --- | --- | --- |
| 模板 `__device__` 函数 | ✅ 做（T1） | EDG 已完成实例化，剩「跳过原型 + 处理重名」 |
| 数学内建 | ✅ 做（T3） | 真实代码高频使用，现状是直接编不过 |
| `__launch_bounds__` | ✅ 做（T4，仅「不报错地忽略」） | 成本极低；映射到 PTX 无实际收益 |
| 模板 kernel | ⚠️ 缓（T2，可砍） | 需改写器 + 桩注册 + mangling 三处联动，成本数倍 |
| cubin / `cudaFuncGetAttributes` 元数据 | ❓ 摸底（T0） | `--emit=cubin`、`coolFuncGetAttributes` 均已在用，可能已完成 |

**总原则**：S2 是「可选进阶」，不得让 S0/S1 已稳定的路径退化。任何一项若必须改动
`<<<>>>` 改写器或桩注册的既有契约，先停下来评估，不硬上。

## 2. 实测现状（2026-10，本机）

以下都不是推测，是用临时探针跑出来的。探针用完已删，结论留档于此。

### 2.1 模板 `__device__` 函数：EDG 已经把活干完了

源码 `template <typename T> __device__ T twice(T v)`，在 kernel 里用 `twice(out[i])`
（推导 `<float>`）与 `twice<int>(3)`（显式）各实例化一次。IL 里的 routine 列表：

```
routine=twice ty_kind=8 ret_kind=15 p0_kind=15 device=1   ← 模板原型（T,T）= 罪魁
routine=k     ty_kind=8 ret_kind=1  p0_kind=7  global=1   ← 非模板 kernel
routine=twice ty_kind=8 ret_kind=4  p0_kind=4  device=1   ← twice<float>，已实例化
routine=twice ty_kind=8 ret_kind=2  p0_kind=2  device=1   ← twice<int>，已实例化
```

`kind` 对照 `agent_space/edg/src/il_def.h` 的 `a_type_kind`：
`1=tk_void`、`2=tk_integer`、`4=tk_float`、`7=tk_pointer`、`8=tk_routine`、**`15=tk_template_param`**。

**三条关键结论：**

1. **实例已经是普通函数**。`twice<float>` 的签名就是 `float(float)`，现有
   `il_to_llvm` 管线完全能翻译它——不需要任何模板类型映射。
2. **只有「模板原型」撞车**。它带着 `T`（`tk_template_param`）进入
   [`map_type`](../src/device/il_to_llvm.cpp) 的 `default:` 分支，报
   「不支持的类型 kind（M1.2.3 仅覆盖标量与指针）」。
3. **同名的三个 routine 都叫 `twice`**。`device_functions` 用 `a_routine_ptr` 做 key
   所以内部查询没事，但 **LLVM 函数名必须唯一**——这是 T1 的第二个工作点。

### 2.2 模板 kernel：坑更隐蔽

`template <typename T> __global__ void kt(T* out, int n)`（未 launch）的 IL：

```
routine=bump ty_kind=8 ret_kind=15 p0_kind=15 device=1   ← 模板原型（签名里有 T）
routine=kt   ty_kind=8 ret_kind=1  p0_kind=7  global=1   ← 原型，但签名「看起来正常」
routine=main ty_kind=8 ret_kind=2  p0_kind=-1
```

`kt` 的原型签名是 `void(float*, int)` —— **指针把 `T` 藏住了**，签名里查不到
`tk_template_param`，不会在 `map_type` 撞车，但函数体里的 `T` 运算无法翻译。

> **设计推论**：**不能用「签名里有没有 `tk_template_param`」来识别模板原型**，
> 会漏判 `kt` 这一类。必须找到 EDG 侧可靠的「这是模板」标志。见 §6。

另一条：`kt<float><<<1, 4>>>(d, 4)` 在**前端改写**阶段就失败——
`coolfe++: 源码改写失败：<<< 前未找到内核名（疑似歧义或非法启动语法）`，
改写器在 `kt<float>` 的 `<` 处就断了。

且**未被 launch 的模板 kernel 不产生实例**（EDG 以使用驱动实例化），所以模板 kernel
必然依赖改写器先认出 launch 语法——这两件事是绑死的。

### 2.3 其它两块（待 T0 摸底）

`grep` 过 `hacc/src`：`template` / `sqrt` / `sinf` / `fmaf` / `launch_bounds` **零命中**。
即数学内建与 `__launch_bounds__` 目前没有任何专门处理，行为未知，需实测（T0）。

## 3. 任务分解

每项都要求「正向用例 + 反向验证」——反向验证指故意把实现改错/期望值改错，确认用例变红。
本项目已有教训：**没验证过会变红的断言，等于没测**（见 KI-4 与 wmma 的 192/256 反向验证）。

### T0 摸底（0.5d，只读）

三个探针，避免把「已经能用」的东西当新功能重做：

1. `--emit=cubin` 到底到哪一步？ptxas 汇编是否已内置（`-keep` 会留 `.ptx`，说明已在调 ptxas）。
2. `__launch_bounds__(256, 4)` 现在什么反应：编过？报错？静默忽略？
3. `sinf` / `sqrtf` / `__fmaf` / `fabsf` 现在什么反应：编不过？编过但 PTX 里是未定义符号？

**验收**：三项各有明确结论，写回本文件 §2.3，不留下「大概吧」。

### T1 模板 `__device__` 函数（1–2d，收益/成本比最高）

1. **找到 EDG 判定「模板原型」的可靠标志**（`a_routine` 或其 `extra_info`/`source_corresp`
   里的字段），而非签名嗅探——理由见 §2.2。
2. 在 [`emit_all_routines`](../src/device/il_to_llvm.cpp) 的三处遍历中跳过模板原型，
   并给**明确诊断**：若某原型没有任何实例，提示「模板未被使用」，而不是静默跳过。
3. **解决同名冲突**：device 函数按签名生成唯一 LLVM 名（mangled 或后缀）。
   kernel 名**不能动**（是 `cuModuleGetFunction` 的外部契约）。
4. `map_type` 遇到 `tk_template_param` 时给出可理解的错误（「模板原型未被实例化」），
   替代现在的「不支持的类型 kind」。

**风险**：第 1 点是唯一不确定项。若无可靠标志，退路是「签名含 `tk_template_param` 跳过」
+「同名多签名按有无 body / 是否被调用判定」，但这属启发式，须先 spike 确认再动手。

**验收**：`tests/golden/e2e/template_device.cu`——`twice<float>` 与 `twice<int>` 同文件共存、
真机数值正确；反向验证（改期望值或改实参）必须变红。

### T2 模板 kernel（2–4d，建议缓）

三处联动，缺一不可：

1. 改写器认出 `kt<float><<<...>>>`（含嵌套 `>>` 与 `<<<` 的歧义，
   [m2-host-path-plan.md](m2-host-path-plan.md) §3.2 已记录该风险：歧义则 fail loud）。
2. 桩生成按**实例**注册，PTX 符号名与 `cuModuleGetFunction` 用的名字对齐。
3. 主机侧能区分 `kt<float>` 与 `kt<int>` 是两个独立注册项。

**建议一期不做**：手写模板 kernel 的场景远少于模板 device 函数，成本却是数倍，
且会牵动已稳定的改写器与桩生成。

**若要做，先 spike**：用一个临时 hack 让 `kt<float><<<>>>` 穿过改写器，确认 EDG 实例出的
`kt<float>` routine 的**名字与签名**，再谈改动。

### T3 数学内建（1–2d，现实收益最高）

真实 CUDA 代码几乎必用 `sqrtf` / `fabsf` / `__fmaf`，现状大概率直接编不过。

1. 按 T0 摸底结果确定失败形态（未定义符号 / 当普通 extern 函数 / 其它）。
2. 建映射表，最小集：`sqrtf`→`sqrt.rn.f32`、`fabsf`→`abs.f32`、`__fmaf_rn`→`fma.rn.f32`、
   `__expf`/`__logf`→`ex2.approx`/`lg2.approx`、`floorf`/`ceilf`、`fminf`/`fmaxf`。
3. **明确精度契约**：`sinf` 走 `sin.approx.f32`（快但不精确）还是精确实现？
   一期只做「能用」，文档写清「approx 指令，不保证与 host `sinf` 逐位一致」，
   **测试用容差（如 1e-5）而不是 `==`**。
4. 未映射的函数必须 **fail loud**，不得静默生成外部符号（否则链接期才炸，或更糟：
   PTX 里留未定义符号）。

**验收**：`tests/golden/e2e/math_builtins.cu`，容差比较；反向验证（改回精确相等）必须变红。

### T4 `__launch_bounds__`（0.5d）

目标只是**「不报错、不静默出错」**：

- 若 EDG 直接忽略 → 文档写明「提示被忽略，无优化效果」，并加用例锁定「带
  `__launch_bounds__` 的 kernel 能编能跑」。
- 映射到 PTX `.maxntid` / `.minnctapersm` 属锦上添花，**不做**：它只影响寄存器分配与
  occupancy，不影响正确性；而 hacc 目前 `-O0`、不做寄存器压力优化，映射了也无收益。

## 4. 顺序与工作量

```
T0 摸底（0.5d）
  └─ T1 模板 device 函数（1–2d）      ← 收益/成本比最高
       └─ T3 数学内建（1–2d）         ← 现实收益最高
            └─ T4 launch_bounds（0.5d）
                 └─ T2 模板 kernel（2–4d，可砍）
```

合计 **约 5–9 天**，其中 T2 占 2–4 天且可整体砍掉，砍掉后约 3–5 天。

## 5. 明确不做（Non-Goals）

- **跨 TU 的模板实例共享** —— 与 [ADR 0007](adr/0007-module-granularity-per-tu.md) 的
  「设备函数不跨 TU」一致，不为此破例。
- **模板特化 / 偏特化 / 可变参模板** —— 一期只做最基础的单一类型参数模板。
- **`__launch_bounds__` 到 PTX 的映射** —— 见 T4，无实际收益。
- **精确 libdevice 数学库** —— 一期只做 approx 指令 + 容差测试。
- **为支持模板而引入 C++ ABI mangling 库** —— device 函数名只需模块内唯一，
  自己定一套简单后缀即可，不引入 Itanium mangling 的复杂度。

## 6. 开工前必须先做的事

**唯一阻塞项：T1 的「模板原型识别」（spike，约 2h）。**

做法：临时在 `emit_all_routines` 打印 `a_routine` 的 `source_corresp` 与
`extra_info` 中与模板相关的字段，对比以下三者的**稳定差异**：

- `twice`（原型，签名含 `tk_template_param`）
- `twice<float>`（实例，签名具体）
- `kt`（原型，**签名被指针藏住，看起来正常**）

找不到可靠标志 → T1 从「小改」退化为「启发式」，需重新评估 T1 是否值得做。
（同类教训：C11 的「递归设备函数前端拦截」就是基于未验证的假设立项，实测后发现
前提根本不成立，见 KI-4。）

## 7. 验收总则

1. 每个任务都要有 **e2e 真机用例**（不是只编过），并做反向验证确认会变红。
2. 新能力若触发未支持构造，必须 **fail loud 且给出源位置**，不得静默产出错误代码。
3. 不得降低现有 215 项测试的覆盖；`make check` 保持零 SKIP。
4. 每项完成时更新本文件的状态、CHANGELOG、README 的测试计数与 TODO 的 S2 条目。
