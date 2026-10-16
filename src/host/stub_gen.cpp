// M2.2b：主机桩生成器实现（见 stub_gen.hpp 与 docs/m2-host-path-plan.md §3.3 / §8.3）。
#include "host/stub_gen.hpp"

#include <cstddef>
#include <cstdio>
#include <string>

namespace hacc::host {

std::string render_byte_array(const std::string& bytes) {
    // 每行 12 字节，`0x..` 逗号分隔；末尾补 NUL（coolModuleLoadData 按 PTX 文本解析，需终止符）。
    constexpr std::size_t kPerLine = 12;
    std::string out = "{";
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i % kPerLine == 0) {
            out += "\n    ";
        }
        char buf[8];
        std::snprintf(buf, sizeof(buf), "0x%02x,", static_cast<unsigned char>(bytes[i]));
        out += buf;
    }
    // 终止 NUL。
    if (bytes.size() % kPerLine == 0) {
        out += "\n    ";
    }
    out += "0x00\n}";
    return out;
}

std::string generate_stubs(const std::vector<std::string>& kernel_names,
                           const std::string& ptx_text,
                           const std::vector<ConstantSymbol>& constant_symbols) {
    if (kernel_names.empty()) {
        return std::string();
    }
    std::string out;
    // 自包含：桩依赖 hacc::KernelRegistration / haccLaunch，故带上运行时头。
    out += "\n#include <coolcc/hacc_runtime.h>\n";
    out += "\n// ---- hacc 主机桩（M2.2b 自动生成；内嵌设备映像 + 惰性注册描述）----\n";
    for (const std::string& name : kernel_names) {
        out += "static const unsigned char __cool_image_" + name +
               "[] = " + render_byte_array(ptx_text) + ";\n";
        out += "static coolKernel_t __cool_kernel_" + name + " = nullptr;\n";
        out += "static coolModule_t __cool_module_" + name + " = nullptr;\n";
        out += "static std::once_flag __cool_once_" + name + ";\n";
        out += "static const hacc::KernelRegistration __cool_reg_" + name + " = {\n";
        out +=
            "    \"" + name + "\", __cool_image_" + name + ", sizeof(__cool_image_" + name + "),\n";
        out += "    &__cool_kernel_" + name + ", &__cool_once_" + name + ", &__cool_module_" +
               name + "};\n";
    }
    // M3.2：全局注册表（供 haccMemcpyToSymbol 遍历所有模块填充 __constant__ 符号）。
    out += "static const hacc::KernelRegistration* const __cool_all_regs[] = {\n";
    for (const std::string& name : kernel_names) {
        out += "    &__cool_reg_" + name + ",\n";
    }
    out += "};\n";
    out +=
        "static const std::size_t __cool_all_regs_count = " + std::to_string(kernel_names.size()) +
        ";\n";
    // M3.2：__constant__ 符号描述符 + 拷贝包装（module 由 haccMemcpyToSymbolAll 运行时填充）。
    // 包装函数遍历所有内核模块，确保每个模块里的该符号都被填充。
    for (const ConstantSymbol& sym : constant_symbols) {
        out += "static coolSymbolDesc __cool_sym_" + sym.name + " = {nullptr, \"" + sym.name +
               "\", " + std::to_string(sym.size) + "};\n";
        out += "static inline coolError_t __cool_memcpy_to_symbol_" + sym.name +
               "(const void* src, std::size_t count, std::size_t offset) {\n";
        out += "    return hacc::haccMemcpyToSymbolAll(__cool_all_regs, __cool_all_regs_count,\n";
        out += "        &__cool_sym_" + sym.name + ", src, count, offset);\n";
        out += "}\n";
    }
    return out;
}

}  // namespace hacc::host
