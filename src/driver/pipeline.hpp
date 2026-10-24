// hacc driver 的编译流水线编排（M1.2.5 / M2.3 / B7）。
//
// 一期流水线（`.cu` → PTX，--emit=ptx）：
//   coolfe++ <input.cu> -o <stem>.hacc.il                  # .cu → EDG IL
//   coolicc --from-il -arch=sm_XX -o <out.ptx> <stem>.hacc.il   # IL → PTX
//
// M2.3 主机路径（`.cu` → 可执行，--emit=exe，默认）：
//   coolfe++ <input.cu> --emit-host=<stem>.hacc.cpp -o <stem>.hacc.il
//   coolicc --from-il -arch=sm_XX -o <stem>.ptx <stem>.hacc.il
//   driver: rewrite_source(input.cu) → kernel_names
//   driver: generate_stubs → <stem>.hacc.stub.cpp，再拼成 <stem>.hacc.gen.cpp
//   <host-cxx> <stem>.hacc.gen.cpp -I<coolrt> -lcoolrt -o <out>
//
// 两个子工具与 hacc 同目录（构建产物在一起）；可用环境变量覆盖（见 .cpp）。
// 纯逻辑（参数拼装、路径推导）与执行分离，便于单测。
#ifndef HACC_DRIVER_PIPELINE_HPP
#define HACC_DRIVER_PIPELINE_HPP

#include <string>
#include <vector>

namespace hacc::driver {

// 产物模式（对应 nvcc 的 `-ptx` 与默认「一步到可执行」等）。
enum class EmitMode {
    Exe,   // 可执行文件（默认，对齐 nvcc「一步到可执行」）
    Ptx,   // 止于 PTX（对齐 nvcc `-ptx`）
    Il,    // 止于 EDG IL（本编译器特有，供观测）
    Host,  // 止于 host 源（拼桩之前的主机代码，供观测）
};

// 流水线选项。
struct PipelineOptions {
    // 输入（C3：多 TU）。
    //   `input_paths` —— 命令行给出的全部 `.cu`，按给出顺序；为空时退回 `input_path`。
    //   `input_path`  —— **当前正在处理的那一个** TU。单文件时两者一致；多文件时
    //                    run_pipeline 为每个输入派生一份副本并改写这个字段，
    //                    于是所有 path 推导（`<stem>.hacc.*`）天然按 TU 分开。
    std::string input_path;
    std::vector<std::string> input_paths;
    std::string output_path;                // 输出（-o）：PTX 或可执行文件
    std::string arch = "sm_80";             // -arch=sm_XX
    EmitMode emit = EmitMode::Exe;          // 产物模式
    bool keep = false;                      // -keep / --keep：保留中间产物
    std::string keep_dir;                   // --keep-dir：中间产物目录（空 = 当前工作目录）
    std::vector<std::string> include_dirs;  // -I：透传给 coolfe++ 与 g++
    // 子工具路径（空则按同目录 / 环境变量推导）。
    std::string coolfe_path;
    std::string coolicc_path;
    std::string host_cxx;  // 主机 C++ 编译器（显式 > $HACC_HOST_CXX > $HACC_GXX > "g++"）
    // COOLRuntime 头目录与库（主机编译用；空则按环境变量 / 默认推导）。
    std::string coolrt_include;  // -I<coolrt>/include
    std::string coolrt_lib;      // <coolrt>/build/libcoolrt_static.a
    // hacc 自身路径（argv[0]），用于推导同目录子工具。
    std::string self_path;
};

// 流水线执行结果。
struct PipelineResult {
    bool ok = false;
    std::string error;  // 失败原因（含子工具 stderr）
};

// 推导 coolfe++ 路径：显式 > $HACC_COOLFE > self_path 同目录。
std::string resolve_coolfe_path(const PipelineOptions& opts);

// 推导 coolicc 路径：显式 > $HACC_COOLICC > self_path 同目录。
std::string resolve_coolicc_path(const PipelineOptions& opts);

// 推导主机编译器路径：显式 > $HACC_HOST_CXX > $HACC_GXX（旧名兼容）> "g++"。
std::string resolve_host_cxx_path(const PipelineOptions& opts);

// 拼装 coolfe++ 参数（不含程序名）：<input.cu> -o <cil>。
std::vector<std::string> build_coolfe_args(const PipelineOptions& opts,
                                           const std::string& cil_path);

// 拼装 coolfe++ 参数（含 --emit-host=<host_path>）：<input.cu> --emit-host=<host> -o <cil>。
std::vector<std::string> build_coolfe_host_args(const PipelineOptions& opts,
                                                const std::string& cil_path,
                                                const std::string& host_path);

// 拼装 coolicc 参数（不含程序名）：--from-il -arch=sm_XX -o <ptx> <cil>。
std::vector<std::string> build_coolicc_args(const PipelineOptions& opts,
                                            const std::string& cil_path);

// 拼装主机编译参数（不含程序名）：-std=c++20 <host.cpp> -I<coolrt>/include
// -I<hacc>/include [-I <dir> ...] -o <out> <lib> -lcuda。
// 注：-lcuda 由 coolrt 静态库的依赖决定（见 coolruntime/CMakeLists.txt）。
std::vector<std::string> build_host_compile_args(const PipelineOptions& opts,
                                                 const std::string& host_path);

// 多 TU 版本（C3）：一次主机编译链接多个编译单元。
// `-o` 只有一个，其余参数同单文件版本。
std::vector<std::string> build_host_compile_args(const PipelineOptions& opts,
                                                 const std::vector<std::string>& host_paths);

// 执行流水线。Exe：.cu → IL → PTX → 桩 + 主机编译；Ptx / Il / Host 止于对应阶段。
// 中间产物命名（B7 定稿，对齐 nvcc 的产物角色；落在 --keep-dir 或当前工作目录）：
//   <stem>.hacc.il        EDG IL（对应 nvcc `.cudafe1.gpu`）
//   <stem>.hacc.cpp       前端 host 源，未拼桩（对应 `.cudafe1.cpp`）
//   <stem>.hacc.stub.cpp  主机桩（对应 `.cudafe1.stub.c`）
//   <stem>.hacc.gen.cpp   桩 + host，主机编译的实际输入
//   <stem>.ptx            设备映像（一期终产物）
// 成功时默认删除中间产物，`-keep` 保留并列出；失败始终保留并列出（便于定位断在哪阶段）。
PipelineResult run_pipeline(const PipelineOptions& opts);

}  // namespace hacc::driver

#endif  // HACC_DRIVER_PIPELINE_HPP
