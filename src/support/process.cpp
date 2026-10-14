// 通用子进程执行实现（fork/exec + stderr 管道）。
#include "support/process.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace hacc {

namespace {

// 子进程：把 stderr 重定向到管道写端，然后 exec。失败时 _exit(127)。
[[noreturn]] void exec_child(const std::vector<std::string>& argv, int pipe_write_fd) {
    if (dup2(pipe_write_fd, STDERR_FILENO) < 0) {
        _exit(127);
    }
    std::vector<char*> raw;
    raw.reserve(argv.size() + 1);
    for (const auto& a : argv) {
        raw.push_back(const_cast<char*>(a.c_str()));
    }
    raw.push_back(nullptr);
    execvp(raw[0], raw.data());
    // exec 失败：写一行到 stderr（已重定向到管道），再退出。
    std::fprintf(stderr, "hacc: 无法执行 %s: %s\n", raw[0], std::strerror(errno));
    _exit(127);
}

// 父进程：读子进程 stderr 到 *stderr_out，等待退出并解析状态。
bool collect_child(int pipe_read_fd, pid_t pid, std::string* stderr_out, int* exit_code_out,
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
        *error_out = "子进程被信号终止";
        return false;
    }
    *error_out = "子进程异常退出";
    return false;
}

}  // namespace

ProcessResult run_process(const std::vector<std::string>& argv) {
    ProcessResult result;
    if (argv.empty()) {
        result.error = "空命令";
        return result;
    }

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
        close(pipefd[0]);
        exec_child(argv, pipefd[1]);
    }

    close(pipefd[1]);
    result.ok = collect_child(pipefd[0], pid, &result.stderr_text, &result.exit_code,
                              &result.error);
    return result;
}

}  // namespace hacc
