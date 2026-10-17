// M2.0：主机源码改写器实现（见 launch_rewrite.hpp 与 docs/m2-host-path-plan.md §3.2 / §8.2）。
//
// 实现策略：单趟词法扫描，跳过注释与字符串/字符字面量，避免误改其中的 `<<<` 或 `__global__`。
// 两件事在同一趟内完成：
//   - 遇到 `<<<`：解析 `<<<...>>>` 配置 + 紧随的 `(...)` 实参，改写为 `haccLaunch(...)`；
//   - 遇到 `__global__`：定位其函数体 `{...}` 并剔除（只留声明）。
#include "host/launch_rewrite.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace hacc::host {
namespace {

// 判断是否为标识符字符（用于内核名回溯）。
bool is_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// 判断是否为空白字符。
bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// 从位置 i 起跳过空白（含换行），返回首个非空白字符下标。
std::size_t skip_ws(const std::string& s, std::size_t i) {
    while (i < s.size() && is_space(s[i])) {
        ++i;
    }
    return i;
}

// 跳过行注释（`//` 起，到行尾不含换行），返回之后的下标。
std::size_t skip_line_comment(const std::string& s, std::size_t i) {
    std::size_t j = i + 2;
    while (j < s.size() && s[j] != '\n') {
        ++j;
    }
    return j;
}

// 跳过块注释（`/*` 起，到 `*/` 之后），返回之后的下标。
std::size_t skip_block_comment(const std::string& s, std::size_t i) {
    const std::size_t n = s.size();
    std::size_t j = i + 2;
    while (j + 1 < n && !(s[j] == '*' && s[j + 1] == '/')) {
        ++j;
    }
    return (j + 1 < n) ? j + 2 : n;
}

// 跳过字符串 / 字符字面量（含反斜杠转义），返回闭合引号之后的下标。
std::size_t skip_literal(const std::string& s, std::size_t i) {
    const std::size_t n = s.size();
    const char quote = s[i];
    std::size_t j = i + 1;
    while (j < n && s[j] != quote) {
        if (s[j] == '\\' && j + 1 < n) {
            ++j;
        }
        ++j;
    }
    return (j < n) ? j + 1 : n;
}

// 若 s[i] 处是注释或字符串/字符字面量，返回其**之后**的下标；否则原样返回 i。
// 统一「跳过词法噪声」逻辑，供括号配对 / 顶层切分 / 主扫描复用。
std::size_t skip_trivia(const std::string& s, std::size_t i) {
    const std::size_t n = s.size();
    if (i >= n) {
        return i;
    }
    const char c = s[i];
    if (c == '/' && i + 1 < n && s[i + 1] == '/') {
        return skip_line_comment(s, i);
    }
    if (c == '/' && i + 1 < n && s[i + 1] == '*') {
        return skip_block_comment(s, i);
    }
    if (c == '"' || c == '\'') {
        return skip_literal(s, i);
    }
    return i;
}

// 从位置 i（指向开括号）起，找到与之配对的闭括号，返回闭括号下标；不配对返回 npos。
// 跳过字符串/字符字面量与注释，避免括号出现在其中造成误配。
std::size_t match_paren(const std::string& s, std::size_t i) {
    char open = s[i];
    char close = (open == '(') ? ')' : (open == '{') ? '}' : (open == '[') ? ']' : '\0';
    if (close == '\0') {
        return std::string::npos;
    }
    int depth = 0;
    for (std::size_t j = i; j < s.size();) {
        std::size_t skipped = skip_trivia(s, j);
        if (skipped != j) {
            j = skipped;
            continue;
        }
        char c = s[j];
        if (c == open) {
            ++depth;
        } else if (c == close) {
            --depth;
            if (depth == 0) {
                return j;
            }
        }
        ++j;
    }
    return std::string::npos;
}

// 按顶层逗号切分（跳过嵌套括号/方括号/花括号与字面量/注释）；空串返回单元素列表。
std::vector<std::string> split_top_level(const std::string& s) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    int depth = 0;
    for (std::size_t j = 0; j < s.size();) {
        std::size_t skipped = skip_trivia(s, j);
        if (skipped != j) {
            j = skipped;
            continue;
        }
        char c = s[j];
        if (c == '(' || c == '[' || c == '{') {
            ++depth;
        } else if (c == ')' || c == ']' || c == '}') {
            --depth;
        } else if (c == ',' && depth == 0) {
            parts.push_back(s.substr(start, j - start));
            start = j + 1;
        }
        ++j;
    }
    parts.push_back(s.substr(start));
    return parts;
}

