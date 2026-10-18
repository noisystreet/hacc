// M5.4 e2e：自定义 __device__ 函数在真机上的语义校验。
//
// 两个用例分别钉住两条最容易写错的路径：
//   - 有返回值的调用链（add2 → scale3）：期望 (i+1)*3，能同时验证
//     参数传递、返回值回填、连续两次 call
//   - 无返回值 + 指针参数的调用（setv）：期望全部被写成 777，
//     能验证 .param .b64 指针传参（写错地址会崩或写偏）
#include <cool/cool_runtime.h>

#include <stdio.h>

#define N 32
#define NT 32

__device__ int add2(int a, int b) {
    return a + b;
}

__device__ int scale3(int x) {
    return x * 3;
}

__device__ void setv(int* p, int idx, int v) {
    p[idx] = v;
}

__global__ void kern_chain(int* out, int n) {
    int i = threadIdx.x;
    int s = add2(i, 1);
    out[i] = scale3(s);
}

__global__ void kern_void(int* out, int n) {
    setv(out, threadIdx.x, 777);
}

int main() {
    int* d = nullptr;
    // 无可用 CUDA 设备时 coolMalloc 失败：打印 SKIP 并返回 77。
    if (coolMalloc((void**)&d, sizeof(int) * N) != coolSuccess) {
        printf("deviceCall SKIP: no usable CUDA device\n");
        return 77;
    }

    int h[N];

    // ---- 有返回值：out[i] = (i + 1) * 3 ----
    for (int i = 0; i < N; ++i) {
        h[i] = -1;
    }
    coolMemcpy(d, h, sizeof(h), coolMemcpyHostToDevice);
    kern_chain<<<1, NT>>>(d, N);
    coolDeviceSynchronize();
    coolMemcpy(h, d, sizeof(h), coolMemcpyDeviceToHost);
    for (int i = 0; i < N; ++i) {
        const int want = (i + 1) * 3;
        if (h[i] != want) {
            printf("deviceCall FAIL[chain]: out[%d] = %d, want %d\n", i, h[i], want);
            coolFree(d);
            return 1;
        }
    }

    // ---- 无返回值 + 指针参数：全部写成 777 ----
    for (int i = 0; i < N; ++i) {
        h[i] = -1;
    }
    coolMemcpy(d, h, sizeof(h), coolMemcpyHostToDevice);
    kern_void<<<1, NT>>>(d, N);
    coolDeviceSynchronize();
    coolMemcpy(h, d, sizeof(h), coolMemcpyDeviceToHost);
    for (int i = 0; i < N; ++i) {
        if (h[i] != 777) {
            printf("deviceCall FAIL[void]: out[%d] = %d, want 777\n", i, h[i]);
            coolFree(d);
            return 1;
        }
    }

    coolFree(d);
    printf("deviceCall OK: chained calls + void fn with pointer param verified (N=%d)\n", N);
    return 0;
}
