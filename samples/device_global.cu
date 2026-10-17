// 示例：__device__ 设备全局变量，以及**跨 kernel 共享**。
//
// 演示：文件作用域 __device__ 变量（编译期初值）、多个 kernel 读写同一份设备全局。
// 编译运行：hacc device_global.cu -o device_global && ./device_global
//
// 关键语义：设备全局属于**模块**（= 一个编译单元），同一编译单元内的所有 kernel
// 共享同一份实例——所以 kernel A 的写入，kernel B 读得到。
// 本示例专门用「A 写、B 读」来演示这一点（该能力由 ADR 0007 修复后成立）。
//
// 注意：跨 block 的同步不能靠 __syncthreads()（它只同步一个 block）；
// 这里用「两个 kernel 顺序启动」来做同步点——同一流上的 kernel 按序执行。
#include <cool/cool_runtime.h>

#include <stdio.h>

__device__ int g_counter = 100;  // 编译期初值

// kernel A：改设备全局。
__global__ void bump(int* out) {
    g_counter = g_counter + 5;
    out[1] = g_counter;  // A 看到的终值
}

// kernel B：读 A 留下的终值（跨 kernel 可见性）。
__global__ void read_back(int* out) {
    out[0] = g_counter;
}

int main() {
    int* d_out = nullptr;
    // 无可用 CUDA 设备时 coolMalloc 失败：打印 SKIP 并返回 77。
    if (coolMalloc((void**)&d_out, sizeof(int) * 4) != coolSuccess) {
        printf("deviceGlobal SKIP: no usable CUDA device\n");
        return 77;
    }

    int h_out[4] = {0};
    coolMemcpy(d_out, h_out, sizeof(h_out), coolMemcpyHostToDevice);

    bump<<<1, 1>>>(d_out);
    read_back<<<1, 1>>>(d_out);
    coolDeviceSynchronize();

    coolMemcpy(h_out, d_out, sizeof(h_out), coolMemcpyDeviceToHost);
    coolFree(d_out);

    const int want = 105;  // 初值 100 + 5
    if (h_out[1] != want) {
        printf("deviceGlobal FAILED: kernel A 读到 %d, want %d\n", h_out[1], want);
        return 1;
    }
    if (h_out[0] != want) {
        printf("deviceGlobal FAILED: kernel B 读到 %d, want %d（跨 kernel 未共享？）\n",
               h_out[0], want);
        return 1;
    }
    printf("deviceGlobal OK: kernel A/B 均读到 %d\n", h_out[0]);
    return 0;
}
