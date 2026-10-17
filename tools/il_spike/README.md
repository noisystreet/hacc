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
tools/il_spike/fetch_edg.sh            # 稀疏检出 + 构建 cpfe（已带 RECORD_UNRECOGNIZED_ATTRIBUTES=1）
tools/il_spike/run.sh inputs/kernel_probe.cpp   # 导出 IL
tools/il_spike/verify_attrs.sh         # 验证未知属性是否随 IL 传递（A2 决策 (a)）
```

## 构建要点（`fetch_edg.sh`）

- git **稀疏检出**（`--cone`）只取构建所需目录，避开庞大的 `tests/`、`benchmarks/`：
  `src cmake util nt_util bases include_c++ include_c99 lib_src dev_tools`。
  —— 注意 `dev_tools/bin/edg-cmakedef` 必须存在，否则 CMake 报
  「Failed to process cmakedef」。
- CMake：`-G Ninja -DCMAKE_BUILD_TYPE=Debug -DEDG_MACRO_CONF=linux-gcc -DEDG_BUILD_LIB=OFF`
  （`EDG_BUILD_LIB=OFF` 跳过需要 `EDG_BASE` 的运行时库）。
- **A2 决策 (a)**：额外加 `-DCMAKE_CXX_FLAGS=-DRECORD_UNRECOGNIZED_ATTRIBUTES=1`，
  让 EDG 把未知/自定义属性记录进 IL。**必须用 `CMAKE_CXX_FLAGS`**——EDG 把 `.c` 源当
  C++ 编译（`set_source_files_properties(... LANGUAGE CXX)`），`CMAKE_C_FLAGS` 不生效。
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

## (a)/(b) 取舍：通用性对比

**结论：论「通用性」，(b) 正式注册属性族更强；(a) 只是更省事。**

| 维度 | (a) `RECORD_UNRECOGNIZED_ATTRIBUTES` | (b) 注册属性族 |
| --- | --- | --- |
| 属性名 | 任意名字都能进 IL，但 `kind` 恒为 `unrecognized` | 每个属性有**稳定 `kind`**（如 `ak_shared`） |
| 语义 | EDG 不校验、不生效，纯「原样搬运」 | EDG 原生识别/校验/生效（适用约束、apply 函数） |
| 下游消费 | 消费方只能靠 `name` 字符串匹配 | 可按 `kind` 枚举匹配，**不依赖拼写** |
| 跨 TU 一致性 | 无规则 | 可经 `attr_corresp_table[]` 定义匹配规则 |
| 可扩展性 | 加新属性零成本，但都堆在 `unrecognized` 里 | 加新属性要改表，但语义清晰、可演进 |
| 维护成本 | 零补丁 | 需在 ADR 0005 固定 commit 上维护补丁 |

- (a) 的「通用」是「什么都能塞进去」，但塞进去的东西**没有类型**：下游（`coolicc`）只能按字符串认属性，
  拼写一变就断，也无法让 EDG 做适用性检查。
- (b) 是「有类型的通用」：属性成为 IL 的一等公民，语义稳定、可校验、可跨 TU 比对。

**但「更通用」不等于「现在就该选」：**

- 目标是**尽快跑通阶段 A、验证端到端** → (a) 更合适：零补丁、立刻可用，且**不锁死未来**
  （将来转 (b) 时，IL 里已有的属性名可平滑迁移）。
- 目标是**把 CUDA 属性作为长期稳定契约**（多后端、跨 TU、要 EDG 帮查错）→ (b) 更值。
- 两者**不互斥**：常见做法是先用 (a) 打通，等属性集合稳定后再补 (b) 的注册表。

## (a)/(b) 取舍：通用性对比

**结论：论「通用性」，(b) 正式注册属性族更强；(a) 只是更省事。**

| 维度 | (a) `RECORD_UNRECOGNIZED_ATTRIBUTES` | (b) 注册属性族 |
| --- | --- | --- |
| 属性名 | 任意名字都能进 IL，但 `kind` 恒为 `unrecognized` | 每个属性有**稳定 `kind`**（如 `ak_shared`） |
| 语义 | EDG 不校验、不生效，纯「原样搬运」 | EDG 原生识别/校验/生效（适用约束、apply 函数） |
| 下游消费 | 消费方只能靠 `name` 字符串匹配 | 可按 `kind` 枚举匹配，**不依赖拼写** |
| 跨 TU 一致性 | 无规则 | 可经 `attr_corresp_table[]` 定义匹配规则 |
| 可扩展性 | 加新属性零成本，但都堆在 `unrecognized` 里 | 加新属性要改表，但语义清晰、可演进 |
| 维护成本 | 零补丁 | 需在 ADR 0005 固定 commit 上维护补丁 |

- (a) 的「通用」是「什么都能塞进去」，但塞进去的东西**没有类型**：下游（`coolicc`）只能按字符串认属性，
  拼写一变就断，也无法让 EDG 做适用性检查。
- (b) 是「有类型的通用」：属性成为 IL 的一等公民，语义稳定、可校验、可跨 TU 比对。

**但「更通用」不等于「现在就该选」：**

- 目标是**尽快跑通阶段 A、验证端到端** → (a) 更合适：零补丁、立刻可用，且**不锁死未来**
  （将来转 (b) 时，IL 里已有的属性名可平滑迁移）。
- 目标是**把 CUDA 属性作为长期稳定契约**（多后端、跨 TU、要 EDG 帮查错）→ (b) 更值。
- 两者**不互斥**：常见做法是先用 (a) 打通，等属性集合稳定后再补 (b) 的注册表。

## 决策：选 (a)（已定）

**一期采用路线 (a)：EDG 构建期开启 `RECORD_UNRECOGNIZED_ATTRIBUTES`，零 EDG 源码补丁。**

- 理由：一期目标是**跑通阶段 A（IL 文件）端到端**，成本最低、可回退；且不锁死未来
  （属性名可平滑迁移到 (b)）。
- 配套：把「CUDA 属性清单」固定成一份文档/枚举（见下），供 `coolfe++` 前置改写与 `coolicc` 消费对齐。
- 升级路径：属性清单稳定后，再按 `attribute.c` 的 6 步 recipe 升级到 (b)（届时另开 ADR/方案修订）。

**落地（已实现）：**

1. `fetch_edg.sh` 已注入 `-DCMAKE_CXX_FLAGS=-DRECORD_UNRECOGNIZED_ATTRIBUTES=1`。
   注意 EDG 把 `.c` 源当 **C++** 编译（`set_source_files_properties(... LANGUAGE CXX)`），
   故须用 **`CMAKE_CXX_FLAGS`**（`CMAKE_C_FLAGS` 不生效）。
2. 该变量是**全局量**：阶段 B（链接 EDG 前端库）可在运行期置真；阶段 A（调 `edgcpfe`）需构建期定义。
3. 属性以 `__attribute__((…))` 承载（或由 `coolfe++` 前置改写生成），IL 中表现为
   `file-scope attribute@…`（`kind: unrecognized`、带 `name`/`arguments`），并挂到实体
   `source_corresp.attributes:`。
4. 验证脚本 `verify_attrs.sh`：对 `inputs/kernel_probe_attr2.cpp` 断言清单中的属性名
   出现在 IL 的 `name:` 字段（严格匹配，避免注释/变量名假阳性），且存在 `kind: unrecognized` 条目；
   前置缺失（cpfe/EDG base/输入）或 cpfe 失败时给出清晰报错并返回 2。
   **实测通过（exit 0）**：

   ```text
   OK   attribute in IL: cool_shared
   OK   attribute in IL: cool_global
   OK   attribute in IL: cool_device
   OK   attribute in IL: cool_host
   OK   attribute in IL: cool_constant
   OK   attribute in IL: address_space
   OK   IL contains 'kind: unrecognized' entries
   ```

**CUDA 属性清单：** 见 [cuda-attributes.md](cuda-attributes.md)（`coolfe++` ↔ `coolicc` 的契约，
含执行空间 / 存储空间 / 地址空间 / 启动提示 / 非属性项，及与 `verify_attrs.sh` 的对应）。

> `verify_attrs.sh` 的 `EXPECTED` 与探针 `inputs/kernel_probe_attr2.cpp` 覆盖清单 §1–§3 的核心项
> （`cool_address_space` 暂以 `address_space` 探针代替；`__syncthreads` 非属性，未纳入断言）。

## 待办（下一步）

- **（已定）(a)/(b) 取舍 = (a)**；方案 §5.2 / [ADR 0001](../../docs/adr/0001-edg-cpp-frontend.md) 已回写。
- **（已实现）(a)**：`fetch_edg.sh` 注入 `RECORD_UNRECOGNIZED_ATTRIBUTES=1`（`CMAKE_CXX_FLAGS`），
  `verify_attrs.sh` 实测通过。
- **（已细化）CUDA 属性清单**：见 [cuda-attributes.md](cuda-attributes.md)（`coolfe++` ↔ `coolicc` 契约）；
  **S1/M4.0 已补 `cool_restrict`**（探针 + `EXPECTED` 已覆盖，实测通过）；
  后续按 S2 补 `cool_launch_bounds`/`cool_forceinline`/`cool_noinline`。
- **（已查清）二进制 IL 往返**：`cdisp` 取**位置参数**（`cdisp <il-file>`；依据源码
  `il_display.c:8920` `file_name = argv[optind]`），二进制 IL 由 `cpfe ... -o <file>` 产出
  （文件头 `EDG IL file (v7.0)`）。
  - 无属性源码：`cdisp` 正常（exit 0）。
  - **含任意 `__attribute__` 的源码：`cdisp` 断言崩溃**（`Internal error: assertion failed at
    "il_display.c", line 210 in disp_ptr_value`，SIGABRT/exit 134）——**与 `RECORD_UNRECOGNIZED_ATTRIBUTES`
    无关**（EDG 已知属性如 `aligned`/`unused` 同样触发），属 **EDG `cdisp` 显示路径的 bug**。
  - 结论：**IL 本身正确**（`--il_display` 与 `verify_attrs.sh` 均能读到属性）；仅 `cdisp` 的
    `disp_ptr_value` 在「条目指针落在数组基址之前」时断言失败（据 `il_display.c:210` 的
    `check_assertion(offset >= 0)` 推断）。**当前用 `--il_display` 重定向落盘已满足检索需求**；
    `cdisp` 崩溃可上报 EDG 上游。

  **最小复现（可直接复制）**：

  ```sh
  CPFE=build/il_spike/edg/build/gcc/bin/cpfe
  CDISP=build/il_spike/edg/build/gcc/bin/cdisp
  BASE=build/il_spike/edg/bases/docker/dev-env/gcc
  printf 'int __attribute__((unused)) u;\n' > /tmp/t.cpp
  "$CPFE" --edg_base_dir "$BASE" --g++ -N -o /tmp/t.cil /tmp/t.cpp   # 产出二进制 IL
  "$CDISP" /tmp/t.cil                                                # 断言崩溃（exit 134）
  ```

- **（已查清）cpfe `--il_display` 与 `-o` 互斥**：`cpfe` 的 `--il_display`（文本 dump）与 `-o <真实文件>`
  （二进制 IL）**不能在同一次调用中同时使用**，否则 `-o` 写出的二进制 IL 会变成「显示变体」，
  `il_read` 读回时报 `Catastrophic error: invalid intermediate language file`（exit 4）。
  另有第二坑：`--il_display` **不带 `-o`** 时，`cpfe` 会把默认名 `<input-stem>.cil` 的显示变体 IL
  写到**当前目录**，可能覆盖真正的二进制 IL（尤其当二进制输出也在当前目录时）。
  - 现象：`coolfe++` 同时要二进制 + 文本时，产出的 `.cil` 只有 ~3 KB（显示变体）而非 ~22 KB（正常二进制），
    且 `coolicc --emit=il-list` 读回失败。**M1.2.0 的 golden 测试只 grep 文本 IL，未校验二进制 IL 可读，故被掩盖。**
  - 结论：**二进制 IL 与文本 IL 必须分两次 `cpfe` 调用**；文本调用必须带 `-o <临时文件>` 抑制默认写
    （`cpfe` 拒绝 `/dev/null`，报 `cannot open IL output file "/dev/null": not a regular file`），用完删除。
  - 修复落点：`src/frontend/edg_invoke.cpp` 的 `build_cpfe_args` / `invoke_cpfe`（两趟调用）。

  **最小复现（可直接复制）**：

  ```sh
  CPFE=build/il_spike/edg/build/gcc/bin/cpfe
  BASE=build/il_spike/edg/bases/docker/dev-env/gcc
  printf 'int __attribute__((unused)) u;\n' > /tmp/t.cpp
  # 坑 1：--il_display 与 -o 同用 → -o 写出显示变体（~3KB），il_read 读回失败
  "$CPFE" --edg_base_dir "$BASE" --g++ -N --il_display -o /tmp/t.cil /tmp/t.cpp
  ls -l /tmp/t.cil        # 明显偏小
  # 坑 2：--il_display 不带 -o → 默认名 t.cil 写到当前目录
  cd /tmp && "$CPFE" --edg_base_dir "$BASE" --g++ -N --il_display t.cpp >/dev/null
  ls -l /tmp/t.cil        # 被显示变体覆盖
  # 正确：分两次；文本趟用 -o <临时文件> 抑制默认写
  "$CPFE" --edg_base_dir "$BASE" --g++ -N -o /tmp/t.cil /tmp/t.cpp
  "$CPFE" --edg_base_dir "$BASE" --g++ -N --il_display -o /tmp/t.tmp.cil /tmp/t.cpp > /tmp/t.il.txt
  rm -f /tmp/t.tmp.cil
  ```
