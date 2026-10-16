// 示例：向量加法 c[i] = a[i] + b[i]（基础内核）。
//
// 演示：指针参数、threadIdx.x 索引、<<<grid, block>>> 启动、主机/设备内存拷贝与结果校验。
// 编译运行：hacc vector_add.cu -o vector_add && ./vector_add
#include <cool/cool_runtime.h>

#include <stdio.h>

#define N 256

__global__ void vector_add(const int* a, const int* b, int* c) {
    int i = threadIdx.x;
    // 子集约束：左值仅支持变量与 *p，故用 *(p + i) 而非 p[i]。
    *(c + i) = *(a + i) + *(b + i);
}

int main() {
    int h_a[N];
    int h_b[N];
    int h_c[N];
    for (int i = 0; i < N; ++i) {
        h_a[i] = i;
        h_b[i] = 2 * i;
        h_c[i] = -1;
    }

    int* d_a = nullptr;
    int* d_b = nullptr;
    int* d_c = nullptr;
    // 无可用 CUDA 设备时 coolMalloc 失败：打印 SKIP 并返回 77。
    if (coolMalloc((void**)&d_a, sizeof(int) * N) != coolSuccess ||
        coolMalloc((void**)&d_b, sizeof(int) * N) != coolSuccess ||
        coolMalloc((void**)&d_c, sizeof(int) * N) != coolSuccess) {
        printf("vectorAdd SKIP: no usable CUDA device\n");
        return 77;
    }
    coolMemcpy(d_a, h_a, sizeof(int) * N, coolMemcpyHostToDevice);
    coolMemcpy(d_b, h_b, sizeof(int) * N, coolMemcpyHostToDevice);

    // 启动 1 个 block、N 个线程；每个线程处理一个元素。
    vector_add<<<1, N>>>(d_a, d_b, d_c);

    coolMemcpy(h_c, d_c, sizeof(int) * N, coolMemcpyDeviceToHost);
    coolDeviceSynchronize();

    // 校验：c[i] 应为 3*i。
    int failures = 0;
    for (int i = 0; i < N; ++i) {
        if (h_c[i] != 3 * i) {
            ++failures;
        }
    }
    if (failures != 0) {
        printf("vectorAdd FAILED: %d/%d mismatches\n", failures, N);
        return 1;
    }
    printf("vectorAdd OK: %d elements verified\n", N);
    return 0;
}