// 去除首尾空白。
std::string trim(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && is_space(s[b])) {
        ++b;
    }
    while (e > b && is_space(s[e - 1])) {
        --e;
    }
    return s.substr(b, e - b);
}

// 从 `<<<` 起始下标 lt 回溯内核名（紧邻其前的标识符）；无则返回空串。
std::string kernel_name_before(const std::string& s, std::size_t lt) {
    std::size_t e = lt;
    while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t')) {
        --e;
    }
    std::size_t b = e;
    while (b > 0 && is_ident_char(s[b - 1])) {
        --b;
    }
    return s.substr(b, e - b);
}

// 改写 `<<<...>>>`：从 lt（指向首个 '<'）起，返回改写文本与消耗到的下标（紧随 `>>>` 之后）。
// 失败时 ok=false 并填 error。
struct LaunchRewrite {
    bool ok = false;
    std::string text;
    std::size_t next = 0;
    std::size_t name_start = 0;  // 内核名在源中的起始下标（供调用方回退已输出的名字）
    std::string kernel_name;     // 回溯到的内核名（供调用方收集）
    std::string error;
};

LaunchRewrite rewrite_launch(const std::string& s, std::size_t lt) {
    LaunchRewrite r;
    // 内核名回溯。
    std::string name = kernel_name_before(s, lt);
    if (name.empty()) {
        r.error = "`<<<` 前未找到内核名（疑似歧义或非法启动语法）";
        return r;
    }
    r.name_start = lt - name.size();
    r.kernel_name = name;
    // 定位 `<<<` 后的配置段，直到 `>>>`。
    std::size_t cfg_begin = lt + 3;
    std::size_t gt = s.find(">>>", cfg_begin);
    if (gt == std::string::npos) {
        r.error = "`<<<` 未找到配对的 `>>>`";
        return r;
    }
    std::string cfg = s.substr(cfg_begin, gt - cfg_begin);
    std::vector<std::string> cfg_parts = split_top_level(cfg);
    if (cfg_parts.size() < 2 || cfg_parts.size() > 4) {
        r.error = "`<<<...>>>` 配置参数个数应为 2/3/4，实际 " + std::to_string(cfg_parts.size());
        return r;
    }
    std::string grid = trim(cfg_parts[0]);
    std::string block = trim(cfg_parts[1]);
    std::string smem = (cfg_parts.size() >= 3) ? trim(cfg_parts[2]) : "0";
    std::string stream = (cfg_parts.size() >= 4) ? trim(cfg_parts[3]) : "nullptr";
    if (grid.empty() || block.empty()) {
        r.error = "`<<<...>>>` 的 grid/block 不能为空";
        return r;
    }
    // 紧随的 `(...)` 实参。
    std::size_t after = skip_ws(s, gt + 3);
    if (after >= s.size() || s[after] != '(') {
        r.error = "`>>>` 后未找到实参列表 `(...)`";
        return r;
    }
    std::size_t close = match_paren(s, after);
    if (close == std::string::npos) {
        r.error = "启动实参 `(...)` 括号不配对";
        return r;
    }
    std::string args_trim = trim(s.substr(after + 1, close - after - 1));

    r.text =
        "haccLaunch(__cool_reg_" + name + ", " + grid + ", " + block + ", " + smem + ", " + stream;
    if (!args_trim.empty()) {
        r.text += ", " + args_trim;
    }
    r.text += ")";
    r.next = close + 1;
    r.ok = true;
    return r;
}

