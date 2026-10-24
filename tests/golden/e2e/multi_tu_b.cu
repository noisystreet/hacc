// 多 TU 用例（C3）之 B：自带内核与 main，并通过 extern 调用 A 里的主机函数。
// 两个 TU 各自一个 module（ADR 0007），互不干扰；main 只能在其中一个 TU。
#include <cool/cool_runtime.h>

#include <stdio.h>

// A 里定义的主机入口（普通 C++ 函数，跨 TU 调用）。
extern void run_a(int* d);

__global__ void k_b(int* out) { out[0] = 22; }

static void run_b(int* d) { k_b<<<1, 1>>>(d); }

int main() {
    int* d1 = nullptr;
    int* d2 = nullptr;
    if (coolMalloc((void**)&d1, sizeof(int)) != coolSuccess ||
        coolMalloc((void**)&d2, sizeof(int)) != coolSuccess) {
        printf("multiTU SKIP: 无法分配设备内存（无可用的 CUDA 设备）\n");
        return 77;
    }
    int h1 = 0;
    int h2 = 0;
    coolMemset(d1, 0, sizeof(int));
    coolMemset(d2, 0, sizeof(int));
    run_a(d1);
    run_b(d2);
    coolDeviceSynchronize();
    coolMemcpy(&h1, d1, sizeof(int), coolMemcpyDeviceToHost);
    coolMemcpy(&h2, d2, sizeof(int), coolMemcpyDeviceToHost);
    coolFree(d1);
    coolFree(d2);

    if (h1 != 11 || h2 != 22) {
        printf("multiTU FAILED: 期望 11/22，实际 %d/%d\n", h1, h2);
        return 1;
    }
    printf("multiTU OK: A=%d B=%d（两个 TU 各自的 module）\n", h1, h2);
    return 0;
}
