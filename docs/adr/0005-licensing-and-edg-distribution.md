# ADR 0005: 许可证与 EDG 分发策略

## Status

Accepted（已拍板）

## Context

hacc 的两种实现形态（见 [ADR 0001](0001-edg-cpp-frontend.md)）都必然**基于外部 C++ 前端**：
- 前端：**EDG C/C++ Front End**（`github.com/edgcpp/compiler`，The C++ Alliance 托管）；
- 设备端后端：**LLVM / NVPTX**。

两者均为 `Apache-2.0 WITH LLVM-exception`；而 hacc 要衔接的 [coolrt](../..) 为 **MIT**。
由此产生两个必须拍板、且互相牵连的问题：

1. **hacc 自身采用何种许可证？** 关键在于：hacc 会把**编译器随附的 runtime 支持**（
   `include/coolcc/hacc_runtime.h` 及生成的桩/注册片段）**嵌入到用户产物**（PTX/cubin 与主机目标文件）中，
   这正是 LLVM-exception 所针对的场景。
2. **EDG 源码（约 853MB）如何分发/获取？** 全量 vendor 会让仓库与克隆体积失控，且不利于版本锁定与升级。

约束与依据：

- EDG：`Apache-2.0 WITH LLVM-exception`，2026-09-30 开源；检出体积约 853MB（`--filter=blob:none` 可显著瘦身）。
- LLVM：`Apache-2.0 WITH LLVM-exception`。
- coolrt：`MIT`（hacc 单向依赖其公开 C ABI，见 [ADR 0003](0003-host-stub-fatbin-registration.md)）。
- LLVM-exception 允许「把编译器 runtime 部分以 Object 形式嵌入产物并再分发」，无需满足 Apache 4(a)/4(b)/4(d)。
- AGENTS.md 红线：不得在未保留许可证与归属的前提下 vendor 第三方源码。

## Decision

### D1 许可证：hacc 采用 `Apache-2.0 WITH LLVM-exception`

- 与两项依赖（EDG、LLVM）**同源**，消除 Apache 专利条款 / 再分发条款的兼容性问题；
  LLVM-exception 恰好覆盖「runtime 嵌入产物二进制」这一 hacc 的核心场景。
- 落 `LICENSE`（Apache-2.0 全文 + LLVM Exceptions）与 `NOTICE`（EDG 与 LLVM 归属声明）。
- 对 coolrt 的 MIT 依赖为**单向**（hacc → coolruntime），各自许可证独立、互不影响。

### D2 EDG 分发：CMake 按需拉取 + 固定 commit，**不 vendor**

- **不**将 EDG 源码纳入 hacc 仓库（不使用 git submodule，不拷贝源码）。
- 通过 **CMake `FetchContent`/`ExternalProject`** 在**配置期按需拉取**，并**固定到具体 EDG commit**
  （commit 记录于仓库内清单文件，作为版本锚点）；拉取使用 `--filter=blob:none` 以减小传输。
- 同时支持 **`-DHACC_EDG_ROOT=/path`**：指向用户本地已拉取/已构建的 EDG 副本，跳过网络拉取，
  适配离线与复用构建缓存的场景。
- 构建基线随 EDG：Python ≥ 3.6、CMake ≥ 3.19、支持 C++11 的编译器、推荐 Ninja。

### D3 第三方归属

- `NOTICE` 列明 EDG 与 LLVM 归属，并保留各自许可证全文（同一份 `LICENSE` 已含 LLVM Exceptions）。
- 分发的产物或二进制包中随附 `LICENSE` 与 `NOTICE`。

## Consequences

Positive:

- **许可证同源、低摩擦**：与依赖一致，无需为「runtime 嵌入产物」单独走 Apache 再分发合规路径。
- **仓库轻量、版本可锁**：853MB 的 EDG 不进仓，克隆与 CI 缓存友好；EDG 版本以 commit 精确锚定。
- **离线/复用友好**：`-DHACC_EDG_ROOT` 支持本地已构建副本与离线环境。
- **归属清晰**：`NOTICE` 满足 Apache 的归属保留要求。

Negative / trade-offs:

- **首次配置需联网拉取** EDG（除非提供 `-DHACC_EDG_ROOT`），CI 需缓存或预热。
- **外部锚点**：EDG commit 需随上游安全/功能更新有节奏地推进，属于长期维护项。
- **构建前置**：EDG 自身的构建依赖（CMake/Ninja/Python）成为 hacc 的隐含前置，文档需明示。

## Alternatives Considered

| Option | Pros | Cons |
| --- | --- | --- |
| **D1: Apache-2.0 WITH LLVM-exception**（本决策） | 与 EDG/LLVM 同源；覆盖 runtime 嵌入产物；含专利授权 | 与 coolrt 的 MIT 不同源（但单向依赖无冲突） |
| D1: 沿用 MIT | 与 coolrt 一致、最宽松 | 仍需随附 EDG/LLVM 的 Apache 声明；自身不再享有专利条款 |
| D1: 双许可 `Apache-2.0 WITH LLVM-exception OR MIT` | 使用者可选、灵活 | LICENSE 与文件头标注复杂，非主流 |
| **D2: CMake 按需拉取 + 固定 commit**（本决策） | 仓库轻、版本可锁、支持本地副本 | 首次配置需联网；引入外部锚点 |
| D2: git submodule vendored | 检出即完整、强绑定 | 克隆体积巨大、CI/子模块管理负担重 |
| D2: 仅文档约定、用户自备 | 仓库最轻 | 上手门槛最高，CI 需自行准备 |