// `__global__` 处理结果：从 i（指向 `__global__`）起，返回替换文本与消耗到的下标。
struct GlobalStrip {
    bool ok = false;
    std::string text;  // 替换文本（声明，或空串表示纯声明剥关键字）
    std::size_t next = 0;
    std::string error;
};

// 处理 `__global__`（host 源）：剥掉关键字，剔除函数体（只留声明）。
GlobalStrip strip_global(const std::string& s, std::size_t i) {
    GlobalStrip g;
    const std::size_t n = s.size();
    // 找到函数体起始 `{`（跳过参数列表等）；遇 `;` 说明是纯声明。
    std::size_t brace = std::string::npos;
    for (std::size_t j = i + 10; j < n; ++j) {
        if (s[j] == '{') {
            brace = j;
            break;
        }
        if (s[j] == ';') {
            break;
        }
    }
    if (brace == std::string::npos) {
        // 纯声明：剥掉 `__global__` 与其后空白，其余原样透传。
        g.text.clear();
        g.next = skip_ws(s, i + 10);
        g.ok = true;
        return g;
    }
    std::size_t close = match_paren(s, brace);
    if (close == std::string::npos) {
        g.error = "`__global__` 函数体 `{...}` 括号不配对";
        return g;
    }
    // host 源：剥掉 `__global__` 与其后空白，保留到 `{` 之间的声明部分（含参数列表），
    // 丢弃函数体；去掉声明末尾空白再补 `;`（避免 `void k() ;` 这类多余空格）。
    std::size_t decl_begin = skip_ws(s, i + 10);
    std::string decl = s.substr(decl_begin, brace - decl_begin);
    while (!decl.empty() && is_space(decl.back())) {
        decl.pop_back();
    }
    g.text = decl + ";";
    g.next = close + 1;
    g.ok = true;
    return g;
}

// 从存储限定符声明处收集变量名：`__constant__ <type> <name> ...;` /
// `__device__ <type> <name> ...;`。返回变量名（非变量则空串）。
// i 指向限定符起始，qualifier_len 为其长度（`__constant__` 12 / `__device__` 10）。
//
// 必须排除**函数**：`__device__ void f(...)` 是常见写法，若把它当符号名，会生成
// 永远解析不到的符号描述符（cuModuleGetGlobal 找不到内核外的 function），
// 并在改写时把函数名误替换成包装调用。判定方式：名字后紧跟 `(` 即视为函数。
std::string symbol_name_after(const std::string& s, std::size_t i, std::size_t qualifier_len) {
    // 跳过限定符与其后空白。
    std::size_t j = skip_ws(s, i + qualifier_len);
    // 跳过类型（可能含 `const`/`unsigned` 等与 `*`/`&`），直到标识符后跟 `;`/`[`/`=`/`,`/`(`。
    // 简化：取最后一个「标识符」作为变量名（类型与变量名之间以空白分隔）。
    std::string last_ident;
    while (j < s.size()) {
        char c = s[j];
        if (c == '(') {
            // 名字后紧跟 `(`：这是函数，不是变量。
            return std::string();
        }
        if (c == ';' || c == '=' || c == '[' || c == ',') {
            break;
        }
        if (is_ident_char(c)) {
            std::size_t b = j;
            while (j < s.size() && is_ident_char(s[j])) {
                ++j;
            }
            last_ident = s.substr(b, j - b);
            continue;
        }
        ++j;
    }
    return last_ident;
}

// 改写 `cudaMemcpyToSymbol` / `coolMemcpyToSymbol`：把第一个参数（符号名）替换为
// hacc 生成的包装函数名，并去掉 kind 参数（包装固定 H2D）。
// 仅当第一个参数是已收集的 `__constant__` 符号名时才改写；否则原样返回（ok=false）。
struct SymbolCopyRewrite {
    bool ok = false;
    std::string text;
    std::size_t next = 0;
};

