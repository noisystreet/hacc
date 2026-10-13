# M0 · EDG IL spike

验证 **EDG IL 是否保留 hacc 需要的关键信息**（A2 判据）：
① 地址空间（global/shared/constant/local）② `__shared__`/`__constant__`
③ 同步点（`__syncthreads`）④ 内核 / 执行空间标记。
任一关键属性缺失即判「IL 表达力不足」，触发切阶段 B。
见 [../../docs/TODO.md](../../docs/TODO.md)（A2）与
[../../docs/compiler-implementation-plan.md](../../docs/compiler-implementation-plan.md) §5.2。

## EDG 获取（ADR 0005：按需拉取 + 固定 commit，不 vendor）

- 仓库：`https://github.com/edgcpp/compiler`
- 固定 commit：`43e10b90fc83ed2a436fb79b241be8dbf3dc6846`
- 许可：Apache-2.0 WITH LLVM-exception

```sh
tools/il_spike/fetch_edg.sh            # 稀疏检出 + 构建 cpfe
tools/il_spike/run.sh inputs/kernel_probe.cpp   # 导出 IL
```

## 构建要点（`fetch_edg.sh`）

- git **稀疏检出**（`--cone`）只取构建所需目录，避开庞大的 `tests/`、`benchmarks/`：
  `src cmake util nt_util bases include_c++ include_c99 lib_src dev_tools`。
  —— 注意 `dev_tools/bin/edg-cmakedef` 必须存在，否则 CMake 报
  「Failed to process cmakedef」。
- CMake：`-G Ninja -DCMAKE_BUILD_TYPE=Debug -DEDG_MACRO_CONF=linux-gcc -DEDG_BUILD_LIB=OFF`
  （`EDG_BUILD_LIB=OFF` 跳过需要 `EDG_BASE` 的运行时库）。
- 产物：`build/il_spike/edg/build/gcc/bin/{cpfe,cpfe-cp,cdisp,eccp,mk_errinfo,...}`。

## 导出 IL

```sh
cpfe --edg_base_dir <bases/docker/dev-env/gcc> --g++ -N --il_display <input.cpp>
```

- `--edg_base_dir`：指向含 `lib/predefined_macros.txt` 的目录；否则报
  「cannot open predefined macro file “lib/predefined_macros.txt”」。
- `--il_display`：人类可读显示 IL。
- `-N`（`--no_il_lowering`）：保留**未降级** IL（C++ 层，信息最全）。
- `--g++`：GNU C++ 扩展（`__attribute__` 等）。**注意** `--gcc` 是 **C** 模式；
  新版标准用 `--c++11` / `--c++14` 等。默认方言是 **C++98**。

## 已确认（本轮 spike）

- EDG 前端可构建；`cpfe --version` → `7.0`。
- IL 导出链路可用；IL 是**两级 memory region**（file scope + 每函数）的人类可读树，
  实体带 `attributes`、`source_corresp`、`name_linkage`、`storage_class` 等字段。
- 默认方言 C++98；`--g++` 得到 C++14（`std_version 201402`）。
- EDG 的**命名地址空间**（named address spaces）仅在其 test/demo 配置
  （`INCLUDE_EDG_TEST_NAMED_ADDRESS_SPACES`）启用，**标准构建不含**——
  hacc 不能直接依赖它表达 CUDA 地址空间。

## 属性核验结论（A2 · 本轮）

把探针 IL **落盘**（`--il_display` 重定向到文件）后全量检索，得到：

**1. EDG IL 具备一等的「属性」表示。**

- 有独立的属性实体 `file-scope attribute@…`，字段含 `kind`（如 `align`）、
  `family`（如 `gnu`）、`syntactic_location`（`prefix`）、`name`（文本）、
  `arguments`（`attribute-arg@…` → 常量）、`group`（`attribute-group@…`）与位置。
- 声明实体的 `source_corresp.attributes:` 字段**指向**该属性实体。例如
  `int __attribute__((aligned(64))) g_tile[64]` 得到
  `attributes: file-scope attribute@…`，并伴 `alignment: 64`、`has_associated_pragma: TRUE`
  （无属性的实体该字段为 `NULL`）。

