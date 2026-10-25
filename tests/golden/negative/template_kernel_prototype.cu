// S2 T1 负向：模板 kernel 的**原型**（没有任何具体类型的启动）。
//
// 原型没有实例化上下文，签名里的 T 无处落地。曾经（探针阶段）它会静默产出一个只有
// `ret` 的空 `.entry` —— 按名调用等于静默空实现。现在的行为是：
//   1. stderr 提示「模板 kt 在本 TU 内未被实例化（无具体类型的调用），已跳过」；
//   2. 不发射 `.entry kt`；
//   3. 若整个 TU 没有别的内核，则以「IL 中未找到内核」失败（本文件即此情形）。
//
// 期望：编译失败 + 上述提示（由 tests/CMakeLists.txt 的
// device_template_kernel_prototype_message 断言）。
#include <cool/cool_runtime.h>

template <typename T>
__global__ void kt(T* out) {
    out[threadIdx.x] = (T)0;
}
