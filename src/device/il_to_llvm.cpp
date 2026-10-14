// il_to_llvm.cpp —— EDG IL → LLVM IR 翻译适配层（M1.2.2 / M1.2.3）。
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
// 范围：
//   M1.2.2：带 cool_global 属性的**无参内核** → 空 Function。
//   M1.2.3：标量参数 + 简单语句（赋值/算术/if）。
//     - 参数：tk_routine 的 param_type_list → LLVM 参数（标量 int/float/pointer）。
//     - 语句：stmk_expr / stmk_if / stmk_block / stmk_return / stmk_decl / stmk_init。
//     - 表达式：enk_variable / enk_constant / enk_operation（算术/比较/赋值/取址/解引用）。
//     - 类型：tk_integer（i8..i64）/ tk_float（float/double）/ tk_pointer（ptr）。
//   未覆盖的构造一律报错（fail loud），不静默产出错误 IR。
#include "device/il_to_llvm.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
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

// 翻译失败时抛出，由顶层 emit_ptx_from_il 捕获并转成 err_out。
// 用异常而非错误码，避免在深层递归里层层传递错误状态（clean-code：失败显式）。
class TranslateError : public std::exception {
   public:
    explicit TranslateError(std::string msg) : msg_(std::move(msg)) {}
    const char* what() const noexcept override { return msg_.c_str(); }

   private:
    std::string msg_;
};

[[noreturn]] void fail(const std::string& msg) {
    throw TranslateError(msg);
}

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
//
// 注意：**不关闭文件**。EDG 的 read_memory_region（scope_for_routine 内部调用）
// 依赖全局 f_il_input 句柄按需加载函数体 region；关闭文件会导致
// "invalid intermediate language file"。文件句柄由调用方在翻译结束后关闭。
a_scope_ptr read_file_scope(const std::string& cil_path, FILE** file_out, std::string* err_out) {
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

    a_scope_ptr file_scope = il_header.primary_scope;
    if (file_scope == nullptr) {
        std::fclose(f);
        if (err_out != nullptr) {
            *err_out = "IL 中未找到 file scope（文件可能损坏或非 EDG IL）";
        }
        return nullptr;
    }
    if (file_out != nullptr) {
        *file_out = f;
    }
    return file_scope;
}

// 是否为 64 位整型 kind。
bool is_64bit_integer_kind(an_integer_kind kind) {
    switch (kind) {
        case ik_long:
        case ik_unsigned_long:
        case ik_long_long:
        case ik_unsigned_long_long:
            return true;
        default:
            return false;
    }
}

// 是否为 32 位整型 kind。
bool is_32bit_integer_kind(an_integer_kind kind) {
    switch (kind) {
        case ik_int:
        case ik_unsigned_int:
            return true;
        default:
            return false;
    }
}

// 是否为 16 位整型 kind。
bool is_16bit_integer_kind(an_integer_kind kind) {
    switch (kind) {
        case ik_short:
        case ik_unsigned_short:
            return true;
        default:
            return false;
    }
}

// 是否为 8 位整型 kind。
bool is_8bit_integer_kind(an_integer_kind kind) {
    switch (kind) {
        case ik_char:
        case ik_signed_char:
        case ik_unsigned_char:
            return true;
        default:
            return false;
    }
}

// 整型 kind → 位宽（bit）。未覆盖的 kind 返回 0。
unsigned edg_integer_bit_width(an_integer_kind kind) {
    if (is_64bit_integer_kind(kind)) {
        return 64;
    }
    if (is_32bit_integer_kind(kind)) {
        return 32;
    }
    if (is_16bit_integer_kind(kind)) {
        return 16;
    }
    if (is_8bit_integer_kind(kind)) {
        return 8;
    }
    return 0;
}

// 整型 kind → LLVM 整型类型。
llvm::Type* map_integer_type(a_type_ptr t, llvm::LLVMContext& ctx) {
    switch (edg_integer_bit_width(t->variant.integer.int_kind)) {
        case 8:
            return llvm::Type::getInt8Ty(ctx);
        case 16:
            return llvm::Type::getInt16Ty(ctx);
        case 32:
            return llvm::Type::getInt32Ty(ctx);
        case 64:
            return llvm::Type::getInt64Ty(ctx);
        default:
            fail("不支持的整型 kind（M1.2.3 仅覆盖 8/16/32/64 位）");
    }
}

// 浮点 kind → LLVM 浮点类型。
llvm::Type* map_float_type(a_type_ptr t, llvm::LLVMContext& ctx) {
    switch (t->variant.float_kind) {
        case fk_float:
            return llvm::Type::getFloatTy(ctx);
        case fk_double:
            return llvm::Type::getDoubleTy(ctx);
        default:
            fail("不支持的浮点 kind（M1.2.3 仅覆盖 float/double）");
    }
}

