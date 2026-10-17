// M2.2b：主机桩生成器（纯文本变换，无 EDG/LLVM/GPU 依赖）。
//
// 为**整个编译单元**生成「内嵌设备映像 + 模块注册 + 内核注册描述」的 C++ 定义，
// 供 host 源（同一 TU）引用（见 docs/m2-host-path-plan.md §3.3 / §8.3、
// docs/adr/0007-module-granularity-per-tu.md）。
//
// 生成内容（ADR 0007：模块粒度 = 编译单元，PTX 只渲染/载入一次）：
//   static const unsigned char __cool_image[] = { ... };        // PTX 文本字节（仅一份）
//   static coolModule_t __cool_module = nullptr;
//   static std::once_flag __cool_once_module;
//   static const hacc::ModuleRegistration __cool_mod_reg = { ... };
//
// 以及每个内核 `foo` 一份（只负责取函数，不再各持一份映像/模块）：
//   static coolKernel_t __cool_kernel_foo = nullptr;
//   static std::once_flag __cool_once_foo;
//   static const hacc::KernelRegistration __cool_reg_foo = {
//       "foo", &__cool_mod_reg, &__cool_kernel_foo, &__cool_once_foo };
//
// 设计为**纯函数**：输入（内核名列表 + PTX 文本）→ 输出桩代码字符串，便于独立单测。
// 命名规则确定性：`foo` → `__cool_reg_foo`（与改写器 §3.2 一致）。
#ifndef HACC_HOST_STUB_GEN_HPP
#define HACC_HOST_STUB_GEN_HPP

#include <string>
#include <vector>

namespace hacc::host {

// 文件作用域设备符号：名字 + 字节数。
// 由 driver 从设备侧翻译结果收集，供生成 coolSymbolDesc。
// 既用于 __constant__（M3.2），也用于 __device__ 变量（主机侧读取）。
struct SymbolSpec {
    std::string name;  // 设备侧符号名（如 "c_scale" / "g_counter"）
    std::size_t size;  // 字节数
};

// 生成桩代码。kernel_names 为内核名（去重、按序）；ptx_text 为设备映像（PTX 文本）。
// constant_symbols 为文件作用域 __constant__ 符号，生成 coolSymbolDesc + To 拷贝包装
// （M3.2，供 cudaMemcpyToSymbol）。
// device_symbols 为文件作用域 __device__ 变量，生成 coolSymbolDesc + To/From 拷贝包装
// 与取址包装（供 cudaMemcpyToSymbol / cudaMemcpyFromSymbol / cudaGetSymbolAddress）。
// 返回可直接拼入 host 源的 C++ 代码片段（含末尾换行）。
// 空 kernel_names 返回空串（无内核则无桩）。
std::string generate_stubs(const std::vector<std::string>& kernel_names,
                           const std::string& ptx_text,
                           const std::vector<SymbolSpec>& constant_symbols = {},
                           const std::vector<SymbolSpec>& device_symbols = {});

// 把任意字节串渲染为 C 字节数组字面量（`{ 0x.., ... }`，每行 12 字节）。
// 供 generate_stubs 复用，也便于单测。
std::string render_byte_array(const std::string& bytes);

}  // namespace hacc::host

#endif  // HACC_HOST_STUB_GEN_HPP
