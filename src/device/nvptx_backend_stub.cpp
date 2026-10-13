#include "device/nvptx_backend.hpp"

// 未启用 LLVM 后端（HACC_WITH_LLVM=OFF）时的占位实现：
// 保持 coolicc 可构建（--version/-h），但 --emit=ptx 明确报错而非静默失败。
namespace hacc::device {

bool emit_ptx(const std::string& /*llvm_ir*/, const NvptxOptions& /*opts*/,
              std::string* /*ptx_out*/, std::string* err_out) {
    if (err_out) {
        *err_out = "本构建未启用 LLVM/NVPTX 后端（需 -DHACC_WITH_LLVM=ON）";
    }
    return false;
}

}  // namespace hacc::device
