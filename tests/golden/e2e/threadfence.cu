// M4.3 golden/e2e 输入：__threadfence / __threadfence_block / __threadfence_system。
//
// 设备侧映射（见 m4-subset-s1-plan.md §3.4）：
//   __threadfence        → llvm.nvvm.membar.gl   → PTX membar.gl
//   __threadfence_block  → llvm.nvvm.membar.cta  → PTX membar.cta
//   __threadfence_system → llvm.nvvm.membar.sys  → PTX membar.sys
//
// 主机侧用例用「1 block × 1 thread」保证结果确定：栅栏本身不改变单线程语义，
// 故 out[0] 必然等于 g_seq 的终值 2。
//
// 说明：跨块可见性（真正的 __threadfence 语义）需要多 block 竞争窗口，测试难以稳定
// 复现，故 golden 只校验 PTX 指令，e2e 只校验编译产物可加载执行且语义未变。
#include <cool/cool_runtime.h>

#include <stdio.h>

#define N 8

// 设备全局：栅栏前后都要访问，确保 fence 不被优化掉且顺序合理。
__device__ int g_seq = 0;

__global__ void fence_kernel(int* out) {
    int t = threadIdx.x;
    if (t == 0) {
        g_seq = g_seq + 2;
        // 三种栅栏依次发射（PTX 层应出现 membar.gl / membar.cta / membar.sys）。
        __threadfence();
        __threadfence_block();
        __threadfence_system();
        out[0] = g_seq;
    }
}

int main() {
    int* d_out = nullptr;
    // 无可用 CUDA 设备时 coolMalloc 失败：返回 77（SKIP 约定），供测试框架跳过。
    if (coolMalloc((void**)&d_out, sizeof(int) * N) != coolSuccess) {
        printf("threadfence SKIP: no usable CUDA device\n");
        return 77;
    }

    fence_kernel<<<1, 1>>>(d_out);
    coolDeviceSynchronize();

    int h_out[N];
    coolMemcpy(h_out, d_out, sizeof(int) * N, coolMemcpyDeviceToHost);

    // 校验：g_seq 初值 0，加 2 后应为 2。
    if (h_out[0] != 2) {
        printf("threadfence FAILED: out[0]=%d, expected 2\n", h_out[0]);
        coolFree(d_out);
        return 1;
    }
    printf("threadfence OK: out[0]=%d\n", h_out[0]);
    coolFree(d_out);
    return 0;
}
