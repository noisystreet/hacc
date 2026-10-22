# hacc 一期（M0–M5）待决策清单（TODO）

> 范围：**一期 = M0–M5**——设备产物**止于 PTX**，主机桩 / 惰性注册 / `<<<>>>` 端到端跑通
> （见 [compiler-implementation-plan.md](compiler-implementation-plan.md) §2「分期」、§10 里程碑）。
> `--emit=cubin` + `ptxas` 属**后续阶段**，不在本清单。
>
> 用途：记录一期**尚未拍板**的决策点，逐条闭环后转写进方案 / ADR。已拍板项见方案 §13。
> 勾选 `- [x]` 时，请在对应条目注明落地的方案章节或 ADR。
>
> 关联文档：[compiler-implementation-plan.md](compiler-implementation-plan.md)、
> [ADR 0001–0005](adr/)、[README.md](../README.md)、[AGENTS.md](../AGENTS.md)。
>
> **编号消歧（务必先读）**：本文件里并存两套「M5」编号，含义不同——
> - **里程碑 M5「稳定」**（方案 §10：诊断 / `--emit`/`-keep` / CI / 文档 / golden 回归）；
> - **实施项 M5.1–M5.4 = C7–C10**（循环 / `break`-`continue` / `switch` / 自定义 `__device__` 函数），
>   它们补的是 **ADR 0004 §S0「控制流」** 的缺口，**不是**里程碑 M5 的完成信号。
>
> 当前真实进度（2026-10 复核）：**M0–M4 完成**；C7–C10（M5.1–M5.4）与 B9/B9-2 完成，
> `make check` **199/199**、零 SKIP；**里程碑 M5 仍差 C4（跳过机制）、C5（CI 形态）**。

---

## A. 阻塞开工（不拍板无法起 M0）

- [x] **A1 评审 ADR 0001–0004**：已定稿——ADR 0001–0004 与 0005 均为 `Accepted（已拍板）`。
- [x] **A2 前端集成接口与切 B 阈值**：**已定——一期只做阶段 A（IL 文件接口）**，阶段 B（回调）延后；
      IL spike 判据见方案 §5.2（`__global__`/`<<<>>>`/`threadIdx`/`__syncthreads` 及地址空间等关键属性缺失即判「表达力不足」）。
      **（M0 IL spike）** EDG 已按 ADR 0005 拉取并构建（`cpfe` 7.0）、IL 导出链路已打通
      （`cpfe --edg_base_dir … --g++ -N --il_display`，见 [`tools/il_spike/`](../tools/il_spike/README.md)）；
      **已核验（IL 落盘后全量检索）**：IL 有一等的属性表示（`attribute@…` 实体 + 实体
      `source_corresp.attributes:` 引用，含 `kind`/`family`/`name`/`arguments`）；
      **默认只保留 EDG 已知属性名**（如 `aligned`），未知属性被 `warning: unknown attribute`
      丢弃。**但已实测：EDG 构建期开关 `RECORD_UNRECOGNIZED_ATTRIBUTES=TRUE`
      （`host_envir.h`，零源码改动）即可让未知属性（含名字/参数）随 IL 传递并挂到实体上**——
      即 CUDA 存储/执行限定符以 `__attribute__((…))` 承载，在**阶段 A 内可行**；
      若要正式「注册属性族」，另有 `attribute.c` 的 6 步 recipe（需 EDG 源码补丁）。
      **已拍板：阶段 A 通过（不切阶段 B），(a)/(b) 取舍 = (a)**——EDG 构建期开启
      `RECORD_UNRECOGNIZED_ATTRIBUTES`（零源码补丁）。方案 §5.2 / [ADR 0001](adr/0001-edg-cpp-frontend.md) 已回写；
      **已实现**：`tools/il_spike/fetch_edg.sh` 注入该开关，`verify_attrs.sh` 实测通过；
      **CUDA 属性清单**已细化为契约文档 [`tools/il_spike/cuda-attributes.md`](../tools/il_spike/cuda-attributes.md)。
      （`cdisp` 二进制 IL 往返已查清：含任意 `__attribute__` 时 `cdisp` 断言崩溃，属 EDG 上游 bug，
      与本次开关无关；详见 spike README。）
- [x] **A3 `coolfe++` / `coolicc` 的形态**：**已定——独立可执行文件**，由 `hacc` driver 子进程编排
      （降低耦合、便于独立验证）。方案 §4/§5/§6.1 已回写。
- [x] **A4 前端↔设备端的中间产物切分与命名**：**已定——参考 nvcc、一期只保留基本集**
      （host 源 / 主机桩 / device 中间表示 / PTX；不产 cubin/fatbin/dlink）。方案 §6.3 已回写。
- [x] **A5 「EDG AST/IL → LLVM IR」衔接点的具体形态**：**已定——一期以 EDG IL 为交接物**
      （`coolfe++` 产 IL → `coolicc` 消费并降为 LLVM IR）；IL 结构留待 EDG 接入阶段。方案 §6.3 已回写。

## B. 一期接口 / 语义细节

