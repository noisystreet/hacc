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

bool emit_ptx(const std::string& llvm_ir, const NvptxOptions& opts, std::string* ptx_out,
              std::string* err_out) {
    // 初始化 NVPTX 目标（幂等；多次调用安全）。
    LLVMInitializeNVPTXTargetInfo();
    LLVMInitializeNVPTXTarget();
    LLVMInitializeNVPTXTargetMC();
    LLVMInitializeNVPTXAsmPrinter();

    llvm::LLVMContext ctx;
    llvm::SMDiagnostic diag;
    std::unique_ptr<llvm::Module> mod =
        llvm::parseIR(llvm::MemoryBufferRef(llvm::StringRef(llvm_ir), "<hacc-ir>"), diag, ctx);
    if (!mod) {
        if (err_out) {
            std::string msg;
            llvm::raw_string_ostream os(msg);
            diag.print("coolicc", os);
            *err_out = os.str();
        }
        return false;
    }

    // 目标三元组：nvptx64-nvidia-cuda（方案 §6.1）。
    mod->setTargetTriple("nvptx64-nvidia-cuda");

    std::string triple_err;
    const llvm::Target* target =
        llvm::TargetRegistry::lookupTarget(mod->getTargetTriple(), triple_err);
    if (target == nullptr) {
        if (err_out) *err_out = "lookupTarget failed: " + triple_err;
        return false;
    }

    llvm::TargetOptions target_opts;
    std::string cpu = normalize_arch(opts.arch);
    std::string features;
    auto tm = std::unique_ptr<llvm::TargetMachine>(target->createTargetMachine(
        mod->getTargetTriple(), cpu, features, target_opts, llvm::Reloc::PIC_));
    if (!tm) {
        if (err_out) *err_out = "createTargetMachine failed for cpu=" + cpu;
        return false;
    }
    mod->setDataLayout(tm->createDataLayout());

    if (llvm::verifyModule(*mod, &llvm::errs())) {
        if (err_out) *err_out = "module verification failed";
        return false;
    }

    llvm::SmallString<0> ptx;
    llvm::raw_svector_ostream ptx_os(ptx);
    llvm::legacy::PassManager pm;
    if (tm->addPassesToEmitFile(pm, ptx_os, nullptr, llvm::CodeGenFileType::AssemblyFile)) {
        if (err_out) *err_out = "addPassesToEmitFile failed (no asm printer for target)";
        return false;
    }
    pm.run(*mod);

    if (ptx_out) *ptx_out = std::string(ptx.str());
    return true;
}

}  // namespace hacc::device
