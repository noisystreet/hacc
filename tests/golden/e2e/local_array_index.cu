// M3.5 边界用例（正向）：局部数组 + 动态下标（a[i]）。
//
// 回归保护：曾因数组退化的 GEP 源类型误用元素类型而产出非法 LLVM IR
// （getelementptr i32, ptr %a, i64 0, i64 0）。修复后应能正常编译为 PTX。
// 期望：hacc --emit=ptx 成功，PTX 含内核入口与局部数组的地址计算。
#include <cool/cool_runtime.h>

__global__ void local_array_index(int* out) {
    int a[4];
    int i = threadIdx.x;
    a[i] = 1;
    *out = a[i];
}
