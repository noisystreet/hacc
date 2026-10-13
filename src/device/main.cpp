// coolicc：hacc 的设备端编译器（对标 nvcc 的 cicc）。
//
// 独立可执行：消费 coolfe++ 产出的 EDG IL，降为 LLVM IR → NVPTX → PTX
// （见 docs/compiler-implementation-plan.md §6.1、§6.3）。
//
// M1.1：打通「LLVM IR → PTX」这一跳（NVPTX 后端）。EDG IL 解析（M1.2）尚未接入，
//       故当前以 --emit=ptx 从文件/stdin 读入 LLVM IR 文本并发射 PTX，用于验证后端链路。
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "device/il_read_backend.hpp"
#include "device/nvptx_backend.hpp"
#include "support/version.hpp"

namespace {

constexpr const char* kUsage =
    "coolicc —— hacc 设备端编译器（对标 cicc，LLVM/NVPTX）。\n"
    "用法: coolicc [选项] [input]\n"
    "  --emit=ptx      发射 PTX（M1.1；输入为 LLVM IR 文本）\n"
    "  --emit=il-list  读回 EDG 二进制 IL（.cil），列出 file scope 的 routine（M1.2.1）\n"
    "  -arch=sm_XX     目标架构（默认 sm_80，见 B1）\n"
    "  -o <file>       输出文件（默认 stdout）\n"
    "  --version       打印版本\n"
    "  -h, --help      打印本帮助\n"
    "\n"
    "M1.1：--emit=ptx 输入为 LLVM IR 文本（省略 input 则读 stdin）。\n"
    "M1.2.1：--emit=il-list 输入为 EDG 二进制 IL（.cil）。\n";

std::string read_all(std::istream& in) {
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

int main(int argc, char** argv) {
    std::string input_path;
    std::string output_path;
    std::string emit = "ptx";  // 默认发射 PTX（M1.1）
    hacc::device::NvptxOptions opts;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--version") == 0) {
            std::printf("coolicc %s\n", hacc::version());
            return 0;
        }
        if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
            std::fputs(kUsage, stdout);
            return 0;
        }
        if (std::strncmp(a, "-arch=", 6) == 0) {
            opts.arch = a + 6;
            continue;
        }
        if (std::strcmp(a, "-o") == 0 && i + 1 < argc) {
            output_path = argv[++i];
            continue;
        }
        if (std::strncmp(a, "--emit=", 7) == 0) {
            // 一期支持 ptx（M1.1）与 il-list（M1.2.1）；cubin 属后续阶段（M6）。
            const char* mode = a + 7;
            if (std::strcmp(mode, "ptx") != 0 && std::strcmp(mode, "il-list") != 0) {
                std::fprintf(stderr, "coolicc: 暂不支持 --emit=%s（一期仅 ptx / il-list）\n", mode);
                return 2;
            }
            emit = mode;
            continue;
        }
        if (a[0] == '-') {
            std::fprintf(stderr, "coolicc: 未知选项 %s\n", a);
            return 2;
        }
        input_path = a;
    }

    // M1.2.1：--emit=il-list 从 EDG 二进制 IL 读回并列出 routine。
    if (emit == "il-list") {
        if (input_path.empty()) {
            std::fprintf(stderr, "coolicc: --emit=il-list 需要输入 .cil 文件\n");
            return 2;
        }
        std::vector<hacc::device::IlRoutine> routines;
        std::string err;
        if (!hacc::device::list_routines(input_path, &routines, &err)) {
            std::fprintf(stderr, "coolicc: 读回 IL 失败: %s\n", err.c_str());
            return 1;
        }
        std::string out_text;
        for (const auto& r : routines) {
            out_text += r.name;
            if (r.is_kernel) {
                out_text += "  [kernel]";
            }
            out_text += '\n';
        }
        if (output_path.empty()) {
            std::fwrite(out_text.data(), 1, out_text.size(), stdout);
        } else {
            std::ofstream out(output_path, std::ios::binary);
            if (!out) {
                std::fprintf(stderr, "coolicc: 无法写入 %s\n", output_path.c_str());
                return 1;
            }
            out << out_text;
        }
        return 0;
    }

    std::string ir;
    if (input_path.empty()) {
        ir = read_all(std::cin);
    } else {
        std::ifstream in(input_path, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "coolicc: 无法打开输入 %s\n", input_path.c_str());
            return 1;
        }
        ir = read_all(in);
    }

    std::string ptx;
    std::string err;
    if (!hacc::device::emit_ptx(ir, opts, &ptx, &err)) {
        std::fprintf(stderr, "coolicc: 发射 PTX 失败: %s\n", err.c_str());
        return 1;
    }

    if (output_path.empty()) {
        std::fwrite(ptx.data(), 1, ptx.size(), stdout);
    } else {
        std::ofstream out(output_path, std::ios::binary);
        if (!out) {
            std::fprintf(stderr, "coolicc: 无法写入 %s\n", output_path.c_str());
            return 1;
        }
        out << ptx;
    }
    return 0;
}