- [x] **B1 默认 `-arch` 的确定方式**：**已定——三级优先：显式 `-arch` → 查设备 → 回退默认 `sm_80`**。
      1. 用户显式 `-arch=sm_XY` → 直接用（最高优先级）。
      2. 未给 `-arch` 且 `coolGetDeviceProperties` 成功 → 用设备 `major.minor` 映射为 `sm_XY`。
      3. 未给 `-arch` 且查不到设备（无 GPU / 驱动不可用）→ **回退默认 `sm_80`**（Ampere），
         并向 stderr 打印提示（非错误）：`hacc: 未指定 -arch 且未检测到设备，回退默认 sm_80（可用 -arch=sm_XY 覆盖）`。
      - **编译期查设备是「尽力而为」**：查失败**不报错**，仅降级到默认 + 提示，保证无卡机器/CI 也能编译
        （对齐方案 §11「无 GPU 时用例可跳过而非失败」）。
      - `sm_XY` → NVPTX `-mcpu=sm_XY` + 对应 PTX ISA 版本（方案 §6.1）。方案 §7 检查项 3 已同步。
- [x] **B2 内核名 / mangling 规则**：**已定——`extern "C"` 直用原名；C++ 链接用 Itanium ABI mangling + `_hacc_k_` 前缀**。
      - `extern "C" __global__ void foo(...)` → 符号名 **`foo`**（原名，不加前缀；符合 `extern "C"` 语义）。
      - `__global__ void foo(...)`（C++ 链接）→ **`_hacc_k_<Itanium-mangled>`**，如 `_hacc_k__Z3foov`。
      - **不自造 mangling**：复用 Itanium ABI（经 LLVM 的 mangle/demangle，C1 已链 LLVM 19）；
        `_hacc_k_` 前缀隔离命名空间，避免与主机普通符号撞名。
      - **三处同名、同一来源**：PTX `.entry` 名 = 主机注册名（`coolModuleGetFunction`）= 主机桩引用名，
        由 `coolfe++` 在 IL 中标注（`cool_symbol_name` 或等价字段），`coolicc` 与主机生成器都读它。
      - **约束**：同一 TU 内内核名唯一（[ADR 0003](adr/0003-host-stub-fatbin-registration.md)）；跨 TU 一期不支持（见 C3）。
      - **确定性**：同一源码 → 同一名字，保证 golden 回归可复现。ADR 0003「命名约束」段已同步。
- [x] **B3 参数按值物化生成细则**：**已拍板（分两步）**——见 [ADR 0003](adr/0003-host-stub-fatbin-registration.md)「语义对齐要点」段。
      **B3-a（标量，已定）**：① **临时量命名**——`haccLaunch` 模板参数 `Args... args` 按值传入，
      本身即具名临时，`void* argv[] = {(void*)&args...}` 仅取地址；**不生成显式命名临时**（隐式已满足，
      显式需改写器介入且无收益）。② **求值顺序**——遵循 C++ 函数调用规则（C++17 起未指定），但 `argv`
      仅取已构造形参地址，无副作用/顺序依赖，故不影响语义。③ **生命周期**——按值形参覆盖整个
      `haccLaunch` 调用（含 `coolLaunchKernel`）。
     **B3-b（聚合/向量）——✅ 已完成（M3.0）**：主机侧 `haccLaunch` 对 POD 聚合（如 `int3`）已可编译；
     设备侧 `il_to_llvm` 已支持聚合参数（M3.0）。三处协同已落地——① coolrt 提供向量类型
     （`float2/3/4` 等，提交 `f3cfbdd`）；② 向量类型映射为 LLVM `<N x T>`（PTX `ld.param.v4.f32`）；
     ③ 聚合参数 PTX ABI 经 spike 确认为 **byval**（NVPTX 自动发射，无需拆解）。e2e 真跑通过
     （`vectorType OK: 110.000000`）。
- [x] **B4【跨项目】默认流语义 × coolruntime `COOL_STRICT_DEFAULT_STREAM`**：**已定 —— `NULL` = CUDA legacy 默认流，不引入该开关**（coolruntime 的 ADR 0005 `docs/adr/0005-default-stream-semantics.md`：与所有 blocking 流隐式同步，不支持 per-thread 默认流，该开关自 P2 起从未存在，决定不引入）。
      因此 hacc 生成 `<<<>>>`（`stream=NULL`）的行为**等同 nvcc 默认编译结果**，符合预期。
- [x] **B5 `dim3` 等类型的来源**：**已定 —— `dim3` 由 coolrt 公开头（`cool_types.h`）唯一提供**。
      - hacc 的 `hacc_runtime.h` **只消费不定义**：显式 `#include <cool/cool_types.h>` 声明依赖，
        并加 `static_assert(std::is_trivially_copyable<dim3>)` 锁住「按值传 C ABI」的前提。
        （此前是靠 `cool_runtime_api.h` 间接拿到的隐式依赖。）
      - 依据：`coolLaunchKernel(kernel, dim3, dim3, ...)` 的 grid/block 按值传递，两侧必须是同一
        类型（ODR / ABI 一致性）；另立同义类型在 C++ 下 ODR 冲突、在 C 下成为两个不兼容结构体。
      - 顺带补齐 **CUDA 兼容的 `dim3(x,y,z)` 构造函数**（coolrt 侧，默认各维为 1）；设备侧仍按
        12 字节聚合传参，ABI 不变。
      - 验证：coolrt `test_dim3`（构造 / 默认值 / ABI / 非聚合）、hacc `e2e_hacc_dim3_ctor_build`
        / `_run`（真机 `dim3Ctor OK`）；`make check` coolrt **9/9**、hacc **139/139**。
      - 已知限制（不是 B5 引入的）：`<cstdio>` 等 C++ 包装头会拉进 libstdc++ 的 `c++config.h`，
        EDG 不认其中的 `__has_builtin(...)` → 前端成片报错。写 `.cu` 请用 `<stdio.h>` 等 C 头。
