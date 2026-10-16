// 示例：向量类型（float4）作为内核参数 + 成员访问。
//
// 演示：POD 聚合参数（float4）按值传递、成员访问 a.x/a.y/a.z/a.w。
// 编译运行：hacc vector_type.cu -o vector_type && ./vector_type
#include <cool/cool_runtime.h>
#include <cool/device_builtins.h>

#include <stdio.h>

__global__ void add4(float4 a, float4 b, float* out) {
    float s = (a.x + b.x) + (a.y + b.y) + (a.z + b.z) + (a.w + b.w);
    *out = s;
}

int main() {
    float4 h_a = {1.0f, 2.0f, 3.0f, 4.0f};
    float4 h_b = {10.0f, 20.0f, 30.0f, 40.0f};
    float h_out = -1.0f;

    float* d_out = nullptr;
    // 无可用 CUDA 设备时 coolMalloc 失败：打印 SKIP 并返回 77。
    if (coolMalloc((void**)&d_out, sizeof(float)) != coolSuccess) {
        printf("vectorType SKIP: no usable CUDA device\n");
        return 77;
    }

    // 聚合参数按值传入（hacc 映射为 <4 x float> 向量参数）。
    add4<<<1, 1>>>(h_a, h_b, d_out);

    coolMemcpy(&h_out, d_out, sizeof(float), coolMemcpyDeviceToHost);
    coolDeviceSynchronize();

    // 校验：1+10 + 2+20 + 3+30 + 4+40 = 110。
    if (h_out != 110.0f) {
        printf("vectorType FAILED: got %f, want 110.0\n", h_out);
        return 1;
    }
    printf("vectorType OK: %f\n", h_out);
    return 0;
}
