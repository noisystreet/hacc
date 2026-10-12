// coolfe++：hacc 的 CUDA 前端（对标 nvcc 的 cudafe++），基于 EDG C++ 前端。
//
// 独立可执行：由 hacc driver 以子进程编排；产出 EDG IL 供 coolicc 消费
// （见 docs/compiler-implementation-plan.md §5、§6.3）。
//
// M0 骨架：仅 --version / -h；EDG 集成尚未接入。
#include <cstdio>
#include <cstring>

#include "support/version.hpp"

namespace {

constexpr const char* kUsage =
    "coolfe++ —— hacc CUDA 前端（对标 cudafe++，基于 EDG C++）。\n"
    "用法: coolfe++ [选项] <input.cu>\n"
    "  --version       打印版本\n"
    "  -h, --help      打印本帮助\n";

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("coolfe++ %s\n", hacc::version());
            return 0;
        }
        if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            std::fputs(kUsage, stdout);
            return 0;
        }
    }
    std::fprintf(stderr, "coolfe++: 尚未接入（M0 骨架）。用 -h 查看用法。\n");
    return 2;
}
