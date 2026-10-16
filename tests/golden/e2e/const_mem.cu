// M3.2 e2e 输入：__constant__ 常量内存 + coolMemcpyToSymbol。
//
// 内核：void scale_kernel(float* out) {
//     int t = threadIdx.x;
//     out[t] = out[t] * c_scale;   // 读 __constant__ 变量
// }
// 主机：coolMemcpyToSymbol(c_scale, &scale, sizeof(float)) 把 3.0 写入常量内存，
// 启动内核后校验 out[i] == 2.0 * 3.0 == 6.0。
// 经 hacc 编译为可执行文件，在 coolrt 上跑通并校验结果（见 tests/CMakeLists.txt 的
// e2e_hacc_const_mem_run）。本机有 GPU（RTX 4060），可真验证。
//
// 子集约束（M3.2）：__constant__ 仅文件作用域；标量类型；coolMemcpyToSymbol 拷贝。
#include <cool/cool_runtime.h>

#include <stdio.h>

#define N 64

__constant__ float c_scale;

__global__ void scale_kernel(float* out) {
    int t = threadIdx.x;
    out[t] = out[t] * c_scale;
}

int main() {
    float h_out[N];
    for (int i = 0; i < N; ++i) {
        h_out[i] = 2.0f;
    }

    float* d_out = nullptr;
    // 无可用 CUDA 设备时 coolMalloc 失败：返回 77（SKIP 约定），供测试框架跳过。
    if (coolMalloc((void**)&d_out, sizeof(float) * N) != coolSuccess) {
        printf("constMem SKIP: no usable CUDA device\n");
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
        printf("constMem FAILED: %d/%d mismatches\n", failures, N);
        return 1;
    }
    printf("constMem OK: %d elements verified\n", N);
    return 0;
}
