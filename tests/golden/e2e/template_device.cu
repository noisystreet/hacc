// S2 T1 e2e：模板 `__device__` 函数。
//
// 覆盖两件事：
//   1. 隐式推导（`twice(*(out + i))` → `<float>`）与显式实参（`twice<int>(i)`）都能翻；
//   2. 两个实例**同名不同签名**，靠 hacc 的签名唯一化得到 `twice__float_float` /
//      `twice__i32_i32`，不依赖 LLVM 自动改名（twice / twice1）。
//
// 反向验证（改坏会怎样）：
//   - 去掉 `src/frontend/edg_invoke.cpp` 里的 `--instantiate=used`：EDG 只把实例写成
//     声明（function_def_number == 0），coolicc 在调用点报「调用了只有声明、没有定义的
//     设备函数 twice」，编译阶段即失败；
//   - 把下面的期望值改成 `5.0f * i`：本用例必须报 mismatches（实测 63/64）。
#include <cool/cool_runtime.h>

#include <stdio.h>

#define N 64

template <typename T>
__device__ T twice(T v) {
    return v + v;
}

__global__ void k_tmpl(float* out) {
    int i = threadIdx.x;
    float f = twice(*(out + i));  // 隐式推导 <float>
    int g = twice<int>(i);        // 显式实参 <int>
    *(out + i) = f + (float)g;
}

int main() {
    float h_a[N];
    float h_c[N];
    for (int i = 0; i < N; ++i) {
        h_a[i] = (float)i;
        h_c[i] = -1.0f;
    }

    float* d = nullptr;
    if (coolMalloc((void**)&d, sizeof(float) * N) != coolSuccess) {
        printf("tmplDevice SKIP: no usable CUDA device\n");
        return 77;
    }
    coolMemcpy(d, h_a, sizeof(float) * N, coolMemcpyHostToDevice);
    k_tmpl<<<1, N>>>(d);
    coolMemcpy(h_c, d, sizeof(float) * N, coolMemcpyDeviceToHost);
    coolDeviceSynchronize();

    // twice(out[i]) = 2i，twice<int>(i) = 2i → 期望 4i。
    int failures = 0;
    for (int i = 0; i < N; ++i) {
        const float want = 4.0f * (float)i;
        if (h_c[i] != want) {
            ++failures;
        }
    }
    if (failures != 0) {
        printf("tmplDevice FAILED: %d/%d mismatches (h_c[1]=%f want 4)\n", failures, N, h_c[1]);
        return 1;
    }
    printf("tmplDevice OK: %d elements verified\n", N);
    return 0;
}
