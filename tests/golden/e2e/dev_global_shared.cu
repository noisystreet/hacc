// KI-1 / ADR 0007 e2e 输入：设备全局变量**跨 kernel 共享**。
//
// 这是一个回归测试：修复前必然失败（每个 kernel 各载入一份 module，`.global` 各一份副本，
// 后一个 kernel 读到的是模块初值而非前一个 kernel 的写入）。修复后（模块粒度 = TU）通过。
//
// 覆盖两个维度：
//   1. 竞争累加：writer 用 128 线程 atomicAdd，reader 读回，应为 128（而非初值 0）。
//   2. 交叉读写：两个 kernel 各写**不同**的全局，第三个 kernel 一起读，断言互不覆盖、互不丢失
//      （防止「只共享一个变量」或「后写覆盖前写」这类伪修复）。
//
// 注意：kernel 之间的顺序由默认流保证；本用例不依赖块间同步。
#include <cool/cool_runtime.h>

#include <stdio.h>

#define NTHREADS 128

__device__ int g_counter = 0;  // writer 累加，reader 读取
__device__ int g_a = 0;        // 交叉读写用例
__device__ int g_b = 0;

__global__ void writer(int* out) {
    atomicAdd(&g_counter, 1);
}

__global__ void reader(int* out) {
    out[0] = g_counter;
}

__global__ void write_a(int* out) {
    g_a = 11;
}

__global__ void write_b(int* out) {
    g_b = 22;
}

__global__ void read_both(int* out) {
    out[1] = g_a;
    out[2] = g_b;
}

int main() {
    int* d_out = nullptr;
    // 无可用 CUDA 设备时 coolMalloc 失败：返回 77（SKIP 约定），供测试框架跳过。
    if (coolMalloc((void**)&d_out, sizeof(int) * 8) != coolSuccess) {
        printf("devGlobalShared SKIP: no usable CUDA device\n");
        return 77;
    }

    int h_out[8] = {0};
    coolMemcpy(d_out, h_out, sizeof(h_out), coolMemcpyHostToDevice);

    writer<<<1, NTHREADS>>>(d_out);
    reader<<<1, 1>>>(d_out);

    write_a<<<1, 1>>>(d_out);
    write_b<<<1, 1>>>(d_out);
    read_both<<<1, 1>>>(d_out);

    coolDeviceSynchronize();
    coolMemcpy(h_out, d_out, sizeof(h_out), coolMemcpyDeviceToHost);
    coolFree(d_out);

    // 逐项校验，失败时打印具体项：能区分「跨 kernel 完全不可见」（0）与「部分共享」。
    struct {
        const char* what;
        int got;
        int want;
    } checks[] = {
        {"跨 kernel 竞争累加（writer → reader）", h_out[0], NTHREADS},
        {"g_a 跨 kernel 可见（write_a → read_both）", h_out[1], 11},
        {"g_b 跨 kernel 可见（write_b → read_both）", h_out[2], 22},
    };

    int failed = 0;
    for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); ++i) {
        if (checks[i].got != checks[i].want) {
            printf("devGlobalShared FAILED: %s: got %d, want %d\n", checks[i].what, checks[i].got,
                   checks[i].want);
            failed = 1;
        }
    }
    if (failed) {
        return 1;
    }
    printf("devGlobalShared OK: counter=%d a=%d b=%d\n", h_out[0], h_out[1], h_out[2]);
    return 0;
}
