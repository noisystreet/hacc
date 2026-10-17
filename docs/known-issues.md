# 已知缺陷登记（Known Issues）

> 用途：记录**已确认但尚未修复**的缺陷。每条包含症状、复现、根因（定位到代码）、影响、
> 当前绕过、修复方向与验收方式。修复后把状态改为「已修」，注明修复提交，不要直接删除条目。
>
> 与 [TODO.md](TODO.md) 的分工：TODO 记「待拍板的决策 / 待排期的里程碑」，本文件记「已确认的
> 行为缺陷」。缺陷修复所需的排期决策仍写进 TODO。

---

## KI-1 设备全局变量不跨 kernel 共享（每个 kernel 各载入一份 module）

- **状态**：未修（Open）
- **发现于**：M4.4 原子内建 e2e（提交 `699083f` 之前的一次中间版本）
- **影响面**：M4.1 起的所有设备全局变量（`__device__`），与原子/栅栏无关
- **严重度**：高（跨 kernel 的设备全局状态**静默失效**，不报错、不崩溃）

### 症状

kernel A 写入的 `__device__` 变量，kernel B 读不到。每个 kernel 看到的都是自己那份
「模块初始值」，而非上一个 kernel 留下的终值。

单 kernel 内一切正常，所以既有 golden / e2e 全部通过——包括 M4.1 的 `dev_global` 用例
（它只用一个 kernel）。

### 复现

```cuda
__device__ int g = 0;

__global__ void writer() { atomicAdd(&g, 1); }   // 128 线程 → g 应为 128
__global__ void reader(int* out) { out[0] = g; }  // 期望读出 128

int main() {
    writer<<<1, 128>>>(d_out);
    reader<<<1, 1>>>(d_out);
    coolDeviceSynchronize();
    // 实测：out[0] == 0，而非 128
}
```

同期还观察到：`reader` **自己**对同一批全局做的原子是有效的（`atomicExch` 的旧值/新值都正确），
只是看不到 `writer` 的写入——这正好把问题定性为「实例隔离」，而非原子实现有错。

### 根因

`src/host/stub_gen.cpp` 按**内核名**逐个生成模块状态：

```cpp
for (const std::string& name : kernel_names) {
    out += "static const unsigned char __cool_image_" + name + "[] = " + render_byte_array(ptx_text) + ";";
    out += "static coolModule_t __cool_module_" + name + " = nullptr;";
    out += "static std::once_flag __cool_once_" + name + ";";
    // KernelRegistration{ name, image, image_size, &kernel, &once, &module }
}
```

`include/coolcc/hacc_runtime.h` 的惰性注册再对**每个** registration 各做一次：

```cpp
std::call_once(*reg.once, [&reg] {
    coolModuleLoadData(&module, reg.image);            // ← 同一份 PTX 载入 N 次
    coolModuleGetFunction(&kernel, module, reg.name);
});
```

于是 N 个 kernel → N 次 `coolModuleLoadData` → 驱动创建 **N 个 module 实例**，每个实例
各自持有该 PTX 中全部 `.global` 变量的一份独立副本。

顺带两个佐证 / 副作用：

1. `__cool_image_<name>` 把**完整** `ptx_text` 重复渲染 N 份，二进制体积按 kernel 数线性膨胀。
2. M3.2 的 `haccMemcpyToSymbolAll` 必须遍历**所有** registration 去逐个模块填 `__constant__`
   符号——那本身就是在给这个设计打补丁。

### 影响

- 跨 kernel 的设备全局状态一律不可用：全局计数器、设备侧配置、分阶段归约的收尾读取、
  「kernel A 准备数据 / kernel B 使用」这类常见写法全部失效，且**无任何报错**。
- 用户若从小 kernel 数迁移到多 kernel 程序，会遇到「单测全绿、实际行为错」的静默故障。

### 当前绕过

需要跨 kernel 的状态放到**设备内存**（`coolMalloc` + 指针参数）里，而非 `__device__` 变量；
或把逻辑收进单个 kernel，用块内 `__syncthreads()` 同步。
M4.4 的 e2e（`tests/golden/e2e/atomics.cu`）即采用后者，并在文件头注明了原因。

### 修复方向（未实施，待排期）

1. 把「模块」从 kernel 维度**上移到编译单元维度**：同一份 PTX 只载入一次，
   `KernelRegistration` 只保留 `name` + `kernel` 槽位，`module`/`once` 提到单元级。
2. 首启时一次 `coolModuleLoadData`，随后对每个 kernel 只做 `coolModuleGetFunction`。
3. 连带简化 `haccMemcpyToSymbolAll`：只有一份 module，无需遍历 fan-out
   （`__cool_all_regs` 的符号填充逻辑可退化为单模块操作）。
4. 注意生命周期：`coolModuleUnload` 现按注册项隐式管理，上移后需明确「进程级不卸载」或
   加显式的收尾钩子。
5. 兼容多 `.cu` 混合编译：若将来一次链接包含多个编译单元，需要按 TU 各自一个 module，
   而不是全局只有一个。

### 验收

1. 新增 e2e：`writer`（128 线程 `atomicAdd`）→ `reader`（拷出），断言 `out[0] == 128`。
2. 新增 e2e：两个 kernel 各自写**不同**的设备全局，交叉读取，断言互不覆盖、互不丢失。
3. 回归：既有 `make check` 全绿；顺带确认生成的主机桩中 PTX 只出现一次（体积不再随 kernel 数膨胀）。
