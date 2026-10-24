# 已知缺陷登记（Known Issues）

> 用途：记录**已确认但尚未修复**的缺陷。每条包含症状、复现、根因（定位到代码）、影响、
> 当前绕过、修复方向与验收方式。修复后把状态改为「已修」，注明修复提交，不要直接删除条目。
>
> 与 [TODO.md](TODO.md) 的分工：TODO 记「待拍板的决策 / 待排期的里程碑」，本文件记「已确认的
> 行为缺陷」。缺陷修复所需的排期决策仍写进 TODO。

---

## KI-1 设备全局变量不跨 kernel 共享（每个 kernel 各载入一份 module）

- **状态**：✅ **已修**（ADR 0007，模块粒度改为编译单元）
- **发现于**：M4.4 原子内建 e2e（提交 `699083f` 之前的一次中间版本）
- **影响面**：M4.1 起的所有设备全局变量（`__device__`），与原子/栅栏无关
- **严重度**：高（跨 kernel 的设备全局状态**静默失效**，不报错、不崩溃）
- **回归测试**：`tests/golden/e2e/dev_global_shared.cu`（`e2e_hacc_dev_global_shared_run`）

### 症状

kernel A 写入的 `__device__` 变量，kernel B 读不到。每个 kernel 看到的都是自己那份
「模块初始值」，而非上一个 kernel 留下的终值。

单 kernel 内一切正常，所以既有 golden / e2e 全部通过——包括 M4.1 的 `dev_global` 用例
（它只用一个 kernel）。

### 复现

```cuda
__device__ int g = 0;

__global__ void writer() { atomicAdd(&g, 1); }   // 128 线程 → g 应为 128
__global__ void reader(int* out) { out[0] = g; }  // 期望读出 128

int main() {
    writer<<<1, 128>>>(d_out);
    reader<<<1, 1>>>(d_out);
    coolDeviceSynchronize();
    // 实测：out[0] == 0，而非 128
}
```

同期还观察到：`reader` **自己**对同一批全局做的原子是有效的（`atomicExch` 的旧值/新值都正确），
只是看不到 `writer` 的写入——这正好把问题定性为「实例隔离」，而非原子实现有错。

### 根因

`src/host/stub_gen.cpp` 按**内核名**逐个生成模块状态：

```cpp
for (const std::string& name : kernel_names) {
    out += "static const unsigned char __cool_image_" + name + "[] = " + render_byte_array(ptx_text) + ";";
    out += "static coolModule_t __cool_module_" + name + " = nullptr;";
    out += "static std::once_flag __cool_once_" + name + ";";
    // KernelRegistration{ name, image, image_size, &kernel, &once, &module }
}
```

`include/coolcc/hacc_runtime.h` 的惰性注册再对**每个** registration 各做一次：

```cpp
std::call_once(*reg.once, [&reg] {
    coolModuleLoadData(&module, reg.image);            // ← 同一份 PTX 载入 N 次
    coolModuleGetFunction(&kernel, module, reg.name);
});
```

于是 N 个 kernel → N 次 `coolModuleLoadData` → 驱动创建 **N 个 module 实例**，每个实例
各自持有该 PTX 中全部 `.global` 变量的一份独立副本。

顺带两个佐证 / 副作用：

1. `__cool_image_<name>` 把**完整** `ptx_text` 重复渲染 N 份，二进制体积按 kernel 数线性膨胀。
2. M3.2 的 `haccMemcpyToSymbolAll` 必须遍历**所有** registration 去逐个模块填 `__constant__`
   符号——那本身就是在给这个设计打补丁。

### 影响

- 跨 kernel 的设备全局状态一律不可用：全局计数器、设备侧配置、分阶段归约的收尾读取、
  「kernel A 准备数据 / kernel B 使用」这类常见写法全部失效，且**无任何报错**。
- 用户若从小 kernel 数迁移到多 kernel 程序，会遇到「单测全绿、实际行为错」的静默故障。

### 当前绕过

需要跨 kernel 的状态放到**设备内存**（`coolMalloc` + 指针参数）里，而非 `__device__` 变量；
或把逻辑收进单个 kernel，用块内 `__syncthreads()` 同步。
M4.4 的 e2e（`tests/golden/e2e/atomics.cu`）即采用后者，并在文件头注明了原因。

### 修复方向（已实施）

> **已按 [ADR 0007](adr/0007-module-granularity-per-tu.md) 实施**：`ModuleRegistration`（每 TU 一个）
> 持有映像/模块/once，`KernelRegistration` 只留 `name` + `owner` + 槽位 + 取函数 once；
> PTX 只渲染一份。`haccMemcpyToSymbolAll` 的 fan-out 基数由「内核数」降为「TU 数」。

1. 把「模块」从 kernel 维度**上移到编译单元维度**：同一份 PTX 只载入一次，
   `KernelRegistration` 只保留 `name` + `kernel` 槽位，`module`/`once` 提到单元级。
2. 首启时一次 `coolModuleLoadData`，随后对每个 kernel 只做 `coolModuleGetFunction`。
3. 连带简化 `haccMemcpyToSymbolAll`：只有一份 module，无需遍历 fan-out
   （`__cool_all_regs` 的符号填充逻辑可退化为单模块操作）。
4. 注意生命周期：`coolModuleUnload` 现按注册项隐式管理，上移后需明确「进程级不卸载」或
   加显式的收尾钩子。
5. 兼容多 `.cu` 混合编译：若将来一次链接包含多个编译单元，需要按 TU 各自一个 module，
   而不是全局只有一个。

### 验收

