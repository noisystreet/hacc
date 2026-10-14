// hacc 驱动（driver）：选项解析 / 阶段编排 / 子进程调度（对标 nvcc）。
//
// M1.2.5：打通端到端 `.cu` → PTX——编排 coolfe++（.cu → EDG IL）与
// coolicc（IL → PTX）。两个子工具与 hacc 同目录（构建产物在一起）。
#include <cstdio>
#include <cstring>
#include <string>

#include "driver/pipeline.hpp"
#include "support/version.hpp"

namespace {

constexpr const char* kUsage =
    "hacc —— COOL 的 CUDA C++ 子集编译器（对标 nvcc）\n"
    "用法: hacc [选项] <input.cu>\n"
    "\n"
    "选项:\n"
    "  -o <file>       输出文件（--emit=ptx 时为 PTX）\n"
    "  -I <dir>        头文件搜索目录（透传给 coolfe++）\n"
    "  --emit=ptx      设备产物（一期默认，止于 PTX）\n"
    "  -arch=sm_XX     目标架构（默认 sm_80）\n"
    "  -keep           保留中间产物（.cil）\n"
    "  --version       打印版本\n"
    "  -h, --help      打印本帮助\n";

// 命令行选项。
struct Options {
    hacc::driver::PipelineOptions pipeline;
    bool want_help = false;
    bool want_version = false;
};

// 处理需要单个值的选项（-o / -I）。命中则消费下一个参数并返回 true。
bool take_value_option(const char* a, int* i, int argc, char** argv, Options* out) {
    if (std::strcmp(a, "-o") == 0 && *i + 1 < argc) {
        out->pipeline.output_path = argv[++(*i)];
        return true;
    }
    if (std::strcmp(a, "-I") == 0 && *i + 1 < argc) {
        out->pipeline.include_dirs.push_back(argv[++(*i)]);
        return true;
    }
    return false;
}

// 解析命令行。返回 0 表示继续；非 0 为应直接返回的退出码。
int parse_args(int argc, char** argv, Options* out) {
    // argv[0] 用于推导同目录子工具。
    if (argc > 0) {
        out->pipeline.self_path = argv[0];
    }
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--version") == 0) {
            out->want_version = true;
            return 0;
        }
        if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
            out->want_help = true;
            return 0;
        }
        if (take_value_option(a, &i, argc, argv, out)) {
            continue;
        }
        if (std::strncmp(a, "-arch=", 6) == 0) {
            out->pipeline.arch = a + 6;
            continue;
        }
        if (std::strcmp(a, "-keep") == 0) {
            out->pipeline.keep = true;
            continue;
        }
        if (std::strcmp(a, "--emit=ptx") == 0) {
            continue;  // 一期唯一产物，显式接受。
        }
        if (a[0] == '-') {
            std::fprintf(stderr, "hacc: 未知选项 %s\n", a);
            return 2;
        }
        out->pipeline.input_path = a;
    }
    return -1;  // 继续执行
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    const int parse_rc = parse_args(argc, argv, &o);
    if (parse_rc >= 0) {
        if (o.want_version) {
            std::printf("hacc %s\n", hacc::version());
        } else if (o.want_help) {
            std::fputs(kUsage, stdout);
        }
        return parse_rc;
    }

    if (o.pipeline.input_path.empty()) {
        std::fprintf(stderr, "hacc: 缺少输入文件。用 -h 查看用法。\n");
        return 2;
    }
    if (o.pipeline.output_path.empty()) {
        std::fprintf(stderr, "hacc: 缺少输出文件（-o）。用 -h 查看用法。\n");
        return 2;
    }

    const hacc::driver::PipelineResult r = hacc::driver::run_pipeline(o.pipeline);
    if (!r.ok) {
        std::fprintf(stderr, "hacc: %s\n", r.error.c_str());
        return 1;
    }
    return 0;
}
