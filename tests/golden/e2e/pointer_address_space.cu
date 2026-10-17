// M4.5 golden/e2e 输入：cool_address_space(n) 指针地址空间传播。
//
// 演示两件事：
//   1. 指针变量标注 cool_address_space(3) 后，经它读写的访存是 `.shared` 而非 generic；
//   2. 未标注（默认 generic）的指针行为不变。
//
// 为什么需要它：未做 M4.5 前，`int* p = sdata;` 声明的指针是 generic（addrspace(0)），
// 经 p 的读写落成 generic `ld` / `st`（需额外 cvta 与运行时地址判断），
// 而 LLVM 拿不到「这是共享内存」的信息，也就无法选 .shared 指令。
//
// 该用例同时供 golden PTX 断言（见 tests/CMakeLists.txt）与 e2e 运行校验。
#include <cool/cool_runtime.h>

#include <stdio.h>

#define N 32

__global__ void pointer_address_space(int* out) {
    __shared__ int sdata[N];
    int t = threadIdx.x;

    // 经 shared 指针写（应落 st.shared）
    int __attribute__((cool_address_space(3)))* sp = sdata;
    sp[t] = t * 2;
    __syncthreads();

    // 经 shared 指针读（应落 ld.shared），再写到 global（st.global）
    out[t] = sp[N - 1 - t];
}

int main() {
    int* d_out = nullptr;
    if (coolMalloc((void**)&d_out, sizeof(int) * N) != coolSuccess) {
        printf("pointerAddressSpace SKIP: no usable CUDA device\n");
        return 77;
    }

    pointer_address_space<<<1, N>>>(d_out);
    coolDeviceSynchronize();

    int h_out[N];
    coolMemcpy(h_out, d_out, sizeof(int) * N, coolMemcpyDeviceToHost);
    coolFree(d_out);

    // 期望 out[t] == (N-1-t)*2，校验经 shared 指针的往返读写正确。
    int failures = 0;
    for (int i = 0; i < N; ++i) {
        if (h_out[i] != (N - 1 - i) * 2) {
            ++failures;
        }
    }
    if (failures != 0) {
        printf("pointerAddressSpace FAILED: %d/%d mismatches\n", failures, N);
        return 1;
    }
    printf("pointerAddressSpace OK: %d elements via addrspace(3) pointer\n", N);
    return 0;
}
