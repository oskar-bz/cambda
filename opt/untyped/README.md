# clam — a fast lambda calculus evaluator

A full β-normalizer for the untyped lambda calculus, written in C and built
with `clang`.  It starts from a readable, obviously-correct baseline and is
optimized — measurement by measurement — into a high-throughput evaluator that
reduces on the order of **10⁸ β-reductions per second**.

```
$ cat prelude.lam bench/fib.lam | ./clam.exe
[fast] 278946043 beta in 1.67s = 167 Mβ/s
church 75025
```

## Results

Throughput in millions of β-reductions per second (best of 3, this machine).
`baseline` is the naive substitution normalizer; `nbe` is the readable
Normalization-by-Evaluation reference; `clam` is the optimized engine.

| workload      | what it stresses                    | baseline | nbe   | clam  |
|---------------|-------------------------------------|---------:|------:|------:|
| `collapse25`  | reduction throughput (id^2²⁵ → y)   |    —     |  130  |  264  |
| `fib`         | realistic recursion (fib 25, Church)|    —     |  102  |  167  |
| `pow25`       | building a 2²⁵-node normal form     |    —     |   37  |   61  |

The substitution baseline is *asymptotically* slower — it copies subterms on
every β-step. Wall-clock on a size it can still finish:

| workload (size)    | baseline | clam     | speedup |
|--------------------|---------:|---------:|--------:|
| `collapse15` (2¹⁵) |  1.13 s  | 0.021 s  |  ~54×   |
| `collapse18` (2¹⁸) |  9.76 s  | 0.022 s  | ~440×   |

The speedup grows without bound with input size: `clam` is linear in the number
of β-reductions, the substitution baseline is super-linear.

## The files

| file         | what it is                                                       |
|--------------|------------------------------------------------------------------|
| `baseline.c` | Readable reference: capture-avoiding substitution, normal order. |
| `nbe.c`      | Readable Normalization-by-Evaluation (closures + de Bruijn).     |
| `clam.c`     | The optimized engine — readable & commented (compiled to `clam.exe`). |
| `jit.c`      | Bytecode compiler + threaded VM (compiled to `jit.exe`) — see below. |
| `gc.c`       | An explored-and-rejected alternative (copying GC) — see below.   |
| `prelude.lam`| Church-encoding prelude (numerals, arithmetic, booleans).        |

## How it works (and how it got fast)

Every stage was driven by measurement; here is the journey.

1. **Baseline — substitution (`baseline.c`).** Terms are an AST with string
   variable names; reduction is leftmost-outermost to full normal form;
   substitution is capture-avoiding via fresh-variable renaming. Correct and
   readable, but every β-step copies a subterm → super-linear.

2. **Normalization by Evaluation (`nbe.c`).** Convert to de Bruijn indices
   (no α-renaming), evaluate into a domain of **closures** (a λ captures its
   environment; β just extends an environment — arguments are *shared*, never
   copied), then **read back** ("quote") the value to a normal form, going
   under binders against fresh neutral variables. This is the asymptotic win:
   `id^n` collapses in O(n) instead of O(n²). Readback is iterative so deep
   normal forms don't blow the C stack.

3. **`clam.c` — squeezing the constant factor.** Profiling (and a 6.6 GB memory
   measurement) showed the cost is *compute per β*, dominated by allocation and
   call overhead — **not** memory bandwidth (in strict evaluation dead cells are
   never re-read and live reads hit recently-allocated, cache-hot cells). So:

   - **Top-level `let`s lifted to globals** — evaluated once, O(1) lookup.
   - **Tagged-pointer values** — a value is a `uint64_t`; closures are 16-byte
     cells; no separate 32-byte `Val` struct, no extra indirection on lookup.
   - **Inlined fixed-16B bump allocator** — all four runtime cell types are
     exactly 16 bytes, so allocation is one pointer-bump + one bounds check.
   - **`eval_atom` fast path** — variable/global/lambda arguments are evaluated
     inline instead of through a recursive call. *(biggest single win)*
   - **Allocation-free neutral heads + 1-cell application** — a neutral spine is
     a cons-list ending in an *immediate* head, so applying a neutral is a single
     16-byte allocation and heads cost nothing.
   - **Fast bump allocator for readback `Tm` nodes.**
   - **Profile-Guided Optimization** (`build_pgo.sh`) — ~15–30% on the branchy
     interpreter loop.

