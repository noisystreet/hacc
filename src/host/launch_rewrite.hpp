// M2.0：主机源码改写器（纯文本变换，无 EDG/LLVM/GPU 依赖）。
//
// 做两件事（见 docs/m2-host-path-plan.md §3.2 / §8.2）：
//   1. `<<<>>>` 启动语法 → `haccLaunch(__cool_reg_<kernel>, grid, block, smem, stream, args...)`；
//   2. 剔除 `__global__` 函数体（只留声明）——host 侧不编译设备代码。
//
// 设计为**纯函数**：输入源码字符串 → 输出改写后字符串，便于独立单测。
// 失败（如 `<<<` 歧义、括号不配对）时返回 ok=false 并给出可读诊断（fail loud，不静默误改）。
#ifndef HACC_HOST_LAUNCH_REWRITE_HPP
#define HACC_HOST_LAUNCH_REWRITE_HPP

#include <string>

namespace hacc::host {

// 改写结果。
struct RewriteResult {
    bool ok = false;     // 改写是否成功
    std::string source;  // 成功时为改写后源码；失败时为空
    std::string error;   // 失败时的可读原因（含大致位置）
};

// 对 CUDA C++ 子集源码做主机侧改写（`<<<>>>` + 剔除 `__global__` 函数体）。
// 纯文本变换，不解析 C++ 语义；子集内要求 `<<<` 仅用于启动语法。
RewriteResult rewrite_source(const std::string& src);

}  // namespace hacc::host

#endif  // HACC_HOST_LAUNCH_REWRITE_HPP
