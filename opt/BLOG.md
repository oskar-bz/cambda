# Writing a fast lambda calculus evaluator in C

*A build log: from a textbook substitution interpreter to a tree-walker that
normalizes on the order of 10⁸ β-reductions per second — and an honest account
of everything that didn't work.*

This is a long, technical post. It starts from the very beginning (what lambda
calculus *is*) so it is self-contained, then walks the optimization journey one
measurement at a time. Every speedup is justified, every regression is
recorded, and the dead ends get as much space as the wins — because the dead
ends are where most of the learning is.

The code lives in a handful of single-file C programs built with `clang`:

| file         | what it is                                                       |
|--------------|------------------------------------------------------------------|
| `baseline.c` | Readable reference: capture-avoiding substitution, normal order. |
| `nbe.c`      | Readable Normalization-by-Evaluation (closures + de Bruijn).     |
| `clam.c`     | The optimized engine — the champion (`clam.exe`).                |
| `jit.c`      | Bytecode compiler + threaded VM — a tuned interpreter that *lost*.|
| `gc.c`       | A copying garbage collector — works, slower, rejected.           |
| `opt.c`      | Unused-binder elision + arity uncurrying — correct, and *also* lost.|

---

## Part 0 — What problem are we even solving?

### Lambda calculus in five minutes

The untyped lambda calculus is the smallest interesting programming language.
There are exactly three kinds of term:

```
t  ::=  x            -- a variable
     |  \x. t        -- a lambda abstraction  (an anonymous one-argument function)
     |  t t          -- an application        (call a function on an argument)
```

That's it. No numbers, no booleans, no loops, no `if`. And yet it is
Turing-complete: every computation can be expressed by these three constructs.
`\x. t` is a function of one argument `x` whose body is `t`; `f a` applies `f`
to `a`. Application associates to the left, so `f a b` means `(f a) b` — call
`f` on `a`, then call the result on `b`. The lambda body extends as far right as
possible, so `\x. f x` means `\x. (f x)`, not `(\x. f) x`.

The single rule of computation is **β-reduction**: applying a lambda to an
argument substitutes the argument for the bound variable in the body.

```
(\x. t) a   →β   t[x := a]
```

A subterm of the shape `(\x. t) a` is called a **redex** (reducible
expression). To "run" a program is to keep reducing redexes until there are
none left. A term with no redexes is in **normal form** — it is the answer.

Two wrinkles make this subtler than it looks.

**Wrinkle 1: variable capture.** Substitution must not let a free variable get
accidentally bound. Consider `(\x. \y. x) y`. Naively substituting gives
`\y. y` — the identity function — but that's wrong: the outer `y` we passed in
was a *free* variable, unrelated to the inner binder `y`. The correct answer
keeps them distinct, e.g. `\y'. y`. Substitution that respects this is called
*capture-avoiding*, and it requires renaming bound variables (**α-conversion**:
`\y. x` and `\z. x` are "the same" term up to renaming). Getting this right by
hand is fiddly and a classic source of interpreter bugs.

**Wrinkle 2: evaluation order.** If a term has several redexes, which do you
reduce first? Two famous strategies:

- **Normal order** (leftmost-outermost first): reduce the outermost redex
  before its arguments. This is the strategy that *always* finds the normal
  form if one exists (it is *normalizing*). It corresponds to call-by-name /
  lazy-ish evaluation.
- **Applicative order** (leftmost-innermost first): reduce arguments to normal
  form before substituting. This is call-by-value. It can loop forever on terms
  that normal order would finish, because it insists on evaluating an argument
  that the function was going to throw away.

The classic example: let `Ω = (\x. x x)(\x. x x)`, which β-reduces to itself
forever. Then `(\a. \b. b) Ω y` has normal form `y` under normal order (the `Ω`
is in a discarded argument position and never gets touched), but loops forever
under naive applicative order. Hold onto this example — it comes back at the
very end, because one of our optimizations turns out to recover exactly this
behavior.

### Full normalization, not just "running"

