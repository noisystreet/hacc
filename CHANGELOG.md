# Changelog

本项目遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 与语义化版本（SemVer）。

## [Unreleased]

### Added

- **M4.4 原子内建**：`atomicAdd` / `atomicSub` / `atomicExch` / `atomicMin` / `atomicMax` / `atomicCAS`
  （int / unsigned int / float 重载，coolrt `device_builtins.h` 在 `__COOLCC__` 下声明）映射到
  LLVM 原生 `atomicrmw` / `cmpxchg`，落到 PTX `atom.global.<op>.<type>`。
  **不用** `llvm.nvvm.atomic.*`：那套只有 `.cta` / `.sys` 作用域，表达不出 CUDA 默认的 device
  scope（实测见 `docs/m4-subset-s1-plan.md` §3.5）。有符号/无符号 min/max 分别落 `.s32` / `.u32`。
  新增 golden 测试（逐条断言 8 种 `atom.global.*`）与 e2e（128 线程竞争同一地址，
  `atomics OK: sum=128 ...`）。


- **M4.5 指针地址空间传播**：`map_type` 的 `tk_pointer` 不再一律 `addrspace(0)`，改为读取
  被指类型上的 `cool_address_space(n)` 属性（NVPTX 0/1/3/4/5）。带标注的指针变量，其
  GEP / load / store 带上目标地址空间，PTX 直接落 `ld.shared` / `st.shared` 等专用指令；
  未标注仍为 generic（**不猜、不静默升格**），非法取值 fail loud。
  实测增量：`int* p = sdata;` 经 `p` 的读写此前是 generic `ld`/`st`，现在是 `ld.shared`/`st.shared`。
  新增 `pointer_address_space`（golden 三条 PTX 断言 + 真机 e2e）与 `diag_address_space_invalid`。
  实现细节：`cool_address_space` 对 EDG 是未识别属性，实参以 `aak_raw_token`（字面文本）传递，
  故按 token 解析而非读常量节点。

- **M4.6 samples 与边界用例**（收尾 S1）。新增三个面向用户的示例：
  `atomic_reduce.cu`（`atomicAdd` 并发归约，演示「为什么必须用原子」）、
  `device_global.cu`（`__device__` 全局 + 跨 kernel 共享，依赖 ADR 0007）、
  `threadfence.cu`（`__threadfence*` 三种作用域，说明「栅栏保顺序、不提供同步」）。
  新增两个 fail-loud 边界用例：`__device__` 声明在函数内、`atomicAdd(double*)`
  无匹配重载——后者锁住「原子类型不会被悄悄泛化」。

### Changed

- **`(void)expr` 不再导致编译失败**：此前设备端 `translate_cast` 把「转成 void」当成真实类型转换，
  生成非法 IR（`trunc i32 %2 to void` / `sext ptr %2 to void`），LLVM 报
  "void type only allowed for function results"。现按 C++ 语义处理为「丢弃值、保留副作用」：
  不生成任何转换指令，操作数照常求值（`(void)atomicAdd(&g, 1)` 的原子操作仍会执行）。
  `(void)x;` 是抑制未使用变量警告的常见写法，修复前用户很容易撞上。
  新增 e2e 回归（`tests/golden/e2e/void_cast.cu`，同时断言值不被破坏与副作用不丢失）。

- **`coolfe++` 不再默认产出文本 IL**（`*.il.txt`）。此前 `resolve_outputs` 无条件把 `want_text`
  置为 true，且默认名经 `stem_of()` 丢掉了输入文件的目录前缀——结果是**每次**编译都往调用者
  当前目录丢一个 0.5–1.6 MB 的 IL 文本（`hacc` 驱动每次编译都会调 `coolfe++`，仓库根目录曾因此
  累积 11 MB 残留）。现改为：仅 `--emit-il-text[=F]` 显式请求时产出；省略 `=F` 时落在**输入文件
  旁边**（新增 `sibling_with_ext`），不再污染 cwd。移除已无用的 `text_explicit` 字段。
  相关 golden 测试本就显式传 `--emit-il-text=`，断言不变。

### Added

- 项目骨架（M0）：CMake 构建、`Makefile` 质量入口（`make check`）、`.clang-format`、
  `.gitignore`、`.pre-commit-config.yaml`。
