// M2.0：主机源码改写器（纯文本变换，无 EDG/LLVM/GPU 依赖）。
//
// 做两件事（见 docs/m2-host-path-plan.md §3.2 / §8.2）：
//   1. `<<<>>>` 启动语法 → `haccLaunch(__cool_reg_<kernel>, grid, block, smem, stream, args...)`；
//   2. 处理 `__global__`：host 源剔除函数体（只留声明）；device 源原样保留（见下）。
//
// 设计为**纯函数**：输入源码字符串 → 输出改写后源码，便于独立单测。
// 失败（如 `<<<` 歧义、括号不配对）时返回 ok=false 并给出可读诊断（fail loud，不静默误改）。
//
// M2.1：新增 `keep_global_bodies` 开关——device 源（喂 cpfe）需**保留** `__global__`
//   函数体（设备 IL 依赖内核体），host 源（喂 g++）才剔除。并回传 `kernel_names`
//   （`<<<>>>` 引用到的内核名），供调用方生成 `__cool_reg_<name>` 声明。
#ifndef HACC_HOST_LAUNCH_REWRITE_HPP
#define HACC_HOST_LAUNCH_REWRITE_HPP

#include <string>
#include <vector>

namespace hacc::host {

// 改写选项。
struct RewriteOptions {
    // true：保留 `__global__` 函数体（device 源，供 cpfe 产设备 IL）；
    // false：剔除函数体、只留声明（host 源，供 g++ 编译）。
    bool keep_global_bodies = false;
};

// 改写结果。
struct RewriteResult {
    bool ok = false;     // 改写是否成功
    std::string source;  // 成功时为改写后源码；失败时为空
    std::string error;   // 失败时的可读原因（含大致位置）
    // `<<<>>>` 引用到的内核名（去重、按首次出现顺序）。供生成注册描述声明。
    std::vector<std::string> kernel_names;
};

// 对 CUDA C++ 子集源码做改写（`<<<>>>` → haccLaunch；按选项处理 `__global__` 函数体）。
// 纯文本变换，不解析 C++ 语义；子集内要求 `<<<` 仅用于启动语法。
RewriteResult rewrite_source(const std::string& src, const RewriteOptions& options = {});

}  // namespace hacc::host

#endif  // HACC_HOST_LAUNCH_REWRITE_HPP
