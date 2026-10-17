// M4.6 边界用例：未覆盖的原子类型 → 编译期报错（fail loud）。
//
// 一期原子覆盖 int / unsigned int / float（见 m4-subset-s1-plan.md §3.5）。
// double 在 CUDA 里要 sm_60+ 且有专门的重载，hacc 一期不提供，故不应有匹配重载：
// coolrt 的 device_builtins.h 只声明上述三种，`atomicAdd(double*, double)` 在
// **前端**就因无匹配重载而报错——这是期望行为，不是缺陷。
//
// 该用例的价值：锁住「类型泛化」不会被悄悄放开。若将来有人给头文件加了 double 重载
// 却没在翻译层实现，本用例会失败，提醒补翻译 + 补测试，而不是静默产出错误代码。
#include <cool/cool_runtime.h>

__device__ double g_d = 0.0;

__global__ void bad_atomic_type(int* out) {
    atomicAdd(&g_d, 1.0);  // 无匹配重载（第 15 行触发诊断）
    out[0] = 1;
}
