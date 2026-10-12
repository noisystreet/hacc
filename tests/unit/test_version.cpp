// 单元测试：版本接口的最小冒烟（M0 骨架）。
#include <cstring>

#include "support/version.hpp"

int main() {
    const char* v = hacc::version();
    if (v == nullptr || std::strlen(v) == 0) {
        return 1;
    }
    return 0;
}
