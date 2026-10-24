#!/bin/sh
# PTX 指纹校验：逐个断言给定正则都命中目标 PTX 文件。
#
# 用法：check_ptx_patterns.sh <file.ptx> <正则1> [正则2 ...]
# 退出码：0 = 全部命中；1 = 有缺失（打印缺失的模式）。
#
# 为什么用脚本而不是多条 grep 测试：一条指令没发射时，能直接看到**哪一条**缺失，
# 而不是只得到「某个 grep 测试失败」。
set -eu

ptx_file="$1"
shift

if [ "$#" -eq 0 ]; then
    echo "用法: check_ptx_patterns.sh <file.ptx> <正则...>" >&2
    exit 1
fi

missing=0
for pattern in "$@"; do
    if grep -qE "$pattern" "$ptx_file"; then
        echo "OK: $pattern"
    else
        echo "FAIL: 缺失 $pattern" >&2
        missing=1
    fi
done

exit "$missing"
