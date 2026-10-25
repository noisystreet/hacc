// KI-5 正向用例：**只声明、从不调用**的设备函数不应报错。
//
// 为什么必须保留这个方向：coolrt 将来会成批声明数学内建（sqrtf / sinf / ...），而用户
// 只会用到其中少数几个。若「无定义即报错」，任何 #include 都会炸——那是把修复做成了新缺陷。
// 所以拦截点放在**调用**这一层，而不是声明。
//
// 期望：hacc 编译成功，且 PTX 里**不出现**这些未定义函数的空函数体
// （修复前会生成 `.visible .func sqrtf/cosf` 并返回 0）。
#include <cool/cool_runtime.h>

__device__ float sqrtf(float); /* 声明但不用 */
__device__ float cosf(float);  /* 声明但不用 */

__global__ void k(float* out) {
    out[threadIdx.x] = 1.0f;
}
