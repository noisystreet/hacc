// 单元测试：coolfe++ 的 cpfe 命令行拼装（M1.2.0）。
//
// 只测纯逻辑（build_cpfe_args），不实际调用 cpfe——后者由 golden 测试覆盖。
#include <cstdio>
#include <string>
#include <vector>

#include "frontend/edg_invoke.hpp"

namespace {

int g_failures = 0;

void expect(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

// 在 args 中查找相邻的 <flag> <value> 对。
bool has_pair(const std::vector<std::string>& args, const std::string& flag,
              const std::string& value) {
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == flag && args[i + 1] == value) {
            return true;
        }
    }
    return false;
}

bool has_flag(const std::vector<std::string>& args, const std::string& flag) {
    for (const auto& a : args) {
        if (a == flag) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main() {
    using hacc::frontend::build_cpfe_args;
    using hacc::frontend::EdgInvokeOptions;

    // 1) 基本：base + --g++ + -N + 输入（二进制模式）。
    {
        EdgInvokeOptions o;
        o.edg_base_dir = "/base";
        o.input_path = "k.cu";
        auto a = build_cpfe_args(o, /*for_text=*/false, "");
        expect(has_pair(a, "--edg_base_dir", "/base"), "base dir 透传");
        expect(has_flag(a, "--g++"), "--g++ 存在");
        expect(has_flag(a, "-N"), "-N 存在");
        expect(!has_flag(a, "--il_display"), "二进制模式不加 --il_display");
        expect(!has_flag(a, "-o"), "无二进制输出时不加 -o");
        expect(a.back() == "k.cu", "输入在末尾");
    }

    // 2) 二进制 IL 模式：-o 指向 .cil，且**不带** --il_display。
    //    （--il_display 与 -o 同用会让 -o 写出无效 IL，见 edg_invoke.cpp 实测说明。）
    {
        EdgInvokeOptions o;
        o.edg_base_dir = "/base";
        o.input_path = "k.cu";
        o.il_output_path = "k.cil";
        o.il_text_path = "k.il.txt";
        auto a = build_cpfe_args(o, /*for_text=*/false, "");
        expect(!has_flag(a, "--il_display"), "二进制模式不带 --il_display");
        expect(has_pair(a, "-o", "k.cil"), "-o 指向二进制 IL");
        expect(a.back() == "k.cu", "输入仍在末尾");
    }

    // 2b) 文本 IL 模式：--il_display，且 -o /dev/null（抑制默认 .cil 写入）。
    {
        EdgInvokeOptions o;
        o.edg_base_dir = "/base";
        o.input_path = "k.cu";
        o.il_output_path = "k.cil";
        o.il_text_path = "k.il.txt";
        auto a = build_cpfe_args(o, /*for_text=*/true, "/tmp/throwaway.cil");
        expect(has_flag(a, "--il_display"), "文本模式加 --il_display");
        expect(has_pair(a, "-o", "/tmp/throwaway.cil"), "文本模式 -o 指向丢弃文件");
        expect(a.back() == "k.cu", "输入仍在末尾");
    }

    // 3) extra_args 透传（-I / -D）。
    {
        EdgInvokeOptions o;
        o.edg_base_dir = "/base";
        o.input_path = "k.cu";
        o.extra_args = {"-I", "/inc", "-D", "FOO=1"};
        auto a = build_cpfe_args(o, /*for_text=*/false, "");
        expect(has_pair(a, "-I", "/inc"), "-I 透传");
        expect(has_pair(a, "-D", "FOO=1"), "-D 透传");
    }

    // 4) 无 base：不产生 --edg_base_dir（由 cpfe 自行处理）。
    {
        EdgInvokeOptions o;
        o.input_path = "k.cu";
        auto a = build_cpfe_args(o, /*for_text=*/false, "");
        expect(!has_flag(a, "--edg_base_dir"), "无 base 时不加 --edg_base_dir");
    }

    if (g_failures == 0) {
        std::printf("test_edg_invoke: OK\n");
        return 0;
    }
    std::fprintf(stderr, "test_edg_invoke: %d 处失败\n", g_failures);
    return 1;
}
