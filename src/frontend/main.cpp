// coolfe++：hacc 的 CUDA 前端（对标 nvcc 的 cudafe++），基于 EDG C++ 前端。
//
// 独立可执行：由 hacc driver 以子进程编排；产出 EDG IL 供 coolicc 消费
// （见 docs/compiler-implementation-plan.md §5、§6.3）。
//
// M1.2.0：打通「.cu → EDG IL」——调用 cpfe 产出
//   ① 二进制 IL（-o，供 coolicc 用 il_read 读回）；
//   ② 人类可读文本 IL（--il_display，编译期 dump，见方案 §7.1）。
// M2.1：新增 --emit-host[=F]——产出**主机源**（`<<<>>>` → haccLaunch + 剔除
//   `__global__` 函数体，见 docs/m2-host-path-plan.md §3.1/§8.2）。
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "frontend/edg_invoke.hpp"
#include "host/launch_rewrite.hpp"
#include "support/version.hpp"

// hacc include 目录（含 coolcc/hacc_device_decls.h）；由 CMake 注入，未注入则为空。
#ifndef HACC_INCLUDE_DIR
#define HACC_INCLUDE_DIR ""
#endif

namespace {

constexpr const char* kUsage =
    "coolfe++ —— hacc CUDA 前端（对标 cudafe++，基于 EDG C++）。\n"
    "用法: coolfe++ [选项] <input.cu>\n"
    "  -o <file>            二进制 IL 输出（默认 <input>.cil）\n"
    "  --emit-il-text[=F]   人类可读文本 IL（默认不产出；省略 F 时为 <input>.il.txt）\n"
    "  --emit-host[=F]      主机源输出（改写后源码，默认 <input>.hacc.cpp）\n"
    "  --emit-host-only[=F] 仅产主机源，不调用 cpfe（纯文本，无需 EDG）\n"
    "  --cpfe <path>        cpfe 可执行路径（默认 $HACC_CPFE 或构建期默认）\n"
    "  --edg-base-dir <dir> EDG base 目录（默认 $HACC_EDG_BASE 或构建期默认）\n"
    "  --host-cxx <path>    主机 C++ 编译器（用于探测系统头；\n"
    "                       默认 $HACC_HOST_CXX > $HACC_GXX > g++）\n"
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

// 替换扩展名但**保留目录前缀**（与 stem_of 不同，不丢目录）。
// 用于「显式请求的辅助产物」——落在输入文件旁边，而不是调用者的 cwd。
std::string sibling_with_ext(const std::string& path, const char* ext) {
    size_t slash = path.find_last_of('/');
    size_t dot = path.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        return path.substr(0, dot) + ext;
    }
    return path + ext;
}

// 命令行选项。
struct Options {
    hacc::frontend::EdgInvokeOptions edg;
    std::string input_path;
    std::string il_output;    // 显式 -o
    std::string il_text;      // 显式 --emit-il-text=...
    bool want_text = false;   // 是否产出文本（默认 false；仅 --emit-il-text 开启）
    std::string host_output;  // 显式 --emit-host=...
    bool want_host = false;   // 是否产出主机源（默认 false）
    bool host_only = false;   // --emit-host-only：仅产主机源，不调用 cpfe
};

// 处理需要单个值的选项（-o / --cpfe / --edg-base-dir / --host-cxx）。
// 命中则消费下一个参数并返回 true；未命中返回 false。
bool take_value_option(const char* a, int* i, int argc, char** argv, Options* out) {
    if (std::strcmp(a, "-o") == 0 && *i + 1 < argc) {
        out->il_output = argv[++(*i)];
        return true;
    }
    if (std::strcmp(a, "--cpfe") == 0 && *i + 1 < argc) {
        out->edg.cpfe_path = argv[++(*i)];
        return true;
    }
    if (std::strcmp(a, "--edg-base-dir") == 0 && *i + 1 < argc) {
        out->edg.edg_base_dir = argv[++(*i)];
        return true;
    }
    if (std::strncmp(a, "--host-cxx=", 11) == 0) {
        out->edg.host_cxx = a + 11;
        return true;
    }
    if (std::strcmp(a, "--host-cxx") == 0 && *i + 1 < argc) {
        out->edg.host_cxx = argv[++(*i)];
        return true;
    }
    return false;
}

