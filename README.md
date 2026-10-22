# hacc

hacc 是 [coolrt](../coolruntime) 的**编译前端**，用于补上 coolrt 架构中明确列为 Non-Goal 的一环：
**CUDA C++ 编译前端**。它把 **CUDA C++ 子集**源码（`__global__` / `<<<>>>` / `dim3` / 内置变量等）
编译为：

1. **设备端**：`nvptx64` 目标的 **PTX**（可选经 `ptxas` 得到 **cubin**）；
2. **主机端**：普通 C++ 目标文件，含 `__global__` 的**主机桩**与**内核注册**，
   `<<<>>>` 启动被改写为对 coolrt `coolLaunchKernel` 的调用。

目标是在**编程模型语义**上对齐 cudart，让以 CUDA 写法编写的源码能以接近原样的方式编译并运行在 coolrt 上。

> 定位：hacc 是 coolrt 的编译前端，**不是** nvcc 的复刻。范围与边界见
> [ADR 0004](docs/adr/0004-cuda-cpp-subset-scope.md)。

## 命名

本生态在命名上与 NVIDIA/CUDA 生态逐层对位：

| 层 | NVIDIA 生态 | 本生态 |
| --- | --- | --- |
| 厂商 vendor | NVIDIA（`nv`） | **HAPPY**（`ha`） |
| 运行时 / API | CUDA（`cuda*`） | **COOL**（[coolrt](../coolruntime)，`cool*`） |
| 编译器 compiler | `nvcc` | **`hacc`** |
| 前端 front end | `cudafe++` | **`coolfe++`** |
| 设备编译器 device compiler | `cicc` | **`coolicc`** |

`hacc` = `ha`(ppy) + `cc`，即 HAPPY 的 C/C++ 编译器，角色等同 `nvcc`，但**不是**其复刻。

`coolfe++`（前端，基于 EDG C++）与 `coolicc`（设备编译器，LLVM/NVVM）分别对位 `cudafe++` 与 `cicc`；
对位细节见 [compiler-implementation-plan.md](docs/compiler-implementation-plan.md) §4.1。

## 状态

**子集 S1 已完成（M4.0–M4.6）**：设备后端（LLVM IR → PTX，M1.1）、前端
（`.cu` → EDG IL → LLVM IR → PTX，M1.2）、主机路径（M2：`<<<>>>` 改写、主机桩 + 内嵌 PTX
映像 + 惰性注册、driver 编排主机编译链接，**`.cu` 一步到可执行文件**）、子集 S0（M3：
`__shared__` / `__constant__` / 向量类型 / `dim3`）与子集 S1（M4：`__device__` 设备全局 /
`__restrict__` / `__threadfence*` / 原子内建 / 指针地址空间传播 / samples 与边界用例）均已打通。
真 kernel 已在本机 GPU 上跑通并校验结果。

**M5.1 已支持内核内循环**（`while` / `for` / `do-while`），**M5.2 补上 `break` / `continue`**
（EDG 把它们降为 `stmk_goto` + `stmk_label`），**M5.3 补上 `switch`**（比较链 + case 块，
fallthrough 由顺序执行天然成立），**M5.4 补上自定义 `__device__` 函数调用**（发射独立设备函数，
PTX 里是 `.func` + `call.uni`，前向/相互调用皆可）——此前内核体里连循环都写不了，
只能写「一线程一元素」的演示 kernel。剩余边界见 `docs/TODO.md` C11（非阻塞：内联优化、
range-for/asm/coroutine、递归设备函数需前端拦截）。

主机编译默认调用 `g++`；可用 `--host-cxx <path>`（或 `$HACC_HOST_CXX`）切到 `clang++`
（已验证可编）。**前端**系统头探测仍固定取 GCC 的头：EDG 按 GCC 语义解析，clang 的
resource-dir 内建头会让它成片报错，详见 [docs/TODO.md](docs/TODO.md) 的 B8。

> 指针地址空间按 `cool_address_space(n)`（0/1/3/4/5）**显式标注**传播；未标注一律 generic，
> 不猜、不静默升格，非法取值编译期报错。见
> [m4-subset-s1-plan.md](docs/m4-subset-s1-plan.md) §3.6。

```sh
# 一步到可执行文件（默认 --emit=exe）
build/hacc tests/golden/e2e/vector_add.cu -o vector_add
./vector_add        # → vectorAdd OK: 256 elements verified

# 用户示例集（结果自校验；无 CUDA 设备时打印 SKIP 并返回 77）
build/hacc samples/atomic_reduce.cu -o atomic_reduce && ./atomic_reduce
# → atomicReduce OK: sum(0..127) = 8128
```

### 中间产物与 `-keep`

`--emit=exe` 成功时默认只留可执行文件；`-keep`（`--keep`）保留中间产物，`--keep-dir <d>`
指定目录（默认当前工作目录）。**失败时始终保留并在 stderr 列出清单**，便于定位断在哪一阶段：

| 文件（`<stem>` = 输入文件名去扩展名） | 内容 | 对应 nvcc |
| --- | --- | --- |
| `<stem>.hacc.il` | EDG IL（device 中间表示） | `.cudafe1.gpu` |
| `<stem>.hacc.cpp` | 前端输出的 host 源（**未拼桩**） | `.cudafe1.cpp` |
| `<stem>.hacc.stub.cpp` | 主机桩 + 注册 | `.cudafe1.stub.c` |
| `<stem>.hacc.gen.cpp` | 桩 + host（主机编译的实际输入） | — |
| `<stem>.ptx` | 设备映像 | `.ptx` |

