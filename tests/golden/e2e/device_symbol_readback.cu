// e2e：__device__ 变量的主机侧访问（cudaMemcpyFromSymbol / ToSymbol / GetSymbolAddress）。
//
// 覆盖点：
//   1. 主机侧经 cudaMemcpyFromSymbol 读回内核改过的 __device__ 变量；
//   2. cudaGetSymbolAddress 取到的地址与 FromSymbol 读写的是同一块内存；
//   3. 主机侧经 cudaMemcpyToSymbol 写回 __device__ 变量。
//
// 无可用 CUDA 设备时返回 77（由测试框架按 SKIP 处理）。
#include <cool/cool_runtime.h>
#include <stdio.h>

__device__ int g_counter = 7;

__global__ void bump(int* out) {
    int t = threadIdx.x;
    atomicAdd(&g_counter, 1);
    out[t] = g_counter;
}

int main() {
    int* d = nullptr;
    if (coolMalloc((void**)&d, sizeof(int)) != coolSuccess) {
        printf("coolMalloc 失败\n");
        return 77;  // 无设备 → SKIP
    }
    bump<<<1, 1>>>(d);
    if (coolDeviceSynchronize() != coolSuccess) {
        printf("coolDeviceSynchronize 失败\n");
        coolFree(d);
        return 1;
    }

    // 1) 读回：初始 7，内核 +1 → 8。
    int from_symbol = 0;
    if (cudaMemcpyFromSymbol(&from_symbol, g_counter, sizeof(int), 0,
                             coolMemcpyDeviceToHost) != coolSuccess) {
        printf("cudaMemcpyFromSymbol 失败\n");
        coolFree(d);
        return 1;
    }
    if (from_symbol != 8) {
        printf("FromSymbol 读回错误：%d != 8\n", from_symbol);
        coolFree(d);
        return 1;
    }

    // 2) 取址交叉验证：同一块内存。
    void* addr = nullptr;
    if (cudaGetSymbolAddress(&addr, g_counter) != coolSuccess || addr == nullptr) {
        printf("cudaGetSymbolAddress 失败\n");
        coolFree(d);
        return 1;
    }
    int via_addr = 0;
    if (coolMemcpy(&via_addr, addr, sizeof(int), coolMemcpyDeviceToHost) != coolSuccess ||
        via_addr != 8) {
        printf("取址读回错误：%d != 8\n", via_addr);
        coolFree(d);
        return 1;
    }

    // 3) 写回：ToSymbol 后经 FromSymbol 应读到新值。
    const int new_value = 100;
    if (cudaMemcpyToSymbol(g_counter, &new_value, sizeof(int), 0,
                           coolMemcpyHostToDevice) != coolSuccess) {
        printf("cudaMemcpyToSymbol 失败\n");
        coolFree(d);
        return 1;
    }
    int after_wrap = 0;
    if (cudaMemcpyFromSymbol(&after_wrap, g_counter, sizeof(int), 0,
                             coolMemcpyDeviceToHost) != coolSuccess ||
        after_wrap != new_value) {
        printf("ToSymbol 写回错误：%d != %d\n", after_wrap, new_value);
        coolFree(d);
        return 1;
    }

    printf("deviceSymbolReadback OK\n");
    coolFree(d);
    return 0;
}
