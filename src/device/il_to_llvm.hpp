#ifndef HACC_DEVICE_IL_TO_LLVM_HPP
#define HACC_DEVICE_IL_TO_LLVM_HPP

#include <string>

#include "device/nvptx_backend.hpp"

namespace hacc::device {

// 把 EDG 二进制 IL（.cil）翻译为 LLVM IR 并发射 PTX（M1.2.2 / M1.2.3）。
//
// 这是「EDG IL → hacc 的 LLVM IR」衔接点的对外接口（AGENTS.md 硬约束）：
// 只暴露 hacc 自己的类型（NvptxOptions / std::string），**不**暴露任何 EDG 或
// LLVM 类型。EDG 头文件仅在 il_to_llvm.cpp 内 include（与 il_read_backend.cpp
// 同属 EDG 适配层）；LLVM 类型也仅在该 .cpp 内使用。
//
// 范围：
//   M1.2.2：带 cool_global 属性的**无参内核** → 空 Function。
//   M1.2.3：标量参数 + 简单语句（赋值/算术/if）。
//     - 参数：tk_routine 的 param_type_list → LLVM 参数（标量 int/float/pointer）。
//     - 语句：stmk_expr / stmk_if / stmk_block / stmk_return / stmk_decl / stmk_init。
//     - 表达式：enk_variable / enk_constant / enk_operation（算术/比较/赋值/取址/解引用）。
//     - 类型：tk_integer（i8..i64）/ tk_float（float/double）/ tk_pointer（ptr）。
//   未覆盖的构造一律报错（fail loud），不静默产出错误 IR。
//
// 成功返回 true 并写入 *ptx_out；失败返回 false 并写入 *err_out。
// 需要 HACC_WITH_EDG=ON 且 HACC_WITH_LLVM=ON；否则走 stub 实现并明确报错。
bool emit_ptx_from_il(const std::string& cil_path, const NvptxOptions& opts, std::string* ptx_out,
                      std::string* err_out);

}  // namespace hacc::device

#endif  // HACC_DEVICE_IL_TO_LLVM_HPP
