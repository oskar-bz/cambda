# typed — a typed lambda-calculus interpreter

A small, fast interpreter for a typed lambda calculus, sibling to the untyped
`clam` normalizer. Where `clam` is about *fast normalization*, this is about
*static typing*: catching ill-typed programs before they run.

## Type system: Hindley–Milner (let-polymorphism)

Chosen as the sweet spot of the design space — see the debate table below. HM
gives **full type inference with zero annotations** while staying
**Turing-complete** (via `let rec`). It is what ML/OCaml/Haskell-98 are built on.

- **Algorithm W** with destructive **unification** (union-find on mutable type
  variables, path-compressed).
- **Generalization via levels** (Rémy/Kiselyov): each unbound tyvar carries a
  level; generalizing a `let` keeps only the vars deeper than the current level
  instead of scanning the environment. The occurs check doubles as the
  level-lowering pass.
- Types **erase** before evaluation; a strict (CBV) de-Bruijn closure machine
  runs the underlying untyped term. Recursion is a self-referential closure.

```
let id = \x. x          -- id : a -> a          (inferred, no annotations)
let f = \x. x in f f    -- a -> a               (let-polymorphism)
\x. x x                 -- REJECTED: occurs check (canonical un-HM-typable term)
true + 1                -- REJECTED: cannot unify Int with Bool
```

## Build & run

```sh
bash build.sh                 # or: clang -O2 -o typed.exe typed.c

./typed.exe                   # REPL  (try :help)
./typed.exe examples.tl       # run a file
./typed.exe --no-eval f.tl    # type-check only, print types, skip evaluation
./typed.exe --bench N f.tl    # re-run inference on the file's final expression
                              #   N times; report throughput
```

### REPL

```
> let rec fact = \n. if n == 0 then 1 else n * fact (n-1)
fact : Int -> Int = <function>
> fact 5
120 : Int
> :type \x y. x
: a -> b -> a
```

Commands: `:type`/`:t`, `:env`, `:load`/`:l`, `:reset`, `:help`/`:h`/`:?`,
`:quit`/`:q`. A line left mid-expression (open paren, trailing `=`, `in`, …)
continues on the next line at a `...` prompt; otherwise one statement per line.

## Syntax

```
e ::= \x y. e | \x:T. e       lambda (curried; annotations optional, T checked)
    | let x = e in e          polymorphic let
    | let rec f = e in e      recursive let (general recursion)
    | if e then e else e
    | e e                     application
    | e+e  e-e  e*e           arithmetic  : Int -> Int -> Int
    | e==e e<e e<=e e>e e>=e  comparison  : Int -> Int -> Bool
    | n | true | false | x | ( e )
T ::= Int | Bool | T -> T | ( T )
```

## Performance

The inference path is the optimization target. Versus a clean
assoc-list/`malloc`/`strcmp` baseline (`typed_v1.c`):

| benchmark | baseline | optimized | speedup |
|---|---|---|---|
| `bench.tl`  — 400-deep polymorphic `let` chain | 145 µs/infer | 19 µs/infer | **7.6×** |
| `lookup.tl` — 250-deep scope, lookup-bound      | 206 µs/infer | 9 µs/infer  | **23×**  |

(reproduce: `./typed.exe --bench 20000 bench.tl`, and the same against
`typed_v1.exe`.)

What changed:

- **Arena (bump) allocation** for every node; no per-node `malloc`/`free`.
  Transient inference types live in a scratch arena that is **reset after each
  statement**, so steady-state inference allocates ~nothing and never frees
  individually.
- **String interning** — identifiers become canonical pointers; variable
  comparison is a pointer compare, never `strcmp`.
- **Name resolution to indices** — a resolve pass rewrites each variable to a
  de Bruijn index (locals) or a global slot. Inference and evaluation then do
  **O(1) array lookups** instead of scanning an association list. This is the
  big win on deep scopes (the 16× row).
- **Copy-free instantiation** — instantiating a monomorphic scheme (e.g. a
  primitive's `Int -> Int -> Int`) returns the original nodes, allocating only
  for the parts that actually contain quantified variables.
- **Memoized DAG traversal.** Inferred types are shared DAGs (e.g. `pair x x`
  makes a node share its child). `occurs` / `generalize` / `instantiate` carry a
  per-pass stamp + memo on each node so a shared subterm is visited **once**,
  not once per path. Without this, those passes expand the DAG into the
  (exponentially larger) tree it denotes.
- **Peeled application spine.** When a function already has a known arrow type
  (the common case), `f a` unifies the argument against the domain and returns
  the codomain directly — skipping the fresh result variable and the arrow node
  the general case would allocate. ~24% on the inference path.
- **32-byte type nodes.** A solved variable becomes a `T_LINK` forwarding node,
  letting the link pointer share storage with the unbound var's id/level; the
  node shrinks 40 → 32 bytes (better cache footprint). ~6%.

### Pathological inputs

The classic HM stress test is `let`-doubling with Church pairs, where the
inferred type doubles at every level. Memoization is decisive here:

| doublings | before memo | after memo |
|---|---|---|
| N = 4  | 6 ms      | < 0.1 ms |
| N = 5  | **> 10 s** | < 0.1 ms |
| N = 12 | (hopeless) | 7 ms     |

It moves the cliff ~7 levels (the un-memoized passes were **doubly**
exponential, 2^(2^N); memoized they are the inherent single exponential). HM
inference is DEXPTIME-complete, so a wall still exists around N = 13 — but no
implementation escapes that, and real code never approaches it. `show` carries a
node budget so printing such a type can't hang the REPL.

### PGO

`bash build_pgo.sh` produces a profile-guided `typed.exe` (instrument → train on
the benchmarks → rebuild). Worth a steady ~3–6% on the inference path for free;
needs `clang` + `llvm-profdata`.

## Design debate: which type system fits which workload

| Workload | Best fit | Why |
|---|---|---|
| Teaching core / guaranteed termination | **STLC** | trivial checker, but not Turing-complete |
| General use, no annotations, *usable*   | **Hindley–Milner + `fix`** ← chosen | full inference, Turing-complete |
| Typed IR / showcase parametricity        | **System F** | max parametric power, but inference undecidable → must annotate |
| Higher-kinded abstraction                | **Fω** | type-level functions; usually overkill |
| Proofs / types-as-propositions           | **Dependent (λΠ/CoC)** | checker must evaluate terms; big effort, totality concerns |

The natural next step up is **bidirectional checking** to admit explicit
higher-rank (System F) annotations where HM gives up, without losing inference
elsewhere.

## Files

- `typed.c` — the interpreter (single file).
- `typed_v1.c` — the readable baseline before the performance work; kept for the
  `--bench` comparison above.
- `examples.tl`, `bench.tl`, `lookup.tl` — sample / benchmark programs.
- `build.sh` (plain `-O2`), `build_pgo.sh` (profile-guided).
