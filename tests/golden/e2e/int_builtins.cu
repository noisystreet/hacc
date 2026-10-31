// P6/R5 第一批：整数单指令内建 + 内核内计时。
//
// 断言分两类：
//   1. 纯函数（__popc / __clz / __ffs / __brev）→ 与期望值**严格相等**，并覆盖
//      CUDA 的边界语义（__clz(0)=32、__ffs(0)=0、__ffs 是 1-based）。
//      这几条边界正是「能编译、看着跑通、数值不对」的高发区：少一个特判就静默错值。
//   2. 计时（clock / clock64）→ 只验证「读到了时间」这一个结构性质。
//
// **操作数一律依赖主机传入的 seed，不用字面量**：操作数全为常量时，LLVM 的
// 常量折叠会让 PTX 里根本不出现 popc/clz/brev 指令 —— golden 断言就成了空转
// （本项目在 C11 前半踩过两次）。
//
// **不比较 clock() 与 clock64() 的差值大小**：实测同一区间 38 vs 7，两者的读数
// 刻度/采样点不保证可比，跨计数器比较是错误假设；只各自验证「非零」。
#include <cool/cool_runtime.h>

#include <stdio.h>

#define N 12

/* seed = 0xF0F0F0F0：高低位各有 16 个 1，便于手算每条期望值。 */
#define SEED 0xF0F0F0F0u

__global__ void int_k(unsigned int* out, unsigned int seed) {
    const int i = threadIdx.x;
    switch (i) {
        /* --- 位计数：原值 / 取反（都是 16 个 1）/ 全 1 --- */
        case 0: out[i] = __popc(seed); break;         /* 16 */
        case 1: out[i] = __popc(~seed); break;        /* 16 */
        case 2: out[i] = __popc(seed | ~seed); break; /* 32（全 1） */
        /* --- 前导零：最高位 / 低位掩码 / 0（边界）--- */
        case 3: out[i] = __clz(seed); break;               /* 0 */
        case 4: out[i] = __clz(seed & 0x00FFFFFFu); break; /* 8 */
        case 5: out[i] = __clz(seed & 0x1u); break;        /* 32 —— 边界 */
        /* --- 最低位 1 的位置：1-based / 掩码 / 0（边界）--- */
        case 6: out[i] = __ffs(seed); break;               /* 5 */
        case 7: out[i] = __ffs(seed & 0x00FF0000u); break; /* 21 */
        case 8: out[i] = __ffs(seed & 0x1u); break;        /* 0 —— 边界 */
        /* --- 位序反转 --- */
        case 9: out[i] = __brev(seed); break;         /* 0x0F0F0F0F */
        case 10: out[i] = __brev(seed | 0x1u); break; /* 0x8F0F0F0F */
        /* --- 前导零：取反后的高位 --- */
        default: out[i] = __clz(~seed); break; /* 4 */
    }
}

__global__ void clock_k(unsigned long long* out) {
    const unsigned int t0 = clock();
    const unsigned long long t64_0 = clock64();

    /* 做点可观测的工作：既验证 __popc 在循环里可用，也让两次读数之间必然有间隔。 */
    unsigned int acc = 0;
    for (int i = 0; i < 64; i = i + 1) {
        acc += __popc((unsigned int)i);
    }

    const unsigned int t1 = clock();
    const unsigned long long t64_1 = clock64();

    out[0] = t1 - t0;       /* 32 位差值 */
    out[1] = t64_1 - t64_0; /* 64 位差值 */
    out[2] = acc;           /* sum(__popc(0..63)) == 192 */
}

int main() {
    unsigned int* d_int = nullptr;
    if (coolMalloc((void**)&d_int, sizeof(unsigned int) * N) != coolSuccess) {
        printf("intBuiltins SKIP: no usable CUDA device\n");
        return 77;
    }

    int_k<<<1, N>>>(d_int, SEED);
    if (coolDeviceSynchronize() != coolSuccess) {
        printf("intBuiltins FAIL: kernel sync failed\n");
        coolFree(d_int);
        return 1;
    }

    unsigned int h[N];
    coolMemcpy(h, d_int, sizeof(h), coolMemcpyDeviceToHost);
    coolFree(d_int);

    static const unsigned int want[N] = {
        16u, 16u, 32u, 0u,          8u, 32u,
        5u,  21u, 0u,  0x0F0F0F0Fu, 0x8F0F0F0Fu, 4u,
    };
    static const char* names[N] = {
        "__popc(seed)",      "__popc(~seed)",        "__popc(seed|~seed)", "__clz(seed)",
        "__clz(seed&0xFFFFFF)", "__clz(seed&1)",     "__ffs(seed)",        "__ffs(seed&0xFF0000)",
        "__ffs(seed&1)",     "__brev(seed)",         "__brev(seed|1)",     "__clz(~seed)",
    };

    int failed = 0;
    for (int i = 0; i < N; ++i) {
        if (h[i] != want[i]) {
            printf("intBuiltins FAIL: %s = 0x%08X, want 0x%08X\n", names[i], h[i], want[i]);
            ++failed;
        }
    }

    /* 计时：只验证「读到了时间」，不比较两个计数器的差值大小。 */
    unsigned long long* d_clk = nullptr;
    if (coolMalloc((void**)&d_clk, sizeof(unsigned long long) * 3) != coolSuccess) {
        printf("intBuiltins FAIL: clock buffer alloc failed\n");
        return 1;
    }
    clock_k<<<1, 1>>>(d_clk);
    if (coolDeviceSynchronize() != coolSuccess) {
        printf("intBuiltins FAIL: clock kernel sync failed\n");
        coolFree(d_clk);
        return 1;
    }
    unsigned long long clk[3];
    coolMemcpy(clk, d_clk, sizeof(clk), coolMemcpyDeviceToHost);
    coolFree(d_clk);

    if (clk[0] == 0) {
        printf("intBuiltins FAIL: clock() 区间读数为 0（32 位时钟未被真正读取）\n");
        ++failed;
    }
    if (clk[1] == 0) {
        printf("intBuiltins FAIL: clock64() 区间读数为 0（时钟未被真正读取）\n");
        ++failed;
    }
    if (clk[2] != 192) {
        printf("intBuiltins FAIL: __popc 累加和 = %llu, want 192（循环未真正执行）\n", clk[2]);
        ++failed;
    }

    if (failed != 0) {
        return 1;
    }

    printf("intBuiltins OK: %d 项 + clock/clock64（区间 %llu / %llu 周期）\n", N, clk[0], clk[1]);
    return 0;
}
