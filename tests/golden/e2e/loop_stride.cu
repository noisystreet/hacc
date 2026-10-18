// M5.1 e2e：内核内 stride for 循环在真机上跑通并校验结果。
//
// 关键点：N(1024) > 线程数(128)，每个线程必须迭代 8 次。若循环降级错误
// （只跑一次 / 条件写反 / 步进丢失），元素会被漏写而保留 -1，主机侧校验即可发现。
// 这类「能编译、看着跑通、结果不对」的缺陷只有真机 e2e 能抓（见 m4-handoff §5.3）。
#include <cool/cool_runtime.h>

#include <stdio.h>

#define N 1024
#define NT 128

__global__ void stride_scale(int* out, int n) {
    for (int i = threadIdx.x; i < n; i = i + blockDim.x) {
        out[i] = i * 2;
    }
}

int main() {
    int* d_out = nullptr;
    // 无可用 CUDA 设备时 coolMalloc 失败：打印 SKIP 并返回 77。
    if (coolMalloc((void**)&d_out, sizeof(int) * N) != coolSuccess) {
        printf("loopStride SKIP: no usable CUDA device\n");
        return 77;
    }

    int h_out[N];
    for (int i = 0; i < N; ++i) {
        h_out[i] = -1;
    }
    coolMemcpy(d_out, h_out, sizeof(h_out), coolMemcpyHostToDevice);

    stride_scale<<<1, NT>>>(d_out, N);
    coolDeviceSynchronize();
    coolMemcpy(h_out, d_out, sizeof(h_out), coolMemcpyDeviceToHost);

    for (int i = 0; i < N; ++i) {
        if (h_out[i] != i * 2) {
            printf("loopStride FAIL: out[%d] = %d, want %d\n", i, h_out[i], i * 2);
            coolFree(d_out);
            return 1;
        }
    }

    coolFree(d_out);
    printf("loopStride OK: %d elements verified (N=%d, threads=%d)\n", N, N, NT);
    return 0;
}
