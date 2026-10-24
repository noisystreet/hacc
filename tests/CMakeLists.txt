# 一期测试入口。目录划分：unit（纯逻辑）/ golden（.cu → 期望 PTX/主机桩）/ e2e（在 coolrt 上运行）。
#
# M0 骨架：先接 unit 与 CLI 冒烟；golden/e2e 待编译链路（coolfe++/coolicc）接入后再加，
# 且 e2e 需在无 GPU / 无 PTX JIT 环境可跳过（见方案 §11）。

add_executable(test_version unit/test_version.cpp)
target_link_libraries(test_version PRIVATE hacc_support)
add_test(NAME unit_version COMMAND test_version)

# M1.2.0：cpfe 命令行拼装（纯逻辑，不调用 cpfe）。
add_executable(test_edg_invoke unit/test_edg_invoke.cpp
    "${CMAKE_SOURCE_DIR}/src/frontend/edg_invoke.cpp")
target_link_libraries(test_edg_invoke PRIVATE hacc_support)
add_test(NAME unit_edg_invoke COMMAND test_edg_invoke)

# B8：主机 C++ 编译器解析（纯逻辑：显式 > $HACC_HOST_CXX > $HACC_GXX > g++）。
add_executable(test_toolchain unit/test_toolchain.cpp)
target_link_libraries(test_toolchain PRIVATE hacc_support)
add_test(NAME unit_toolchain COMMAND test_toolchain)

# M2.0：主机源码改写器（`<<<>>>` → haccLaunch + 剔除 `__global__` 函数体）。
# 纯文本变换，无需 EDG/LLVM/GPU。
add_executable(test_launch_rewrite unit/test_launch_rewrite.cpp)
target_link_libraries(test_launch_rewrite PRIVATE hacc_support)
add_test(NAME unit_launch_rewrite COMMAND test_launch_rewrite)

# M2.2b：主机桩生成器（内嵌映像 + 注册描述）。纯文本变换，无需 EDG/LLVM/GPU。
add_executable(test_stub_gen unit/test_stub_gen.cpp)
target_link_libraries(test_stub_gen PRIVATE hacc_support)
add_test(NAME unit_stub_gen COMMAND test_stub_gen)

# M2.0 golden：对真实 .cu 跑改写器，与期望 host 源逐字节比对。
add_executable(test_launch_rewrite_golden unit/test_launch_rewrite_golden.cpp)
target_link_libraries(test_launch_rewrite_golden PRIVATE hacc_support)
add_test(NAME golden_host_vector_add
    COMMAND test_launch_rewrite_golden
            "${CMAKE_CURRENT_SOURCE_DIR}/golden/host/vector_add.cu"
            "${CMAKE_CURRENT_SOURCE_DIR}/golden/host/vector_add.hacc.cpp")

# M2.1 golden：coolfe++ --emit-host-only 产主机源，与期望逐字节比对。
# 纯文本路径（不调用 cpfe），无需 EDG/LLVM/GPU。
set(_host_out "${CMAKE_CURRENT_BINARY_DIR}/vector_add.hacc.cpp")
add_test(NAME golden_host_coolfe_emit_host
    COMMAND coolfe++ --emit-host-only=${_host_out}
            "${CMAKE_CURRENT_SOURCE_DIR}/golden/host/vector_add.cu")
add_test(NAME golden_host_coolfe_emit_host_match
    COMMAND diff -u "${CMAKE_CURRENT_SOURCE_DIR}/golden/host/vector_add.hacc.cpp" "${_host_out}")
set_tests_properties(golden_host_coolfe_emit_host_match PROPERTIES
    DEPENDS golden_host_coolfe_emit_host)

add_test(NAME cli_hacc_version    COMMAND hacc --version)
add_test(NAME cli_coolfe_version  COMMAND coolfe++ --version)
add_test(NAME cli_coolicc_version COMMAND coolicc --version)
set_tests_properties(cli_hacc_version    PROPERTIES PASS_REGULAR_EXPRESSION "hacc")
set_tests_properties(cli_coolfe_version  PROPERTIES PASS_REGULAR_EXPRESSION "coolfe")
set_tests_properties(cli_coolicc_version PROPERTIES PASS_REGULAR_EXPRESSION "coolicc")

