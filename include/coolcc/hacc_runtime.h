/* hacc 运行时支持头（随编译器分发，被生成的主机代码 include）。
 *
 * 提供主机桩 / 内嵌映像 / 惰性注册的支持接口（对标 nvcc 的 crt/host_runtime.h）。
 * 一期范围与接口见 docs/compiler-implementation-plan.md §6.2、ADR 0003。
 *
 * M0 骨架：占位。KernelRegistration / hacc::resolve() 等接口待 M2 落定。
 */
#ifndef HACC_RUNTIME_H
#define HACC_RUNTIME_H

/* TODO(M2): 定义 KernelRegistration 与 hacc::resolve()——惰性执行
 *           coolModuleLoadData + coolModuleGetFunction，供 §6.2 生成的主机桩调用。 */

#endif /* HACC_RUNTIME_H */
