// coolfe++ 的 EDG 前端调用层实现（M1.2.0）。
#include "frontend/edg_invoke.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace hacc::frontend {

namespace {

// 读取环境变量；未设置或为空返回空串。
std::string getenv_str(const char* name) {
    const char* v = std::getenv(name);
    return (v != nullptr && v[0] != '\0') ? std::string(v) : std::string();
}

// 构建期默认值（由 CMake 注入；未注入则为空）。
#ifndef HACC_DEFAULT_CPFE
#define HACC_DEFAULT_CPFE ""
#endif
#ifndef HACC_DEFAULT_EDG_BASE
#define HACC_DEFAULT_EDG_BASE ""
#endif

}  // namespace

std::string resolve_cpfe_path(const std::string& explicit_path) {
    if (!explicit_path.empty()) {
        return explicit_path;
    }
    std::string env = getenv_str("HACC_CPFE");
    if (!env.empty()) {
        return env;
    }
    return std::string(HACC_DEFAULT_CPFE);
}

std::string resolve_edg_base_dir(const std::string& explicit_dir) {
    if (!explicit_dir.empty()) {
        return explicit_dir;
    }
    std::string env = getenv_str("HACC_EDG_BASE");
    if (!env.empty()) {
        return env;
    }
    return std::string(HACC_DEFAULT_EDG_BASE);
}

// 构建单次 cpfe 调用的参数。
//
// 重要（实测 cpfe 7.0）：
//   1) --il_display 与 -o <真实文件> 不能同时使用：--il_display 生效时，-o 写出的
//      二进制 IL 会变成「显示变体」，il_read 读回时报 "invalid intermediate
//      language file"。
//   2) --il_display 不带 -o 时，cpfe 会把一个默认名（<input-stem>.cil）的
//      「显示变体」IL 写到**当前目录**，可能覆盖真正的二进制 IL。
// 因此：二进制 IL 与文本 IL 分两次调用；文本调用必须带 -o <丢弃文件> 抑制默认写。
// 注意 cpfe 要求 -o 是**常规文件**（/dev/null 会被拒），故用临时文件。
//   - 二进制：-o <file>（不带 --il_display）
//   - 文本：  --il_display -o <throwaway>（stdout 重定向到文本文件）
std::vector<std::string> build_cpfe_args(const EdgInvokeOptions& opts, bool for_text,
                                         const std::string& throwaway_il) {
    std::vector<std::string> args;
    const std::string base = resolve_edg_base_dir(opts.edg_base_dir);
    if (!base.empty()) {
        args.push_back("--edg_base_dir");
        args.push_back(base);
    }
    // --g++：GNU C++ 扩展（__attribute__ 等）；-N：保留未降级 IL（信息最全）。
    args.push_back("--g++");
    args.push_back("-N");
    if (for_text) {
        // 编译期直接 dump 内存 IL（不经过 cdisp，见方案 §7.1）。
        args.push_back("--il_display");
        // 抑制 cpfe 把默认名「显示变体」IL 写到当前目录（见上方说明 2）。
        args.push_back("-o");
        args.push_back(throwaway_il);
    } else if (!opts.il_output_path.empty()) {
        args.push_back("-o");
        args.push_back(opts.il_output_path);
    }
    args.push_back(opts.input_path);
    for (const auto& a : opts.extra_args) {
        args.push_back(a);
    }
    return args;
}

// 子进程：把 stderr 重定向到管道写端；若 stdout_path 非空，stdout 重定向到该文件。
// 任一步失败返回 false（调用方应 _exit(127)）。
bool setup_child_stdio(int pipe_write_fd, const std::string& stdout_path) {
    if (dup2(pipe_write_fd, STDERR_FILENO) < 0) {
        return false;
    }
    if (stdout_path.empty()) {
        return true;
    }
    FILE* f = std::fopen(stdout_path.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    const bool ok = dup2(fileno(f), STDOUT_FILENO) >= 0;
    std::fclose(f);
    return ok;
}

// 子进程：exec cpfe（不返回；失败时打印并 _exit(127)）。
[[noreturn]] void exec_cpfe(const std::string& cpfe, const std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(cpfe.c_str()));
    for (const auto& a : args) {
        argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);
    execvp(cpfe.c_str(), argv.data());
    // exec 失败。
    std::fprintf(stderr, "coolfe++: 无法执行 %s: %s\n", cpfe.c_str(), std::strerror(errno));
    _exit(127);
}

