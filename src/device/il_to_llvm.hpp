#ifndef HACC_DEVICE_IL_TO_LLVM_HPP
#define HACC_DEVICE_IL_TO_LLVM_HPP

#include <string>

#include "device/nvptx_backend.hpp"

namespace hacc::device {

// 把 EDG 二进制 IL（.cil）翻译为 LLVM IR 并发射 PTX（M1.2.2）。
//
// 这是「EDG IL → hacc 的 LLVM IR」衔接点的对外接口（AGENTS.md 硬约束）：
// 只暴露 hacc 自己的类型（NvptxOptions / std::string），**不**暴露任何 EDG 或
// LLVM 类型。EDG 头文件仅在 il_to_llvm.cpp 内 include（与 il_read_backend.cpp
// 同属 EDG 适配层）；LLVM 类型也仅在该 .cpp 内使用。
//
// 一期（M1.2.2）范围：只处理带 cool_global 属性的**无参内核**，为每个内核建
// 一个空 Function（调用约定 ptx_kernel，属性 nvptx-kernel），函数体为
// 单个 BasicBlock + ret void。参数与语句翻译属后续里程碑（M1.2.3+）。
//
// 成功返回 true 并写入 *ptx_out；失败返回 false 并写入 *err_out。
// 需要 HACC_WITH_EDG=ON 且 HACC_WITH_LLVM=ON；否则走 stub 实现并明确报错。
bool emit_ptx_from_il(const std::string& cil_path, const NvptxOptions& opts, std::string* ptx_out,
                      std::string* err_out);

}  // namespace hacc::device

#endif  // HACC_DEVICE_IL_TO_LLVM_HPP
