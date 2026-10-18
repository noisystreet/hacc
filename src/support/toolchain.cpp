// 主机 C++ 工具链选择实现（B8）。
#include "support/toolchain.hpp"

#include <cstdlib>
#include <string>

namespace hacc::support {

namespace {

// 读取环境变量；未设置或为空返回空串。
std::string getenv_str(const char* name) {
    const char* v = std::getenv(name);
    return (v != nullptr && v[0] != '\0') ? std::string(v) : std::string();
}

}  // namespace

std::string resolve_host_cxx(const std::string& explicit_path) {
    if (!explicit_path.empty()) {
        return explicit_path;
    }
    std::string env = getenv_str("HACC_HOST_CXX");
    if (!env.empty()) {
        return env;
    }
    // HACC_GXX 是 B8 之前的唯一环境变量名；语义现已扩展为「主机 C++ 编译器」，
    // 保留为兼容别名，避免既有脚本失效。
    env = getenv_str("HACC_GXX");
    return env.empty() ? std::string("g++") : env;
}

std::string basename_of(const std::string& path) {
    const std::string::size_type slash = path.find_last_of('/');
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

}  // namespace hacc::support
