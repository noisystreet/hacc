// 通用子进程执行（driver 编排 coolfe++ / coolicc 用）。
//
// 与 frontend/edg_invoke.cpp 的 cpfe 调用同源（fork/exec + stderr 管道），
// 但抽到 support 层供 driver 复用，避免 driver 依赖 frontend。
#ifndef HACC_SUPPORT_PROCESS_HPP
#define HACC_SUPPORT_PROCESS_HPP

#include <string>
#include <vector>

namespace hacc {

// 子进程执行结果。
struct ProcessResult {
    bool ok = false;       // 退出码为 0
    int exit_code = -1;    // 退出码；被信号终止时为 128+signo
    std::string stderr_text;  // 子进程 stderr（诊断信息）
    std::string error;        // 失败时的可读原因（如「无法执行」）
};

// 执行 argv[0]（经 PATH 解析），argv 为完整参数列表（含程序名）。
// 捕获子进程 stderr 到 result.stderr_text；stdout 继承父进程。
// 返回结果；ok 表示退出码为 0。
ProcessResult run_process(const std::vector<std::string>& argv);

}  // namespace hacc

#endif  // HACC_SUPPORT_PROCESS_HPP
