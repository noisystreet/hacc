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

    // 3) 单内核：命名规则 + 各字段。
    {
        std::string s = generate_stubs({"foo"}, "PTX");
        expect_contains(s, "#include <coolcc/hacc_runtime.h>", "单内核 include");
        expect_contains(s, "static const unsigned char __cool_image_foo[] = {", "单内核 image");
        expect_contains(s, "static coolKernel_t __cool_kernel_foo = nullptr;", "单内核 slot");
        expect_contains(s, "static std::once_flag __cool_once_foo;", "单内核 once");
        expect_contains(s, "static const hacc::KernelRegistration __cool_reg_foo = {", "单内核 reg");
        expect_contains(s, "\"foo\", __cool_image_foo, sizeof(__cool_image_foo),", "单内核 name");
        expect_contains(s, "&__cool_kernel_foo, &__cool_once_foo};", "单内核 指针");
        // PTX 文本字节（'P'=0x50, 'T'=0x54, 'X'=0x58）。
        expect_contains(s, "0x50,0x54,0x58,0x00", "单内核 PTX 字节");
    }

    // 4) 多内核：各自独立命名，互不串扰。
    {
        std::string s = generate_stubs({"a", "b"}, "P");
        expect_contains(s, "__cool_reg_a", "多内核 a");
        expect_contains(s, "__cool_reg_b", "多内核 b");
        expect_contains(s, "\"a\", __cool_image_a", "多内核 a name");
        expect_contains(s, "\"b\", __cool_image_b", "多内核 b name");
    }

    if (g_failures == 0) {
        std::printf("test_stub_gen: 全部通过\n");
        return 0;
    }
    std::fprintf(stderr, "test_stub_gen: %d 处失败\n", g_failures);
    return 1;
}