Most language implementations only need **weak** normal forms: they evaluate a
program to a value and stop. They never reduce *inside* the body of a function,
because you can't print a function anyway. We want something stronger: **full
β-normal form**, reducing even under lambda binders, because we want to compare
and print the actual normal-form term. This is what a proof assistant's
conversion checker does, and it is strictly harder than evaluation — it's where
all the interesting performance problems live.

### Church encodings: how we get numbers out of nothing

Since the calculus has no numbers, we encode them. A **Church numeral** is "apply
a function `n` times":

```
0  =  \f. \x. x
1  =  \f. \x. f x
2  =  \f. \x. f (f x)
n  =  \f. \x. f^n x
```

On this encoding arithmetic is just lambda terms:

```
succ  =  \n. \f. \x. f (n f x)
plus  =  \m. \n. \f. \x. m f (n f x)
mul   =  \m. \n. \f. m (n f)
exp   =  \m. \n. n m            -- n applied to m: m^n
```

These give us real, scalable workloads. `exp two twentyfive` is the Church
numeral 2²⁵ ≈ 33 million — a normal form with ~33M nodes, which is a great
stress test for "building a huge result." And because a normal form of shape
`\f. \x. f (f ... x)` is recognizable, the evaluator can print it as
`church N` and we can check answers trivially (`fib 25` had better come out
`church 75025`).

The benchmark suite is built from these:

- **`collapse`** — `exp two N id y`, i.e. apply the identity function 2ᴺ times
  to `y`. The normal form is just `y`, but reaching it takes 2ᴺ β-reductions.
  This is a pure *reduction-throughput* benchmark: a tight loop of trivial
  redexes, almost no allocation of result structure.
- **`pow`** — `exp two N`, building the actual 2ᴺ-node Church numeral. This
  stresses *result construction* (read-back / allocation), not just reduction.
- **`fib`** — Fibonacci via a Church-encoded recursion (a Z-combinator fixpoint
  with Church arithmetic and predecessor). This is the "realistic" workload:
  deep recursion, lots of intermediate closures, branchy control.

All throughput numbers below are millions of β-reductions per second
(**Mβ/s**), best of three on one machine (Windows, `clang`/LLVM with
`-O3 -march=native`, `lld`). Absolute numbers are hardware-specific; the
*ratios* and *trends* are the point.

---

## Part 1 — The baseline: substitution, the way the textbook says

`baseline.c` is the obviously-correct reference — the oracle every later engine
is checked against. It represents terms as an AST with string variable names and
implements exactly the textbook rules: normal-order reduction to full normal
form, with capture-avoiding substitution via fresh-variable renaming.

```
Term  ::= Var name | Lam name body | App fun arg
```

The reducer repeatedly finds the leftmost-outermost redex and fires it; `subst`
walks the term replacing the bound variable, generating fresh names whenever it
would otherwise capture. It is maybe 150 lines, it is easy to believe, and it
gives the right answers. As a specification it is perfect.

As an *implementation* it is a disaster, for one fundamental reason:

> **Every β-step copies a subterm.**

When you reduce `(\x. body) arg`, substitution walks `body` and splices in a
(copied) `arg` at each occurrence of `x`. If `x` occurs twice, `arg` is
duplicated — and now there are two copies to reduce, independently, later. Work
that should have been shared is instead replicated. The result is not a constant
factor; it is an **asymptotic** blow-up. Terms that should reduce in O(n) steps
take O(n²) or worse, and each step is itself an O(size) tree copy.

You can watch it happen. The `collapse` benchmark applies `id` 2ᴺ times:

| workload (size)    | baseline |
|--------------------|---------:|
| `collapse15` (2¹⁵) |  1.13 s  |
| `collapse18` (2¹⁸) |  9.76 s  |

Tripling the exponent (8× the reductions) takes ~9× the time, and it only gets
worse. `collapse25`, which the fast engine finishes in a fraction of a second,
is simply not attainable with the baseline — you'd wait for hours. The baseline
isn't slow by a constant we can tune away; it is computing in the wrong
complexity class. **No amount of micro-optimization fixes an algorithm that
copies on every step.** We need a fundamentally different evaluation model.

---

## Part 2 — The asymptotic fix: Normalization by Evaluation