SymbolCopyRewrite rewrite_memcpy_to_symbol(const std::string& s, std::size_t i,
                                           const std::vector<std::string>& constants) {
    SymbolCopyRewrite r;
    // 定位紧随的 `(`。
    std::size_t paren = skip_ws(s, i);
    if (paren >= s.size() || s[paren] != '(') {
        return r;
    }
    std::size_t close = match_paren(s, paren);
    if (close == std::string::npos) {
        return r;
    }
    std::vector<std::string> args = split_top_level(s.substr(paren + 1, close - paren - 1));
    if (args.size() < 3) {
        return r;  // 至少 symbol, src, count
    }
    std::string sym = trim(args[0]);
    if (std::find(constants.begin(), constants.end(), sym) == constants.end()) {
        return r;  // 非已知 __constant__ 符号：不改写
    }
    // 包装签名：__cool_memcpy_to_symbol_<sym>(src, count, offset)。
    std::string src_arg = trim(args[1]);
    std::string count_arg = trim(args[2]);
    std::string offset_arg = (args.size() >= 4) ? trim(args[3]) : "0";
    r.text = "__cool_memcpy_to_symbol_" + sym + "(" + src_arg + ", " + count_arg + ", " +
             offset_arg + ")";
    r.next = close + 1;
    r.ok = true;
    return r;
}

// 改写 `cudaMemcpyFromSymbol` / `coolMemcpyFromSymbol`：参数为
// `(dst, symbol, count, offset?, kind?)`，把 symbol 换成 hacc 包装函数名。
// 包装固定 D2H，故丢掉 kind 参数。仅当 symbol 是已收集的符号名时才改写。
SymbolCopyRewrite rewrite_memcpy_from_symbol(const std::string& s, std::size_t i,
                                             const std::vector<std::string>& symbols) {
    SymbolCopyRewrite r;
    std::size_t paren = skip_ws(s, i);
    if (paren >= s.size() || s[paren] != '(') {
        return r;
    }
    std::size_t close = match_paren(s, paren);
    if (close == std::string::npos) {
        return r;
    }
    std::vector<std::string> args = split_top_level(s.substr(paren + 1, close - paren - 1));
    if (args.size() < 3) {
        return r;  // 至少 dst, symbol, count
    }
    std::string sym = trim(args[1]);
    if (std::find(symbols.begin(), symbols.end(), sym) == symbols.end()) {
        return r;  // 非已知符号：不改写
    }
    // 包装签名：__cool_memcpy_from_symbol_<sym>(dst, count, offset)。
    std::string dst_arg = trim(args[0]);
    std::string count_arg = trim(args[2]);
    std::string offset_arg = (args.size() >= 4) ? trim(args[3]) : "0";
    r.text = "__cool_memcpy_from_symbol_" + sym + "(" + dst_arg + ", " + count_arg + ", " +
             offset_arg + ")";
    r.next = close + 1;
    r.ok = true;
    return r;
}

// 改写 `cudaGetSymbolAddress` / `coolGetSymbolAddress`：参数为 `(devPtr, symbol)`，
// 把 symbol 换成 hacc 包装函数名（包装负责先确保模块已载入再取地址）。
SymbolCopyRewrite rewrite_get_symbol_address(const std::string& s, std::size_t i,
                                             const std::vector<std::string>& symbols) {
    SymbolCopyRewrite r;
    std::size_t paren = skip_ws(s, i);
    if (paren >= s.size() || s[paren] != '(') {
        return r;
    }
    std::size_t close = match_paren(s, paren);
    if (close == std::string::npos) {
        return r;
    }
    std::vector<std::string> args = split_top_level(s.substr(paren + 1, close - paren - 1));
    if (args.size() < 2) {
        return r;
    }
    std::string sym = trim(args[1]);
    if (std::find(symbols.begin(), symbols.end(), sym) == symbols.end()) {
        return r;  // 非已知符号：不改写
    }
    r.text = "__cool_get_symbol_address_" + sym + "(" + trim(args[0]) + ")";
    r.next = close + 1;
    r.ok = true;
    return r;
}

}  // namespace

