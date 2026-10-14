// 单元测试：M2.0 主机源码改写器（`<<<>>>` → haccLaunch + 剔除 `__global__` 函数体）。
//
// 纯文本变换，无需 EDG/LLVM/GPU。覆盖：2/3/4 参数、无参、嵌套括号、注释/字面量、
// `__global__` 剔除、以及 fail-loud 错误路径。
#include <cstdio>
#include <string>

#include "host/launch_rewrite.hpp"

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

// 便捷：改写并断言成功，返回结果源码。
std::string rewrite_ok(const std::string& src, const char* what) {
    auto r = hacc::host::rewrite_source(src);
    if (!r.ok) {
        std::fprintf(stderr, "FAIL: %s（改写失败：%s）\n", what, r.error.c_str());
        ++g_failures;
        return {};
    }
    return r.source;
}

}  // namespace

int main() {
    using hacc::host::rewrite_source;

    // 1) 2 参数：grid/block，smem=0、stream=nullptr。
    expect_eq(rewrite_ok("kernel<<<grid, block>>>(a, b);", "2 参数"),
              "haccLaunch(__cool_reg_kernel, grid, block, 0, nullptr, a, b);", "2 参数");

    // 2) 3 参数：带 smem。
    expect_eq(rewrite_ok("kernel<<<grid, block, smem>>>(a, b);", "3 参数"),
              "haccLaunch(__cool_reg_kernel, grid, block, smem, nullptr, a, b);", "3 参数");

    // 3) 4 参数：带 smem + stream。
    expect_eq(rewrite_ok("kernel<<<grid, block, smem, stream>>>(a, b);", "4 参数"),
              "haccLaunch(__cool_reg_kernel, grid, block, smem, stream, a, b);", "4 参数");

    // 4) 无参内核。
    expect_eq(rewrite_ok("k<<<1, 1>>>();", "无参"), "haccLaunch(__cool_reg_k, 1, 1, 0, nullptr);",
              "无参");

    // 5) 实参含嵌套括号与逗号（顶层逗号切分不应误切）。
    expect_eq(rewrite_ok("k<<<g, b>>>(f(a, b), c);", "嵌套实参"),
              "haccLaunch(__cool_reg_k, g, b, 0, nullptr, f(a, b), c);", "嵌套实参");

    // 6) 配置段含表达式（如 (n+255)/256）与空格。
    expect_eq(rewrite_ok("k<<< (n + 255) / 256, 256 >>>(p);", "配置表达式"),
              "haccLaunch(__cool_reg_k, (n + 255) / 256, 256, 0, nullptr, p);", "配置表达式");

    // 7) 注释与字符串中的 `<<<` 不应被改写。
    expect_eq(rewrite_ok("// k<<<1,1>>>()\nconst char* s = \"a<<<b\";\n", "注释/字面量"),
              "// k<<<1,1>>>()\nconst char* s = \"a<<<b\";\n", "注释/字面量");

    // 8) 剔除 `__global__` 函数体（只留声明，并剥掉 `__global__` 关键字）。
    expect_eq(
        rewrite_ok("__global__ void tid_kernel(int* out) {\n    *out = 1;\n}\n", "__global__ 剔除"),
        "void tid_kernel(int* out);\n", "__global__ 剔除");

    // 9) `__global__` 与 `<<<>>>` 组合（典型 host 源）。
    {
        const std::string src =
            "__global__ void k(int* out) {\n    *out = 0;\n}\n"
            "int main() {\n    k<<<1, 1>>>(p);\n    return 0;\n}\n";
        const std::string want =
            "void k(int* out);\n"
            "int main() {\n    haccLaunch(__cool_reg_k, 1, 1, 0, nullptr, p);\n    return 0;\n}\n";
        expect_eq(rewrite_ok(src, "组合"), want, "组合");
    }

    // 10) 函数体内含嵌套花括号与字符串（花括号配对需正确）。
    expect_eq(
        rewrite_ok("__global__ void k() { if (1) { const char* s = \"}\"; } }\n", "嵌套花括号"),
        "void k();\n", "嵌套花括号");

    // 11) 纯声明（无函数体）的 `__global__` 剥掉关键字，保留其余声明。
    expect_eq(rewrite_ok("__global__ void k(int* out);\n", "纯声明"), "void k(int* out);\n",
              "纯声明");

    // 12) fail loud：`<<<` 前无内核名。
    {
        auto r = rewrite_source("<<<1, 1>>>();");
        expect(!r.ok, "无内核名应失败");
        expect(!r.error.empty(), "失败应带诊断");
    }

    // 13) fail loud：配置参数个数非法（1 个）。
    {
        auto r = rewrite_source("k<<<1>>>(p);");
        expect(!r.ok, "配置参数个数非法应失败");
    }

    // 14) fail loud：`>>>` 后无实参列表。
    {
        auto r = rewrite_source("k<<<1, 1>>>;");
        expect(!r.ok, "缺实参列表应失败");
    }

    // 15) fail loud：`__global__` 函数体括号不配对。
    {
        auto r = rewrite_source("__global__ void k() { int x = 1;\n");
        expect(!r.ok, "函数体括号不配对应失败");
    }

    if (g_failures == 0) {
        std::printf("test_launch_rewrite: OK\n");
        return 0;
    }
    std::fprintf(stderr, "test_launch_rewrite: %d 处失败\n", g_failures);
    return 1;
}
