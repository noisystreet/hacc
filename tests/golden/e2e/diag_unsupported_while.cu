// M3.5 边界用例：while 循环（子集未覆盖）→ 应 fail loud 且带源位置。
//
// 期望：coolicc 翻译失败，stderr 含 "<...>:6: 不支持的语句 kind"（第 6 行是 while）。
#include <cool/cool_runtime.h>

__global__ void unsupported_while(int* out) {
    int i = 0;
    while (i < 4) {
        i = i + 1;
    }
    *out = i;
}
