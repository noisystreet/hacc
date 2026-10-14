// M3.1 e2e 输入：__shared__ 静态共享内存 + 数组下标（动态索引）。
//
// 内核：void shared_reduce(int* out) {
//     __shared__ int sdata[256];
//     int t = threadIdx.x;
//     sdata[t] = t;              // 每线程写自己的槽
//     __syncthreads();           // 块内同步
//     out[t] = sdata[255 - t];   // 跨线程读（验证共享语义）
// }
// 经 hacc 编译为可执行文件，在 coolrt 上跑通并校验结果（见 tests/CMakeLists.txt 的
// e2e_hacc_shared_reduce_run）。本机有 GPU（RTX 4060），可真验证。
//
// 子集约束（M3.1）：__shared__ 静态数组；数组下标 sdata[i]；无动态共享内存。
#include <cool/cool_runtime.h>

#include <stdio.h>

#define N 256

__global__ void shared_reduce(int* out) {
    __shared__ int sdata[N];
    int t = threadIdx.x;
    sdata[t] = t;
    __syncthreads();
    out[t] = sdata[N - 1 - t];
}

int main() {
    int h_out[N];
    for (int i = 0; i < N; ++i) {
        h_out[i] = -1;
    }

    int* d_out = nullptr;
    // 无可用 CUDA 设备时 coolMalloc 失败：返回 77（SKIP 约定），供测试框架跳过。
    if (coolMalloc((void**)&d_out, sizeof(int) * N) != coolSuccess) {
        printf("sharedReduce SKIP: no usable CUDA device\n");
        return 77;
    }

    shared_reduce<<<1, N>>>(d_out);

    coolMemcpy(h_out, d_out, sizeof(int) * N, coolMemcpyDeviceToHost);
    coolDeviceSynchronize();

    // 校验：out[i] 应为 N-1-i（跨线程读共享内存）。
    int failures = 0;
    for (int i = 0; i < N; ++i) {
        if (h_out[i] != N - 1 - i) {
            ++failures;
        }
    }
    if (failures != 0) {
        printf("sharedReduce FAILED: %d/%d mismatches\n", failures, N);
        return 1;
    }
    printf("sharedReduce OK: %d elements verified\n", N);
    return 0;
}
