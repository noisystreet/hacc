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

// 返回 flag 首次出现的下标；未找到返回 -1。
int index_of(const std::vector<std::string>& args, const std::string& flag) {
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == flag) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// 返回 value 首次出现的下标；未找到返回 -1。
int index_of_value(const std::vector<std::string>& args, const std::string& value) {
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == value) {
            return static_cast<int>(i);
        }
    }
    return -1;
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

    // 3b) -D__COOLCC__ 无条件存在（对标 __CUDACC__，让运行时头暴露设备端内建）。
    {
        EdgInvokeOptions o;
        o.input_path = "k.cu";
        auto a = build_cpfe_args(o, /*for_text=*/false, "");
        expect(has_flag(a, "-D__COOLCC__"), "-D__COOLCC__ 无条件存在");
    }

    // 3c) extra_args（-I/-D）必须排在输入文件**之前**：cpfe 只识别输入前的选项。
    //     （回归：曾把 extra_args 追加在输入之后，导致 #include <cool/...> 找不到。）
    {
        EdgInvokeOptions o;
        o.input_path = "k.cu";
        o.extra_args = {"-I", "/inc"};
        auto a = build_cpfe_args(o, /*for_text=*/false, "");
        const int inc = index_of(a, "-I");
        const int input = index_of_value(a, "k.cu");
        expect(inc >= 0 && input >= 0 && inc < input, "-I 在输入文件之前");
    }

    // 3d) --sys_include 注入（依赖 gcc 探测；无 gcc 时跳过断言）。
    {
        EdgInvokeOptions o;
        o.input_path = "k.cu";
        auto a = build_cpfe_args(o, /*for_text=*/false, "");
        const bool has_sys = has_flag(a, "--sys_include");
        // 若探测到系统头，则 --sys_include 必须成对出现且值非空。
        if (has_sys) {
            const int idx = index_of(a, "--sys_include");
            expect(idx + 1 < static_cast<int>(a.size()) && !a[idx + 1].empty(),
                   "--sys_include 后跟非空目录");
        }
        // 无 gcc 时不断言（探测返回空，属预期）。
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
