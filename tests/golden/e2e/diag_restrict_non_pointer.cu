// M4.2 诊断用例：__restrict__ 用在非指针形参上 → 编译期报错（fail loud）。
//
// CUDA 中 __restrict__ 只对指针有意义；误标会让 LLVM 得到错误的 noalias 前提，
// 可能引入错误优化，故 hacc 一律报错而非静默忽略。
#include <cool/cool_runtime.h>

__global__ void bad_restrict_kernel(int __restrict__ v) {
    // 空内核：错误在翻译形参时触发（第 7 行）。
}
