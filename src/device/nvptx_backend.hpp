#ifndef HACC_DEVICE_NVPTX_BACKEND_HPP
#define HACC_DEVICE_NVPTX_BACKEND_HPP

#include <string>

namespace hacc::device {

// 设备端后端配置（M1：最小集）。
struct NvptxOptions {
    // 目标架构，如 "sm_80"（B1 默认）。映射到 NVPTX 的 -mcpu。
    std::string arch = "sm_80";
    // 优化级别（0..3），一期透传给 LLVM。
    int opt_level = 0;
};

// 把 LLVM IR 文本编译为 PTX 文本（目标 nvptx64-nvidia-cuda）。
//
// 成功返回 true 并写入 *ptx_out；失败返回 false 并写入 *err_out。
// M1.1：仅做「LLVM IR → PTX」这一跳，不涉及 EDG IL 解析（M1.2）。
bool emit_ptx(const std::string& llvm_ir, const NvptxOptions& opts, std::string* ptx_out,
              std::string* err_out);

}  // namespace hacc::device

#endif  // HACC_DEVICE_NVPTX_BACKEND_HPP