// 把 EDG 类型翻译为 LLVM 类型。M1.2.3 只覆盖标量（整型/浮点）与指针。
llvm::Type* map_type(a_type_ptr t, llvm::LLVMContext& ctx) {
    if (t == nullptr) {
        fail("类型为空指针");
    }
    switch (t->kind) {
        case tk_void:
            return llvm::Type::getVoidTy(ctx);
        case tk_integer:
            // int_kind 决定位宽；explicitly_signed 与 int_kind 共同决定符号。
            return map_integer_type(t, ctx);
        case tk_float:
            return map_float_type(t, ctx);
        case tk_pointer:
            // 统一用不透明指针（LLVM 15+ 默认），指向类型不参与 IR 文本。
            return llvm::PointerType::get(ctx, 0);
        case tk_typeref:
            // typedef / 限定符：解引用到目标类型。
            return map_type(t->variant.typeref.type, ctx);
        default:
            fail("不支持的类型 kind（M1.2.3 仅覆盖标量与指针）");
    }
}

// 整型是否无符号（用于比较/除法的符号性）。
bool is_unsigned_integer(a_type_ptr t) {
    if (t == nullptr || t->kind != tk_integer) {
        return false;
    }
    switch (t->variant.integer.int_kind) {
        case ik_unsigned_char:
        case ik_unsigned_short:
        case ik_unsigned_int:
        case ik_unsigned_long:
        case ik_unsigned_long_long:
            return true;
        default:
            return false;
    }
}

// 单个内核的翻译上下文：变量 → LLVM 存储地址的映射，以及当前 IRBuilder。
struct KernelContext {
    llvm::LLVMContext& ctx;
    llvm::IRBuilder<> builder;
    // 变量 → 其「存储地址」（alloca）。变量引用先取地址再 load。
    std::unordered_map<a_variable_ptr, llvm::AllocaInst*> locals;

    explicit KernelContext(llvm::LLVMContext& c) : ctx(c), builder(c) {}
};

// 前向声明（语句与表达式互相递归）。
void translate_statement(a_statement_ptr stmt, KernelContext& kc);
llvm::Value* translate_expr(an_expr_node_ptr expr, KernelContext& kc);

// 取变量的「地址」（alloca）。参数在函数入口已 alloca。
llvm::Value* variable_address(a_variable_ptr var, KernelContext& kc) {
    auto it = kc.locals.find(var);
    if (it != kc.locals.end()) {
        return it->second;
    }
    fail("引用了未声明的变量（M1.2.3 仅支持参数与函数内局部变量）");
}

// 把表达式翻译为「地址」（lvalue）。仅 enk_variable / eok_indirect 支持。
llvm::Value* translate_expr_address(an_expr_node_ptr expr, KernelContext& kc) {
    if (expr == nullptr) {
        fail("取地址时表达式为空");
    }
    if (expr->kind == enk_variable) {
        return variable_address(expr->variant.variable.ptr, kc);
    }
    if (expr->kind == enk_operation && expr->variant.operation.kind == eok_indirect) {
        // *p 的地址就是 p 的值。
        return translate_expr(expr->variant.operation.operands, kc);
    }
    fail("不支持的左值表达式（M1.2.3 仅支持变量与 *p）");
}

// 常量 → LLVM 常量。
llvm::Constant* translate_constant(a_constant_ptr c, llvm::LLVMContext& ctx) {
    if (c == nullptr) {
        fail("常量为空指针");
    }
    switch (c->kind) {
        case ck_integer: {
            llvm::Type* ty = map_type(c->type, ctx);
            // integer_value 是 an_integer_value 结构体（内部多字表示）；
            // 用 EDG 的 conv_integer_value_to_host_large_integer 取宿主整数值。
            a_host_large_integer host_val = 0;
            a_boolean err = FALSE;
            conv_integer_value_to_host_large_integer(&c->variant.integer_value,
                                                     /*is_signed=*/FALSE, &host_val, &err);
            if (err) {
                fail("整型常量超出宿主表示范围");
            }
            uint64_t raw = static_cast<uint64_t>(host_val);
            return llvm::ConstantInt::get(ty, raw, /*isSigned=*/false);
        }
        case ck_float: {
            llvm::Type* ty = map_type(c->type, ctx);
            // an_internal_float_value 是内部字节表示；用 EDG 的 fetch_host_fp_value
            // 取宿主浮点值，再转 double（M1.2.3 只处理 float/double 字面量）。
            a_float_kind fk = (c->type->kind == tk_float) ? c->type->variant.float_kind : fk_double;
            a_host_fp_value hv = fetch_host_fp_value(fk, &c->variant.float_value);
            double d = static_cast<double>(hv);
            return llvm::ConstantFP::get(ty, d);
        }
        default:
            fail("不支持的常量 kind（M1.2.3 仅覆盖整型与浮点字面量）");
    }
}

