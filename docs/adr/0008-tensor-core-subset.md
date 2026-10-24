# ADR 0008: Tensor Core（MMA）子集范围

## Status

Accepted（已拍板，2026-10）

> 本条在 [ADR 0004](0004-cuda-cpp-subset-scope.md) 的子集之外**新增**一项能力，**不修订**
> ADR 0004 的任何既有条目 —— ADR 0004 的范围表里从未列入 MMA，因此在此之前是「用户写
> `wmma` 相关代码会撞『不支持的函数调用』并 fail loud」，而不是「曾被明确排除」。
>
> 代码已落地并通过真机数值验证（见文末「实施状态」）。

## Context

算子库的主路径是 GEMM。没有 Tensor Core 时只能用 `fma` 标量路径，单精度吞吐差一到两个
数量级；而 CUDA 生态里 `wmma::` 已是写 GEMM 的默认手段之一。

前置障碍有两个，都不是 hacc 的既有能力：

1. **fragment 类型**：`wmma::fragment<...>` 是 `nvcuda::wmma` 命名空间里的模板类型，
   带 `__frag_base` 内部布局。hacc 的 `map_type` 只覆盖标量/指针/POD 聚合，支持模板类
   等于把「模板类型布局」这条大河搬进来。
2. **16 位浮点**：`half` 在 EDG 里是 typedef 而非独立 kind；实测 `__fp16` 可用（IL 出现
   `fp16` kind）而 `_Float16` 在默认方言下不可用，但 hacc 的 `map_float_type` 两者都还没覆盖。

A0 spike 先把「intrinsic 到底长什么样」这件事测掉了（NVVM，LLVM 19）：

```llvm
{<2 x half> x 8} @llvm.nvvm.wmma.m16n16k16.load.a.row.f16(ptr)
{<2 x half> x 8} @llvm.nvvm.wmma.m16n16k16.load.b.col.f16(ptr)
{float x 8}      @llvm.nvvm.wmma.m16n16k16.load.c.row.f32(ptr)
{float x 8}      @llvm.nvvm.wmma.m16n16k16.mma.row.col.f32.f32(<2 x half> x 8, <2 x half> x 8, float x 8)
void             @llvm.nvvm.wmma.m16n16k16.store.d.row.f32(ptr, float x 8)
```

结论（实测，非推导）：fragment 是**8 个 32 位寄存器**、不是单个 `<16 x half>`；
`llc` **不校验** intrinsic 名（拼错照样出 PTX），只有 `ptxas` 会报。

## Decision

### 1. 支持且仅支持一种形状

`m16n16k16`，A/B 为 f16、C/D 为 f32；A 取 row-major、B 取 col-major、D 存 row-major。
即 15 个 wmma intrinsic 中只取 5 个（load.a / load.b / load.c / mma / store.d）。

**不支持的**（明确记录，不留模糊地带）：其它形状（`m8n32k16` / `m32n8k16` / `m16n8k16`）、
`bf16` / `f64` / `s8` / `u8` / `b1` 数据类型、`satfinite` 变体、`ldmatrix`、`cp.async`、
以及 `mma.sync.m16n8k16` 这一代更底层的指令。

### 2. 内建 ABI 用「出参」，不用值返回

```c
void cool_wmma_load_a_row(cool_wmma_frag_a* frag, const void* src);
void cool_wmma_load_b_col(cool_wmma_frag_b* frag, const void* src);
void cool_wmma_load_c_row(cool_wmma_frag_c* frag, const void* src);
void cool_wmma_mma(cool_wmma_frag_d* d, const cool_wmma_frag_a* a,
                   const cool_wmma_frag_b* b, const cool_wmma_frag_c* c);
void cool_wmma_store_d_row(void* dst, const cool_wmma_frag_d* d);
```

理由：值返回要求 hacc 生成「多返回值 struct intrinsic 的 SSA 管道」（`insertvalue` 组装），
而内建映射此前只支持「无参 + void」——两个缺口叠在一起，风险高于收益。出参只需
聚合 `call` + `store` + 常量下标 `extractvalue`，复用既有通路。

### 3. fragment 是 4 个不同的 32 字节 POD，不透出 `half`

`cool_wmma_frag_a/_b/_c/_d`：A/B 用 `unsigned int[8]`、C/D 用 `float[8]`，一律
**32 字节、对齐 4**。这样 MVP 不需要 `half` 类型，正确性由**矩阵数据布局**保证
（f16 数据由用户自己填进缓冲，与 fragment 的内部视图无关）。

- **为什么是 u32 而非 `unsigned short[16]`**：两者都是 32 字节，但后者对齐只有 2，而 hacc
  生成的 intrinsic 聚合值对齐是 4。hacc 目前不读类型的 `aligned` 属性（会丢弃），局部变量
  的对齐取自字段类型 —— align 2 的槽位存 align 4 的值属未定义行为，且只在真机数值上暴露。
- **为什么是 4 个不同类型名**：出参 ABI 下 hacc 不校验 fragment 类型，把「A/B/C/D 传反」
  的检查留在 C 编译器这一层。

