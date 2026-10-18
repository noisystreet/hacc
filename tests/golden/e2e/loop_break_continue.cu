// M5.2 e2e：break / continue 在真机上的语义校验。
//
// 为什么这个用例能抓住真 bug：
//   - break 若失效 → 元素被写过，`-1` 守不住；
//   - continue 若跳到循环头（而不是 increment）→ i 永不递增，**死循环**（测试挂住而非误判通过）；
//   - continue 若被当成 break → 后续元素全部漏写。
// 每个线程 i 序列为 tid, tid+NT, tid+2NT…，各线程写的下标互不重叠，无数据竞争。
#include <cool/cool_runtime.h>

#include <stdio.h>

#define N 64
#define NT 8

// 只写 i < 4 的元素。
__global__ void kern_break(int* out, int n) {
    for (int i = threadIdx.x; i < n; i = i + blockDim.x) {
        if (i >= 4) {
            break;
        }
        out[i] = i + 100;
    }
}

// 跳过 i == 4，其余照写。
__global__ void kern_continue(int* out, int n) {
    for (int i = threadIdx.x; i < n; i = i + blockDim.x) {
        if (i == 4) {
            continue;
        }
        out[i] = i + 200;
    }
}

static void fill_host(int* h, int n, int v) {
    for (int i = 0; i < n; ++i) {
        h[i] = v;
    }
}

int main() {
    int* d = nullptr;
    // 无可用 CUDA 设备时 coolMalloc 失败：打印 SKIP 并返回 77。
    if (coolMalloc((void**)&d, sizeof(int) * N) != coolSuccess) {
        printf("loopCtrl SKIP: no usable CUDA device\n");
        return 77;
    }

    int h[N];

    // ---- break：只有 0..3 被写 ----
    fill_host(h, N, -1);
    coolMemcpy(d, h, sizeof(h), coolMemcpyHostToDevice);
    kern_break<<<1, NT>>>(d, N);
    coolDeviceSynchronize();
    coolMemcpy(h, d, sizeof(h), coolMemcpyDeviceToHost);
    for (int i = 0; i < N; ++i) {
        const int want = (i < 4) ? (i + 100) : -1;
        if (h[i] != want) {
            printf("loopCtrl FAIL[break]: out[%d] = %d, want %d\n", i, h[i], want);
            coolFree(d);
            return 1;
        }
    }

    // ---- continue：跳过 4，其余全写 ----
    fill_host(h, N, -1);
    coolMemcpy(d, h, sizeof(h), coolMemcpyHostToDevice);
    kern_continue<<<1, NT>>>(d, N);
    coolDeviceSynchronize();
    coolMemcpy(h, d, sizeof(h), coolMemcpyDeviceToHost);
    for (int i = 0; i < N; ++i) {
        const int want = (i == 4) ? -1 : (i + 200);
        if (h[i] != want) {
            printf("loopCtrl FAIL[continue]: out[%d] = %d, want %d\n", i, h[i], want);
            coolFree(d);
            return 1;
        }
    }

    coolFree(d);
    printf("loopCtrl OK: break + continue verified (N=%d, threads=%d)\n", N, NT);
    return 0;
}
