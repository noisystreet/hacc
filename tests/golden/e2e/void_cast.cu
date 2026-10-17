// 回归输入：`(void)expr` —— C++ 的「丢弃值」转换。
//
// 曾经的缺陷：translate_cast 把它当成真实类型转换，生成非法 IR
//   `sext ptr %2 to void` / `trunc i32 %2 to void`
// → LLVM 报 "void type only allowed for function results"，编译直接失败。
// 而 `(void)x;` 是抑制「未使用变量/参数」警告的常见写法，用户很容易撞上。
//
// 语义要点（本用例的断言核心）：`(void)e` **只丢弃值，不丢弃副作用**。
// 故 `(void)atomicAdd(&g_n, 3)` 里的原子操作必须真的执行（g_n 必须变成 3）；
// 若修复方式是「整条语句忽略」，本用例会失败。
//
// 覆盖三种操作数类型：指针（原报错形态）、整型、有副作用的调用返回值。
#include <cool/cool_runtime.h>

#include <stdio.h>

__device__ int g_n = 0;

__global__ void void_cast_kernel(int* out) {
    (void)out;  // 指针 → void（原 `sext ptr ... to void`）
    int v = 7;
    (void)v;                   // 整数 → void（原 `trunc i32 ... to void`）
    (void)atomicAdd(&g_n, 3);  // 丢弃返回值，但原子副作用必须保留

    out[0] = v;
    out[1] = g_n;
}

int main() {
    int* d_out = nullptr;
    // 无可用 CUDA 设备时 coolMalloc 失败：返回 77（SKIP 约定），供测试框架跳过。
    if (coolMalloc((void**)&d_out, sizeof(int) * 4) != coolSuccess) {
        printf("voidCast SKIP: no usable CUDA device\n");
        return 77;
    }

    int h_out[4] = {0};
    coolMemcpy(d_out, h_out, sizeof(h_out), coolMemcpyHostToDevice);

    void_cast_kernel<<<1, 1>>>(d_out);
    coolDeviceSynchronize();

    coolMemcpy(h_out, d_out, sizeof(h_out), coolMemcpyDeviceToHost);
    coolFree(d_out);

    int failed = 0;
    // 值未被 void 转换破坏。
    if (h_out[0] != 7) {
        printf("voidCast FAILED: v 被破坏: got %d, want 7\n", h_out[0]);
        failed = 1;
    }
    // 副作用被保留（关键断言）：丢弃返回值不等于丢弃调用。
    if (h_out[1] != 3) {
        printf("voidCast FAILED: (void)atomicAdd 的副作用丢失: g_n got %d, want 3\n", h_out[1]);
        failed = 1;
    }
    if (failed) {
        return 1;
    }
    printf("voidCast OK: v=%d g_n=%d\n", h_out[0], h_out[1]);
    return 0;
}