- [ ] **B6 惰性注册的生命周期 / 回收**：模块何时 `coolModuleUnload`（`atexit`？全程不卸？）
      ——[ADR 0003](adr/0003-host-stub-fatbin-registration.md) 只讲「惰性加载」，未讲「卸载 / 释放」。
- [x] **B7 `--emit` / `-keep` 的产物集与命名**：**已定稿**（对应可观测性目标 G5）。
      - 命名（基于输入 stem，与 `-o` 无关）：`<stem>.hacc.il`（EDG IL）/ `.hacc.cpp`（前端 host 源，
        **未拼桩**）/ `.hacc.stub.cpp`（主机桩）/ `.hacc.gen.cpp`（桩 + host，主机编译输入）/ `.ptx`。
      - 行为：成功默认删除、`-keep` / `--keep` 保留并列出；**失败始终保留并列出清单**；
        `--keep-dir <d>` 指定目录（默认当前工作目录）。
      - `--emit=` 统一解析，未知值明确报错（`可选：exe / ptx / il / host`）。
      - 顺带修复：拼桩不再就地覆盖 `<stem>.hacc.cpp`（此前 `-keep` 看不到前端原始输出）。
      - 顺带重构：`take_value_option` 抽出 `take_prefixed_value`，CCN 11 → 7。
      - 新增 11 个 CLI 测试（`cli_keep_*` / `cli_default_clean_*` / `cli_failure_keep_*` /
        `cli_emit_ptx_keep_*` / `cli_emit_unknown_*`）；`make check` **137/137**。
- [x] **B8 主机编译器 / 链接器选择**：**已定——默认 `g++`，可用 `--host-cxx <path>` / `--host-cxx=<path>` /
      `$HACC_HOST_CXX` / `$HACC_GXX`（旧名兼容）覆盖；`clang++` 已验证可编**（`e2e_hacc_host_cxx_clang_build`）。
      - 解析收敛到 `src/support/toolchain.cpp`（`resolve_host_cxx`），避免 driver 与前端两处各自
        解析导致优先级漂移。
      - **前端系统头探测固定优先 `g++`**，与主机编译器解耦：EDG 的 `--g++` 模式按 GCC 语义解析，
        而 clang++ 的 resource-dir 内建头（`stddef.h` 等用 `__has_feature` / `__building_module`）
        会让 EDG 成片报错（实测 55 个错误、`size_t` undefined）。宿主编译器仅作 g++ 缺失时的回退。
      - 可观测性：stderr 打印 `hacc: 主机编译器 = <path>`；失败阶段名带编译器
        （`主机编译（clang++）失败: ...`）；显式空值 fail loud。
      - 验证：`unit_toolchain`、`e2e_hacc_host_cxx_missing` / `_empty`（WILL_FAIL）、
        `e2e_hacc_host_cxx_clang_build` / `_clang_run`（真机 `vectorAdd OK`）；`make check` **126/126**。

