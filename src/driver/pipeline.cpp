// hacc driver 的编译流水线编排实现（M1.2.5 / M2.3）。
#include "driver/pipeline.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
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

// 中间产物目录：--keep-dir 优先，否则当前工作目录（nvcc 行为）。
std::string intermediate_dir(const PipelineOptions& opts) {
    return opts.keep_dir.empty() ? std::string(".") : opts.keep_dir;
}

// 从路径取基准名（去目录、去扩展名）：`a/b/foo.cu` → `foo`。
std::string stem_of(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    const std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const size_t dot = base.find_last_of('.');
    return (dot == std::string::npos || dot == 0) ? base : base.substr(0, dot);
}

// 中间产物基准名 = 输入文件名去掉扩展名。
// nvcc 的中间产物以源文件命名（`foo.cu` → `foo.ptx`），与 `-o` 无关；
// 这里保持一致，避免 `-o build/out` 时中间产物跟着跑进 build/。
std::string input_stem(const PipelineOptions& opts) {
    return stem_of(opts.input_path);
}

// 拼中间产物路径：<keep-dir|.>/<input stem><suffix>。
std::string intermediate_path(const PipelineOptions& opts, const char* suffix) {
    return intermediate_dir(opts) + "/" + input_stem(opts) + suffix;
}

std::string il_path_for(const PipelineOptions& opts) {
    return intermediate_path(opts, ".hacc.il");
}

std::string host_path_for(const PipelineOptions& opts) {
    return intermediate_path(opts, ".hacc.cpp");
}

std::string stub_path_for(const PipelineOptions& opts) {
    return intermediate_path(opts, ".hacc.stub.cpp");
}

std::string gen_path_for(const PipelineOptions& opts) {
    return intermediate_path(opts, ".hacc.gen.cpp");
}

std::string ptx_path_for(const PipelineOptions& opts) {
    return intermediate_path(opts, ".ptx");
}

