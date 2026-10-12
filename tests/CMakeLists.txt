# 一期测试入口。目录划分：unit（纯逻辑）/ golden（.cu → 期望 PTX/主机桩）/ e2e（在 coolrt 上运行）。
#
# M0 骨架：先接 unit 与 CLI 冒烟；golden/e2e 待编译链路（coolfe++/coolicc）接入后再加，
# 且 e2e 需在无 GPU / 无 PTX JIT 环境可跳过（见方案 §11）。

add_executable(test_version unit/test_version.cpp)
target_link_libraries(test_version PRIVATE hacc_support)
add_test(NAME unit_version COMMAND test_version)

add_test(NAME cli_hacc_version    COMMAND hacc --version)
add_test(NAME cli_coolfe_version  COMMAND coolfe++ --version)
add_test(NAME cli_coolicc_version COMMAND coolicc --version)
set_tests_properties(cli_hacc_version    PROPERTIES PASS_REGULAR_EXPRESSION "hacc")
set_tests_properties(cli_coolfe_version  PROPERTIES PASS_REGULAR_EXPRESSION "coolfe")
set_tests_properties(cli_coolicc_version PROPERTIES PASS_REGULAR_EXPRESSION "coolicc")
