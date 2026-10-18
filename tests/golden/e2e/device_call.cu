// M5.4 golden：自定义 __device__ 函数 → PTX .func + call.uni（独立函数，不是内联展开）。
//
// 覆盖四件事：
//   - 有返回值 + 标量参数（add2 / scale3）
//   - 无返回值 + 指针参数（setv：验证 .param .b64 传参）
//   - 前向引用（user 调用定义在它后面的 forward_use）
//   - 内核调用链（kern → add2 → scale3）
#include <cool/cool_runtime.h>

__device__ int add2(int a, int b) {
    return a + b;
}

__device__ int scale3(int x) {
    return x * 3;
}

__device__ void setv(int* p, int idx, int v) {
    p[idx] = v;
}

// 先声明、后定义：device_functions 必须在填 body 前就包含两个 routine，
// 否则 user 翻译时找不到 forward_use。
__device__ int forward_use(int x);

__device__ int user(int x) {
    return forward_use(x) + 1;
}

__device__ int forward_use(int x) {
    return x * 2;
}

__global__ void call_add_scale(int* out, int n) {
    int i = threadIdx.x;
    int s = add2(i, 1);
    out[i] = scale3(s);
}

__global__ void call_forward(int* out, int n) {
    int i = threadIdx.x;
    out[i] = user(i);
}

__global__ void call_void(int* out, int n) {
    setv(out, threadIdx.x, 777);
}
