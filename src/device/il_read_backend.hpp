#ifndef HACC_DEVICE_IL_READ_BACKEND_HPP
#define HACC_DEVICE_IL_READ_BACKEND_HPP

#include <string>
#include <vector>

namespace hacc::device {

// 从 EDG 二进制 IL（.cil）读回的一个 routine 条目。
//
// 这是 hacc 自己的类型：EDG 的 IL 表项类型（a_routine 等）**不得**泄漏到本头文件，
// 以遵守 AGENTS.md「EDG API 须经适配层隔离」的硬约束。
struct IlRoutine {
    // routine 名（source_corresp.name）；无对应源实体时为 "(anonymous)"。
    std::string name;
    // 是否为内核：source_corresp.attributes 链中含 name == "cool_global"。
    bool is_kernel = false;
};

// 读回 .cil 的 file scope，列出其中的 routine（M1.2.1）。
//
// 成功返回 true 并写入 *routines_out；失败返回 false 并写入 *err_out。
// 需要 HACC_WITH_EDG=ON（链接 EDG 前端库）；否则走 stub 实现并明确报错。
bool list_routines(const std::string& cil_path, std::vector<IlRoutine>* routines_out,
                   std::string* err_out);

}  // namespace hacc::device

#endif  // HACC_DEVICE_IL_READ_BACKEND_HPP
