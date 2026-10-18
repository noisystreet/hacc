// M5.3 golden：switch → 比较链 + case 块（含 case 合并、fallthrough、default）。
//
// 三个内核覆盖 switch 的关键形态：
//   - 多 case 共用 body（case 1 / case 2 落到同一块）
//   - 故意不写 break → fallthrough 到下一个 case
//   - 无 default（全部落空直接结束）vs 有 default
// 语义正确性由 e2e switch_dispatch 在真机上校验。
#include <cool/cool_runtime.h>

__global__ void switch_basic(int* out, int n) {
    int i = threadIdx.x;
    switch (i) {
        case 0:
            out[i] = 10;
            break;
        case 1:
        case 2:
            out[i] = 20;
            break;
        default:
            out[i] = 30;
            break;
    }
}

__global__ void switch_fallthrough(int* out, int n) {
    int i = threadIdx.x;
    int v = 0;
    switch (i) {
        case 0:
            v = v + 1;  // 故意不 break
        case 1:
            v = v + 10;
            break;
        default:
            v = 99;
    }
    out[i] = v;
}

__global__ void switch_no_default(int* out, int n) {
    int i = threadIdx.x;
    int v = -1;
    switch (i) {
        case 0:
            v = 1;
            break;
        case 1:
            v = 2;
            break;
    }
    out[i] = v;
}