// 处理 --emit-il-text[=F]。命中返回 true。
bool take_emit_il_text_option(const char* a, Options* out) {
    if (std::strncmp(a, "--emit-il-text", 14) != 0) {
        return false;
    }
    out->want_text = true;
    if (a[14] == '=') {
        out->il_text = a + 15;
    }
    return true;
}

// 处理 --emit-host[=F] / --emit-host-only[=F]。命中返回 true。
// 注意：先匹配更长的 --emit-host-only，避免被 --emit-host 前缀吞掉。
bool take_emit_host_option(const char* a, Options* out) {
    if (std::strncmp(a, "--emit-host-only", 16) == 0) {
        out->want_host = true;
        out->host_only = true;
        if (a[16] == '=') {
            out->host_output = a + 17;
        }
        return true;
    }
    if (std::strncmp(a, "--emit-host", 11) == 0) {
        out->want_host = true;
        if (a[11] == '=') {
            out->host_output = a + 12;
        }
        return true;
    }
    return false;
}

// 处理 -I <dir> / -D <macro>（透传给 cpfe）。命中返回 true。
bool take_include_option(const char* a, int* i, int argc, char** argv, Options* out) {
    if ((std::strcmp(a, "-I") != 0 && std::strcmp(a, "-D") != 0) || *i + 1 >= argc) {
        return false;
    }
    out->edg.extra_args.push_back(a);
    out->edg.extra_args.push_back(argv[++(*i)]);
    return true;
}

// 解析命令行。返回 0 表示继续；非 0 为应直接返回的退出码（--help/--version/错误）。
int parse_args(int argc, char** argv, Options* out) {
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
        if (take_value_option(a, &i, argc, argv, out)) {
            continue;
        }
        if (take_emit_il_text_option(a, out)) {
            continue;
        }
        if (take_emit_host_option(a, out)) {
            continue;
        }
        if (take_include_option(a, &i, argc, argv, out)) {
            continue;
        }
        if (a[0] == '-') {
            std::fprintf(stderr, "coolfe++: 未知选项 %s\n", a);
            return 2;
        }
        out->input_path = a;
    }
    return -1;  // 继续执行
}

// 推导默认输出名（方案 §7.1 方案 ②）：二进制 IL + 主机源；文本 IL 需显式请求。
void resolve_outputs(Options* o) {
    if (o->il_output.empty()) {
        o->il_output = stem_of(o->input_path) + ".cil";
    }
    // 文本 IL 是调试辅助，**默认不产出**：hacc 驱动每次编译都会调 coolfe++，
    // 默认开启会让每次编译都往调用者 cwd 丢一个 0.5–1.6 MB 的 .il.txt。
    // 显式请求（无 =F）时落在输入文件旁边，而不是 cwd。
    if (o->want_text && o->il_text.empty()) {
        o->il_text = sibling_with_ext(o->input_path, ".il.txt");
    }
    if (o->want_host && o->host_output.empty()) {
        o->host_output = stem_of(o->input_path) + ".hacc.cpp";
    }
}

// 读取整个文件；失败返回 false。
bool read_file(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    out = std::move(content);
    return true;
}

// 写出主机源（改写后源码）。成功返回 true。
bool write_host_source(const std::string& input_path, const std::string& output_path,
                       std::string* error) {
    std::string src;
    if (!read_file(input_path, src)) {
        *error = "无法读取输入：" + input_path;
        return false;
    }
    hacc::host::RewriteResult r = hacc::host::rewrite_source(src);
    if (!r.ok) {
        *error = "源码改写失败：" + r.error;
        return false;
    }
    std::ofstream out(output_path, std::ios::binary);
    if (!out) {
        *error = "无法写入主机源：" + output_path;
        return false;
    }
    out << r.source;
    return static_cast<bool>(out);
}