// 取二元运算符的两个操作数（a, b）；缺失则报错。
an_expr_node_ptr binary_operands(an_expr_node_ptr operands, const char* what) {
    an_expr_node_ptr a = operands;
    an_expr_node_ptr b = (a != nullptr) ? a->next : nullptr;
    if (a == nullptr || b == nullptr) {
        fail(std::string(what) + "缺少操作数");
    }
    return a;
}

// 赋值：operands: [lhs, rhs]；lhs 是左值。
llvm::Value* translate_assign(an_expr_node_ptr operands, KernelContext& kc) {
    an_expr_node_ptr lhs = operands;
    an_expr_node_ptr rhs = (lhs != nullptr) ? lhs->next : nullptr;
    if (lhs == nullptr || rhs == nullptr) {
        fail("赋值运算符缺少操作数");
    }
    llvm::Value* addr = translate_expr_address(lhs, kc);
    llvm::Value* val = translate_expr(rhs, kc);
    kc.builder.CreateStore(val, addr);
    return val;
}

// 浮点二元算术（+ - * /）；取余不支持。
llvm::Value* emit_fp_arith(an_expr_operator_kind op, llvm::Value* va, llvm::Value* vb,
                           KernelContext& kc) {
    switch (op) {
        case eok_add:
            return kc.builder.CreateFAdd(va, vb);
        case eok_subtract:
            return kc.builder.CreateFSub(va, vb);
        case eok_multiply:
            return kc.builder.CreateFMul(va, vb);
        case eok_divide:
            return kc.builder.CreateFDiv(va, vb);
        default:
            fail("浮点不支持取余（M1.2.3）");
    }
}

// 整型二元算术（+ - * / %）；除法/取余按符号性选指令。
llvm::Value* emit_int_arith(an_expr_operator_kind op, bool is_unsigned, llvm::Value* va,
                            llvm::Value* vb, KernelContext& kc) {
    switch (op) {
        case eok_add:
            return kc.builder.CreateAdd(va, vb);
        case eok_subtract:
            return kc.builder.CreateSub(va, vb);
        case eok_multiply:
            return kc.builder.CreateMul(va, vb);
        case eok_divide:
            return is_unsigned ? kc.builder.CreateUDiv(va, vb) : kc.builder.CreateSDiv(va, vb);
        default:
            return is_unsigned ? kc.builder.CreateURem(va, vb) : kc.builder.CreateSRem(va, vb);
    }
}

// 指针算术（p + i / p - i）：用 GEP 按 pointee 类型步进。
// 指针操作数取 ptr 值，整数操作数转 i64 作索引；减法取负索引。
// 仅支持「指针 ± 整数」（不支持指针 - 指针，M2.4 范围）。
llvm::Value* emit_pointer_arith(an_expr_operator_kind op, an_expr_node_ptr a, an_expr_node_ptr b,
                                llvm::Value* va, llvm::Value* vb, KernelContext& kc) {
    const bool a_is_ptr = a->type != nullptr && skip_typerefs(a->type)->kind == tk_pointer;
    const bool b_is_ptr = b->type != nullptr && skip_typerefs(b->type)->kind == tk_pointer;
    if (a_is_ptr == b_is_ptr) {
        fail("指针算术要求一侧为指针、另一侧为整数（不支持指针 - 指针）");
    }
    an_expr_node_ptr ptr_expr = a_is_ptr ? a : b;
    llvm::Value* ptr_val = a_is_ptr ? va : vb;
    llvm::Value* idx_val = a_is_ptr ? vb : va;
    // 指针在减法右侧时（i - p）语义非法，明确报错。
    if (op == eok_psubtract && !a_is_ptr) {
        fail("不支持「整数 - 指针」");
    }
    // 索引统一为 i64（GEP 索引类型）。
    if (idx_val->getType() != kc.builder.getInt64Ty()) {
        idx_val = kc.builder.CreateIntCast(idx_val, kc.builder.getInt64Ty(), /*isSigned=*/true);
    }
    if (op == eok_psubtract) {
        idx_val = kc.builder.CreateNeg(idx_val);
    }
    // pointee 类型：从指针操作数的类型取（不透明指针下 GEP 仍需元素类型）。
    a_type_ptr pointee = skip_typerefs(ptr_expr->type)->variant.pointer.type;
    llvm::Type* elem_ty = map_type(pointee, kc.ctx);
    return kc.builder.CreateGEP(elem_ty, ptr_val, idx_val);
}

// 二元算术（+ - * / %）：按操作数类型选浮点/整型指令。
llvm::Value* translate_arith(an_expr_operator_kind op, an_expr_node_ptr expr, KernelContext& kc) {
    an_expr_node_ptr a = binary_operands(expr->variant.operation.operands, "二元算术运算符");
    an_expr_node_ptr b = a->next;
    llvm::Value* va = translate_expr(a, kc);
    llvm::Value* vb = translate_expr(b, kc);
    // 指针算术：结果为指针类型（p + i / p - i）。
    if (expr->type != nullptr && skip_typerefs(expr->type)->kind == tk_pointer) {
        return emit_pointer_arith(op, a, b, va, vb, kc);
    }
    const bool is_fp = expr->type != nullptr && expr->type->kind == tk_float;
    if (is_fp) {
        return emit_fp_arith(op, va, vb, kc);
    }
    return emit_int_arith(op, is_unsigned_integer(expr->type), va, vb, kc);
}

