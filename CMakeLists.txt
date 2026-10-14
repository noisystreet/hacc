# hacc —— COOL 的 CUDA C++ 子集编译器（对标 nvcc）。
#
# 构建三个可执行：hacc（驱动）/ coolfe++（CUDA 前端，对标 cudafe++）/
# coolicc（设备端编译器，对标 cicc）。见 docs/compiler-implementation-plan.md §4.1。
#
# EDG 与 LLVM 默认关闭：骨架可在无外部依赖的环境下 build/test；
# M0 接入时再按需打开（HACC_WITH_EDG / HACC_WITH_LLVM，见 ADR 0005）。
cmake_minimum_required(VERSION 3.15)
project(hacc LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(HACC_WITH_EDG "Fetch/build the EDG C++ front end (used by coolfe++)" OFF)
option(HACC_WITH_LLVM "Link the LLVM/NVPTX backend (used by coolicc)" OFF)

# COOLRuntime 头目录（设备端内建 device_builtins.h 等，供 .cu 源码 #include <cool/...>）。
# 默认取同级目录 ../coolruntime；可用 -DHACC_COOLRUNTIME_DIR=<path> 覆盖。
# 仅用于测试（golden .cu 的 -I）；未找到时相关测试跳过，不影响构建。
set(HACC_COOLRUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../coolruntime"
    CACHE PATH "COOLRuntime 仓库根目录（含 include/cool/）")
if(EXISTS "${HACC_COOLRUNTIME_DIR}/include/cool/cool_runtime.h")
    set(HACC_COOLRUNTIME_INCLUDE "${HACC_COOLRUNTIME_DIR}/include")
    message(STATUS "hacc: COOLRuntime 头目录 = ${HACC_COOLRUNTIME_INCLUDE}")
else()
    set(HACC_COOLRUNTIME_INCLUDE "")
    message(STATUS "hacc: 未找到 COOLRuntime 头（${HACC_COOLRUNTIME_DIR}/include/cool/cool_runtime.h）；"
                   "依赖它的 golden 测试将跳过。可用 -DHACC_COOLRUNTIME_DIR=<path> 指定。")
endif()

# 公共支持层：诊断 / 选项 / 版本 / 进程封装（src/support）。
add_library(hacc_support STATIC
    src/support/version.cpp
    src/support/process.cpp
    src/host/launch_rewrite.cpp
    src/host/stub_gen.cpp
)
target_include_directories(hacc_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(hacc_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(hacc_support PRIVATE HACC_VERSION_STRING="${PROJECT_VERSION}")

# hacc：命令行驱动（对标 nvcc）—— 选项解析 / 阶段编排 / 子进程调度。
add_executable(hacc src/driver/main.cpp src/driver/pipeline.cpp)
target_link_libraries(hacc PRIVATE hacc_support)

# coolfe++：CUDA 前端（对标 cudafe++，基于 EDG C++，独立可执行）。
add_executable(coolfe++ src/frontend/main.cpp src/frontend/edg_invoke.cpp)
target_link_libraries(coolfe++ PRIVATE hacc_support)
# 注入 hacc 自身 include 目录（含 coolcc/hacc_device_decls.h），供 device 源 #include。
target_compile_definitions(coolfe++ PRIVATE
    HACC_INCLUDE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/include")

# HACC_WITH_EDG：把 EDG 前端（cpfe）的默认路径注入 coolfe++（M1.2.0）。
# 关闭时 coolfe++ 仍可构建；运行期需 --cpfe / $HACC_CPFE 指定 cpfe。
# 路径可用 HACC_EDG_DIR 覆盖（默认 build/il_spike/edg，见 tools/il_spike/fetch_edg.sh）。
if(HACC_WITH_EDG)
    set(HACC_EDG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/il_spike/edg"
        CACHE PATH "EDG 源码/构建根目录（含 build/gcc/bin/cpfe 与 bases/）")
    set(HACC_DEFAULT_CPFE "${HACC_EDG_DIR}/build/gcc/bin/cpfe")
    set(HACC_DEFAULT_EDG_BASE "${HACC_EDG_DIR}/bases/docker/dev-env/gcc")
    target_compile_definitions(coolfe++ PRIVATE
        HACC_DEFAULT_CPFE="${HACC_DEFAULT_CPFE}"
        HACC_DEFAULT_EDG_BASE="${HACC_DEFAULT_EDG_BASE}")
    message(STATUS "hacc: coolfe++ 默认 cpfe = ${HACC_DEFAULT_CPFE}")
endif()

# coolicc：设备端编译器（对标 cicc，LLVM/NVPTX，独立可执行）。
add_executable(coolicc src/device/main.cpp)
target_link_libraries(coolicc PRIVATE hacc_support)

# HACC_WITH_EDG：coolicc 链接 EDG 前端库，用 il_read 读回二进制 IL（M1.2.1）。
# 关闭时走 stub（il_read_backend_stub.cpp），--emit=il-list 明确报错。
#
# 链接对象集：EDG 的 cdisp 目标（src/cmake/cdisp）——它是 EDG 专为「读二进制 IL +
# 显示」构建的独立工具，对象集恰好是 il_read + standalone_utility_*_init 所需的最小集，
# 且只有 il_display.c.o 含 main（必须排除，否则与 coolicc 的 main 冲突）。
# 注意：EDG 对象**不再**是 ASan 构建（fetch_edg.sh 已传 -DEDG_CPFE_USE_ASAN=OFF），
# 故 coolicc 无需 -fsanitize=address，可产出干净的发布二进制。
if(HACC_WITH_EDG)
    # EDG 的 cdisp 对象目录（由 tools/il_spike/fetch_edg.sh 构建）。
    set(HACC_EDG_CDISP_OBJDIR
        "${HACC_EDG_DIR}/build/gcc/src/cmake/cdisp/CMakeFiles/cdisp.dir/__/__")
    file(GLOB HACC_EDG_CDISP_OBJS "${HACC_EDG_CDISP_OBJDIR}/*.o")
    # 排除 il_display.c.o（含 main，与 coolicc 冲突）。
    list(FILTER HACC_EDG_CDISP_OBJS EXCLUDE REGEX "il_display\\.c\\.o$")
    if(NOT HACC_EDG_CDISP_OBJS)
        message(FATAL_ERROR
            "hacc: 未找到 EDG cdisp 对象（${HACC_EDG_CDISP_OBJDIR}）。\n"
            "请先跑 tools/il_spike/fetch_edg.sh 构建 EDG。")
    endif()
    list(LENGTH HACC_EDG_CDISP_OBJS _hacc_edg_obj_count)
    message(STATUS "hacc: coolicc 链接 EDG cdisp 对象 ${_hacc_edg_obj_count} 个")

    target_sources(coolicc PRIVATE src/device/il_read_backend.cpp)
    target_compile_definitions(coolicc PRIVATE HACC_WITH_EDG=1)
    # EDG 头文件目录：src（源码头）+ build/gcc/src/includes（生成的 cmake_defines.h 等）。
    target_include_directories(coolicc SYSTEM PRIVATE
        "${HACC_EDG_DIR}/src"
        "${HACC_EDG_DIR}/build/gcc/src/includes")
    # 与 EDG 构建一致的宏（见 build.ninja 的 DEFINES/FLAGS）。
    target_compile_definitions(coolicc PRIVATE
        RECORD_UNRECOGNIZED_ATTRIBUTES=1
        USE_CMAKE_DEFINES
        BACK_END_IS_C_GEN_BE=1
        COMPILE_MULTIPLE_TRANSLATION_UNITS=1
        DO_IL_LOWERING=1
        LOWER_FIXED_POINT=1
        LOWER_IFUNC=1
        RECORD_BACKING_EXPRS_WITH_IL_LOWERING=1
        SUN_EXTENSIONS_ALLOWED=1
        STANDALONE_IL_DISPLAY=1)
    target_link_libraries(coolicc PRIVATE ${HACC_EDG_CDISP_OBJS})
else()
    target_sources(coolicc PRIVATE src/device/il_read_backend_stub.cpp)
endif()

# HACC_WITH_LLVM：接入 LLVM/NVPTX 后端（C1：系统 LLVM，锁 19.x）。
# 关闭时 coolicc 仍可构建（--version/-h），--emit=ptx 走占位实现并明确报错。
if(HACC_WITH_LLVM)
    # C1：系统 LLVM，锁 19.x。LLVM 的 Config 对 "19" 做精确匹配会拒绝 "19.1.7"，
    # 故用 "19.1" 下限匹配，再显式校验主版本。
    find_package(LLVM 19.1 REQUIRED CONFIG)
    if(NOT LLVM_PACKAGE_VERSION MATCHES "^19\\.")
        message(FATAL_ERROR "hacc: 需要 LLVM 19.x，实际 ${LLVM_PACKAGE_VERSION}")
    endif()
    message(STATUS "hacc: using LLVM ${LLVM_PACKAGE_VERSION} (${LLVM_DIR})")

    # 要求 NVPTX 目标（设备后端必需，见 C1）。
    if(NOT "NVPTX" IN_LIST LLVM_TARGETS_TO_BUILD)
        message(FATAL_ERROR "hacc: LLVM 未构建 NVPTX 目标（LLVM_TARGETS_TO_BUILD=${LLVM_TARGETS_TO_BUILD}）")
    endif()

    target_sources(coolicc PRIVATE src/device/nvptx_backend.cpp)
    target_compile_definitions(coolicc PRIVATE HACC_WITH_LLVM=1)
    target_include_directories(coolicc SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
    target_compile_definitions(coolicc PRIVATE ${LLVM_DEFINITIONS})
    # Debian 的 LLVMConfig 不填 LLVM_LIBRARIES；用导入目标 LLVM（单体 dylib libLLVM-19.so）。
    if(TARGET LLVM)
        target_link_libraries(coolicc PRIVATE LLVM)
    else()
        target_link_libraries(coolicc PRIVATE ${LLVM_LIBRARIES})
    endif()

    # M1.2.2：IL → LLVM IR 翻译（需 EDG + LLVM 同时开启）。
    # il_to_llvm.cpp 同时 include EDG 头与 LLVM 头，故仅在两者都开时编译；
    # 否则走 stub（il_to_llvm_stub.cpp），--from-il 明确报错。
    if(HACC_WITH_EDG)
        target_sources(coolicc PRIVATE src/device/il_to_llvm.cpp)
    else()
        target_sources(coolicc PRIVATE src/device/il_to_llvm_stub.cpp)
    endif()
else()
    target_sources(coolicc PRIVATE src/device/nvptx_backend_stub.cpp)
    # 无 LLVM 时 IL → IR 翻译也不可用，走 stub。
    target_sources(coolicc PRIVATE src/device/il_to_llvm_stub.cpp)
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