The key realization: substitution is the enemy because it *eagerly copies*.
What if arguments were **shared** instead of copied, and only "substituted"
lazily, by remembering what each variable points to?

That is exactly what an **environment** gives you, and the technique that uses
environments to compute *full* normal forms is **Normalization by Evaluation
(NbE)**. `nbe.c` is the readable implementation. It has three ideas.

### Idea 1 — de Bruijn indices kill α-renaming

Instead of names, a variable is a number: how many binders out you have to count
to reach the one that binds it. `\x. \y. x` becomes `\. \. 1` (the `x` is bound
two binders out, so index 1 counting from 0). `\x. \y. y` becomes `\. \. 0`.

Why this matters: **there are no names, so there is no capture, so there is no
α-renaming.** The single most error-prone part of the baseline simply
evaporates. Two α-equivalent terms become *literally identical* data. Variable
lookup becomes "walk `n` steps down a list."

The conversion from named syntax to de Bruijn (`to_db`) happens once, up front,
during compilation. A small refinement we add immediately: **top-level `let`s
become globals.** A program is usually a pile of definitions (`let id = ...`,
`let plus = ...`) followed by a body. Rather than desugaring each `let` into a
runtime `(\x. body) value` redex, we lift them out as global definitions,
evaluate each once, and look them up in O(1). This also keeps runtime
environment chains short — they only hold genuinely local bindings.

### Idea 2 — evaluate into closures; β just extends an environment

We *evaluate* the term into a semantic domain of **values**. A value is either:

- a **closure**: a lambda body paired with the environment it captured, or
- a **neutral**: a stuck computation — a variable (or free variable) applied to
  zero or more value arguments, e.g. `x a b`. Neutrals are what you get when
  there is no function to call yet (the head is a variable), which is exactly
  what happens when you reduce *under* a binder.

The evaluator is the textbook NbE `eval(term, env)`:

- a variable indexes into the environment;
- a lambda becomes a closure capturing the current environment — **no traversal,
  no copy**, just a pointer to the body and a pointer to the env;
- an application evaluates the function; if it's a closure, we β-reduce by
  **extending the environment** with the argument value and evaluating the body;
  if it's a neutral, we extend its spine.

The crucial line is "β-reduction extends an environment." There is no
substitution and no copying. If an argument is used twice, both uses look up the
*same* value in the environment — sharing, for free. This is what restores the
right complexity class. `id` applied 2ᴺ times collapses in O(2ᴺ) total work
instead of O(4ᴺ).

### Idea 3 — read back ("quote") the value into a normal form

A value isn't a term — a closure is an opaque function. To produce a printable
normal form we **read it back** (the NbE "quote" / "reify" step). To quote a
closure `\. body`, we invent a fresh neutral variable standing for the binder,
apply the closure to it, recursively quote the result, and wrap it in a lambda.
To quote a neutral, we quote its head and all its argument values. Going under
the binder is exactly how NbE reaches full normal form: we reduce inside lambda
bodies by feeding them symbolic variables.

Fresh variables are named by their **de Bruijn level** (depth from the outside),
which is converted back to an index at the end — levels are stable as you go
deeper, indices are not, so using levels internally avoids a reindexing headache.

### The result

NbE is the asymptotic win. The `collapse` benchmark that the baseline couldn't
finish now runs in milliseconds, and throughput is suddenly a meaningful
concept:

| workload     | nbe (Mβ/s) |
|--------------|-----------:|
| `collapse25` |    ~130    |
| `fib`        |    ~102    |
| `pow25`      |     ~37    |

### One bug worth recording: the recursive quote stack overflow

The natural way to write `quote` is recursively. It works fine — until you quote
a deep result like `pow25`, whose normal form is a spine 33 million nodes deep.
Recursive quote means 33M nested C calls, and the C stack overflows long before
that. The fix is to make **quote iterative**, with an explicit heap-allocated
work stack of "quote this value into this slot" tasks. This is the first
appearance of a recurring theme: *deep lambda terms must never ride the C
stack.* It will come back when we discuss why beating the tree-walker is hard.

---

## Part 3 — `clam.c`: squeezing the constant factor

