// C12 / MMA golden：cool_wmma_* 内建 → llvm.nvvm.wmma.* → PTX wmma.* 指令。
//
// 只校验**编译路径**（PTX 指令指纹 + ptxas 汇编）：数值正确性由 A4 的真机 e2e 负责，
// 因为 mma 的 24 个实参同型，A/B 传反、fragment 内部错位都能编过、只有数值错。
//
// 覆盖全部 5 条内建：load.a / load.b / load.c / mma / store.d（出参 ABI）。
// 刻意**不**把调用包进 `if` —— mma 是 warp 级语义，32 个 lane 必须一致执行到调用点。
#include <cool/cool_runtime.h>

__global__ void mma_kernel(float* a, float* b, float* c, float* d) {
    cool_wmma_frag_a fa;
    cool_wmma_frag_b fb;
    cool_wmma_frag_c fc;
    cool_wmma_frag_d fd;

    cool_wmma_load_a_row(&fa, a);
    cool_wmma_load_b_col(&fb, b);
    cool_wmma_load_c_row(&fc, c);
    cool_wmma_mma(&fd, &fa, &fb, &fc);
    cool_wmma_store_d_row(d, &fd);
}
