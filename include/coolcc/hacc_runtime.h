/* hacc 运行时支持头（随编译器分发，被生成的主机代码 include）。
 *
 * 提供主机桩 / 内嵌映像 / 惰性注册的支持接口（对标 nvcc 的 crt/host_runtime.h）。
 * 一期范围与接口见 docs/compiler-implementation-plan.md §6.2、ADR 0003、
 * docs/m2-host-path-plan.md §3.3/§3.4/§8.5。
 *
 * 用法（由 hacc driver 生成的 host 源）：
 *   #include <coolcc/hacc_runtime.h>
 *   static const unsigned char __cool_image_foo[] = { ... };   // PTX 文本字节
 *   static coolKernel_t __cool_kernel_foo = nullptr;
 *   static std::once_flag __cool_once_foo;
 *   static const hacc::KernelRegistration __cool_reg_foo = {
 *       "foo", __cool_image_foo, sizeof(__cool_image_foo), &__cool_kernel_foo,
 *       &__cool_once_foo };
 *   ...
 *   haccLaunch(__cool_reg_foo, grid, block, smem, stream, args...);
 *
 * 设计要点（见方案 §8）：
 *   - 惰性注册：首次启动时 resolve（call_once 保护），失败静默返回 nullptr
 *     （last error 由 coolrt 覆盖，符合 C ABI 语义）。
 *   - 实参物化：haccLaunch 的模板参数按值传入，本身即具名临时，取地址成 void*[]。
 *   - grid/block 模板化：接受 int / dim3（对齐 CUDA <<<g,b>>> 的隐式转换）。
 */
#ifndef HACC_RUNTIME_H
#define HACC_RUNTIME_H

#include <cstddef>
#include <mutex>

#include <cool/cool_runtime_api.h>

namespace hacc {

// 内核注册描述：名字 + 内嵌设备映像（PTX 文本字节）+ 句柄槽位。
// 由 hacc driver 为每个 `__global__` 函数生成一个 static 实例（与 host 源同 TU）。
struct KernelRegistration {
    const char* name;       // 内核符号名（coolModuleGetFunction 按名取）
    const void* image;      // 内嵌设备映像（PTX 文本字节）
    std::size_t imageSize;  // 映像字节数
    coolKernel_t* slot;     // 句柄槽位（惰性解析后缓存）
    std::once_flag* once;   // 一次性注册保护（每个注册描述一个）
};

// 惰性解析：首次调用时 coolModuleLoadData + coolModuleGetFunction，缓存到 *slot。
// 失败返回 nullptr（不抛异常）。线程安全（once 保护）。
//
// 实现放头文件（inline）：本头随**生成的 host 源**一起编译，由最终 g++ 链接 coolrt；
// hacc 自身工具链（hacc/coolfe++/coolicc）不链接 coolrt，故不能把实现放进 hacc_support。
inline coolKernel_t resolve(const KernelRegistration& reg) {
    std::call_once(*reg.once, [&reg] {
        coolModule_t module = nullptr;
        if (coolModuleLoadData(&module, reg.image) != coolSuccess) {
            return;  // 载入失败：*reg.slot 保持 nullptr
        }
        coolKernel_t kernel = nullptr;
        if (coolModuleGetFunction(&kernel, module, reg.name) != coolSuccess) {
            return;  // 取函数失败：*reg.slot 保持 nullptr
        }
        *reg.slot = kernel;
    });
    return *reg.slot;
}

// grid/block 归一化为 dim3：接受 int（如 (n+255)/256）或 dim3。
inline dim3 to_dim3(unsigned int v) {
    return dim3{v, 1, 1};
}
inline dim3 to_dim3(int v) {
    return dim3{static_cast<unsigned int>(v), 1, 1};
}
inline dim3 to_dim3(dim3 d) {
    return d;
}

// 启动跳板：把实参物化为具名临时，取地址成 void*[]，调 coolLaunchKernel。
// 第一个参数是注册描述（__cool_reg_foo），内部 resolve 为 coolKernel_t。
// 丢弃 coolLaunchKernel 返回码（last error 由 coolrt 覆盖，见方案 §3.4）。
template <typename G, typename B, typename... Args>
void haccLaunch(const KernelRegistration& reg, G grid, B block, std::size_t smem,
                coolStream_t stream, Args... args) {
    coolKernel_t k = resolve(reg);
    if (k == nullptr) {
        return;  // 失败静默（last error 由 coolrt 覆盖）
    }
    void* argv[] = {(void*)&args...};  // 实参物化为具名临时（模板参数即临时）
    (void)coolLaunchKernel(k, to_dim3(grid), to_dim3(block), argv, smem, stream);
}

}  // namespace hacc

#endif /* HACC_RUNTIME_H */