- [x] **B9【跨项目·coolrt P5.1 暴露】位运算符 `<<` `>>` `&` `|` `^` `~`** —— **已完成**。
      coolrt 已声明 `atomicAnd` / `atomicOr` / `atomicXor`，hacc 也已能映射到
      `atomicrmw and/or/xor` → `atom.global.{and,or,xor}.b32`，但 `il_to_llvm` 的表达式翻译
      （M1.2.3）原本只覆盖「算术 / 比较 / 赋值 / 取址 / 解引用 / 转换」，**位运算符一个都没有**。
      后果很具体：用户写不出 `atomicOr(&flag, 1u << tid)` 这种最常见的用法，只能传字面量
      或已算好的变量 —— P5.1 的 e2e 用例原本就是这么写的，编译报
      「不支持的运算符」后才改成常量。

      **实现（EDG 的实际命名与最初假设不同，这里记下以免下次再猜）**：
      - `eok_and` / `eok_or` / `eok_xor`（**不是** `eok_bitand` / `eok_bitor`）
      - `eok_shiftl` / `eok_shiftr`（**不是** `eok_shl` / `eok_shr`）
      - `eok_complement`（**不是** `eok_compl`）
      - 新增 `OpClass::Bitwise` + `is_bitwise_operator()` + `emit_bitwise_binop()` +
        `translate_bitwise()`；`~` 走 `translate_unary` 的独立 case。
      - 右移按符号性分派：有符号 `ashr`、无符号 `lshr`；左移 `shl` 不分符号。
      - 顺带把 `classify_operator` 里那串 `||`（一元运算符判断）抽成 `is_unary_operator()`，
        否则加 `Bitwise` 分支会把圈复杂度顶过门限 10。
      - 浮点操作数直接报错（与 C 一致），不做隐式截断。

      **测试**：`tests/golden/e2e/bitwise.cu`，16 项数值断言 + 6 条 PTX 指令断言
      （`and.b32` / `or.b32` / `xor.b32` / `shl.b32` / `shr.s32` / `shr.u32`）。
      `or` 的断言必须写成 `(^|[^x])or\.b32` —— `xor.b32` 含子串 `or.b32`，不加边界会误判。
      变量一律掺入 `threadIdx.x`，否则常量折叠后 PTX 里根本不出指令，断言就成了空转。
      `make check` **183/183**。

      **B9-2（同一轮补完）**：复合赋值 `+= -= *= /= %=` `<<= >>= &= |= ^=` 与
      **逻辑非 `!` 的语义错误**。

      先做了 spike 才动手，因为「EDG 会不会把 `a += 1` 降级成 `a = a + 1`」是未知的。
      实测答案是**不会**：IL 里保留 `eok_add_assign` / `eok_or_assign` 等节点，
      所以此前 `x += 1` 会直接撞「不支持的运算符」。同一份 IL 还暴露了两件事：
      - 逻辑非在 IL 里是 `eok_not(eok_bool_cast(x))`，而 `eok_bool_cast` 当时完全没处理；
      - `bool` 类型不是独立 kind，而是 `int_kind=char` + `bool_type=TRUE`（映射为 i8）。

      **逻辑非的旧实现是错的**：`eok_not` 落到 `translate_unary` 的 `default` 分支走
      `CreateNot`（按位取反），`!0` 得 -1、`!5` 得 -6 —— 对任意非零输入恒为非零，
      与逻辑非完全相反。它没被暴露是因为 `!x` 前面总套着不被识别的 `eok_bool_cast`，
      编译先失败了，压根走不到 `eok_not`。现在两者合成 `emit_logical_or_bool`：
      先 `to_i1` 归一再按需取反，最后扩回目标类型。

      **复合赋值**不展开成 `a = a op b`，而是「算一次地址 → load → 运算 → store」，
      这样 `p[i++] += 1` 这类左值只求值一次。基础算子映射拆成算术/位运算两个小函数
      （合起来 10 case + default 会顶到 CCN 11）。

      **测试**：`tests/golden/e2e/compound_assign.cu`，21 项数值断言 + 4 条 PTX 断言
      （`and` / `or` / `xor` / `shr.s32`）。PTX 只挑不容易被优化改写的：`b <<= 2` 会被
      LLVM 化成 `add b, b`、`a /= 4` 会被化成移位，硬断言这些会误报。
      中间结果全部写进 `out[]` —— spike 里吃过亏：kernel 若被代数化简成
      `a + ~a = -1`，整条依赖链会被折叠成一条 `mov`，断言就测了个寂寞。
      `make check` **191/191**。

      当初未覆盖的两项**已完成**（C11 前半）：指针复合赋值（`eok_padd_assign` /
      `eok_psubtract_assign`，即 `p += i`）与 `&&` / `||`（`eok_land` / `eok_lor`，**含短路**）。
      见 CHANGELOG 对应条目与 `tests/golden/e2e/logical_ptr.cu`。

## B'. 里程碑实施（M1）

- [x] **M1.1 设备后端（LLVM IR → PTX）**：**已实现**——`coolicc` 接入 LLVM/NVPTX（`-DHACC_WITH_LLVM=ON`，
  系统 LLVM 锁 19.x，见 C1），支持 `--emit=ptx` / `-arch=sm_XX` / `-o`；golden 测试
  `golden_device_empty_kernel`。见 CHANGELOG。
- [x] **M1.2.0 前端（`.cu` → EDG IL）**：**已实现**——`coolfe++` 以子进程调 `cpfe` 产出
      二进制 IL（`-o`）+ 人类可读文本 IL（`--il_display`）；`-DHACC_WITH_EDG=ON` 注入默认路径。
      测试：`unit_edg_invoke`、`golden_frontend_empty_kernel[_text]`。见 CHANGELOG。
- [x] **M1.2.1 设备端 IL 读回（`.cil` → routine 列表）**：**已实现**——`coolicc` 新增 `--emit=il-list`，
      链接 EDG 前端库（`-DHACC_WITH_EDG=ON`）用 `il_read` 读回二进制 IL，列出 file scope 的 routine
      并标记内核（`cool_global` 属性）。新增 `src/device/il_read_backend.{hpp,cpp}`（EDG 适配层，
      EDG 类型不外泄）与未启用 EDG 时的 stub；测试：`golden_device_il_list_empty_kernel`。见 CHANGELOG。
