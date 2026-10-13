# EDG IL 表项速查（M1.2 实施参考）

> 依据：EDG 源码 `src/il_def.h`（commit `43e10b90`，19491 行）。
> 用途：M1.2「EDG IL → LLVM IR」翻译器实现时的**字段级参考**。
> 上游：[m1.2-il-to-llvm-plan.md](m1.2-il-to-llvm-plan.md)。
> 说明：本文只覆盖 **M1.2 最小切片**（无参/标量参内核 + 内建变量 + `__syncthreads`）所需表项；
> 完整定义以 `il_def.h` 为准。

## 0. 命名约定

| 前缀 | 含义 |
| --- | --- |
| `a_*` | IL 表项结构体（如 `a_routine`、`a_statement`） |
| `an_*` | 另一类表项（如 `an_expr_node`、`an_attribute`） |
| `iek_*` | entry kind（`an_il_entry_kind`，表项类型标签） |
| `enk_*` | expr node kind（`an_expr_node_kind`） |
| `eok_*` | expr operator kind（`an_expr_operator_kind`） |
| `stmk_*` | statement kind（`a_statement_kind`） |
| `tk_*` | type kind（`a_type_kind`） |
| `ck_*` | constant kind（`a_constant_kind`） |
| `sc_*` | storage class（`a_storage_class`） |
| `nlk_*` | name linkage kind（`a_name_linkage_kind`） |

## 1. 表项总览（`an_il_entry_kind`，il_def.h:604）

M1.2 关心的核心表项：

| `iek_*` | 结构体 | 用途 |
| --- | --- | --- |
| `iek_source_file` | `a_source_file` | 编译单元根 |
| `iek_routine` | `a_routine` | **函数/内核** |
| `iek_variable` | `a_variable` | 变量（含内建 `threadIdx` 等） |
| `iek_type` | `a_type` | 类型 |
| `iek_constant` | `a_constant` | 常量 |
| `iek_expr_node` | `an_expr_node` | 表达式节点 |
| `iek_statement` | `a_statement` | 语句 |
| `iek_block` | `a_block` | 块（语句序列） |
| `iek_scope` | `a_scope` | 作用域（声明列表） |
| `iek_label` | `a_label` | 标签 |
| `iek_attribute` | `an_attribute` | **属性**（A2 落点） |
| `iek_attribute_arg` | `an_attribute_arg` | 属性参数 |
| `iek_attribute_group` | `an_attribute_group` | 属性组 |
| `iek_id_name` | （字符串） | 标识符名 |
| `iek_string_text` | （字符串） | 字符串字面量文本 |
| `iek_other_text` | （字符串） | 文件名等文本 |

> 字符串类表项用 `is_string_entry_kind()` 判定（il_def.h:820）。

## 2. 核心结构体字段

### 2.1 `a_routine`（il_def.h:12228）—— 函数/内核

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `source_corresp` | `a_source_correspondence` | **必须第一个**；含 `name`、`attributes` |
| `next` | `a_routine_ptr` | 同作用域下一个 routine |
| `type` | `a_type_ptr` | 函数类型（`tk_routine`）：返回类型 + 参数 |
| `function_def_number` | `a_function_def_number` | 非空表示**有函数体**；用于定位函数体 scope |
| `memory_region` | `a_memory_region_number` | 函数体所在 memory region |
| `storage_class` | `a_storage_class` | 有效存储类（`sc_extern`/`sc_static`/…） |
| `special_kind` | `a_special_function_kind` | 特殊成员函数种类（`sfk_none` 为普通函数） |
| `defined` | bit | 函数体是否已完整扫描 |
| `has_been_defined` | bit | 是否曾定义 |
| `is_inline` | bit | 是否 inline 候选 |

> **内核识别**：`a_routine` 的 `source_corresp.attributes` 链中是否有 `name == "cool_global"`。
> **函数体**：经 `function_def_number` → `function_def_table` → 对应 `a_scope`（见 §2.6）。

### 2.2 `a_statement`（il_def.h:16516）—— 语句

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `position` | `a_source_position` | 源位置 |
| `next` | `a_statement_ptr` | 同序列下一条语句 |
| `parent` | `a_statement_ptr` | 所属结构化语句（顶层 block 为 NULL） |
| `attributes` | `an_attribute_ptr` | 语句上的属性 |
| `kind` | `a_statement_kind` | 语句种类（`stmk_*`） |
| `expr` | `an_expr_node_ptr` | 主表达式（按 kind 解释，见下） |
| `union` | 变体 | 按 kind 取不同字段（如 `if` 的 then/else） |

