// coolfe++：hacc 的 CUDA 前端（对标 nvcc 的 cudafe++），基于 EDG C++ 前端。
//
// 独立可执行：由 hacc driver 以子进程编排；产出 EDG IL 供 coolicc 消费
// （见 docs/compiler-implementation-plan.md §5、§6.3）。
//
// M1.2.0：打通「.cu → EDG IL」——调用 cpfe 产出
//   ① 二进制 IL（-o，供 coolicc 用 il_read 读回）；
//   ② 人类可读文本 IL（--il_display，编译期 dump，见方案 §7.1）。
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "frontend/edg_invoke.hpp"
#include "support/version.hpp"

namespace {

constexpr const char* kUsage =
    "coolfe++ —— hacc CUDA 前端（对标 cudafe++，基于 EDG C++）。\n"
    "用法: coolfe++ [选项] <input.cu>\n"
    "  -o <file>            二进制 IL 输出（默认 <input>.cil）\n"
    "  --emit-il-text[=F]   人类可读文本 IL 输出（默认 <input>.il.txt）\n"
    "  --cpfe <path>        cpfe 可执行路径（默认 $HACC_CPFE 或构建期默认）\n"
    "  --edg-base-dir <dir> EDG base 目录（默认 $HACC_EDG_BASE 或构建期默认）\n"
    "  -I <dir> / -D <macro>  透传给 cpfe\n"
    "  --version            打印版本\n"
    "  -h, --help           打印本帮助\n";

// 去掉目录前缀，取文件名主干（用于默认输出名）。
std::string stem_of(const std::string& path) {
    size_t slash = path.find_last_of('/');
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    return (dot == std::string::npos) ? base : base.substr(0, dot);
}

}  // namespace

int main(int argc, char** argv) {
    hacc::frontend::EdgInvokeOptions opts;
    std::string input_path;
    std::string il_output;   // 显式 -o
    std::string il_text;     // 显式 --emit-il-text=...
    bool want_text = false;  // 是否产出文本（默认 true）
    bool text_explicit = false;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--version") == 0) {
            std::printf("coolfe++ %s\n", hacc::version());
            return 0;
        }
        if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
            std::fputs(kUsage, stdout);
            return 0;
        }
        if (std::strcmp(a, "-o") == 0 && i + 1 < argc) {
            il_output = argv[++i];
            continue;
        }
        if (std::strcmp(a, "--cpfe") == 0 && i + 1 < argc) {
            opts.cpfe_path = argv[++i];
            continue;
        }
        if (std::strcmp(a, "--edg-base-dir") == 0 && i + 1 < argc) {
            opts.edg_base_dir = argv[++i];
            continue;
        }
        if (std::strncmp(a, "--emit-il-text", 14) == 0) {
            want_text = true;
            text_explicit = true;
            if (a[14] == '=') {
                il_text = a + 15;
            }
            continue;
        }
        if ((std::strcmp(a, "-I") == 0 || std::strcmp(a, "-D") == 0) && i + 1 < argc) {
            opts.extra_args.push_back(a);
            opts.extra_args.push_back(argv[++i]);
            continue;
        }
        if (a[0] == '-') {
            std::fprintf(stderr, "coolfe++: 未知选项 %s\n", a);
            return 2;
        }
        input_path = a;
    }

    if (input_path.empty()) {
        std::fprintf(stderr, "coolfe++: 缺少输入文件。用 -h 查看用法。\n");
        return 2;
    }

    // 默认产出：二进制 IL + 文本 IL（方案 §7.1 方案 ②）。
    if (il_output.empty()) {
        il_output = stem_of(input_path) + ".cil";
    }
    if (!text_explicit) {
        want_text = true;
    }
    if (want_text && il_text.empty()) {
        il_text = stem_of(input_path) + ".il.txt";
    }

    opts.input_path = input_path;
    opts.il_output_path = il_output;
    opts.il_text_path = want_text ? il_text : std::string();

    hacc::frontend::EdgInvokeResult r = hacc::frontend::invoke_cpfe(opts);
    if (!r.stderr_text.empty()) {
        std::fputs(r.stderr_text.c_str(), stderr);
    }
    if (!r.ok) {
        if (!r.error.empty()) {
            std::fprintf(stderr, "coolfe++: %s\n", r.error.c_str());
        } else {
            std::fprintf(stderr, "coolfe++: cpfe 失败（退出码 %d）\n", r.exit_code);
        }
        return 1;
    }

    std::fprintf(stderr, "coolfe++: IL → %s", il_output.c_str());
    if (want_text) {
        std::fprintf(stderr, "（文本 %s）", il_text.c_str());
    }
    std::fputc('\n', stderr);
    return 0;
}
