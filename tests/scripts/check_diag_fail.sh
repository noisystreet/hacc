#!/bin/sh
# M3.5 边界用例单测：验证 hacc 对非法/未覆盖构造 fail loud（编译失败 + 诊断匹配）。
#
# 用法：check_diag_fail.sh <hacc> <input.cu> <coolrt_include> <期望正则>
# 期望：hacc 编译失败（退出码非 0），且 stderr 匹配给定正则（如 ":6: 不支持的语句 kind"）。
#
# 退出码：0 = 通过；1 = 失败（未失败或诊断不匹配）。
set -eu

hacc_bin="$1"
input_cu="$2"
coolrt_inc="$3"
want_re="$4"

tmp_ptx="$(mktemp)"
trap 'rm -f "$tmp_ptx"' EXIT

# 捕获 stderr（诊断在 stderr）；预期编译失败。
if err="$("$hacc_bin" "$input_cu" --emit=ptx -o "$tmp_ptx" -I "$coolrt_inc" 2>&1)"; then
    echo "FAIL: 预期编译失败，但 hacc 成功退出" >&2
    exit 1
fi

# 校验诊断匹配期望正则。
if printf '%s' "$err" | grep -qE "$want_re"; then
    echo "OK: 诊断匹配（$want_re）"
    exit 0
fi

echo "FAIL: 诊断不匹配（期望正则 $want_re）" >&2
echo "实际 stderr: $err" >&2
exit 1
