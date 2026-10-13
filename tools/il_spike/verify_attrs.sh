#!/usr/bin/env bash
# 验证 A2 决策 (a)：EDG 构建期开启 RECORD_UNRECOGNIZED_ATTRIBUTES 后，
# 未知/自定义属性（CUDA 关键字拟以 __attribute__((…)) 承载）是否随 IL 传递。
#
# 前置：tools/il_spike/fetch_edg.sh（已带 -DRECORD_UNRECOGNIZED_ATTRIBUTES=1 构建 cpfe）。
# 用法: tools/il_spike/verify_attrs.sh [input.cpp]
# 退出码：0 = 全部关键属性名出现在 IL；1 = 有缺失（判「IL 表达力不足」）。
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
CPFE=${CPFE:-build/il_spike/edg/build/gcc/bin/cpfe}
EDG_BASE=${EDG_BASE:-build/il_spike/edg/bases/docker/dev-env/gcc}
SRC=${1:-"$HERE/inputs/kernel_probe_attr2.cpp"}

# 期望出现在 IL 中的属性名（CUDA 存储/执行限定符 + 地址空间）。
EXPECTED=(cool_shared cool_global address_space)

IL=$("$CPFE" --edg_base_dir "$EDG_BASE" --g++ -N --il_display "$SRC" 2>/dev/null)

rc=0
for name in "${EXPECTED[@]}"; do
    if grep -q "\"$name\"" <<<"$IL"; then
        echo "OK   attribute in IL: $name"
    else
        echo "MISS attribute NOT in IL: $name"
        rc=1
    fi
done

# 顺带确认「未知属性」确实以 kind: unrecognized 记录（而非被丢弃）。
if grep -q "kind: *unrecognized" <<<"$IL"; then
    echo "OK   IL contains 'kind: unrecognized' entries"
else
    echo "MISS no 'kind: unrecognized' entry in IL"
    rc=1
fi

exit $rc
