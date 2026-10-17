// M1.2.0 golden 输入：最小 CUDA 内核（无参）。
// 期望：coolfe++ 产出二进制 IL（.cil）；文本 IL（.il.txt）需 `--emit-il-text` 显式请求
//       （本测试在 CMake 里显式传了 --emit-il-text=<路径>），文本中含内核名与 cool_global 属性。
__attribute__((cool_global)) void empty_kernel() {}
