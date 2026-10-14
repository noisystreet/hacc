// coolfe++ 注入的 device 源前置声明（M2.1）。
//
// 背景：`<<<>>>` 改写后，device 源里出现 `haccLaunch(__cool_reg_<kernel>, ...)`。
// cpfe（EDG C++）需要这些标识符的**声明**才能解析；本头提供最小声明集，
// 由 coolfe++ 在喂给 cpfe 的 device 源顶部注入（见 src/frontend/main.cpp）。
//
// 注意：这里只声明、不定义——device 源只用于产设备 IL，不参与链接。
// 真正的定义在 host 侧（M2.2 的 hacc_runtime.h）。
#ifndef HACC_COOLCC_DEVICE_DECLS_H
#define HACC_COOLCC_DEVICE_DECLS_H

namespace hacc {
namespace device {

// 内核注册描述符（device 侧仅需**完整**类型即可按值传参；host 侧定义真实内容）。
struct KernelRegistration {};

// 启动内核：`<<<grid, block, smem, stream>>>` 改写后的目标。
// device 源只需**解析**（不链接），故用 C 风格可变参数声明，接受任意实参组合
// （避免模板推导失败；EDG 对全 pack 模板的推导不可靠）。
void haccLaunch(...);

}  // namespace device
}  // namespace hacc

// 把 haccLaunch 引入全局命名空间（改写文本直接写 `haccLaunch(...)`）。
using hacc::device::haccLaunch;

// 声明一个内核的注册描述符符号（`foo` → `__cool_reg_foo`）。
// 由 coolfe++ 按改写器收集到的内核名逐个展开。
#define __COOL_REG_DECL(name) extern ::hacc::device::KernelRegistration __cool_reg_##name

#endif  // HACC_COOLCC_DEVICE_DECLS_H
