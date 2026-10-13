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

// 拼装单次 cpfe 命令行（不含程序名）。便于单测与日志。
//
// for_text=false：二进制 IL 模式，产出顺序
//   --edg_base_dir <base> --g++ -N [-o <il>] <input> [extra...]
// for_text=true：文本 IL 模式（--il_display，stdout 输出），产出顺序
//   --edg_base_dir <base> --g++ -N --il_display -o <throwaway_il> <input> [extra...]
//
// 注意：--il_display 与 -o <真实文件> 不可同时出现；文本模式必须用 throwaway_il
// 抑制 cpfe 把默认名「显示变体」IL 写到当前目录（见 .cpp 中的实测说明）。
std::vector<std::string> build_cpfe_args(const EdgInvokeOptions& opts, bool for_text,
                                         const std::string& throwaway_il);

// 运行 cpfe。返回结果；ok 表示退出码为 0。
EdgInvokeResult invoke_cpfe(const EdgInvokeOptions& opts);

}  // namespace hacc::frontend

#endif  // HACC_FRONTEND_EDG_INVOKE_HPP