// 浮点比较指令。
llvm::Value* emit_fp_compare(an_expr_operator_kind op, llvm::Value* va, llvm::Value* vb,
                             KernelContext& kc) {
    switch (op) {
        case eok_lt:
            return kc.builder.CreateFCmpOLT(va, vb);
        case eok_le:
            return kc.builder.CreateFCmpOLE(va, vb);
        case eok_gt:
            return kc.builder.CreateFCmpOGT(va, vb);
        case eok_ge:
            return kc.builder.CreateFCmpOGE(va, vb);
        case eok_eq:
            return kc.builder.CreateFCmpOEQ(va, vb);
        default:
            return kc.builder.CreateFCmpONE(va, vb);
    }
}

// 整型比较指令（按有符号/无符号选）。
llvm::Value* emit_int_compare(an_expr_operator_kind op, bool is_unsigned, llvm::Value* va,
                              llvm::Value* vb, KernelContext& kc) {
    switch (op) {
        case eok_lt:
            return is_unsigned ? kc.builder.CreateICmpULT(va, vb)
                               : kc.builder.CreateICmpSLT(va, vb);
        case eok_le:
            return is_unsigned ? kc.builder.CreateICmpULE(va, vb)
                               : kc.builder.CreateICmpSLE(va, vb);
        case eok_gt:
            return is_unsigned ? kc.builder.CreateICmpUGT(va, vb)
                               : kc.builder.CreateICmpSGT(va, vb);
        case eok_ge:
            return is_unsigned ? kc.builder.CreateICmpUGE(va, vb)
                               : kc.builder.CreateICmpSGE(va, vb);
        case eok_eq:
            return kc.builder.CreateICmpEQ(va, vb);
        default:
            return kc.builder.CreateICmpNE(va, vb);
    }
}

// 按操作数类型（浮点/有符号/无符号）发射比较指令。
llvm::Value* emit_compare(an_expr_operator_kind op, bool is_fp, bool is_unsigned, llvm::Value* va,
                          llvm::Value* vb, KernelContext& kc) {
    if (is_fp) {
        return emit_fp_compare(op, va, vb, kc);
    }
    return emit_int_compare(op, is_unsigned, va, vb, kc);
}

// 比较（< <= > >= == !=）：按操作数类型选浮点/有符号/无符号指令。
llvm::Value* translate_compare(an_expr_operator_kind op, an_expr_node_ptr expr, KernelContext& kc) {
    an_expr_node_ptr a = binary_operands(expr->variant.operation.operands, "比较运算符");
    an_expr_node_ptr b = a->next;
    llvm::Value* va = translate_expr(a, kc);
    llvm::Value* vb = translate_expr(b, kc);
    const bool is_fp = a->type != nullptr && a->type->kind == tk_float;
    const bool is_unsigned = is_unsigned_integer(a->type);
    return emit_compare(op, is_fp, is_unsigned, va, vb, kc);
}

// 标量间转换：整型↔整型、整型↔浮点、浮点↔浮点。
llvm::Value* translate_cast(an_expr_node_ptr expr, KernelContext& kc) {
    an_expr_node_ptr operand = expr->variant.operation.operands;
    llvm::Value* v = translate_expr(operand, kc);
    llvm::Type* dst = map_type(expr->type, kc.ctx);
    llvm::Type* src = v->getType();
    if (src == dst) {
        return v;
    }
    const bool src_fp = src->isFloatingPointTy();
    const bool dst_fp = dst->isFloatingPointTy();
    if (src_fp && dst_fp) {
        return kc.builder.CreateFPCast(v, dst);
    }
    if (!src_fp && !dst_fp) {
        return kc.builder.CreateIntCast(v, dst, /*isSigned=*/!is_unsigned_integer(operand->type));
    }
    if (!src_fp && dst_fp) {
        return is_unsigned_integer(operand->type) ? kc.builder.CreateUIToFP(v, dst)
                                                  : kc.builder.CreateSIToFP(v, dst);
    }
    return is_unsigned_integer(expr->type) ? kc.builder.CreateFPToUI(v, dst)
                                           : kc.builder.CreateFPToSI(v, dst);
}