// 父进程：读子进程 stderr 到 *stderr_out，等待退出并解析状态。
// 返回 true 表示 cpfe 正常退出（exit 0）。
bool collect_child_result(int pipe_read_fd, pid_t pid, std::string* stderr_out, int* exit_code_out,
                          std::string* error_out) {
    char buf[4096];
    ssize_t n;
    while ((n = read(pipe_read_fd, buf, sizeof(buf))) > 0) {
        stderr_out->append(buf, static_cast<size_t>(n));
    }
    close(pipe_read_fd);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        *error_out = std::string("waitpid 失败: ") + std::strerror(errno);
        return false;
    }
    if (WIFEXITED(status)) {
        *exit_code_out = WEXITSTATUS(status);
        return *exit_code_out == 0;
    }
    if (WIFSIGNALED(status)) {
        *exit_code_out = 128 + WTERMSIG(status);
        *error_out = "cpfe 被信号终止";
        return false;
    }
    *error_out = "cpfe 异常退出";
    return false;
}

// 执行一次 cpfe：stderr 收集到 *stderr_out；若 stdout_path 非空，stdout 重定向到该文件。
// 返回 true 表示 cpfe 正常退出（exit 0）。
bool run_cpfe_once(const std::string& cpfe, const std::vector<std::string>& args,
                   const std::string& stdout_path, std::string* stderr_out, int* exit_code_out,
                   std::string* error_out) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        *error_out = std::string("pipe 失败: ") + std::strerror(errno);
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        *error_out = std::string("fork 失败: ") + std::strerror(errno);
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }

    if (pid == 0) {
        // 子进程：stderr → 管道；stdout → 文本 IL 文件（若需要）。
        close(pipefd[0]);
        if (!setup_child_stdio(pipefd[1], stdout_path)) {
            _exit(127);
        }
        close(pipefd[1]);
        exec_cpfe(cpfe, args);
    }

    // 父进程：读 stderr。
    close(pipefd[1]);
    return collect_child_result(pipefd[0], pid, stderr_out, exit_code_out, error_out);
}

EdgInvokeResult invoke_cpfe(const EdgInvokeOptions& opts) {
    EdgInvokeResult result;

    const std::string cpfe = resolve_cpfe_path(opts.cpfe_path);
    if (cpfe.empty()) {
        result.error =
            "未指定 cpfe 路径（用 --cpfe、环境变量 HACC_CPFE，或构建期 HACC_DEFAULT_CPFE）";
        return result;
    }
    if (opts.input_path.empty()) {
        result.error = "缺少输入文件";
        return result;
    }

    // 二进制 IL：-o <file>，不带 --il_display（见 build_cpfe_args 的说明）。
    if (!opts.il_output_path.empty()) {
        std::string err;
        int code = 0;
        if (!run_cpfe_once(cpfe, build_cpfe_args(opts, /*for_text=*/false, std::string()),
                           std::string(), &result.stderr_text, &code, &err)) {
            result.exit_code = code;
            result.error = err;
            return result;
        }
    }

    // 文本 IL：--il_display，stdout 重定向到文件；用丢弃文件抑制默认 .cil 写入。
    if (!opts.il_text_path.empty()) {
        // cpfe 要求 -o 是常规文件（/dev/null 会被拒），故用临时文件；用完删除。
        const std::string throwaway = opts.il_text_path + ".tmp.cil";
        std::string err;
        int code = 0;
        const bool ok = run_cpfe_once(cpfe, build_cpfe_args(opts, /*for_text=*/true, throwaway),
                                      opts.il_text_path, &result.stderr_text, &code, &err);
        std::remove(throwaway.c_str());
        if (!ok) {
            result.exit_code = code;
            result.error = err;
            return result;
        }
    }

    result.ok = true;
    return result;
}

}  // namespace hacc::frontend
