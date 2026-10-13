// M1.2.4 探针：内建变量（threadIdx/blockIdx/blockDim/gridDim）+ __syncthreads()。
// 期望：dump 出真实 IL，核对内建变量的声明/引用形态与 __syncthreads 的调用形态。
//
// 说明：EDG 不认识 CUDA 内建，故此处以「普通全局变量 + 结构体」模拟其形态，
// 目的是观察「结构体成员访问（threadIdx.x）」与「函数调用（__syncthreads）」在 IL 中
// 分别长什么样——这正是 M1.2.4 翻译器要识别的两种构造。

struct uint3 {
    unsigned int x;
    unsigned int y;
    unsigned int z;
};

// 模拟 CUDA 内建变量（真实 CUDA 中由 cuda_runtime 头提供，带 __device__ 限定）。
extern uint3 threadIdx;
extern uint3 blockIdx;
extern uint3 blockDim;
extern uint3 gridDim;

// 模拟 __syncthreads（真实 CUDA 中为内建函数）。
void __syncthreads();

__attribute__((cool_global)) void tid_kernel(int* out) {
    int t = threadIdx.x;
    int b = blockIdx.x;
    __syncthreads();
    *out = t + b;
}