- 三个可执行骨架：`hacc`（驱动，对标 `nvcc`）、`coolfe++`（CUDA 前端，对标 `cudafe++`）、
  `coolicc`（设备端编译器，对标 `cicc`）；均支持 `--version` / `-h`。
- CTest 冒烟测试（unit + 三个 CLI `--version`）。
- 随编译器分发的运行时头占位 `include/coolcc/hacc_runtime.h`。
- **M1.1 设备后端（LLVM IR → PTX）**：`coolicc` 接入 LLVM/NVPTX（`-DHACC_WITH_LLVM=ON`，
  系统 LLVM 锁 19.x，见 C1），支持 `--emit=ptx` / `-arch=sm_XX` / `-o`；最小内核产出
  `.visible .entry`。新增 golden 测试 `golden_device_empty_kernel`。未启用 LLVM 时走占位实现。
- **M1.2.0 前端（`.cu` → EDG IL）**：`coolfe++` 接入 EDG 前端（`-DHACC_WITH_EDG=ON`），
  以子进程调用 `cpfe` 产出 ① 二进制 IL（`-o`，供 `coolicc` 用 `il_read` 读回）与
  ② 人类可读文本 IL（`--il_display`，编译期 dump，规避 `cdisp` 对含属性 IL 的崩溃，见方案 §7.1）。
  新增 `--cpfe` / `--edg-base-dir` / `-o` / `--emit-il-text[=F]` / `-I` / `-D` 选项；
  路径解析优先级：显式参数 > `$HACC_CPFE`/`$HACC_EDG_BASE` > 构建期默认。
  新增单元测试 `unit_edg_invoke` 与 golden 测试 `golden_frontend_empty_kernel[_text]`。
  未启用 EDG 时 `coolfe++` 仍可构建（运行期需显式指定 `cpfe`）。
- **M1.2.1 设备端 IL 读回（`.cil` → routine 列表）**：`coolicc` 新增 `--emit=il-list`，
  链接 EDG 前端库（`-DHACC_WITH_EDG=ON`）用 `il_read` 读回二进制 IL，列出 file scope 的 routine
  并标记内核（`cool_global` 属性）。新增 `src/device/il_read_backend.{hpp,cpp}`（EDG 适配层，
  EDG 类型不外泄）与未启用 EDG 时的 stub；新增 golden 测试 `golden_device_il_list_empty_kernel`。
- **M1.2.2 EDG IL → LLVM IR（最小内核）**：`coolicc` 新增 `--from-il`，把 EDG 二进制 IL（`.cil`）
  翻译为 LLVM IR 并发射 PTX。新增 `src/device/il_to_llvm.{hpp,cpp}`（EDG 适配层，同时 include
  EDG 与 LLVM 头，对外只暴露 hacc 类型）与未启用 EDG/LLVM 时的 stub；一期只处理带 `cool_global`
  属性的**无参内核**，建空 `Function`（调用约定 `ptx_kernel`，属性 `nvptx-kernel`）。
    新增 golden 测试 `golden_device_il_to_ptx_empty_kernel`（`.cu` → `.cil` → PTX）。
- **M1.2.3 EDG IL → LLVM IR（标量参数 + 简单语句）**：`coolicc --from-il` 的翻译器扩展到
  标量参数与简单语句——参数（`tk_routine` 的 `param_type_list` → LLVM 参数）、语句
  （`stmk_expr` / `stmk_if` / `stmk_block` / `stmk_return` / `stmk_decl` / `stmk_init`）、
  表达式（`enk_variable` / `enk_constant` / `enk_operation`：算术/比较/赋值/取址/解引用/转换）、
  类型（`tk_integer` i8..i64 / `tk_float` float/double / `tk_pointer` ptr）。未覆盖的构造一律
  报错（fail loud），不静默产出错误 IR。新增 golden 测试 `golden_frontend_scalar_kernel` 与
  `golden_device_il_to_ptx_scalar_kernel`（`add_kernel(int*, int, int)`，校验参数/算术/分支）。
  实测细节见 [edg-il-reference §5.5](docs/edg-il-reference.md)。