# M1.2.0 golden：.cu → EDG IL（二进制 + 人类可读文本）。
# 仅当 HACC_WITH_EDG 且 cpfe 存在时启用（否则跳过，保持无 EDG 环境可构建）。
if(HACC_WITH_EDG AND EXISTS "${HACC_DEFAULT_CPFE}")
    # 若找到 COOLRuntime 头，则给 .cu 传 -I（供 #include <cool/...>）；否则不传。
    set(_coolrt_inc "")
    if(HACC_COOLRUNTIME_INCLUDE)
        set(_coolrt_inc "-I${HACC_COOLRUNTIME_INCLUDE}")
    endif()
    set(_ek_cil "${CMAKE_CURRENT_BINARY_DIR}/empty_kernel.cil")
    set(_ek_txt "${CMAKE_CURRENT_BINARY_DIR}/empty_kernel.il.txt")
    add_test(NAME golden_frontend_empty_kernel
        COMMAND coolfe++ --cpfe "${HACC_DEFAULT_CPFE}"
                --edg-base-dir "${HACC_DEFAULT_EDG_BASE}"
                -o "${_ek_cil}"
                --emit-il-text=${_ek_txt}
                "${CMAKE_CURRENT_SOURCE_DIR}/golden/frontend/empty_kernel.cu")
    # 文本 IL 应含内核名（用 grep 直接校验，避免 cmake -E grep 在 CMake 4.x 缺失）。
    add_test(NAME golden_frontend_empty_kernel_text
        COMMAND grep -q "empty_kernel" "${_ek_txt}")
    set_tests_properties(golden_frontend_empty_kernel_text PROPERTIES
        DEPENDS golden_frontend_empty_kernel)

    # M1.2.1 golden：coolicc 读回 .cil，列出 file scope 的 routine（应含内核名）。
    # 依赖 golden_frontend_empty_kernel 先产出 .cil。
    add_test(NAME golden_device_il_list_empty_kernel
        COMMAND coolicc --emit=il-list "${_ek_cil}")
    set_tests_properties(golden_device_il_list_empty_kernel PROPERTIES
        PASS_REGULAR_EXPRESSION "empty_kernel"
        DEPENDS golden_frontend_empty_kernel)

    # M2.1 golden：含 `<<<>>>` + `__global__` 的 .cu → device 改写源 → cpfe → IL。
    # 校验：① cpfe 成功（能解析改写后的 haccLaunch）；② 文本 IL 仍含内核 routine
    # （证明 device 源保留了 `__global__` 函数体，设备 IL 未丢内核）。
    if(HACC_COOLRUNTIME_INCLUDE)
        set(_vh_cil "${CMAKE_CURRENT_BINARY_DIR}/vector_add.cil")
        set(_vh_txt "${CMAKE_CURRENT_BINARY_DIR}/vector_add.il.txt")
        add_test(NAME golden_host_device_il_vector_add
            COMMAND coolfe++ --cpfe "${HACC_DEFAULT_CPFE}"
                    --edg-base-dir "${HACC_DEFAULT_EDG_BASE}"
                    -o "${_vh_cil}"
                    --emit-il-text=${_vh_txt}
                    -I "${HACC_COOLRUNTIME_INCLUDE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/golden/host/vector_add.cu")
        add_test(NAME golden_host_device_il_vector_add_has_kernel
            COMMAND grep -q "routine.*vector_add" "${_vh_txt}")
        set_tests_properties(golden_host_device_il_vector_add_has_kernel PROPERTIES
            DEPENDS golden_host_device_il_vector_add)
    endif()

    # M1.2.2 golden：.cil → LLVM IR → PTX（需 EDG + LLVM 同时开启）。
    # 校验最小内核产出 `.visible .entry empty_kernel`。
    if(HACC_WITH_LLVM)
        add_test(NAME golden_device_il_to_ptx_empty_kernel
            COMMAND coolicc --from-il -arch=sm_80 "${_ek_cil}")
        set_tests_properties(golden_device_il_to_ptx_empty_kernel PROPERTIES
            PASS_REGULAR_EXPRESSION "\\.visible \\.entry empty_kernel"
            DEPENDS golden_frontend_empty_kernel)
    endif()

    # M1.2.3 golden：标量参数 + 简单语句（赋值/算术/if）→ PTX。
    # 校验参数（.param .u64/.u32）、算术（add.s32）与分支（setp/bra）。
    if(HACC_WITH_LLVM)
        set(_sk_cil "${CMAKE_CURRENT_BINARY_DIR}/scalar_kernel.cil")
        add_test(NAME golden_frontend_scalar_kernel
            COMMAND coolfe++ --cpfe "${HACC_DEFAULT_CPFE}"
                    --edg-base-dir "${HACC_DEFAULT_EDG_BASE}"
                    -o "${_sk_cil}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/golden/frontend/scalar_kernel.cu")
        add_test(NAME golden_device_il_to_ptx_scalar_kernel
            COMMAND coolicc --from-il -arch=sm_80 "${_sk_cil}")
        set_tests_properties(golden_device_il_to_ptx_scalar_kernel PROPERTIES
            PASS_REGULAR_EXPRESSION "\\.visible \\.entry add_kernel"
            DEPENDS golden_frontend_scalar_kernel)
    endif()

    # M1.2.4 golden：内建变量（threadIdx/blockIdx）+ __syncthreads() → PTX。
    # 需 COOLRuntime 头（.cu 用 #include <cool/cool_runtime.h> 提供 __global__ 与内建）；
    # 未找到头时跳过。校验 %tid.x / %ctaid.x 与 barrier0。
    if(HACC_WITH_LLVM AND HACC_COOLRUNTIME_INCLUDE)
        set(_tk_cil "${CMAKE_CURRENT_BINARY_DIR}/tid_kernel.cil")
        set(_tk_ptx "${CMAKE_CURRENT_BINARY_DIR}/tid_kernel.ptx")
        add_test(NAME golden_frontend_tid_kernel
            COMMAND coolfe++ --cpfe "${HACC_DEFAULT_CPFE}"
                    --edg-base-dir "${HACC_DEFAULT_EDG_BASE}"
                    -o "${_tk_cil}"
                    -I "${HACC_COOLRUNTIME_INCLUDE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/golden/frontend/tid_kernel.cu")
        # 产出 PTX 到文件（供后续 grep 校验具体指令）。
        add_test(NAME golden_device_il_to_ptx_tid_kernel
            COMMAND coolicc --from-il -arch=sm_80 -o "${_tk_ptx}" "${_tk_cil}")
        set_tests_properties(golden_device_il_to_ptx_tid_kernel PROPERTIES
            DEPENDS golden_frontend_tid_kernel)
        # 校验内核入口与内建映射：entry tid_kernel、threadIdx.x → %tid.x、
        # blockIdx.x → %ctaid.x、__syncthreads → bar.sync。
        add_test(NAME golden_device_il_to_ptx_tid_kernel_builtins
            COMMAND grep -qE "\\.visible \\.entry tid_kernel|%tid\\.x|%ctaid\\.x|bar\\.sync"
                    "${_tk_ptx}")
        set_tests_properties(golden_device_il_to_ptx_tid_kernel_builtins PROPERTIES
            DEPENDS golden_device_il_to_ptx_tid_kernel)
    endif()

    # M2.4 golden：指针算术（p + i → GEP）。对真 vectorAdd 的 device 源产 PTX，
    # 校验内核入口与按元素步进的地址计算（mul.wide + add.s64，即 GEP 的 PTX 形态）。
    # 需 COOLRuntime 头（.cu 用 #include <cool/cool_runtime.h>）。
    if(HACC_WITH_LLVM AND HACC_COOLRUNTIME_INCLUDE)
        set(_va_cil "${CMAKE_CURRENT_BINARY_DIR}/e2e_vector_add.cil")
        set(_va_ptx "${CMAKE_CURRENT_BINARY_DIR}/e2e_vector_add.ptx")
        add_test(NAME golden_frontend_vector_add_ptr_arith
            COMMAND coolfe++ --cpfe "${HACC_DEFAULT_CPFE}"
                    --edg-base-dir "${HACC_DEFAULT_EDG_BASE}"
                    -o "${_va_cil}"
                    -I "${HACC_COOLRUNTIME_INCLUDE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/vector_add.cu")
        add_test(NAME golden_device_il_to_ptx_vector_add_ptr_arith
            COMMAND coolicc --from-il -arch=sm_80 -o "${_va_ptx}" "${_va_cil}")
        set_tests_properties(golden_device_il_to_ptx_vector_add_ptr_arith PROPERTIES
            DEPENDS golden_frontend_vector_add_ptr_arith)
        # 校验：内核入口 + 指针步进（mul.wide.s32 索引缩放 + add.s64 地址相加）。
        add_test(NAME golden_device_il_to_ptx_vector_add_ptr_arith_gep
            COMMAND grep -qE "\\.visible \\.entry vector_add|mul\\.wide\\.s32|add\\.s64"
                    "${_va_ptx}")
        set_tests_properties(golden_device_il_to_ptx_vector_add_ptr_arith_gep PROPERTIES
            DEPENDS golden_device_il_to_ptx_vector_add_ptr_arith)
    endif()

    # M1.2.5 e2e：hacc driver 编排 `.cu` → PTX（coolfe++ → coolicc）。
    # hacc 按 argv[0] 同目录定位子工具（构建产物在一起）。
    if(HACC_WITH_LLVM)
        set(_e2e_sk_ptx "${CMAKE_CURRENT_BINARY_DIR}/e2e_scalar_kernel.ptx")
        add_test(NAME e2e_hacc_scalar_kernel
            COMMAND hacc "${CMAKE_CURRENT_SOURCE_DIR}/golden/frontend/scalar_kernel.cu"
                    --emit=ptx -o "${_e2e_sk_ptx}")
        # 校验端到端产物：内核入口 + 算术。
        add_test(NAME e2e_hacc_scalar_kernel_ptx
            COMMAND grep -qE "\\.visible \\.entry add_kernel|add\\.s32" "${_e2e_sk_ptx}")

        set_tests_properties(e2e_hacc_scalar_kernel_ptx PROPERTIES
            DEPENDS e2e_hacc_scalar_kernel)

        # M2.4 e2e：真 vectorAdd 编译 + 运行 + 结果校验（c[i] == 3*i）。
        # 无可用 CUDA 设备时样例返回 77，测试框架按 SKIP 处理（对齐 coolrt 的
        # test_kernel 做法）。
        set(_e2e_va_run_exe "${CMAKE_CURRENT_BINARY_DIR}/e2e_vector_add_run")
        add_test(NAME e2e_hacc_vector_add_run_build
            COMMAND hacc "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/vector_add.cu"
                    -o "${_e2e_va_run_exe}")
        add_test(NAME e2e_hacc_vector_add_run
            COMMAND "${_e2e_va_run_exe}")
        set_tests_properties(e2e_hacc_vector_add_run PROPERTIES
            DEPENDS e2e_hacc_vector_add_run_build
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "vectorAdd OK")

        # M3.0 golden：向量类型（float4）作为内核参数 → PTX 按 <N x T> 向量化
        # （.param .align 16 .b8 [16] + ld.param.v4.f32）。需 COOLRuntime 头。
        if(HACC_COOLRUNTIME_INCLUDE)
            set(_vt_cil "${CMAKE_CURRENT_BINARY_DIR}/e2e_vector_type.cil")
            set(_vt_ptx "${CMAKE_CURRENT_BINARY_DIR}/e2e_vector_type.ptx")
            add_test(NAME golden_frontend_vector_type
                COMMAND coolfe++ --cpfe "${HACC_DEFAULT_CPFE}"
                        --edg-base-dir "${HACC_DEFAULT_EDG_BASE}"
                        -o "${_vt_cil}"
                        -I "${HACC_COOLRUNTIME_INCLUDE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/vector_type.cu")
            add_test(NAME golden_device_il_to_ptx_vector_type
                COMMAND coolicc --from-il -arch=sm_80 -o "${_vt_ptx}" "${_vt_cil}")
            set_tests_properties(golden_device_il_to_ptx_vector_type PROPERTIES
                DEPENDS golden_frontend_vector_type)
            # 校验：内核入口 + 向量化参数加载（ld.param.v4.f32）+ 对齐 16。
            add_test(NAME golden_device_il_to_ptx_vector_type_vec
                COMMAND grep -qE "\\.visible \\.entry add4|ld\\.param\\.v4\\.f32|\\.param \\.align 16 \\.b8 add4_param_0\[16\]"
                        "${_vt_ptx}")
            set_tests_properties(golden_device_il_to_ptx_vector_type_vec PROPERTIES
                DEPENDS golden_device_il_to_ptx_vector_type)
        endif()

        # __device__ 变量主机侧访问 e2e：cudaMemcpyFromSymbol 读回 / GetSymbolAddress 取址 /
        # cudaMemcpyToSymbol 写回。无可用 CUDA 设备时样例返回 77，按 SKIP 处理。
        set(_e2e_dsr_exe "${CMAKE_CURRENT_BINARY_DIR}/e2e_device_symbol_readback_run")
        add_test(NAME e2e_hacc_device_symbol_readback_build
            COMMAND hacc "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/device_symbol_readback.cu"
                    -o "${_e2e_dsr_exe}")
        add_test(NAME e2e_hacc_device_symbol_readback
            COMMAND "${_e2e_dsr_exe}")
        set_tests_properties(e2e_hacc_device_symbol_readback PROPERTIES
            DEPENDS e2e_hacc_device_symbol_readback_build
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "deviceSymbolReadback OK")

        # M3.0 e2e：向量类型（float4）编译 + 运行 + 结果校验（out == 110）。
        # 无可用 CUDA 设备时样例返回 77，测试框架按 SKIP 处理。
        set(_e2e_vt_run_exe "${CMAKE_CURRENT_BINARY_DIR}/e2e_vector_type_run")
        add_test(NAME e2e_hacc_vector_type_run_build
            COMMAND hacc "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/vector_type.cu"
                    -o "${_e2e_vt_run_exe}")
        add_test(NAME e2e_hacc_vector_type_run
            COMMAND "${_e2e_vt_run_exe}")
        set_tests_properties(e2e_hacc_vector_type_run PROPERTIES
            DEPENDS e2e_hacc_vector_type_run_build
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "vectorType OK")

        # M3.3 golden：dim3 作为内核参数（3 分量聚合 → struct {i32,i32,i32}）。
        # 校验：PTX 参数按 12 字节（.param .align 4 .b8 dim3_kernel_param_0[12]）传递，
        # 且成员访问走 ld.param.u32（非向量化 v2/v4）——证明 3 分量未被误判为向量。
        # 需 COOLRuntime 头（.cu 用 #include <cool/cool_runtime.h>）。
        if(HACC_COOLRUNTIME_INCLUDE)
            set(_d3_cil "${CMAKE_CURRENT_BINARY_DIR}/e2e_dim3_param.cil")
            set(_d3_ptx "${CMAKE_CURRENT_BINARY_DIR}/e2e_dim3_param.ptx")
            add_test(NAME golden_frontend_dim3_param
                COMMAND coolfe++ --cpfe "${HACC_DEFAULT_CPFE}"
                        --edg-base-dir "${HACC_DEFAULT_EDG_BASE}"
                        -o "${_d3_cil}"
                        -I "${HACC_COOLRUNTIME_INCLUDE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/dim3_param.cu")
            add_test(NAME golden_device_il_to_ptx_dim3_param
                COMMAND coolicc --from-il -arch=sm_80 -o "${_d3_ptx}" "${_d3_cil}")
            set_tests_properties(golden_device_il_to_ptx_dim3_param PROPERTIES
                DEPENDS golden_frontend_dim3_param)
            # 校验：内核入口 + dim3 参数 12 字节（align 4）+ 逐字段 ld.param.u32。
            add_test(NAME golden_device_il_to_ptx_dim3_param_struct
                COMMAND grep -qE "\\.visible \\.entry dim3_kernel|\\.param \\.align 4 \\.b8 dim3_kernel_param_0\\[12\\]|ld\\.param\\.u32"
                        "${_d3_ptx}")
            set_tests_properties(golden_device_il_to_ptx_dim3_param_struct PROPERTIES
                DEPENDS golden_device_il_to_ptx_dim3_param)
        endif()

        # M3.1 golden：__shared__ 静态共享内存 → PTX .shared 段 + st.shared/ld.shared。
        # 需 COOLRuntime 头（.cu 用 #include <cool/cool_runtime.h>）。
        if(HACC_COOLRUNTIME_INCLUDE)
            set(_sr_cil "${CMAKE_CURRENT_BINARY_DIR}/e2e_shared_reduce.cil")
            set(_sr_ptx "${CMAKE_CURRENT_BINARY_DIR}/e2e_shared_reduce.ptx")
            add_test(NAME golden_frontend_shared_reduce
                COMMAND coolfe++ --cpfe "${HACC_DEFAULT_CPFE}"
                        --edg-base-dir "${HACC_DEFAULT_EDG_BASE}"
                        -o "${_sr_cil}"
                        -I "${HACC_COOLRUNTIME_INCLUDE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/shared_reduce.cu")
            add_test(NAME golden_device_il_to_ptx_shared_reduce
                COMMAND coolicc --from-il -arch=sm_80 -o "${_sr_ptx}" "${_sr_cil}")
            set_tests_properties(golden_device_il_to_ptx_shared_reduce PROPERTIES
                DEPENDS golden_frontend_shared_reduce)
            # 校验：内核入口 + .shared 段 + 共享内存读写指令。
            add_test(NAME golden_device_il_to_ptx_shared_reduce_shared
                COMMAND grep -qE "\\.visible \\.entry shared_reduce|\\.shared \\.align [0-9]+ \\.b8 __shared_sdata\\[1024\\]|st\\.shared|ld\\.shared"
                        "${_sr_ptx}")
            set_tests_properties(golden_device_il_to_ptx_shared_reduce_shared PROPERTIES
                DEPENDS golden_device_il_to_ptx_shared_reduce)
        endif()

        # M3.2 golden：__constant__ 常量内存 → PTX .const 段 + ld.const。
        # 需 COOLRuntime 头（.cu 用 #include <cool/cool_runtime.h>）。
        if(HACC_COOLRUNTIME_INCLUDE)
            set(_cm_cil "${CMAKE_CURRENT_BINARY_DIR}/e2e_const_mem.cil")
            set(_cm_ptx "${CMAKE_CURRENT_BINARY_DIR}/e2e_const_mem.ptx")
            add_test(NAME golden_frontend_const_mem
                COMMAND coolfe++ --cpfe "${HACC_DEFAULT_CPFE}"
                        --edg-base-dir "${HACC_DEFAULT_EDG_BASE}"
                        -o "${_cm_cil}"
                        -I "${HACC_COOLRUNTIME_INCLUDE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/const_mem.cu")
            add_test(NAME golden_device_il_to_ptx_const_mem
                COMMAND coolicc --from-il -arch=sm_80 -o "${_cm_ptx}" "${_cm_cil}")
            set_tests_properties(golden_device_il_to_ptx_const_mem PROPERTIES
                DEPENDS golden_frontend_const_mem)
            # 校验：内核入口 + .const 段（可见符号 c_scale）+ 常量内存加载指令。
            add_test(NAME golden_device_il_to_ptx_const_mem_const
                COMMAND grep -qE "\\.visible \\.entry scale_kernel|\\.visible \\.const \\.align [0-9]+ \\.f32 c_scale|ld\\.const\\.f32"
                        "${_cm_ptx}")
            set_tests_properties(golden_device_il_to_ptx_const_mem_const PROPERTIES
                DEPENDS golden_device_il_to_ptx_const_mem)
        endif()

        # M4.1 golden：__device__ 设备全局 → PTX .global 段（addrspace 1）+ ld.global/st.global。
        # 需 COOLRuntime 头（.cu 用 #include <cool/cool_runtime.h>）。
        if(HACC_COOLRUNTIME_INCLUDE)
            set(_dg_cil "${CMAKE_CURRENT_BINARY_DIR}/e2e_dev_global.cil")
            set(_dg_ptx "${CMAKE_CURRENT_BINARY_DIR}/e2e_dev_global.ptx")
            add_test(NAME golden_frontend_dev_global
                COMMAND coolfe++ --cpfe "${HACC_DEFAULT_CPFE}"
                        --edg-base-dir "${HACC_DEFAULT_EDG_BASE}"
                        -o "${_dg_cil}"
                        -I "${HACC_COOLRUNTIME_INCLUDE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/dev_global.cu")
            add_test(NAME golden_device_il_to_ptx_dev_global
                COMMAND coolicc --from-il -arch=sm_80 -o "${_dg_ptx}" "${_dg_cil}")
            set_tests_properties(golden_device_il_to_ptx_dev_global PROPERTIES
                DEPENDS golden_frontend_dev_global)
            # 校验：内核入口 + .global 段（可见符号 g_counter，带静态初值 100）+ 全局访存指令。
            add_test(NAME golden_device_il_to_ptx_dev_global_global
                COMMAND grep -qE "\\.visible \\.entry dev_global_kernel|\\.visible \\.global \\.align [0-9]+ \\.u32 g_counter = 100|ld\\.global\\.u32|st\\.global\\.u32"
                        "${_dg_ptx}")
            set_tests_properties(golden_device_il_to_ptx_dev_global_global PROPERTIES
                DEPENDS golden_device_il_to_ptx_dev_global)
        endif()

        # M4.2 golden：__restrict__ 指针形参 → LLVM noalias（IR 层校验；PTX 层不可见）。
        # 需 COOLRuntime 头（.cu 用 #include <cool/cool_runtime.h>）。
        if(HACC_COOLRUNTIME_INCLUDE)
            set(_rs_cil "${CMAKE_CURRENT_BINARY_DIR}/e2e_restrict_param.cil")
            set(_rs_ll "${CMAKE_CURRENT_BINARY_DIR}/e2e_restrict_param.ll")
            set(_rs_ptx "${CMAKE_CURRENT_BINARY_DIR}/e2e_restrict_param.ptx")
            add_test(NAME golden_frontend_restrict_param
                COMMAND coolfe++ --cpfe "${HACC_DEFAULT_CPFE}"
                        --edg-base-dir "${HACC_DEFAULT_EDG_BASE}"
                        -o "${_rs_cil}"
                        -I "${HACC_COOLRUNTIME_INCLUDE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/restrict_param.cu")
            # IR 层：两个指针形参都带 noalias（M4.2 的核心校验点）。
            add_test(NAME golden_device_ir_restrict_param
                COMMAND coolicc --from-il --emit=llvm-ir -o "${_rs_ll}" "${_rs_cil}")
            set_tests_properties(golden_device_ir_restrict_param PROPERTIES
                DEPENDS golden_frontend_restrict_param)
            add_test(NAME golden_device_ir_restrict_param_noalias
                COMMAND grep -qE "define ptx_kernel void @axpy\\(ptr noalias %[0-9]+, ptr noalias %[0-9]+\\)"
                        "${_rs_ll}")
            set_tests_properties(golden_device_ir_restrict_param_noalias PROPERTIES
                DEPENDS golden_device_ir_restrict_param)
            # PTX 层回归：__restrict__ 不改变 PTX 语义（仍应产出内核入口）。
            add_test(NAME golden_device_il_to_ptx_restrict_param
                COMMAND coolicc --from-il -arch=sm_80 -o "${_rs_ptx}" "${_rs_cil}")
            set_tests_properties(golden_device_il_to_ptx_restrict_param PROPERTIES
                DEPENDS golden_frontend_restrict_param)
            add_test(NAME golden_device_il_to_ptx_restrict_param_entry
                COMMAND grep -qE "\\.visible \\.entry axpy" "${_rs_ptx}")
            set_tests_properties(golden_device_il_to_ptx_restrict_param_entry PROPERTIES
                DEPENDS golden_device_il_to_ptx_restrict_param)
            # 诊断：__restrict__ 用于非指针形参 → 报错且带源位置（第 7 行）。
            add_test(NAME diag_restrict_non_pointer
                COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_diag_position.sh"
                        "$<TARGET_FILE:hacc>"
                        "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/diag_restrict_non_pointer.cu"
                        "${HACC_COOLRUNTIME_INCLUDE}" "7")
        endif()

        # M3.4 诊断质量：翻译错误消息带源位置（"文件:行: 成因"）。
        # 用脚本捕获 stderr 并校验 ":<行号>: " 前缀（错误在 stderr，PASS_REGULAR_EXPRESSION 只匹配 stdout）。
        if(HACC_COOLRUNTIME_INCLUDE)
            add_test(NAME diag_position_bad_op
                COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_diag_position.sh"
                        "$<TARGET_FILE:hacc>"
                        "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/diag_bad_op.cu"
                        "${HACC_COOLRUNTIME_INCLUDE}" "10")
            add_test(NAME diag_position_bad_shared
                COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_diag_position.sh"
                        "$<TARGET_FILE:hacc>"
                        "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/diag_bad_shared.cu"
                        "${HACC_COOLRUNTIME_INCLUDE}" "6")
        endif()

        # M3.5 边界用例：非法/未覆盖构造应 fail loud（编译失败 + 诊断匹配）。
        # 用脚本捕获 stderr 并校验诊断正则（错误在 stderr，PASS_REGULAR_EXPRESSION 只匹配 stdout）。
        if(HACC_COOLRUNTIME_INCLUDE)
            # M4.6：__device__ 变量声明在函数内（子集只允许文件作用域）→ fail loud。
            add_test(NAME diag_device_var_in_kernel
                COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_diag_fail.sh"
                        "$<TARGET_FILE:hacc>"
                        "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/diag_device_var_in_kernel.cu"
                        "${HACC_COOLRUNTIME_INCLUDE}" "__device__ 变量不能声明在函数内")
            # M4.6：未覆盖的原子类型（double）→ 前端无匹配重载，fail loud。
            # 锁住原子类型不会被悄悄泛化：加了头文件声明却没补翻译时必须让测试失败。
            add_test(NAME diag_atomic_type_unsupported
                COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_diag_fail.sh"
                        "$<TARGET_FILE:hacc>"
                        "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/diag_atomic_type_unsupported.cu"
                        "${HACC_COOLRUNTIME_INCLUDE}" "line 15: error: no instance")
            # 类型不匹配（指针赋整型）→ 前端 cpfe 报错（第 8 行）。
            add_test(NAME diag_type_mismatch
                COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_diag_fail.sh"
                        "$<TARGET_FILE:hacc>"
                        "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/diag_type_mismatch.cu"
                        "${HACC_COOLRUNTIME_INCLUDE}" "line 8: error:")
        endif()

        # M3.5 边界用例（正向）：局部数组 + 动态下标（a[i]）应正常编译为 PTX。
        # 回归保护：曾因数组退化的 GEP 源类型误用元素类型而产出非法 LLVM IR。
        if(HACC_COOLRUNTIME_INCLUDE)
            set(_lai_ptx "${CMAKE_CURRENT_BINARY_DIR}/e2e_local_array_index.ptx")
            add_test(NAME golden_device_local_array_index
                COMMAND hacc "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/local_array_index.cu"
                        --emit=ptx -o "${_lai_ptx}" -I "${HACC_COOLRUNTIME_INCLUDE}")
            add_test(NAME golden_device_local_array_index_ptx
                COMMAND grep -qE "\\.visible \\.entry local_array_index" "${_lai_ptx}")
            set_tests_properties(golden_device_local_array_index_ptx PROPERTIES
                DEPENDS golden_device_local_array_index)
        endif()

        # M3.5 samples：samples/ 下的示例应始终可编译 + 可运行（结果自校验）。
        # 无可用 CUDA 设备时示例返回 77，测试框架按 SKIP 处理。
        # 与 golden/e2e 的区别：samples 是面向用户的示例，此处仅做「示例不腐化」回归。
        # M4.6 samples：新增原子归约 / 设备全局 / 栅栏三个示例（同受「示例不腐化」回归保护）。
        if(HACC_COOLRUNTIME_INCLUDE)
            foreach(_sample vector_add vector_type shared_reduce constant_pi dim3_grid
                            atomic_reduce device_global threadfence stream_launch)
                set(_sample_exe "${CMAKE_CURRENT_BINARY_DIR}/sample_${_sample}")
                add_test(NAME sample_${_sample}_build
                    COMMAND hacc "${CMAKE_SOURCE_DIR}/samples/${_sample}.cu"
                            -o "${_sample_exe}" -I "${HACC_COOLRUNTIME_INCLUDE}")
                add_test(NAME sample_${_sample}_run
                    COMMAND "${_sample_exe}")
                set_tests_properties(sample_${_sample}_run PROPERTIES
                    DEPENDS sample_${_sample}_build
                    SKIP_RETURN_CODE 77
                    PASS_REGULAR_EXPRESSION "OK")
            endforeach()
        endif()

        # M4.5 指针地址空间传播：cool_address_space(3) 标注的指针，其访存应落 .shared
        # （未标注时是 generic ld/st——该用例正是锁住「标注后地址空间真的传播下去了」）。
        if(HACC_COOLRUNTIME_INCLUDE)
            set(_pas_ptx "${CMAKE_CURRENT_BINARY_DIR}/e2e_pointer_address_space.ptx")
            add_test(NAME golden_pointer_address_space
                COMMAND hacc "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/pointer_address_space.cu"
                        --emit=ptx -o "${_pas_ptx}" -I "${HACC_COOLRUNTIME_INCLUDE}")
            add_test(NAME golden_pointer_address_space_shared_store
                COMMAND grep -qE "st\\.shared\\.u32" "${_pas_ptx}")
            set_tests_properties(golden_pointer_address_space_shared_store PROPERTIES
                DEPENDS golden_pointer_address_space)
            add_test(NAME golden_pointer_address_space_shared_load
                COMMAND grep -qE "ld\\.shared\\.u32" "${_pas_ptx}")
            set_tests_properties(golden_pointer_address_space_shared_load PROPERTIES
                DEPENDS golden_pointer_address_space)
            # 内核参数仍是 generic（未标注），其写应经 cvta 落成 .global——防止改动
            # 误把「未标注」也升格成某个地址空间。
            add_test(NAME golden_pointer_address_space_global_store
                COMMAND grep -qE "cvta\\.to\\.global" "${_pas_ptx}")
            set_tests_properties(golden_pointer_address_space_global_store PROPERTIES
                DEPENDS golden_pointer_address_space)

            # 运行校验：结果正确性（无设备时 SDK 约定返回 77 → SKIP）。
            set(_pas_exe "${CMAKE_CURRENT_BINARY_DIR}/e2e_pointer_address_space_run")
            add_test(NAME e2e_pointer_address_space_run_build
                COMMAND hacc "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/pointer_address_space.cu"
                        -o "${_pas_exe}" -I "${HACC_COOLRUNTIME_INCLUDE}")
            add_test(NAME e2e_pointer_address_space_run
                COMMAND "${_pas_exe}")
            set_tests_properties(e2e_pointer_address_space_run PROPERTIES
                DEPENDS e2e_pointer_address_space_run_build
                SKIP_RETURN_CODE 77
                PASS_REGULAR_EXPRESSION "pointerAddressSpace OK")

            # 边界用例：cool_address_space 取值非法 → fail loud（不是静默当 generic）。
            add_test(NAME diag_address_space_invalid
                COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_diag_fail.sh"
                        "$<TARGET_FILE:hacc>"
                        "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/diag_address_space_invalid.cu"
                        "${HACC_COOLRUNTIME_INCLUDE}" "cool_address_space 取值非法")
        endif()

        # M3.1 e2e：__shared__ 共享内存编译 + 运行 + 结果校验（out[i] == N-1-i）。
        # 无可用 CUDA 设备时样例返回 77，测试框架按 SKIP 处理。
        set(_e2e_sr_run_exe "${CMAKE_CURRENT_BINARY_DIR}/e2e_shared_reduce_run")
        add_test(NAME e2e_hacc_shared_reduce_run_build
            COMMAND hacc "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/shared_reduce.cu"
                    -o "${_e2e_sr_run_exe}")
        add_test(NAME e2e_hacc_shared_reduce_run
            COMMAND "${_e2e_sr_run_exe}")
        set_tests_properties(e2e_hacc_shared_reduce_run PROPERTIES
            DEPENDS e2e_hacc_shared_reduce_run_build
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "sharedReduce OK")

        # M3.2 e2e：__constant__ 常量内存编译 + 运行 + 结果校验（out[i] == 6.0）。
        # 无可用 CUDA 设备时样例返回 77，测试框架按 SKIP 处理。
        set(_e2e_cm_run_exe "${CMAKE_CURRENT_BINARY_DIR}/e2e_const_mem_run")
        add_test(NAME e2e_hacc_const_mem_run_build
            COMMAND hacc "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/const_mem.cu"
                    -o "${_e2e_cm_run_exe}")
        add_test(NAME e2e_hacc_const_mem_run
            COMMAND "${_e2e_cm_run_exe}")
        set_tests_properties(e2e_hacc_const_mem_run PROPERTIES
            DEPENDS e2e_hacc_const_mem_run_build
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "constMem OK")

        # M5.1 golden：循环子集（while / for / do-while）→ PTX 循环结构。
        if(HACC_COOLRUNTIME_INCLUDE)
            set(_lf_cil "${CMAKE_CURRENT_BINARY_DIR}/e2e_loop_forms.cil")
            set(_lf_ptx "${CMAKE_CURRENT_BINARY_DIR}/e2e_loop_forms.ptx")
            add_test(NAME golden_frontend_loop_forms
                COMMAND coolfe++ --cpfe "${HACC_DEFAULT_CPFE}"
                        --edg-base-dir "${HACC_DEFAULT_EDG_BASE}"
                        -o "${_lf_cil}"
                        -I "${HACC_COOLRUNTIME_INCLUDE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/loop_forms.cu")
            add_test(NAME golden_device_il_to_ptx_loop_forms
                COMMAND coolicc --from-il -arch=sm_80 -o "${_lf_ptx}" "${_lf_cil}")
            set_tests_properties(golden_device_il_to_ptx_loop_forms PROPERTIES
                DEPENDS golden_frontend_loop_forms)
            # 三种循环各有独立入口 → 确认 stmk_while / stmk_for / stmk_end_test_while
            # 都被真正降级（而不是某个被静默跳过）。
            add_test(NAME golden_device_il_to_ptx_loop_forms_while
                COMMAND grep -qE "\\.visible \\.entry loop_while" "${_lf_ptx}")
            add_test(NAME golden_device_il_to_ptx_loop_forms_for
                COMMAND grep -qE "\\.visible \\.entry loop_for" "${_lf_ptx}")
            add_test(NAME golden_device_il_to_ptx_loop_forms_do
                COMMAND grep -qE "\\.visible \\.entry loop_do" "${_lf_ptx}")
            # 回跳分支：循环以 br 回跳表示（而非直线展开）。
            add_test(NAME golden_device_il_to_ptx_loop_forms_branch
                COMMAND grep -qE "bra\\.uni" "${_lf_ptx}")
            set_tests_properties(golden_device_il_to_ptx_loop_forms_while
                                 golden_device_il_to_ptx_loop_forms_for
                                 golden_device_il_to_ptx_loop_forms_do
                                 golden_device_il_to_ptx_loop_forms_branch PROPERTIES
                DEPENDS golden_device_il_to_ptx_loop_forms)
        endif()

        # M5.1 e2e：内核内 stride for 循环（N > 线程数，每线程迭代多次）+ 结果校验。
        set(_e2e_loop_exe "${CMAKE_CURRENT_BINARY_DIR}/e2e_loop_stride_run")
        add_test(NAME e2e_hacc_loop_stride_run_build
            COMMAND hacc "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/loop_stride.cu"
                    -o "${_e2e_loop_exe}")
        add_test(NAME e2e_hacc_loop_stride_run
            COMMAND "${_e2e_loop_exe}")
        set_tests_properties(e2e_hacc_loop_stride_run PROPERTIES
            DEPENDS e2e_hacc_loop_stride_run_build
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "loopStride OK")

        # M5.2 golden：break / continue（EDG 降为 stmk_goto + stmk_label）→ PTX 跳转结构。
        if(HACC_COOLRUNTIME_INCLUDE)
            set(_lc_cil "${CMAKE_CURRENT_BINARY_DIR}/e2e_loop_control.cil")
            set(_lc_ptx "${CMAKE_CURRENT_BINARY_DIR}/e2e_loop_control.ptx")
            add_test(NAME golden_frontend_loop_control
                COMMAND coolfe++ --cpfe "${HACC_DEFAULT_CPFE}"
                        --edg-base-dir "${HACC_DEFAULT_EDG_BASE}"
                        -o "${_lc_cil}"
                        -I "${HACC_COOLRUNTIME_INCLUDE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/loop_control.cu")
            add_test(NAME golden_device_il_to_ptx_loop_control
                COMMAND coolicc --from-il -arch=sm_80 -o "${_lc_ptx}" "${_lc_cil}")
            set_tests_properties(golden_device_il_to_ptx_loop_control PROPERTIES
                DEPENDS golden_frontend_loop_control)
            # 三个入口都在 → goto/label 在 for / while 两种循环里都降级成功。
            add_test(NAME golden_device_il_to_ptx_loop_control_break
                COMMAND grep -qE "entry ctrl_break" "${_lc_ptx}")
            add_test(NAME golden_device_il_to_ptx_loop_control_while
                COMMAND grep -qE "entry ctrl_while_break" "${_lc_ptx}")
            # continue 的指纹：i == 4 会编译成 setp.eq（另两个内核用的是 ge / gt）。
            add_test(NAME golden_device_il_to_ptx_loop_control_continue
                COMMAND grep -qE "setp\\.eq\\.s32" "${_lc_ptx}")
            set_tests_properties(golden_device_il_to_ptx_loop_control_break
                                 golden_device_il_to_ptx_loop_control_while
                                 golden_device_il_to_ptx_loop_control_continue PROPERTIES
                DEPENDS golden_device_il_to_ptx_loop_control)
        endif()

        # M5.2 e2e：break / continue 真机语义校验（continue 若跳到循环头会死循环）。
        set(_e2e_lc_exe "${CMAKE_CURRENT_BINARY_DIR}/e2e_loop_break_continue_run")
        add_test(NAME e2e_hacc_loop_break_continue_run_build
            COMMAND hacc "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/loop_break_continue.cu"
                    -o "${_e2e_lc_exe}")
        add_test(NAME e2e_hacc_loop_break_continue_run
            COMMAND "${_e2e_lc_exe}")
        set_tests_properties(e2e_hacc_loop_break_continue_run PROPERTIES
            DEPENDS e2e_hacc_loop_break_continue_run_build
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "loopCtrl OK")

        # M5.3 golden：switch → 比较链 + case 块（case 合并 / fallthrough / default）。
        if(HACC_COOLRUNTIME_INCLUDE)
            set(_sw_cil "${CMAKE_CURRENT_BINARY_DIR}/e2e_switch_dispatch.cil")
            set(_sw_ptx "${CMAKE_CURRENT_BINARY_DIR}/e2e_switch_dispatch.ptx")
            add_test(NAME golden_frontend_switch_dispatch
                COMMAND coolfe++ --cpfe "${HACC_DEFAULT_CPFE}"
                        --edg-base-dir "${HACC_DEFAULT_EDG_BASE}"
                        -o "${_sw_cil}"
                        -I "${HACC_COOLRUNTIME_INCLUDE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/switch_dispatch.cu")
            add_test(NAME golden_device_il_to_ptx_switch_dispatch
                COMMAND coolicc --from-il -arch=sm_80 -o "${_sw_ptx}" "${_sw_cil}")
            set_tests_properties(golden_device_il_to_ptx_switch_dispatch PROPERTIES
                DEPENDS golden_frontend_switch_dispatch)
            # 三个内核入口各自存在（basic / fallthrough / no_default）。
            add_test(NAME golden_device_il_to_ptx_switch_dispatch_basic
                COMMAND grep -qE "entry switch_basic" "${_sw_ptx}")
            add_test(NAME golden_device_il_to_ptx_switch_dispatch_fallthrough
                COMMAND grep -qE "entry switch_fallthrough" "${_sw_ptx}")
            add_test(NAME golden_device_il_to_ptx_switch_dispatch_no_default
                COMMAND grep -qE "entry switch_no_default" "${_sw_ptx}")
            # 比较链：case 值比较用 setp.eq。
            add_test(NAME golden_device_il_to_ptx_switch_dispatch_cmp
                COMMAND grep -qE "setp\\.eq\\.s32" "${_sw_ptx}")
            set_tests_properties(golden_device_il_to_ptx_switch_dispatch_basic
                                 golden_device_il_to_ptx_switch_dispatch_fallthrough
                                 golden_device_il_to_ptx_switch_dispatch_no_default
                                 golden_device_il_to_ptx_switch_dispatch_cmp PROPERTIES
                DEPENDS golden_device_il_to_ptx_switch_dispatch)
        endif()

        # M5.3 e2e：switch 真机语义（case 合并 / fallthrough / default）。
        set(_e2e_sw_exe "${CMAKE_CURRENT_BINARY_DIR}/e2e_switch_runtime_run")
        add_test(NAME e2e_hacc_switch_runtime_run_build
            COMMAND hacc "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/switch_runtime.cu"
                    -o "${_e2e_sw_exe}")
        add_test(NAME e2e_hacc_switch_runtime_run
            COMMAND "${_e2e_sw_exe}")
        set_tests_properties(e2e_hacc_switch_runtime_run PROPERTIES
            DEPENDS e2e_hacc_switch_runtime_run_build
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "switchDispatch OK")

        # M5.4 golden：__device__ 函数 → PTX .func + call.uni（独立函数，非内联）。
        if(HACC_COOLRUNTIME_INCLUDE)
            set(_dc_cil "${CMAKE_CURRENT_BINARY_DIR}/e2e_device_call.cil")
            set(_dc_ptx "${CMAKE_CURRENT_BINARY_DIR}/e2e_device_call.ptx")
            add_test(NAME golden_frontend_device_call
                COMMAND coolfe++ --cpfe "${HACC_DEFAULT_CPFE}"
                        --edg-base-dir "${HACC_DEFAULT_EDG_BASE}"
                        -o "${_dc_cil}"
                        -I "${HACC_COOLRUNTIME_INCLUDE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/device_call.cu")
            add_test(NAME golden_device_il_to_ptx_device_call
                COMMAND coolicc --from-il -arch=sm_80 -o "${_dc_ptx}" "${_dc_cil}")
            set_tests_properties(golden_device_il_to_ptx_device_call PROPERTIES
                DEPENDS golden_frontend_device_call)
            # 有返回值的设备函数是独立 .func（不是内联进 entry）。
            add_test(NAME golden_device_il_to_ptx_device_call_func
                COMMAND grep -qE "^\\.visible \\.func.* add2\\(" "${_dc_ptx}")
            # 前向引用：forward_use 的定义（带括号）必须在，且 user 能 call 到它。
            add_test(NAME golden_device_il_to_ptx_device_call_forward
                COMMAND grep -qE "^\\.visible \\.func.* forward_use\\(" "${_dc_ptx}")
            add_test(NAME golden_device_il_to_ptx_device_call_callseq
                COMMAND grep -qE "call\\.uni \\(retval0\\)" "${_dc_ptx}")
            # void 设备函数的指针参数走 .param .b64（指针宽度）。
            add_test(NAME golden_device_il_to_ptx_device_call_void_ptr
                COMMAND grep -qE "\\.param \\.b64 setv_param_0" "${_dc_ptx}")
            set_tests_properties(golden_device_il_to_ptx_device_call_func
                                 golden_device_il_to_ptx_device_call_forward
                                 golden_device_il_to_ptx_device_call_callseq
                                 golden_device_il_to_ptx_device_call_void_ptr PROPERTIES
                DEPENDS golden_device_il_to_ptx_device_call)
        endif()

        # M5.4 e2e：__device__ 调用链 + 指针参数真机校验。
        set(_e2e_dc_exe "${CMAKE_CURRENT_BINARY_DIR}/e2e_device_call_run")
        add_test(NAME e2e_hacc_device_call_run_build
            COMMAND hacc "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/device_call_runtime.cu"
                    -o "${_e2e_dc_exe}")
        add_test(NAME e2e_hacc_device_call_run
            COMMAND "${_e2e_dc_exe}")
        set_tests_properties(e2e_hacc_device_call_run PROPERTIES
            DEPENDS e2e_hacc_device_call_run_build
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "deviceCall OK")

        # 阶段 2（C11 前半）：短路求值 && / || + 指针复合赋值 p += i / p -= i。
        # e2e 的断言同时看**副作用计数**（c1/c2 必须为 0）：只断言结果值会漏掉
        # 「退化成按位 and」这类错误 —— 那种写法算出来的值恰好也是对的。
        if(HACC_COOLRUNTIME_INCLUDE)
            set(_lp_cil "${CMAKE_CURRENT_BINARY_DIR}/e2e_logical_ptr.cil")
            set(_lp_ptx "${CMAKE_CURRENT_BINARY_DIR}/e2e_logical_ptr.ptx")
            add_test(NAME golden_frontend_logical_ptr
                COMMAND coolfe++ --cpfe "${HACC_DEFAULT_CPFE}"
                        --edg-base-dir "${HACC_DEFAULT_EDG_BASE}"
                        -o "${_lp_cil}"
                        -I "${HACC_COOLRUNTIME_INCLUDE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/logical_ptr.cu")
            add_test(NAME golden_device_il_to_ptx_logical_ptr
                COMMAND coolicc --from-il -arch=sm_80 -o "${_lp_ptx}" "${_lp_cil}")
            set_tests_properties(golden_device_il_to_ptx_logical_ptr PROPERTIES
                DEPENDS golden_frontend_logical_ptr)
            # 两个内核入口都在。
            add_test(NAME golden_device_il_to_ptx_logical_ptr_entries
                COMMAND grep -qE "\\.visible \\.entry k_short_circuit|\\.visible \\.entry k_ptr_compound"
                        "${_lp_ptx}")
            # 短路指纹：谓词化条件跳转（@%p bra）跳过右值块。若 && 被翻译成按位 and，
            # PTX 里只会出现 and.b32，没有这种谓词跳转。
            add_test(NAME golden_device_il_to_ptx_logical_ptr_shortcircuit
                COMMAND grep -qE "@%p[0-9]+ bra" "${_lp_ptx}")
            # 指针复合赋值走 GEP → 索引缩放（mul.wide）或地址相加（add.s64）。
            add_test(NAME golden_device_il_to_ptx_logical_ptr_gep
                COMMAND grep -qE "mul\\.wide|add\\.s64" "${_lp_ptx}")
            # 防退化：本文件没有任何 & 运算符，出现 and.b32 就说明逻辑运算符
            # 被当成了位运算符处理。
            add_test(NAME golden_device_il_to_ptx_logical_ptr_no_bitand
                COMMAND sh -c "! grep -qE 'and\\.b32' \"$1\"" _ "${_lp_ptx}")
            set_tests_properties(golden_device_il_to_ptx_logical_ptr_entries
                                 golden_device_il_to_ptx_logical_ptr_shortcircuit
                                 golden_device_il_to_ptx_logical_ptr_gep
                                 golden_device_il_to_ptx_logical_ptr_no_bitand PROPERTIES
                DEPENDS golden_device_il_to_ptx_logical_ptr)
        endif()

        # 阶段 2 e2e：短路副作用 + 指针复合赋值的真机校验。
        set(_e2e_lp_exe "${CMAKE_CURRENT_BINARY_DIR}/e2e_logical_ptr_run")
        add_test(NAME e2e_hacc_logical_ptr_run_build
            COMMAND hacc "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/logical_ptr.cu"
                    -o "${_e2e_lp_exe}")
        add_test(NAME e2e_hacc_logical_ptr_run
            COMMAND "${_e2e_lp_exe}")
        set_tests_properties(e2e_hacc_logical_ptr_run PROPERTIES
            DEPENDS e2e_hacc_logical_ptr_run_build
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "logicalPtr OK")

        # M4.3 golden：__threadfence{,_block,_system} → NVVM membar.{gl,cta,sys}。
        # 需 COOLRuntime 头（.cu 用 #include <cool/cool_runtime.h>）。
        if(HACC_COOLRUNTIME_INCLUDE)
            set(_tf_cil "${CMAKE_CURRENT_BINARY_DIR}/e2e_threadfence.cil")
            set(_tf_ptx "${CMAKE_CURRENT_BINARY_DIR}/e2e_threadfence.ptx")
            add_test(NAME golden_frontend_threadfence
                COMMAND coolfe++ --cpfe "${HACC_DEFAULT_CPFE}"
                        --edg-base-dir "${HACC_DEFAULT_EDG_BASE}"
                        -o "${_tf_cil}"
                        -I "${HACC_COOLRUNTIME_INCLUDE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/threadfence.cu")
            add_test(NAME golden_device_il_to_ptx_threadfence
                COMMAND coolicc --from-il -arch=sm_80 -o "${_tf_ptx}" "${_tf_cil}")
            set_tests_properties(golden_device_il_to_ptx_threadfence PROPERTIES
                DEPENDS golden_frontend_threadfence)
            # 三种栅栏必须各自落到对应作用域的 membar 指令（逐个断言，避免「命中一个即通过」）。
            foreach(_tf_scope gl cta sys)
                add_test(NAME golden_device_il_to_ptx_threadfence_membar_${_tf_scope}
                    COMMAND grep -qE "membar\\.${_tf_scope}" "${_tf_ptx}")
                set_tests_properties(golden_device_il_to_ptx_threadfence_membar_${_tf_scope}
                    PROPERTIES DEPENDS golden_device_il_to_ptx_threadfence)
            endforeach()
        endif()

        # M4.4 golden：原子内建 → LLVM atomicrmw / cmpxchg → atom.global.*。
        # 需 COOLRuntime 头（.cu 用 #include <cool/cool_runtime.h>）。
        if(HACC_COOLRUNTIME_INCLUDE)
            set(_at_cil "${CMAKE_CURRENT_BINARY_DIR}/e2e_atomics.cil")
            set(_at_ptx "${CMAKE_CURRENT_BINARY_DIR}/e2e_atomics.ptx")
            add_test(NAME golden_frontend_atomics
                COMMAND coolfe++ --cpfe "${HACC_DEFAULT_CPFE}"
                        --edg-base-dir "${HACC_DEFAULT_EDG_BASE}"
                        -o "${_at_cil}"
                        -I "${HACC_COOLRUNTIME_INCLUDE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/atomics.cu")
            add_test(NAME golden_device_il_to_ptx_atomics
                COMMAND coolicc --from-il -arch=sm_80 -o "${_at_ptx}" "${_at_cil}")
            set_tests_properties(golden_device_il_to_ptx_atomics PROPERTIES
                DEPENDS golden_frontend_atomics)
            # 每条原子指令单独断言，避免「命中一个即通过」。
            # 有符号/无符号 min/max 必须是不同的 PTX 类型，故 .s32 与 .u32 都查。
            foreach(_at_pat
                    "atom\\.global\\.add\\.u32"
                    "atom\\.global\\.add\\.f32"
                    "atom\\.global\\.exch\\.b32"
                    "atom\\.global\\.min\\.s32"
                    "atom\\.global\\.max\\.s32"
                    "atom\\.global\\.min\\.u32"
                    "atom\\.global\\.max\\.u32"
                    "atom\\.global\\.and\\.b32"
                    "atom\\.global\\.or\\.b32"
                    "atom\\.global\\.xor\\.b32"
                    "atom\\.global\\.cas\\.b32")
                # CMake 里 "\\" 求值为**单个**反斜杠；_at_pat 求值后是 atom\.global\.add\.u32，
                # 故此处只能剥一层。写成 "\\\\"（两个）会匹配不到、反斜杠留在测试名里，
                # 进而触发 CMake CMP0110「测试名含非法字符」警告刷屏。见下方 B9 段同类教训。
                string(REPLACE "\\" "" _at_name "${_at_pat}")
                string(REPLACE "." "_" _at_name "${_at_name}")
                add_test(NAME golden_device_il_to_ptx_atomics_${_at_name}
                    COMMAND grep -qE "${_at_pat}" "${_at_ptx}")
                set_tests_properties(golden_device_il_to_ptx_atomics_${_at_name}
                    PROPERTIES DEPENDS golden_device_il_to_ptx_atomics)
            endforeach()
        endif()

        # B9 golden：位运算符（& | ^ << >> ~）→ LLVM and/or/xor/shl/ashr/lshr → PTX。
        # 注意 PTX 断言里的 \bor\b：xor.b32 含子串 "or.b32"，不加词边界会误判
        # 「or 指令已生成」。~（eok_complement）不单独断言指令 —— LLVM 可能把它
        # 翻成 not.b32 也可能保留为 xor -1，形式不唯一，靠 e2e 的数值断言覆盖。
        if(HACC_COOLRUNTIME_INCLUDE)
            set(_bw_cil "${CMAKE_CURRENT_BINARY_DIR}/e2e_bitwise.cil")
            set(_bw_ptx "${CMAKE_CURRENT_BINARY_DIR}/e2e_bitwise.ptx")
            add_test(NAME golden_frontend_bitwise
                COMMAND coolfe++ --cpfe "${HACC_DEFAULT_CPFE}"
                        --edg-base-dir "${HACC_DEFAULT_EDG_BASE}"
                        -o "${_bw_cil}"
                        -I "${HACC_COOLRUNTIME_INCLUDE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/bitwise.cu")
            add_test(NAME golden_device_il_to_ptx_bitwise
                COMMAND coolicc --from-il -arch=sm_80 -o "${_bw_ptx}" "${_bw_cil}")
            set_tests_properties(golden_device_il_to_ptx_bitwise PROPERTIES
                DEPENDS golden_frontend_bitwise)
            # 逐条显式断言。不写成 foreach 是因为要从正则反推测试名，
            # 而正则里的 `\b` 会在 CTest 生成阶段触发 "Invalid character escape"。
            # `(^|[^x])or\.b32` 排除 xor.b32 的子串误匹配。
            add_test(NAME golden_device_il_to_ptx_bitwise_and
                COMMAND grep -qE "and\\.b32" "${_bw_ptx}")
            add_test(NAME golden_device_il_to_ptx_bitwise_or
                COMMAND grep -qE "(^|[^x])or\\.b32" "${_bw_ptx}")
            add_test(NAME golden_device_il_to_ptx_bitwise_xor
                COMMAND grep -qE "xor\\.b32" "${_bw_ptx}")
            add_test(NAME golden_device_il_to_ptx_bitwise_shl
                COMMAND grep -qE "shl\\.b32" "${_bw_ptx}")
            add_test(NAME golden_device_il_to_ptx_bitwise_ashr
                COMMAND grep -qE "shr\\.s32" "${_bw_ptx}")
            add_test(NAME golden_device_il_to_ptx_bitwise_lshr
                COMMAND grep -qE "shr\\.u32" "${_bw_ptx}")
            foreach(_bw_name and or xor shl ashr lshr)
                set_tests_properties(golden_device_il_to_ptx_bitwise_${_bw_name}
                    PROPERTIES DEPENDS golden_device_il_to_ptx_bitwise)
            endforeach()
        endif()

        # C12 / MMA golden：cool_wmma_* → llvm.nvvm.wmma.* → PTX wmma.*。
        # 需 COOLRuntime 头（.cu 用 <cool/cool_runtime.h> 提供 fragment 类型与内建声明）。
        # 两层校验（对齐 mma-support-plan.md §9.3）：
        #   1. PTX 指纹：5 条 wmma 指令逐条断言（用脚本，缺哪条就打印哪条）；
        #   2. ptxas 汇编：llc **不校验** intrinsic 名（拼错照样出 PTX），只有 ptxas 才报。
        if(HACC_COOLRUNTIME_INCLUDE)
            set(_wm_cil "${CMAKE_CURRENT_BINARY_DIR}/e2e_wmma.cil")
            set(_wm_ptx "${CMAKE_CURRENT_BINARY_DIR}/e2e_wmma.ptx")
            add_test(NAME golden_frontend_wmma
                COMMAND coolfe++ --cpfe "${HACC_DEFAULT_CPFE}"
                        --edg-base-dir "${HACC_DEFAULT_EDG_BASE}"
                        -o "${_wm_cil}"
                        -I "${HACC_COOLRUNTIME_INCLUDE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/wmma.cu")
            add_test(NAME golden_device_il_to_ptx_wmma
                COMMAND coolicc --from-il -arch=sm_80 -o "${_wm_ptx}" "${_wm_cil}")
            set_tests_properties(golden_device_il_to_ptx_wmma PROPERTIES
                DEPENDS golden_frontend_wmma)
            # 只断指令指纹，不断寄存器列表（随分配变化）。
            add_test(NAME golden_device_il_to_ptx_wmma_instr
                COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_ptx_patterns.sh" "${_wm_ptx}"
                        "wmma\\.load\\.a\\.sync\\.aligned\\.row\\.m16n16k16\\.f16"
                        "wmma\\.load\\.b\\.sync\\.aligned\\.col\\.m16n16k16\\.f16"
                        "wmma\\.load\\.c\\.sync\\.aligned\\.row\\.m16n16k16\\.f32"
                        "wmma\\.mma\\.sync\\.aligned\\.row\\.col\\.m16n16k16\\.f32\\.f32"
                        "wmma\\.store\\.d\\.sync\\.aligned\\.row\\.m16n16k16\\.f32")
            set_tests_properties(golden_device_il_to_ptx_wmma_instr PROPERTIES
                DEPENDS golden_device_il_to_ptx_wmma)
            # ptxas 汇编（机器上没装 CUDA toolkit 时跳过，不阻塞 CI）。
            find_program(HACC_PTXAS_EXECUTABLE ptxas)
            if(HACC_PTXAS_EXECUTABLE)
                add_test(NAME golden_device_ptx_wmma_ptxas
                    COMMAND "${HACC_PTXAS_EXECUTABLE}" -arch=sm_80
                            -o "${CMAKE_CURRENT_BINARY_DIR}/e2e_wmma.cubin" "${_wm_ptx}")
                set_tests_properties(golden_device_ptx_wmma_ptxas PROPERTIES
                    DEPENDS golden_device_il_to_ptx_wmma)
            endif()

            # C12 / MMA e2e（A4）：单 warp 的 D = A×B + C 与 CPU 参考比对。
            # 无可用设备时样例返回 77，按 SKIP 处理；缓冲由 coolMalloc 分配（256B 对齐 ≥ 32B）。
            set(_e2e_wmma_exe "${CMAKE_CURRENT_BINARY_DIR}/e2e_wmma_run")
            add_test(NAME e2e_hacc_wmma_run_build
                COMMAND hacc "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/wmma_e2e.cu"
                        -o "${_e2e_wmma_exe}")
            add_test(NAME e2e_hacc_wmma_run
                COMMAND "${_e2e_wmma_exe}")
            set_tests_properties(e2e_hacc_wmma_run PROPERTIES
                DEPENDS e2e_hacc_wmma_run_build
                SKIP_RETURN_CODE 77
                PASS_REGULAR_EXPRESSION "mma OK")

            # C4 跳过机制：把「没 GPU」与「有 GPU 但初始化失败（如 nvidia_uvm 未加载）」分开。
            # 有 /dev/nvidia0 却枚举不到设备 → 返回 1（FAIL），不再伪装成 SKIP。
            set(_gpu_state_exe "${CMAKE_CURRENT_BINARY_DIR}/e2e_gpu_state_run")
            add_test(NAME e2e_hacc_gpu_state_build
                COMMAND hacc "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/gpu_state.cu"
                        -o "${_gpu_state_exe}")
            add_test(NAME e2e_hacc_gpu_state
                COMMAND "${_gpu_state_exe}")
            set_tests_properties(e2e_hacc_gpu_state PROPERTIES
                DEPENDS e2e_hacc_gpu_state_build
                SKIP_RETURN_CODE 77
                PASS_REGULAR_EXPRESSION "gpuState OK")

            # 另两条分支的自测：本机有 GPU 时也要覆盖「无设备」与「初始化失败」，
            # 否则这两条分支等于没测过（真机故障不可按需制造）。
            # 用 PASS_REGULAR_EXPRESSION 判输出而非退出码（77 / 1 都是预期的「非零」）。
            add_test(NAME e2e_hacc_gpu_state_no_device
                COMMAND ${CMAKE_COMMAND} -E env HACC_GPU_STATE_FAULT=n "${_gpu_state_exe}")
            set_tests_properties(e2e_hacc_gpu_state_no_device PROPERTIES
                DEPENDS e2e_hacc_gpu_state_build
                PASS_REGULAR_EXPRESSION "gpuState WARNING")

            add_test(NAME e2e_hacc_gpu_state_init_fail
                COMMAND ${CMAKE_COMMAND} -E env HACC_GPU_STATE_FAULT=i "${_gpu_state_exe}")
            set_tests_properties(e2e_hacc_gpu_state_init_fail PROPERTIES
                DEPENDS e2e_hacc_gpu_state_build
                PASS_REGULAR_EXPRESSION "gpuState FAILED")
        endif()

        # M4.1 e2e：__device__ 设备全局编译 + 运行 + 结果校验（g_counter == 105）。
        # 无可用 CUDA 设备时样例返回 77，测试框架按 SKIP 处理。
        set(_e2e_dg_run_exe "${CMAKE_CURRENT_BINARY_DIR}/e2e_dev_global_run")
        add_test(NAME e2e_hacc_dev_global_run_build
            COMMAND hacc "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/dev_global.cu"
                    -o "${_e2e_dg_run_exe}")
        add_test(NAME e2e_hacc_dev_global_run
            COMMAND "${_e2e_dg_run_exe}")
        set_tests_properties(e2e_hacc_dev_global_run PROPERTIES
            DEPENDS e2e_hacc_dev_global_run_build
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "devGlobal OK")

        # M4.3 e2e：__threadfence* 编译 + 运行 + 结果校验（栅栏不改变单线程语义）。
        # 无可用 CUDA 设备时样例返回 77，测试框架按 SKIP 处理。
        set(_e2e_tf_run_exe "${CMAKE_CURRENT_BINARY_DIR}/e2e_threadfence_run")
        add_test(NAME e2e_hacc_threadfence_run_build
            COMMAND hacc "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/threadfence.cu"
                    -o "${_e2e_tf_run_exe}")
        add_test(NAME e2e_hacc_threadfence_run
            COMMAND "${_e2e_tf_run_exe}")
        set_tests_properties(e2e_hacc_threadfence_run PROPERTIES
            DEPENDS e2e_hacc_threadfence_run_build
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "threadfence OK")

        # M4.4 e2e：原子内建编译 + 运行 + 竞争正确性校验。
        # 128 线程并发打同一地址，断言累加值恰好等于线程数（非原子实现会丢更新）。
        # 无可用 CUDA 设备时样例返回 77，测试框架按 SKIP 处理。
        set(_e2e_at_run_exe "${CMAKE_CURRENT_BINARY_DIR}/e2e_atomics_run")
        add_test(NAME e2e_hacc_atomics_run_build
            COMMAND hacc "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/atomics.cu"
                    -o "${_e2e_at_run_exe}")
        add_test(NAME e2e_hacc_atomics_run
            COMMAND "${_e2e_at_run_exe}")
        set_tests_properties(e2e_hacc_atomics_run PROPERTIES
            DEPENDS e2e_hacc_atomics_run_build
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "atomics OK")

        # B9 e2e：位运算符编译 + 运行 + 数值校验。
        # 覆盖 4 种指令 + 有符号/无符号右移的区分（同一 bit 模式，两种结果）。
        # 无可用 CUDA 设备时样例返回 77，测试框架按 SKIP 处理。
        set(_e2e_bw_run_exe "${CMAKE_CURRENT_BINARY_DIR}/e2e_bitwise_run")
        add_test(NAME e2e_hacc_bitwise_run_build
            COMMAND hacc "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/bitwise.cu"
                    -o "${_e2e_bw_run_exe}")
        add_test(NAME e2e_hacc_bitwise_run
            COMMAND "${_e2e_bw_run_exe}")
        set_tests_properties(e2e_hacc_bitwise_run PROPERTIES
            DEPENDS e2e_hacc_bitwise_run_build
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "bitwise OK")

        # B9-2 golden：复合赋值（+= -= *= /= %= <<= >>= &= |= ^=）与逻辑非 !。
        # PTX 只断言最不容易被优化改写的四条：
        #   `b <<= 2` 可能被 LLVM 化成 `add b, b`，`a /= 4` / `a %= 4` 会被化成
        #   移位与掩码，硬断言 shl / div / rem 会误报。逻辑非同理（没有专门的 PTX
        #   指令，最终是 `setp` + `selp`），改由 e2e 的数值断言覆盖。
        if(HACC_COOLRUNTIME_INCLUDE)
            set(_ca_cil "${CMAKE_CURRENT_BINARY_DIR}/e2e_compound.cil")
            set(_ca_ptx "${CMAKE_CURRENT_BINARY_DIR}/e2e_compound.ptx")
            add_test(NAME golden_frontend_compound
                COMMAND coolfe++ --cpfe "${HACC_DEFAULT_CPFE}"
                        --edg-base-dir "${HACC_DEFAULT_EDG_BASE}"
                        -o "${_ca_cil}"
                        -I "${HACC_COOLRUNTIME_INCLUDE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/compound_assign.cu")
            add_test(NAME golden_device_il_to_ptx_compound
                COMMAND coolicc --from-il -arch=sm_80 -o "${_ca_ptx}" "${_ca_cil}")
            set_tests_properties(golden_device_il_to_ptx_compound PROPERTIES
                DEPENDS golden_frontend_compound)
            add_test(NAME golden_device_il_to_ptx_compound_and
                COMMAND grep -qE "and\\.b32" "${_ca_ptx}")
            add_test(NAME golden_device_il_to_ptx_compound_or
                COMMAND grep -qE "(^|[^x])or\\.b32" "${_ca_ptx}")
            add_test(NAME golden_device_il_to_ptx_compound_xor
                COMMAND grep -qE "xor\\.b32" "${_ca_ptx}")
            add_test(NAME golden_device_il_to_ptx_compound_ashr
                COMMAND grep -qE "shr\\.s32" "${_ca_ptx}")
            foreach(_ca_name and or xor ashr)
                set_tests_properties(golden_device_il_to_ptx_compound_${_ca_name}
                    PROPERTIES DEPENDS golden_device_il_to_ptx_compound)
            endforeach()
        endif()

        # B9-2 e2e：复合赋值 + 逻辑非编译 + 运行 + 数值校验（21 项）。
        # 无可用 CUDA 设备时样例返回 77，测试框架按 SKIP 处理。
        set(_e2e_ca_run_exe "${CMAKE_CURRENT_BINARY_DIR}/e2e_compound_run")
        add_test(NAME e2e_hacc_compound_run_build
            COMMAND hacc "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/compound_assign.cu"
                    -o "${_e2e_ca_run_exe}")
        add_test(NAME e2e_hacc_compound_run
            COMMAND "${_e2e_ca_run_exe}")
        set_tests_properties(e2e_hacc_compound_run PROPERTIES
            DEPENDS e2e_hacc_compound_run_build
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "compound OK")

        # KI-1 / ADR 0007 e2e：设备全局**跨 kernel 共享**（模块粒度 = 编译单元）。
        # 修复前必然失败：每个 kernel 各载入一份 module，.global 各一份副本，
        # 后一个 kernel 读到模块初值。无可用 CUDA 设备时返回 77，按 SKIP 处理。
        set(_e2e_dgs_run_exe "${CMAKE_CURRENT_BINARY_DIR}/e2e_dev_global_shared_run")
        add_test(NAME e2e_hacc_dev_global_shared_run_build
            COMMAND hacc "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/dev_global_shared.cu"
                    -o "${_e2e_dgs_run_exe}")
        add_test(NAME e2e_hacc_dev_global_shared_run
            COMMAND "${_e2e_dgs_run_exe}")
        set_tests_properties(e2e_hacc_dev_global_shared_run PROPERTIES
            DEPENDS e2e_hacc_dev_global_shared_run_build
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "devGlobalShared OK")

        # `(void)expr`（丢弃值转换）：回归用例。曾生成非法 IR（`... to void`）导致编译失败。
        # 断言同时覆盖「值不被破坏」与「副作用不丢失」（(void)atomicAdd 必须真的执行）。
        set(_e2e_vc_run_exe "${CMAKE_CURRENT_BINARY_DIR}/e2e_void_cast_run")
        add_test(NAME e2e_hacc_void_cast_run_build
            COMMAND hacc "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/void_cast.cu"
                    -o "${_e2e_vc_run_exe}")
        add_test(NAME e2e_hacc_void_cast_run
            COMMAND "${_e2e_vc_run_exe}")
        set_tests_properties(e2e_hacc_void_cast_run PROPERTIES
            DEPENDS e2e_hacc_void_cast_run_build
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "voidCast OK")

        # M4.2 e2e：__restrict__ 指针形参编译 + 运行 + 结果校验（语义不变）。
        # 无可用 CUDA 设备时样例返回 77，测试框架按 SKIP 处理。
        set(_e2e_rs_run_exe "${CMAKE_CURRENT_BINARY_DIR}/e2e_restrict_param_run")
        add_test(NAME e2e_hacc_restrict_param_run_build
            COMMAND hacc "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/restrict_param.cu"
                    -o "${_e2e_rs_run_exe}")
        add_test(NAME e2e_hacc_restrict_param_run
            COMMAND "${_e2e_rs_run_exe}")
        set_tests_properties(e2e_hacc_restrict_param_run PROPERTIES
            DEPENDS e2e_hacc_restrict_param_run_build
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "restrict OK")

        # 内建变量 + __syncthreads 的端到端（需 COOLRuntime 头）。
        if(HACC_COOLRUNTIME_INCLUDE)
            set(_e2e_tk_ptx "${CMAKE_CURRENT_BINARY_DIR}/e2e_tid_kernel.ptx")
            add_test(NAME e2e_hacc_tid_kernel
                COMMAND hacc "${CMAKE_CURRENT_SOURCE_DIR}/golden/frontend/tid_kernel.cu"
                        -I "${HACC_COOLRUNTIME_INCLUDE}"
                        --emit=ptx -o "${_e2e_tk_ptx}")
            add_test(NAME e2e_hacc_tid_kernel_ptx
                COMMAND grep -qE "\\.visible \\.entry tid_kernel|%tid\\.x|%ctaid\\.x|bar\\.sync"
                        "${_e2e_tk_ptx}")

            # M2.3 e2e：hacc --emit=exe 产可执行文件（含拼桩 + 主机编译链接 coolrt）。
            # 仅验证编译链接成功（不运行，避免依赖 GPU）；产物存在即通过。
            set(_e2e_va_exe "${CMAKE_CURRENT_BINARY_DIR}/e2e_vector_add_exe")
            add_test(NAME e2e_hacc_vector_add_exe
                COMMAND hacc "${CMAKE_CURRENT_SOURCE_DIR}/golden/host/vector_add.cu"
                        -o "${_e2e_va_exe}")
            add_test(NAME e2e_hacc_vector_add_exe_exists
                COMMAND test -x "${_e2e_va_exe}")
            set_tests_properties(e2e_hacc_vector_add_exe_exists PROPERTIES
                DEPENDS e2e_hacc_vector_add_exe)
            set_tests_properties(e2e_hacc_tid_kernel_ptx PROPERTIES
                DEPENDS e2e_hacc_tid_kernel)

            # B7 产物集与命名（--emit / -keep）：-keep 全保留、默认清理、失败保留并列出。
            set(_b7_keep "${CMAKE_CURRENT_BINARY_DIR}/b7_keep")
            set(_b7_clean "${CMAKE_CURRENT_BINARY_DIR}/b7_clean")
            set(_b7_fail "${CMAKE_CURRENT_BINARY_DIR}/b7_fail")
            set(_b7_ptx "${CMAKE_CURRENT_BINARY_DIR}/b7_ptx")
            file(REMOVE_RECURSE "${_b7_keep}" "${_b7_clean}" "${_b7_fail}" "${_b7_ptx}")
            file(MAKE_DIRECTORY "${_b7_keep}" "${_b7_clean}" "${_b7_fail}" "${_b7_ptx}")
            set(_b7_src "${CMAKE_CURRENT_SOURCE_DIR}/golden/host/vector_add.cu")
            # 期望的中间产物，与 pipeline.hpp 的命名表一致。
            set(_b7_files
                "vector_add.hacc.il" "vector_add.hacc.cpp" "vector_add.hacc.stub.cpp"
                "vector_add.hacc.gen.cpp" "vector_add.ptx")
            string(REPLACE ";" " " _b7_files_sp "${_b7_files}")

            # 1) -keep：5 个中间产物全部保留。
            add_test(NAME cli_keep_build
                COMMAND hacc "${_b7_src}" -o "${_b7_keep}/keep_exe"
                        -keep --keep-dir "${_b7_keep}")
            add_test(NAME cli_keep_intermediates
                COMMAND sh -c "for f in ${_b7_files_sp}; do \
                    test -f '${_b7_keep}'/$f || { echo \"缺少 $f\"; exit 1; }; done")
            set_tests_properties(cli_keep_intermediates PROPERTIES DEPENDS cli_keep_build)

            # 2) 默认：成功即清理，目录里只剩可执行文件。
            add_test(NAME cli_default_clean_build
                COMMAND hacc "${_b7_src}" -o "${_b7_clean}/clean_exe" --keep-dir "${_b7_clean}")
            add_test(NAME cli_default_clean_assert
                COMMAND sh -c "for f in ${_b7_files_sp}; do \
                    test ! -f '${_b7_clean}'/$f || { echo \"未清理 $f\"; exit 1; }; done")
            set_tests_properties(cli_default_clean_assert PROPERTIES DEPENDS cli_default_clean_build)

            # 3) 失败：中间产物保留，stderr 列出清单（便于定位断在哪阶段）。
            # 注意：WILL_FAIL 与 PASS_REGULAR_EXPRESSION 会互相翻转，故退出码与消息分开验证。
            add_test(NAME cli_failure_keep_build
                COMMAND hacc "${_b7_src}" -o "${_b7_fail}/fail_exe"
                        --keep-dir "${_b7_fail}" --host-cxx=/bin/false)
            set_tests_properties(cli_failure_keep_build PROPERTIES WILL_FAIL TRUE)
            add_test(NAME cli_failure_keep_message
                COMMAND sh -c "\"$1\" \"$2\" -o \"$3\" --keep-dir \"$4\" --host-cxx=/bin/false \
                            2>&1 | grep -q '保留中间产物'"
                        _ $<TARGET_FILE:hacc> "${_b7_src}" "${_b7_fail}/fail_msg" "${_b7_fail}")
            add_test(NAME cli_failure_keep_assert
                COMMAND sh -c "for f in ${_b7_files_sp}; do \
                    test -f '${_b7_fail}'/$f || { echo \"缺少 $f\"; exit 1; }; done")
            set_tests_properties(cli_failure_keep_assert PROPERTIES DEPENDS cli_failure_keep_build)

            # 4) --emit=ptx + -keep：终产物落到 -o，IL 作为中间产物保留。
            add_test(NAME cli_emit_ptx_keep_build
                COMMAND hacc "${_b7_src}" --emit=ptx -keep
                        -o "${_b7_ptx}/emit.ptx" --keep-dir "${_b7_ptx}")
            add_test(NAME cli_emit_ptx_keep_assert
                COMMAND sh -c "test -f '${_b7_ptx}/emit.ptx' && \
                    test -f '${_b7_ptx}/vector_add.hacc.il'")
            set_tests_properties(cli_emit_ptx_keep_assert PROPERTIES DEPENDS cli_emit_ptx_keep_build)

            # 5) 未知 --emit：明确报错，不静默忽略（退出码与消息分开验证）。
            add_test(NAME cli_emit_unknown_exit
                COMMAND hacc "${_b7_src}" --emit=xyz -o "${_b7_clean}/never")
            set_tests_properties(cli_emit_unknown_exit PROPERTIES WILL_FAIL TRUE)
            add_test(NAME cli_emit_unknown_message
                COMMAND sh -c "\"$1\" \"$2\" --emit=xyz -o \"$3\" 2>&1 | grep -q '未知的 --emit'"
                        _ $<TARGET_FILE:hacc> "${_b7_src}" "${_b7_clean}/never2")

            # B5：dim3 的 CUDA 风格构造（dim3(x) / (x,y) / (x,y,z)，默认 (1,1,1)）端到端。
            # build 阶段即可验证「构造式可编译」；run 阶段无 GPU 时返回 77 → SKIP。
            set(_e2e_d3_exe "${CMAKE_CURRENT_BINARY_DIR}/e2e_dim3_ctor")
            add_test(NAME e2e_hacc_dim3_ctor_build
                COMMAND hacc "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/dim3_ctor.cu"
                        -o "${_e2e_d3_exe}")
            add_test(NAME e2e_hacc_dim3_ctor_run
                COMMAND "${_e2e_d3_exe}")
            set_tests_properties(e2e_hacc_dim3_ctor_run PROPERTIES
                DEPENDS e2e_hacc_dim3_ctor_build
                SKIP_RETURN_CODE 77
                PASS_REGULAR_EXPRESSION "dim3Ctor OK")
        endif()

        # B8 主机编译器选择（--host-cxx）。
        # 1) 指定不存在的编译器 → 主机编译阶段失败（fail loud，不静默回退 g++）。
        add_test(NAME e2e_hacc_host_cxx_missing
            COMMAND hacc "${CMAKE_CURRENT_SOURCE_DIR}/golden/host/vector_add.cu"
                    -o "${CMAKE_CURRENT_BINARY_DIR}/e2e_host_cxx_missing"
                    --host-cxx=/nonexistent/hacc-host-cxx)
        set_tests_properties(e2e_hacc_host_cxx_missing PROPERTIES WILL_FAIL TRUE)

        # 2) 显式空值 → 参数校验直接拒绝（不进入流水线）。
        add_test(NAME e2e_hacc_host_cxx_empty
            COMMAND hacc "${CMAKE_CURRENT_SOURCE_DIR}/golden/host/vector_add.cu"
                    -o "${CMAKE_CURRENT_BINARY_DIR}/e2e_host_cxx_empty" --host-cxx=)
        set_tests_properties(e2e_hacc_host_cxx_empty PROPERTIES WILL_FAIL TRUE)

        # 3) 真切到 clang++ 编一遍 + 运行（无 GPU 返回 77 → SKIP）。
        #    这条同时验证「前端系统头探测跟随主机编译器」：clang++ 的搜索列表与 g++ 不同。
        find_program(HACC_CLANGXX clang++)
        if(HACC_CLANGXX)
            set(_e2e_hcx_exe "${CMAKE_CURRENT_BINARY_DIR}/e2e_host_cxx_clang_run")
            add_test(NAME e2e_hacc_host_cxx_clang_build
                COMMAND hacc "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/vector_add.cu"
                        -o "${_e2e_hcx_exe}" --host-cxx=${HACC_CLANGXX})
            add_test(NAME e2e_hacc_host_cxx_clang_run
                COMMAND "${_e2e_hcx_exe}")
            set_tests_properties(e2e_hacc_host_cxx_clang_run PROPERTIES
                DEPENDS e2e_hacc_host_cxx_clang_build
                SKIP_RETURN_CODE 77
                PASS_REGULAR_EXPRESSION "vectorAdd OK")
        endif()
    endif()
endif()

# M1.1 golden：LLVM IR → PTX（仅当 coolicc 编译了 LLVM 后端时）。
# 校验最小内核产出 `.visible .entry empty_kernel`。
if(HACC_WITH_LLVM)
    add_test(NAME golden_device_empty_kernel
        COMMAND coolicc --emit=ptx -arch=sm_80
                "${CMAKE_CURRENT_SOURCE_DIR}/golden/device/empty_kernel.ll")
    set_tests_properties(golden_device_empty_kernel PROPERTIES
        PASS_REGULAR_EXPRESSION "\\.visible \\.entry empty_kernel")
endif()
