// C12 / MMA e2e（A4）：单 warp 的 D = A×B + C（m16n16k16，f16 输入 / f32 累加），
// 与 CPU 参考结果比对。
//
// **为什么矩阵元素只用 2 的幂**（1 / 0.5 / -1 / 2）：这些值在 f16 内精确，乘积在 f32 精确，
// 累加（16 项、|值| ≤ 4）也无舍入 —— 于是 CPU 参考与 GPU 结果可期望**一致**，
// 比对能用极紧的容差。若用「随便的精度友好值」，A/B 传反或 fragment 内部错位这类
// 结构性错误会被容差吃掉 —— 而本用例存在的唯一意义就是抓这类错误
// （mma 的 24 个实参同型，传错顺序也编得过）。
//
// 三条红线（hacc 侧不设防，这里主动遵守）：
//   1. 1 block × 32 线程 = 整 warp 一致执行，**不**把 mma 放进 if；
//   2. 缓冲全部走 coolMalloc（实测 256B 对齐 ≥ 32B 要求），不传 ptr+i 给 load/store；
//   3. 顺序固定 D = A×B + C，A row-major、B col-major（见 coolrt 头注释）。
#include <cool/cool_runtime.h>

#include <stdio.h>

// **不要 include `<math.h>`**：EDG 走 `--g++` 模式时会命中 libstdc++ 的
// `/usr/include/c++/14/math.h`（转发到 `<cmath>`），一路拉进 `c++config.h`，
// 那里成片的 `__has_builtin(...)` EDG 不认，报 21 个错（与 m5-handoff §7 记的
// `<cstdio>` 同一个坑，`math.h` 也会中招）。这里只需要绝对值，自己写。
static double abs_d(double v) { return v < 0.0 ? -v : v; }

#define M 16
#define N 16
#define K 16

__global__ void mma_kernel(const unsigned short* a, const unsigned short* b, const float* c,
                           float* d) {
    cool_wmma_frag_a fa;
    cool_wmma_frag_b fb;
    cool_wmma_frag_c fc;
    cool_wmma_frag_d fd;

    cool_wmma_load_a_row(&fa, a);
    cool_wmma_load_b_col(&fb, b);
    cool_wmma_load_c_row(&fc, c);
    cool_wmma_mma(&fd, &fa, &fb, &fc);
    cool_wmma_store_d_row(d, &fd);
}

// 可精确表示的 f16 值与其位模式（0x3C00=1.0, 0x3800=0.5, 0xBC00=-1.0, 0x4000=2.0）。
// 用表而不是 float→f16 转换：转换本身的舍入会引入「精度差异」，掩盖真正要抓的错误。
static const float kValues[4] = {1.0f, 0.5f, -1.0f, 2.0f};
static const unsigned short kValueBits[4] = {0x3C00u, 0x3800u, 0xBC00u, 0x4000u};

// A[i][k] / B[k][j] 的取值只依赖下标，CPU 侧用同一组公式算参考值。
static int pick_a(int i, int k) { return (i * 3 + k) & 3; }
static int pick_b(int k, int j) { return (k * 2 + j + 1) & 3; }

int main() {
    unsigned short* d_a = nullptr;
    unsigned short* d_b = nullptr;
    float* d_c = nullptr;
    float* d_d = nullptr;

    // 无可用 CUDA 设备时 coolMalloc 失败：返回 77（SKIP 约定），供测试框架跳过。
    if (coolMalloc((void**)&d_a, sizeof(unsigned short) * M * K) != coolSuccess ||
        coolMalloc((void**)&d_b, sizeof(unsigned short) * K * N) != coolSuccess ||
        coolMalloc((void**)&d_c, sizeof(float) * M * N) != coolSuccess ||
        coolMalloc((void**)&d_d, sizeof(float) * M * N) != coolSuccess) {
        printf("mma SKIP: no usable CUDA device\n");
        return 77;
    }

    // A 按 row-major：a[i*K + k] = A[i][k]。
    unsigned short a[M * K];
    for (int i = 0; i < M; ++i) {
        for (int k = 0; k < K; ++k) {
            a[i * K + k] = kValueBits[pick_a(i, k)];
        }
    }
    // B 按 **col-major**：b[j*K + k] = B[k][j]（load_b_col 要求）。
    unsigned short b[K * N];
    for (int j = 0; j < N; ++j) {
        for (int k = 0; k < K; ++k) {
            b[j * K + k] = kValueBits[pick_b(k, j)];
        }
    }
    // C 是 f32：0.25*(i+j) 在 f32 内精确。
    float c[M * N];
    for (int i = 0; i < M; ++i) {
        for (int j = 0; j < N; ++j) {
            c[i * N + j] = 0.25f * (float)(i + j);
        }
    }

    // CPU 参考：double 累加后比对（f16 值精确 → f32 累加也无舍入）。
    double ref[M * N];
    for (int i = 0; i < M; ++i) {
        for (int j = 0; j < N; ++j) {
            double acc = (double)c[i * N + j];
            for (int k = 0; k < K; ++k) {
                acc += (double)kValues[pick_a(i, k)] * (double)kValues[pick_b(k, j)];
            }
            ref[i * N + j] = acc;
        }
    }

    coolMemcpy(d_a, a, sizeof(a), coolMemcpyHostToDevice);
    coolMemcpy(d_b, b, sizeof(b), coolMemcpyHostToDevice);
    coolMemcpy(d_c, c, sizeof(c), coolMemcpyHostToDevice);

    // 1 block × 32 线程：整 warp 一致执行。
    mma_kernel<<<1, 32>>>(d_a, d_b, d_c, d_d);
    coolDeviceSynchronize();

    float h_d[M * N];
    coolMemcpy(h_d, d_d, sizeof(h_d), coolMemcpyDeviceToHost);

    int failures = 0;
    double worst = 0.0;
    for (int idx = 0; idx < M * N; ++idx) {
        double diff = abs_d((double)h_d[idx] - ref[idx]);
        double tol = 1e-6 * (1.0 + abs_d(ref[idx]));
        if (diff > tol) {
            if (failures < 3) {
                printf("mma FAILED: d[%d] = %f, expected %f\n", idx, (double)h_d[idx], ref[idx]);
            }
            ++failures;
        }
        if (diff > worst) {
            worst = diff;
        }
    }

    coolFree(d_a);
    coolFree(d_b);
    coolFree(d_c);
    coolFree(d_d);

    if (failures != 0) {
        printf("mma FAILED: %d/%d elements mismatch (worst abs diff %g)\n", failures, M * N,
               worst);
        return 1;
    }
    printf("mma OK: %d elements verified (worst abs diff %g)\n", M * N, worst);
    return 0;
}
