// M1.2.3 golden 输入：标量参数 + 简单语句（赋值/算术/if）。
// 期望：coolfe++ 产出 .cil，coolicc --from-il 产出含参数与分支的 PTX。
__attribute__((cool_global)) void add_kernel(int* out, int a, int b) {
    int c = a + b;
    if (c > 0) {
        *out = c;
    } else {
        *out = 0;
    }
}
