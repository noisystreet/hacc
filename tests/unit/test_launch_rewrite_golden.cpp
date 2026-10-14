// M2.0 golden：对真实 .cu 跑改写器，与期望 host 源逐字节比对。
//
// 用法：test_launch_rewrite_golden <input.cu> <expected.hacc.cpp>
// 读取输入 → rewrite_source → 与期望文件比对；不一致时打印首个差异位置。
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "host/launch_rewrite.hpp"

namespace {

// 读取整个文件；失败返回 false。
bool read_file(const char* path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "用法: %s <input.cu> <expected.hacc.cpp>\n", argv[0]);
        return 2;
    }
    std::string src;
    std::string want;
    if (!read_file(argv[1], src)) {
        std::fprintf(stderr, "无法读取输入：%s\n", argv[1]);
        return 2;
    }
    if (!read_file(argv[2], want)) {
        std::fprintf(stderr, "无法读取期望：%s\n", argv[2]);
        return 2;
    }

    auto r = hacc::host::rewrite_source(src);
    if (!r.ok) {
        std::fprintf(stderr, "改写失败：%s\n", r.error.c_str());
        return 1;
    }
    if (r.source == want) {
        std::printf("test_launch_rewrite_golden: OK\n");
        return 0;
    }

    // 打印首个差异位置，便于定位。
    std::size_t i = 0;
    while (i < r.source.size() && i < want.size() && r.source[i] == want[i]) {
        ++i;
    }
    std::fprintf(stderr, "golden 不一致（首个差异 @%zu）\n  期望: [%s]\n  实际: [%s]\n", i,
                 want.c_str(), r.source.c_str());
    return 1;
}
