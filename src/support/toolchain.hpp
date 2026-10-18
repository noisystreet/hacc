// 主机 C++ 工具链选择（B8）。
//
// 为什么收敛到一处：主机编译器路径被两处消费——driver 的主机编译阶段
// （src/driver/pipeline.cpp）与 coolfe++ 的系统头探测回退（src/frontend/edg_invoke.cpp）。
// 两处各自解析环境变量会让优先级顺序漂移（例如 driver 认 $HACC_HOST_CXX 而前端只认
// $HACC_GXX），故统一走本模块。
#ifndef HACC_SUPPORT_TOOLCHAIN_HPP
#define HACC_SUPPORT_TOOLCHAIN_HPP

#include <string>

namespace hacc::support {

// 解析主机 C++ 编译器路径。
// 优先级：显式参数 > $HACC_HOST_CXX > $HACC_GXX（旧名，保留兼容）> "g++"。
//
// 默认仍是 g++（而非 clang++）：主机桩与 coolrt 头按 GNU C++ 扩展编写，g++ 是已验证
// 的基线路径；clang++ 属可选项，切过去要显式指定（见 docs/TODO.md 的 B8 结论）。
std::string resolve_host_cxx(const std::string& explicit_path);

// 取路径中的文件名部分（诊断文案用）："/usr/bin/clang++" → "clang++"。
std::string basename_of(const std::string& path);

}  // namespace hacc::support

#endif  // HACC_SUPPORT_TOOLCHAIN_HPP
