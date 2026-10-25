// S2 T3 负向用例：需要 libdevice 精确实现的数学函数（sinf 等）必须**明确报错**，
// 不得静默降级成 `sin.approx.f32` —— sinf 与 __sinf 在 CUDA 里是两个精度档，
// 把前者当后者用是「悄悄降低精度」，比编不过更危险。
//
// 这里用**用户自己声明**的方式模拟 `#include <math.h>` 的情形（coolrt 故意不声明这些
// 函数，只声明能用的 16 个）。期望 hacc 报：
//   sinf 需要 libdevice 的精确实现，hacc 尚未内置；可改用近似内建（如 __sinf / __expf）…
#include <cool/cool_runtime.h>

float sinf(float x); /* 用户自行声明 / 来自 <math.h> */

__global__ void k(float* out, float x) {
    out[threadIdx.x] = sinf(x); /* ← 应在此报「需要 libdevice」 */
}
