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
    "  -o <file>       输出文件（--emit=exe 时为可执行文件；ptx/il/host 时为对应产物）\n"
    "  -I <dir>        头文件搜索目录（透传给 coolfe++ 与主机编译器）\n"
    "  --host-cxx <p>  主机 C++ 编译器（默认 $HACC_HOST_CXX > $HACC_GXX > g++）\n"
    "  --emit=exe      可执行文件（默认，对齐 nvcc「一步到可执行」）\n"
    "  --emit=ptx      止于 PTX（对齐 nvcc -ptx）\n"
    "  --emit=il       止于 EDG IL（观测用）\n"
    "  --emit=host     止于 host 源（拼桩之前，观测用）\n"
    "  -arch=sm_XX     目标架构（默认 sm_80）\n"
    "  -keep, --keep   保留中间产物（默认成功即删；失败始终保留并列出）\n"
    "  --keep-dir <d>  中间产物目录（默认当前工作目录）\n"
    "                  命名：<stem>.hacc.il（EDG IL） \n"
    "                        <stem>.hacc.cpp（前端 host 源，未拼桩）\n"
    "                        <stem>.hacc.stub.cpp（主机桩）\n"
    "                        <stem>.hacc.gen.cpp（桩 + host，编译输入）\n"
    "                        <stem>.ptx（设备映像）\n"
    "  --version       打印版本\n"
    "  -h, --help      打印本帮助\n";

// 命令行选项。
struct Options {
    hacc::driver::PipelineOptions pipeline;
    bool want_help = false;
    bool want_version = false;
    bool host_cxx_given = false;  // 是否显式给了 --host-cxx
};

// 处理 `--flag=value` 或 `--flag value` 两种写法。
// 返回：1 = 命中（值已写入 dest）；0 = 未命中或缺值（交由调用方兜底报错）。
int take_prefixed_value(const char* a, int* i, int argc, char** argv, const char* flag,
                        std::string* dest) {
    const size_t n = std::strlen(flag);
    if (std::strncmp(a, flag, n) != 0) {
        return 0;
    }
    if (a[n] == '=') {
        *dest = a + n + 1;
        return 1;
    }
    if (a[n] == '\0' && *i + 1 < argc) {
        *dest = argv[++(*i)];
        return 1;
    }
    return 0;
}

// 处理需要单个值的选项（-o / -I / --host-cxx / --keep-dir）。命中则消费参数并返回 true。
// `--xxx=value` 与 `--xxx value` 共用 take_prefixed_value，避免每加一个选项就多两个分支
// （lizard CCN 门禁：阈值 10）。
bool take_value_option(const char* a, int* i, int argc, char** argv, Options* out) {
    if (std::strcmp(a, "-o") == 0 && *i + 1 < argc) {
        out->pipeline.output_path = argv[++(*i)];
        return true;
    }
    if (std::strcmp(a, "-I") == 0 && *i + 1 < argc) {
        out->pipeline.include_dirs.push_back(argv[++(*i)]);
        return true;
    }
    if (take_prefixed_value(a, i, argc, argv, "--host-cxx", &out->pipeline.host_cxx) == 1) {
        out->host_cxx_given = true;
        return true;
    }
    if (take_prefixed_value(a, i, argc, argv, "--keep-dir", &out->pipeline.keep_dir) == 1) {
        return true;
    }
    return false;
}

// 解析 `--emit=<mode>`（调用方已确认前缀）。未知值打印错误并返回 false。
// 单独成函数：避免 parse_args 的分支数（lizard CCN）随可选值增加而膨胀。
bool parse_emit_value(const char* a, hacc::driver::EmitMode* out) {
    const char* v = a + 7;  // 跳过 "--emit="
    if (std::strcmp(v, "exe") == 0) {
        *out = hacc::driver::EmitMode::Exe;
        return true;
    }
    if (std::strcmp(v, "ptx") == 0) {
        *out = hacc::driver::EmitMode::Ptx;
        return true;
    }
    if (std::strcmp(v, "il") == 0) {
        *out = hacc::driver::EmitMode::Il;
        return true;
    }
    if (std::strcmp(v, "host") == 0) {
        *out = hacc::driver::EmitMode::Host;
        return true;
    }
    std::fprintf(stderr, "hacc: 未知的 --emit=%s（可选：exe / ptx / il / host）\n", v);
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
        if (std::strcmp(a, "-keep") == 0 || std::strcmp(a, "--keep") == 0) {
            out->pipeline.keep = true;
            continue;
        }
        if (std::strncmp(a, "--emit=", 7) == 0) {
            if (!parse_emit_value(a, &out->pipeline.emit)) {
                return 2;
            }
            continue;
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
    // 显式给空值视为错误：静默回退到 g++ 会让「我明明指定了 clang++」变成难查的谜团。
    if (o.host_cxx_given && o.pipeline.host_cxx.empty()) {
        std::fprintf(stderr, "hacc: --host-cxx 需要非空路径。用 -h 查看用法。\n");
        return 2;
    }

    const hacc::driver::PipelineResult r = hacc::driver::run_pipeline(o.pipeline);
    if (!r.ok) {
        std::fprintf(stderr, "hacc: %s\n", r.error.c_str());
        return 1;
    }
    return 0;
}
