// 示例：用 atomicAdd 做块内归约（原子操作作用在设备全局变量上）。
//
// 演示：__device__ 全局 + atomicAdd 并发累加 + __syncthreads() 收尾导出。
// 编译运行：hacc atomic_reduce.cu -o atomic_reduce && ./atomic_reduce
//
// 为什么必须用原子：N 个线程同时执行 `g_sum = g_sum + t` 会丢更新（读-改-写之间的
// 竞争），结果依赖调度顺序、必然小于期望值。atomicAdd 把这三步做成一条不可分割的
// 指令，故终值与线程执行顺序无关。
#include <cool/cool_runtime.h>

#include <stdio.h>

#define NTHREADS 128

__device__ int g_sum = 0;

__global__ void atomic_reduce(int* out) {
    int t = threadIdx.x;
    atomicAdd(&g_sum, t);  // 并发累加 0 + 1 + ... + (NTHREADS-1)
    __syncthreads();       // 块内同步：确保所有线程的累加都已生效
    if (t == 0) {
        out[0] = g_sum;    // 由 0 号线程导出终值
    }
}

int main() {
    int* d_out = nullptr;
    // 无可用 CUDA 设备时 coolMalloc 失败：打印 SKIP 并返回 77。
    if (coolMalloc((void**)&d_out, sizeof(int) * 4) != coolSuccess) {
        printf("atomicReduce SKIP: no usable CUDA device\n");
        return 77;
    }

    int h_out[4] = {0};
    coolMemcpy(d_out, h_out, sizeof(h_out), coolMemcpyHostToDevice);

    atomic_reduce<<<1, NTHREADS>>>(d_out);
    coolDeviceSynchronize();

    coolMemcpy(h_out, d_out, sizeof(h_out), coolMemcpyDeviceToHost);
    coolFree(d_out);

    // 等差数列求和，与线程调度顺序无关（这正是原子的意义）。
    const int want = NTHREADS * (NTHREADS - 1) / 2;
    if (h_out[0] != want) {
        printf("atomicReduce FAILED: got %d, want %d\n", h_out[0], want);
        return 1;
    }
    printf("atomicReduce OK: sum(0..%d) = %d\n", NTHREADS - 1, h_out[0]);
    return 0;
}
