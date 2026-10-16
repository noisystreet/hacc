// 示例：__constant__ 常量内存 + coolMemcpyToSymbol。
//
// 演示：文件作用域 __constant__ 变量、主机侧 coolMemcpyToSymbol 填充常量内存、
//       内核内读取常量内存。
// 编译运行：hacc constant_pi.cu -o constant_pi && ./constant_pi
#include <cool/cool_runtime.h>

#include <stdio.h>

#define N 64

// 文件作用域 __constant__ 变量（CUDA 仅允许文件作用域）。
__constant__ float c_scale;

__global__ void scale_kernel(float* out) {
    int t = threadIdx.x;
    out[t] = out[t] * c_scale;  // 读 __constant__ 变量
}

int main() {
    float h_out[N];
    for (int i = 0; i < N; ++i) {
        h_out[i] = 2.0f;
    }

    float* d_out = nullptr;
    // 无可用 CUDA 设备时 coolMalloc 失败：打印 SKIP 并返回 77。
    if (coolMalloc((void**)&d_out, sizeof(float) * N) != coolSuccess) {
        printf("constantPi SKIP: no usable CUDA device\n");
        return 77;
    }
    coolMemcpy(d_out, h_out, sizeof(float) * N, coolMemcpyHostToDevice);

    // 把 3.0 拷入 __constant__ c_scale（hacc 改写为 haccMemcpyToSymbol 包装）。
    float scale = 3.0f;
    coolMemcpyToSymbol(c_scale, &scale, sizeof(float));

    scale_kernel<<<1, N>>>(d_out);
    coolDeviceSynchronize();

    coolMemcpy(h_out, d_out, sizeof(float) * N, coolMemcpyDeviceToHost);

    // 校验：out[i] 应为 2.0 * 3.0 == 6.0。
    int failures = 0;
    for (int i = 0; i < N; ++i) {
        if (h_out[i] != 6.0f) {
            ++failures;
        }
    }
    if (failures != 0) {
        printf("constantPi FAILED: %d/%d mismatches\n", failures, N);
        return 1;
    }
    printf("constantPi OK: %d elements verified\n", N);
    return 0;
}