RewriteResult rewrite_source(const std::string& src, const RewriteOptions& options) {
    RewriteResult result;
    std::string out;
    out.reserve(src.size());

    // M3.2 预扫描：收集文件作用域 `__constant__` / `__device__` 变量名
    // （供改写符号拷贝/取址 API）。单趟主循环中声明通常先于使用，但预扫描更稳妥
    // （顺序无关）。
    for (std::size_t k = 0; k + 12 <= src.size();) {
        std::size_t sk = skip_trivia(src, k);
        if (sk != k) {
            k = sk;
            continue;
        }
        if (src.compare(k, 12, "__constant__") == 0) {
            std::string nm = symbol_name_after(src, k, 12);
            if (!nm.empty() && std::find(result.constant_names.begin(), result.constant_names.end(),
                                         nm) == result.constant_names.end()) {
                result.constant_names.push_back(nm);
            }
            k += 12;
            continue;
        }
        // `__device__` 变量（函数已由 symbol_name_after 排除）。
        if (src.compare(k, 10, "__device__") == 0) {
            std::string nm = symbol_name_after(src, k, 10);
            if (!nm.empty() && std::find(result.device_names.begin(), result.device_names.end(),
                                         nm) == result.device_names.end()) {
                result.device_names.push_back(nm);
            }
            k += 10;
            continue;
        }
        ++k;
    }

    std::size_t i = 0;
    const std::size_t n = src.size();
    while (i < n) {
        // 注释 / 字符串 / 字符字面量：原样透传。
        std::size_t skipped = skip_trivia(src, i);
        if (skipped != i) {
            out.append(src, i, skipped - i);
            i = skipped;
            continue;
        }

        char c = src[i];

        // `<<<` 启动语法。
        if (c == '<' && i + 2 < n && src[i + 1] == '<' && src[i + 2] == '<') {
            LaunchRewrite lr = rewrite_launch(src, i);
            if (!lr.ok) {
                result.ok = false;
                result.error = lr.error;
                return result;
            }
            // 记录内核名（去重、按首次出现顺序）。
            if (std::find(result.kernel_names.begin(), result.kernel_names.end(), lr.kernel_name) ==
                result.kernel_names.end()) {
                result.kernel_names.push_back(lr.kernel_name);
            }
            // 内核名已随主循环逐字符写入 out，需回退（改写文本自带 `__cool_reg_<name>`）。
            out.resize(out.size() - (i - lr.name_start));
            out += lr.text;
            i = lr.next;
            continue;
        }

        // `__global__`：剥关键字；host 源（keep_global_bodies=false）再剔除函数体。
        // device 源（keep_global_bodies=true）**不动** `__global__`（cpfe 需其属性识别内核）。
        if (!options.keep_global_bodies && c == '_' && src.compare(i, 10, "__global__") == 0) {
            GlobalStrip g = strip_global(src, i);
            if (!g.ok) {
                result.ok = false;
                result.error = g.error;
                return result;
            }
            out += g.text;
            i = g.next;
            continue;
        }

        // M3.2 `__constant__`：host 源剔除声明（g++ 不认识该关键字）；device 源原样保留。
        if (!options.keep_global_bodies && c == '_' && src.compare(i, 12, "__constant__") == 0) {
            // 跳到声明末尾 `;`（跳过括号/字面量）。
            std::size_t j = i + 12;
            while (j < n && src[j] != ';') {
                std::size_t sj = skip_trivia(src, j);
                if (sj != j) {
                    j = sj;
                    continue;
                }
                ++j;
            }
            i = (j < n) ? j + 1 : n;  // 连同 `;` 一起剔除
            continue;
        }

        // `cudaMemcpyToSymbol` / `coolMemcpyToSymbol`（主机 → 设备符号）：
        //   host 源 → 改写为 hacc 包装调用（__cool_memcpy_to_symbol_<sym>）；
        //   device 源 → 整体替换为 `coolSuccess`（该调用只在 host 侧执行，device 源仅需
        //     能解析；用 `coolSuccess` 而非 `(void)0`，因为调用常出现在
        //     `coolError_t e = ...` 这类表达式位置，`(void)0` 无法初始化 coolError_t）。
        if (c == 'c' && (src.compare(i, 18, "cudaMemcpyToSymbol") == 0 ||
                         src.compare(i, 18, "coolMemcpyToSymbol") == 0)) {
            if (options.keep_global_bodies) {
                std::size_t paren = skip_ws(src, i + 18);
                if (paren < n && src[paren] == '(') {
                    std::size_t close = match_paren(src, paren);
                    if (close != std::string::npos) {
                        out += "coolSuccess";
                        i = close + 1;
                        continue;
                    }
                }
            } else {
                // 符号可能是 __constant__ 或 __device__ 变量：两张表都要查
                // （两者互斥，故按序尝试即可）。
                SymbolCopyRewrite sr = rewrite_memcpy_to_symbol(src, i + 18, result.constant_names);
                if (!sr.ok) {
                    sr = rewrite_memcpy_to_symbol(src, i + 18, result.device_names);
                }
                if (sr.ok) {
                    out += sr.text;
                    i = sr.next;
                    continue;
                }
            }
        }

        // `cudaMemcpyFromSymbol` / `coolMemcpyFromSymbol`（设备符号 → 主机/设备）：
        //   host 源 → 改写为 hacc 包装调用（__cool_memcpy_from_symbol_<sym>）;
        //   device 源 → 替换为 `coolSuccess`。不用 `(void)0` 是因为该 API 有返回值，
        //     常出现在 `coolError_t e = ...` 这类表达式位置，`(void)0` 无法赋给 coolError_t。
        if (c == 'c' && (src.compare(i, 20, "cudaMemcpyFromSymbol") == 0 ||
                         src.compare(i, 20, "coolMemcpyFromSymbol") == 0)) {
            if (options.keep_global_bodies) {
                std::size_t paren = skip_ws(src, i + 20);
                if (paren < n && src[paren] == '(') {
                    std::size_t close = match_paren(src, paren);
                    if (close != std::string::npos) {
                        out += "coolSuccess";
                        i = close + 1;
                        continue;
                    }
                }
            } else {
                SymbolCopyRewrite sr = rewrite_memcpy_from_symbol(src, i + 20, result.device_names);
                if (sr.ok) {
                    out += sr.text;
                    i = sr.next;
                    continue;
                }
            }
        }

        // `cudaGetSymbolAddress` / `coolGetSymbolAddress`：同上，host 源改写为
        // __cool_get_symbol_address_<sym>(devPtr)，device 源替换为 `coolSuccess`。
        if (c == 'c' && (src.compare(i, 20, "cudaGetSymbolAddress") == 0 ||
                         src.compare(i, 20, "coolGetSymbolAddress") == 0)) {
            if (options.keep_global_bodies) {
                std::size_t paren = skip_ws(src, i + 20);
                if (paren < n && src[paren] == '(') {
                    std::size_t close = match_paren(src, paren);
                    if (close != std::string::npos) {
                        out += "coolSuccess";
                        i = close + 1;
                        continue;
                    }
                }
            } else {
                SymbolCopyRewrite sr = rewrite_get_symbol_address(src, i + 20, result.device_names);
                if (sr.ok) {
                    out += sr.text;
                    i = sr.next;
                    continue;
                }
            }
        }

        out += c;
        ++i;
    }

    result.ok = true;
    result.source = std::move(out);
    return result;
}

}  // namespace hacc::host
