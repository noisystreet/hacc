// M3.5 边界用例：类型不匹配（指针赋给整型）→ 前端应报错（fail loud）。
//
// 期望：coolfe++（cpfe）编译失败，stderr 含 "line 8: error:"（第 8 行是 int x = p;）。
#include <cool/cool_runtime.h>

__global__ void type_mismatch(int* out) {
    int* p = out;
    int x = p;
    *out = x;
}
