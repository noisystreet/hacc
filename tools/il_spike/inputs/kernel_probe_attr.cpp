// IL spike 探针 3：把「属性」放在文件作用域变量上（其 IL 条目出现在输出前段），
// 观察 IL 的 attributes 字段是否承载属性。需 GNU C++ 模式：cpfe 加 --gpp。
extern "C" int __attribute__((aligned(64))) g_tile[64];

extern "C" void use_tile(float* y) {
    y[0] = static_cast<float>(g_tile[0]);
}