- [x] **M1.2.2 EDG IL → LLVM IR（最小内核）**：**已实现**——`coolicc` 新增 `--from-il`，把 EDG
      二进制 IL（`.cil`）翻译为 LLVM IR 并发射 PTX。新增 `src/device/il_to_llvm.{hpp,cpp}`（EDG 适配层，
      同时 include EDG 与 LLVM 头，对外只暴露 hacc 类型）与未启用 EDG/LLVM 时的 stub；一期只处理带
      `cool_global` 属性的**无参内核**，建空 `Function`（调用约定 `ptx_kernel`，属性 `nvptx-kernel`）。
      测试：`golden_device_il_to_ptx_empty_kernel`（`.cu` → `.cil` → PTX）。见 CHANGELOG。
- [x] **M1.2.3 EDG IL → LLVM IR（标量参数 + 简单语句）**：**已实现**——`coolicc --from-il` 的翻译器
      扩展到标量参数与简单语句（赋值/算术/比较/`if`/`return`/声明/初始化），类型覆盖
      `tk_integer`（i8..i64）/ `tk_float`（float/double）/ `tk_pointer`（ptr）；未覆盖构造 fail loud。
      测试：`golden_frontend_scalar_kernel`、`golden_device_il_to_ptx_scalar_kernel`。见 CHANGELOG。
- [x] **M1.2.4 EDG IL → LLVM IR（内建变量 + `__syncthreads`）**：**已实现**——`coolicc --from-il` 的
      翻译器新增内建变量成员访问（`threadIdx.x` 等 → `llvm.nvvm.read.ptx.sreg.*`）与内建函数调用
      （`__syncthreads` → `llvm.nvvm.barrier0`），按名字识别。`coolfe++` 定义 `-D__COOLCC__` 并自动
      探测 gcc 系统头（`--sys_include`），支持 `.cu` `#include <cool/cool_runtime.h>`（COOLRuntime
      运行时头，`HACC_COOLRUNTIME_DIR` 定位）。测试：`golden_frontend_tid_kernel`、
      `golden_device_il_to_ptx_tid_kernel`（校验 `%tid.x` / `%ctaid.x` / `bar.sync`）。见 CHANGELOG。
- [x] **M1.2.5 端到端（`.cu` → PTX，`hacc` driver 编排）**：**已实现**——`hacc` driver 接入真实流水线，
      解析 `-o` / `-I` / `-arch=sm_XX` / `-keep` / `--emit=ptx`，以子进程编排 `coolfe++`（`.cu` → EDG IL）
      与 `coolicc`（IL → PTX）。子工具按 `argv[0]` 同目录定位（`$HACC_COOLFE` / `$HACC_COOLICC` 可覆盖）。
      新增 `src/driver/pipeline.{hpp,cpp}` 与 `src/support/process.{hpp,cpp}`。测试：`e2e_hacc_scalar_kernel[_ptx]`、
      `e2e_hacc_tid_kernel[_ptx]`。见 CHANGELOG。
- [x] **M1.2 EDG IL → LLVM IR**：**已完成（M1.2.0–M1.2.5）**——见
      [m1.2-il-to-llvm-plan.md](m1.2-il-to-llvm-plan.md)。核心结论：EDG IL 是**内存树**（非文本契约），
      采用**链接 EDG 前端库、内存遍历 IL**（官方「逐函数翻译后丢弃」路径）；阶段 A 的「IL 文件」是
      **二进制 IL**（`il_read` 读回），非文本解析。
      **已拍板**：① `coolicc` 直接链接 EDG 库；② IL 文件 = 二进制 IL；③ 翻译核心 = 独立库
      （`src/device/il_to_llvm/`）；④ **IL 须能 dump 成人类可读**（`cdisp` 对含属性 IL 崩溃，
      改用 `coolfe++` 落文本 + 自建 `hacc-il-dump`，见方案 §7.1）。
      字段级参考见 [edg-il-reference.md](edg-il-reference.md)（§5.5 标量参数、§5.6 内建变量/`__syncthreads`）。
- [x] **M2 主机路径（桩 + 内嵌映像 + `<<<>>>` 改写）**：**已完成（M2.0–M2.4）**——见
      [m2-host-path-plan.md](m2-host-path-plan.md)。目标：让 `__global__` + `<<<>>>` 的源码经 hacc
      编译后在 coolrt 上跑通。关键设计：① `<<<>>>` 在**词法层改写**为 `haccLaunch(...)`（纯文本变换，可独立单测）；
      ② host 源 = **改写后源码**（不需 EDG 代码生成，`__global__` 等由运行时头提供；**但须剔除 `__global__` 函数体**，见方案 §8.2）；
      ③ 主机桩 + 内嵌 PTX 映像 + 惰性注册（`hacc_runtime.h` 收敛公共逻辑，对齐 ADR 0003）；
      ④ driver 编排 `g++` 主机编译与链接。
      **已实现**：M2.0 改写器（`src/host/launch_rewrite`）→ M2.1 `coolfe++` 产 device 改写源喂 cpfe →
      M2.2 运行时头落定 + 桩生成器（`src/host/stub_gen`）→ M2.3 driver 编排主机编译（`--emit=exe` 默认）→
      M2.4 指针算术（`eok_padd`/`eok_psubtract` → GEP）+ 真 vectorAdd e2e。
      **验证**：本机 RTX 4060 上 `hacc vector_add.cu -o exe` 后运行输出 `vectorAdd OK: 256 elements verified`；
      全量 33/33 测试通过。见 CHANGELOG。
