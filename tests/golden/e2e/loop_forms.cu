// M5.1 golden：循环子集（while / for / do-while）→ PTX 的循环结构。
//
// 三个内核分别覆盖 stmk_while / stmk_for / stmk_end_test_while 的降级路径；
// 断言见 tests/CMakeLists.txt（内核入口 + 回跳 br + setp 比较）。
// 三者都带 blockDim 步进或上界比较，确保「条件在循环头、回跳在尾」成立。
#include <cool/cool_runtime.h>

__global__ void loop_while(int* out, int n) {
    int i = threadIdx.x;
    while (i < n) {
        out[i] = i;
        i = i + blockDim.x;
    }
}

__global__ void loop_for(int* out, int n) {
    for (int i = threadIdx.x; i < n; i = i + blockDim.x) {
        out[i] = i * 2;
    }
}

__global__ void loop_do(int* out, int n) {
    int i = 0;
    do {
        out[i] = i;
        i = i + 1;
    } while (i < n);
}
