# 开发环境前置与排查（Dev Environment）

> 用途：记录**环境层面**的前置条件与踩过的坑，以及「只有真机能抓到」的缺陷教训。
> 与 [known-issues.md](known-issues.md) 的分工：那里记**未修复的行为缺陷**；本文件记**环境问题与已修缺陷的经验**。
>
> 来源：M4 阶段交接文档（handoff，已废弃删除）的 §3.1 / §5.3，内容经复核后迁入仓库。

---

## 1. CUDA 可用性（`nvidia_uvm`）—— 踩过的坑

**`nvidia-smi` 正常 ≠ CUDA 可用。** 本机曾出现：`nvidia-smi` 显示 RTX 4060 / 8188 MiB / 44C 一切正常，
但 `libcuda` 的 `cuInit(0)` 返回 **999 `CUDA_ERROR_UNKNOWN`**、`cuDeviceGetCount` 为 0，
于是所有 GPU e2e 打印 `SKIP: no usable CUDA device` 返回 77 —— ctest 记为 **Skipped，看起来像"全绿"**。

根因：**`nvidia_uvm` 内核模块未加载**。`nvidia-smi` 走 NVML（`/dev/nvidiactl`），**完全不碰 uvm**，
所以它在容器里怎么跑都不会把 uvm 拉起来（实测：跑完整 `nvidia-smi` 后 `lsmod` 仍无 uvm）。

**诊断顺序（照抄，别只看 nvidia-smi）**：

```sh
lsmod | grep nvidia_uvm                 # 应有 nvidia_uvm 一行
ls -l /dev/nvidia-uvm                   # 应有 crw-rw-rw- 510,0（major 可能是别的值）
grep -i uvm /proc/devices               # 应显示 "510 nvidia-uvm"
# 三者齐备后再看 cuInit（必须 0）：
python3 -c 'import ctypes;l=ctypes.CDLL("libcuda.so.1");print(l.cuInit(0))'
```

**修复只能在宿主机**：

```sh
sudo modprobe nvidia_uvm     # 模块文件：/lib/modules/<ver>/updates/dkms/nvidia-uvm.ko.xz
```

**为什么容器内救不了自己**（本机实测 `CapEff: 0000000000000000`、`NoNewPrivs: 1`）：

- `sudo` 被 `no_new_privs` 阻断（`/etc/sudo.conf` 还属于 uid 65534，是容器注入的）；
- `/usr/bin/nvidia-modprobe` 虽是 `-rwsr-xr-x`（setuid root），同一标志让提权失效，实跑**退出码 1**；
- 容器内 `mknod /dev/nvidia-uvm` 也不行：无 `CAP_MKNOD`，且模块没加载就没有 major 可注册。

宿主机加载后，容器内**可能需要重启容器**才能看到新设备节点（节点通常由 nvidia-container-toolkit
在启动时注入，需 `--gpus all` 或 `NVIDIA_DRIVER_CAPABILITIES=compute,utility`）。
**本机实测：宿主机 `modprobe` 后容器内直接可见，无需重启。**

### 1.1 C4 之后：假绿已收敛，但诊断流程仍然有效

上面的「SKIP 显绿」是 **C4 之前**的语义。C4（`gpu_state` 探针 + `make check-strict`，见
[CHANGELOG](../CHANGELOG.md)）之后：

- 设备节点在、但初始化失败 → **FAIL（1）**，不再静默 SKIP；
- 连设备节点都没有 → 仍是 77（SKIP），但打印醒目 WARNING；
- `make check` 末尾汇总 SKIP 数量；`make check-strict` 把「有 SKIP」直接判失败。

所以现在遇到 GPU 用例失败，**本节的四条命令仍是定位「是 uvm 没加载还是代码真错」的第一步**。

> 教训：看到 ctest 大面积 **Skipped 不要当成"这机器没 GPU，正常"**。先查 `cuInit` 返回码。
> Skipped 会掩盖真实回归——KI-1（设备全局不跨 kernel 共享，"能编译、结果不对"）当年就是靠真机
> e2e 发现的。修好 uvm 后本机 `126/126` 全过、**零 SKIP**。

---

## 2. 「能编译、看着跑通、结果不对」—— 三个真 bug 的教训

M4 阶段本轮修掉三个缺陷，**全部属于 golden 文本断言抓不到、只有真机 e2e 能暴露**的类型：

| 缺陷 | 症状 | 根因 |
| --- | --- | --- |
| `cudaGetSymbolAddress` 改写永不命中 | 主机源里残留 `cudaGetSymbolAddress` → 未定义标识符 | 标识符长度写成 **21**，实际 **20** |
| device 源 `(void)0` 替换炸掉（**既有缺陷**） | `coolError_t e = cudaMemcpyToSymbol(...)` → `= (void)0` 编译失败 | 只考虑了语句位置，未考虑有返回值的表达式位置；已统一改 `coolSuccess` |
| `haccMemcpyFromSymbolAll` 参数顺序反了 | 首次实测 `err=1 (InvalidValue)` | 照 `ToSymbol` 的 `(symbol, src, ...)` 写成 `(desc, dst, ...)`，实为 `(dst, symbol, ...)` |

> 教训：这类缺陷**只有真机 e2e 能暴露**。新增符号/API 类改动，务必配一条真机用例，
> 不要只靠 golden 文本断言。

同类但属于**未修复**的缺陷（wmma 三条红线、递归深度上限等）见 [known-issues.md](known-issues.md)。