**2. 但只保留 EDG「已知」的属性名；未知/自定义属性被丢弃。**

- `__attribute__((aligned(64)))` → 保留（`kind: align`、`family: gnu`）。
- `__attribute__((cool_shared))` / `((cool_global))` / `((address_space(3)))`
  （见 `inputs/kernel_probe_attr2.cpp`）→ EDG 报 `warning: unknown attribute "…"`，
  且这些名字在 IL 中**出现 0 次**。

**3. 把 CUDA 属性带进 IL 有两条路线（已在 EDG 源码中确认）——均属前端定制，但不必改 EDG 源码。**

- **(a) 构建期开关 `RECORD_UNRECOGNIZED_ATTRIBUTES`（零源码改动，已实测）。**
  `host_envir.h` 定义 `RECORD_UNRECOGNIZED_ATTRIBUTES`（默认 FALSE；CP-GEN 后端为 TRUE），
  它初始化全局 `record_unrecognized_attributes`；`attribute.c` 据此决定「把未知属性记录进 IL」
  还是「告警并丢弃」。
  实测：用 `-DCMAKE_CXX_FLAGS=-DRECORD_UNRECOGNIZED_ATTRIBUTES=1` 重建 cpfe 后，
  `cool_shared` / `cool_global` / `address_space(3)` 全部出现为 `file-scope attribute@…`
  （`kind: unrecognized`、`family: gnu`、带 `name` 与 `arguments`），并**经实体的
  `source_corresp.attributes:` 挂到声明上**，stderr 无告警。
  （该变量是全局量：阶段 B 链接 EDG 前端库时可在运行期置真。）
- **(b) 正式注册 CUDA 属性族（EDG 源码级小补丁）。**
  `src/attribute.c` 头部文档给出 6 步 recipe：
  ① `il_def.h` 加 `ak_*` kind → ② `known_attr_table[]` 加行（名字/签名/模式字）→
  ③ `known_attr_appl_table[]` 加行（适用约束 + apply 函数）→ ④ 写 apply 函数 →
  ⑤（可选）`attr_corresp_table[]` → ⑥ 更新 `il_display.c` 的 `disp_attribute` switch。
  收益：EDG 原生识别/校验/生效，IL 里是稳定的 `kind`（而非 `unrecognized`）。
  代价：需在 ADR 0005 的固定 commit 上维护一个小补丁（FetchContent 需支持打补丁）。

**对 A2 判据（方案 §5.2）的含义。**

- 判据要求 IL 保留四类关键属性：地址空间 / `__shared__`·`__constant__` / 同步点 / 内核标记。
- 纯「源级 `__attribute__` + 标准构建」确实丢失这些属性（见前节）。
- 但**路线 (a) 只需一个 EDG 构建开关，即可让未知属性（含名字与参数）随 IL 传递并挂到实体上**；
  以 `__attribute__((…))`（或 coolfe++ 前置改写）承载 CUDA 存储/执行限定符，在**阶段 A（IL 文件）**
  内即可成立。同步点（`__syncthreads`）本就是普通调用，在 IL 中表现为 routine 调用（带专门标记更佳）。
- 仍待拍板：`<<<>>>` 启动语法属源码改写（coolfe++ 扩展层，非属性问题）；以及 (a)/(b) 取舍
  （(a) 零补丁但 `kind` 为 `unrecognized`；(b) 需补丁但语义规范）。**A2 结论由维护者据 §5.2 拍板。**

## 待办（下一步）

- 定 (a)/(b) 取舍，并据此回写方案 §5.2 / [ADR 0001](../../docs/adr/0001-edg-cpp-frontend.md)。
- 若选 (a)：确认 `RECORD_UNRECOGNIZED_ATTRIBUTES` 在 hacc 的 EDG 构建里如何注入
  （CMake 变量，或在 FetchContent 后作为补丁）。
- 复核二进制 IL 往返：本构建下 `cdisp <file>` 对现有 `*.cil` 报
  `invalid intermediate language file`（疑似格式/参数不符），待查；当前用 `--il_display`
  重定向落盘已能满足检索需求。
