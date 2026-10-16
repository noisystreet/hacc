# samples —— 可运行的 CUDA C++ 子集示例

本目录是**面向用户**的示例集：每个 `.cu` 都是自包含、可直接编译运行的小程序，
用来演示 hacc 当前支持的 CUDA C++ 子集（S0）能力。

> 与 `tests/golden/e2e/` 的区别：`tests/` 下的 `.cu` 是**测试输入**（含 SKIP 约定与断言，
> 供 CTest 驱动）；本目录的示例是**教学/演示**用途，注释更侧重「怎么用」。

## 编译与运行

```bash
# 编译为可执行文件（默认 --emit=exe，自动拼桩 + 链接 coolrt）
hacc vector_add.cu -o vector_add

# 运行（需可用的 CUDA 设备；无设备时示例会打印 SKIP 并返回 77）
./vector_add
```

若 coolrt 头不在默认搜索路径，用 `-I` 指定：

```bash
hacc vector_add.cu -o vector_add -I /path/to/coolruntime/include
```

## 示例一览

| 示例 | 演示能力 | 里程碑 |
| --- | --- | --- |
| [`vector_add.cu`](vector_add.cu) | 基础内核：指针参数、`threadIdx.x`、`<<<>>>` 启动、结果校验 | M2 |
| [`vector_type.cu`](vector_type.cu) | 向量类型（`float4`）作为内核参数 + 成员访问 | M3.0 |
| [`shared_reduce.cu`](shared_reduce.cu) | `__shared__` 静态共享内存 + `__syncthreads()` | M3.1 |
| [`constant_pi.cu`](constant_pi.cu) | `__constant__` 常量内存 + `coolMemcpyToSymbol` | M3.2 |
| [`dim3_grid.cu`](dim3_grid.cu) | `dim3` 作为内核参数（3 分量聚合） | M3.3 |

## 子集约束（S0）

示例均遵守当前子集边界，便于对照学习：

- 内核参数为标量或 POD 聚合（`float4` / `dim3` 等），无引用/模板参数。
- 左值仅支持变量与 `*p`，故用 `*(p + i)` 而非 `p[i]`（数组下标 `a[i]` 亦支持）。
- `__shared__` 仅内核内、静态数组；`__constant__` 仅文件作用域。
- 未覆盖的构造（如 `while` / `switch`）会 **fail loud**（编译失败并给出源位置），
  不会静默产出错误代码。