// 一元运算：取负 / 取反 / 取址 / 解引用。
llvm::Value* translate_unary(an_expr_operator_kind op, an_expr_node_ptr expr, KernelContext& kc) {
    an_expr_node_ptr operand = expr->variant.operation.operands;
    switch (op) {
        case eok_indirect: {
            // *p：取 p 的值作为地址，再 load。
            llvm::Value* addr = translate_expr(operand, kc);
            return kc.builder.CreateLoad(map_type(expr->type, kc.ctx), addr);
        }
        case eok_address_of:
            // &x：取 x 的地址。
            return translate_expr_address(operand, kc);
        case eok_negate: {
            llvm::Value* v = translate_expr(operand, kc);
            const bool is_fp = expr->type != nullptr && expr->type->kind == tk_float;
            return is_fp ? kc.builder.CreateFNeg(v) : kc.builder.CreateNeg(v);
        }
        default: {
            llvm::Value* v = translate_expr(operand, kc);
            return kc.builder.CreateNot(v);
        }
    }
}

// ---- M1.2.4：内建变量与内建函数 ----
//
// 内建变量（threadIdx/blockIdx/blockDim/gridDim）在 IL 中是 file-scope variable，
// 成员访问 threadIdx.x 表现为 eok_dot_field（基对象 enk_variable + enk_field）。
// 翻译器按**名字**识别（与 A2 决策 (a) 一致，见 edg-il-reference §5.6），
// 映射到 NVVM 特殊寄存器读取 intrinsic（llvm.nvvm.read.ptx.sreg.*）。
//
// 内建函数 __syncthreads 在 IL 中是 eok_call（operand 为 enk_routine），
// 按名字映射到 llvm.nvvm.barrier0。

// 内建变量名 → NVVM 特殊寄存器名（不含维度后缀）。
// 返回 nullptr 表示不是内建变量。
const char* builtin_sreg_base(const char* name) {
    if (name == nullptr) {
        return nullptr;
    }
    if (std::strcmp(name, "threadIdx") == 0) {
        return "tid";
    }
    if (std::strcmp(name, "blockIdx") == 0) {
        return "ctaid";
    }
    if (std::strcmp(name, "blockDim") == 0) {
        return "ntid";
    }
    if (std::strcmp(name, "gridDim") == 0) {
        return "nctaid";
    }
    return nullptr;
}

// 成员名（x/y/z）→ 维度后缀；非 x/y/z 返回 nullptr。
const char* field_dimension_suffix(const char* name) {
    if (name == nullptr) {
        return nullptr;
    }
    if (std::strcmp(name, "x") == 0) {
        return "x";
    }
    if (std::strcmp(name, "y") == 0) {
        return "y";
    }
    if (std::strcmp(name, "z") == 0) {
        return "z";
    }
    return nullptr;
}

// 发射对 NVVM 特殊寄存器的读取：llvm.nvvm.read.ptx.sreg.<base>.<dim>。
// 该 intrinsic 返回 i32（与 CUDA 内建变量的 unsigned int 成员一致）。
llvm::Value* emit_sreg_read(const char* base, const char* dim, KernelContext& kc) {
    const std::string fn_name =
        std::string("llvm.nvvm.read.ptx.sreg.") + base + "." + dim;
    llvm::Function* fn = kc.builder.GetInsertBlock()->getParent()->getParent()->getFunction(
        fn_name);
    if (fn == nullptr) {
        llvm::FunctionType* fn_ty =
            llvm::FunctionType::get(llvm::Type::getInt32Ty(kc.ctx), /*isVarArg=*/false);
        fn = llvm::Function::Create(fn_ty, llvm::GlobalValue::ExternalLinkage, fn_name,
                                    kc.builder.GetInsertBlock()->getParent()->getParent());
    }
    return kc.builder.CreateCall(fn);
}

// 翻译内建变量的成员访问（threadIdx.x 等）。
// 返回 nullptr 表示「不是内建变量成员访问」，交由调用方走普通路径。
llvm::Value* try_translate_builtin_field(an_expr_node_ptr expr, KernelContext& kc) {
    an_expr_node_ptr base = expr->variant.operation.operands;
    if (base == nullptr || base->kind != enk_variable) {
        return nullptr;
    }
    a_variable_ptr var = base->variant.variable.ptr;
    const char* base_name = (var != nullptr) ? var->source_corresp.name : nullptr;
    const char* sreg = builtin_sreg_base(base_name);
    if (sreg == nullptr) {
        return nullptr;
    }
    an_expr_node_ptr field = base->next;
    if (field == nullptr || field->kind != enk_field) {
        fail("内建变量的成员访问缺少字段（M1.2.4 仅支持 threadIdx.x 等）");
    }
    const char* dim = field_dimension_suffix(field->variant.field.ptr->source_corresp.name);
    if (dim == nullptr) {
        fail("内建变量仅支持 .x/.y/.z 成员（M1.2.4）");
    }
    return emit_sreg_read(sreg, dim, kc);
}

