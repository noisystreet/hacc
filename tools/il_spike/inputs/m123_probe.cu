// M1.2.3 探针：标量参数 + 简单语句（赋值/算术/if）。
// 期望：dump 出真实 IL，核对参数、语句、表达式、if 的结构。
__attribute__((cool_global)) void add_kernel(int* out, int a, int b) {
    int c = a + b;
    if (c > 0) {
        *out = c;
    } else {
        *out = 0;
    }
}
