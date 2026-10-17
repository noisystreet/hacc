// 示例：__threadfence 设备级内存栅栏。
//
// 演示：__threadfence() / __threadfence_block() / __threadfence_system() 的用法位置。
// 编译运行：hacc threadfence.cu -o threadfence && ./threadfence
//
// 三种栅栏的作用域（PTX 层分别落 membar.gl / membar.cta / membar.sys）：
//   __threadfence_block() —— 本 block 内可见性排序（`.cta`）
//   __threadfence()       —— 全设备可见性排序（`.gl`）
//   __threadfence_system()—— 含主机内存（`.sys`）
//
// 它们保证的是**顺序**，不是同步：栅栏之前的写不会排到栅栏之后，但它**不等待**
// 其他线程，也不提供任何「谁先谁后」的保证。真正需要互相等待时，块内用
// __syncthreads()，跨 kernel 用「顺序启动 + 流」。
//
// 本示例因此用 1 block × 1 thread，结果与栅栏无关地确定（out[0] 必为 2）：
// 目的是演示栅栏**该写在哪儿**，而不是用单线程去证明并发正确性——跨块可见性
// 需要多 block 竞争窗口，单机测试很难稳定复现。
#include <cool/cool_runtime.h>

#include <stdio.h>

#define N 8

__device__ int g_seq = 0;

__global__ void fence_demo(int* out) {
    int t = threadIdx.x;
    if (t == 0) {
        g_seq = g_seq + 2;
        __threadfence();         // 先保证上面的写对全设备有序
        __threadfence_block();   // 再收窄到块内作用域演示
        __threadfence_system();  // 最后演示含主机内存的作用域
        out[0] = g_seq;
    }
}

int main() {
    int* d_out = nullptr;
    // 无可用 CUDA 设备时 coolMalloc 失败：打印 SKIP 并返回 77。
    if (coolMalloc((void**)&d_out, sizeof(int) * N) != coolSuccess) {
        printf("threadfence SKIP: no usable CUDA device\n");
        return 77;
    }

    int h_out[N] = {0};
    coolMemcpy(d_out, h_out, sizeof(h_out), coolMemcpyHostToDevice);

    fence_demo<<<1, 1>>>(d_out);
    coolDeviceSynchronize();

    coolMemcpy(h_out, d_out, sizeof(h_out), coolMemcpyDeviceToHost);
    coolFree(d_out);

    if (h_out[0] != 2) {
        printf("threadfence FAILED: out[0]=%d, want 2\n", h_out[0]);
        return 1;
    }
    printf("threadfence OK: out[0]=%d（栅栏不改变单线程语义）\n", h_out[0]);
    return 0;
}