- **M1.2.4 EDG IL → LLVM IR（内建变量 + `__syncthreads`）**：`coolicc --from-il` 的翻译器新增
  两类构造——内建变量成员访问（`threadIdx.x` 等，IL 中为 `eok_dot_field` + `enk_field`）映射到
  NVVM 特殊寄存器读取（`llvm.nvvm.read.ptx.sreg.{tid,ctaid,ntid,nctaid}.{x,y,z}`）；内建函数调用
  （`__syncthreads`，IL 中为 `eok_call` + `enk_routine`）映射到 `llvm.nvvm.barrier0`。内建按**名字**
  识别（与 A2 决策 (a) 一致）。新增 golden 测试 `golden_frontend_tid_kernel` 与
  `golden_device_il_to_ptx_tid_kernel`（`tid_kernel(int*)`，校验 `%tid.x` / `%ctaid.x` / `bar.sync`）。
  实测细节见 [edg-il-reference §5.6](docs/edg-il-reference.md)。
- **`coolfe++` 支持 `#include <cool/...>`（对接 COOLRuntime 运行时头）**：`coolfe++` 无条件定义
  `-D__COOLCC__`（对标 nvcc 的 `__CUDACC__`），使 COOLRuntime 的 `device_builtins.h` 暴露设备端
  内建（`threadIdx` / `__syncthreads` 等）与 `__global__` 等限定符宏；并自动探测 gcc 系统头目录
  以 `--sys_include` 传给 `cpfe`（`cpfe --g++` 不自动带系统 include 路径）。`CMakeLists.txt` 新增
  `HACC_COOLRUNTIME_DIR`（默认 `../coolruntime`，可覆盖）自动定位运行时头；未找到时相关 golden
  测试跳过。
- **M1.2.5 端到端（`.cu` → PTX，`hacc` driver 编排）**：`hacc` driver 从 M0 骨架接入真实流水线——
  解析 `-o` / `-I` / `-arch=sm_XX` / `-keep` / `--emit=ptx`，以子进程编排 `coolfe++`（`.cu` → EDG IL）
  与 `coolicc`（IL → PTX）。子工具按 `argv[0]` **同目录**定位（构建产物在一起），可用
  `$HACC_COOLFE` / `$HACC_COOLICC` 覆盖。新增 `src/driver/pipeline.{hpp,cpp}`（编排逻辑，纯逻辑与
  执行分离）与 `src/support/process.{hpp,cpp}`（通用子进程执行，fork/exec + stderr 捕获）。
  中间 `.cil` 默认删除，`-keep` 保留。新增 e2e 测试 `e2e_hacc_scalar_kernel[_ptx]` 与
  `e2e_hacc_tid_kernel[_ptx]`（`.cu` → PTX，校验内核入口与内建映射）。
- **M2.0 主机源码改写器（`<<<>>>` → `haccLaunch` + 剔除 `__global__` 函数体）**：新增
  `src/host/launch_rewrite.{hpp,cpp}`（纯函数 `rewrite_source`，单趟词法扫描，跳过注释/字符串
  字面量；`<<<` 歧义或括号不配对时 fail loud）。`<<<>>>` 改写为 `haccLaunch(...)`，并剔除
  `__global__` 函数体（host 侧不编译设备代码，见方案 §8.2）。新增单测 `unit_launch_rewrite`
  与 golden `golden_host_vector_add`（对真实 `.cu` 跑改写并与期望 host 源逐字节比对）。
- **M2.1 `coolfe++` 产 device 改写源喂 `cpfe`（含 `<<<>>>` 的 `.cu` → 设备 IL）**：改写器新增
  `keep_global_bodies` 开关（device 源保留 `__global__` 函数体供 cpfe 识别内核，host 源仍剔除）
  并回传 `kernel_names`；新增 `include/coolcc/hacc_device_decls.h`（device 源前置声明 `haccLaunch`
  与 `__COOL_REG_DECL(name)` 宏）。`coolfe++` 生成 device 源（注入声明头 + 每个内核的 `__cool_reg_*`
  声明）写临时文件喂 cpfe，用完删除。新增 IL golden `golden_host_device_il_vector_add`（校验内核
  routine 仍在，防回归）。
