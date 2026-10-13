// M1.2.0 golden 输入：最小 CUDA 内核（无参）。
// 期望：coolfe++ 产出二进制 IL（.cil）+ 人类可读文本（.il.txt），
//       文本中含内核名与 cool_global 属性。
__attribute__((cool_global)) void empty_kernel() {}
