// M5.4 前向引用的**数值**回归（此前只有 PTX 文本断言）。
//
// device_call.cu 覆盖了前向引用（先声明 → 中间调用 → 后面定义），但它**没有 main**，
// 只 grep PTX 里的 `.func` / `call.uni`，所以该场景从未真机验证过数值。
//
// 修 KI-5 时顺带查清了它此前的真实行为（用原始实现 `--emit=ptx` 实测）：
// PTX 里会出现**两个同名 `forward_use`** —— 一个是声明（以 `;` 结尾、无 body），
// 一个是有 body 的定义。原因是「声明」与「定义」在 IL 里是两条独立 routine 记录，
// 而 declare_routine 每次都 Function::Create 一个新对象。
// ptxas 恰好把调用解析到有 body 的那份，所以**数值本来是正确的**（实测 forwardRef OK）；
// 但重复符号本身是隐患，现已在 declare_device_functions 里让纯声明复用同名定义。
//
// 本用例断言 out[i] == i * 2 + 1，锁住「前向引用必须落到有定义那份」这一性质：
// 若将来重构退化成调到空函数（末尾兜底补 `ret 0`），就会得到恒 1 而在此失败。
#include <cool/cool_runtime.h>

#include <stdio.h>

#define N 8

__device__ int forward_use(int x); /* 只有声明 */

__device__ int user(int x) {
    return forward_use(x) + 1; /* ← 调用早于定义 */
}

__device__ int forward_use(int x) { /* 定义在后 */
    return x * 2;
}

__global__ void k(int* out) {
    out[threadIdx.x] = user(threadIdx.x);
}

int main() {
    int* d_out = nullptr;
    if (coolMalloc((void**)&d_out, sizeof(int) * N) != coolSuccess) {
        printf("forwardRef SKIP: no usable CUDA device\n");
        return 77;
    }

    k<<<1, N>>>(d_out);
    coolDeviceSynchronize();

    int h_out[N];
    coolMemcpy(h_out, d_out, sizeof(h_out), coolMemcpyDeviceToHost);
    coolFree(d_out);

    for (int i = 0; i < N; ++i) {
        const int want = i * 2 + 1;
        if (h_out[i] != want) {
            printf("forwardRef FAIL: out[%d] = %d, want %d\n", i, h_out[i], want);
            return 1;
        }
    }

    printf("forwardRef OK: %d elements, forward-declared device function resolves to its definition\n",
           N);
    return 0;
}