**`expr` 字段按 kind 的含义**（il_def.h:16516 注释）：

| kind | `expr` 含义 |
| --- | --- |
| `stmk_expr` | 待求值表达式 |
| `stmk_return` | 返回值（或 NULL） |
| `stmk_if` / `stmk_constexpr_if` | 条件表达式 |
| `stmk_while` / `stmk_end_test_while` | 条件表达式 |
| `stmk_for` | 条件表达式（或 NULL） |
| `stmk_switch` | switch 表达式 |

> 条件表达式**已标准化为整型/布尔**。

### 2.3 `a_statement_kind`（il_def.h:15802）—— 语句种类

M1.2 需要的：

| `stmk_*` | 含义 |
| --- | --- |
| `stmk_expr` | 表达式语句 |
| `stmk_if` | if-then-else |
| `stmk_while` | while（顶测） |
| `stmk_end_test_while` | do-while（底测） |
| `stmk_for` | for |
| `stmk_return` | return |
| `stmk_block` | 语句块（含声明） |
| `stmk_decl` | 声明语句 |
| `stmk_goto` / `stmk_label` | goto / 标签 |
| `stmk_switch` / `stmk_switch_case` | switch / case |
| `stmk_empty` | 空语句 |
| `stmk_init` | 动态初始化 |

### 2.4 `an_expr_node`（il_def.h:14906）—— 表达式节点

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `type` | `a_type_ptr` | 表达式类型 |
| `orig_lvalue_type` | `a_type_ptr` | glvalue→prvalue 转换前的类型 |
| `next` | `an_expr_node_ptr` | 操作数链表 |
| `kind` | `an_expr_node_kind` | 节点种类（`enk_*`） |
| `is_lvalue` / `is_xvalue` | bit | 值类别 |
| `result_is_not_used` | bit | 结果是否被丢弃 |
| `union` | 变体 | 按 kind 取字段 |

**`an_expr_node_kind`（il_def.h:13570）**：

| `enk_*` | 含义 |
| --- | --- |
| `enk_operation` | 运算符 + n 个操作数（见 `eok_*`） |
| `enk_constant` | 常量值 |
| `enk_variable` | 变量引用 |
| `enk_field` | 字段（用于 `eok_dot_field` 等） |
| `enk_temp_init` | 临时量初始化 |
| `enk_sizeof` | sizeof |
| `enk_error` | 错误 |

**`an_expr_operator_kind`（il_def.h:13855）**：`eok_add`/`eok_subtract`/`eok_call`/`eok_cast`/`eok_assign`/`eok_dot`/`eok_subscript`/… （完整列表见 il_def.h）。

### 2.5 `a_type`（il_def.h:9768）与 `a_type_kind`（il_def.h:5573）

**`a_type_kind`**：

| `tk_*` | 含义 | LLVM 映射（M1.2） |
| --- | --- | --- |
| `tk_void` | void | `void` |
| `tk_integer` | 整型（含 enum） | `iN` |
| `tk_float` | 浮点 | `float`/`double` |
| `tk_pointer` | 指针（C++ 引用同） | `ptr` |
| `tk_routine` | 函数 | `FunctionType` |
| `tk_array` | 数组 | `[N x T]` |
| `tk_struct`/`tk_class`/`tk_union` | 聚合 | `StructType` |
| `tk_typeref` | typedef / 限定符 | 解引用到目标类型 |

> 取类型对齐须用 `alignment_of_type` 宏（il_def.h 脚注 f3）。

### 2.6 `a_scope`（il_def.h:18295）—— 作用域/声明列表

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `parent` | `a_scope_ptr` | 父作用域（file scope 为 NULL） |
| `kind` | `a_scope_kind` | `sck_file`/`sck_func_prototype`/`sck_block`/… |
| `routines` | `a_routine_ptr` | **本作用域的 routine 列表** |
| `types` | `a_type_ptr` | 本作用域类型列表 |
| `variables` | `a_variable_ptr` | 变量列表（static/thread 存储期） |
| `constants` | `a_constant_ptr` | 常量列表 |
| `namespaces` | `a_namespace_ptr` | 命名空间列表 |