NbE fixed the algorithm. Everything from here is constant-factor work: the same
number of β-reductions, fewer nanoseconds each. The discipline of this phase was
**measure, change one thing, measure again, keep it only if it helped.** That
discipline mattered, because intuition was wrong about as often as it was right.

### Profiling first: this workload is compute-bound, not memory-bound

Before optimizing, find out what you're actually spending. The engine
bump-allocates cells and never frees them, so a long run touches a *lot* of
memory — `fib` allocated around **6.6 GB** of cells in one measurement. The
obvious hypothesis is "we're memory-bandwidth bound; cut allocation and we win."

That hypothesis is **wrong**, and proving it wrong saved a lot of wasted effort.
Two pieces of evidence:

1. Instruction-level accounting showed roughly ~18 cycles of *compute* per
   β-reduction — allocation, the dispatch, the environment walk, the call
   overhead. That's the budget. The arithmetic intensity is high enough that the
   cell stores are not the bottleneck.
2. In *strict* evaluation, freshly allocated cells are written, read back almost
   immediately while still cache-hot, and then *never read again* (dead cells
   are abandoned, not revisited). So the 6.6 GB is mostly write-once,
   read-once-hot traffic that the store buffer and L1/L2 absorb. The DRAM
   footprint is huge but the *working set* is tiny.

The conclusion shaped everything that followed: **the win is cutting
instructions per β, not cutting bytes per β.** (This is also why, much later,
adding a garbage collector — which trades memory for *more* instructions —
backfired. Foreshadowing.)

So the targets are: shrink the per-β instruction count in `eval`, `apply`, and
allocation; remove indirections; remove real function calls. Here is each
optimization, in roughly the order it landed, with why it works.

### 3.1 — Tagged-pointer values (a value is one 64-bit word)

The readable NbE used a tagged `struct Value { enum tag; union {...} }`, which
is 24–32 bytes and forces a pointer-dereference-then-load on every value
inspection. We collapse a value to a single `uint64_t` with the tag living in
the **low bits** of a pointer (everything is at least 8-byte aligned, so the low
3 bits are always free):

```
...000   CLOSURE      pointer to Clo{ Tm *body; Env *env; }
...001   APP-NEUTRAL  pointer to AppCell{ V arg; V next; }
..x011   HEAD-NEUTRAL immediate, no allocation (see 3.4)
```

The hot discriminator becomes a single bit test:

```c
static inline int isclo(V v) { return (v & 1) == 0; }   // closure vs neutral
```

No boxed value struct, no extra load to find the tag, and a value fits in a
register and a single 8-byte store. This shrinks both the instruction count and
the memory traffic of every single value that flows through the engine.

### 3.2 — One uniform 16-byte cell + an inlined bump allocator

The three runtime cell types — environment links (`Env{val,next}`), closures
(`Clo{body,env}`), and neutral spine links (`AppCell{arg,next}`) — are each
*exactly two words = 16 bytes*. That is not a coincidence; it is engineered.
Because every runtime cell is the same size, allocation needs no size argument,
no size class, no rounding, no free-list. It is a pointer bump and a bounds
check, fully inlined:

```c
static inline void *ralloc16(void) {
    if (R_ptr >= R_end) R_grow();      // R_grow allocates a fresh 1 GiB block
    void *p = R_ptr; R_ptr += 16;
    return p;
}
```