// 翻译内建函数调用（__syncthreads 等）。
// 返回 nullptr 表示「不是内建函数调用」，交由调用方走普通路径。
llvm::Value* try_translate_builtin_call(an_expr_node_ptr expr, KernelContext& kc) {
    an_expr_node_ptr callee = expr->variant.operation.operands;
    if (callee == nullptr || callee->kind != enk_routine) {
        return nullptr;
    }
    a_routine_ptr r = callee->variant.routine.ptr;
    const char* name = (r != nullptr) ? r->source_corresp.name : nullptr;
    if (name == nullptr || std::strcmp(name, "__syncthreads") != 0) {
        return nullptr;
    }
    // __syncthreads() 无参；映射到 llvm.nvvm.barrier0（void()）。
    if (callee->next != nullptr) {
        fail("__syncthreads 不接受参数（M1.2.4）");
    }
    llvm::Module* module = kc.builder.GetInsertBlock()->getParent()->getParent();
    llvm::Function* fn = module->getFunction("llvm.nvvm.barrier0");
    if (fn == nullptr) {
        llvm::FunctionType* fn_ty = llvm::FunctionType::get(llvm::Type::getVoidTy(kc.ctx),
                                                            /*isVarArg=*/false);
        fn = llvm::Function::Create(fn_ty, llvm::GlobalValue::ExternalLinkage,
                                    "llvm.nvvm.barrier0", module);
    }
    return kc.builder.CreateCall(fn);
}

// 运算符类别（用于把多 case 的 dispatch 收敛为少量分支）。
enum class OpClass { Assign, Arith, Compare, Cast, Unary, DotField, Call, Unsupported };

// 是否为二元算术运算符（+ - * / %，含指针算术 padd/psubtract）。
bool is_arith_operator(an_expr_operator_kind op) {
    switch (op) {
        case eok_add:
        case eok_subtract:
        case eok_multiply:
        case eok_divide:
        case eok_remainder:
        // 指针算术：EDG 用专门运算符（padd/psubtract），非 eok_add/subtract。
        case eok_padd:
        case eok_psubtract:
            return true;
        default:
            return false;
    }
}

// 是否为比较运算符（< <= > >= == !=）。
bool is_compare_operator(an_expr_operator_kind op) {
    switch (op) {
        case eok_lt:
        case eok_le:
        case eok_gt:
        case eok_ge:
        case eok_eq:
        case eok_ne:
            return true;
        default:
            return false;
    }
}

// 把 EDG 运算符 kind 归类。
OpClass classify_operator(an_expr_operator_kind op) {
    if (op == eok_assign) {
        return OpClass::Assign;
    }
    if (is_arith_operator(op)) {
        return OpClass::Arith;
    }
    if (is_compare_operator(op)) {
        return OpClass::Compare;
    }
    if (op == eok_cast) {
        return OpClass::Cast;
    }
    if (op == eok_indirect || op == eok_address_of || op == eok_negate || op == eok_not) {
        return OpClass::Unary;
    }
    if (op == eok_dot_field) {
        return OpClass::DotField;
    }
    if (op == eok_call) {
        return OpClass::Call;
    }
    return OpClass::Unsupported;
}

// 运算符翻译主入口：按运算符类别分派到具体翻译函数。
llvm::Value* translate_operation(an_expr_node_ptr expr, KernelContext& kc) {
    const an_expr_operator_kind op = expr->variant.operation.kind;
    switch (classify_operator(op)) {
        case OpClass::Assign:
            return translate_assign(expr->variant.operation.operands, kc);
        case OpClass::Arith:
            return translate_arith(op, expr, kc);
        case OpClass::Compare:
            return translate_compare(op, expr, kc);
        case OpClass::Cast:
            return translate_cast(expr, kc);
        case OpClass::Unary:
            return translate_unary(op, expr, kc);
        case OpClass::DotField: {
            // threadIdx.x 等内建变量成员访问；非内建则报错（M1.2.4 不覆盖普通结构体成员）。
            llvm::Value* v = try_translate_builtin_field(expr, kc);
            if (v == nullptr) {
                fail("不支持的成员访问（M1.2.4 仅覆盖内建变量 threadIdx/blockIdx/blockDim/gridDim）");
            }
            return v;
        }
        case OpClass::Call: {
            // __syncthreads 等内建函数调用；非内建则报错（M1.2.4 不覆盖普通函数调用）。
            llvm::Value* v = try_translate_builtin_call(expr, kc);
            if (v == nullptr) {
                fail("不支持的函数调用（M1.2.4 仅覆盖内建函数 __syncthreads）");
            }
            return v;
        }
        default:
            fail("不支持的运算符（M1.2.3 仅覆盖算术/比较/赋值/取址/解引用/转换）");
    }
}

// 表达式翻译主入口：返回 rvalue（变量引用会 load）。
llvm::Value* translate_expr(an_expr_node_ptr expr, KernelContext& kc) {
    if (expr == nullptr) {
        fail("表达式为空指针");
    }
    switch (expr->kind) {
        case enk_variable: {
            llvm::Value* addr = variable_address(expr->variant.variable.ptr, kc);
            return kc.builder.CreateLoad(map_type(expr->type, kc.ctx), addr);
        }
        case enk_constant:
            return translate_constant(expr->variant.constant.ptr, kc.ctx);
        case enk_operation:
            return translate_operation(expr, kc);
        default:
            fail("不支持的表达式 kind（M1.2.3 仅覆盖变量/常量/运算）");
    }
}

