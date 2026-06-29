#!/usr/bin/env bash
# Profile-guided build of the typed interpreter (mirrors clam's build_pgo.sh).
# Needs clang + llvm-profdata on PATH.
set -e
CC="${CC:-clang}"

echo "[1/4] instrumented build"
$CC -O2 -fprofile-instr-generate -o typed_prof.exe typed.c

echo "[2/4] training runs (exercise the inference hot path)"
rm -f prof-*.profraw
LLVM_PROFILE_FILE="prof-1.profraw" ./typed_prof.exe --bench 8000 bench.tl   >/dev/null
LLVM_PROFILE_FILE="prof-2.profraw" ./typed_prof.exe --bench 8000 lookup.tl  >/dev/null
LLVM_PROFILE_FILE="prof-3.profraw" ./typed_prof.exe examples.tl             >/dev/null

echo "[3/4] merge profile"
llvm-profdata merge -output=typed.profdata prof-*.profraw

echo "[4/4] optimized build"
$CC -O2 -fprofile-instr-use=typed.profdata -o typed.exe typed.c

rm -f typed_prof.exe prof-*.profraw
echo "built typed.exe (PGO)"
