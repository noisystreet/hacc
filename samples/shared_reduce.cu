// 示例：__shared__ 静态共享内存 + __syncthreads()。
//
// 演示：块内共享内存数组、动态下标 sdata[i]、块内同步 __syncthreads()。
// 编译运行：hacc shared_reduce.cu -o shared_reduce && ./shared_reduce
#include <cool/cool_runtime.h>

#include <stdio.h>

#define N 256

__global__ void shared_reduce(int* out) {
    __shared__ int sdata[N];
    int t = threadIdx.x;
    sdata[t] = t;              // 每线程写自己的槽
    __syncthreads();           // 块内同步：确保所有写完成
    out[t] = sdata[N - 1 - t]; // 跨线程读（验证共享语义）
}

int main() {
    int h_out[N];
    for (int i = 0; i < N; ++i) {
        h_out[i] = -1;
    }

    int* d_out = nullptr;
    // 无可用 CUDA 设备时 coolMalloc 失败：打印 SKIP 并返回 77。
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
