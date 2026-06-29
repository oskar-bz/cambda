#!/usr/bin/env bash
# Build (-O2 for consistent measurement) and report best-of-7 us/infer.
set -e
CC="${CC:-clang}"
FLAGS="${FLAGS:--O2}"
$CC $FLAGS -o typed_bench.exe typed.c
for b in bench.tl lookup.tl big.tl; do
  best=$(for i in $(seq 7); do ./typed_bench.exe --bench 30000 "$b"; done \
         | grep -oE '[0-9]+\.[0-9]+ us' | grep -oE '[0-9]+\.[0-9]+' | sort -n | head -1)
  printf '  %-10s %8s us/infer\n' "$b" "$best"
done
