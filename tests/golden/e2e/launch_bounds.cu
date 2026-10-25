// S2 T4 e2e：__launch_bounds__ 被「接受但忽略」——带该提示的 kernel 能编、能跑、结果正确。
//
// hacc 不把 __launch_bounds__ 映射到 PTX `.maxntid` / `.minnctapersm`：设备端目前 -O0、
// 不做寄存器压力优化，映射无收益。本用例锁定的契约是「带该提示不改变行为」。
//
// 反向验证：删掉 coolrt `include/cool/device_builtins.h` 里的
// `#define __launch_bounds__(...)` 后，本用例会在**编译阶段**直接失败
// （coolfe++ 报 `incomplete type "void" is not allowed`），而不是静默跑错。
#include <cool/cool_runtime.h>

#include <stdio.h>

#define N 64

// 双参形式（maxThreadsPerBlock, minBlocksPerMultiprocessor）
__global__ void __launch_bounds__(256, 4) lb_two(int* out) {
    int i = threadIdx.x;
    if (i < N) {
        out[i] = i * 3 + 1;
    }
}

// 单参形式（只有 maxThreadsPerBlock）
__global__ void __launch_bounds__(512) lb_one(int* out) {
    int i = threadIdx.x;
    if (i < N) {
        out[i] = out[i] + 100;
    }
}

int main() {
    int* d = nullptr;
    // 无可用 CUDA 设备时 coolMalloc 失败：打印 SKIP 并返回 77。
    if (coolMalloc((void**)&d, sizeof(int) * N) != coolSuccess) {
        printf("launchBounds SKIP: no usable CUDA device\n");
        return 77;
    }

    int h[N];
    for (int i = 0; i < N; ++i) {
        h[i] = -1;
    }
    coolMemcpy(d, h, sizeof(h), coolMemcpyHostToDevice);

    // 两个 kernel 串联：第二个读第一个的写入，能顺带验证跨 kernel 的设备内存可见性。
    lb_two<<<1, N>>>(d);
    lb_one<<<1, N>>>(d);
    coolDeviceSynchronize();
    coolMemcpy(h, d, sizeof(h), coolMemcpyDeviceToHost);

    for (int i = 0; i < N; ++i) {
        const int want = i * 3 + 1 + 100;
        if (h[i] != want) {
            printf("launchBounds FAIL: out[%d] = %d, want %d\n", i, h[i], want);
            coolFree(d);
            return 1;
        }
    }

    coolFree(d);
    printf("launchBounds OK: %d elements, both __launch_bounds__ forms accepted and ignored\n", N);
    return 0;
}