- **M2.2 运行时头落定 + 主机桩生成器（内嵌映像 + 惰性注册）**：`include/coolcc/hacc_runtime.h`
  从占位落定为实现——`KernelRegistration`（name/image/size/slot/once）、`resolve`（`call_once`
  惰性 `coolModuleLoadData` + `coolModuleGetFunction`，失败静默返回 `nullptr`）、`to_dim3` 重载、
  `haccLaunch` 模板（实参物化为具名临时，取地址成 `void*[]`）。`resolve` 实现放头文件（inline），
  随生成的 host 源编译、由最终 `g++` 链接 coolrt，hacc 自身工具链不引入 coolrt 依赖。新增
  `src/host/stub_gen.{hpp,cpp}`（为每个内核生成内嵌 PTX 字节数组 + 注册描述，纯文本变换）。
  新增单测 `unit_stub_gen`。
- **M2.3 driver 编排主机编译（`.cu` → 可执行文件）**：`pipeline` 新增 `EmitMode`（`Exe` 默认 /
  `Ptx`）与主机编译阶段——`coolfe++`（产 host 源 + IL）→ `coolicc`（产 PTX）→ 拼桩（读 host 源 +
  PTX，跑改写器拿内核名，`generate_stubs` 拼到 host 源顶部）→ `g++` 编译链接 coolrt。新增
  `resolve_gxx_path` / `build_coolfe_host_args` / `build_gxx_args`；`resolve_coolrt_root` 支持
  显式 > 环境变量 > 构建期默认 > 回退。新增 e2e `e2e_hacc_vector_add_exe`（编译链接成功即通过）。
- **M2.4 指针算术 + 真 vectorAdd e2e 跑通并校验结果**：device 后端新增**指针算术**——EDG 用
  专门运算符 `eok_padd` / `eok_psubtract`（非 `eok_add` / `eok_subtract`）表示指针加减，此前未
  覆盖导致 `*(p + i)` 报「不支持的运算符」。新增 `emit_pointer_arith`：按 pointee 类型发 GEP，
  索引转 i64，减法取负；仅支持「指针 ± 整数」（指针 - 指针留待后续）。`coolfe++` 系统头探测改用
  `g++ -x c++ -E -Wp,-v -`（原 `gcc` 只有 C 头，`<cstdio>` 找不到）。新增
  `tests/golden/e2e/vector_add.cu`（真 vectorAdd：`c[i] = a[i] + b[i]`，`threadIdx.x` 索引，256
  线程，含结果校验；无 CUDA 设备时返回 77 供测试框架 SKIP）。新增测试：指针算术 golden
  `golden_frontend_vector_add_ptr_arith` + `golden_device_il_to_ptx_vector_add_ptr_arith[_gep]`
  （校验 `mul.wide.s32` + `add.s64`）；e2e 真跑 `e2e_hacc_vector_add_run[_build]`（编译 + 运行 +
  校验 `c[i] == 3*i`，`SKIP_RETURN_CODE 77`）。**本机 RTX 4060 上真跑通过**（输出
  `vectorAdd OK: 256 elements verified`）。
- **M3.0 向量类型（聚合参数 + 成员访问）**：device 后端新增**聚合类型**支持——`map_type` 新增
  `tk_struct`/`tk_class`/`tk_union` 分支（`map_struct_type`：遍历 `field_list` 逐成员递归，仅支持
  无位域 POD 聚合）；**向量类型（成员全同标量、2/3/4 个）映射为 LLVM `<N x T>`**（对齐 §8 选型：
  PTX 用 `ld.param.v4.f32` 向量化加载，对齐 16），其余 POD 聚合映射为 `StructType`。新增**普通
  结构体成员访问**（`try_translate_struct_field`：向量类型 load 后 `extractelement`，struct 走
  `CreateStructGEP` + load，按名字在 `field_list` 定位成员索引）。**聚合参数 ABI 经 spike 确认为
  byval**（`.param .align N .b8 [size]`，NVPTX 后端自动发射，无需手动拆解）。
  coolrt 侧 `device_builtins.h` 补全内建向量类型（`float2/3/4`、`int2/4`、`uint2/4`、`double2/4`），
  **带正确对齐**（`float2` align 8、`float4` align 16、`float3` 无 padding align 4，与 CUDA 一致）。
  新增测试：golden `golden_frontend_vector_type` + `golden_device_il_to_ptx_vector_type[_vec]`
  （校验 `ld.param.v4.f32` + `.param .align 16 .b8 add4_param_0[16]`）；e2e
  `e2e_hacc_vector_type_run[_build]`（`float4` 内核参数 + 成员访问，校验 `out == 110`）。
  **本机 RTX 4060 上真跑通过**（输出 `vectorType OK: 110.000000`）。
