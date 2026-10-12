// coolicc：hacc 的设备端编译器（对标 nvcc 的 cicc）。
//
// 独立可执行：消费 coolfe++ 产出的 EDG IL，降为 LLVM IR → NVPTX → PTX
// （见 docs/compiler-implementation-plan.md §6.1、§6.3）。
//
// M0 骨架：仅 --version / -h；LLVM/NVPTX 后端尚未接入。
#include <cstdio>
#include <cstring>

#include "support/version.hpp"

namespace {

constexpr const char* kUsage =
    "coolicc —— hacc 设备端编译器（对标 cicc，LLVM/NVPTX）。\n"
    "用法: coolicc [选项] <input>\n"
    "  --version       打印版本\n"
    "  -h, --help      打印本帮助\n";

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("coolicc %s\n", hacc::version());
            return 0;
        }
        if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            std::fputs(kUsage, stdout);
            return 0;
        }
    }
    std::fprintf(stderr, "coolicc: 尚未接入（M0 骨架）。用 -h 查看用法。\n");
    return 2;
}