// 生成 device 源（喂 cpfe）：`<<<>>>` 改写 + **原样保留** `__global__`（cpfe 需其
// 属性识别内核），并在顶部注入 haccLaunch / __cool_reg_* 声明（cpfe 需声明才能解析）。
// 成功返回 true，并把 device 源写入 output_path。
bool write_device_source(const std::string& input_path, const std::string& output_path,
                         std::string* error) {
    std::string src;
    if (!read_file(input_path, src)) {
        *error = "无法读取输入：" + input_path;
        return false;
    }
    hacc::host::RewriteOptions opts;
    opts.keep_global_bodies = true;  // device 源保留内核体（设备 IL 依赖）
    hacc::host::RewriteResult r = hacc::host::rewrite_source(src, opts);
    if (!r.ok) {
        *error = "源码改写失败：" + r.error;
        return false;
    }
    std::ofstream out(output_path, std::ios::binary);
    if (!out) {
        *error = "无法写入 device 源：" + output_path;
        return false;
    }
    // 前置声明头 + 每个内核的注册描述符声明。
    out << "#include <coolcc/hacc_device_decls.h>\n";
    for (const auto& name : r.kernel_names) {
        out << "__COOL_REG_DECL(" << name << ");\n";
    }
    // M3.4：插入的前置行会偏移行号，用 #line 重置回原始 .cu 的行号，
    // 使 EDG 诊断（如 "文件:行: 成因"）指向用户看到的原始源，而非改写后的 device 源。
    // 改写器对 device 源逐字符复制（仅同行内替换 <<<>>>），故 r.source 行号与原始源一致。
    out << "#line 1 \"" << input_path << "\"\n";
    out << r.source;
    return static_cast<bool>(out);
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    const int parse_rc = parse_args(argc, argv, &o);
    if (parse_rc >= 0) {
        return parse_rc;
    }

    if (o.input_path.empty()) {
        std::fprintf(stderr, "coolfe++: 缺少输入文件。用 -h 查看用法。\n");
        return 2;
    }

    resolve_outputs(&o);

    // M2.1：先产主机源（纯文本改写，不依赖 cpfe）。
    if (o.want_host) {
        std::string err;
        if (!write_host_source(o.input_path, o.host_output, &err)) {
            std::fprintf(stderr, "coolfe++: %s\n", err.c_str());
            return 1;
        }
        std::fprintf(stderr, "coolfe++: 主机源 → %s\n", o.host_output.c_str());
    }

    // --emit-host-only：仅产主机源，不调用 cpfe（纯文本，无需 EDG）。
    if (o.host_only) {
        return 0;
    }

    // M2.1：cpfe 吃 **device 改写源**（`<<<>>>` → haccLaunch，保留内核体 + 注入声明），
    // 因为 cpfe 无法解析原始 `<<<>>>`。device 源写到 IL 输出旁的临时文件，用完删除。
    const std::string device_src = o.il_output + ".hacc_device.cu";
    {
        std::string err;
        if (!write_device_source(o.input_path, device_src, &err)) {
            std::fprintf(stderr, "coolfe++: %s\n", err.c_str());
            return 1;
        }
    }

    o.edg.input_path = device_src;
    o.edg.il_output_path = o.il_output;
    o.edg.il_text_path = o.want_text ? o.il_text : std::string();
    // device 源 #include <coolcc/hacc_device_decls.h>，需把 hacc include 目录透传给 cpfe。
    // -I 必须在输入文件之前（cpfe 只识别输入前的选项，见 edg_invoke.cpp）。
    o.edg.extra_args.insert(o.edg.extra_args.begin(), {"-I", HACC_INCLUDE_DIR});

    hacc::frontend::EdgInvokeResult r = hacc::frontend::invoke_cpfe(o.edg);
    std::remove(device_src.c_str());
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

    std::fprintf(stderr, "coolfe++: IL → %s", o.il_output.c_str());
    if (o.want_text) {
        std::fprintf(stderr, "（文本 %s）", o.il_text.c_str());
    }
    std::fputc('\n', stderr);
    return 0;
}
