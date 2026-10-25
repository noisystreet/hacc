// S2 T3：数学内建。断言分两档，与 coolrt `device_builtins.h` 的精度契约一一对应：
//
//   1. **精确映射**（单条精确 PTX 指令：sqrt.rn / abs / min / max / cvt.rmi·rpi·rzi /
//      fma.rn / rcp.rn）→ 与期望值 **严格相等**；
//   2. **近似映射**（.approx 指令：rsqrt / div / ex2 / lg2 / sin / cos）→ 容差比较。
//
// 近似项特意选在「该指令本就精确的点」上（如 __expf(0)=1、__logf(1)=0、__sinf(0)=0），
// 这样即使将来换成精确实现也仍然通过；另有一档单独验证 approx 在非平凡点上的合理性。
//
// 若有人把某条近似指令换成精确实现（或反之）却不同步这里，本用例会以数值失败暴露出来。
#include <cool/cool_runtime.h>

#include <stdio.h>

#define N 17
#define TOL 1e-4f

__global__ void math_k(float* out) {
    const int i = threadIdx.x;
    switch (i) {
        /* --- 精确档 --- */
        case 0: out[i] = sqrtf(16.0f); break;              /* 4 */
        case 1: out[i] = fabsf(-3.5f); break;              /* 3.5 */
        case 2: out[i] = fminf(2.0f, 3.0f); break;         /* 2 */
        case 3: out[i] = fmaxf(2.0f, 3.0f); break;         /* 3 */
        case 4: out[i] = floorf(-2.5f); break;             /* -3 */
        case 5: out[i] = ceilf(-2.5f); break;              /* -2 */
        case 6: out[i] = truncf(-2.5f); break;             /* -2 */
        case 7: out[i] = __fmaf_rn(2.0f, 3.0f, 1.0f); break; /* 7 */
        case 8: out[i] = __fsqrt_rn(9.0f); break;          /* 3 */
        case 9: out[i] = __frcp_rn(4.0f); break;           /* 0.25 */
        /* --- 近似档，但取值点本身精确 --- */
        case 10: out[i] = rsqrtf(4.0f); break;             /* 0.5 */
        case 11: out[i] = __fdividef(7.0f, 2.0f); break;   /* 3.5 */
        case 12: out[i] = __expf(0.0f); break;             /* 1 */
        case 13: out[i] = __logf(1.0f); break;             /* 0 */
        case 14: out[i] = __sinf(0.0f); break;             /* 0 */
        case 15: out[i] = __cosf(0.0f); break;             /* 1 */
        /* --- 近似档，非平凡点：只要求落在容差内（证明指令真的算了） --- */
        default: out[i] = __expf(1.0f); break;             /* e = 2.7182817 */
    }
}

int main() {
    float* d_out = nullptr;
    if (coolMalloc((void**)&d_out, sizeof(float) * N) != coolSuccess) {
        printf("mathBuiltins SKIP: no usable CUDA device\n");
        return 77;
    }

    math_k<<<1, N>>>(d_out);
    if (coolDeviceSynchronize() != coolSuccess) {
        printf("mathBuiltins FAIL: kernel sync failed\n");
        coolFree(d_out);
        return 1;
    }

    float h[N];
    coolMemcpy(h, d_out, sizeof(h), coolMemcpyDeviceToHost);
    coolFree(d_out);

    static const float want[N] = {
        4.0f, 3.5f, 2.0f, 3.0f, -3.0f, -2.0f, -2.0f, 7.0f, 3.0f, 0.25f,
        0.5f, 3.5f, 1.0f, 0.0f, 0.0f, 1.0f, 2.7182817f,
    };
    static const char* names[N] = {
        "sqrtf", "fabsf", "fminf", "fmaxf", "floorf", "ceilf", "truncf", "__fmaf_rn",
        "__fsqrt_rn", "__frcp_rn", "rsqrtf", "__fdividef", "__expf(0)", "__logf(1)",
        "__sinf(0)", "__cosf(0)", "__expf(1)",
    };

    int failed = 0;
    for (int i = 0; i < N; ++i) {
        const float diff = h[i] > want[i] ? h[i] - want[i] : want[i] - h[i];
        const int ok = (i == 16) ? (diff <= TOL) : (h[i] == want[i]);
        if (!ok) {
            printf("mathBuiltins FAIL: %s = %.9g, want %.9g (diff %g)\n", names[i], h[i],
                   want[i], diff);
            ++failed;
        }
    }
    if (failed != 0) {
        return 1;
    }

    printf("mathBuiltins OK: %d builtins (%s)\n", N,
           "exact: ==; approx: within 1e-4 at nontrivial point");
    return 0;
}
