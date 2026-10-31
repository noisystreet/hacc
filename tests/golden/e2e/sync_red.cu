// P6/R7：块级归约栅栏 __syncthreads_count / __syncthreads_and / __syncthreads_or。
//
// 两个层次验证：
//   1. 数值：块内谓词归约结果必须精确（43 / 128 / 0 / 1 ...，见下面的期望值）；
//   2. PTX 形态：必须出 `bar.red.popc.u32` / `bar.red.and.pred` / `bar.red.or.pred`
//      三条（CMake 里用 && 同时断言）。只断言数值不够 —— 用「shared 内存 + 两次
//      __syncthreads」也能算出同样的数值，但那是另一个实现（多一次栅栏、
//      慢一个量级），形态断言才能锁住「真的用了 bar.red」。
//
// 为什么用 N=128（4 个 warp）：块级归约必须跨 warp，单 warp 的用例可能被
// 错误实现（vote.sync 之类）蒙对。128 也让「按 3 取模的计数」不是 2 的幂，
// 算错就能看出来。
//
// 期望值（N=128）：
//   count(tid % 3 == 0) = 43   —— 0,3,...,126 共 43 个
//   count(tid < N)      = 128
//   count(tid < 0)      = 0
//   and(tid < N)        = 非 0（全部成立）
//   and(tid == 0)       = 0    （只有 0 号成立）
//   or(tid == 0)        = 非 0
//   or(tid == 999)      = 0    （无人成立）
#include <cool/cool_runtime.h>

#include <stdio.h>

#define N 128

__global__ void sync_red_k(int* out_count, int* out_and, int* out_or) {
    const int tid = threadIdx.x;

    // 全部 128 个线程都必须调用这三个（块级栅栏的收敛要求）。
    int c_mul3 = __syncthreads_count(tid % 3 == 0);
    int c_all = __syncthreads_count(tid < N);
    int c_none = __syncthreads_count(tid < 0);

    int a_all = __syncthreads_and(tid < N);
    int a_zero = __syncthreads_and(tid == 0);

    int o_zero = __syncthreads_or(tid == 0);
    int o_none = __syncthreads_or(tid == 999);

    // 每个线程都写自己那份（值应当全块一致；不一致本身就是缺陷）。
    out_count[tid * 3 + 0] = c_mul3;
    out_count[tid * 3 + 1] = c_all;
    out_count[tid * 3 + 2] = c_none;
    out_and[tid * 2 + 0] = a_all;
    out_and[tid * 2 + 1] = a_zero;
    out_or[tid * 2 + 0] = o_zero;
    out_or[tid * 2 + 1] = o_none;
}

int main() {
    int* d_count = nullptr;
    int* d_and = nullptr;
    int* d_or = nullptr;
    if (coolMalloc((void**)&d_count, sizeof(int) * N * 3) != coolSuccess ||
        coolMalloc((void**)&d_and, sizeof(int) * N * 2) != coolSuccess ||
        coolMalloc((void**)&d_or, sizeof(int) * N * 2) != coolSuccess) {
        printf("syncRed SKIP: no usable CUDA device\n");
        return 77;
    }

    sync_red_k<<<1, N>>>(d_count, d_and, d_or);
    if (coolDeviceSynchronize() != coolSuccess) {
        printf("syncRed FAIL: kernel sync failed\n");
        coolFree(d_count);
        coolFree(d_and);
        coolFree(d_or);
        return 1;
    }

    int h_count[N * 3];
    int h_and[N * 2];
    int h_or[N * 2];
    coolMemcpy(h_count, d_count, sizeof(h_count), coolMemcpyDeviceToHost);
    coolMemcpy(h_and, d_and, sizeof(h_and), coolMemcpyDeviceToHost);
    coolMemcpy(h_or, d_or, sizeof(h_or), coolMemcpyDeviceToHost);
    coolFree(d_count);
    coolFree(d_and);
    coolFree(d_or);

    int failed = 0;
    for (int t = 0; t < N; ++t) {
        const int c_mul3 = h_count[t * 3 + 0];
        const int c_all = h_count[t * 3 + 1];
        const int c_none = h_count[t * 3 + 2];
        const int a_all = h_and[t * 2 + 0];
        const int a_zero = h_and[t * 2 + 1];
        const int o_zero = h_or[t * 2 + 0];
        const int o_none = h_or[t * 2 + 1];

        if (c_mul3 != 43) {
            printf("syncRed FAIL: t=%d count(tid%%3==0) = %d, want 43\n", t, c_mul3);
            ++failed;
        }
        if (c_all != N) {
            printf("syncRed FAIL: t=%d count(tid<N) = %d, want %d\n", t, c_all, N);
            ++failed;
        }
        if (c_none != 0) {
            printf("syncRed FAIL: t=%d count(tid<0) = %d, want 0\n", t, c_none);
            ++failed;
        }
        // and/or 只断言「是否非零」，硬件对 .and/.or 给的是谓词，具体非零值不是契约。
        if (a_all == 0) {
            printf("syncRed FAIL: t=%d and(tid<N) = 0, want nonzero\n", t);
            ++failed;
        }
        if (a_zero != 0) {
            printf("syncRed FAIL: t=%d and(tid==0) = %d, want 0\n", t, a_zero);
            ++failed;
        }
        if (o_zero == 0) {
            printf("syncRed FAIL: t=%d or(tid==0) = 0, want nonzero\n", t);
            ++failed;
        }
        if (o_none != 0) {
            printf("syncRed FAIL: t=%d or(tid==999) = %d, want 0\n", t, o_none);
            ++failed;
        }
        if (failed > 8) {
            printf("syncRed FAIL: 错误过多，提前退出\n");
            return 1;
        }
    }

    if (failed != 0) {
        return 1;
    }

    printf("syncRed OK: %d 线程块内 bar.red 归约（count/and/or）全部匹配\n", N);
    return 0;
}