`R_grow` grabs a 1 GiB block and we never free (memory-for-speed; `gc.c` is the
bounded-memory alternative, and it's slower — see the dead ends). Allocation,
the single most frequent operation in the engine, is now about three
instructions. Read-back's output `Tm` nodes get their own analogous bump
allocator so that phase is just as cheap.

### 3.3 — `eval_atom`: the inlined fast path *(the single biggest win)*

Profiling the application case showed most arguments are *trivial*: a variable,
a global, or a literal lambda. Evaluating those through the full recursive
`eval` — with its `switch`, its loop, and a real call frame — is wasteful. So we
split out `eval_atom`, an inlined fast path for exactly the non-application
cases:

```c
static inline V eval_atom(Tm *t, Env *env) {
    switch (t->tag) {
        case T_VAR:  { Env *e = env; for (int i = t->ix; i > 0; i--) e = e->next; return e->val; }
        case T_GLB:   return g_gval[t->ix];
        case T_FREE:  return g_fval[t->ix];
        case T_LAM:   return mkclo(t->lam.body, env);
        default:      return eval(t, env);   // only nested applications recurse
    }
}
```

In the application case of `eval`, both the function and the argument are
evaluated with `eval_atom`, so the overwhelmingly common "apply something to a
variable/global/lambda" costs no call and no second trip through the dispatch
loop. This was **the largest single constant-factor speedup** in the whole
project — easily tens of percent. The lesson: in a tree-walker, the cost isn't
just the work, it's the *plumbing* (calls, frames, dispatch) around small work.
Inline the common case and the plumbing disappears.

### 3.4 — Allocation-free neutral heads + one-cell application

A neutral is a variable applied to arguments: `x a b`. We represent it as a
cons-list of `AppCell{arg, next}` links ending in an **immediate head word** —
the head isn't a pointer to a cell, it's encoded *directly* in the value word:

```c
#define HEAD_BOUND(lvl)  (((uint64_t)(uint32_t)(lvl)  << 4) | 3ULL)   // tag 011
#define HEAD_FREE(fidx)  (((uint64_t)(uint32_t)(fidx) << 4) | 11ULL)  // tag 1011
```

Two consequences, both good:

- A **bare variable** (a head with no arguments) costs **zero allocation** — it's
  an immediate value. Free variables and the fresh variables quote injects under
  binders are therefore free to make.
- Applying a neutral to one more argument is **exactly one 16-byte cell**
  (`mkappneu`): cons the new arg onto the spine. No copying, no reallocation.

Since going under binders during read-back creates a fresh neutral head at every
level, and stuck computations are everywhere in a normalizer, making heads free
and spine-extension a single allocation is a broad, steady win.

### 3.5 — Manual tail-loop in `eval` (don't ride the C stack)

`eval` is structured as `for (;;) switch(...)`. When a β-step or a redex
peephole sends evaluation to a new `(term, env)`, we **reassign `t` and `env`
and `continue` the loop** instead of recursing:

```c
if (fn->tag == T_LAM) {                  // (\.body) arg — a syntactic redex
    V a = eval_atom(t->app.arg, env);
    beta_count++;
    Env *ne = ralloc16(); ne->val = a; ne->next = env;
    env = ne; t = fn->lam.body;          // tail-loop into the body
    continue;
}
```

This turns chains of β-reductions and left-nested application spines into a flat
loop in constant C-stack space. It also exposes a nice peephole: a *syntactic*
redex `(\. body) arg` reduces without ever building a closure for the lambda —
we know statically it's about to be applied, so we skip straight to extending
the environment. Strict argument evaluation (call-by-value in the semantic
`apply`) is what makes this manual TCO valid and keeps the machine simple.

### 3.6 — Profile-Guided Optimization

The `eval` loop is one big branchy `switch` whose branch probabilities the
compiler cannot guess. PGO measures them. The build (`build_pgo.sh`) compiles an
instrumented binary, runs it on representative workloads (`fib`, `collapse25`,
`pow25`), merges the profiles, and recompiles using them. The compiler then lays
out the hot paths to fall through, predicts the common tags, and inlines along
the hot edges. This was a clean **~15–30%** on top of everything else, for zero
source changes. (Toolchain notes from the trenches: LLVM bitcode LTO would not
link against the MSVC linker — `LNK1107` — so the build uses `-fuse-ld=lld`; and
a 256 MB linker stack reservation is needed because, even with iterative quote,
deep strict evaluation chains want headroom.)

### Where that leaves `clam.c`

| workload     | baseline | nbe  | **clam** |
|--------------|---------:|-----:|---------:|
| `collapse25` |    —     | ~130 | **~264–308** |
| `fib`        |    —     | ~102 | **~167–207** |
| `pow25`      |    —     |  ~37 | **~61–76**  |

Versus the substitution baseline on a size it can still finish, the wall-clock
speedup is **~440×** on `collapse18` — and it grows without bound with input
size, because we changed complexity class *and then* hammered the constant.

A note on what each benchmark reveals: `collapse` is fastest per-β (tiny
redexes, almost no result allocation), `pow` is slowest (every β contributes a
node to a giant output that must be allocated and read back), and `fib` sits in
between (real recursion, lots of short-lived closures). The spread tells you the
costs are exactly where the model predicts: allocation and read-back, not
dispatch.

---

## Part 4 — The bytecode VM (`jit.c`): a well-tuned interpreter that lost

The tree-walker dispatches on AST node tags as it traverses. A classic next step
is to **compile** the term to a flat bytecode for an abstract machine and run
*that*, on the theory that a linear instruction stream with specialized opcodes
beats re-walking a tree. `jit.c` does this: it compiles to a strict abstract
machine in the ZINC/ZAM family (the lineage behind OCaml's bytecode), with a
value stack, a return stack of `(pc, env)` frames, and closures that store a
code address instead of an AST pointer.

It was optimized iteratively, and each stage genuinely helped:

| stage                    | fib | collapse25 | pow25 (Mβ/s) |
|--------------------------|----:|-----------:|-------------:|
| switch dispatch          | 120 |    152     |     48       |
| computed-goto threading  | 153 |    180     |     61       |
| + super-instructions     | 173 |    200     |     64       |
| + `APP2` two-operand op  | 181 |    208     |     67       |
| **`clam.c` tree-walker** | **196** | **308** | **70**   |

The intermediate steps are textbook interpreter craft:

- **switch → computed-goto threading.** A `switch` in a dispatch loop is one
  indirect branch that the CPU mispredicts constantly because the next opcode is
  unrelated to this one. *Threaded* dispatch (GCC/Clang `&&label` computed
  `goto`, one jump table indexed at the *end* of each handler) gives each opcode
  its own dispatch site, so the branch predictor can learn per-opcode
  correlations. Big jump: fib 120 → 153.
- **super-instructions.** Fuse common adjacent pairs — `PUSH` with its operand,
  `APPLY` with the following op — into single opcodes, cutting dispatches and
  stack traffic. fib 153 → 173.
- **`APP2`, a two-operand op** for the very common `atom atom` application: read
  both operands inline, never touch the value stack. fib 173 → 181.

And yet, fully threaded and super-instructed, **the bytecode VM does not beat
the PGO tree-walker.** This was an honest negative result, and the reasons were
predicted by the Part-3 profiling:

1. **Every opcode is an indirect dispatch.** Threading makes that branch as
   predictable as it can be, but the tree-walker, compiled and PGO'd, is just
   *straight-line native code* for the hot cases — no dispatch at all. You can't
   out-predict "no branch."
2. **The VM threads control through an explicit return stack; the tree-walker
   uses native C recursion.** And native recursion is *not* the slow option
   here: the CPU's return-address predictor handles call/return essentially for
   free, whereas the VM's manual `(pc, env)` push/pop is real loads, stores, and
   a data-dependent indirect jump on return. The `collapse` benchmark — a tight
   loop of trivial applications — exposes this most brutally (308 vs 208): it is
   almost entirely dispatch + return-stack churn, precisely the overhead native
   recursion avoids.

The takeaway: **interpretation has a floor, and a PGO-compiled tree-walker is
already sitting near it.** To go faster you must remove interpretation
*entirely* — emit native machine code — not build a faster interpreter. That's a
much larger undertaking (and brings its own deep-stack problem: a native code
generator for this needs the application spine turned into a trampolined loop so
strict chains don't overflow the hardware stack). The bytecode VM is the tuned
ceiling of the *interpreted* approach, and that ceiling is below the tree-walker.

---

## Part 5 — The dead ends (where the real learning is)

Three serious, fully-implemented ideas that made things *worse*. Each is kept in
the tree as a documented negative result, because "we tried it and measured it"
is more valuable than "we assumed it would help."

### 5.1 — Spine collection

Idea: instead of handling one application at a time, gather a whole application
spine `(((f a) b) c)` into a buffer, then apply the arguments in a tight loop.
Sounds like it should cut overhead.

Result: **fib 174 → 123.** A large regression. Reverted.

Why it lost: the existing single-application path is *already* extremely lean —
`eval_atom` + a tail-loop `continue`, a handful of instructions. Collecting a
spine adds a buffer, a length, a gather loop, and bookkeeping *in front of* that
lean path, and most spines in real terms are short (length 1–2). You pay the
collection cost on every application to occasionally save on a long one that
rarely occurs. The fast path was fast precisely because it had no preamble;
adding a preamble is pure loss. **Lesson: do not put setup work in front of an
already-minimal hot path.**

### 5.2 — Copying garbage collector (`gc.c`)

Idea: the engine leaks gigabytes; a Cheney-style semispace copying collector
would bound memory and (the hope) improve cache behavior by compacting live
data.

Result: it *works* and it crushes memory — **6.6 GB → 143 MB** — but it is
**much slower** (fib ~167 → ~100; pow ~60 → ~16). Kept as a documented dead end
and the basis for a future memory-constrained mode.

Why it lost — and this is the payoff of the Part-3 profiling: **the workload was
never memory-bandwidth bound.** A copying GC trades memory for *instructions*:
you must reify every GC root (which forces an explicit-stack evaluator, giving up
the native-recursion advantage from Part 4), trace, and *re-copy the entire live
set* on every collection. The cache benefit it offers is small because dead
cells were already never re-read and live cells were already cache-hot. So you
pay a large instruction tax for a bandwidth benefit you didn't need. **Lesson:
optimize the resource you're actually bound on; profiling told us it was
compute, and GC optimizes the other axis.**

### 5.3 — Unused-binder elision + arity uncurrying (`opt.c`)

This was the most sophisticated attempt, and the most instructive failure,
because the optimizations are *real* and *sound* — they just don't pay off on
these workloads.

**Unused-binder elision.** If a lambda never uses its parameter (think `\f.\x. x`
— Church zero ignores `f`; or `\a.\b. b` — `K`-like combinators), then applying
it should (a) allocate no environment cell and (b) not even *evaluate* the
argument, since the result can't depend on it. Implementation:

- a usage analysis `uses(term, k)` decides, per lambda, whether the bound
  variable actually occurs in the body;
- a re-indexing pass `lower` rewrites the de Bruijn term so indices count only
  the *present* (used) binders — because at runtime, elided binders contribute
  no environment cell, so the numbering must skip them;
- unused lambdas get a distinct tag `T_LAMU`, and their closures carry an
  "unused" flag in **bit 1** of the value word (free, since closures have low
  bit 0 and `ralloc16` is 16-byte aligned). When `eval`/`apply` see the flag,
  they skip both argument evaluation and the environment-cell allocation.

**Arity uncurrying.** A curried call `f a b c` is `((f a) b) c`, and the naive
evaluation materializes an intermediate closure for each partial application
(`f a` → a closure, then applied to `b` → another closure, …). Uncurrying
collects the spine and **peels successive lambda binders in local registers**,
binding `a`, `b`, `c` in one go without ever building the intermediate closures.
Partial application (too few args → build one closure for the remainder) and
over-application (too many args → apply the leftovers to the result) both fall
out of the loop conditions naturally. Critically — *learning from 5.1* — this is
done **only** for genuine multi-argument spines (`fn` is itself an application);
the single-application hot path is left exactly as it was, with no preamble.

It is **correct**: 20/20 against the NbE reference. And it still **lost** on the
throughput suite:

| workload     | clam | **opt** | betas (clam → opt) |
|--------------|-----:|--------:|--------------------|
| `fib`        | ~204 | **~187**| 278,946,043 → 278,431,867 |
| `collapse25` | ~248 | **~241**| 67,108,901 → 67,108,901 (identical) |
| `pow25`      | ~72  | **~65** | 33,554,469 → 33,554,469 (identical) |

The beta counts explain everything. On `collapse` and `pow` the counts are
*identical* — there are no unused binders and no uncurryable spines to exploit,
so elision and uncurrying never fire, and all you're left with is the **extra
branches they added to the hot loop** (the `T_LAMU` check, the `clo_unused`
test, the single-vs-multi application split). On `fib`, elision *does* fire, but
saves a microscopic 0.18% of β-reductions — nowhere near enough to pay for the
added per-β branching. Net: 3–8% slower everywhere. Same shape of lesson as 5.1:
**branches added to a compute-bound inner loop must be paid on every iteration,
but only earn their keep on iterations where they fire — and here they almost
never fire.**

But there's a twist, and it's why `opt.c` is kept rather than deleted: elision
isn't *only* a speed play, it's a **change in evaluation semantics**. By not
evaluating dead arguments, it makes the engine non-strict in
discarded-argument position — recovering the normal-order behavior from Part 0.
Where a dead argument is *expensive*, this is an unbounded win:

```
let twentyfive = mul five five in
let k = \a.\b. b in
k (exp two twentyfive id y) y          -- the first argument is discarded
```

The discarded argument `exp two twentyfive id y` costs 2²⁵ β-reductions to
evaluate. The strict champion dutifully computes it and throws it away; `opt`
never touches it:

```
clam (strict):   67,108,903 beta   in 0.216 s
opt  (elides):            4 beta   in ~0 s
```

And on the truly weakly-normalizing `(\a.\b. b) Ω y`, `clam` diverges while
`opt` returns `y`. So `opt.c` isn't a failed optimization so much as a different
*point in the design space*: it trades a few percent of raw throughput on
strict, fully-used workloads for non-strict, more-terminating behavior on dead
arguments. For the project's stated goal — maximum β-reduction throughput — it
loses, and `clam.c` stays champion. As a semantics, it's strictly more useful.

---

## Part 6 — What it would take to actually go faster

Having exhausted the constant-factor wins on the tree-walker and confirmed the
interpreted approaches are slower, the honest ceiling-raising options are all
large:

- **Native code generation.** Stop interpreting; emit machine code per function
  body, with the application spine compiled to a trampolined loop so strict
  chains can't overflow the hardware stack. This is the only thing that removes
  the dispatch and call overhead the tree-walker still pays. It's a real
  compiler backend — weeks of work and a lot of new ways to be wrong.
- **Interaction nets / optimal reduction** (Lamping's algorithm, the lineage
  behind HVM). A completely different model that shares reduction work *optimally*
  and is naturally parallel. It can be asymptotically better than NbE on terms
  with heavy sharing — but it carries the "bookkeeping" overhead of fan
  nodes/croissants, which can lose badly on terms that *don't* need the sharing
  (much like our spine-collection and elision findings: machinery you pay for
  whether or not it fires). Whether it wins is extremely workload-dependent.
- **Parallelism.** Fork-join on independent strict subterms. The evaluation model
  is already strict and largely allocation-local, so independent arguments could
  be reduced on separate cores. The hard parts are the usual ones: identifying
  genuinely independent work cheaply, and a parallel allocator that doesn't
  reintroduce the bandwidth problem we carefully avoided.

The through-line of the whole project: **a PGO-compiled, carefully-laid-out tree
walker with a one-word value representation, a uniform 16-byte cell, an inlined
fast path, and free neutral heads is a remarkably strong baseline** — strong
enough that a tuned bytecode VM, a garbage collector, spine collection, and two
genuinely clever compile-time optimizations all failed to beat it. The wins came
from changing the algorithm (substitution → NbE) and then relentlessly cutting
*instructions per β*; everything that instead added instructions to the inner
loop — however clever — lost, and the profiler said it would.

---

## Appendix — reproducing the numbers

```
bash build.sh      baseline.c baseline.exe   # plain -O3
bash build.sh      nbe.c      nbe.exe
bash build_pgo.sh  clam.c     clam.exe        # the champion (PGO)
bash build_pgo.sh  jit.c      jit.exe         # bytecode VM
bash build_pgo.sh  opt.c      opt.exe         # elision + uncurrying variant
bash check.sh                                 # clam vs nbe, 20/20 correctness
bash bench.sh      ./clam.exe                  # throughput suite
```

Requires `clang` with `lld` and `llvm-profdata`. The engine prints its β-count,
wall time, and Mβ/s to stderr; a Church-numeral normal form is printed as
`church N`. Throughput is hardware-specific — measure on your own machine; the
ratios are what carry over.
