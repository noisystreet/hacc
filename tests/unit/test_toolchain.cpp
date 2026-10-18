// 单元测试：主机 C++ 编译器解析（B8）。
//
// 只测纯逻辑（resolve_host_cxx / basename_of），不实际调用编译器。
#include <cstdio>
#include <cstdlib>
#include <string>

#include "support/toolchain.hpp"

namespace {

int g_failures = 0;

void expect(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

// 清空两个相关环境变量，让每条用例都从确定的基线开始。
void clear_env() {
    unsetenv("HACC_HOST_CXX");
    unsetenv("HACC_GXX");
}

}  // namespace

int main() {
    using hacc::support::basename_of;
    using hacc::support::resolve_host_cxx;

    // 1) 无显式参数、无环境变量 → 默认 g++。
    clear_env();
    expect(resolve_host_cxx("") == "g++", "无显式/无环境变量时默认 g++");

    // 2) 旧环境变量 HACC_GXX 仍生效（B8 之前的唯一入口，保留为兼容别名）。
    setenv("HACC_GXX", "/opt/old/g++", 1);
    expect(resolve_host_cxx("") == "/opt/old/g++", "$HACC_GXX 生效");

    // 3) 新环境变量 $HACC_HOST_CXX 优先于旧名。
    setenv("HACC_HOST_CXX", "/opt/new/clang++", 1);
    expect(resolve_host_cxx("") == "/opt/new/clang++", "$HACC_HOST_CXX 优先于 $HACC_GXX");

    // 4) 显式参数优先于环境变量（命令行最高优先级）。
    expect(resolve_host_cxx("/usr/bin/clang++") == "/usr/bin/clang++", "显式参数优先于环境变量");

    // 5) basename_of：诊断文案用（去掉目录部分）。
    expect(basename_of("/usr/bin/clang++") == "clang++", "basename_of 取文件名");
    expect(basename_of("g++") == "g++", "basename_of 裸名原样返回");

    clear_env();
    if (g_failures == 0) {
        std::printf("test_toolchain: OK\n");
        return 0;
    }
    std::fprintf(stderr, "test_toolchain: %d 处失败\n", g_failures);
    return 1;
}
