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
      **A2 的「阶段 A 通过 / 切阶段 B」与 (a)/(b) 取舍待维护者据方案 §5.2 拍板**（详见
      [`tools/il_spike/README.md`](../tools/il_spike/README.md)「属性核验结论」）。
- [x] **A3 `coolfe++` / `coolicc` 的形态**：**已定——独立可执行文件**，由 `hacc` driver 子进程编排
      （降低耦合、便于独立验证）。方案 §4/§5/§6.1 已回写。
- [x] **A4 前端↔设备端的中间产物切分与命名**：**已定——参考 nvcc、一期只保留基本集**
      （host 源 / 主机桩 / device 中间表示 / PTX；不产 cubin/fatbin/dlink）。方案 §6.3 已回写。
- [x] **A5 「EDG AST/IL → LLVM IR」衔接点的具体形态**：**已定——一期以 EDG IL 为交接物**
      （`coolfe++` 产 IL → `coolicc` 消费并降为 LLVM IR）；IL 结构留待 EDG 接入阶段。方案 §6.3 已回写。

## B. 一期接口 / 语义细节

- [ ] **B1 默认 `-arch` 的确定方式**：不给 `-arch` 时默认 sm_XX 从何而来？方案 §7 称由
      `coolGetDeviceProperties` 决定，但**编译期能否查设备**（hacc 可能运行于无 GPU 机器）未定。
- [ ] **B2 内核名 / mangling 规则**：方案 §6.2「`extern "C"` 直接用，否则固定 mangling」——
      用 Itanium ABI 还是自定义？且 PTX `.entry` 名须与主机注册名一致。
- [ ] **B3 参数按值物化生成细则**：临时量命名、求值顺序、聚合 / 向量类型（`float4` 等）的传参处理
      （[ADR 0003](adr/0003-host-stub-fatbin-registration.md) 仅给原则）。
- [ ] **B4【跨项目】默认流语义 × coolruntime `COOL_STRICT_DEFAULT_STREAM`**：该开关在
      coolruntime `ARCHITECTURE.md` 仍为未决项（Owner: maintainers，Due: P2）；
      它决定 hacc 生成 `<<<>>>`（`stream=NULL`）的行为是否符合预期。需与 coolrt 对齐。
- [ ] **B5 `dim3` 等类型的来源**：生成的主机代码 include coolrt `include/cool/*.h`；
      但 `dim3` 等由 **coolrt 公开头**提供，还是由 `hacc_runtime.h` 提供？需确认，避免缺失 / 重定义。
- [ ] **B6 惰性注册的生命周期 / 回收**：模块何时 `coolModuleUnload`（`atexit`？全程不卸？）
      ——[ADR 0003](adr/0003-host-stub-fatbin-registration.md) 只讲「惰性加载」，未讲「卸载 / 释放」。
- [ ] **B7 `--emit` / `-keep` 的产物集与命名**（对应可观测性目标 G5）。
- [ ] **B8 主机编译器 / 链接器选择**：默认 `clang++` 还是 `g++`、可否配置（方案 §8 图写作 “clang++/g++”）。

## C. 工程 / 流程

- [ ] **C1 LLVM 版本获取与锁定**：EDG 走 `FetchContent` + 固定 commit（[ADR 0005](adr/0005-licensing-and-edg-distribution.md)）已定；
      **LLVM** 用系统包还是 `FetchContent`、如何锁版本未定。
- [x] **C2 命名 / 路径一致性**：**已定——统一为 `include/coolcc/`**；文档与骨架已同步（运行时头 `include/coolcc/hacc_runtime.h`）。
- [ ] **C3 一期是否支持多 `.cu`（多 TU WPC）**：「一步到可执行」默认行为下，多个 `.cu` 各自 host 桩 + 注册
      是否属一期？[ADR 0004](adr/0004-cuda-cpp-subset-scope.md) 仅排除 RDC / 跨 TU 设备函数链接，**未点名 WPC 多 TU**。
- [ ] **C4 golden / 对照策略**：一期是否即引入「与 nvcc 运行结果对照」（方案 §11 标为「可选」）；
      以及**无 GPU / 无 PTX JIT 时的跳过机制**（CTest label / feature）。
- [ ] **C5 hacc 的 CI 形态**：EDG ~853MB 拉取 + 是否有 GPU；coolruntime 的 CI 因无 GPU 仅做格式检查，
      hacc 是否照此。
- [ ] **C6 面向教学的诊断格式**：阶段 / 源位置 / 成因的**具体输出格式**（Coding Rules 仅有原则）。

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