> **遍历入口**：file scope 的 `routines` 列表 → 筛 `cool_global` → 经 `function_def_number` 找函数体 scope。

### 2.7 `a_variable`（il_def.h:11210）—— 变量

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `source_corresp` | `a_source_correspondence` | **必须第一个**；含 `name`、`attributes` |
| `type` | `a_type_ptr` | 变量类型 |
| `assoc_param_type` | `a_param_type_ptr` | 关联的参数类型（若适用） |
| `storage_class` | `a_storage_class` | 有效存储类 |
| `declared_storage_class` | `a_storage_class` | 源码中显式存储类 |

> **内建变量识别**：`source_corresp.name` 为 `threadIdx`/`blockIdx`/`blockDim`/`gridDim`。

### 2.8 `a_source_correspondence`（il_def.h:3261）—— 源对应

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `name` | `a_const_char*` | **实体名**（NULL 表示无对应源实体） |
| `unmangled_name_or_mangled_encoding` | `a_const_char*` | 未修饰名 / 修饰编码 |
| `attributes` | `an_attribute_ptr` | **该实体的属性集**（il_def.h:3609） |
| `position` | `a_source_position` | 源位置 |
| `scope` | `a_scope_ptr` | 声明所在作用域 |

> **属性消费入口**：`entity->source_corresp.attributes` → 遍历 `an_attribute` 链。

### 2.9 `an_attribute`（il_def.h:3157）—— 属性（A2 落点）

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `next` | `an_attribute_ptr` | 属性链表 |
| `kind` | `an_attribute_kind` | 属性种类（未知属性为 `ak_unrecognized`） |
| `family` | `an_attribute_family` | 语法族（`af_gnu`/`af_std`/…） |
| `syntactic_location` | `an_attribute_location` | 语法位置（prefix/postfix） |
| `name` | `a_const_char*` | **属性名**（如 `"cool_global"`） |
| `namespace_name` | `a_const_char*` | 命名空间名（`[[XYZ::fast]]`） |
| `arguments` | `an_attribute_arg_ptr` | 参数列表（NULL 表示无参） |

> **M1.2 消费**：按 `name` 字符串匹配（A2 决策 (a)）；`kind` 为 `ak_unrecognized`。
> 升级到 (b) 后改按稳定 `kind` 匹配。

## 3. 关键枚举

### 3.1 `a_storage_class`（il_def.h:408）

| `sc_*` | 含义 |
| --- | --- |
| `sc_unspecified` | 无显式存储类（隐含外部定义） |
| `sc_extern` | extern |
| `sc_static` | static |
| `sc_auto` | 局部（含参数） |
| `sc_register` | register |

### 3.2 `a_name_linkage_kind`（il_def.h:511）

| `nlk_*` | 含义 |
| --- | --- |
| `nlk_none` | 无链接（局部变量） |
| `nlk_internal` | 内部链接（file-scope static） |
| `nlk_cplusplus_external` | C++ 外部链接（**需 name mangling**） |
| `nlk_external` | 外部链接（`extern "C"`，**不 mangling**） |

> **内核名规则（B2）**：`nlk_external` → 原名；`nlk_cplusplus_external` → `_hacc_k_` + Itanium 修饰名。

## 4. 对 M1.2 翻译器的直接含义

1. **入口**：`a_source_file` → file scope `a_scope` → `routines` 列表。
2. **筛内核**：`routine->source_corresp.attributes` 含 `name == "cool_global"`。
3. **建 Function**：`routine->type`（`tk_routine`）→ 返回类型 + 参数；调用约定 `ptx_kernel`。
4. **函数体**：`routine->function_def_number` → 函数体 `a_scope` → 顶层 `stmk_block` → 语句链。
5. **语句**：按 `a_statement.kind` 分派；`expr` 字段按 kind 解释。
6. **表达式**：`an_expr_node.kind`（`enk_*`）分派；`enk_operation` 再看 `eok_*`。
7. **内建变量**：`a_variable.source_corresp.name` 匹配 `threadIdx` 等 → NVVM intrinsic。
8. **`__syncthreads`**：`eok_call` 的被调 routine 名匹配 → `llvm.nvvm.barrier0`。
9. **属性**：`source_corresp.attributes` → `an_attribute.name` 匹配（A2 (a)）。

