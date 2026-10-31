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

// 向量重载（P6 R5 第二批续）：load 宽度直接由调用表达式的类型决定
// （`__ldg(&v4[i])` 的类型是 float4 → 一条 `ld.global.nc.v4.f32`），
// 不需要另做重载分派机制。标量与向量走同一条翻译路径。
//
// 这里同时是**成员左值**的回归用例（原本 `out4[i].x = ...` 会报
// 「不支持的左值表达式」，C17 之后支持）：三种写法各覆盖一条通路 ——
//   ① 下标基的成员写入      out4[i].x = ...   （基是 eok_subscript）
//   ② 变量基的成员写入       r.x = ...         （基是 enk_variable）
//   ③ 成员上的复合赋值       r.x += 1.0f       （读-改-写，地址只求值一次）
__global__ void ldg_vec_k(const float4* in4, float4* out4, float* out_scal,
                          const int4* in_i4, int4* out_i4, int* out_iscal) {
    const int i = threadIdx.x;

    // ① 下标结果的成员写入。
    float4 v = __ldg(&in4[i]);
    out4[i].x = v.x * 2.0f;
    out4[i].y = v.y * 2.0f;
    out4[i].z = v.z * 2.0f;
    out4[i].w = v.w * 2.0f;

    // ② 局部向量的成员写入 + ③ 成员复合赋值。
    float4 r;
    r.x = v.x;
    r.y = v.y;
    r.z = v.z;
    r.w = v.w;
    r.x += 1.0f;

    // 聚合整体赋值（与成员写入并存，验证两条路不打架）。
    out_i4[i] = __ldg(&in_i4[i]);

    // 成员只读：验证 v4 load 的 4 个分量真的是内存里那 4 个值。
    out_scal[i * 4 + 0] = r.x;
    out_scal[i * 4 + 1] = r.y;
    out_scal[i * 4 + 2] = r.z;
    out_scal[i * 4 + 3] = r.w;

    int4 w = __ldg(&in_i4[i]);
    out_iscal[i * 4 + 0] = w.x;
    out_iscal[i * 4 + 1] = w.y;
    out_iscal[i * 4 + 2] = w.z;
    out_iscal[i * 4 + 3] = w.w;
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

    // ---- 向量重载：float4 / int4 ----
    const int VN = 8;
    float4* d_in4 = nullptr;
    float4* d_out4 = nullptr;
    float* d_out_scal = nullptr;
    int4* d_in_i4 = nullptr;
    int4* d_out_i4 = nullptr;
    int* d_out_iscal = nullptr;
    if (coolMalloc((void**)&d_in4, sizeof(float4) * VN) != coolSuccess ||
        coolMalloc((void**)&d_out4, sizeof(float4) * VN) != coolSuccess ||
        coolMalloc((void**)&d_out_scal, sizeof(float) * VN * 4) != coolSuccess ||
        coolMalloc((void**)&d_in_i4, sizeof(int4) * VN) != coolSuccess ||
        coolMalloc((void**)&d_out_i4, sizeof(int4) * VN) != coolSuccess ||
        coolMalloc((void**)&d_out_iscal, sizeof(int) * VN * 4) != coolSuccess) {
        printf("ldg FAIL: vector device alloc failed\n");
        return 1;
    }
    float4 h_in4[VN];
    int4 h_in_i4[VN];
    for (int i = 0; i < VN; ++i) {
        h_in4[i].x = (float)i;
        h_in4[i].y = (float)i + 0.5f;
        h_in4[i].z = (float)i + 0.25f;
        h_in4[i].w = (float)i - 1.0f;
        h_in_i4[i].x = i;
        h_in_i4[i].y = i * 2;
        h_in_i4[i].z = i * 3;
        h_in_i4[i].w = -i;
    }
    coolMemcpy(d_in4, h_in4, sizeof(h_in4), coolMemcpyHostToDevice);
    coolMemcpy(d_in_i4, h_in_i4, sizeof(h_in_i4), coolMemcpyHostToDevice);

    ldg_vec_k<<<1, VN>>>(d_in4, d_out4, d_out_scal, d_in_i4, d_out_i4, d_out_iscal);
    if (coolDeviceSynchronize() != coolSuccess) {
        printf("ldg FAIL: vector kernel sync failed\n");
        return 1;
    }
    float4 h_out4[VN];
    float h_out_scal[VN * 4];
    int4 h_out_i4[VN];
    int h_out_iscal[VN * 4];
    coolMemcpy(h_out4, d_out4, sizeof(h_out4), coolMemcpyDeviceToHost);
    coolMemcpy(h_out_scal, d_out_scal, sizeof(h_out_scal), coolMemcpyDeviceToHost);
    coolMemcpy(h_out_i4, d_out_i4, sizeof(h_out_i4), coolMemcpyDeviceToHost);
    coolMemcpy(h_out_iscal, d_out_iscal, sizeof(h_out_iscal), coolMemcpyDeviceToHost);
    coolFree(d_in4);
    coolFree(d_out4);
    coolFree(d_out_scal);
    coolFree(d_in_i4);
    coolFree(d_out_i4);
    coolFree(d_out_iscal);

    for (int i = 0; i < VN; ++i) {
        // ① 下标基成员写入：每个分量都是输入的 2 倍。
        if (h_out4[i].x != h_in4[i].x * 2.0f || h_out4[i].y != h_in4[i].y * 2.0f ||
            h_out4[i].z != h_in4[i].z * 2.0f || h_out4[i].w != h_in4[i].w * 2.0f) {
            printf("ldg FAIL: vec[%d] = {%g,%g,%g,%g}, want {%g,%g,%g,%g}\n", i, h_out4[i].x,
                   h_out4[i].y, h_out4[i].z, h_out4[i].w, h_in4[i].x * 2.0f, h_in4[i].y * 2.0f,
                   h_in4[i].z * 2.0f, h_in4[i].w * 2.0f);
            ++failed;
        }
        if (h_out_i4[i].x != i || h_out_i4[i].y != i * 2 || h_out_i4[i].z != i * 3 ||
            h_out_i4[i].w != -i) {
            printf("ldg FAIL: veci[%d] = {%d,%d,%d,%d}, want {%d,%d,%d,%d}\n", i, h_out_i4[i].x,
                   h_out_i4[i].y, h_out_i4[i].z, h_out_i4[i].w, i, i * 2, i * 3, -i);
            ++failed;
        }
        // ②③ 变量基成员写入 + 复合赋值：只有 .x 被 +1，其余原值。
        const float want_scal[4] = {h_in4[i].x + 1.0f, h_in4[i].y, h_in4[i].z, h_in4[i].w};
        const int want_iscal[4] = {i, i * 2, i * 3, -i};
        for (int k = 0; k < 4; ++k) {
            if (h_out_scal[i * 4 + k] != want_scal[k]) {
                printf("ldg FAIL: v[%d].c%d = %g, want %g\n", i, k, h_out_scal[i * 4 + k],
                       want_scal[k]);
                ++failed;
            }
            if (h_out_iscal[i * 4 + k] != want_iscal[k]) {
                printf("ldg FAIL: vi[%d].c%d = %d, want %d\n", i, k, h_out_iscal[i * 4 + k],
                       want_iscal[k]);
                ++failed;
            }
        }
    }

    if (failed != 0) {
        return 1;
    }

    printf("ldg OK: %d floats + %d ints（读值与普通 load 一致）\n", N, N);
    return 0;
}
