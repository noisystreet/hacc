// B9 golden/e2e 输入：位运算符（& | ^ << >> ~）。
//
// 设备侧映射：
//   eok_and / eok_or / eok_xor  → LLVM and / or / xor
//   eok_shiftl                  → LLVM shl
//   eok_shiftr                  → 有符号 ashr / 无符号 lshr（按操作数符号性）
//   eok_complement（~）         → LLVM xor -1
//
// e2e 策略：
//   - 单 block、单线程执行主体（t != 0 直接返回），结果完全确定，断言可以写死。
//   - 先用字面量、再用变量：字面量常量折叠后可能退化成一条 mov，
//     变量版才真正走「寄存器 → 指令 → 寄存器」的路径，两者都要覆盖。
//   - 有符号 / 无符号右移必须分别验证：同一个 bit 模式（0xFFFFFFF0）
//     算术右移得 -4、逻辑右移得 0x3FFFFFFC，选错指令会立刻露出来。
#include <cool/cool_runtime.h>

#include <stdio.h>

__global__ void bitwise_kernel(int* out) {
    int t = threadIdx.x;
    if (t != 0) {
        return;
    }

    // --- 字面量路径 ---
    out[1] = 0xF0 & 0x3C;  // 0x30   = 48
    out[2] = 0xF0 | 0x0F;  // 0xFF   = 255
    out[3] = 0xF0 ^ 0x3C;  // 0xCC   = 204
    out[4] = 1 << 5;       // 0x20   = 32
    out[5] = 0x100 >> 4;   // 0x10   = 16
    out[6] = ~0x0F;        // 0xFFFFFFF0 = -16

    // --- 变量路径（不折叠）---
    // 关键：a/b 都掺入 t，使编译器**无法**常量传播成 mov。
    // 否则纯字面量 `0xF0 & 0x3C` 会被折叠成常量 48，PTX 里根本不会出现 and.b32，
    // 「指令确实生成了」就无从验证。t 在本 kernel 里恒为 0，故取值不受影响。
    int a = t + 0xF0;
    int b = t + 0x3C;
    out[7] = a & b;   // 0x30 = 48
    out[8] = a | b;   // 0xFC = 252
    out[9] = a ^ b;   // 0xCC = 204
    out[10] = a << 2; // 0x3C0 = 960
    out[11] = a >> 2; // 0x3C  = 60

    // --- 有符号 vs 无符号右移 ---
    // 同一 bit 模式 0xFFFFFFF0：有符号按算术右移保留符号位（-4），
    // 无符号按逻辑右移补零（0x3FFFFFFC）。若把 shiftr 一律翻成 ashr，
    // out[13] 会变成 -1 而不是 1073741820。
    // 同样掺 t，避免常量折叠绕过 shr 指令。
    int neg = t - 16;
    out[12] = neg >> 2; // -4
    unsigned int u = (unsigned int)t + (unsigned int)0xFFFFFFF0;
    out[13] = (int)(u >> 2); // 0x3FFFFFFC = 1073741820

    // --- 组合用法：warp 代码里取 lane 掩码的典型形态 ---
    int lane = t & 0x1F;           // 变量 & 常量
    int mask = (1 << 3) | (1 << 5); // 0x08 | 0x20 = 0x28 = 40
    out[14] = lane;                 // 本线程 t=0 → 0
    out[15] = mask;                 // 40
    out[16] = (a & mask) ^ (b & mask); // (0xF0&0x28)^(0x3C&0x28) = 0x20^0x28 = 0x08 = 8
}

int main() {
    int* d_out = nullptr;
    // 无可用 CUDA 设备时 coolMalloc 失败：返回 77（SKIP 约定），供测试框架跳过。
    if (coolMalloc((void**)&d_out, sizeof(int) * 24) != coolSuccess) {
        printf("bitwise SKIP: no usable CUDA device\n");
        return 77;
    }

    int h_out[24] = {0};
    coolMemcpy(d_out, h_out, sizeof(h_out), coolMemcpyHostToDevice);

    bitwise_kernel<<<1, 4>>>(d_out);
    coolDeviceSynchronize();

    coolMemcpy(h_out, d_out, sizeof(h_out), coolMemcpyDeviceToHost);
    coolFree(d_out);

    // 逐项校验，失败时打印具体项，便于定位是哪条位运算语义错了。
    struct {
        const char* what;
        int got;
        int want;
    } checks[] = {
        {"字面量 &", h_out[1], 0x30},
        {"字面量 |", h_out[2], 0xFF},
        {"字面量 ^", h_out[3], 0xCC},
        {"字面量 <<", h_out[4], 32},
        {"字面量 >>", h_out[5], 16},
        {"字面量 ~", h_out[6], -16},
        {"变量 &", h_out[7], 0x30},
        {"变量 |", h_out[8], 0xFC},
        {"变量 ^", h_out[9], 0xCC},
        {"变量 <<", h_out[10], 960},
        {"变量 >>（正数）", h_out[11], 60},
        {"有符号 >>（算术右移）", h_out[12], -4},
        {"无符号 >>（逻辑右移）", h_out[13], 0x3FFFFFFC},
        {"lane = t & 0x1F", h_out[14], 0},
        {"(1<<3)|(1<<5)", h_out[15], 40},
        {"混合 (a&m)^(b&m)", h_out[16], 8},
    };

    int failed = 0;
    for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); ++i) {
        if (checks[i].got != checks[i].want) {
            printf("bitwise FAILED: %s: got %d, want %d\n", checks[i].what, checks[i].got,
                   checks[i].want);
            failed = 1;
        }
    }
    if (failed) {
        return 1;
    }
    printf("bitwise OK: and=%d or=%d xor=%d shl=%d shr=%d not=%d ashr=%d lshr=%d\n", h_out[1],
           h_out[2], h_out[3], h_out[4], h_out[5], h_out[6], h_out[12], h_out[13]);
    return 0;
}
