// M4.4 golden/e2e 输入：原子内建（atomicAdd / Sub / Exch / Min / Max / CAS）。
//
// 设备侧映射（见 m4-subset-s1-plan.md §3.5）：
//   atomicAdd/Sub/Exch/Min/Max → LLVM atomicrmw → PTX atom.global.<op>.<type>
//   atomicAnd/Or/Xor           → LLVM atomicrmw → PTX atom.global.{and,or,xor}.b32
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
// 位运算原子（P5.1）：竞争类用「全体线程共同置位/清位」，结果唯一，断言强。
__device__ unsigned int g_bits_or = 0u;            // 重复 OR 2u → 2
__device__ unsigned int g_bits_and = 0xFFFFFFFFu;  // 重复 AND 0xFFu → 0xFF
__device__ int g_and = 0xFF;                       // 单线程 atomicAnd
__device__ int g_or = 0x00;                        // 单线程 atomicOr
__device__ int g_xor = 0xFF;                       // 单线程 atomicXor

__global__ void atomics_kernel(int* out) {
    int t = threadIdx.x;

    // --- 竞争段：所有线程打同一批地址 ---
    atomicAdd(&g_sum, 1);
    atomicMin(&g_min, -5);
    atomicMax(&g_max, 5);
    atomicMax(&g_umax, 7u);
    atomicMin(&g_umin, 9u);
    atomicAdd(&g_fsum, 1.5f);

    // 位运算原子：128 线程重复施加同一常量，结果必须与「常量自身」一致。
    //   g_bits_or  = 0 OR 2   ×128 = 2      （若误实现为 ADD 会是 256，为 AND/XOR 会是 0）
    //   g_bits_and = 0xFFFFFFFF AND 0xFF ×128 = 0xFF  （若误实现为 OR 则不变）
    // 用常量而非 `1u << (t%32)` 这类表达式：hacc 尚不支持 << / ~ / & / | / ^ 运算符
    //（M1.2.3 只覆盖算术/比较/赋值/取址/解引用/转换），已记入 docs/TODO.md。
    atomicOr(&g_bits_or, 2u);
    atomicAnd(&g_bits_and, 0xFFu);

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

        // 位运算：同样返回旧值。三个算子各给一组能区分运算类型的初值/入参。
        int and_old = atomicAnd(&g_and, 0x0F);  // 0xFF & 0x0F = 0x0F，旧值 0xFF
        int or_old = atomicOr(&g_or, 0x0F);     // 0x00 | 0x0F = 0x0F，旧值 0x00
        int xor_old = atomicXor(&g_xor, 0x0F);  // 0xFF ^ 0x0F = 0xF0，旧值 0xFF

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
        out[13] = (int)g_bits_or;
        out[14] = (int)g_bits_and;
        out[15] = and_old;
        out[16] = g_and;
        out[17] = or_old;
        out[18] = g_or;
        out[19] = xor_old;
        out[20] = g_xor;
    }
}

int main() {
    int* d_out = nullptr;
    // 无可用 CUDA 设备时 coolMalloc 失败：返回 77（SKIP 约定），供测试框架跳过。
    if (coolMalloc((void**)&d_out, sizeof(int) * 24) != coolSuccess) {
        printf("atomics SKIP: no usable CUDA device\n");
        return 77;
    }

    int h_out[24] = {0};
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
        // 位运算：竞争类的期望值唯一，误实现成别的算子会立刻露出来。
        {"atomicOr 竞争（重复 OR 2）", h_out[13], 2},
        {"atomicAnd 竞争（重复 AND 0xFF）", h_out[14], 0xFF},
        {"atomicAnd 旧值", h_out[15], 0xFF},
        {"atomicAnd 结果", h_out[16], 0x0F},
        {"atomicOr 旧值", h_out[17], 0x00},
        {"atomicOr 结果", h_out[18], 0x0F},
        {"atomicXor 旧值", h_out[19], 0xFF},
        {"atomicXor 结果", h_out[20], 0xF0},
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
