// il_read_backend.cpp —— EDG IL 读回适配层（M1.2.1）。
//
// 这是 hacc 中**唯一**直接 include EDG 头文件的地方：EDG 的 IL 表项类型
// （a_routine / a_scope / an_attribute 等）只在本文件内出现，对外仅暴露
// il_read_backend.hpp 的纯 C++ 接口（IlRoutine）。见 AGENTS.md 硬约束。
//
// 链接：需 HACC_WITH_EDG=ON，链接 EDG 的 cdisp 对象集（见 CMakeLists.txt）。
// 调用序列照抄 EDG 的 cdisp（il_display.c 的 main）：
//   standalone_utility_early_init() → il_read(FILE*) → standalone_utility_late_init()
// 之后 il_header.primary_scope 即 file scope，其 routines 为 routine 链表。
#include "device/il_read_backend.hpp"

#include <cstdio>
#include <cstring>

// EDG 头文件：fe_common.h 必须先于 il.h / il_read.h（提供前置声明与宏）。
#include "fe_common.h"
#include "fe_init.h"
#include "il.h"
#include "il_def.h"
#include "il_read.h"

USING_NAMESPACE_EDG

namespace hacc::device {
namespace {

// 内核属性名（契约见 tools/il_spike/cuda-attributes.md；A2 决策 (a) 按 name 匹配）。
constexpr const char* kKernelAttributeName = "cool_global";

// routine 的 source_corresp.attributes 链中是否含指定名字的属性。
bool has_attribute(an_attribute_ptr attrs, const char* wanted) {
    for (an_attribute_ptr a = attrs; a != nullptr; a = a->next) {
        if (a->name != nullptr && std::strcmp(a->name, wanted) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

bool list_routines(const std::string& cil_path, std::vector<IlRoutine>* routines_out,
                   std::string* err_out) {
    if (routines_out == nullptr) {
        if (err_out != nullptr) {
            *err_out = "内部错误：routines_out 为空";
        }
        return false;
    }
    routines_out->clear();

    FILE* f = std::fopen(cil_path.c_str(), "rb");
    if (f == nullptr) {
        if (err_out != nullptr) {
            *err_out = "无法打开 IL 文件: " + cil_path;
        }
        return false;
    }

    // 与 cdisp 相同的初始化顺序：early_init → il_read → late_init。
    standalone_utility_early_init();
    set_position_to(error_position, 0, SP_COL_UNKNOWN);
    primary_source_file_name = nullptr;

    il_read(f);
    standalone_utility_late_init();
    std::fclose(f);

    a_scope_ptr file_scope = il_header.primary_scope;
    if (file_scope == nullptr) {
        if (err_out != nullptr) {
            *err_out = "IL 中未找到 file scope（文件可能损坏或非 EDG IL）";
        }
        return false;
    }

    for (a_routine_ptr r = file_scope->routines; r != nullptr; r = r->next) {
        IlRoutine entry;
        entry.name = (r->source_corresp.name != nullptr) ? r->source_corresp.name : "(anonymous)";
        entry.is_kernel = has_attribute(r->source_corresp.attributes, kKernelAttributeName);
        routines_out->push_back(std::move(entry));
    }
    return true;
}

}  // namespace hacc::device
