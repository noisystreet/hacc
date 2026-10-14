// M3.3 golden 输入：dim3 作为内核参数（3 分量聚合 → struct {i32,i32,i32}）。
//
// 内核：void dim3_kernel(dim3 d, int* out) { *out = d.x + d.y + d.z; }
// 期望：coolfe++ 产出 .cil，coolicc --from-il 产出 PTX，其中 dim3 参数按
//   .param .align 4 .b8 dim3_kernel_param_0[12]（12 字节，对齐 4）传递，
//   成员访问 d.x/d.y/d.z 走 struct GEP + ld.param.u32（非向量化 v2/v4）。
//
// 背景（M3.3）：3 分量类型（dim3 / uint3 / float3）映射为 struct 而非 <3 x T>——
// PTX 无 v3 加载，且 <3 x T> 会被 padding 到 16 字节，破坏 12 字节 ABI。
#include <cool/cool_runtime.h>

__global__ void dim3_kernel(dim3 d, int* out) {
    *out = (int)(d.x + d.y + d.z);
}