`make check`：**191/191 全绿**（golden / e2e / samples / 诊断用例）。子集外的构造一律
**fail loud**（编译失败并给出源位置），不静默产出错误代码——例如 `double` 等未覆盖的
原子类型、函数内的 `__device__` 声明；已确认的缺陷记录在
[known-issues.md](docs/known-issues.md)。

## 构建与使用

```sh
# 默认构建（不依赖 EDG/LLVM，骨架可编译、可测）
make check

# 启用 LLVM/NVPTX 设备后端（C1：系统 LLVM，锁 19.x）
cmake -S . -B build -DHACC_WITH_LLVM=ON -DLLVM_DIR=/usr/lib/llvm-19/lib/cmake/llvm
cmake --build build -j

# M1.1：LLVM IR → PTX（最小内核）
build/coolicc --emit=ptx -arch=sm_80 tests/golden/device/empty_kernel.ll
```

> `HACC_WITH_LLVM=OFF` 时 `coolicc` 仍可构建，但 `--emit=ptx` 走占位实现并明确报错。

## 设计要点（摘要）

- **前端**：基于 **2026-09-30 开源的 EDG C++ 前端**（`github.com/edgcpp/compiler`，Apache-2.0 WITH LLVM-exception）
  做词法/语法/语义，hacc 在其上识别 CUDA 扩展。见 [ADR 0001](docs/adr/0001-edg-cpp-frontend.md)。
- **设备端代码生成**：EDG AST/IL → **LLVM IR** → **NVPTX 后端** → PTX，**不依赖 nvcc**。
  见 [ADR 0002](docs/adr/0002-llvm-nvptx-device-backend.md)。
- **主机端对接**：生成主机桩 + 内嵌映像**惰性注册**，`<<<>>>` → `coolLaunchKernel`，
  **零 coolrt 改动**。见 [ADR 0003](docs/adr/0003-host-stub-fatbin-registration.md)。
- **语言范围**：CUDA C++ 子集，分 S0/S1/S2 三阶段。见 [ADR 0004](docs/adr/0004-cuda-cpp-subset-scope.md)。

## 主要文档

| 文档 | 内容 |
| --- | --- |
| [compiler-implementation-plan.md](docs/compiler-implementation-plan.md) | 编译器实现方案（主文档）：架构、流水线、目录、里程碑、风险 |
| [m1.2-il-to-llvm-plan.md](docs/m1.2-il-to-llvm-plan.md) | M1.2 方案：EDG IL → LLVM IR（选型、翻译核心、分步实施） |
| [m2-host-path-plan.md](docs/m2-host-path-plan.md) | M2 方案：主机路径（桩 + 内嵌映像 + `<<<>>>` 改写） |
| [m3-subset-s0-plan.md](docs/m3-subset-s0-plan.md) | M3 方案：子集 S0 完成（`__shared__` / `__constant__` / 向量类型 / `dim3`） |
| [edg-il-reference.md](docs/edg-il-reference.md) | EDG IL 表项速查（字段级参考，依据 `il_def.h`） |
| [known-issues.md](docs/known-issues.md) | **已知缺陷登记**（症状 / 复现 / 根因 / 绕过 / 修复方向），当前含跨 kernel 设备全局失效 |
| [ADR 0001](docs/adr/0001-edg-cpp-frontend.md) | 前端采用开源 EDG C++ 前端 |
| [ADR 0002](docs/adr/0002-llvm-nvptx-device-backend.md) | 设备端走 LLVM IR → NVPTX → PTX |
| [ADR 0003](docs/adr/0003-host-stub-fatbin-registration.md) | 主机桩 + 内嵌映像注册，`<<<>>>` → `coolLaunchKernel` |
| [ADR 0004](docs/adr/0004-cuda-cpp-subset-scope.md) | CUDA C++ 子集范围与分期 |
| [ADR 0005](docs/adr/0005-licensing-and-edg-distribution.md) | 许可证与 EDG 分发策略 |
| [ADR 0006](docs/adr/0006-il-to-llvm-translation-route.md) | IL → LLVM IR 的翻译路线：链接 EDG 库内存遍历 |
| [ADR 0007](docs/adr/0007-module-granularity-per-tu.md) | 模块粒度为编译单元（修订 ADR 0003，修复设备全局不跨 kernel 共享） |

## 计划的目录结构

```text
hacc/
├── include/coolcc/   # 随编译器分发的运行时头（主机桩/注册支持）
├── src/{driver,frontend,device,host,support}/
├── tests/{unit,golden,e2e}/
├── samples/
└── docs/{compiler-implementation-plan.md, adr/}
```

## 与 coolrt 的关系

```text
foo.cu ──hacc──▶ 设备映像(PTX/cubin) + 主机桩/注册 ──链接──▶ libcoolrt_static ──▶ 运行
                     \__________________ coolModuleLoadData / coolModuleGetFunction / coolLaunchKernel
```

hacc 依赖 coolrt 的现有公开 ABI，**不修改**运行时。

## 许可证与依赖获取

hacc 采用 **Apache-2.0 WITH LLVM-exception**（与 EDG、LLVM 依赖同源），见 [LICENSE](LICENSE)；
第三方归属见 [NOTICE](NOTICE)。

**EDG C++ 前端不随本仓库分发**：约 853MB 的 EDG 源码在配置期由 CMake `FetchContent`/`ExternalProject`
按**固定 commit** 按需拉取（`--filter=blob:none`），也可用 `-DHACC_EDG_ROOT=/path` 指向本地已构建副本。
策略与取舍见 [ADR 0005](docs/adr/0005-licensing-and-edg-distribution.md)。
