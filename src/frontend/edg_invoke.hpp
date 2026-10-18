// coolfe++ 的 EDG 前端调用层（M1.2.0）。
//
// 职责：把「.cu 源 → EDG IL」这一跳封装成可测试的接口——定位 cpfe 可执行与
// EDG base 目录、拼装命令行、以子进程运行、收集 stderr。
//
// 设计要点（见 docs/m1.2-il-to-llvm-plan.md §3.2、§7.1）：
//   - 阶段 A 用「子进程 + IL 文件」隔离：coolfe++ 调 cpfe 产出**二进制 IL**（-o），
//     供 coolicc 用 il_read 读回。
//   - 人类可读 dump：cpfe 的 --il_display 在**编译期**直接 dump 内存 IL，
//     不经过 cdisp（后者对含属性 IL 断言崩溃，见方案 §7.1）。
//   - 路径解析优先级：显式参数 > 环境变量 > 构建期默认值。
#ifndef HACC_FRONTEND_EDG_INVOKE_HPP
#define HACC_FRONTEND_EDG_INVOKE_HPP

#include <string>
#include <vector>

namespace hacc::frontend {

// cpfe 调用参数。
struct EdgInvokeOptions {
    // cpfe 可执行路径。空则回退环境变量 HACC_CPFE，再回退构建期默认值。
    std::string cpfe_path;
    // EDG base 目录（含 lib/predefined_macros.txt）。空则回退 HACC_EDG_BASE，
    // 再回退构建期默认值。
    std::string edg_base_dir;
    // 输入源文件（.cu / .cpp）。
    std::string input_path;
    // 二进制 IL 输出路径（cpfe -o）。空则不产出二进制 IL。
    std::string il_output_path;
    // 人类可读文本 IL 输出路径。空则不产出文本。
    std::string il_text_path;
    // 额外透传给 cpfe 的参数（如 -I、-D）。
    std::vector<std::string> extra_args;
    // 主机 C++ 编译器：仅在 g++ 不可用时用作系统头探测的回退（见
    // probe_system_include_dirs）。空则按 $HACC_HOST_CXX > $HACC_GXX > "g++" 解析。
    std::string host_cxx;
};

// 调用结果。
struct EdgInvokeResult {
    bool ok = false;
    int exit_code = -1;
    // cpfe 的 stderr（诊断信息）。
    std::string stderr_text;
    // 失败时的可读原因（如「找不到 cpfe」）。
    std::string error;
};

// 解析 cpfe 路径：显式 > HACC_CPFE > 构建期默认（HACC_DEFAULT_CPFE）。
// 返回空串表示无法确定。
std::string resolve_cpfe_path(const std::string& explicit_path);

// 解析 EDG base 目录：显式 > HACC_EDG_BASE > 构建期默认（HACC_DEFAULT_EDG_BASE）。
std::string resolve_edg_base_dir(const std::string& explicit_dir);

// 探测系统头目录（供 cpfe 的 --sys_include）。
//
// cpfe 的 `--g++` 模式不自动带系统 include 路径，故 .cu 一旦 #include 标准头
// （如 <stddef.h> / <cstdio>）就会报「cannot open source file」，需显式补齐。
//
// 探测**优先用 g++**，与主机编译器解耦：EDG 按 GCC 语义解析，而 clang++ 的
// resource-dir 内建头用了 __has_feature 等 clang 专有构造，EDG 解析不了。
// g++ 不可用时才退回 host_cxx（尽力而为）。结果按编译器路径缓存；探测失败
// 返回空列表（此时 .cu 不应 include 标准头）。供单测直接调用。
std::vector<std::string> probe_system_include_dirs(const std::string& host_cxx);

// 拼装单次 cpfe 命令行（不含程序名）。便于单测与日志。
//
// for_text=false：二进制 IL 模式，产出顺序
//   --edg_base_dir <base> --g++ -N -D__COOLCC__ [extra...] [--sys_include <dir>...]
//   [-o <il>] <input>
// for_text=true：文本 IL 模式（--il_display，stdout 输出），产出顺序
//   --edg_base_dir <base> --g++ -N -D__COOLCC__ [extra...] [--sys_include <dir>...]
//   --il_display -o <throwaway_il> <input>
//
// 注意：
//   - --il_display 与 -o <真实文件> 不可同时出现；文本模式必须用 throwaway_il
//     抑制 cpfe 把默认名「显示变体」IL 写到当前目录（见 .cpp 中的实测说明）。
//   - extra_args（-I/-D）与 --sys_include 必须放在 <input> **之前**：cpfe 只识别
//     输入文件前的选项（实测：-I 放输入后会被忽略）。
//   - --sys_include 由 probe_system_include_dirs(host_cxx) 自动探测，补齐 cpfe
//     --g++ 模式缺失的系统 include 路径；探测用的编译器须与主机编译相同。
std::vector<std::string> build_cpfe_args(const EdgInvokeOptions& opts, bool for_text,
                                         const std::string& throwaway_il);

// 运行 cpfe。返回结果；ok 表示退出码为 0。
EdgInvokeResult invoke_cpfe(const EdgInvokeOptions& opts);

}  // namespace hacc::frontend

#endif  // HACC_FRONTEND_EDG_INVOKE_HPP
