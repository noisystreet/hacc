// 单元测试：M2.2b 主机桩生成器（内嵌映像 + 注册描述）。
//
// 纯文本变换，无需 EDG/LLVM/GPU。覆盖：字节数组渲染、单/多内核、空列表、
// 命名规则（foo → __cool_reg_foo）、NUL 终止。
#include <cstdio>
#include <string>
#include <vector>

#include "host/stub_gen.hpp"

namespace {

int g_failures = 0;

void expect_eq(const std::string& got, const std::string& want, const char* what) {
    if (got != want) {
        std::fprintf(stderr, "FAIL: %s\n  期望: [%s]\n  实际: [%s]\n", what, want.c_str(),
                     got.c_str());
        ++g_failures;
    }
}

void expect(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

// 子串包含断言。
void expect_contains(const std::string& hay, const std::string& needle, const char* what) {
    if (hay.find(needle) == std::string::npos) {
        std::fprintf(stderr, "FAIL: %s\n  未找到: [%s]\n  在: [%s]\n", what, needle.c_str(),
                     hay.c_str());
        ++g_failures;
    }
}

// 子串出现次数断言（用于「只渲染一份」这类结构性质）。
void expect_count(const std::string& hay, const std::string& needle, std::size_t want,
                  const char* what) {
    std::size_t n = 0;
    for (std::size_t pos = hay.find(needle); pos != std::string::npos;
         pos = hay.find(needle, pos + needle.size())) {
        ++n;
    }
    if (n != want) {
        std::fprintf(stderr, "FAIL: %s\n  期望出现 %zu 次，实际 %zu 次: [%s]\n", what, want, n,
                     needle.c_str());
        ++g_failures;
    }
}

}  // namespace

int main() {
    using hacc::host::generate_stubs;
    using hacc::host::render_byte_array;

    // 1) 字节数组渲染：短串 + NUL 终止。
    expect_eq(render_byte_array("AB"), "{\n    0x41,0x42,0x00\n}", "render 短串");
    // 空串：仅 NUL。
    expect_eq(render_byte_array(""), "{\n    0x00\n}", "render 空串");
    // 12 字节整行后换行再补 NUL。
    {
        std::string twelve(12, 'A');
        std::string got = render_byte_array(twelve);
        expect_contains(got, "0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,",
                        "render 12 字节整行");
        expect_contains(got, "\n    0x00\n}", "render 12 字节后 NUL 换行");
    }

    // 2) 空内核列表 → 空串。
    expect_eq(generate_stubs({}, "ptx"), "", "空内核列表");

    // 3) 单内核：命名规则 + 各字段（ADR 0007：模块粒度 = 编译单元）。
    {
        const std::string s = generate_stubs({"foo"}, "PTX");
        expect_contains(s, "#include <coolcc/hacc_runtime.h>", "单内核 include");
        // 模块级（每 TU 一份）：映像 / 模块句柄 / once / ModuleRegistration。
        expect_contains(s, "static const unsigned char __cool_image[] = {", "模块 映像");
        expect_contains(s, "static coolModule_t __cool_module = nullptr;", "模块 句柄");
        expect_contains(s, "static std::once_flag __cool_once_module;", "模块 once");
        expect_contains(s, "static const hacc::ModuleRegistration __cool_mod_reg = {", "模块 注册");
        expect_contains(s, "&__cool_module, &__cool_once_module};", "模块 注册指针");
        // 内核级：只留句柄槽位 + owner 指针，不再各持映像/模块。
        expect_contains(s, "static coolKernel_t __cool_kernel_foo = nullptr;", "单内核 slot");
        expect_contains(s, "static std::once_flag __cool_once_foo;", "单内核 once");
        expect_contains(s, "static const hacc::KernelRegistration __cool_reg_foo = {",
                        "单内核 reg");
        expect_contains(s, "\"foo\", &__cool_mod_reg, &__cool_kernel_foo, &__cool_once_foo};",
                        "单内核 指向模块");
        // PTX 文本字节（'P'=0x50, 'T'=0x54, 'X'=0x58）。
        expect_contains(s, "0x50,0x54,0x58,0x00", "单内核 PTX 字节");
        // M3.2：模块注册表。
        expect_contains(s, "static const hacc::ModuleRegistration* const __cool_all_mods[] = {",
                        "单内核 注册表");
        expect_contains(s, "&__cool_mod_reg,", "单内核 注册表项");
        expect_contains(s, "static const std::size_t __cool_all_mods_count = 1;",
                        "单内核 注册表计数");
    }

    // 3b) M3.2：__constant__ 符号描述符 + 拷贝包装。
    {
        std::vector<hacc::host::SymbolSpec> syms = {{"c_scale", 4}};
        std::string s = generate_stubs({"foo"}, "PTX", syms);
        expect_contains(s, "static coolSymbolDesc __cool_sym_c_scale = {nullptr, \"c_scale\", 4};",
                        "常量符号 描述符");
        expect_contains(s, "static inline coolError_t __cool_memcpy_to_symbol_c_scale(",
                        "常量符号 包装");
        expect_contains(s, "hacc::haccMemcpyToSymbolAll(__cool_all_mods, __cool_all_mods_count,",
                        "常量符号 遍历注册表");
        expect_contains(s, "&__cool_sym_c_scale, src, count, offset);", "常量符号 传描述符");
        // __constant__ 只出 To 方向的包装；From/取址仅对 __device__ 生成。
        expect_count(s, "__cool_memcpy_from_symbol_c_scale", 0, "常量符号 不应有 From 包装");
        expect_count(s, "__cool_get_symbol_address_c_scale", 0, "常量符号 不应有取址包装");
    }

    // 3c) __device__ 变量：描述符 + To/From/取址三件套包装。
    {
        std::vector<hacc::host::SymbolSpec> devs = {{"g_counter", 4}};
        std::string s = generate_stubs({"foo"}, "PTX", {}, devs);
        expect_contains(s,
                        "static coolSymbolDesc __cool_sym_g_counter = {nullptr, \"g_counter\", 4};",
                        "设备变量 描述符");
        expect_contains(s, "static inline coolError_t __cool_memcpy_to_symbol_g_counter(",
                        "设备变量 To 包装");
        expect_contains(s, "static inline coolError_t __cool_memcpy_from_symbol_g_counter(",
                        "设备变量 From 包装");
        expect_contains(s, "static inline coolError_t __cool_get_symbol_address_g_counter(",
                        "设备变量 取址包装");
        expect_contains(s, "hacc::haccMemcpyFromSymbolAll(__cool_all_mods, __cool_all_mods_count,",
                        "设备变量 From 遍历注册表");
        expect_contains(s, "hacc::haccGetSymbolAddressAll(__cool_all_mods, __cool_all_mods_count,",
                        "设备变量 取址遍历注册表");
        expect_contains(s, "&__cool_sym_g_counter, dst, count, offset);", "设备变量 From 传描述符");
        expect_contains(s, "&__cool_sym_g_counter, devPtr);", "设备变量 取址传描述符");
    }

    // 4) 多内核：各自独立命名，互不串扰；且**共用**一份映像与模块（ADR 0007）。
    {
        std::string s = generate_stubs({"a", "b"}, "P");
        expect_contains(s, "__cool_reg_a", "多内核 a");
        expect_contains(s, "__cool_reg_b", "多内核 b");
        expect_contains(s, "\"a\", &__cool_mod_reg", "多内核 a owner");
        expect_contains(s, "\"b\", &__cool_mod_reg", "多内核 b owner");
        // 关键回归：内嵌映像只渲染一份（修复前每个内核各一份，体积随内核数膨胀）。
        expect_count(s, "static const unsigned char __cool_image[]", 1, "多内核 映像只一份");
        expect_count(s, "static const hacc::ModuleRegistration __cool_mod_reg", 1,
                     "多内核 模块只一个");
        expect_count(s, "static coolModule_t __cool_module", 1, "多内核 模块句柄只一个");
    }

    if (g_failures == 0) {
        std::printf("test_stub_gen: 全部通过\n");
        return 0;
    }
    std::fprintf(stderr, "test_stub_gen: %d 处失败\n", g_failures);
    return 1;
}
