// 示例：`<<<grid, block, sharedMem, stream>>>` 四参数启动 + cool* 流 API 混用（S1 最后一项）。
//
// 演示三件事：
//   1. 默认流与自定义流交替启动同一内核（"混用"的核心场景）
//   2. coolMalloc / coolMemcpy（内存 API）与 coolStreamCreate / Synchronize / Destroy（流 API）并存
//   3. 四参数启动语法：第 3 参 sharedMem、第 4 参 stream
// 编译运行：hacc stream_launch.cu -o stream_launch && ./stream_launch
#include <cool/cool_runtime.h>

#include <stdio.h>

#define N 128

__global__ void fill(int* out, int base) {
    int i = threadIdx.x;
    *(out + i) = base + i;
}

int main() {
    int* d = nullptr;
    // 无可用 CUDA 设备时 coolMalloc 失败：打印 SKIP 并返回 77。
    if (coolMalloc((void**)&d, sizeof(int) * N) != coolSuccess) {
        printf("streamLaunch SKIP: no usable CUDA device\n");
        return 77;
    }

    coolStream_t stream = nullptr;
    if (coolStreamCreate(&stream) != coolSuccess) {
        printf("streamLaunch SKIP: coolStreamCreate failed\n");
        coolFree(d);
        return 77;
    }

    int h[N];

    // ---- 第 1 轮：默认流（两参数形式）----
    fill<<<1, N>>>(d, 0);
    coolDeviceSynchronize();
    coolMemcpy(h, d, sizeof(int) * N, coolMemcpyDeviceToHost);
    int failures = 0;
    for (int i = 0; i < N; ++i) {
        if (h[i] != i) {
            ++failures;
        }
    }
    if (failures != 0) {
        printf("streamLaunch FAILED[default stream]: %d/%d mismatches\n", failures, N);
        coolStreamDestroy(stream);
        coolFree(d);
        return 1;
    }

    // ---- 第 2 轮：同一内核换到自定义流（四参数形式，sharedMem=0）----
    fill<<<1, N, 0, stream>>>(d, 100);
    coolError_t sync = coolStreamSynchronize(stream);
    if (sync != coolSuccess) {
        printf("streamLaunch FAILED: coolStreamSynchronize -> %s\n", coolGetErrorString(sync));
        coolStreamDestroy(stream);
        coolFree(d);
        return 1;
    }
    coolMemcpy(h, d, sizeof(int) * N, coolMemcpyDeviceToHost);
    failures = 0;
    for (int i = 0; i < N; ++i) {
        if (h[i] != 100 + i) {
            ++failures;
        }
    }
    if (failures != 0) {
        printf("streamLaunch FAILED[user stream]: %d/%d mismatches\n", failures, N);
        coolStreamDestroy(stream);
        coolFree(d);
        return 1;
    }

    // ---- 第 3 轮：流可复用（再启动一次，验证 stream 句柄未失效）----
    fill<<<1, N, 0, stream>>>(d, 200);
    coolStreamSynchronize(stream);
    coolMemcpy(h, d, sizeof(int) * N, coolMemcpyDeviceToHost);
    failures = 0;
    for (int i = 0; i < N; ++i) {
        if (h[i] != 200 + i) {
            ++failures;
        }
    }
    if (failures != 0) {
        printf("streamLaunch FAILED[stream reuse]: %d/%d mismatches\n", failures, N);
        coolStreamDestroy(stream);
        coolFree(d);
        return 1;
    }

    coolStreamDestroy(stream);
    coolFree(d);
    printf("streamLaunch OK: default stream + 2 user-stream launches, %d elements\n", N);
    return 0;
}
