// 多 TU 用例（C3）之 A：只定义设备内核 + 一个普通主机函数对外暴露。
//
// 为什么写成「普通主机函数」而不是让 B 直接 `<<<>>>` 调 k_a：设备映像按 TU 生成，
// B 的 PTX 里没有 k_a，跨 TU 启动内核需要链接期合并设备映像（RDC 级），一期不做
// （ADR 0007 的限制）。所以多 TU 的正确用法就是：设备代码按 TU 分组，主机侧用
// 普通 C++ 函数互相调用。
#include <cool/cool_runtime.h>

__global__ void k_a(int* out) { out[0] = 11; }

// 导出给 B（multi_tu_b.cu）调用。
void run_a(int* d) { k_a<<<1, 1>>>(d); }
