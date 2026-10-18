// M5.3 e2e：switch 在真机上的语义校验（case 合并 / fallthrough / default）。
//
// 这个用例的价值在于把三种最容易写错的形态一次性钉住：
//   - case 1 与 case 2 共用 body → 两者都得 20
//   - case 3 故意不 break → 必须 fallthrough 到 case 4，得 3 + 40 = 43
//     （若 fallthrough 丢失，会得到 3；若 case 4 也走错，会得到 40）
//   - i >= 5 落到 default → 99（若 default 缺失或跳错，会保留 -1）
#include <cool/cool_runtime.h>

#include <stdio.h>

#define N 32
#define NT 32

__global__ void kern_switch(int* out, int n) {
    int i = threadIdx.x;
    int v = 0;
    switch (i) {
        case 0:
            v = 10;
            break;
        case 1:
        case 2:
            v = 20;
            break;
        case 3:
            v = 3;  // 故意不 break → fallthrough
        case 4:
            v = v + 40;
            break;
        default:
            v = 99;
    }
    out[i] = v;
}

int main() {
    int* d = nullptr;
    // 无可用 CUDA 设备时 coolMalloc 失败：打印 SKIP 并返回 77。
    if (coolMalloc((void**)&d, sizeof(int) * N) != coolSuccess) {
        printf("switchDispatch SKIP: no usable CUDA device\n");
        return 77;
    }

    int h[N];
    for (int i = 0; i < N; ++i) {
        h[i] = -1;
    }
    coolMemcpy(d, h, sizeof(h), coolMemcpyHostToDevice);

    kern_switch<<<1, NT>>>(d, N);
    coolDeviceSynchronize();
    coolMemcpy(h, d, sizeof(h), coolMemcpyDeviceToHost);

    for (int i = 0; i < N; ++i) {
        int want;
        if (i == 0) {
            want = 10;
        } else if (i == 1 || i == 2) {
            want = 20;
        } else if (i == 3) {
            want = 43;  // fallthrough: 3 + 40
        } else if (i == 4) {
            want = 40;
        } else {
            want = 99;  // default
        }
        if (h[i] != want) {
            printf("switchDispatch FAIL: out[%d] = %d, want %d\n", i, h[i], want);
            coolFree(d);
            return 1;
        }
    }

    coolFree(d);
    printf("switchDispatch OK: %d elements verified (cases 0-4 + default)\n", N);
    return 0;
}
