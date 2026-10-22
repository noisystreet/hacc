// 阶段 2（C11 前半）e2e：短路求值 && / || 与指针复合赋值 p += i / p -= i。
//
// **短路不是优化差异，而是语义**：左侧已决定结果时，右侧的副作用必须不执行。
// 因此断言分两层：
//   1. 结果值 r1..r5；
//   2. 副作用计数 c1..c5 —— 若把 && 退化成按位 and，右侧会多跑一次，
//      c1/c2 立刻从 0 变 1。只断言结果值会漏掉这类错误（值恰好也是对的）。
//
// 两个刻意的写法：
//   - z / o 由**主机传入**而不是写成 `const int z = 0`：字面量会被 LLVM 常量折叠，
//     短路分支与副作用整段消失，断言就成了空转（B9-2 踩过这个坑）。
//   - 指针复合赋值同时写 `p[i]` 与 `*(p + 1)`，把 p+=/p-= 与二元指针算术放在
//     同一处对照 —— 两条路径都走 GEP，但入口不同（复合赋值 vs eok_padd）。
#include <cool/cool_runtime.h>

#include <stdio.h>

#define NOUT 16

// z 期望为 0、o 期望为 1（由主机传入，故都不是编译期常量）。
__global__ void k_short_circuit(int* out, int z, int o) {
    int c1 = 0, c2 = 0, c3 = 0, c4 = 0, c5 = 0;

    const int r1 = z && (c1 += 1);  // 左侧假 → 右侧不得执行
    const int r2 = o || (c2 += 1);  // 左侧真 → 右侧不得执行
    const int r3 = o && (c3 += 1);  // 左侧真 → 右侧必须执行
    const int r4 = z || (c4 += 1);  // 左侧假 → 右侧必须执行
    // 嵌套 land：验证内层建块后插入点回填正确（否则 LLVM 支配性检查失败）。
    const int r5 = (z < o) && (o > z) && (c5 += 1);

    out[0] = r1;
    out[1] = r2;
    out[2] = r3;
    out[3] = r4;
    out[4] = r5;
    out[5] = c1;
    out[6] = c2;
    out[7] = c3;
    out[8] = c4;
    out[9] = c5;
}

// step 由主机传入（=2）：**故意用运行时步长**，否则 LLVM 会把常量步长
// 折叠成 store 的立即数位移（`[%rd+4]`），PTX 里就看不到 GEP 的索引缩放了 ——
// golden 断言 `mul.wide` / `add.s64` 会变成空转。
__global__ void k_ptr_compound(int* out, int n, int step) {
    int* p = out;
    p += step;         // → out + 2
    p -= 1;            // → out + 1
    p[0] = n;          // out[1] = n
    *(p + 1) = n + 1;  // out[2] = n + 1（二元指针算术，与上面复合赋值对照）
    p -= 1;            // → out + 0
    p[0] = n + 2;      // out[0] = n + 2
}

int main() {
    int* d = nullptr;
    // 无可用 CUDA 设备时 coolMalloc 失败：打印 SKIP 并返回 77。
    if (coolMalloc((void**)&d, sizeof(int) * NOUT) != coolSuccess) {
        printf("logicalPtr SKIP: no usable CUDA device\n");
        return 77;
    }

    int h[NOUT];
    for (int i = 0; i < NOUT; ++i) {
        h[i] = -1;
    }

    // ---- 短路求值 ----
    coolMemcpy(d, h, sizeof(h), coolMemcpyHostToDevice);
    k_short_circuit<<<1, 1>>>(d, 0, 1);
    coolDeviceSynchronize();
    coolMemcpy(h, d, sizeof(h), coolMemcpyDeviceToHost);

    // r1=0 r2=1 r3=1 r4=1 r5=1
    // c1=0（短路！） c2=0（短路！） c3=1 c4=1 c5=1
    const int want_sc[10] = {0, 1, 1, 1, 1, 0, 0, 1, 1, 1};
    for (int i = 0; i < 10; ++i) {
        if (h[i] != want_sc[i]) {
            printf("logicalPtr FAIL[short-circuit]: out[%d] = %d, want %d\n", i, h[i],
                   want_sc[i]);
            coolFree(d);
            return 1;
        }
    }

    // ---- 指针复合赋值 ----
    for (int i = 0; i < NOUT; ++i) {
        h[i] = -1;
    }
    coolMemcpy(d, h, sizeof(h), coolMemcpyHostToDevice);
    k_ptr_compound<<<1, 1>>>(d, 40, 2);
    coolDeviceSynchronize();
    coolMemcpy(h, d, sizeof(h), coolMemcpyDeviceToHost);

    // out[0] = n+2 = 42（p 先 +2 再 -1 再 -1）
    // out[1] = n   = 40（p+=2 后 p-=1）
    // out[2] = n+1 = 41（*(p+1)，此时 p 指向 out+1）
    if (h[0] != 42 || h[1] != 40 || h[2] != 41) {
        printf("logicalPtr FAIL[ptr-compound]: out[0..2] = %d,%d,%d, want 42,40,41\n", h[0],
               h[1], h[2]);
        coolFree(d);
        return 1;
    }

    coolFree(d);
    printf("logicalPtr OK: && / || short-circuit + ptr compound assign verified\n");
    return 0;
}