1. 新增 e2e：`writer`（128 线程 `atomicAdd`）→ `reader`（拷出），断言 `out[0] == 128`。
2. 新增 e2e：两个 kernel 各自写**不同**的设备全局，交叉读取，断言互不覆盖、互不丢失。
3. 回归：既有 `make check` 全绿；顺带确认生成的主机桩中 PTX 只出现一次（体积不再随 kernel 数膨胀）。

---

## KI-2 `(void)expr` 生成非法 IR，编译直接失败

- **状态**：✅ **已修**（设备端 `translate_cast` 按「丢弃值」语义处理）
- **发现于**：写 KI-1 的回归测试时，用 `(void)out;` 抑制未使用参数
- **影响面**：设备侧所有 `(void)expr` 表达式语句；与主机路径无关
- **严重度**：中（编译期**失败**，非静默错误；但 `(void)x;` 是极常见的抑制警告写法，
  用户很容易撞上，且报错信息是 LLVM 的 verifier 文案，看不出是哪种写法不被支持）

### 症状

```cuda
__global__ void k(int* out) {
    int v = 1;
    (void)v;        // 或 (void)out;
    out[0] = v;
}
```

编译失败：

```text
coolicc: <hacc-ir>:12:19: error: void type only allowed for function results
  trunc i32 %2 to void          // 操作数为指针时是 `sext ptr %2 to void`
```

### 根因

`src/device/il_to_llvm.cpp` 的 `translate_cast` 把 `(void)e` 当成**真实类型转换**处理：
取 `map_type(expr->type)` 得到 `void` 类型，再走标量转换分支 `CreateIntCast(v, dst)`，
于是生成「转成 void 的转换指令」——而 LLVM IR 中不存在这种指令。

C++ 语义上 `(void)e` 根本不是转换，而是**丢弃值**：求值 `e`、保留其副作用、丢弃结果。

### 修复

`translate_cast` 在算出目标类型后先判 `dst->isVoidTy()`：命中则不生成任何指令，
直接返回操作数的值。操作数已由前面的 `translate_expr` 求值，故副作用（函数调用、
原子操作）完整保留；而 void 表达式的值不可能被使用（不能赋值、不能作实参），
唯一去处是被丢弃的表达式语句（`translate_statement` 的 `stmk_expr`）。

### 验收

`tests/golden/e2e/void_cast.cu`（`e2e_hacc_void_cast_run`），三条断言：

1. 指针 → void（原 `sext ptr` 形态）可编译；
2. 整型 → void 后值不被破坏（`out[0] == 7`）；
3. **`(void)atomicAdd(&g_n, 3)` 的副作用必须保留**（`g_n == 3`）——这条专门防「整条语句
   被忽略」式的错误修复。

实测：撤掉修复重新构建 → 用例在构建阶段失败于 `sext ptr %2 to void`；
恢复修复 → `voidCast OK: v=7 g_n=3`。

### 备注（未覆盖）

`(void)` 只覆盖「显式转 void 的表达式语句」。同类但未处理的情况：`(void)0;` 这类
纯字面量（应由同一路径覆盖，未单独测试）、以及放弃返回值直接用语句调用（`f();`，
本就合法，未受影响）。

---

## KI-3 MMA（wmma）三条红线无任何编译期诊断

- **状态**：⚠️ **未修（已知限制）** —— 属「工具不设防」，不是实现有错
- **发现于**：C12 / [ADR 0008](adr/0008-tensor-core-subset.md) 实施（2026-10）
- **影响面**：所有使用 `cool_wmma_*` 内建的代码
- **严重度**：中（**静默出错**：能编译、能跑、数值不对 —— 与 KI-1 同一类）
- **规范位置**：`coolruntime/include/cool/device_builtins.h` 的 wmma 段注释（hacc 侧不设防）

### 症状

下面三种写法**都能编译通过**、也能正常启动内核，只有结果错（或未定义行为）：

1. **把 mma 放进分支**：`if (threadIdx.x < 16) { cool_wmma_mma(&fd, &fa, &fb, &fc); }`
   —— mma 是 warp 级语义，32 个 lane 必须一致执行到调用点。
2. **传未对齐缓冲**：`cool_wmma_load_a_row(&fa, a + 1);`
   —— PTX 用 `.aligned` 变体，要求 32 字节对齐。
3. **A/B/C/D 传反**：`cool_wmma_mma(&fd, &fb, &fa, &fc);`
   —— C 层能抓（四个不同的结构体名），但**用强制转换绕过**后即静默出错；
   hacc 内部也一律按 32 字节处理，不做 fragment 类型校验。

### 为什么没修

- 第 1 条要判「调用点是否位于 warp-uniform 控制流」，需要控制流分析。一期的 fail-loud
  策略是「不支持就报错」，而这条是「支持了但用错」，语义不同，不能靠同一条路径挡。
- 第 2 条要读类型的 `aligned` 属性并对指针实参做对齐推断 —— hacc 目前**不读** `aligned`
  （会丢弃，见 `map_type` 相关实现），要先补属性传递才谈得上检查。
- 第 3 条已用「四个互不相同的结构体名」把可控部分挡在 C 编译器这一层，剩余的转换绕过
  属用户主动行为。

### 当前绕过

按 `device_builtins.h` 注释执行：整 warp 一致执行、缓冲走 `coolMalloc`（实测 256B 对齐）、
顺序固定 `D = A×B + C`。

### 修复方向（若排期）

优先级：② 对齐检查（属性传递本就要补，收益明确）→ ① warp-uniform 诊断（需控制流分析，
成本最高）→ ③ 保持现状（C 层已挡一部分）。

### 验收方式

`tests/golden/e2e/wmma_e2e.cu` 已能抓第 3 类错误的**数值后果**（A/B 传反 → 192/256 元素
不匹配、worst diff 30 → 测试变红）。第 1、2 类目前**没有**任何回归覆盖；若将来修，
应各加一个「必须编译报错」的诊断用例。