// 声明语句：为每个实体建 alloca（局部变量）。
// entities 是 tagged pointer 链表；只处理 iek_variable。
void translate_decl(a_statement_ptr stmt, KernelContext& kc) {
    for (an_il_entity_list_entry_ptr e = stmt->variant.decl.entities; e != nullptr; e = e->next) {
        if (e->entity.kind != iek_variable) {
            continue;
        }
        a_variable_ptr v = reinterpret_cast<a_variable_ptr>(e->entity.ptr);
        if (kc.locals.count(v) != 0) {
            continue;
        }
        llvm::Type* ty = map_type(v->type, kc.ctx);
        llvm::AllocaInst* slot = kc.builder.CreateAlloca(ty, nullptr, v->source_corresp.name);
        kc.locals[v] = slot;
    }
}

// 动态初始化：int c = a + b; 走这里（dik_expression）。
void translate_init(a_statement_ptr stmt, KernelContext& kc) {
    a_dynamic_init_ptr di = stmt->variant.dynamic_init;
    if (di == nullptr) {
        fail("stmk_init 缺少 dynamic_init");
    }
    if (di->kind != dik_expression) {
        fail("不支持的初始化 kind（M1.2.3 仅覆盖 dik_expression）");
    }
    a_variable_ptr var = di->variable;
    if (var == nullptr) {
        fail("stmk_init 的 dynamic_init 未指向变量");
    }
    auto it = kc.locals.find(var);
    if (it == kc.locals.end()) {
        // 声明语句可能缺失（如参数初始化），补建 alloca。
        llvm::Type* ty = map_type(var->type, kc.ctx);
        llvm::AllocaInst* slot = kc.builder.CreateAlloca(ty, nullptr, var->source_corresp.name);
        it = kc.locals.emplace(var, slot).first;
    }
    llvm::Value* val = translate_expr(di->variant.expression, kc);
    kc.builder.CreateStore(val, it->second);
}

// if 语句：条件转 i1，建 then/else/end 三块并递归翻译分支。
void translate_if(a_statement_ptr stmt, KernelContext& kc) {
    llvm::Value* cond = translate_expr(stmt->expr, kc);
    // 条件已标准化为整型/布尔；转成 i1 供 br 使用。
    if (!cond->getType()->isIntegerTy(1)) {
        cond = kc.builder.CreateICmpNE(cond, llvm::ConstantInt::get(cond->getType(), 0));
    }
    llvm::Function* fn = kc.builder.GetInsertBlock()->getParent();
    llvm::BasicBlock* then_bb = llvm::BasicBlock::Create(kc.ctx, "if.then", fn);
    llvm::BasicBlock* else_bb = llvm::BasicBlock::Create(kc.ctx, "if.else", fn);
    llvm::BasicBlock* end_bb = llvm::BasicBlock::Create(kc.ctx, "if.end", fn);

    kc.builder.CreateCondBr(cond, then_bb, else_bb);

    kc.builder.SetInsertPoint(then_bb);
    translate_statement(stmt->variant.if_stmt.then_statement, kc);
    kc.builder.CreateBr(end_bb);

    kc.builder.SetInsertPoint(else_bb);
    translate_statement(stmt->variant.if_stmt.else_statement, kc);
    kc.builder.CreateBr(end_bb);

    kc.builder.SetInsertPoint(end_bb);
}

// 语句翻译。
void translate_statement(a_statement_ptr stmt, KernelContext& kc) {
    if (stmt == nullptr) {
        return;
    }
    switch (stmt->kind) {
        case stmk_block:
            // 语句块：顺序翻译 statements 链。
            for (a_statement_ptr s = stmt->variant.block.statements; s != nullptr; s = s->next) {
                translate_statement(s, kc);
            }
            return;
        case stmk_expr:
            translate_expr(stmt->expr, kc);
            return;
        case stmk_decl:
            translate_decl(stmt, kc);
            return;
        case stmk_init:
            translate_init(stmt, kc);
            return;
        case stmk_if:
            translate_if(stmt, kc);
            return;
        case stmk_return:
            // 内核返回 void；M1.2.3 只处理无返回值内核。
            kc.builder.CreateRetVoid();
            return;
        case stmk_empty:
            return;
        default:
            fail("不支持的语句 kind（M1.2.3 仅覆盖 block/expr/decl/init/if/return/empty）");
    }
}

// 取函数体 scope：用 EDG 的 scope_for_routine（il.h:3992）。
// 它内部会按需 read_memory_region 加载函数体所在 region，再返回顶层 scope。
// 直接读 il_header.function_def_table 会拿到 nil（函数体 region 未加载）。
a_scope_ptr find_function_scope(a_routine_ptr r) {
    if (r->function_def_number == 0) {
        return nullptr;
    }
    return scope_for_routine(r);
}

