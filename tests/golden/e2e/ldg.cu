// P6/R5 第二批：__ldg（只读数据缓存加载）。
//
// 两个层次验证：
//   1. 数值：__ldg 读到的值与普通 load 必须一致（走的是不同缓存路径，结果不变）；
//   2. PTX 形态：必须**同时**出 `cvta.to.global`（地址空间转换）与 `ld.global.nc`
//      （非相干读）。spike 实测：只加 `!invariant.load` 而指针留在 generic 空间时，
//      PTX 会静默退回普通 `ld.f32` —— 能编能跑，但根本没走只读缓存。
#include <cool/cool_runtime.h>

#include <stdio.h>

#define N 64

__global__ void ldg_k(const float* in, const int* in_i, float* out, int* out_i) {
    const int i = threadIdx.x;
    out[i] = __ldg(&in[i]) * 2.0f;
    out_i[i] = __ldg(&in_i[i]) + 1;
}

int main() {
    float* d_in = nullptr;
    int* d_in_i = nullptr;
    float* d_out = nullptr;
    int* d_out_i = nullptr;
    if (coolMalloc((void**)&d_in, sizeof(float) * N) != coolSuccess ||
        coolMalloc((void**)&d_in_i, sizeof(int) * N) != coolSuccess ||
        coolMalloc((void**)&d_out, sizeof(float) * N) != coolSuccess ||
        coolMalloc((void**)&d_out_i, sizeof(int) * N) != coolSuccess) {
        printf("ldg SKIP: no usable CUDA device\n");
        return 77;
    }

    float h_in[N];
    int h_in_i[N];
    for (int i = 0; i < N; ++i) {
        h_in[i] = (float)i;
        h_in_i[i] = i * 3;
    }
    coolMemcpy(d_in, h_in, sizeof(h_in), coolMemcpyHostToDevice);
    coolMemcpy(d_in_i, h_in_i, sizeof(h_in_i), coolMemcpyHostToDevice);

    ldg_k<<<1, N>>>(d_in, d_in_i, d_out, d_out_i);
    if (coolDeviceSynchronize() != coolSuccess) {
        printf("ldg FAIL: kernel sync failed\n");
        coolFree(d_in);
        coolFree(d_in_i);
        coolFree(d_out);
        coolFree(d_out_i);
        return 1;
    }

    float h_out[N];
    int h_out_i[N];
    coolMemcpy(h_out, d_out, sizeof(h_out), coolMemcpyDeviceToHost);
    coolMemcpy(h_out_i, d_out_i, sizeof(h_out_i), coolMemcpyDeviceToHost);
    coolFree(d_in);
    coolFree(d_in_i);
    coolFree(d_out);
    coolFree(d_out_i);

    int failed = 0;
    for (int i = 0; i < N; ++i) {
        if (h_out[i] != (float)i * 2.0f) {
            printf("ldg FAIL: out[%d] = %g, want %g\n", i, h_out[i], (float)i * 2.0f);
            ++failed;
        }
        if (h_out_i[i] != i * 3 + 1) {
            printf("ldg FAIL: out_i[%d] = %d, want %d\n", i, h_out_i[i], i * 3 + 1);
            ++failed;
        }
    }

    if (failed != 0) {
        return 1;
    }

    printf("ldg OK: %d floats + %d ints（读值与普通 load 一致）\n", N, N);
    return 0;
}
