// M2.0 golden 输入：真实 .cu（含 __global__ 内核 + <<<>>> 启动）。
// 期望：改写器产出 tests/golden/host/vector_add.hacc.cpp（见同目录）。
#include <cool/cool_runtime.h>

__global__ void vector_add(int* out, int a, int b) {
    int c = a + b;
    *out = c;
}

int main() {
    int* d_out = nullptr;
    vector_add<<<1, 1>>>(d_out, 1, 2);
    return 0;
}
