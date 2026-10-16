// 示例：dim3 作为内核参数（3 分量聚合）。
//
// 演示：dim3 类型按值传入内核、成员访问 d.x/d.y/d.z。
// 背景：3 分量类型（dim3 / uint3 / float3）映射为 struct {T,T,T}（12 字节，对齐 4），
//       而非 <3 x T>——PTX 无 v3 加载，且 <3 x T> 会被 padding 到 16 字节，破坏 ABI。
// 编译运行：hacc dim3_grid.cu -o dim3_grid && ./dim3_grid
#include <cool/cool_runtime.h>

#include <stdio.h>

__global__ void dim3_kernel(dim3 d, int* out) {
    *out = (int)(d.x + d.y + d.z);
}

int main() {
    dim3 h_d = {1, 2, 3};
    int h_out = -1;

    int* d_out = nullptr;
    // 无可用 CUDA 设备时 coolMalloc 失败：打印 SKIP 并返回 77。
    if (coolMalloc((void**)&d_out, sizeof(int)) != coolSuccess) {
        printf("dim3Grid SKIP: no usable CUDA device\n");
        return 77;
    }

    dim3_kernel<<<1, 1>>>(h_d, d_out);

    coolMemcpy(&h_out, d_out, sizeof(int), coolMemcpyDeviceToHost);
    coolDeviceSynchronize();

    // 校验：1 + 2 + 3 = 6。
    if (h_out != 6) {
        printf("dim3Grid FAILED: got %d, want 6\n", h_out);
        return 1;
    }
    printf("dim3Grid OK: %d\n", h_out);
    return 0;
}
