// M4.6 边界用例：__device__ 变量声明在**函数内** → 编译期报错（fail loud）。
//
// CUDA 中 __device__ 变量只能位于文件作用域；写在函数里会让「设备全局」这个名字
// 失去意义（无法跨内核/跨调用存活，也无法与 PTX 的 .global 对应）。
// hacc 选择报错而不是静默降级为局部变量——后者会让用户以为自己在用设备全局。
#include <cool/cool_runtime.h>

__global__ void bad_inner_device_var(int* out) {
    __device__ int inner = 0;  // 错误位置（第 10 行触发诊断）
    out[0] = inner;
}