- [x] **M3 子集 S0 完成（`__shared__` / `__constant__` / 向量类型 / `dim3`）**：**已完成（M3.0 / M3.1 / M3.2 / M3.3 / M3.4 / M3.5 全部完成）**——见
      [m3-subset-s0-plan.md](m3-subset-s0-plan.md)。目标：补齐 S0 子集中 M1/M2 未覆盖的部分。
      分步：**M3.0 向量类型 + 聚合参数 ABI（✅ 已完成，承接 B3-b）** → **M3.1 `__shared__`（addrspace(3)，
      纯设备侧）（✅ 已完成）** → **M3.3 `dim3` 设备侧（✅ 已完成：3 分量聚合 → struct，修正 12 字节 ABI）** →
      M3.4 诊断格式（源位置）（✅ 已完成：`<文件:行>: <成因>`，见 CHANGELOG）→ **M3.2 `__constant__`
      （✅ 已完成：addrspace(4) + 主机侧符号拷贝，coolrt 已有 `coolMemcpyToSymbol`，无需降级）** →
      **M3.5 samples + 边界用例（✅ 已完成：`samples/` 5 个示例 + CTest 回归；边界用例随 M3.4 落地）**。
      **待确认**：① ~~coolrt 是否新增向量类型~~（已定：新增，提交 `f3cfbdd`）；② ~~coolrt 是否新增
      `coolMemcpyToSymbol`~~（已定：**已有**，M3.2 按完整方案落地）；③ ~~聚合参数 PTX ABI~~（已定：byval，spike 确认）；④ ~~`dim3` 与 `uint3` 关系~~
      （已定：3 分量统一映射为 struct `{T,T,T}`，2/4 分量才走向量 `<N x T>`；见 M3.3）；
      ⑤ ~~诊断格式（C6）~~（已定：`<文件:行>: <成因>`，见 M3.4）。
- [x] **M4 子集 S1（原子 / `__threadfence` / 设备全局 `__device__` / `__restrict__`）**：**M4.0–M4.6 全部完成**——见
      [m4-subset-s1-plan.md](m4-subset-s1-plan.md)。目标：补齐 S1 子集（[ADR 0004](adr/0004-cuda-cpp-subset-scope.md) §S1）。
      分步：**M4.0 属性契约扩展（`cool_restrict` / `cool_device` 变量 + 探针）（✅ 已完成：`verify_attrs.sh` 实测 exit 0）**
      → **M4.1 设备全局 `__device__` 变量（addrspace(1)）（✅ 已完成：golden + e2e 通过，`make check` 73/73）** → **M4.2 `__restrict__`（→ `noalias`）（✅ 已完成：IR 层 golden + e2e 通过，`make check` 81/81）** → **M4.3 `__threadfence*`（→ `llvm.nvvm.membar.*`）（✅ 已完成：PTX golden + e2e 通过，`make check` 88/88）**
      → **M4.4 原子内建（`atomicAdd` 等 → LLVM `atomicrmw`/`cmpxchg`）（✅ 已完成：8 条 PTX golden + 128 线程竞争 e2e，`make check` 100/100）** → **M4.5 指针地址空间传播（`cool_address_space(n)` → 指针类型地址空间）（✅ 已完成：golden 断言 `st.shared`/`ld.shared` + 未标注参数仍走 `cvta.to.global`，e2e 真机通过，`diag_address_space_invalid` 锁住非法取值，`make check` 119/119）** → **M4.6 samples + 边界用例（✅ 已完成：`atomic_reduce` / `device_global` / `threadfence` 三个示例 + `diag_device_var_in_kernel` / `diag_atomic_type_unsupported` 两个 fail-loud 用例，`make check` 112/112）**。
      **M4.7 `cool*` 内存/流 API 混用验证（✅ 已完成：`samples/stream_launch.cu`——默认流 + 自定义流交替启动、`coolStreamSynchronize`、句柄复用，与同步 `coolMemcpy` 混用；四参数启动与 `haccLaunch` 透传机制早已具备，但此前 samples/e2e 无任何 stream 覆盖，属「实现了没人验证」；`make check` 170/170）**。
      **推荐顺序**：先做无 coolrt 依赖的 M4.0/M4.1/M4.2，再跨项目做 M4.3/M4.4。
      **待确认**：① ~~NVVM atomic intrinsic 的具体名字/签名（LLVM 19 NVPTX，需 spike）~~（已决：**改走原生 `atomicrmw`/`cmpxchg`，不用 NVVM atomic intrinsic**——后者只有 `.cta`/`.sys` 两种作用域、无 `.gpu`，表达不出 CUDA 默认的 device scope；实测映射见 [m4-subset-s1-plan.md](m4-subset-s1-plan.md) §3.5）；② ~~**指针地址空间传播范围**：是否需在一期完整区分 global/shared/local。~~（已定：**M4.5 已实现**——按 `cool_address_space(n)` 显式标注传播 0/1/3/4/5，未标注保持 generic，非法值 fail loud；见 [m4-subset-s1-plan.md](m4-subset-s1-plan.md) §3.6）；
      ③ ~~coolrt 侧内建声明（原子/`__threadfence`/`__restrict__`）由谁改~~（已定：**已由 coolruntime 完成**，`cool_restrict` / `__threadfence*` / 原子内建三笔声明已提交并推送）；④ ~~`__device__` 变量主机侧访问~~（✅ 已完成：coolrt 侧 `coolMemcpyFromSymbol` 对齐 `cudaMemcpyFromSymbol`；hacc 侧预扫描收集 `__device__` 变量（排除 `__device__` 函数）、主机桩生成 `coolSymbolDesc` + To/From/取址三包装、`launch_rewrite` 改写三组 API，未知符号保持原样；回归 `tests/golden/e2e/device_symbol_readback.cu` + `test_launch_rewrite` 19–23 + `test_stub_gen` 3c）；
      ⑤ ~~原子内建一期覆盖的类型范围~~（已定：**int / unsigned int / float**；其余类型无重载、编译期报错，由 `diag_atomic_type_unsupported` 锁住，防止类型被悄悄泛化）。

