// hacc 驱动（driver）：选项解析 / 阶段编排 / 子进程调度（对标 nvcc）。
//
// M0 骨架：仅实现 --version / -h 与选项占位；真正的编译流水线
// （coolfe++ → coolicc → 主机编译器）尚未接入。
#include <cstdio>
#include <cstring>

#include "support/version.hpp"

namespace {

constexpr const char* kUsage =
    "hacc —— COOL 的 CUDA C++ 子集编译器（对标 nvcc）\n"
    "用法: hacc [选项] <input.cu>\n"
    "\n"
    "选项:\n"
    "  -o <file>       输出文件\n"
    "  -c              仅编译，不链接\n"
    "  --emit=ptx      设备产物（一期默认，止于 PTX）\n"
    "  -arch=sm_XX     目标架构\n"
    "  -O<N>           优化级别\n"
    "  -keep           保留中间产物\n"
    "  --version       打印版本\n"
    "  -h, --help      打印本帮助\n";

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("hacc %s\n", hacc::version());
            return 0;
        }
        if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            std::fputs(kUsage, stdout);
            return 0;
        }
    }
    std::fprintf(stderr, "hacc: 编译流水线尚未接入（M0 骨架）。用 -h 查看用法。\n");
    return 2;
}