## 5. 实测验证（2026-10，cpfe 7.0）

用 `fetch_edg.sh` 构建的 `cpfe`（commit `43e10b90`）对探针导出 IL，**逐项核对**本文档：

```sh
CPFE=build/il_spike/edg/build/gcc/bin/cpfe
BASE=build/il_spike/edg/bases/docker/dev-env/gcc
"$CPFE" --edg_base_dir "$BASE" --g++ -N --il_display tools/il_spike/inputs/kernel_probe.cpp > /tmp/kernel_probe.il
```

### 5.1 内核 routine（`kernel_probe.cpp` 的 `saxpy`）

```text
file-scope routine@53100013ab00
source_corresp:
  name:                  file-scope id-name@53100013a180: "saxpy"
  name_linkage:          C
  attributes:            NULL
type:                    file-scope type@53100013a1d0: void (int, float, const float *, float *)
function_def_number:     1
memory_region:           2
storage_class:           sc_unspecified
defined:                 TRUE
is_top_level_in_mem_region: TRUE
```

**核对结论**：
- `source_corresp.name` = `"saxpy"` ✅（§2.8）
- `name_linkage: C` = `nlk_external`（`extern "C"`，不 mangling）✅（§3.2）
- `type` = `void (int, float, const float *, float *)` ✅（§2.1）
- `function_def_number: 1` + `memory_region: 2` → 函数体在 region 2 ✅（§2.1）
- `defined: TRUE` ✅

### 5.2 函数体（memory region 2）

```text
Intermediate language for memory region 2 (function "saxpy"):

func-scope variable@5310001549c0
source_corresp:
  name:                  func-scope id-name@53100013a1a8: "n"
  enclosing_routine:     file-scope routine@53100013ab00: saxpy
  is_local_to_function:  TRUE
type:                    file-scope type@53100002f840: int
assoc_param_type:        file-scope param-type@53100013a300
storage_class:           sc_auto
is_parameter:            TRUE
```

**核对结论**：参数 `n` 是 `func-scope variable`，`storage_class: sc_auto`、`is_parameter: TRUE` ✅（§2.7）。

### 5.3 语句与表达式

```text
func-scope statement@531000156050
next:                    func-scope statement@5310001566d8
parent:                  func-scope statement@531000155f58
kind:                    stmk_expr
expr:                    func-scope expr-node@531000156670

func-scope expr-node@531000156800
type:                    file-scope type@531000036bc8: void
result_is_not_used:      TRUE
kind:                    enk_operation
operation.kind:          eok_call
operands:                func-scope expr-node@531000156798
```

**核对结论**：
- 语句 `kind: stmk_expr` + `expr:` 指针 ✅（§2.2）
- 表达式 `kind: enk_operation` + `operation.kind: eok_call` ✅（§2.4）
- `__cool_syncthreads()` 调用表现为 `eok_call`，被调 routine 名 `__cool_syncthreads` ✅（§4.8）

### 5.4 属性（`kernel_probe_attr2.cpp`）

```text
file-scope attribute@53100013a630
kind:                    unrecognized
family:                  gnu
name:                    file-scope other-text@53100013a600: "cool_global"
...
  attributes:            file-scope attribute@53100013a630
```

**核对结论**：
- `cool_global` 属性实体：`kind: unrecognized`、`family: gnu`、`name: "cool_global"` ✅（§2.9）
- 经 routine 的 `source_corresp.attributes:` 挂载 ✅（§2.8）
- `verify_attrs.sh` 全 7 项通过（`cool_shared`/`cool_global`/`cool_device`/`cool_host`/`cool_constant`/`address_space`/`kind: unrecognized`）✅

> **结论**：本文档的字段级描述与实测 IL **完全一致**；M1.2 翻译器可按 §4 的 9 条指引落地。

## 5.5 M1.2.3 实测发现（标量参数 + 简单语句）

用 `tools/il_spike/inputs/m123_probe.cu`（`add_kernel(int* out, int a, int b)`，含
`int c = a + b;` 与 `if (c > 0) {...} else {...}`）实测，补充以下**字段级细节**
（文档 §2 未覆盖，实现时踩过的坑）：

