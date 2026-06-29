#!/usr/bin/env bash
# build.sh [src] [out] -- optimized build
set -e
SRC="${1:-nbe.c}"
OUT="${2:-nbe.exe}"
clang -O3 -DNDEBUG -march=native -flto -fuse-ld=lld \
      -fno-stack-protector \
      -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE \
      -Xlinker /STACK:268435456 \
      -o "$OUT" "$SRC" 2>&1 | grep -iv deprecat | grep -iE 'error|warn' || true
echo "built $OUT"