## C. 工程 / 流程

- [x] **C1 LLVM 版本获取与锁定**：**已定——用系统 LLVM，锁定 19.x**。
      EDG 走 `FetchContent` + 固定 commit（[ADR 0005](adr/0005-licensing-and-edg-distribution.md)）；
      **LLVM 用系统包**（`find_package(LLVM 19 REQUIRED CONFIG)`，经 `LLVMConfig.cmake`），
      **不**走 `FetchContent`（LLVM 构建过慢、体积过大）。
      - **锁定版本：LLVM 19.1.7**（本机实测：`llvm-config-19 --version` = `19.1.7`，
        `clang --version` = `Debian clang version 19.1.7`；`--targets-built` 含 **NVPTX**；
        `LLVMConfig.cmake` 位于 `/usr/lib/llvm-19/lib/cmake/llvm/`）。
      - **CMake 约定**：`find_package(LLVM 19 REQUIRED CONFIG)`；用 `LLVM_DIR` 或
        `llvm-config-19` 定位；要求 `LLVM_TARGETS_TO_BUILD` 含 `NVPTX`（否则报错退出）。
      - **CI / 环境**：镜像预装 `llvm-19-dev`（含 NVPTX）；版本不符时给出清晰报错。
      - 与方案 §12「LLVM/EDG 版本漂移」缓解一致（固定版本 + CI 锁定）。
- [x] **C2 命名 / 路径一致性**：**已定——统一为 `include/coolcc/`**；文档与骨架已同步（运行时头 `include/coolcc/hacc_runtime.h`）。
- [ ] **C3 一期是否支持多 `.cu`（多 TU WPC）**：「一步到可执行」默认行为下，多个 `.cu` 各自 host 桩 + 注册
      是否属一期？[ADR 0004](adr/0004-cuda-cpp-subset-scope.md) 仅排除 RDC / 跨 TU 设备函数链接，**未点名 WPC 多 TU**。
- [ ] **C4 golden / 对照策略**：一期是否即引入「与 nvcc 运行结果对照」（方案 §11 标为「可选」）；
      以及**无 GPU / 无 PTX JIT 时的跳过机制**（CTest label / feature）。
- [ ] **C5 hacc 的 CI 形态**：EDG ~853MB 拉取 + 是否有 GPU；coolruntime 的 CI 因无 GPU 仅做格式检查，
      hacc 是否照此。
- [ ] **C6 面向教学的诊断格式**：阶段 / 源位置 / 成因的**具体输出格式**（Coding Rules 仅有原则）。
- [x] **C7 ADR 0004 的 S0「控制流」与实现不符 → 已按路线 ② 补实现（M5.1 完成）**：ADR 0004 §S0 写明
      「语言子集：…控制流…」，而当时 `src/device/il_to_llvm.cpp` 的 `translate_statement` 只覆盖
      `block / expr / decl / init / if / return / empty`——**`while` / `for` / `switch` 全不支持**；
      旁证是 9 个 samples 的内核体内零循环（`for` 只在主机侧 `main()`）。
      **已完成**：`stmk_while` / `stmk_for` / `stmk_end_test_while` 三种循环已降级到 LLVM IR（M5.1），
      `translate_statement` 同时改为查表分派；`make check` **147/147**、零 SKIP。
      负向用例 `diag_unsupported_while` 随功能实现删除（正向覆盖由 `golden_*_loop_forms` 承担）。
      详见 CHANGELOG 的 M5.1 条目。
- [x] **C8 循环控制 `break` / `continue` → 已完成（M5.2）**：`break` 与 `continue` 被 EDG 降为
      `stmk_goto` + `stmk_label`（实测：`k_for_break` 的 IL 里两者成对出现）。实现方式：
      `KernelContext::labels`（label 实体 → LLVM `BasicBlock`）按需建块，前向/后向跳转都成立。
      **关键验证**：`continue` 必须跳 increment 块而非循环头（否则 `i` 不增 → 死循环），
      PTX 实测 `bra $L__BB1_4` 且 `BB1_4` 即 `add.s32 %r6, %r6, %r5`；真机 e2e `loopCtrl OK`。
      `make check` **153/153**。负向用例 `diag_unsupported_break` 随实现删除。
