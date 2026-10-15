#!/bin/sh
# M3.4 诊断质量单测：验证 coolicc 的翻译错误消息带源位置（"文件:行: 成因"）。
#
# 用法：check_diag_position.sh <hacc> <input.cu> <coolrt_include> <期望行号>
# 期望：hacc 编译失败（退出码非 0），且 stderr 含 "<...>:<期望行号>: " 形式的位置前缀。
#
# 退出码：0 = 通过；1 = 失败（消息缺位置或行号不符）。
set -eu

hacc_bin="$1"
input_cu="$2"
coolrt_inc="$3"
want_line="$4"

tmp_ptx="$(mktemp)"
trap 'rm -f "$tmp_ptx"' EXIT

# 捕获 stderr（诊断在 stderr）；预期编译失败。
if err="$("$hacc_bin" "$input_cu" --emit=ptx -o "$tmp_ptx" -I "$coolrt_inc" 2>&1)"; then
    echo "FAIL: 预期编译失败，但 hacc 成功退出" >&2
    exit 1
fi

# 校验消息含 ":<行号>: " 位置前缀（文件名可含路径，故只锚定 ":<行号>: "）。
if printf '%s' "$err" | grep -qE ":${want_line}: "; then
    echo "OK: 诊断含源位置（行 $want_line）"
    exit 0
fi

echo "FAIL: 诊断缺少源位置（期望行 $want_line）" >&2
echo "实际 stderr: $err" >&2
exit 1