### 4. intrinsic 签名写死，不从用户类型推导

hacc 眼中 fragment 是 `{[8 x i32]}` / `{[8 x float]}`，与 intrinsic 的 `{<2 x half> x 8}` /
`{float x 8}` **不同型**，只是 32 字节布局兼容：LLVM verifier 会拒掉类型不符的 intrinsic
声明，靠 opaque pointer 下 `store` / `load` 不校验指针元素类型才走得通。

> 因此这条通路**依赖 LLVM 全 opaque pointer（15+）**，旧版 LLVM 不适用。

### 5. 三条红线由用户遵守（hacc 不设防）

1. **warp 级语义**：32 个 lane 必须一致执行到调用点。包进 `if (tid < k)` 里不报错但结果错。
2. **32 字节对齐**：load/store 的缓冲必须 32B 对齐（PTX 用 `.aligned` 变体）。
   `coolMalloc` 实测 256B 对齐，缓冲直接用即可；不要传 `ptr + i`。
3. **顺序固定** `D = A×B + C`；A/B/C/D 传反不会报错。

三条已记入 [known-issues KI-3](../known-issues.md)，因为它们是「静默出错」，与 KI-1 同类。

## Consequences

Positive:

- GEMM 主路径可用；只取一个 intrinsic 家族，测试面与维护面都小。
- 出参 ABI 不需要 `insertvalue` 通道，也不必给 `map_float_type` 补 16 位浮点。
- 与既有内建（`__syncthreads` / 原子）并列新增，未改动已测映射与分派顺序。

Negative / trade-offs:

- **三条红线静默错**（KI-3）。数值断言是唯一防线，故 e2e 必须真跑并做反向验证。
- fragment 布局是「每线程 8 个 32 位寄存器」这一事实的硬编码表达式；换形状、换架构
  （如 Hopper 的 `wgmma`）都要重做，不是可扩展设计。
- 需要 **sm_70+**（wmma f16 的最低要求）。本机 sm_89 实测通过；`-arch` 低于 sm_70 时
  `ptxas` 会拒绝。
- 用户拿不到「值语义 fragment」（不能对 fragment 做元素级运算、不能手工缩放累加器）。
  将来需要时另立决策补值语义通道。

## Alternatives Considered

| Option | Pros | Cons |
| --- | --- | --- |
| 值返回 ABI（`cool_wmma_load_a_row` 返回 fragment） | 语义更贴近 `wmma::load_matrix_sync` | 需要 `insertvalue` 组装 SSA 管道 + 多返回值 struct 支持；两个缺口叠加 |
| 暴露 `half` 类型，fragment 用 `half[16]` | 用户可读、可做元素级运算 | 要先补 `map_float_type` 的 fp16 kind；且 `unsigned short[16]` 对齐 2 与 intrinsic 的 align 4 冲突 |
| 走 `mma.sync.m16n8k16` + `ldmatrix`（路线 B） | 更贴近主流 GEMM 写法，灵活度更高 | 要自己定义 fragment 布局并手写 `ldmatrix` 映射，工作量与风险大得多；本机无硬性需求 |
| 不支持 MMA（现状） | 零成本 | GEMM 只能走 fma 标量路径，吞吐差一到两个数量级 |
| **本决策：`wmma.m16n16k16` + 出参 ABI + u32/float 占位 fragment** | 一条通路吃下 load/mma/store；不需要 half；测试面小 | 形状固定、fragment 不可移植、三条红线静默错 |

## 实施状态

A1（coolrt 声明）/ A2（hacc 映射）/ A4（真机 e2e）均已落地，质量门 `make check`
**205/205**、零 SKIP：

- `coolruntime/include/cool/device_builtins.h`：4 个 fragment 类型 + 5 条内建声明；
  `coolruntime/tests/unit/test_wmma_frag.cpp` 锁住「32B / 对齐 4 / 四类型互异」。
- `src/device/il_to_llvm.cpp`：`try_translate_wmma_call` 等，映射到上列 5 个 intrinsic。
- 测试：`tests/golden/e2e/wmma.cu`（PTX 指纹 + `ptxas` 汇编）、
  `tests/golden/e2e/wmma_e2e.cu`（单 warp `D = A×B + C` 与 CPU 参考比对）。
- 数值验证：矩阵元素只取 2 的幂 → 无舍入，`worst abs diff = 0`；
  **并做过反向验证**（A/B 传反 → 192/256 元素不匹配）——mma 的 24 个实参同型，
  传错顺序编得过，「e2e 绿」本身不构成证据。

## References

- [ADR 0004](0004-cuda-cpp-subset-scope.md)：CUDA C++ 子集范围（本 ADR 在其之外新增能力）
- [known-issues KI-3](../known-issues.md)：MMA 三条红线无诊断
- `coolruntime/include/cool/device_builtins.h`：fragment 类型与内建声明
- `src/device/il_to_llvm.cpp`：`try_translate_wmma_call`、`emit_wmma_*`
- `tests/golden/e2e/wmma.cu`、`tests/golden/e2e/wmma_e2e.cu`
