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

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
