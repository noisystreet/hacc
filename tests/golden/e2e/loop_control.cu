// M5.2 golden：break / continue（EDG 降为 stmk_goto + stmk_label）→ PTX 跳转结构。
//
// 三个内核覆盖 goto/label 的三种典型形态：
//   - for + break    → 前向跳到循环结束（与正常退出块合并）
//   - for + continue → 跳到 increment 块（**不能**跳循环头，否则 i 不增、死循环）
//   - while + break  → 同上，但循环形态不同
// 断言见 tests/CMakeLists.txt；语义正确性由 e2e loop_break_continue 在真机上校验。
#include <cool/cool_runtime.h>

__global__ void ctrl_break(int* out, int n) {
    for (int i = threadIdx.x; i < n; i = i + blockDim.x) {
        if (i >= 4) {
            break;
        }
        out[i] = i;
    }
}

__global__ void ctrl_continue(int* out, int n) {
    for (int i = threadIdx.x; i < n; i = i + blockDim.x) {
        if (i == 4) {
            continue;
        }
        out[i] = i;
    }
}

__global__ void ctrl_while_break(int* out, int n) {
    int i = threadIdx.x;
    while (i < n) {
        if (i > 2) {
            break;
        }
        out[i] = i;
        i = i + 1;
    }
}
