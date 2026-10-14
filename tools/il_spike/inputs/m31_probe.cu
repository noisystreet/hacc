// M3.1 探针：__shared__ 静态共享内存 + 数组下标，观察 EDG IL 表示。
// 期望：dump 出真实 IL，核对
//   ① __shared__ 变量（cool_shared 属性）的声明形态与属性位置；
//   ② 数组类型（tk_array）的 element_type / number_of_elements；
//   ③ 数组下标 sdata[t] 的运算符形态（eok_subscript？操作数结构？）。
//
// 说明：EDG 不认识 CUDA 关键字，故此处直接用 __attribute__((cool_shared)) 标注
// （等价于 coolrt 头在 __COOLCC__ 下对 __shared__ 的展开）。

__attribute__((cool_global)) void shared_probe(int* out) {
    __attribute__((cool_shared)) int sdata[64];
    int t = 0;
    sdata[t] = t;
    out[t] = sdata[t] + sdata[0];
}
