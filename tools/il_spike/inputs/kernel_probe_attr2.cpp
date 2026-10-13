// IL spike 探针 4：EDG 是否保留「未知/自定义」属性名（CUDA 关键字拟以属性承载），
// 并观察 address_space / aligned 的表示。需 GNU C++ 模式：cpfe 加 --g++。
extern "C" int __attribute__((cool_shared)) s_tile[64];
extern "C" int __attribute__((address_space(3))) as_var[8];
extern "C" int __attribute__((aligned(128))) aligned_var[8];

extern "C" __attribute__((cool_global)) void cool_kernel_probe(int* y);

extern "C" int use_probe(void) {
    cool_kernel_probe(aligned_var);
    return s_tile[0] + as_var[0] + aligned_var[0];
}
