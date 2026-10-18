// hacc driver 的编译流水线编排实现（M1.2.5 / M2.3）。
#include "driver/pipeline.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "host/launch_rewrite.hpp"
#include "host/stub_gen.hpp"
#include "support/process.hpp"
#include "support/toolchain.hpp"

namespace hacc::driver {

namespace {

// 读取环境变量；未设置或为空返回空串。
std::string getenv_str(const char* name) {
    const char* v = std::getenv(name);
    return (v != nullptr && v[0] != '\0') ? std::string(v) : std::string();
}

// 取路径所在目录（含末尾 '/'）；无 '/' 返回空串（表示当前目录）。
std::string dir_of(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return (slash == std::string::npos) ? std::string() : path.substr(0, slash + 1);
}

// 推导子工具路径：显式 > 环境变量 > self_path 同目录。
std::string resolve_tool(const std::string& explicit_path, const char* env_name,
                         const std::string& self_path, const char* tool_name) {
    if (!explicit_path.empty()) {
        return explicit_path;
    }
    std::string env = getenv_str(env_name);
    if (!env.empty()) {
        return env;
    }
    // 同目录：<self_dir>/<tool_name>。self_path 无目录时退化为裸名（经 PATH 解析）。
    return dir_of(self_path) + tool_name;
}

// 中间 .cil 路径：由输出路径派生（<output>.cil）。
std::string cil_path_for(const PipelineOptions& opts) {
    return opts.output_path + ".cil";
}

// 中间 host 源路径：由输出路径派生（<output>.hacc.cpp）。
std::string host_path_for(const PipelineOptions& opts) {
    return opts.output_path + ".hacc.cpp";
}

// 中间 PTX 路径（Exe 模式）：由输出路径派生（<output>.ptx）。
std::string ptx_path_for(const PipelineOptions& opts) {
    return opts.output_path + ".ptx";
}

// 读取整个文件；失败返回 false。
bool read_file(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// 写出整个文件；失败返回 false。
bool write_file(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out << content;
    return static_cast<bool>(out);
}

// 推导 COOLRuntime 根目录：显式 > $HACC_COOLRT_DIR > 构建期默认 > self_path 同级 ../coolruntime。
std::string resolve_coolrt_root(const PipelineOptions& opts) {
    if (!opts.coolrt_include.empty()) {
        // 显式给了 include 目录，取其父目录作为根（<root>/include）。
        std::string inc = opts.coolrt_include;
        while (!inc.empty() && inc.back() == '/') {
            inc.pop_back();
        }
        const size_t slash = inc.find_last_of('/');
        return (slash == std::string::npos) ? std::string() : inc.substr(0, slash);
    }
    std::string env = getenv_str("HACC_COOLRT_DIR");
    if (!env.empty()) {
        return env;
    }
#ifdef HACC_DEFAULT_COOLRT_DIR
    // 构建期注入（CMake 的 HACC_COOLRUNTIME_DIR）。
    if (std::string(HACC_DEFAULT_COOLRT_DIR).size() > 0) {
        return HACC_DEFAULT_COOLRT_DIR;
    }
#endif
    // 回退：self_path 同级的 ../coolruntime（hacc 与 coolruntime 通常并列）。
    return dir_of(opts.self_path) + "../coolruntime";
}

// 拼桩：读 host 源 + PTX，跑改写器拿内核名，生成桩并拼到 host 源**顶部**。
// 桩须在用户代码之前（haccLaunch / __cool_reg_* 先声明，见 M2.2b 实测）。
// 成功返回 true；失败填 error。
bool stitch_stubs(const PipelineOptions& opts, const std::string& host_path,
                  const std::string& ptx_path, std::string* error) {
    std::string host_src;
    if (!read_file(host_path, host_src)) {
        *error = "无法读取主机源：" + host_path;
        return false;
    }
    std::string ptx;
    if (!read_file(ptx_path, ptx)) {
        *error = "无法读取 PTX：" + ptx_path;
        return false;
    }
    // 内核名：对原始 .cu 跑改写器（纯文本，与 coolfe++ 同源逻辑）。
    std::string cu_src;
    if (!read_file(opts.input_path, cu_src)) {
        *error = "无法读取输入：" + opts.input_path;
        return false;
    }
    hacc::host::RewriteResult r = hacc::host::rewrite_source(cu_src);
    if (!r.ok) {
        *error = "源码改写失败（取内核名）：" + r.error;
        return false;
    }
    // 设备符号描述符（size 由 coolrt 后端经 cuModuleGetGlobal 解析，
    // 此处仅占位 0；符号名用于按名解析）。
    // __constant__ 与 __device__ 分开收集：前者只需 To 方向（M3.2），
    // 后者主机侧可读可写，需 To/From/取址三种包装。
    std::vector<hacc::host::SymbolSpec> constants;
    for (const std::string& nm : r.constant_names) {
        constants.push_back(hacc::host::SymbolSpec{nm, 0});
    }
    std::vector<hacc::host::SymbolSpec> devices;
    for (const std::string& nm : r.device_names) {
        devices.push_back(hacc::host::SymbolSpec{nm, 0});
    }
    std::string stubs = hacc::host::generate_stubs(r.kernel_names, ptx, constants, devices);
    if (!write_file(host_path, stubs + host_src)) {
        *error = "无法写回主机源：" + host_path;
        return false;
    }
    return true;
}

}  // namespace

std::string resolve_coolfe_path(const PipelineOptions& opts) {
    return resolve_tool(opts.coolfe_path, "HACC_COOLFE", opts.self_path, "coolfe++");
}

std::string resolve_coolicc_path(const PipelineOptions& opts) {
    return resolve_tool(opts.coolicc_path, "HACC_COOLICC", opts.self_path, "coolicc");
}

std::string resolve_host_cxx_path(const PipelineOptions& opts) {
    return support::resolve_host_cxx(opts.host_cxx);
}

std::vector<std::string> build_coolfe_args(const PipelineOptions& opts,
                                           const std::string& cil_path) {
    // coolfe++ <input.cu> -o <cil> [-I <dir> ...]
    // -I 放在输入文件之前（cpfe 只识别输入前的选项，见 frontend/edg_invoke.cpp）。
    // 自动加 coolrt include（.cu 常 #include <cool/...>）。
    std::vector<std::string> args;
    args.push_back("-I");
    args.push_back(resolve_coolrt_root(opts) + "/include");
    for (const auto& dir : opts.include_dirs) {
        args.push_back("-I");
        args.push_back(dir);
    }
    // 主机编译器透传：coolfe++ 仅在 g++ 不可用时用它做系统头探测回退
    // （正常情况探测固定走 g++，见 frontend/edg_invoke.cpp 的说明）。
    args.push_back("--host-cxx");
    args.push_back(resolve_host_cxx_path(opts));
    args.push_back(opts.input_path);
    args.push_back("-o");
    args.push_back(cil_path);
    return args;
}

std::vector<std::string> build_coolfe_host_args(const PipelineOptions& opts,
                                                const std::string& cil_path,
                                                const std::string& host_path) {
    // coolfe++ <input.cu> --emit-host=<host> -o <cil> [-I <dir> ...]
    std::vector<std::string> args;
    args.push_back("-I");
    args.push_back(resolve_coolrt_root(opts) + "/include");
    for (const auto& dir : opts.include_dirs) {
        args.push_back("-I");
        args.push_back(dir);
    }
    // 同上：主机编译器透传给 coolfe++（供系统头探测的回退路径）。
    args.push_back("--host-cxx");
    args.push_back(resolve_host_cxx_path(opts));
    args.push_back(opts.input_path);
    args.push_back("--emit-host=" + host_path);
    args.push_back("-o");
    args.push_back(cil_path);
    return args;
}

std::vector<std::string> build_coolicc_args(const PipelineOptions& opts,
                                            const std::string& cil_path) {
    // coolicc --from-il -arch=sm_XX -o <ptx> <cil>
    return {"--from-il", "-arch=" + opts.arch, "-o", opts.output_path, cil_path};
}

std::vector<std::string> build_host_compile_args(const PipelineOptions& opts,
                                                 const std::string& host_path) {
    // <host-cxx> -std=c++20 <host.cpp> -I<coolrt>/include -I<hacc>/include
    //            [-I <dir> ...] -o <out> <lib> -lcuda
    std::vector<std::string> args;
    args.push_back("-std=c++20");
    args.push_back(host_path);
    const std::string root = resolve_coolrt_root(opts);
    args.push_back("-I" + root + "/include");
    // hacc 自身 include（coolcc/hacc_runtime.h 随编译器分发）。
    args.push_back("-I" + dir_of(opts.self_path) + "../include");
    for (const auto& dir : opts.include_dirs) {
        args.push_back("-I" + dir);
    }
    args.push_back("-o");
    args.push_back(opts.output_path);
    args.push_back(opts.coolrt_lib.empty() ? (root + "/build/libcoolrt_static.a")
                                           : opts.coolrt_lib);
    args.push_back("-lcuda");
    return args;
}

// 把子进程失败信息拼成「<阶段名>失败[: 错误][\n stderr]」。
std::string format_process_error(const char* stage, const ProcessResult& r) {
    std::string msg = std::string(stage) + "失败";
    if (!r.error.empty()) {
        msg += ": " + r.error;
    }
    if (!r.stderr_text.empty()) {
        msg += "\n" + r.stderr_text;
    }
    return msg;
}

// 跑一个子进程阶段；失败时把错误写入 result 并返回 false。
bool run_stage(const std::vector<std::string>& argv, const char* stage, PipelineResult* result) {
    ProcessResult r = run_process(argv);
    if (!r.ok) {
        result->error = format_process_error(stage, r);
        return false;
    }
    return true;
}

// 阶段 1：.cu → EDG IL（Exe 模式同时产 host 源）。
bool run_frontend_stage(const PipelineOptions& opts, const std::string& cil,
                        const std::string& host, bool want_exe, PipelineResult* result) {
    std::vector<std::string> argv = {resolve_coolfe_path(opts)};
    std::vector<std::string> fe_args =
        want_exe ? build_coolfe_host_args(opts, cil, host) : build_coolfe_args(opts, cil);
    for (const auto& a : fe_args) {
        argv.push_back(a);
    }
    return run_stage(argv, "coolfe++", result);
}

// 阶段 2：EDG IL → PTX（Exe 模式 PTX 落中间文件；Ptx 模式直接落 -o）。
bool run_device_stage(const PipelineOptions& opts, const std::string& cil, const std::string& ptx,
                      PipelineResult* result) {
    std::vector<std::string> argv = {resolve_coolicc_path(opts)};
    std::vector<std::string> cc_args = {"--from-il", "-arch=" + opts.arch, "-o", ptx, cil};
    for (const auto& a : cc_args) {
        argv.push_back(a);
    }
    return run_stage(argv, "coolicc", result);
}

// 阶段 3（仅 Exe）：拼桩 + 主机编译。
bool run_host_stage(const PipelineOptions& opts, const std::string& host, const std::string& ptx,
                    PipelineResult* result) {
    std::string err;
    if (!stitch_stubs(opts, host, ptx, &err)) {
        result->error = err;
        return false;
    }
    const std::string cxx = resolve_host_cxx_path(opts);
    // 打印实际使用的主机编译器：这是「到底哪个编译器在编」的唯一凭据（可观测性 G5）。
    std::fprintf(stderr, "hacc: 主机编译器 = %s\n", cxx.c_str());
    std::vector<std::string> argv = {cxx};
    for (const auto& a : build_host_compile_args(opts, host)) {
        argv.push_back(a);
    }
    const std::string stage = "主机编译（" + support::basename_of(cxx) + "）";
    return run_stage(argv, stage.c_str(), result);
}

// 清理中间产物（除非 -keep）。
void cleanup_intermediates(const PipelineOptions& opts, const std::string& cil,
                           const std::string& host, const std::string& ptx, bool want_exe) {
    if (opts.keep) {
        return;
    }
    std::remove(cil.c_str());
    if (want_exe) {
        std::remove(host.c_str());
        std::remove(ptx.c_str());
    }
}

PipelineResult run_pipeline(const PipelineOptions& opts) {
    PipelineResult result;
    if (opts.input_path.empty()) {
        result.error = "缺少输入文件";
        return result;
    }
    if (opts.output_path.empty()) {
        result.error = "缺少输出文件（-o）";
        return result;
    }

    const std::string cil = cil_path_for(opts);
    const bool want_exe = (opts.emit == EmitMode::Exe);
    const std::string host = host_path_for(opts);
    const std::string ptx = want_exe ? ptx_path_for(opts) : opts.output_path;

    if (!run_frontend_stage(opts, cil, host, want_exe, &result)) {
        return result;
    }
    if (!run_device_stage(opts, cil, ptx, &result)) {
        return result;
    }
    if (want_exe && !run_host_stage(opts, host, ptx, &result)) {
        return result;
    }

    cleanup_intermediates(opts, cil, host, ptx, want_exe);

    result.ok = true;
    return result;
}

}  // namespace hacc::driver
