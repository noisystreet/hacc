// B5 e2e：CUDA 风格的 dim3 构造写法 + 默认值语义。
//
// 为什么单列一个用例：CUDA 的 `dim3` 在 C++ 下带构造函数，用户普遍写
//   dim3 block(256);  dim3 grid((n + 255) / 256);
// 而 coolrt 早期是纯 C 聚合，这类写法编译不过。本用例锁住「构造式可用」，
// 同时确认设备侧 dim3 仍按 12 字节聚合正确传参（out[i] == i）。
//
// 注：用 <stdio.h> 而非 <cstdio>——后者拉进 libstdc++ 的 c++config.h，
//     EDG 不认其中的 __has_builtin(...)，属前端既有约束（与本用例无关）。
#include <cool/cool_runtime.h>

#include <stdio.h>

__global__ void fill(int* out) {
    if (threadIdx.x == 0) {
        out[blockIdx.x] = (int)blockIdx.x;
    }
}

int main() {
    const int n = 1000;

    // 构造式（CUDA 同款）；缺省维补 1。
    dim3 block(256);
    dim3 grid((n + 255) / 256);
    if (block.y != 1 || grid.y != 1 || grid.z != 1) {
        printf("dim3Ctor FAILED: 缺省维应为 1，实际 grid=(%u,%u,%u) block=(%u,%u,%u)\n", grid.x,
               grid.y, grid.z, block.x, block.y, block.z);
        return 1;
    }
    // 默认构造各维为 1（CUDA 语义，而非未初始化）。
    const dim3 def;
    if (def.x != 1 || def.y != 1 || def.z != 1) {
        printf("dim3Ctor FAILED: 默认构造应为 (1,1,1)，实际 (%u,%u,%u)\n", def.x, def.y, def.z);
        return 1;
    }

    int* d_out = nullptr;
    if (coolMalloc((void**)&d_out, sizeof(int) * 4) != coolSuccess) {
        printf("dim3Ctor SKIP: no usable CUDA device\n");
        return 77;
    }
    fill<<<grid, block>>>(d_out);
    const coolError_t err = coolDeviceSynchronize();
    int h[4] = {0, 0, 0, 0};
    coolMemcpy(h, d_out, sizeof(int) * 4, coolMemcpyDeviceToHost);
    coolFree(d_out);
    if (err != coolSuccess) {
        printf("dim3Ctor FAILED: sync error %d\n", (int)err);
        return 1;
    }
    // 设备侧 dim3 仍应 12 字节聚合传递：blockIdx.x 落到 out[blockIdx.x]。
    if (h[0] != 0 || h[1] != 1 || h[2] != 2 || h[3] != 3) {
        printf("dim3Ctor FAILED: out[0..3]=%d,%d,%d,%d\n", h[0], h[1], h[2], h[3]);
        return 1;
    }
    printf("dim3Ctor OK: grid=(%u,%u,%u) block=(%u,%u,%u) out[0..3]=0,1,2,3\n", grid.x, grid.y,
           grid.z, block.x, block.y, block.z);
    return 0;
}
