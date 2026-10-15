// M3.4 golden 输入：触发不支持的运算符，验证诊断消息带源位置（文件:行）。
//
// 内核：void bad_op(int* out) { int x=1; int y=2; *out = x ? y : 0; }
// 期望：coolicc 翻译失败，stderr 含 "<...>:9: 不支持的运算符..."（第 9 行是三元表达式）。
#include <cool/cool_runtime.h>

__global__ void bad_op(int* out) {
    int x = 1;
    int y = 2;
    *out = x ? y : 0;
}
