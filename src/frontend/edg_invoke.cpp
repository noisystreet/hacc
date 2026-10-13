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

std::vector<std::string> build_cpfe_args(const EdgInvokeOptions& opts) {
    std::vector<std::string> args;
    const std::string base = resolve_edg_base_dir(opts.edg_base_dir);
    if (!base.empty()) {
        args.push_back("--edg_base_dir");
        args.push_back(base);
    }
    // --g++：GNU C++ 扩展（__attribute__ 等）；-N：保留未降级 IL（信息最全）。
    args.push_back("--g++");
    args.push_back("-N");
    if (!opts.il_text_path.empty()) {
        // 编译期直接 dump 内存 IL（不经过 cdisp，见方案 §7.1）。
        args.push_back("--il_display");
    }
    if (!opts.il_output_path.empty()) {
        args.push_back("-o");
        args.push_back(opts.il_output_path);
    }
    args.push_back(opts.input_path);
    for (const auto& a : opts.extra_args) {
        args.push_back(a);
    }
    return args;
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

    std::vector<std::string> args = build_cpfe_args(opts);

    // 文本 IL 走 stdout 重定向到文件；二进制 IL 由 cpfe -o 直接写。
    // 用 fork + execvp + 管道收集 stderr。
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        result.error = std::string("pipe 失败: ") + std::strerror(errno);
        return result;
    }

    pid_t pid = fork();
    if (pid < 0) {
        result.error = std::string("fork 失败: ") + std::strerror(errno);
        close(pipefd[0]);
        close(pipefd[1]);
        return result;
    }

    if (pid == 0) {
        // 子进程：stderr → 管道；stdout → 文本 IL 文件（若需要）。
        close(pipefd[0]);
        if (dup2(pipefd[1], STDERR_FILENO) < 0) {
            _exit(127);
        }
        close(pipefd[1]);

        if (!opts.il_text_path.empty()) {
            FILE* f = std::fopen(opts.il_text_path.c_str(), "wb");
            if (f == nullptr) {
                _exit(127);
            }
            if (dup2(fileno(f), STDOUT_FILENO) < 0) {
                _exit(127);
            }
            std::fclose(f);
        }

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

    // 父进程：读 stderr。
    close(pipefd[1]);
    char buf[4096];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
        result.stderr_text.append(buf, static_cast<size_t>(n));
    }
    close(pipefd[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        result.error = std::string("waitpid 失败: ") + std::strerror(errno);
        return result;
    }

    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
        result.ok = (result.exit_code == 0);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
        result.error = "cpfe 被信号终止";
    } else {
        result.error = "cpfe 异常退出";
    }
    return result;
}

}  // namespace hacc::frontend
