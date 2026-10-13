// il_to_llvm.cpp —— EDG IL → LLVM IR 翻译适配层（M1.2.2）。
//
// 与 il_read_backend.cpp 同属「EDG 适配层」：EDG 的 IL 表项类型
// （a_routine / a_scope / an_attribute 等）只在本文件内出现，对外仅暴露
// il_to_llvm.hpp 的纯 hacc 接口（emit_ptx_from_il）。见 AGENTS.md 硬约束。
//
// 链接：需 HACC_WITH_EDG=ON（EDG cdisp 对象集）且 HACC_WITH_LLVM=ON（LLVM/NVPTX）。
// 调用序列照抄 EDG 的 cdisp（il_display.c 的 main）：
//   standalone_utility_early_init() → il_read(FILE*) → standalone_utility_late_init()
// 之后 il_header.primary_scope 即 file scope，其 routines 为 routine 链表。
//
// 一期（M1.2.2）范围：只处理带 cool_global 属性的**无参内核**，为每个内核建一个
// 空 Function（调用约定 ptx_kernel，属性 nvptx-kernel），函数体为单个
// BasicBlock + ret void。参数与语句翻译属后续里程碑（M1.2.3+）。
#include "device/il_to_llvm.hpp"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

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

// 读回 .cil 的 file scope。成功返回 file scope 指针；失败返回 nullptr 并写 *err_out。
// 与 il_read_backend.cpp 的读回序列一致（early_init → il_read → late_init）。
a_scope_ptr read_file_scope(const std::string& cil_path, std::string* err_out) {
    FILE* f = std::fopen(cil_path.c_str(), "rb");
    if (f == nullptr) {
        if (err_out != nullptr) {
            *err_out = "无法打开 IL 文件: " + cil_path;
        }
        return nullptr;
    }

    standalone_utility_early_init();
    set_position_to(error_position, 0, SP_COL_UNKNOWN);
    primary_source_file_name = nullptr;

    il_read(f);
    standalone_utility_late_init();
    std::fclose(f);

    a_scope_ptr file_scope = il_header.primary_scope;
    if (file_scope == nullptr && err_out != nullptr) {
        *err_out = "IL 中未找到 file scope（文件可能损坏或非 EDG IL）";
    }
    return file_scope;
}

// 遍历 file scope 的 routine，为每个内核建空 Function。返回内核数。
unsigned build_kernels(a_scope_ptr file_scope, llvm::Module& module) {
    llvm::LLVMContext& ctx = module.getContext();
    // 内核函数类型：一期只处理无参内核 → void ()。
    llvm::FunctionType* fn_type = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), false);

    unsigned kernel_count = 0;
    for (a_routine_ptr r = file_scope->routines; r != nullptr; r = r->next) {
        if (!has_attribute(r->source_corresp.attributes, kKernelAttributeName)) {
            continue;  // 非内核（设备函数等）一期跳过。
        }
        const char* name = r->source_corresp.name;
        if (name == nullptr) {
            continue;  // 无源实体名，无法建具名函数。
        }

        // 调用约定 ptx_kernel（LLVM 的 NVPTX 约定），属性 nvptx-kernel。
        llvm::Function* fn =
            llvm::Function::Create(fn_type, llvm::GlobalValue::ExternalLinkage, name, &module);
        fn->setCallingConv(llvm::CallingConv::PTX_Kernel);
        fn->addFnAttr("nvptx-kernel");

        // 空函数体：单个 BasicBlock + ret void。
        llvm::BasicBlock* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
        llvm::ReturnInst::Create(ctx, entry);

        ++kernel_count;
    }
    return kernel_count;
}

}  // namespace

bool emit_ptx_from_il(const std::string& cil_path, const NvptxOptions& opts, std::string* ptx_out,
                      std::string* err_out) {
    if (ptx_out == nullptr) {
        if (err_out != nullptr) {
            *err_out = "内部错误：ptx_out 为空";
        }
        return false;
    }
    ptx_out->clear();

    a_scope_ptr file_scope = read_file_scope(cil_path, err_out);
    if (file_scope == nullptr) {
        return false;
    }

    // 目标三元组：nvptx64-nvidia-cuda（方案 §6.1）。DataLayout 由后端设置。
    llvm::LLVMContext ctx;
    llvm::Module module("hacc", ctx);
    module.setTargetTriple("nvptx64-nvidia-cuda");

    if (build_kernels(file_scope, module) == 0) {
        if (err_out != nullptr) {
            *err_out = "IL 中未找到内核（带 cool_global 属性的 routine）";
        }
        return false;
    }

    // 序列化为 LLVM IR 文本，复用 M1.1 的 emit_ptx（IR 文本 → PTX）。
    std::string ir_text;
    llvm::raw_string_ostream os(ir_text);
    module.print(os, nullptr);
    os.flush();

    return emit_ptx(ir_text, opts, ptx_out, err_out);
}

}  // namespace hacc::device