// 取函数体 scope 的第 idx 个参数变量（按声明顺序）。
a_variable_ptr nth_parameter(a_scope_ptr body_scope, unsigned idx) {
    unsigned i = 0;
    for (a_variable_ptr v = body_scope->variant.routine.parameters; v != nullptr; v = v->next) {
        if (i == idx) {
            return v;
        }
        ++i;
    }
    return nullptr;
}

// 从 routine 类型构建 LLVM 函数类型（返回类型 + 参数类型列表）。
llvm::FunctionType* make_kernel_function_type(a_type_ptr fn_type, llvm::LLVMContext& ctx) {
    llvm::Type* ret_ty = map_type(fn_type->variant.routine.return_type, ctx);
    std::vector<llvm::Type*> param_tys;
    a_routine_type_supplement_ptr supp = fn_type->variant.routine.extra_info;
    if (supp != nullptr) {
        for (a_param_type_ptr p = supp->param_type_list; p != nullptr; p = p->next) {
            param_tys.push_back(map_type(p->type, ctx));
        }
    }
    return llvm::FunctionType::get(ret_ty, param_tys, false);
}

// 为每个参数建 alloca 并 store 入参（便于取址与赋值），
// 并把 EDG 参数变量与 alloca 关联（按声明顺序）。
void bind_parameters(llvm::Function* fn, a_scope_ptr body_scope, KernelContext& kc) {
    unsigned idx = 0;
    for (llvm::Argument& arg : fn->args()) {
        llvm::AllocaInst* slot = kc.builder.CreateAlloca(arg.getType(), nullptr, arg.getName());
        kc.builder.CreateStore(&arg, slot);
        if (body_scope != nullptr) {
            a_variable_ptr pv = nth_parameter(body_scope, idx);
            if (pv != nullptr) {
                kc.locals[pv] = slot;
            }
        }
        ++idx;
    }
}

// 为内核建 Function：参数 + 函数体。
llvm::Function* build_kernel(a_routine_ptr r, llvm::Module& module) {
    llvm::LLVMContext& ctx = module.getContext();
    const char* name = r->source_corresp.name;
    if (name == nullptr) {
        fail("内核无源实体名，无法建具名函数");
    }

    // 函数类型：从 routine 类型取返回类型与参数列表。
    a_type_ptr fn_type = r->type;
    if (fn_type == nullptr || fn_type->kind != tk_routine) {
        fail("内核的 routine 类型无效");
    }
    llvm::FunctionType* llvm_fn_ty = make_kernel_function_type(fn_type, ctx);
    llvm::Function* fn =
        llvm::Function::Create(llvm_fn_ty, llvm::GlobalValue::ExternalLinkage, name, &module);
    fn->setCallingConv(llvm::CallingConv::PTX_Kernel);
    fn->addFnAttr("nvptx-kernel");

    // 函数体：entry block + 参数 alloca + 语句翻译。
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
    KernelContext kc(ctx);
    kc.builder.SetInsertPoint(entry);

    // 函数体 scope（经 function_def_number 定位）。
    a_scope_ptr body_scope = nullptr;
    if (r->function_def_number != 0) {
        body_scope = find_function_scope(r);
    }

    bind_parameters(fn, body_scope, kc);

    // 翻译函数体顶层 block。
    if (body_scope != nullptr && body_scope->assoc_block != nullptr) {
        translate_statement(body_scope->assoc_block, kc);
    }

    // 若最后一条语句未终止（如无显式 return），补 ret void。
    if (kc.builder.GetInsertBlock()->getTerminator() == nullptr) {
        kc.builder.CreateRetVoid();
    }
    return fn;
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

    FILE* il_file = nullptr;
    a_scope_ptr file_scope = read_file_scope(cil_path, &il_file, err_out);
    if (file_scope == nullptr) {
        return false;
    }

    // 目标三元组：nvptx64-nvidia-cuda（方案 §6.1）。DataLayout 由后端设置。
    llvm::LLVMContext ctx;
    llvm::Module module("hacc", ctx);
    module.setTargetTriple("nvptx64-nvidia-cuda");

    unsigned kernel_count = 0;
    try {
        for (a_routine_ptr r = file_scope->routines; r != nullptr; r = r->next) {
            if (!has_attribute(r->source_corresp.attributes, kKernelAttributeName)) {
                continue;  // 非内核（设备函数等）一期跳过。
            }
            build_kernel(r, module);
            ++kernel_count;
        }
    } catch (const TranslateError& e) {
        std::fclose(il_file);
        if (err_out != nullptr) {
            *err_out = std::string("IL → LLVM 翻译失败: ") + e.what();
        }
        return false;
    }
    std::fclose(il_file);

    if (kernel_count == 0) {
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