- **M3.1 `__shared__` 静态共享内存（→ `addrspace(3)` 全局）**：device 后端新增**共享内存**支持——
  `translate_decl` 识别 `cool_shared` 属性，内核内 `__shared__` 变量发射为 `internal addrspace(3) global`
  （名字加 `__shared_` 前缀，避免不同内核同名冲突）；**文件作用域 `__shared__` 报错**（CUDA 不允许）。
  新增 `map_array_type`（`tk_array` → `ArrayType`，仅常量上界）、`eok_array_to_pointer`（数组→指针退化，
  GEP `[0,0]`）、`eok_subscript`（下标 → GEP）；`translate_init` 支持 `dik_constant`/`dik_zero`；
  `KernelContext.locals` 放宽为 `Value*`。新增 e2e `e2e_hacc_shared_reduce_run[_build]`
  （`__shared__ int sdata[256]` + 跨线程读，校验 `sharedReduce OK: 256 elements verified`）。
  **本机 RTX 4060 上真跑通过**。
- **M3.2 `__constant__` 常量内存（→ `addrspace(4)` 全局 + 主机侧符号拷贝）**：新增**常量内存**支持。
  **设备侧**：`il_to_llvm` 识别文件作用域 `cool_constant` 属性，发射 `addrspace(4)` 全局
  （`ExternalLinkage` + **原名**，供运行时按名 `cuModuleGetGlobal` 解析；`isConstant=false` 以避免
  LLVM 把 load 折叠成 initializer）；内核内引用经 `KernelContext.constant_globals` 并入 `locals`
  （新增 `seed_constant_globals`，守住 `build_kernel` 的 CCN 门禁）。**内核内 `__constant__` 报错**
  （CUDA 仅允许文件作用域）。**主机侧**：`KernelRegistration` 新增 `module` 槽位（`resolve()` 保存
  模块句柄）；`hacc_runtime.h` 新增 `haccMemcpyToSymbolAll`（遍历**所有**内核模块填充符号——因每个
  内核是独立模块，同一符号各有一份）；`stub_gen` 为每个 `__constant__` 符号生成 `coolSymbolDesc`
  + 拷贝包装 `__cool_memcpy_to_symbol_<sym>`，并生成全局注册表 `__cool_all_regs[]`。**改写器**：
  `launch_rewrite` 预扫描收集 `__constant__` 名；host 源剔除 `__constant__` 声明（g++ 不认识该关键字）
  并把 `cudaMemcpyToSymbol`/`coolMemcpyToSymbol` 改写为包装调用；device 源把该调用替换为 `(void)0`
  （仅需可解析）。新增 golden `golden_frontend_const_mem` + `golden_device_il_to_ptx_const_mem[_const]`
  （校验 `.visible .const .align 4 .f32 c_scale` + `ld.const.f32`）；e2e `e2e_hacc_const_mem_run[_build]`
  （`__constant__ float c_scale` + `coolMemcpyToSymbol`，校验 `constMem OK: 64 elements verified`）。
  **本机 RTX 4060 上真跑通过**。
- **M3.3 `dim3` 设备侧类型映射（3 分量聚合 → struct）**：修正 `map_struct_type` 的向量识别——
  由「成员数 2/3/4 且全同类型 → `<N x T>`」收紧为「**成员数 2 或 4** 且全同类型 → `<N x T>`」，
  **3 分量类型（`dim3` / `uint3` / `float3`）改映射为 struct `{T,T,T}`**。原因：PTX 只有 `v2`/`v4`
  加载指令、**无 `v3`**，且 LLVM `<3 x T>` 的 store size 会被 padding 到 16 字节，破坏 3 分量类型的
  **12 字节 ABI**（`dim3` 作为内核参数时，主机侧 `sizeof(dim3)==12` 与设备侧 16 字节不匹配，导致
  越界读 + 布局错位）。修正后 `dim3` 参数按 `.param .align 4 .b8 [12]` 传递，成员访问走 struct GEP +
  `ld.param.u32`。新增 golden `golden_frontend_dim3_param` + `golden_device_il_to_ptx_dim3_param[_struct]`
  （校验 `.param .align 4 .b8 dim3_kernel_param_0[12]` + `ld.param.u32`）。**顺带修正 `float3` 的同类
  ABI 隐患**（原 `<3 x float>` 亦为 16 字节）。
