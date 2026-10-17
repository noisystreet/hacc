#include "device/il_to_llvm.hpp"

// 未启用 EDG 或 LLVM（HACC_WITH_EDG=OFF 或 HACC_WITH_LLVM=OFF）时的占位实现：
// 保持 coolicc 可构建（--version/-h），但 --from-il 明确报错而非静默失败。
namespace hacc::device {

bool emit_ptx_from_il(const std::string& /*cil_path*/, const NvptxOptions& /*opts*/,
                      std::string* ptx_out, std::string* err_out) {
    if (ptx_out != nullptr) {
        ptx_out->clear();
    }
    if (err_out != nullptr) {
        *err_out =
            "本构建未启用 EDG IL → LLVM IR 翻译（需 -DHACC_WITH_EDG=ON 且 -DHACC_WITH_LLVM=ON）";
    }
    return false;
}

bool emit_ir_from_il(const std::string& /*cil_path*/, std::string* ir_out, std::string* err_out) {
    if (ir_out != nullptr) {
        ir_out->clear();
    }
    if (err_out != nullptr) {
        *err_out =
            "本构建未启用 EDG IL → LLVM IR 翻译（需 -DHACC_WITH_EDG=ON 且 -DHACC_WITH_LLVM=ON）";
    }
    return false;
}

}  // namespace hacc::device
