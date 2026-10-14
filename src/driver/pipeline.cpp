// hacc driver 的编译流水线编排实现（M1.2.5）。
#include "driver/pipeline.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

#include "support/process.hpp"

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

}  // namespace

std::string resolve_coolfe_path(const PipelineOptions& opts) {
    return resolve_tool(opts.coolfe_path, "HACC_COOLFE", opts.self_path, "coolfe++");
}

std::string resolve_coolicc_path(const PipelineOptions& opts) {
    return resolve_tool(opts.coolicc_path, "HACC_COOLICC", opts.self_path, "coolicc");
}

std::vector<std::string> build_coolfe_args(const PipelineOptions& opts,
                                           const std::string& cil_path) {
    // coolfe++ <input.cu> -o <cil> [-I <dir> ...]
    // -I 放在输入文件之前（cpfe 只识别输入前的选项，见 frontend/edg_invoke.cpp）。
    std::vector<std::string> args;
    for (const auto& dir : opts.include_dirs) {
        args.push_back("-I");
        args.push_back(dir);
    }
    args.push_back(opts.input_path);
    args.push_back("-o");
    args.push_back(cil_path);
    return args;
}

std::vector<std::string> build_coolicc_args(const PipelineOptions& opts,
                                            const std::string& cil_path) {
    // coolicc --from-il -arch=sm_XX -o <ptx> <cil>
    return {"--from-il", "-arch=" + opts.arch, "-o", opts.output_path, cil_path};
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

    // 阶段 1：.cu → EDG IL。
    {
        std::vector<std::string> argv = {resolve_coolfe_path(opts)};
        for (const auto& a : build_coolfe_args(opts, cil)) {
            argv.push_back(a);
        }
        ProcessResult r = run_process(argv);
        if (!r.ok) {
            result.error = "coolfe++ 失败";
            if (!r.error.empty()) {
                result.error += ": " + r.error;
            }
            if (!r.stderr_text.empty()) {
                result.error += "\n" + r.stderr_text;
            }
            return result;
        }
    }

    // 阶段 2：EDG IL → PTX。
    {
        std::vector<std::string> argv = {resolve_coolicc_path(opts)};
        for (const auto& a : build_coolicc_args(opts, cil)) {
            argv.push_back(a);
        }
        ProcessResult r = run_process(argv);
        if (!r.ok) {
            result.error = "coolicc 失败";
            if (!r.error.empty()) {
                result.error += ": " + r.error;
            }
            if (!r.stderr_text.empty()) {
                result.error += "\n" + r.stderr_text;
            }
            return result;
        }
    }

    // 清理中间产物（除非 -keep）。
    if (!opts.keep) {
        std::remove(cil.c_str());
    }

    result.ok = true;
    return result;
}

}  // namespace hacc::driver
