// M4.4 golden/e2e 输入：原子内建（atomicAdd / Sub / Exch / Min / Max / CAS）。
//
// 设备侧映射（见 m4-subset-s1-plan.md §3.5）：
//   atomicAdd/Sub/Exch/Min/Max → LLVM atomicrmw → PTX atom.global.<op>.<type>
//   atomicCAS                  → LLVM cmpxchg   → PTX atom.global.cas.b32
// 有符号与无符号的 min/max 落不同的 PTX 类型（.s32 / .u32），故两者都要覆盖。
//
// e2e 策略（全部在**一个 kernel、一个 block** 内完成）：
//   - NTHREADS 个线程并发打同一个地址 → 真实竞争；非原子实现会丢更新，
//     g_sum 必然小于 NTHREADS。
//   - 块内 __syncthreads() 之后由 0 号线程导出终值：块内同步是确定的，
//     跨块同步不是（PTX 无 grid 级 barrier）。
//   - 单线程确定性算子（exch/sub）用 `if (t == 0)` 隔开，避免顺序不定使断言失效。
//
// 注意：本用例**不**依赖「前一个 kernel 写、后一个 kernel 读设备全局」。
// 该写法原先不可用（每个 kernel 各载入一份 module，设备全局各一份副本），现已由
// ADR 0007 修复（模块粒度 = 编译单元）；跨 kernel 共享由 dev_global_shared.cu 专门覆盖，
// 本文件仍把竞争集中在单 kernel 内，以便断言只反映原子语义、不与共享语义纠缠。
#include <cool/cool_runtime.h>

#include <stdio.h>

#define NTHREADS 128

__device__ int g_sum = 0;             // 竞争累加（int atomicAdd）
__device__ int g_min = 0;             // atomicMin 负值 → 必须落 atom.min.s32
__device__ int g_max = 0;             // atomicMax 有符号 → 落 atom.max.s32
__device__ unsigned int g_umax = 0u;  // 无符号 max → 落 atom.max.u32
__device__ unsigned int g_umin = 100u;  // 无符号 min → 落 atom.min.u32
__device__ float g_fsum = 0.0f;       // float atomicAdd → 落 atom.add.f32
__device__ int g_cas = 0;             // atomicCAS 抢占目标
__device__ int g_cas_wins = 0;        // 抢占成功者计数（应为 1）
__device__ int g_sub = 100;           // 单线程 atomicSub
__device__ int g_exch = 0;            // 单线程 atomicExch

__global__ void atomics_kernel(int* out) {
    int t = threadIdx.x;

    // --- 竞争段：所有线程打同一批地址 ---
    atomicAdd(&g_sum, 1);
    atomicMin(&g_min, -5);
    atomicMax(&g_max, 5);
    atomicMax(&g_umax, 7u);
    atomicMin(&g_umin, 9u);
    atomicAdd(&g_fsum, 1.5f);

    // 只有第一个把 0 换成 42 的线程算「抢占成功」，其余线程拿到旧值 42。
    int old = atomicCAS(&g_cas, 0, 42);
    if (old == 0) {
        atomicAdd(&g_cas_wins, 1);
    }

    // --- 块内同步：保证导出时竞争段的原子都已生效 ---
    __syncthreads();

    if (t == 0) {
        // 单线程确定性算子：返回值是**运算前的旧值**。
        int exch_old = atomicExch(&g_exch, 9);
        int sub_old = atomicSub(&g_sub, 3);

        out[1] = g_sum;
        out[2] = g_min;
        out[3] = (int)g_umax;
        out[4] = (int)g_fsum;
        out[5] = g_cas;
        out[6] = exch_old;
        out[7] = g_exch;
        out[8] = sub_old;
        out[9] = g_sub;
        out[10] = g_cas_wins;
        out[11] = g_max;
        out[12] = (int)g_umin;
    }
}

int main() {
    int* d_out = nullptr;
    // 无可用 CUDA 设备时 coolMalloc 失败：返回 77（SKIP 约定），供测试框架跳过。
    if (coolMalloc((void**)&d_out, sizeof(int) * 16) != coolSuccess) {
        printf("atomics SKIP: no usable CUDA device\n");
        return 77;
    }

    int h_out[16] = {0};
    coolMemcpy(d_out, h_out, sizeof(h_out), coolMemcpyHostToDevice);

    atomics_kernel<<<1, NTHREADS>>>(d_out);
    coolDeviceSynchronize();

    coolMemcpy(h_out, d_out, sizeof(h_out), coolMemcpyDeviceToHost);
    coolFree(d_out);

    // 逐项校验，失败时打印具体项，便于定位是哪条原子语义错了。
    struct {
        const char* what;
        int got;
        int want;
    } checks[] = {
        {"atomicAdd 竞争（并发累加次数）", h_out[1], NTHREADS},
        {"atomicMin 负值（int）", h_out[2], -5},
        {"atomicMax（unsigned）", h_out[3], 7},
        {"atomicAdd（float）", h_out[4], (int)(1.5f * NTHREADS)},
        {"atomicCAS 结果值", h_out[5], 42},
        {"atomicExch 旧值", h_out[6], 0},
        {"atomicExch 新值", h_out[7], 9},
        {"atomicSub 旧值", h_out[8], 100},
        {"atomicSub 结果", h_out[9], 97},
        {"atomicCAS 抢占成功次数", h_out[10], 1},
        {"atomicMax（int）", h_out[11], 5},
        {"atomicMin（unsigned）", h_out[12], 9},
    };

    int failed = 0;
    for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); ++i) {
        if (checks[i].got != checks[i].want) {
            printf("atomics FAILED: %s: got %d, want %d\n", checks[i].what, checks[i].got,
                   checks[i].want);
            failed = 1;
        }
    }
    if (failed) {
        return 1;
    }
    printf("atomics OK: sum=%d min=%d umax=%d fsum=%d cas=%d sub=%d\n", h_out[1], h_out[2], h_out[3],
           h_out[4], h_out[5], h_out[9]);
    return 0;
}
