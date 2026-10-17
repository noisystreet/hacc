// M4.5 边界用例：cool_address_space 取值非法 → 编译期报错（fail loud）。
//
// 合法取值只有 NVPTX 的 0/1/3/4/5（generic/global/shared/constant/local）。
// 写错（如 2 或 7）必须报错而不是静默当成 generic——地址空间错会让访存落到
// 错误的存储层次，是那种「能跑但结果/性能诡异」的隐蔽 bug。
#include <cool/cool_runtime.h>

__global__ void bad_address_space(int* out) {
    __shared__ int sdata[8];
    // 2 不是 NVPTX 地址空间（第 8 行触发诊断）
    int __attribute__((cool_address_space(2)))* p = sdata;
    out[0] = p[0];
}
