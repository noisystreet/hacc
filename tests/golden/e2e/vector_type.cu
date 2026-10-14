// M3.0 e2e 输入：向量类型（float4）作为内核参数（聚合参数 byval）+ 成员访问。
//
// 内核：void add4(float4 a, float4 b, float* out) { *out = a.x+b.x + a.y+b.y + a.z+b.z + a.w+b.w; }
// 经 hacc 编译为可执行文件，在 coolrt 上跑通并校验结果（见 tests/CMakeLists.txt 的
// e2e_hacc_vector_type_run）。本机有 GPU（RTX 4060），可真验证。
//
// 子集约束（M3.0）：内核参数为 POD 聚合（float4）；成员访问 a.x 等；无 __shared__。
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
    // 无可用 CUDA 设备时 coolMalloc 失败：返回 77（SKIP 约定），供测试框架跳过。
    if (coolMalloc((void**)&d_out, sizeof(float)) != coolSuccess) {
        printf("vectorType SKIP: no usable CUDA device\n");
        return 77;
    }

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