### Bytecode VM (`jit.c`)

A second engine that *compiles* the program to a flat bytecode for a strict
abstract machine (a call-by-value cousin of OCaml's ZINC/ZAM), then executes it.
It was optimized iteratively — switch dispatch → computed-goto threading →
super-instructions (`PUSH`/`APPLY` fused with their operand) → a two-operand
`APP2` instruction for `atom atom` applications (no value-stack traffic):

| stage                    | fib | collapse25 | pow25  (Mβ/s) |
|--------------------------|----:|-----------:|------:|
| switch dispatch          | 120 |    152     |  48   |
| computed-goto threading  | 153 |    180     |  61   |
| + super-instructions     | 173 |    200     |  64   |
| + `APP2` two-operand op   | 181 |    208     |  67   |
| **`clam.c` tree-walker**  | **196** | **308** | **70** |

The honest result: even fully threaded with super-instructions, the bytecode VM
**does not beat the PGO-compiled tree-walker** on this hardware.  Two reasons,
both predicted: (1) every instruction is an indirect dispatch branch, whereas
the tree-walker is straight-line native code; (2) the VM threads control through
an explicit return stack, while the tree-walker uses native C recursion, which
the CPU's return-address predictor handles essentially for free.  The collapse
benchmark (a tight loop of trivial applications) exposes this most: it is almost
all dispatch + return-stack churn, where native recursion dominates.

Beating the tree-walker requires removing interpretation entirely — i.e. a
*native* code generator (emit machine code, with the application spine turned
into a trampolined loop so deep strict chains don't overflow).  That is a large,
separate undertaking; the bytecode VM here is the well-tuned ceiling of the
*interpreted* approach.

### Approaches that were tried and rejected

- **Spine collection** (gather an application spine into a buffer, apply with a
  tail-loop): the bookkeeping cost more than it saved (fib 174 → 123). Reverted.
- **Copying garbage collector (`gc.c`).** It works and cuts peak memory from
  6.6 GB to 143 MB, but it is *slower* (fib 167 → ~100; pow 60 → 16): an
  explicit-stack machine is needed to reify GC roots, and that overhead — plus
  re-copying the live set — outweighs any cache benefit, because the workload was
  never memory-bandwidth bound. Kept as a documented dead-end and as the basis
  for a memory-constrained mode.

`clam` trades memory for speed: it bump-allocates and never frees, so a long
run can use many GB. Use `gc.exe` if you need bounded memory.

## Syntax

```
term  := app
app   := atom atom*            -- application, left associative
atom  := '\' var+ '.' term     -- lambda (multi-binder sugar: \x y. e)
       | 'let' var '=' term 'in' term
       | '(' term ')'
       | var
```

`#` starts a line comment. A normal form of shape `\f.\x. f (f ... x)` is
printed as `church N`. Free variables are left in the output.

**Evaluation strategy.** `clam`/`nbe` are *strict* (call-by-value in the
semantic apply): they find the normal form of every strongly-normalizing term,
and agree with the normal-order `baseline` on those. They will diverge on the
rare weakly-normalizing term whose value needs a non-strict argument (e.g.
`(\x.\y.y) Ω`); for those use `baseline`.

## Build & test

```
bash build.sh      baseline.c baseline.exe   # plain -O3 builds
bash build.sh      nbe.c      nbe.exe
bash build_pgo.sh  clam.c     clam.exe        # profile-guided optimized engine
bash build_pgo.sh  jit.c      jit.exe         # bytecode VM (computed-goto)
bash check.sh                                 # clam vs nbe on a correctness suite
bash bench.sh      ./clam.exe                  # throughput on the benchmark suite
```

Requires `clang` with `lld` and `llvm-profdata` (ships with LLVM).
