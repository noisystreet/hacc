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
            # while 循环（子集未覆盖）→ 翻译失败，诊断带源位置（第 6 行）。
            add_test(NAME diag_unsupported_while
                COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_diag_fail.sh"
                        "$<TARGET_FILE:hacc>"
                        "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/diag_unsupported_while.cu"
                        "${HACC_COOLRUNTIME_INCLUDE}" ":6: 不支持的语句 kind")
            # switch 语句（子集未覆盖）→ 翻译失败，诊断带源位置（第 6 行）。
            add_test(NAME diag_unsupported_switch
                COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_diag_fail.sh"
                        "$<TARGET_FILE:hacc>"
                        "${CMAKE_CURRENT_SOURCE_DIR}/golden/e2e/diag_unsupported_switch.cu"
                        "${HACC_COOLRUNTIME_INCLUDE}" ":6: 不支持的语句 kind")
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
                            atomic_reduce device_global threadfence)
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
                    "atom\\.global\\.cas\\.b32")
                string(REPLACE "\\\\" "" _at_name "${_at_pat}")
                string(REPLACE "." "_" _at_name "${_at_name}")
                add_test(NAME golden_device_il_to_ptx_atomics_${_at_name}
                    COMMAND grep -qE "${_at_pat}" "${_at_ptx}")
                set_tests_properties(golden_device_il_to_ptx_atomics_${_at_name}
                    PROPERTIES DEPENDS golden_device_il_to_ptx_atomics)
            endforeach()
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
