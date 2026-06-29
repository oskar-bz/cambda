# Inference performance log

Metric: best-of-7 µs/infer via `bash bench_all.sh` (plain `-O2`, clang). PGO is
applied only at the end (adds a few % on top). Lower is better.

Profile (PGO counts) hot functions: `prune` 80M, `infer` 31M, `arena_alloc`/`mk`
~30M, `inst_rec` 27M, `mk_arrow` 15M, `imap` ~20M, `fresh_var` 12M, `unify` 10M.
So: allocation + pruning + instantiation dominate; the algorithm is incidental.

| # | change | bench | lookup | big | notes |
|---|--------|------:|-------:|----:|-------|
| 0 | baseline (round-2 state) | 19.47 | 9.00 | 73.87 | starting point this push |
| 1 | inline fast paths: prune / arena_alloc / mk (slow paths split out-of-line) | 15.63 | 8.40 | 58.07 | **−20% bench/big**. Function-call overhead on 80M+30M+30M calls was real. |
| 2 | inline mk_* allocators; reuse one global instantiation map (no per-call stack frame) | 15.50 | 8.27 | 56.77 | ~1-2% |
| 3 | dedicated 32-byte node allocator (skip align math) | 15.47 | 8.40 | 56.73 | noise (compiler already folded the constant) — kept, harmless |
| – | compiler flags -O3 / -march=native / -flto | 15.40 | 8.20 | 57.0 | within noise; LTO link fails on MSVC. Stay -O2 dev, PGO at end. |
