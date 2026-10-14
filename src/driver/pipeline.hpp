// hacc driver 的编译流水线编排（M1.2.5 / M2.3）。
//
// 一期流水线（`.cu` → PTX，--emit=ptx）：
//   coolfe++ <input.cu> -o <tmp.cil>            # .cu → EDG IL
//   coolicc --from-il -arch=sm_XX -o <out.ptx> <tmp.cil>   # IL → PTX
//
// M2.3 主机路径（`.cu` → 可执行，--emit=exe，默认）：
//   coolfe++ <input.cu> --emit-host=<tmp>.hacc.cpp -o <tmp>.cil   # host 源 + IL
//   coolicc --from-il -arch=sm_XX -o <tmp>.ptx <tmp>.cil          # IL → PTX
//   driver: rewrite_source(input.cu) → kernel_names               # 拿内核名
//   driver: 读 <tmp>.ptx + generate_stubs → 拼到 host 源顶部       # 桩
//   g++ <tmp>.hacc.cpp -I<coolrt> -lcoolrt -o <out>               # 主机编译
//
// 两个子工具与 hacc 同目录（构建产物在一起）；可用环境变量覆盖（见 .cpp）。
// 纯逻辑（参数拼装、路径推导）与执行分离，便于单测。
#ifndef HACC_DRIVER_PIPELINE_HPP
#define HACC_DRIVER_PIPELINE_HPP

#include <string>
#include <vector>

namespace hacc::driver {

// 产物模式。
enum class EmitMode {
    Ptx,  // 纯设备产物（止于 PTX，M1.2 已有）
    Exe,  // 可执行文件（默认，对齐 nvcc「一步到可执行」）
};

// 流水线选项。
struct PipelineOptions {
    std::string input_path;                 // 输入 .cu
    std::string output_path;                // 输出（-o）：PTX 或可执行文件
    std::string arch = "sm_80";             // -arch=sm_XX
    EmitMode emit = EmitMode::Exe;          // 产物模式
    bool keep = false;                      // -keep：保留中间产物（.cil / .hacc.cpp / .ptx）
    std::vector<std::string> include_dirs;  // -I：透传给 coolfe++ 与 g++
    // 子工具路径（空则按同目录 / 环境变量推导）。
    std::string coolfe_path;
    std::string coolicc_path;
    std::string gxx_path;  // 主机编译器（默认 $HACC_GXX 或 "g++"）
    // COOLRuntime 头目录与库（主机编译用；空则按环境变量 / 默认推导）。
    std::string coolrt_include;  // -I<coolrt>/include
    std::string coolrt_lib;      // <coolrt>/build/libcoolruntime.a
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

// 推导主机编译器路径：显式 > $HACC_GXX > "g++"。
std::string resolve_gxx_path(const PipelineOptions& opts);

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

// 拼装 g++ 参数（不含程序名）：<host.cpp> -I<coolrt>/include -o <out> <lib> -lcuda。
// 注：-lcuda 由 coolrt 静态库的依赖决定（见 coolruntime/CMakeLists.txt）。
std::vector<std::string> build_gxx_args(const PipelineOptions& opts, const std::string& host_path);

// 执行流水线。emit=Ptx 时 .cu → .cil → .ptx；emit=Exe 时再拼桩 + 主机编译。
// 成功返回 ok=true。keep=false 时删除中间产物。
PipelineResult run_pipeline(const PipelineOptions& opts);

}  // namespace hacc::driver

#endif  // HACC_DRIVER_PIPELINE_HPP