- **M3.4 诊断质量（翻译错误带源位置）**：翻译失败消息此前只有成因、无源位置，用户难以定位。
  现按方案 §3.5 输出 `"<文件:行>: <成因>"`——`il_to_llvm` 的 `fail()` 增加可选位置参数，新增
  `format_position()` 用 EDG 的 `conv_seq_to_file_and_line` 把 IL 位置序号转成「文件:行」（`seq==0`
  视为未知）；`translate_expr` 入口补表达式位置（主体抽到 `translate_expr_inner` 以守住 CCN 门禁），
  `build_kernel` 用内核位置兜底语句级错误，文件作用域 `__shared__` 错误补变量位置。`coolfe++` 在
  device 源开头插入的 `#include`/`__COOL_REG_DECL` 会偏移行号，插入后加 `#line 1 "<原始.cu>"`
  重置回原始源行号与文件名。新增 `tests/scripts/check_diag_position.sh` + 两个 golden 用例
  （`diag_bad_op.cu` 不支持的运算符、`diag_bad_shared.cu` 文件作用域 `__shared__`），校验 stderr
  含 `":<行号>: "` 位置前缀。
- **M3.5 samples + 边界用例**：新增面向用户的 `samples/` 示例集（`README.md` + 5 个自包含、
  可编译运行的 `.cu`）：`vector_add`（基础内核）、`vector_type`（`float4` 聚合参数）、
  `shared_reduce`（`__shared__` + `__syncthreads`）、`constant_pi`（`__constant__` +
  `coolMemcpyToSymbol`）、`dim3_grid`（`dim3` 参数）。`tests/CMakeLists.txt` 为每个示例加
  `sample_<name>_build` / `_run` 两个测试（编译 + 运行 + 结果自校验，无设备时按 77 SKIP），
  防止示例腐化。边界用例（fail loud）此前已随 M3.4 落地：`diag_unsupported_while` /
  `diag_unsupported_switch`（子集未覆盖构造）、`diag_type_mismatch`（前端类型错误）、
  `golden_device_local_array_index`（局部数组动态下标正向回归）。本机 RTX 4060 上 5 个示例
  真跑通过；`make check` 68/68 全绿。



### Changed

- **EDG 改为非 ASan 构建，`coolicc` 不再带 ASan**：EDG 在 Linux+Debug 下默认开 ASan
  （`EDG_CPFE_USE_ASAN`），迫使链接方 `coolicc` 也带 `-fsanitize=address`（发布隐患）。
  现 `tools/il_spike/fetch_edg.sh` 传 `-DEDG_CPFE_USE_ASAN=OFF` 重建 EDG，
  `CMakeLists.txt` 去掉 `coolicc` 的 `-fsanitize=address`；`coolicc` 不再依赖 `libasan.so`。
- **lizard 门禁阈值收紧到 CCN 10**：`Makefile` 的 `CCN_THRESHOLD` 由 15 收紧到 10，
  并把 8 个 CCN>10 的函数拆分为职责单一的小函数（行为等价）：`il_to_llvm` 的
  `map_integer_type` / `translate_arith` / `emit_compare` / `build_kernel`，`nvptx_backend`
  的 `emit_ptx`，`edg_invoke` 的 `run_cpfe_once`，`device/main` 与 `frontend/main` 的
  `parse_args`。

### Fixed

- **M1.2.0 `coolfe++` 产出的二进制 IL 无效**：`cpfe` 的 `--il_display` 与 `-o <真实文件>` 不能在同一次
  调用中同时使用（否则 `-o` 写出「显示变体」IL，`il_read` 读回报 `invalid intermediate language file`）；
  且 `--il_display` 不带 `-o` 时会把默认名 `<input-stem>.cil` 的显示变体写到当前目录，可能覆盖二进制 IL。
  现改为**分两次 `cpfe` 调用**（二进制趟 + 文本趟，文本趟用 `-o <临时文件>` 抑制默认写）。
  详见 [方案 §2.4/§7.1](docs/m1.2-il-to-llvm-plan.md) 与 [spike README](tools/il_spike/README.md)。
