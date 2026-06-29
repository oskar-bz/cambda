#!/usr/bin/env bash
# build_pgo.sh [src] [out] -- profile-guided optimized build
set -e
SRC="${1:-clam.c}"; OUT="${2:-clam.exe}"
PROFDATA="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/Llvm/x64/bin/llvm-profdata.exe"
FLAGS="-O3 -DNDEBUG -march=native -fuse-ld=lld -fno-stack-protector \
 -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE -Xlinker /STACK:268435456"

# 1. instrumented build
clang $FLAGS -fprofile-instr-generate -o _prof.exe "$SRC" 2>&1 | grep -iE 'error' || true
# 2. training runs (representative workloads)
cat prelude.lam bench/fib.lam        > _t1.lam
cat prelude.lam bench/collapse25.lam > _t2.lam
cat prelude.lam bench/pow25.lam      > _t3.lam
LLVM_PROFILE_FILE=_p1.profraw ./_prof.exe _t1.lam >/dev/null 2>&1
LLVM_PROFILE_FILE=_p2.profraw ./_prof.exe _t2.lam >/dev/null 2>&1
LLVM_PROFILE_FILE=_p3.profraw ./_prof.exe _t3.lam >/dev/null 2>&1
# 3. merge + 4. optimized build
"$PROFDATA" merge -output=_clam.profdata _p1.profraw _p2.profraw _p3.profraw
clang $FLAGS -fprofile-instr-use=_clam.profdata -o "$OUT" "$SRC" 2>&1 \
  | grep -iE 'error|warning' | grep -iv deprecat || true
rm -f _prof.exe _p*.profraw _t*.lam _clam.profdata
echo "built $OUT (PGO)"
