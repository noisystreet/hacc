#include "device/il_read_backend.hpp"

// 未启用 EDG（HACC_WITH_EDG=OFF）时的占位实现：
// 保持 coolicc 可构建（--version/-h），但 --emit=il-list 明确报错而非静默失败。
namespace hacc::device {

bool list_routines(const std::string& /*cil_path*/, std::vector<IlRoutine>* routines_out,
                   std::string* err_out) {
    if (routines_out != nullptr) {
        routines_out->clear();
    }
    if (err_out != nullptr) {
        *err_out = "本构建未启用 EDG 前端（需 -DHACC_WITH_EDG=ON）";
    }
    return false;
}

}  // namespace hacc::device
