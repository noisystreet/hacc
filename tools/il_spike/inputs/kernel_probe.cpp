// IL spike 探针：观测 EDG IL 是否保留「内核 / 共享数组 / 同步点」等关键信息。
//
// 说明：EDG 不识别 CUDA 专有语法（__global__ / <<<>>> ）。这里给出的是
//       「coolfe++ 预改写后」的等价 C++ 表示，便于 cpfe 成功解析并导出 IL。
//       地址空间 / __shared__ / __constant__ 的可保留表达（EDG 命名地址空间或属性）
//       是本次 spike 要回答的问题，先用最朴素形态验证内核与调用的 IL 形态。
struct dim3 {
    unsigned x, y, z;
};

extern dim3 threadIdx;  // 对应 CUDA 内建 threadIdx
extern dim3 blockIdx;   // 对应 CUDA 内建 blockIdx
extern dim3 blockDim;   // 对应 CUDA 内建 blockDim
void __cool_syncthreads();  // 对应 __syncthreads()

extern "C" void saxpy(int n, float a, const float* x, float* y) {
    float tile[256];  // 对应 __shared__ float tile[256]
    const int i = static_cast<int>(threadIdx.x) +
                  static_cast<int>(blockIdx.x) * static_cast<int>(blockDim.x);
    if (i < n) {
        tile[i % 256] = a * x[i];
        __cool_syncthreads();
        y[i] = tile[i % 256] + y[i];
    }
}
