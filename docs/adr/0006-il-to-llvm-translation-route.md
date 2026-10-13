# ADR 0006: IL → LLVM IR 的翻译路线：链接 EDG 库内存遍历

## Status

Accepted（已拍板，2026-10）

## Context

[ADR 0002](0002-llvm-nvptx-device-backend.md) 已定「EDG AST/IL → LLVM IR（`nvptx64-nvidia-cuda`）→
NVPTX 后端 → PTX」的大方向；[ADR 0001](0001-edg-cpp-frontend.md) 已定前端为 EDG，且阶段 A 以
**IL 文件**为前后端接口。但「**IL 具体如何翻译成 LLVM IR**」仍是一个独立、有取舍的决策点，
需要单独拍板并记录理由。

约束与依据（详见 [m1.2-il-to-llvm-plan.md](../m1.2-il-to-llvm-plan.md) §2）：

- **IL 是内存中的树，不是稳定文本契约**：IL 由 `il_def.h` 定义全部表项，官方明确「IL 数据结构是
  前端传给后端的**唯一**信息」；`--il_display` 文本仅供人类阅读，**非契约**、随版本漂移。
- **官方列出三种前后端连接方式**：① 内存直传；② IL 文件往返；③ 逐函数翻译后丢弃。官方指出方式 ③
  即「把 IL 翻译成后端使用的另一种中间语言，然后丢弃原 IL」——**正是 hacc 要做的**。
- **EDG 自带源到源后端**（`c_gen_be`，可产 C），理论上可「产 C 再喂 clang」。
- **ADR 0002 取向**：不依赖 nvcc / clang 前端，保持自研链路可控、可观测。
- **阶段 A 的「IL 文件接口」是进程隔离手段**：`coolfe++` 写**二进制 IL**（`il_write`），
  `coolicc` 用 `il_read` 读回成**内存树**；接口是 IL 文件，消费的是内存树，**不是**解析文本。

## Decision

采用 **链接 EDG 前端库、内存遍历 IL 树直接构建 LLVM IR**（方案 A）。

- `coolicc` 链接 EDG 前端库，用 `il_read` 读回二进制 IL，遍历 file scope / function scope 的 IL 树，
  直接构建 `llvm::Module`；翻译核心独立为 `src/device/il_to_llvm/`，便于阶段 B 复用与单元测试。
- **不解析 `--il_display` 文本**：文本非契约、易随版本变、信息可能丢失。
- **不采用「EDG 产 C 再喂 clang」**：引入 clang 依赖、C 往返丢信息，且违背 ADR 0002 取向。
- 与阶段 B 的关系：阶段 A（本决策）`coolfe++` 写 IL 文件 → `coolicc` 读回 → 遍历建 IR（进程隔离）；
  阶段 B 改为单进程回调，但**共用**同一「IL 树 → LLVM IR」翻译核心，仅换「IL 从哪来」。
- 人类可读 dump 作为**可观测性**要求并行落地（`coolfe++` 落文本 + 自建 `hacc-il-dump`），
  但**不阻塞**翻译器实现（翻译器读内存树）。

## Consequences

Positive:

- **官方推荐路径**：EDG 文档明确「IL 树是前后端唯一接口」，方式 ③ 即为 hacc 这类后端设计。
- **信息完整**：内存树含全部字段（属性、类型、`source_corresp`），无文本往返损耗；CUDA 属性
  （`cool_global` 等）经 `source_corresp.attributes:` 完整可达。
- **与阶段 A 兼容**：阶段 A 的 IL 文件是进程隔离手段，`coolfe++`/`coolicc` 均链接 EDG 库
  （`il_write`/`il_read`），接口仍是 IL 文件，符合 ADR 0001 §5.2 阶段 A 定义。
- **不违背 ADR 0002**：不引入 nvcc/clang 前端依赖，保持自研链路可控、可观测（可导出 IR/PTX）。

Negative / trade-offs:

- **与 EDG 库强耦合**：`coolicc` 需链接 EDG 前端库（`HACC_WITH_EDG` 开关 + EDG 构建产物），
  体积增大；需摸清 `il_def.h` 表项。
- **IL 版本漂移**：IL 非稳定契约，需固定 EDG commit（[ADR 0005](0005-licensing-and-edg-distribution.md)），
  升级时回归 golden。
- **memory region 规则**：遍历须遵守「function scope 指针不外泄」，跨函数引用走 file scope。
- **语义降级自担**：EDG 不做优化，IL 贴近源码，翻译到 LLVM IR 需自行做语义降级。

## Alternatives Considered

| Option | Pros | Cons |
| --- | --- | --- |
| **A. 链接 EDG 库，内存遍历 IL**（本决策） | 官方路径；无文本往返；信息最全；与阶段 A 兼容 | 与 EDG 库强耦合；需摸清 `il_def.h` 全部表项 |
| B. 解析 `--il_display` 文本 | 松耦合、易调试 | 文本非契约、易随版本变；信息可能丢失；脆弱 |
| C. 用 EDG 的 C 生成后端（`c_gen_be`）产 C，再喂 clang | 复用成熟后端 | 引入 clang 依赖；C 往返丢信息；违背 ADR 0002「不依赖 nvcc/clang 前端」取向 |

## 参考

- [m1.2-il-to-llvm-plan.md](../m1.2-il-to-llvm-plan.md) §2（调研结论）、§3（方案选型）、§7（决策）。
- [ADR 0001](0001-edg-cpp-frontend.md)（EDG 前端、阶段 A/B）、[ADR 0002](0002-llvm-nvptx-device-backend.md)
  （LLVM/NVPTX 后端）、[ADR 0005](0005-licensing-and-edg-distribution.md)（EDG 固定 commit）。
- EDG 官方文档（`github.com/edgcpp/compiler`，`doc/source/`）：`il.rst`、`lower_il.rst`、`int_overview.rst`。
