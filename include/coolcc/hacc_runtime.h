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

// 模块注册描述：每个**编译单元**（= 一份设备映像）一个（ADR 0007）。
// 同一份 PTX 只载入一次，故同一 TU 内所有内核共享同一个 module 实例——
// 设备全局变量（`__device__`）因此对 TU 内所有内核可见。
struct ModuleRegistration {
    const char* name;       // 诊断用（产物 / TU 名）
    const void* image;      // 内嵌设备映像（PTX 文本字节）
    std::size_t imageSize;  // 映像字节数
    coolModule_t* module;   // 模块句柄槽位（惰性载入后缓存）
    std::once_flag* once;   // 一次性载入保护（每模块一个）
};

// 内核注册描述：每个 `__global__` 函数一个，指向所属编译单元的模块。
// 由 hacc driver 为每个 `__global__` 函数生成一个 static 实例（与 host 源同 TU）。
struct KernelRegistration {
    const char* name;                 // 内核符号名（coolModuleGetFunction 按名取）
    const ModuleRegistration* owner;  // 所属编译单元的模块
    coolKernel_t* slot;               // 句柄槽位（惰性解析后缓存）
    std::once_flag* once;             // 仅保护 GetFunction 一步（模块载入由 owner 的 once 保护）
};

// 惰性载入模块：首次调用时 coolModuleLoadData，缓存到 *module。
// 失败返回 nullptr（不抛异常）。线程安全（once 保护）。
//
// 实现放头文件（inline）：本头随**生成的 host 源**一起编译，由最终 g++ 链接 coolrt；
// hacc 自身工具链（hacc/coolfe++/coolicc）不链接 coolrt，故不能把实现放进 hacc_support。
inline coolModule_t resolve_module(const ModuleRegistration& mod) {
    std::call_once(*mod.once, [&mod] {
        coolModule_t module = nullptr;
        if (coolModuleLoadData(&module, mod.image) != coolSuccess) {
            return;  // 载入失败：*mod.module 保持 nullptr
        }
        *mod.module = module;
    });
    return *mod.module;
}

// 惰性解析内核：先确保所属模块已载入，再 coolModuleGetFunction（缓存到 *slot）。
// 失败返回 nullptr（不抛异常）。两层 once 使用**不同**标志，嵌套调用不会死锁。
inline coolKernel_t resolve(const KernelRegistration& reg) {
    std::call_once(*reg.once, [&reg] {
        const coolModule_t module = resolve_module(*reg.owner);
        if (module == nullptr) {
            return;  // 模块未载入：*reg.slot 保持 nullptr
        }
        coolKernel_t kernel = nullptr;
        if (coolModuleGetFunction(&kernel, module, reg.name) != coolSuccess) {
            return;  // 取函数失败：*reg.slot 保持 nullptr
        }
        *reg.slot = kernel;
    });
    return *reg.slot;
}

// M3.2：把主机数据拷入设备符号（__constant__）。
// ADR 0007 之后模块粒度 = 编译单元：单 TU 只有一份模块，退化为**一次** coolMemcpyToSymbol；
// 若将来一次链接包含多个 TU（各自 module），仍需遍历——但基数由「内核数」降为「TU 数」，
// 不再随内核数量膨胀。mods/count 为 hacc 生成的模块注册表；desc 为符号描述符。
// 返回首个非成功错误码（全部成功返回 coolSuccess）。
inline coolError_t haccMemcpyToSymbolAll(const ModuleRegistration* const* mods, std::size_t count,
                                         coolSymbolDesc* desc, const void* src, std::size_t bytes,
                                         std::size_t offset) {
    if (desc == nullptr) {
        return coolErrorInvalidValue;
    }
    coolError_t first_err = coolSuccess;
    for (std::size_t i = 0; i < count; ++i) {
        const ModuleRegistration* mod = mods[i];
        if (mod == nullptr) {
            continue;
        }
        const coolModule_t module = resolve_module(*mod);  // 确保模块已载入
        if (module == nullptr) {
            continue;  // 该模块未成功载入：跳过
        }
        desc->module = module;
        const coolError_t err =
            coolMemcpyToSymbol(desc, src, bytes, offset, coolMemcpyHostToDevice);
        if (err != coolSuccess && first_err == coolSuccess) {
            first_err = err;
        }
    }
    return first_err;
}

// 设备符号 → 主机/设备（__device__ 变量主机侧读取）。
// 与 haccMemcpyToSymbolAll 对称：同样的 fan-out 语义，方向固定 D2H。
inline coolError_t haccMemcpyFromSymbolAll(const ModuleRegistration* const* mods, std::size_t count,
                                           coolSymbolDesc* desc, void* dst, std::size_t bytes,
                                           std::size_t offset) {
    if (desc == nullptr) {
        return coolErrorInvalidValue;
    }
    coolError_t first_err = coolSuccess;
    for (std::size_t i = 0; i < count; ++i) {
        const ModuleRegistration* mod = mods[i];
        if (mod == nullptr) {
            continue;
        }
        const coolModule_t module = resolve_module(*mod);  // 确保模块已载入
        if (module == nullptr) {
            continue;  // 该模块未成功载入：跳过
        }
        desc->module = module;
        // 注意参数顺序与 ToSymbol 相反：FromSymbol 是 (dst, symbol, ...)。
        const coolError_t err =
            coolMemcpyFromSymbol(dst, desc, bytes, offset, coolMemcpyDeviceToHost);
        if (err != coolSuccess && first_err == coolSuccess) {
            first_err = err;
        }
    }
    return first_err;
}

// 取设备符号地址（__device__ / __constant__ 均可）：先确保模块载入，再取地址。
// 与上面两个包装一致地遍历模块注册表；成功时写入 *devPtr。
inline coolError_t haccGetSymbolAddressAll(const ModuleRegistration* const* mods, std::size_t count,
                                           coolSymbolDesc* desc, void** devPtr) {
    if (desc == nullptr || devPtr == nullptr) {
        return coolErrorInvalidValue;
    }
    coolError_t first_err = coolSuccess;
    for (std::size_t i = 0; i < count; ++i) {
        const ModuleRegistration* mod = mods[i];
        if (mod == nullptr) {
            continue;
        }
        const coolModule_t module = resolve_module(*mod);
        if (module == nullptr) {
            continue;
        }
        desc->module = module;
        const coolError_t err = coolGetSymbolAddress(devPtr, desc);
        if (err == coolSuccess) {
            return coolSuccess;
        }
        if (first_err == coolSuccess) {
            first_err = err;
        }
    }
    return first_err;
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
