#!/usr/bin/env bash
# 按 ADR 0005 拉取并构建 EDG C/C++ 前端（不 vendor 进仓库）。
# 产物：<workdir>/edg/build/gcc/bin/cpfe
#
# 用法: tools/il_spike/fetch_edg.sh [workdir]
set -euo pipefail

# ADR 0005：固定 commit 作为版本锚点。
EDG_COMMIT=43e10b90fc83ed2a436fb79b241be8dbf3dc6846
EDG_URL=https://github.com/edgcpp/compiler

WORKDIR=${1:-build/il_spike}
EDG_DIR="$WORKDIR/edg"
BUILD_DIR="$EDG_DIR/build/gcc"

if [ ! -d "$EDG_DIR/.git" ]; then
    git clone --filter=blob:none --no-checkout --depth 1 "$EDG_URL" "$EDG_DIR"
fi

# 只检出构建所需目录，避开庞大的 tests/benchmarks。
git -C "$EDG_DIR" sparse-checkout set --cone \
    src cmake util nt_util bases include_c++ include_c99 lib_src dev_tools
git -C "$EDG_DIR" checkout

# 注意：dev_tools/bin 需要存在，否则 CMake 找不到 edg-cmakedef。
cmake -S "$EDG_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DEDG_MACRO_CONF=linux-gcc \
    -DEDG_BUILD_LIB=OFF
cmake --build "$BUILD_DIR" -j

echo "cpfe: $BUILD_DIR/bin/cpfe"
