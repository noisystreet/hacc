#include "support/version.hpp"

// 由 CMake 注入（见 CMakeLists.txt 的 HACC_VERSION_STRING）。
#ifndef HACC_VERSION_STRING
#define HACC_VERSION_STRING "0.0.0"
#endif

namespace hacc {

const char* version() noexcept {
    return HACC_VERSION_STRING;
}

}  // namespace hacc
