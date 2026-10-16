// M3.5 边界用例：switch 语句（子集未覆盖）→ 应 fail loud 且带源位置。
//
// 期望：coolicc 翻译失败，stderr 含 "<...>:6: 不支持的语句 kind"（第 6 行是 switch）。
#include <cool/cool_runtime.h>

__global__ void unsupported_switch(int* out) {
    int x = 1;
    switch (x) {
        case 1:
            *out = 10;
            break;
        default:
            *out = 0;
            break;
    }
}
