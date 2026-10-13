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
