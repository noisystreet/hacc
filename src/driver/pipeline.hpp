// hacc driver 的编译流水线编排（M1.2.5）。
//
// 一期流水线（`.cu` → PTX）：
//   coolfe++ <input.cu> -o <tmp.cil>            # .cu → EDG IL
//   coolicc --from-il -arch=sm_XX -o <out.ptx> <tmp.cil>   # IL → PTX
//
// 两个子工具与 hacc 同目录（构建产物在一起）；可用环境变量覆盖（见 .cpp）。
// 纯逻辑（参数拼装、路径推导）与执行分离，便于单测。
#ifndef HACC_DRIVER_PIPELINE_HPP
#define HACC_DRIVER_PIPELINE_HPP

#include <string>
#include <vector>

namespace hacc::driver {

// 流水线选项。
struct PipelineOptions {
    std::string input_path;   // 输入 .cu
    std::string output_path;  // 输出 PTX（-o）
    std::string arch = "sm_80";  // -arch=sm_XX
    bool keep = false;        // -keep：保留中间产物（.cil）
    std::vector<std::string> include_dirs;  // -I：透传给 coolfe++
    // 子工具路径（空则按同目录 / 环境变量推导）。
    std::string coolfe_path;
    std::string coolicc_path;
    // hacc 自身路径（argv[0]），用于推导同目录子工具。
    std::string self_path;
};

// 流水线执行结果。
struct PipelineResult {
    bool ok = false;
    std::string error;  // 失败原因（含子工具 stderr）
};

// 推导 coolfe++ 路径：显式 > $HACC_COOLFE > self_path 同目录。
std::string resolve_coolfe_path(const PipelineOptions& opts);

// 推导 coolicc 路径：显式 > $HACC_COOLICC > self_path 同目录。
std::string resolve_coolicc_path(const PipelineOptions& opts);

// 拼装 coolfe++ 参数（不含程序名）：<input.cu> -o <cil>。
std::vector<std::string> build_coolfe_args(const PipelineOptions& opts, const std::string& cil_path);

// 拼装 coolicc 参数（不含程序名）：--from-il -arch=sm_XX -o <ptx> <cil>。
std::vector<std::string> build_coolicc_args(const PipelineOptions& opts,
                                            const std::string& cil_path);

// 执行流水线：.cu → .cil → .ptx。成功返回 ok=true。
// keep=false 时删除中间 .cil。
PipelineResult run_pipeline(const PipelineOptions& opts);

}  // namespace hacc::driver

#endif  // HACC_DRIVER_PIPELINE_HPP
