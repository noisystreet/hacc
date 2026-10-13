// M1.2.4 golden 输入：内建变量（threadIdx/blockIdx）+ __syncthreads()。
// 期望：coolfe++ 产出 .cil，coolicc --from-il 产出含 %tid.x / %ctaid.x 与 barrier0 的 PTX。
//
// 使用运行时头（COOLRuntime 的 device_builtins.h）提供 __global__ 宏与内建变量/函数；
// 由 coolfe++ 定义 __COOLCC__ 使其生效（对标 nvcc 的 __CUDACC__）。
#include <cool/cool_runtime.h>

__global__ void tid_kernel(int* out) {
    int t = threadIdx.x;
    int b = blockIdx.x;
    __syncthreads();
    *out = t + b;
}
