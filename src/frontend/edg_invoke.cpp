// coolfe++ 的 EDG 前端调用层实现（M1.2.0）。
#include "frontend/edg_invoke.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "support/toolchain.hpp"

namespace hacc::frontend {

namespace {

// 读取环境变量；未设置或为空返回空串。
std::string getenv_str(const char* name) {
    const char* v = std::getenv(name);
    return (v != nullptr && v[0] != '\0') ? std::string(v) : std::string();
}

// 把路径安全地嵌入 /bin/sh 命令行（单引号包裹，内部单引号按 '\'' 逃逸）。
// 之所以需要：编译器路径或头目录可能含空格（如 "/opt/my tools/clang++"），
// 而 popen 走的是 /bin/sh。
std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (const char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

// 解析编译器 `-Wp,-v` 打到 stderr 的 include 搜索列表。
//
// 背景：cpfe 的 --g++ 模式**不自动**带系统 include 路径，故 .cu 一旦 #include
// 标准头（如 <stddef.h> / <cstdio>）就会报「cannot open source file」。
// g++ 与 clang++ 的输出格式一致（"#include <...> search starts here:" 起、
// "End of search list." 止，实测 GCC 14 / LLVM 19），故共用同一套解析。
std::vector<std::string> parse_include_search_list(FILE* p) {
    std::vector<std::string> result;
    char line[4096];
    bool in_list = false;
    while (std::fgets(line, sizeof(line), p) != nullptr) {
        std::string s(line);
        if (s.find("#include <...> search starts here:") != std::string::npos) {
            in_list = true;
            continue;
        }
        if (s.find("End of search list.") != std::string::npos) {
            break;
        }
        if (!in_list) {
            continue;
        }
        // 去掉首尾空白（含换行）。
        const size_t b = s.find_first_not_of(" \t\r\n");
        const size_t e = s.find_last_not_of(" \t\r\n");
        if (b != std::string::npos && e != std::string::npos) {
            result.push_back(s.substr(b, e - b + 1));
        }
    }
    return result;
}

// 探测单个编译器（具体路径）的系统头目录，按路径缓存。
// 单线程 CLI 使用，故不加锁；map 的元素引用在插入后保持稳定。
const std::vector<std::string>& probe_one_cxx(const std::string& cxx) {
    static std::map<std::string, std::vector<std::string>> cache;
    const auto it = cache.find(cxx);
    if (it != cache.end()) {
        return it->second;
    }
    std::vector<std::string> dirs;
    // 2>&1：搜索列表打在 stderr 上，与 stdout 合并后逐行解析。
    const std::string cmd = shell_quote(cxx) + " -x c++ -E -Wp,-v - 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    if (p != nullptr) {
        dirs = parse_include_search_list(p);
        pclose(p);
    }
    return cache.emplace(cxx, std::move(dirs)).first->second;
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

std::vector<std::string> probe_system_include_dirs(const std::string& host_cxx) {
    // 优先用 g++：EDG 的 `--g++` 模式按 GCC 语义解析，而 clang++ 的 resource-dir
    // 内建头（/usr/lib/llvm-19/lib/clang/19/include 下的 stddef.h 等）用了
    // __has_feature / __building_module 等 clang 专有构造，EDG 会成片报错
    // （实测 55 个错误，size_t 直接 undefined）。故主机侧可以切 clang++，
    // 但**前端系统头必须取 GCC 的头**。
    std::vector<std::string> dirs = probe_one_cxx("g++");
    if (!dirs.empty()) {
        return dirs;
    }
    // g++ 不可用：退回主机编译器（尽力而为）。clang++ 场景可能失败，此时诊断会
    // 直接指向标准头解析错误，而不是静默产出坏 IL。
    return probe_one_cxx(support::resolve_host_cxx(host_cxx));
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
    // 设备编译标识宏（对标 nvcc 的 __CUDACC__）：让运行时头 device_builtins.h 暴露
    // 设备端内建（threadIdx / __syncthreads 等）。coolfe++ 是设备前端，故无条件定义。
    args.push_back("-D__COOLCC__");
    // 透传的 -I / -D 等必须放在输入文件**之前**：cpfe 只识别输入文件前的选项
    // （实测：-I 放在输入文件之后会被忽略，导致 #include <cool/...> 找不到）。
    for (const auto& a : opts.extra_args) {
        args.push_back(a);
    }
    // 系统头目录：cpfe 的 --g++ 不自动带，需显式 --sys_include（否则 <stddef.h> 等找不到）。
    for (const auto& dir : probe_system_include_dirs(opts.host_cxx)) {
        args.push_back("--sys_include");
        args.push_back(dir);
    }
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
