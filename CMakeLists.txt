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

# 公共支持层：诊断 / 选项 / 版本 / 进程封装（src/support）。
add_library(hacc_support STATIC
    src/support/version.cpp
)
target_include_directories(hacc_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(hacc_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(hacc_support PRIVATE HACC_VERSION_STRING="${PROJECT_VERSION}")

# hacc：命令行驱动（对标 nvcc）—— 选项解析 / 阶段编排 / 子进程调度。
add_executable(hacc src/driver/main.cpp)
target_link_libraries(hacc PRIVATE hacc_support)

# coolfe++：CUDA 前端（对标 cudafe++，基于 EDG C++，独立可执行）。
add_executable(coolfe++ src/frontend/main.cpp)
target_link_libraries(coolfe++ PRIVATE hacc_support)

# coolicc：设备端编译器（对标 cicc，LLVM/NVPTX，独立可执行）。
add_executable(coolicc src/device/main.cpp)
target_link_libraries(coolicc PRIVATE hacc_support)

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
else()
    target_sources(coolicc PRIVATE src/device/nvptx_backend_stub.cpp)
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
