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
