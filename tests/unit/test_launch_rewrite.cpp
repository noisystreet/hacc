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

// 便捷：device 模式改写（保留 `__global__` 函数体）。
std::string rewrite_device_ok(const std::string& src, const char* what) {
    hacc::host::RewriteOptions opts;
    opts.keep_global_bodies = true;
    auto r = hacc::host::rewrite_source(src, opts);
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

    // 16) device 模式：`__global__` 原样保留（cpfe 需其属性识别内核）。
    expect_eq(
        rewrite_device_ok("__global__ void k(int* out) {\n    *out = 1;\n}\n", "device 保留体"),
        "__global__ void k(int* out) {\n    *out = 1;\n}\n", "device 保留体");

    // 17) device 模式：`<<<>>>` 仍改写，`__global__` 原样保留。
    expect_eq(
        rewrite_device_ok("__global__ void k() {}\nint main() { k<<<1, 1>>>(); }\n", "device 组合"),
        "__global__ void k() {}\nint main() { haccLaunch(__cool_reg_k, 1, 1, 0, nullptr); }\n",
        "device 组合");

    // 18) kernel_names 收集：去重、按首次出现顺序。
    {
        auto r = rewrite_source("a<<<1,1>>>();\nb<<<1,1>>>();\na<<<2,2>>>();\n");
        expect(r.ok, "kernel_names 用例应成功");
        expect(r.kernel_names.size() == 2, "kernel_names 应去重为 2");
        if (r.kernel_names.size() == 2) {
            expect(r.kernel_names[0] == "a", "kernel_names[0] 应为 a");
            expect(r.kernel_names[1] == "b", "kernel_names[1] 应为 b");
        }
    }

    // 19) 设备符号收集：__device__ 变量进 device_names，__device__ 函数不进。
    {
        const std::string src =
            "__device__ int g_counter = 7;\n"
            "__device__ int twice(int v) { return v * 2; }\n"
            "__constant__ float c_scale = 2.0f;\n";
        auto r = hacc::host::rewrite_source(src);
        expect(r.ok, "符号收集 用例应成功");
        expect(r.device_names.size() == 1, "device_names 应只含变量 g_counter");
        if (r.device_names.size() == 1) {
            expect_eq(r.device_names[0], "g_counter", "device_names[0]");
        }
        expect(r.constant_names.size() == 1, "constant_names 应含 c_scale");
        if (r.constant_names.size() == 1) {
            expect_eq(r.constant_names[0], "c_scale", "constant_names[0]");
        }
    }

    // 20) cudaMemcpyFromSymbol / cudaGetSymbolAddress 改写为 hacc 包装调用。
    {
        const std::string src =
            "__device__ int g_counter = 7;\n"
            "void f() {\n"
            "  int h = 0;\n"
            "  void* p = 0;\n"
            "  cudaMemcpyFromSymbol(&h, g_counter, sizeof(int), 0, coolMemcpyDeviceToHost);\n"
            "  cudaGetSymbolAddress(&p, g_counter);\n"
            "}\n";
        std::string out = rewrite_ok(src, "From/取址 改写");
        expect(out.find("__cool_memcpy_from_symbol_g_counter(&h, sizeof(int), 0)") !=
                   std::string::npos,
               "FromSymbol 应改写为包装调用（含 dst/count/offset）");
        expect(out.find("__cool_get_symbol_address_g_counter(&p)") != std::string::npos,
               "GetSymbolAddress 应改写为包装调用");
        expect(out.find("cudaMemcpyFromSymbol") == std::string::npos,
               "改写后不应残留 cudaMemcpyFromSymbol");
        expect(out.find("cudaGetSymbolAddress") == std::string::npos,
               "改写后不应残留 cudaGetSymbolAddress");
    }

    // 21) 未知符号不改写（避免误伤用户自定义同名 API 或外部符号）。
    {
        const std::string src =
            "__device__ int g_counter = 7;\n"
            "void f() { int h = 0; cudaMemcpyFromSymbol(&h, other_sym, 4, 0, "
            "coolMemcpyDeviceToHost); }\n";
        std::string out = rewrite_ok(src, "未知符号不改写");
        expect(out.find("cudaMemcpyFromSymbol") != std::string::npos, "未知符号应保持原样");
    }

    // 22) device 源（keep_global_bodies）：符号 API 替换为 coolSuccess 以保表达式合法。
    {
        const std::string src =
            "__device__ int g_counter = 7;\n"
            "void f() {\n"
            "  int h = 0;\n"
            "  coolError_t a = cudaMemcpyFromSymbol(&h, g_counter, 4, 0, coolMemcpyDeviceToHost);\n"
            "  coolError_t b = cudaMemcpyToSymbol(g_counter, &h, 4, 0, coolMemcpyHostToDevice);\n"
            "}\n";
        std::string out = rewrite_device_ok(src, "device 源符号 API");
        expect(out.find("coolError_t a = coolSuccess") != std::string::npos,
               "device 源 FromSymbol 应替换为 coolSuccess（可用作初值）");
        expect(out.find("coolError_t b = coolSuccess") != std::string::npos,
               "device 源 ToSymbol 应替换为 coolSuccess（可用作初值）");
    }

    // 23) __device__ 变量也可走 cudaMemcpyToSymbol（主机侧写入）。
    {
        const std::string src =
            "__device__ int g_counter = 7;\n"
            "void f() { int nv = 3; cudaMemcpyToSymbol(g_counter, &nv, sizeof(int), 0, "
            "coolMemcpyHostToDevice); }\n";
        std::string out = rewrite_ok(src, "device 变量 ToSymbol");
        expect(
            out.find("__cool_memcpy_to_symbol_g_counter(&nv, sizeof(int), 0)") != std::string::npos,
            "__device__ 变量的 ToSymbol 也应改写为包装调用");
    }

    if (g_failures == 0) {
        std::printf("test_launch_rewrite: OK\n");
        return 0;
    }
    std::fprintf(stderr, "test_launch_rewrite: %d 处失败\n", g_failures);
    return 1;
}
