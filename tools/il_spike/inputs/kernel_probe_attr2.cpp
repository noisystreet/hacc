// IL spike 探针 4：EDG 是否保留「未知/自定义」属性名（CUDA 关键字拟以属性承载），
// 并观察 address_space / aligned 的表示。需 GNU C++ 模式：cpfe 加 --g++。
// 覆盖 README「CUDA 属性清单」中的名字，供 verify_attrs.sh 断言。
extern "C" int __attribute__((cool_shared)) s_tile[64];
extern "C" int __attribute__((cool_constant)) c_tile[16];
extern "C" int __attribute__((address_space(3))) as_var[8];
extern "C" int __attribute__((aligned(128))) aligned_var[8];

// M4.0：__device__ 变量（文件作用域，addrspace 1）——与函数限定符区分，单独探针。
extern "C" int __attribute__((cool_device)) d_global_var;

extern "C" __attribute__((cool_global)) void cool_kernel_probe(int* y);
extern "C" __attribute__((cool_device)) void cool_device_probe(int* y);
extern "C" __attribute__((cool_host)) void cool_host_probe(int* y);

// M4.0：__restrict__ 指针参数（→ noalias）——属性挂在参数上。
extern "C" __attribute__((cool_global)) void cool_restrict_probe(
    int* __attribute__((cool_restrict)) a, int* __attribute__((cool_restrict)) b);

extern "C" int use_probe(void) {
    cool_kernel_probe(aligned_var);
    cool_device_probe(aligned_var);
    cool_host_probe(aligned_var);
    return s_tile[0] + c_tile[0] + as_var[0] + aligned_var[0] + d_global_var;
}
