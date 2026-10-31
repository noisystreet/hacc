// 成员左值（C18）：`s.f = ...` / `p->f = ...` —— 混合字段结构体的成员写入。
//
// 与 `ldg.cu` 里的向量段互补：那边覆盖的是「float4 → <4 x float>」的向量分支
// （成员地址用 GEP 的第二下标取元素），这里覆盖**普通聚合**分支
// （`CreateStructGEP`），两条路必须都测到。
//
// **本用例只有数值断言，没有 PTX 形态断言 —— 理由要写清楚**：
// 这个功能的失效模式是「取错成员/算错偏移」，而这类错误在 PTX 形态上完全看不出来
// （依然是普通 `st.local.f32` / `st.global.f32`，只是偏移量错了）。反向验证也证实了
// 这一点：把向量成员的 GEP 两个下标写反后，**PTX 形态断言毫无反应，只有数值断言变红**。
// 所以此处的数值断言就是那一层有效断言，不是「省掉了形态断言」。
//
// 覆盖三种基：
//   ① 局部变量基      s.f = ...        （enk_variable）
//   ② 指针基（箭头）  p->i = ...       （基是 eok_indirect）
//   ③ 成员复合赋值    s.f += ...       （读-改-写，地址只求值一次）
#include <cool/cool_runtime.h>

#include <stdio.h>

struct Pair {
    float f;
    int i;
};

__global__ void struct_k(struct Pair* p, float* out_f, int* out_i) {
    // ① 局部混合字段结构体：成员写入 + ③ 成员复合赋值。
    struct Pair s;
    s.f = 1.5f;
    s.i = 7;
    s.f += 2.0f;

    // ② 指针基的成员写入（p->f / p->i）。
    p->f = s.f * 2.0f;
    p->i = s.i + 1;

    out_f[0] = s.f;
    out_i[0] = s.i;
}

int main() {
    struct Pair* d_p = nullptr;
    float* d_f = nullptr;
    int* d_i = nullptr;
    if (coolMalloc((void**)&d_p, sizeof(struct Pair)) != coolSuccess ||
        coolMalloc((void**)&d_f, sizeof(float)) != coolSuccess ||
        coolMalloc((void**)&d_i, sizeof(int)) != coolSuccess) {
        printf("structMember SKIP: no usable CUDA device\n");
        return 77;
    }

    struct Pair h_p = {-1.0f, -1};
    coolMemcpy(d_p, &h_p, sizeof(h_p), coolMemcpyHostToDevice);

    struct_k<<<1, 1>>>(d_p, d_f, d_i);
    if (coolDeviceSynchronize() != coolSuccess) {
        printf("structMember FAIL: kernel sync failed\n");
        coolFree(d_p);
        coolFree(d_f);
        coolFree(d_i);
        return 1;
    }

    float h_f = 0.0f;
    int h_i = 0;
    coolMemcpy(&h_f, d_f, sizeof(float), coolMemcpyDeviceToHost);
    coolMemcpy(&h_i, d_i, sizeof(int), coolMemcpyDeviceToHost);
    coolMemcpy(&h_p, d_p, sizeof(h_p), coolMemcpyDeviceToHost);
    coolFree(d_p);
    coolFree(d_f);
    coolFree(d_i);

    int failed = 0;
    // s.f = 1.5 + 2.0 = 3.5；p->f = 3.5 * 2 = 7.0。
    if (h_f != 3.5f) {
        printf("structMember FAIL: s.f = %g, want 3.5\n", h_f);
        ++failed;
    }
    if (h_i != 7) {
        printf("structMember FAIL: s.i = %d, want 7\n", h_i);
        ++failed;
    }
    if (h_p.f != 7.0f) {
        printf("structMember FAIL: p->f = %g, want 7\n", h_p.f);
        ++failed;
    }
    if (h_p.i != 8) {
        printf("structMember FAIL: p->i = %d, want 8\n", h_p.i);
        ++failed;
    }

    if (failed != 0) {
        return 1;
    }

    printf("structMember OK: 局部成员写入 / 复合赋值 / 箭头写入 全部匹配\n");
    return 0;
}
