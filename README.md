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

**M1.1 进行中**：骨架（M0）已落地；设备后端已打通 **LLVM IR → PTX**（`coolicc` 接入 LLVM/NVPTX）。
EDG 前端接入（`coolfe++`）与 EDG IL → LLVM IR（M1.2）尚在进行。

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
| [ADR 0001](docs/adr/0001-edg-cpp-frontend.md) | 前端采用开源 EDG C++ 前端 |
| [ADR 0002](docs/adr/0002-llvm-nvptx-device-backend.md) | 设备端走 LLVM IR → NVPTX → PTX |
| [ADR 0003](docs/adr/0003-host-stub-fatbin-registration.md) | 主机桩 + 内嵌映像注册，`<<<>>>` → `coolLaunchKernel` |
| [ADR 0004](docs/adr/0004-cuda-cpp-subset-scope.md) | CUDA C++ 子集范围与分期 |
| [ADR 0005](docs/adr/0005-licensing-and-edg-distribution.md) | 许可证与 EDG 分发策略 |

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
foo.cu ──hacc──▶ 设备映像(PTX/cubin) + 主机桩/注册 ──链接──▶ libcoolruntime ──▶ 运行
                     \__________________ coolModuleLoadData / coolModuleGetFunction / coolLaunchKernel
```

hacc 依赖 coolrt 的现有公开 ABI，**不修改**运行时。

## 许可证与依赖获取

hacc 采用 **Apache-2.0 WITH LLVM-exception**（与 EDG、LLVM 依赖同源），见 [LICENSE](LICENSE)；
第三方归属见 [NOTICE](NOTICE)。

**EDG C++ 前端不随本仓库分发**：约 853MB 的 EDG 源码在配置期由 CMake `FetchContent`/`ExternalProject`
按**固定 commit** 按需拉取（`--filter=blob:none`），也可用 `-DHACC_EDG_ROOT=/path` 指向本地已构建副本。
策略与取舍见 [ADR 0005](docs/adr/0005-licensing-and-edg-distribution.md)。