// 确保中间产物目录存在（未给 --keep-dir 时不创建任何东西）。
bool ensure_intermediate_dir(const PipelineOptions& opts) {
    if (opts.keep_dir.empty()) {
        return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(opts.keep_dir, ec);
    return !ec && std::filesystem::is_directory(opts.keep_dir);
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

// 生成主机桩与「桩 + host 源」编译单元（B7）：
//   <stem>.hacc.stub.cpp —— 只有桩（对应 nvcc 的 `.cudafe1.stub.c`）
//   <stem>.hacc.gen.cpp  —— 桩置顶 + 前端 host 源（主机编译的实际输入）
// 桩须在用户代码之前（haccLaunch / __cool_reg_* 先声明，见 M2.2b 实测）。
// 不改写 <stem>.hacc.cpp：`-keep` 时要能看到前端的原始输出。
// 成功返回 true；失败填 error。
bool write_stub_and_gen(const PipelineOptions& opts, const std::string& host_path,
                        const std::string& ptx_path, const std::string& stub_path,
                        const std::string& gen_path, std::string* error) {
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
    if (!write_file(stub_path, stubs)) {
        *error = "无法写主机桩：" + stub_path;
        return false;
    }
    if (!write_file(gen_path, stubs + host_src)) {
        *error = "无法写编译单元：" + gen_path;
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
                                                 const std::vector<std::string>& host_paths) {
    // <host-cxx> -std=c++20 <host.cpp ...> -I<coolrt>/include -I<hacc>/include
    //            [-I <dir> ...] -o <out> <lib> -lcuda
    std::vector<std::string> args;
    args.push_back("-std=c++20");
    for (const auto& p : host_paths) {
        args.push_back(p);
    }
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

std::vector<std::string> build_host_compile_args(const PipelineOptions& opts,
                                                 const std::string& host_path) {
    return build_host_compile_args(opts, std::vector<std::string>{host_path});
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

// 阶段 1'（--emit=il）：止于 EDG IL，`-o` 直接落 IL 产物。
bool run_il_stage(const PipelineOptions& opts, PipelineResult* result) {
    std::vector<std::string> argv = {resolve_coolfe_path(opts)};
    for (const auto& a : build_coolfe_args(opts, opts.output_path)) {
        argv.push_back(a);
    }
    return run_stage(argv, "coolfe++", result);
}

// 阶段 1''（--emit=host）：止于 host 源，`-o` 直接落 host 产物（不拼桩）。
bool run_host_only_stage(const PipelineOptions& opts, const std::string& cil,
                         PipelineResult* result) {
    std::vector<std::string> argv = {resolve_coolfe_path(opts)};
    for (const auto& a : build_coolfe_host_args(opts, cil, opts.output_path)) {
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

// 阶段 3a（仅 Exe）：生成桩与编译单元（每 TU 一次，不编译）。
// 拆成 3a/3b 是为了多 TU：桩必须逐 TU 生成，而主机编译只在最后做一次（C3）。
bool write_stub_stage(const PipelineOptions& opts, const std::string& host, const std::string& stub,
                      const std::string& gen, const std::string& ptx, PipelineResult* result) {
    std::string err;
    if (!write_stub_and_gen(opts, host, ptx, stub, gen, &err)) {
        result->error = err;
        return false;
    }
    return true;
}

// 阶段 3b（仅 Exe）：主机编译。单 TU 一个源；多 TU 多个源一次调用、由链接器合并。
// 注意多个 TU 各自带 static 的 module/kernel 注册表，符号是内部链接，不会互相冲突；
// 反过来，跨 TU 的 __device__ / __constant__ 也因此不共享（ADR 0007 已记录）。
bool compile_host_stage(const PipelineOptions& opts, const std::vector<std::string>& gen_paths,
                        PipelineResult* result) {
    const std::string cxx = resolve_host_cxx_path(opts);
    // 打印实际使用的主机编译器：这是「到底哪个编译器在编」的唯一凭据（可观测性 G5）。
    std::fprintf(stderr, "hacc: 主机编译器 = %s\n", cxx.c_str());
    std::vector<std::string> argv = {cxx};
    for (const auto& a : build_host_compile_args(opts, gen_paths)) {
        argv.push_back(a);
    }
    const std::string stage = "主机编译（" + support::basename_of(cxx) + "）";
    return run_stage(argv, stage.c_str(), result);
}

// 删除中间产物（-keep 时保留）。
void remove_files(const std::vector<std::string>& paths) {
    for (const auto& p : paths) {
        std::remove(p.c_str());
    }
}

// 列出仍然存在的中间产物：-keep 成功与任意失败时都要报（可观测性 G5）。
void report_intermediates(const char* reason, const std::vector<std::string>& paths) {
    std::vector<std::string> present;
    for (const auto& p : paths) {
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) {
            present.push_back(p);
        }
    }
    if (present.empty()) {
        return;
    }
    std::fprintf(stderr, "hacc: %s，保留中间产物（%zu 个）：\n", reason, present.size());
    for (const auto& p : present) {
        std::fprintf(stderr, "hacc:   %s\n", p.c_str());
    }
}

// 本次运行涉及的中间产物（按 emit 模式）。
std::vector<std::string> intermediates_for(EmitMode emit, const std::string& il,
                                           const std::string& host, const std::string& stub,
                                           const std::string& gen, const std::string& ptx) {
    std::vector<std::string> v;
    if (emit == EmitMode::Il) {
        return v;  // 前端直接写 -o，无中间产物
    }
    v.push_back(il);
    if (emit != EmitMode::Exe) {
        return v;  // Ptx / Host 额外只有 IL
    }
    v.push_back(host);
    v.push_back(stub);
    v.push_back(gen);
    v.push_back(ptx);
    return v;
}

// 按 emit 模式跑阶段；全部成功返回 true。
bool dispatch_stages(const PipelineOptions& opts, const std::string& il, const std::string& host,
                     const std::string& stub, const std::string& gen, const std::string& ptx,
                     PipelineResult* result) {
    if (opts.emit == EmitMode::Il) {
        return run_il_stage(opts, result);
    }
    if (opts.emit == EmitMode::Host) {
        return run_host_only_stage(opts, il, result);
    }
    const bool want_exe = (opts.emit == EmitMode::Exe);
    // Ptx 模式终产物就是 `-o`；Exe 模式的 PTX 是中间产物（<stem>.ptx）。
    const std::string ptx_target = want_exe ? ptx : opts.output_path;
    if (!run_frontend_stage(opts, il, host, want_exe, result)) {
        return false;
    }
    if (!run_device_stage(opts, il, ptx_target, result)) {
        return false;
    }
    if (want_exe && !write_stub_stage(opts, host, stub, gen, ptx_target, result)) {
        return false;
    }
    if (want_exe && !compile_host_stage(opts, {gen}, result)) {
        return false;
    }
    return true;
}

// 多 TU 的输入列表：`input_paths` 优先，否则退回单个 `input_path`
// （程序化调用者可能只设其中一个）。
std::vector<std::string> inputs_of(const PipelineOptions& opts) {
    if (!opts.input_paths.empty()) {
        return opts.input_paths;
    }
    if (!opts.input_path.empty()) {
        return {opts.input_path};
    }
    return {};
}

// 多 TU 的两条硬限制，动手前查清并报错，而不是产出错误的东西：
//   1. 只支持 --emit=exe —— ptx / il / host 的产物是「每文件一份」，单个 `-o` 表达不了。
//   2. 输入不能同名 —— 中间产物按 `<stem>.hacc.*` 命名，同名会静默互相覆盖。
bool validate_multi_tu(const PipelineOptions& opts, const std::vector<std::string>& inputs,
                       PipelineResult* result) {
    if (opts.emit != EmitMode::Exe) {
        result->error =
            "多 TU 只支持 --emit=exe；--emit=ptx / il / host 请对每个文件单独跑"
            "（这些模式的产物是每文件一份，无法写进一个 -o）";
        return false;
    }
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        for (std::size_t j = i + 1; j < inputs.size(); ++j) {
            if (stem_of(inputs[i]) != stem_of(inputs[j])) {
                continue;
            }
            result->error = "多个输入同名（" + stem_of(inputs[i]) +
                            "）：中间产物按 <stem>.hacc.* 命名会互相覆盖，请改名或分次编译";
            return false;
        }
    }
    return true;
}

// 多 TU：每个输入各跑「前端 → PTX → 桩」，最后一次性主机编译链接（C3）。
// 每个 TU 一个 module + 一份 PTX 映像（ADR 0007），互不影响；代价是跨 TU 的
// __device__ / __constant__ 不共享 —— 这是 ADR 0007 已记录的限制，不是这里的疏漏：
// 要共享设备全局需要链接期合并设备映像（RDC 级），不在一期范围。
PipelineResult run_multi_tu(const PipelineOptions& opts, const std::vector<std::string>& inputs) {
    PipelineResult result;
    std::vector<std::string> gen_paths;
    std::vector<std::string> candidates;
    for (const std::string& in : inputs) {
        PipelineOptions per = opts;
        per.input_path = in;  // 只改这一处，后续所有 path 推导就都按本 TU 走了
        const std::string il = il_path_for(per);
        const std::string host = host_path_for(per);
        const std::string stub = stub_path_for(per);
        const std::string gen = gen_path_for(per);
        const std::string ptx = ptx_path_for(per);
        const std::vector<std::string> tu_files = {il, host, stub, gen, ptx};
        candidates.insert(candidates.end(), tu_files.begin(), tu_files.end());

        const bool ok = run_frontend_stage(per, il, host, true, &result) &&
                        run_device_stage(per, il, ptx, &result) &&
                        write_stub_stage(per, host, stub, gen, ptx, &result);
        if (!ok) {
            result.error = "[" + in + "] " + result.error;  // 标明是哪个文件断的
            report_intermediates("编译失败", candidates);
            return result;
        }
        gen_paths.push_back(gen);
    }
    if (!compile_host_stage(opts, gen_paths, &result)) {
        report_intermediates("编译失败", candidates);
        return result;
    }
    if (opts.keep) {
        report_intermediates("-keep", candidates);
    } else {
        remove_files(candidates);
    }
    result.ok = true;
    return result;
}

PipelineResult run_pipeline(const PipelineOptions& opts) {
    PipelineResult result;
    const std::vector<std::string> inputs = inputs_of(opts);
    if (inputs.empty()) {
        result.error = "缺少输入文件";
        return result;
    }
    if (opts.output_path.empty()) {
        result.error = "缺少输出文件（-o）";
        return result;
    }
    if (!ensure_intermediate_dir(opts)) {
        result.error = "无法创建 --keep-dir 目录：" + opts.keep_dir;
        return result;
    }
    if (inputs.size() > 1) {
        if (!validate_multi_tu(opts, inputs, &result)) {
            return result;
        }
        return run_multi_tu(opts, inputs);
    }

    // 单 TU：把唯一输入显式写回 input_path（调用方可能只设了 input_paths）。
    PipelineOptions single = opts;
    single.input_path = inputs.front();
    const std::string il = il_path_for(single);
    const std::string host = host_path_for(single);
    const std::string stub = stub_path_for(single);
    const std::string gen = gen_path_for(single);
    const std::string ptx = ptx_path_for(single);
    const std::vector<std::string> candidates =
        intermediates_for(single.emit, il, host, stub, gen, ptx);

    if (!dispatch_stages(single, il, host, stub, gen, ptx, &result)) {
        // 失败时默认保留中间产物（同 nvcc），并列出，便于定位是哪一阶段断的。
        report_intermediates("编译失败", candidates);
        return result;
    }
    if (single.keep) {
        report_intermediates("-keep", candidates);
    } else {
        remove_files(candidates);
    }
    result.ok = true;
    return result;
}

}  // namespace hacc::driver
