// 取整语义两档 + fmaf 别名。
//
// **这个用例的存在理由**：`roundf` 与 `rintf` 只差一个 tie 规则，`truncf` 又和它们
// 在前一半输入上完全一致 —— 拿 1.4 / -1.4 这类「非 .5」的值测，三种实现全部通过，
// 等于没测。所以输入全部取 **.5 边界**，让 round / rint / trunc 三者两两可分：
//
//   输入      roundf（.5 远离零，CUDA 语义）  rintf（.5 到偶数）  truncf（截断）
//    0.5            1                            0                 0
//    1.5            2                            2                 1
//    2.5            3                            2                 2
//   -0.5           -1                           -0                -0
//   -1.5           -2                           -2                -1
//   -2.5           -3                           -2                -2
//
// 输入由**主机传入**（`in[]`），不是字面量：字面量会被常量折叠掉，PTX 里根本
// 不出现目标指令，golden 断言就变成空转（int_builtins 首次实现踩过这个坑）。
//
// fmaf：除了「2*3+1 = 7」这种平凡值，还放了**只有真 fma 才对**的一组 ——
//   fmaf(1+2^-23, 1-2^-23, -1) = -2^-46 ≈ -1.4210855e-14
// 若实现退化成「先乘后加」，乘积 (1+eps)(1-eps) = 1-eps² 会先被舍入成 1.0，
// 结果变成 0 —— 两者相差无穷多个 ulp，一眼可辨。
#include <cool/cool_runtime.h>

#include <stdio.h>

#define N 10

__global__ void round_k(const float* in, const float* in_b, const float* in_c, float* out_round,
                        float* out_rint, float* out_fma) {
    const int i = threadIdx.x;
    const float x = in[i];
    out_round[i] = roundf(x);
    out_rint[i] = rintf(x);
    out_fma[i] = fmaf(x, in_b[i], in_c[i]);
}

int main() {
    float* d_in = nullptr;
    float* d_in_b = nullptr;
    float* d_in_c = nullptr;
    float* d_round = nullptr;
    float* d_rint = nullptr;
    float* d_fma = nullptr;
    if (coolMalloc((void**)&d_in, sizeof(float) * N) != coolSuccess ||
        coolMalloc((void**)&d_in_b, sizeof(float) * N) != coolSuccess ||
        coolMalloc((void**)&d_in_c, sizeof(float) * N) != coolSuccess ||
        coolMalloc((void**)&d_round, sizeof(float) * N) != coolSuccess ||
        coolMalloc((void**)&d_rint, sizeof(float) * N) != coolSuccess ||
        coolMalloc((void**)&d_fma, sizeof(float) * N) != coolSuccess) {
        printf("roundFma SKIP: no usable CUDA device\n");
        return 77;
    }

    // 前 8 个是 .5 边界（见文件头表）；后 2 个用于 fma 的精确性。
    const float h_in[N] = {0.5f,  1.5f,   2.5f,   -0.5f,  -1.5f,
                           -2.5f, 1.4f,   -1.4f,  1.0000001192092896f, /* 1 + 2^-23 */
                           2.0f};
    const float h_b[N] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                          1.0f, 1.0f, 1.0f, 0.9999998807907104f, /* 1 - 2^-23 */
                          3.0f};
    const float h_c[N] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 1.0f};

    coolMemcpy(d_in, h_in, sizeof(h_in), coolMemcpyHostToDevice);
    coolMemcpy(d_in_b, h_b, sizeof(h_b), coolMemcpyHostToDevice);
    coolMemcpy(d_in_c, h_c, sizeof(h_c), coolMemcpyHostToDevice);

    round_k<<<1, N>>>(d_in, d_in_b, d_in_c, d_round, d_rint, d_fma);
    if (coolDeviceSynchronize() != coolSuccess) {
        printf("roundFma FAIL: kernel sync failed\n");
        coolFree(d_in);
        coolFree(d_in_b);
        coolFree(d_in_c);
        coolFree(d_round);
        coolFree(d_rint);
        coolFree(d_fma);
        return 1;
    }

    float h_round[N];
    float h_rint[N];
    float h_fma[N];
    coolMemcpy(h_round, d_round, sizeof(h_round), coolMemcpyDeviceToHost);
    coolMemcpy(h_rint, d_rint, sizeof(h_rint), coolMemcpyDeviceToHost);
    coolMemcpy(h_fma, d_fma, sizeof(h_fma), coolMemcpyDeviceToHost);
    coolFree(d_in);
    coolFree(d_in_b);
    coolFree(d_in_c);
    coolFree(d_round);
    coolFree(d_rint);
    coolFree(d_fma);

    // roundf：.5 远离零。
    const float want_round[N] = {1.0f, 2.0f, 3.0f, -1.0f, -2.0f,
                                 -3.0f, 1.0f, -1.0f, 1.0f, 2.0f};
    // rintf：.5 到偶数（0.5→0、2.5→2、-0.5→-0、-2.5→-2）。
    const float want_rint[N] = {0.0f, 2.0f, 2.0f, 0.0f, -2.0f,
                                -2.0f, 1.0f, -1.0f, 1.0f, 2.0f};
    // fmaf：前 8 个 b=1,c=0 → 等于输入；index 8 是只有真 fma 才对的精确值。
    const float want_fma[N] = {0.5f,  1.5f,   2.5f,   -0.5f, -1.5f,
                               -2.5f, 1.4f,   -1.4f,
                               -1.4210854715202004e-14f, /* -2^-46 */
                               7.0f};
    static const char* names_round[N] = {"roundf(0.5)",  "roundf(1.5)",  "roundf(2.5)",
                                         "roundf(-0.5)", "roundf(-1.5)", "roundf(-2.5)",
                                         "roundf(1.4)",  "roundf(-1.4)", "roundf(1+eps)",
                                         "roundf(2)"};
    static const char* names_rint[N] = {"rintf(0.5)",  "rintf(1.5)",  "rintf(2.5)",
                                        "rintf(-0.5)", "rintf(-1.5)", "rintf(-2.5)",
                                        "rintf(1.4)",  "rintf(-1.4)", "rintf(1+eps)",
                                        "rintf(2)"};
    static const char* names_fma[N] = {"fmaf(0.5)",  "fmaf(1.5)",  "fmaf(2.5)", "fmaf(-0.5)",
                                       "fmaf(-1.5)", "fmaf(-2.5)", "fmaf(1.4)", "fmaf(-1.4)",
                                       "fmaf(1+eps,1-eps,-1)", "fmaf(2,3,1)"};

    int failed = 0;
    for (int i = 0; i < N; ++i) {
        // 全部是精确可表示的值，用 == 断言（不留容差口子）。
        if (h_round[i] != want_round[i]) {
            printf("roundFma FAIL: %s = %.9g, want %.9g\n", names_round[i], h_round[i],
                   want_round[i]);
            ++failed;
        }
        if (h_rint[i] != want_rint[i]) {
            printf("roundFma FAIL: %s = %.9g, want %.9g\n", names_rint[i], h_rint[i],
                   want_rint[i]);
            ++failed;
        }
        if (h_fma[i] != want_fma[i]) {
            printf("roundFma FAIL: %s = %.9g, want %.9g\n", names_fma[i], h_fma[i],
                   want_fma[i]);
            ++failed;
        }
    }

    if (failed != 0) {
        return 1;
    }

    printf("roundFma OK: roundf/rintf 在 .5 边界两两可分，fmaf 精确（%d 项）\n", N);
    return 0;
}
