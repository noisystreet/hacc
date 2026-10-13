#include "device/nvptx_backend.hpp"

#include <memory>

#include "llvm/ADT/SmallString.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"

namespace hacc::device {
namespace {

// 把 "sm_80" 归一为 NVPTX 的 -mcpu 值（NVPTX 接受 "sm_80" 形式）。
std::string normalize_arch(const std::string& arch) {
    if (arch.empty()) return "sm_80";
    return arch;
}

}  // namespace

// 解析 LLVM IR 文本为 Module；失败时把诊断写入 *err_out 并返回 nullptr。
std::unique_ptr<llvm::Module> parse_ir_module(const std::string& llvm_ir, llvm::LLVMContext& ctx,
                                              std::string* err_out) {
    llvm::SMDiagnostic diag;
    std::unique_ptr<llvm::Module> mod =
        llvm::parseIR(llvm::MemoryBufferRef(llvm::StringRef(llvm_ir), "<hacc-ir>"), diag, ctx);
    if (!mod && err_out) {
        std::string msg;
        llvm::raw_string_ostream os(msg);
        diag.print("coolicc", os);
        *err_out = os.str();
    }
    return mod;
}

// 为 nvptx64-nvidia-cuda 创建目标机；失败时写 *err_out 并返回 nullptr。
std::unique_ptr<llvm::TargetMachine> make_nvptx_target_machine(const std::string& triple,
                                                               const std::string& cpu,
                                                               std::string* err_out) {
    std::string triple_err;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, triple_err);
    if (target == nullptr) {
        if (err_out) *err_out = "lookupTarget failed: " + triple_err;
        return nullptr;
    }
    llvm::TargetOptions target_opts;
    std::string features;
    auto tm = std::unique_ptr<llvm::TargetMachine>(
        target->createTargetMachine(triple, cpu, features, target_opts, llvm::Reloc::PIC_));
    if (!tm && err_out) *err_out = "createTargetMachine failed for cpu=" + cpu;
    return tm;
}

// 用目标机把 Module 发射为 PTX 文本；失败时写 *err_out 并返回 false。
bool emit_ptx_from_module(llvm::Module& mod, llvm::TargetMachine& tm, std::string* ptx_out,
                          std::string* err_out) {
    llvm::SmallString<0> ptx;
    llvm::raw_svector_ostream ptx_os(ptx);
    llvm::legacy::PassManager pm;
    if (tm.addPassesToEmitFile(pm, ptx_os, nullptr, llvm::CodeGenFileType::AssemblyFile)) {
        if (err_out) *err_out = "addPassesToEmitFile failed (no asm printer for target)";
        return false;
    }
    pm.run(mod);
    if (ptx_out) *ptx_out = std::string(ptx.str());
    return true;
}

bool emit_ptx(const std::string& llvm_ir, const NvptxOptions& opts, std::string* ptx_out,
              std::string* err_out) {
    // 初始化 NVPTX 目标（幂等；多次调用安全）。
    LLVMInitializeNVPTXTargetInfo();
    LLVMInitializeNVPTXTarget();
    LLVMInitializeNVPTXTargetMC();
    LLVMInitializeNVPTXAsmPrinter();

    llvm::LLVMContext ctx;
    std::unique_ptr<llvm::Module> mod = parse_ir_module(llvm_ir, ctx, err_out);
    if (!mod) {
        return false;
    }

    // 目标三元组：nvptx64-nvidia-cuda（方案 §6.1）。
    mod->setTargetTriple("nvptx64-nvidia-cuda");

    std::string cpu = normalize_arch(opts.arch);
    auto tm = make_nvptx_target_machine(mod->getTargetTriple(), cpu, err_out);
    if (!tm) {
        return false;
    }
    mod->setDataLayout(tm->createDataLayout());

    if (llvm::verifyModule(*mod, &llvm::errs())) {
        if (err_out) *err_out = "module verification failed";
        return false;
    }

    return emit_ptx_from_module(*mod, *tm, ptx_out, err_out);
}

}  // namespace hacc::device
