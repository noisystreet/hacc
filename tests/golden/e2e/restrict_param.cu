// M4.2 golden/e2e 输入：__restrict__ 指针形参 → LLVM noalias。
//
// 内核：void axpy(int* __restrict__ out, const int* __restrict__ in, int n)
//   —— out 与 in 声明为互不重叠（restrict），设备侧降为参数 noalias 属性。
// 主机：正常执行，校验结果与无 restrict 时一致（noalias 是优化提示，不改语义）。
//
// __restrict__ 在设备编译下由 coolrt 的 device_builtins.h 映射为 cool_restrict 属性
// （见 coolruntime/include/cool/device_builtins.h）。
#include <cool/cool_runtime.h>

#include <stdio.h>

#define N 64

// M4.2：指针形参带 __restrict__ → LLVM noalias（在 --emit=llvm-ir 下可见）。
__global__ void axpy(int* __restrict__ out, const int* __restrict__ in) {
    int t = threadIdx.x;
    out[t] = in[t] + 1;
}

int main() {
    int* d_out = nullptr;
    int* d_in = nullptr;
    // 无可用 CUDA 设备时 coolMalloc 失败：返回 77（SKIP 约定），供测试框架跳过。
    if (coolMalloc((void**)&d_out, sizeof(int) * N) != coolSuccess ||
        coolMalloc((void**)&d_in, sizeof(int) * N) != coolSuccess) {
        printf("restrict SKIP: no usable CUDA device\n");
        return 77;
    }

    int h_in[N];
    for (int i = 0; i < N; ++i) {
        h_in[i] = i;
    }
    coolMemcpy(d_in, h_in, sizeof(int) * N, coolMemcpyHostToDevice);

    axpy<<<1, N>>>(d_out, d_in);
    coolDeviceSynchronize();

    int h_out[N];
    coolMemcpy(h_out, d_out, sizeof(int) * N, coolMemcpyDeviceToHost);

    // 校验：out[i] == in[i] + 1 == i + 1。
    int failures = 0;
    for (int i = 0; i < N; ++i) {
        if (h_out[i] != i + 1) {
            ++failures;
        }
    }
    if (failures != 0) {
        printf("restrict FAILED: %d/%d mismatches\n", failures, N);
        coolFree(d_out);
        coolFree(d_in);
        return 1;
    }
    printf("restrict OK: %d elements verified\n", N);
    coolFree(d_out);
    coolFree(d_in);
    return 0;
}
