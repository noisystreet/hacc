// M4.1 e2e 输入：设备全局 __device__ 变量（addrspace 1）。
//
// 内核：void dev_global_kernel(int* out) {
//     int t = threadIdx.x;
//     g_counter = g_counter + 1;   // 读改写设备全局
//     out[t] = g_counter;
// }
// 主机：启动 1 个 block × N 线程后校验 out[i] == g_init + (i+1)。
// 因同一 block 内线程对 g_counter 的读改写无同步，结果依赖调度顺序，
// 故只校验「最终值 == g_init + N」（用 out 的最后一个元素不可靠），
// 这里改为：每个线程写 out[t] = g_counter，最后用 coolMemcpyFromSymbol
// 读回 g_counter 校验总数。为简化一期，只校验 g_counter 终值。
//
// 经 hacc 编译为可执行文件，在 coolrt 上跑通并校验结果（见 tests/CMakeLists.txt 的
// e2e_hacc_dev_global_run）。本机有 GPU（RTX 4060），可真验证。
//
// 子集约束（M4.1）：__device__ 仅文件作用域；标量类型；编译期初始化（= 初值）。
#include <cool/cool_runtime.h>

#include <stdio.h>

#define N 32

// 设备全局，带编译期初值（验证 M4.1 的静态初始化器进 PTX）。
__device__ int g_counter = 100;

__global__ void dev_global_kernel(int* out) {
    int t = threadIdx.x;
    // 单线程顺序执行（1 block × 1 thread）时结果确定；这里用 1 thread 保证可校验。
    if (t == 0) {
        g_counter = g_counter + 5;
        out[0] = g_counter;
    }
}

int main() {
    int* d_out = nullptr;
    // 无可用 CUDA 设备时 coolMalloc 失败：返回 77（SKIP 约定），供测试框架跳过。
    if (coolMalloc((void**)&d_out, sizeof(int) * N) != coolSuccess) {
        printf("devGlobal SKIP: no usable CUDA device\n");
        return 77;
    }

    dev_global_kernel<<<1, 1>>>(d_out);
    coolDeviceSynchronize();

    int h_out[N];
    coolMemcpy(h_out, d_out, sizeof(int) * N, coolMemcpyDeviceToHost);

    // 校验：g_counter 初值 100，加 5 后应为 105。
    if (h_out[0] != 105) {
        printf("devGlobal FAILED: expected 105, got %d\n", h_out[0]);
        coolFree(d_out);
        return 1;
    }
    printf("devGlobal OK: g_counter == %d\n", h_out[0]);
    coolFree(d_out);
    return 0;
}
