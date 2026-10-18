// B9-2 golden/e2e 输入：复合赋值（+= -= *= /= %= <<= >>= &= |= ^=）与逻辑非 !。
//
// 为什么单独一份：这两类语法此前都编译不过，而它们的失败信息都是
// 「不支持的运算符」，无法从错误里区分是哪个算子出问题，故逐项落断言。
//
// e2e 策略：
//   - 单 block 单线程，结果完全确定。
//   - **每一步中间结果都写进 out[]**：若只写最终值，像 `a + ~a` 这类可被
//     代数化简的表达式会让 LLVM 把整条依赖链折叠掉（spike 里就发生过：
//     整个 kernel 被压成一条 `mov -1`），断言就测不到实际生成的指令。
//   - 算子之间用互相独立的初值，避免一处算错牵连后面全部失败。
#include <cool/cool_runtime.h>

#include <stdio.h>

__global__ void compound_kernel(int* out) {
    int t = threadIdx.x;
    if (t != 0) {
        return;
    }

    // --- 算术复合赋值 ---
    int a = 10;
    a += 5;
    out[1] = a; // 15
    a -= 3;
    out[2] = a; // 12
    a *= 2;
    out[3] = a; // 24
    a /= 4;
    out[4] = a; // 6
    a %= 4;
    out[5] = a; // 2

    // --- 位运算复合赋值 ---
    int b = 0xF0;
    b &= 0x3C;
    out[6] = b; // 0x30 = 48
    b |= 0x03;
    out[7] = b; // 0x33 = 51
    b ^= 0x0F;
    out[8] = b; // 0x3C = 60
    b <<= 2;
    out[9] = b; // 0xF0 = 240
    b >>= 4;
    out[10] = b; // 0x0F = 15

    // 有符号右移：负数必须走算术右移（保留符号位）。
    int neg = -16;
    neg >>= 2;
    out[11] = neg; // -4

    // --- 逻辑非 ---
    // 这里最需要盯的是「0 与非 0 两个方向都要对」：
    // 按位取反的旧实现会让 !0 得 -1、!5 得 -6，两者都非零，看似「总是真」。
    int z = 0;
    int nz = 5;
    out[12] = !z;   // 1
    out[13] = !nz;  // 0
    out[14] = !!nz; // 1（双重否定回原真值）
    out[15] = !!z;  // 0

    // --- bool 变量与条件 ---
    bool flag = (a == 2);
    out[16] = flag; // 1
    int cnt = 0;
    if (!flag) {
        cnt += 100;
    }
    if (flag) {
        cnt += 1;
    }
    out[17] = cnt; // 1（只走 flag 为真的分支）

    // --- 浮点复合赋值 ---
    float f = 1.5f;
    f += 2.5f;
    out[18] = (int)(f * 10.0f); // 4.0*10 = 40
    f *= 2.0f;
    out[19] = (int)(f * 10.0f); // 8.0*10 = 80

    // --- 复合赋值表达式的值 = 新值（不是旧值，也不是右操作数）---
    // a 此时为 2，r = (a += 8) - 10 = 10 - 10 = 0，之后 a = 10。
    int r = (a += 8) - 10;
    out[20] = r; // 0
    out[21] = a; // 10
}

int main() {
    int* d_out = nullptr;
    // 无可用 CUDA 设备时 coolMalloc 失败：返回 77（SKIP 约定），供测试框架跳过。
    if (coolMalloc((void**)&d_out, sizeof(int) * 24) != coolSuccess) {
        printf("compound SKIP: no usable CUDA device\n");
        return 77;
    }

    int h_out[24] = {0};
    coolMemcpy(d_out, h_out, sizeof(h_out), coolMemcpyHostToDevice);

    compound_kernel<<<1, 4>>>(d_out);
    coolDeviceSynchronize();

    coolMemcpy(h_out, d_out, sizeof(h_out), coolMemcpyDeviceToHost);
    coolFree(d_out);

    struct {
        const char* what;
        int got;
        int want;
    } checks[] = {
        {"+=", h_out[1], 15},
        {"-=", h_out[2], 12},
        {"*=", h_out[3], 24},
        {"/=", h_out[4], 6},
        {"%=", h_out[5], 2},
        {"&=", h_out[6], 0x30},
        {"|=", h_out[7], 0x33},
        {"^=", h_out[8], 0x3C},
        {"<<=", h_out[9], 0xF0},
        {">>=", h_out[10], 0x0F},
        {"有符号 >>= （算术右移）", h_out[11], -4},
        {"!0", h_out[12], 1},
        {"!5", h_out[13], 0},
        {"!!5", h_out[14], 1},
        {"!!0", h_out[15], 0},
        {"bool flag = (a == 2)", h_out[16], 1},
        {"if (!flag) / if (flag)", h_out[17], 1},
        {"float +=", h_out[18], 40},
        {"float *=", h_out[19], 80},
        {"(a += 8) 表达式值", h_out[20], 0},
        {"a += 8 后 a", h_out[21], 10},
    };

    int failed = 0;
    for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); ++i) {
        if (checks[i].got != checks[i].want) {
            printf("compound FAILED: %s: got %d, want %d\n", checks[i].what, checks[i].got,
                   checks[i].want);
            failed = 1;
        }
    }
    if (failed) {
        return 1;
    }
    printf("compound OK: add=%d and=%d shl=%d not0=%d not5=%d flag=%d float=%d\n", h_out[1],
           h_out[6], h_out[9], h_out[12], h_out[13], h_out[16], h_out[18]);
    return 0;
}
