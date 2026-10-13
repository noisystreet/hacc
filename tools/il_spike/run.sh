#!/usr/bin/env bash
# 用 EDG 前端导出人类可读 IL（A2 的 IL spike）。
# 前置：先跑 tools/il_spike/fetch_edg.sh 构建 cpfe。
# 用法: tools/il_spike/run.sh <input.cpp>
set -euo pipefail

CPFE=${CPFE:-build/il_spike/edg/build/gcc/bin/cpfe}
EDG_BASE=${EDG_BASE:-build/il_spike/edg/bases/docker/dev-env/gcc}
SRC=${1:?usage: run.sh <input.cpp>}

# --edg_base_dir：指向含 lib/predefined_macros.txt 的目录（否则报「cannot open
#               predefined macro file」）。
# --il_display ：人类可读显示 IL。
# -N           ：保留「未降级」IL（C++ 层，信息最全，对应 DO_IL_LOWERING 的反面）。
# --g++        ：打开 GNU 扩展（__attribute__ 等）；注意 --gcc 是 C 模式。
"$CPFE" --edg_base_dir "$EDG_BASE" --g++ -N --il_display "$SRC"
