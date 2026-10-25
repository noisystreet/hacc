// KI-5 负向用例：调用「只有声明、没有定义」的设备函数，必须**在调用点报错**。
//
// 曾经的缺陷行为（见 docs/known-issues.md KI-5）：hacc 把它编译成一个返回 0 的空函数体
// ——PTX 里是 `.visible .func` 而非 `.extern .func`：
//     mov.f32 %f1, 0f00000000;
//     st.param.f32 [func_retval0+0], %f1;
// 于是编译、链接、运行全部成功，结果恒为 0，属纯静默错值。
//
// 期望：hacc 退出码非 0，stderr 含「只有声明、没有定义的设备函数 sqrtf」，
// 且位置指向下方 `out[threadIdx.x] = sqrtf(x);` 那一行的调用点（而非声明处）。
#include <cool/cool_runtime.h>

__device__ float sqrtf(float); /* 只有声明，没有任何实现 */

__global__ void k(float* out, float x) {
    out[threadIdx.x] = sqrtf(x); /* ← 调用点：应在此处报错 */
}
