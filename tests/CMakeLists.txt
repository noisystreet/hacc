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
        add_test(NAME golden_frontend_tid_kernel
            COMMAND coolfe++ --cpfe "${HACC_DEFAULT_CPFE}"
                    --edg-base-dir "${HACC_DEFAULT_EDG_BASE}"
                    -o "${_tk_cil}"
                    -I "${HACC_COOLRUNTIME_INCLUDE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/golden/frontend/tid_kernel.cu")
        add_test(NAME golden_device_il_to_ptx_tid_kernel
            COMMAND coolicc --from-il -arch=sm_80 "${_tk_cil}")
        set_tests_properties(golden_device_il_to_ptx_tid_kernel PROPERTIES
            PASS_REGULAR_EXPRESSION "\\.visible \\.entry tid_kernel"
            DEPENDS golden_frontend_tid_kernel)
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
