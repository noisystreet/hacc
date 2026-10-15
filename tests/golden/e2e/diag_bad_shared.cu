// M3.4 golden 输入：文件作用域 __shared__（CUDA 不允许），验证诊断带源位置。
//
// 期望：coolicc 翻译失败，stderr 含 "<...>:5: __shared__ 变量不能声明在文件作用域..."。
#include <cool/cool_runtime.h>

__shared__ int g_bad[16];

__global__ void k(int* out) {
    *out = g_bad[0];
}