### 5.5.1 union 变体名（易错）

`a_statement` / `an_expr_node` / `a_type` / `a_scope` 的 union **都有名字 `variant`**，
但内部变体名与「kind 名」不同：

| 类型 | kind | 访问路径 |
| --- | --- | --- |
| `a_statement` | `stmk_block` | `stmt->variant.block.statements` |
| `a_statement` | `stmk_decl` | `stmt->variant.decl.entities`（`an_il_entity_list_entry_ptr`） |
| `a_statement` | `stmk_init` | `stmt->variant.dynamic_init`（`a_dynamic_init_ptr`） |
| `a_statement` | `stmk_if` | `stmt->variant.if_stmt.then_statement` / `.else_statement` |
| `an_expr_node` | `enk_variable` | `expr->variant.variable.ptr`（**不是** `.variable`） |
| `an_expr_node` | `enk_constant` | `expr->variant.constant.ptr`（**不是** `.constant`） |
| `an_expr_node` | `enk_operation` | `expr->variant.operation.kind` / `.operands` |
| `a_type` | `tk_integer` | `t->variant.integer.int_kind` |
| `a_type` | `tk_float` | `t->variant.float_kind` |
| `a_type` | `tk_pointer` | `t->variant.pointer.type` |
| `a_type` | `tk_routine` | `t->variant.routine.return_type` / `.extra_info` |
| `a_type` | `tk_typeref` | `t->variant.typeref.type` |
| `a_scope` | `sck_function` | `scope->variant.routine.parameters`（**不是** `.function`） |

> `a_scope.assoc_block` / `a_scope.nonstatic_variables` 是**顶层字段**（不在 union 内）。

### 5.5.2 函数体 scope 必须用 `scope_for_routine`

`il_read` 只加载 **file scope region**；函数体在另一 memory region，**未加载**。
直接读 `il_header.function_def_table[n].scope` 或 `region_scope_entry[mr]` 会得到
`NULL`。必须调用 EDG 的 **`scope_for_routine(routine)`**（il.h:3992），它内部按需
`read_memory_region` 加载函数体 region。

**关键约束**：`read_memory_region` 依赖全局 `f_il_input` 句柄，因此 `il_read` 后
**不能关闭 IL 文件**，否则报 `Catastrophic error: invalid intermediate language file`。
文件句柄应在翻译全部结束后再 `fclose`。

### 5.5.3 常量值不是裸整型

- `a_constant.variant.integer_value` 是 **`an_integer_value` 结构体**（内部多字表示，
  `an_int_value_part part[...]`），**不能** `static_cast` 到整型。用
  `conv_integer_value_to_host_large_integer(&value, is_signed, &host_val, &err)`
  （const_ints.h:128）取宿主整数值。
- `a_constant.variant.float_value` 是 `an_internal_float_value`（字节数组）。用
  `fetch_host_fp_value(kind, &value)`（float_pt.h:102）取宿主浮点值。

### 5.5.4 `int c = a + b;` 的语句形态

局部变量带初始化会拆成**两条语句**：
1. `stmk_decl`（声明 `c`，`entities` 链）
2. `stmk_init`（`dynamic_init` → `a_dynamic_init`，`kind == dik_expression`，
   表达式在 `variant.expression`）

`stmk_init` 的 `expr` 字段为 `NULL`，初始化表达式在 `dynamic_init` 里。

### 5.5.5 条件标准化

`if (c > 0)` 在 IL 中条件表达式为 `eok_gt`，但 NVPTX 后端可能产出 `setp.lt.s32 %p1, %r1, 1`
（即 `c < 1` 的取反）——这是 EDG 的**条件标准化**（§2.2 注：条件已标准化为整型/布尔），
语义等价，翻译器无需特殊处理。


## 6. 待补充（后续阶段）

- `a_constant`（il_def.h:4741）与 `a_constant_kind`（`ck_*`）—— 常量折叠。
- `a_param_type`（il_def.h:6385）—— 参数细节。
- `a_field`（il_def.h:11898）—— 结构体字段（M3）。
- `a_block`（il_def.h:15864）—— 块结构。
- `an_attribute_kind`（il_def.h:2948）—— 属性种类全表。
- `a_scope_kind` —— 作用域种类全表。

> 这些在 M1.2 实施中按需展开；本文随实现推进补充。