- [x] **C9 `switch` → 已完成（M5.3）**：case 标签在 EDG IL 里是 `stmk_switch_case` 语句，其后同链语句
      即该 case 的 body —— 「顺序执行」天然就是 fallthrough。实现：`KernelContext::case_blocks`
      （case-entry 实体 → BasicBlock）由 `stmk_switch` 预建，`emit_switch_dispatch` 生成比较链，
      `stmk_switch_case` 切插入点。真机 e2e 已验证 case 合并 / fallthrough / default 三种形态。
      `make check` **160/160**。负向用例 `diag_unsupported_switch` 随实现删除。
- [x] **C10 自定义 `__device__` 函数调用 → 已完成（M5.4）**：路线选「发射独立设备函数」而非内联展开。
      `build_kernel` 拆成 `declare_routine` + `define_routine_body`，新增 `emit_all_routines` 分三遍
      （先声明全部设备函数 → 再填 body → 最后建内核），以支持前向/相互调用。
      PTX 产出 `.func` + `call.uni`（callseq 走 `.param` 空间）。`stmt_return` 同步支持返回值。
      真机 e2e 已验证调用链与指针参数传参。`make check` **168/168**。
- [ ] **C11 表达力补遗（非阻塞）**：`__device__` 函数内联优化（目前 `-O0` 下每次真 `call`，
      NVPTX 的 call ABI 有寄存器保存开销，热路径可能需要 `alwaysinline`）；
      `stmk_range_based_for` / `stmk_asm` / `stmk_coroutine` 仍未覆盖；
      递归的设备函数会生成 PTX 但 `ptxas` 会拒绝（CUDA 不支持递归），可在前端提前报错。
      **已完成（C11 前半）**：`&&` / `||`（含短路）与指针复合赋值 `p += i` / `p -= i`，
      见 CHANGELOG 与 `tests/golden/e2e/logical_ptr.cu`（`make check` **199/199**）。
      **剩余**：内联优化、`stmk_range_based_for` / `stmk_asm` / `stmk_coroutine`、递归设备函数拦截。

---

## D. 已知缺陷（待排期修复）

> 详细记录（症状 / 复现 / 根因 / 绕过 / 修复方向 / 验收）在
> [known-issues.md](known-issues.md)。本处只列编号与排期诉求。

- [x] **KI-1 设备全局变量不跨 kernel 共享** —— ✅ 已修（[ADR 0007](adr/0007-module-granularity-per-tu.md)：
      模块粒度改为编译单元；回归测试 `tests/golden/e2e/dev_global_shared.cu`）。
      严重度**高**：跨 kernel 状态**静默失效**，不报错。M4.1 起存在，M4.4 e2e 发现。
      修复前绕过：单 kernel 内完成，或状态改放设备内存。修复方式（已实施）：把 module 从 kernel
      粒度上移到编译单元粒度（连带简化 `haccMemcpyToSymbolAll` 的 fan-out、消除 PTX 映像重复 N 份）。
      **决策见 [ADR 0007](adr/0007-module-granularity-per-tu.md)（Accepted，已实施）**；
      **建议优先于 M4.6**：用户一旦写多 kernel 程序即会撞上，且故障无提示。

---

## 已定（不必再议，供对照）

| 项 | 结论 | 依据 |
| --- | --- | --- |
| 许可证 | `Apache-2.0 WITH LLVM-exception` | [ADR 0005](adr/0005-licensing-and-edg-distribution.md) |
| EDG 分发 | CMake 按需拉取 + 固定 commit，不 vendor | [ADR 0005](adr/0005-licensing-and-edg-distribution.md) |
| 前端集成顺序 | 先阶段 A（IL）后阶段 B（回调） | [ADR 0001](adr/0001-edg-cpp-frontend.md) |
| 默认产物 | 一步到可执行 | [ADR 0003](adr/0003-host-stub-fatbin-registration.md) |
| `last error` 归属 | 依赖 coolrt A 层现有机制（零改动） | [ADR 0003](adr/0003-host-stub-fatbin-registration.md) |
| 设备产物分期 | 一期只到 PTX | 方案 §2、[ADR 0002](adr/0002-llvm-nvptx-device-backend.md) |
| 子集范围 | S0 / S1 / S2 分期 | [ADR 0004](adr/0004-cuda-cpp-subset-scope.md) |
| 主机桩 + 注册方案 | 内嵌映像 + 惰性注册 | [ADR 0003](adr/0003-host-stub-fatbin-registration.md) |
| 命名 | 前端 `coolfe++`、设备端 `coolicc` | [README](../README.md)、方案 §4.1 |

---

## 维护约定

- 本文件是**活文档**：闭环一条即勾选并补落地指引；新增决策点直接追加到对应分组。
- 决策落地后，须回写 [compiler-implementation-plan.md](compiler-implementation-plan.md) 或新增 / 更新 ADR
  （Architecture docs that require human approval before editing，见 [AGENTS.md](../AGENTS.md)）。
- 本清单**不替代**方案 §13（已拍板项）与各 ADR，仅汇集「待定」点。
