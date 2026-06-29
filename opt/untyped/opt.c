/* opt.c — clam.c + arity uncurrying + unused-binder elision.
 * ===========================================================================
 *
 * Two compile-time optimizations layered on the clam.c engine, both aimed at
 * the measured #1 cost (one Env cell allocated per beta):
 *
 *  - UNUSED-BINDER ELISION.  If a lambda's variable is never used, applying it
 *    allocates no environment cell *and* does not even evaluate the argument
 *    (it is dead).  Implemented by a usage analysis + a re-indexing pass that
 *    drops elided binders from the runtime de Bruijn numbering; such lambdas
 *    become T_LAMU and their closures carry an "unused" tag bit.
 *
 *  - ARITY UNCURRYING.  A curried call `f a b c` is evaluated by *peeling*
 *    successive lambda binders in local registers, so the intermediate partial
 *    applications are never materialized as closures.  Partial application and
 *    over-application both fall out naturally.
 *
 * Everything else is clam.c.  Note: elision makes evaluation lazier in unused-
 * argument position, so this engine can terminate on terms where strict
 * clam.c/nbe.c diverge; where all terminate they agree.
 *
 * --- original clam.c header ---
 *
 * This is the readable, documented edition of the optimized engine.  It is
 * byte-for-byte equivalent in behaviour and performance to the terse `fast.c`;
 * the only differences are whitespace and comments, which the optimizer
 * discards.  Build it with `build_pgo.sh` to get the `clam.exe` champion.
 *
 * WHAT IT DOES
 *   Reads a lambda term, reduces it to full beta-normal form, prints it.
 *   A normal form shaped like a Church numeral (\f.\x. f (f ... x)) is printed
 *   as "church N"; otherwise the term is printed with generated variable names.
 *
 * HOW IT IS FAST  (each point is explained at its definition below)
 *   1. Normalization by Evaluation: we don't rewrite terms with substitution
 *      (which copies subterms on every step).  Instead we *evaluate* the term
 *      into a domain of closures — a lambda becomes a closure capturing its
 *      environment, and beta-reduction merely extends an environment, so
 *      arguments are shared, never copied.  Then we "read back" (quote) the
 *      resulting value into a normal-form term.
 *   2. de Bruijn indices: variables are integers (distance to their binder),
 *      so there is no alpha-renaming and variable lookup is an array/list walk.
 *   3. Top-level `let`s become GLOBAL definitions: evaluated once, looked up in
 *      O(1), and never re-entered into runtime environments (which keeps the
 *      environment chains short).
 *   4. Tagged-pointer values: a runtime value is a single 64-bit word with a
 *      tag in its low bits, so there is no boxed "value struct" to dereference.
 *   5. A dedicated fixed-size bump allocator: every runtime cell is exactly
 *      16 bytes, so allocation is one pointer increment and one bounds check.
 *   6. An inlined fast path (`eval_atom`) for the common case where a function
 *      argument is a variable / global / lambda, avoiding a real recursive call.
 *   7. Allocation-free neutral "heads": a stuck application like `x a b` is a
 *      cons-list of argument cells ending in an *immediate* head word, so
 *      applying a stuck term costs one allocation and a head costs none.
 *   8. Iterative read-back: deep normal forms (e.g. a 2^25 Church numeral) are
 *      reconstructed with an explicit heap stack, never the C stack.
 *
 * EVALUATION STRATEGY
 *   Arguments are evaluated before substitution (call-by-value in the semantic
 *   `apply`).  This finds the normal form of every strongly-normalizing term
 *   and agrees with a normal-order reducer on those.  It can diverge on the
 *   rare weakly-normalizing term that needs a non-strict argument
 *   (e.g. (\x.\y.y) Omega); use the normal-order `baseline.c` for those.
 *
 * MEMORY
 *   Runtime cells are bump-allocated and never freed (we trade memory for
 *   speed): a long reduction can use many GB.  `gc.c` is a bounded-memory
 *   variant if that matters.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <time.h>

/* Total number of beta-reductions performed — reported as the throughput
 * metric at the end.  A single global counter; measured to be free in the
 * hot loop (the optimizer keeps it in a register across iterations). */
static uint64_t beta_count = 0;

/* ===========================================================================
 * Arena allocator (for immortal data: parsed AST nodes, interned names).
 *
 * A simple growable bump allocator.  Used only during parsing / compilation,
 * never in the hot evaluation loop, so a per-call size-rounding and block
 * check is fine here.
 * ===========================================================================*/
typedef struct Block { struct Block *next; size_t used, cap; char *mem; } Block;
typedef struct { Block *head; } Arena;
static Arena g_arena;

static void *arena_alloc(Arena *a, size_t n) {
    n = (n + 15) & ~(size_t)15;                 /* round up to 16-byte multiple */
    Block *b = a->head;
    if (!b || b->used + n > b->cap) {           /* need a fresh block */
        size_t cap = b ? b->cap * 2 : (size_t)1 << 22;   /* start at 4 MiB, double */
        if (cap < n) cap = n;
        b = malloc(sizeof *b);
        b->mem = malloc(cap);
        b->used = 0; b->cap = cap; b->next = a->head; a->head = b;
    }
    void *p = b->mem + b->used;
    b->used += n;
    return p;
}
#define NEW(T) ((T *)arena_alloc(&g_arena, sizeof(T)))

/* ---------------------------------------------------------------------------
 * Fast fixed-size bump allocators for the hot path.
 *
 * Every *runtime* cell (Env, Clo, AppCell) is exactly 16 bytes, so we can
 * allocate one with a single pointer bump plus one bounds check — no size
 * rounding and no block-list walk.  Blocks are large (1 GiB) and intentionally
 * leaked; we never free during a run.
 * -------------------------------------------------------------------------- */
static char *R_ptr, *R_end;
static void R_grow(void) {
    size_t cap = (size_t)1 << 30;               /* 1 GiB block (leaked) */
    char *m = malloc(cap);
    R_ptr = m; R_end = m + cap;
}
static inline void *ralloc16(void) {
    if (R_ptr >= R_end) R_grow();
    void *p = R_ptr; R_ptr += 16;
    return p;
}

/* A separate 32-byte bump allocator for the Tm nodes produced by read-back.
 * Output trees can be enormous (e.g. a materialized Church numeral), so giving
 * them their own fast allocator keeps that phase cheap. */
static char *TM_ptr, *TM_end;
static void TM_grow(void) {
    size_t cap = (size_t)1 << 30;
    char *m = malloc(cap);
    TM_ptr = m; TM_end = m + cap;
}
static inline void *tmalloc(void) {
    if (TM_ptr >= TM_end) TM_grow();
    void *p = TM_ptr; TM_ptr += 32;
    return p;
}

/* ===========================================================================
 * Named AST — the term as written, before de Bruijn conversion.
 * ===========================================================================*/
typedef enum { N_VAR, N_LAM, N_APP } NTag;
typedef struct NTerm {
    NTag tag;
    union {
        char *var;                                       /* N_VAR */
        struct { char *param; struct NTerm *body; } lam; /* N_LAM */
        struct { struct NTerm *fun, *arg;        } app;  /* N_APP */
    };
} NTerm;

static NTerm *nvar(char *s)            { NTerm *t = NEW(NTerm); t->tag = N_VAR; t->var = s; return t; }
static NTerm *nlam(char *p, NTerm *b)  { NTerm *t = NEW(NTerm); t->tag = N_LAM; t->lam.param = p; t->lam.body = b; return t; }
static NTerm *napp(NTerm *f, NTerm *a) { NTerm *t = NEW(NTerm); t->tag = N_APP; t->app.fun = f; t->app.arg = a; return t; }

/* ===========================================================================
 * Parser.
 *
 *   term  := app
 *   app   := atom atom*                 -- application, left associative
 *   atom  := '\' var+ '.' term          -- lambda (multi-binder sugar)
 *          | 'let' var '=' term 'in' term
 *          | '(' term ')'
 *          | var
 *
 * '#' starts a line comment.  `in` and `let` are keywords.
 * ===========================================================================*/
typedef struct { const char *p; } Parser;

/* Skip whitespace and line comments. */
static void skip(Parser *ps) {
    for (;;) {
        while (isspace((unsigned char)*ps->p)) ps->p++;
        if (*ps->p == '#') { while (*ps->p && *ps->p != '\n') ps->p++; }
        else break;
    }
}
static int is_ident(int c) { return isalnum(c) || c == '_' || c == '\'' || c == '?'; }

/* Read one identifier, copying it into the arena. */
static char *parse_ident(Parser *ps) {
    skip(ps);
    const char *s = ps->p;
    while (is_ident((unsigned char)*ps->p)) ps->p++;
    size_t n = (size_t)(ps->p - s);
    char *r = arena_alloc(&g_arena, n + 1);
    memcpy(r, s, n);
    r[n] = 0;
    return r;
}
static int starts_atom(char c) { return c == '\\' || c == '(' || is_ident((unsigned char)c); }

/* Look ahead: is the next token exactly the keyword `kw`? (does not consume) */
static int peek_kw(Parser *ps, const char *kw) {
    skip(ps);
    size_t n = strlen(kw);
    return strncmp(ps->p, kw, n) == 0 && !is_ident((unsigned char)ps->p[n]);
}

static NTerm *parse_term(Parser *ps);

static NTerm *parse_atom(Parser *ps) {
    skip(ps);
    char c = *ps->p;
    if (c == '(') {                                   /* parenthesized term */
        ps->p++;
        NTerm *t = parse_term(ps);
        skip(ps);
        if (*ps->p == ')') ps->p++;
        return t;
    }
    if (c == '\\') {                                  /* lambda, possibly multi-binder */
        ps->p++;
        char *names[64]; int n = 0;
        for (;;) {
            skip(ps);
            if (*ps->p == '.') { ps->p++; break; }
            names[n++] = parse_ident(ps);
        }
        NTerm *body = parse_term(ps);
        for (int i = n - 1; i >= 0; i--) body = nlam(names[i], body);  /* \x y. e = \x.\y. e */
        return body;
    }
    char *id = parse_ident(ps);
    if (strcmp(id, "let") == 0) {                     /* let x = v in b  ==  (\x. b) v */
        char *name = parse_ident(ps);
        skip(ps); if (*ps->p == '=') ps->p++;
        NTerm *val = parse_term(ps);
        if (peek_kw(ps, "in")) parse_ident(ps);
        NTerm *body = parse_term(ps);
        return napp(nlam(name, body), val);
    }
    return nvar(id);
}

/* An application is a head atom followed by zero or more argument atoms.
 * `in` terminates the surrounding term so `let`s parse correctly. */
static NTerm *parse_term(Parser *ps) {
    NTerm *t = parse_atom(ps);
    for (;;) {
        skip(ps);
        if (!starts_atom(*ps->p)) break;
        if (peek_kw(ps, "in")) break;
        t = napp(t, parse_atom(ps));
    }
    return t;
}

/* ---------------------------------------------------------------------------
 * A program is an outermost chain of `let`s followed by a body.  We peel those
 * leading lets off as GLOBAL definitions instead of desugaring them into
 * runtime lambdas; this lets us evaluate each once and look them up in O(1).
 * -------------------------------------------------------------------------- */
#define MAXG 4096
static char  *g_gname[MAXG];     /* global names, in definition order */
static NTerm *g_gdef[MAXG];      /* their (named-AST) bodies */
static int    g_ndef = 0;

static NTerm *parse_program(Parser *ps) {
    for (;;) {
        if (!peek_kw(ps, "let")) break;
        parse_ident(ps);                              /* consume 'let' */
        char *name = parse_ident(ps);
        skip(ps); if (*ps->p == '=') ps->p++;
        NTerm *val = parse_term(ps);
        if (peek_kw(ps, "in")) parse_ident(ps);
        g_gname[g_ndef] = name; g_gdef[g_ndef] = val; g_ndef++;
    }
    return parse_term(ps);                            /* the body */
}

/* ===========================================================================
 * de Bruijn terms — the compiled form the evaluator runs.
 *
 * A variable is one of:
 *   T_VAR ix   a locally-bound variable, ix = de Bruijn index (0 = innermost)
 *   T_GLB ix   a global definition, ix indexes g_gval[]
 *   T_FREE ix  a truly free variable, ix indexes g_free[] / g_fval[]
 * ===========================================================================*/
/* T_LAM  = lambda whose binder IS used; T_LAMU = binder unused (elided). */
typedef enum { T_VAR, T_GLB, T_FREE, T_LAM, T_LAMU, T_APP } TTag;
typedef struct Tm {
    TTag tag;
    union {
        int ix;                                       /* T_VAR / T_GLB / T_FREE */
        struct { struct Tm *body;        } lam;       /* T_LAM / T_LAMU */
        struct { struct Tm *fun, *arg;   } app;       /* T_APP */
    };
} Tm;

static Tm *tnode(TTag tg, int i) { Tm *t = NEW(Tm); t->tag = tg; t->ix = i; return t; }
static Tm *tlam(Tm *b)           { Tm *t = NEW(Tm); t->tag = T_LAM; t->lam.body = b; return t; }
static Tm *tlamu(Tm *b)          { Tm *t = NEW(Tm); t->tag = T_LAMU; t->lam.body = b; return t; }
static Tm *tapp(Tm *f, Tm *a)    { Tm *t = NEW(Tm); t->tag = T_APP; t->app.fun = f; t->app.arg = a; return t; }

/* Free-variable table: names that are neither bound nor global. */
static char *g_free[1024];
static int   g_nfree = 0;
static int free_index(char *name) {
    for (int i = 0; i < g_nfree; i++)
        if (strcmp(g_free[i], name) == 0) return i;
    g_free[g_nfree] = name;
    return g_nfree++;
}

/* Lexical scope: a stack of bound names, innermost first. */
typedef struct Scope { char *name; struct Scope *next; } Scope;

/* Convert a named term to de Bruijn form.  `nglob` is how many globals are in
 * scope (a definition only sees the globals defined before it). */
static Tm *to_db(NTerm *t, Scope *sc, int nglob) {
    switch (t->tag) {
        case N_VAR: {
            int i = 0;                                /* search local binders first */
            for (Scope *s = sc; s; s = s->next, i++)
                if (strcmp(s->name, t->var) == 0) return tnode(T_VAR, i);
            for (int g = nglob - 1; g >= 0; g--)      /* then globals (later shadows earlier) */
                if (strcmp(g_gname[g], t->var) == 0) return tnode(T_GLB, g);
            return tnode(T_FREE, free_index(t->var)); /* otherwise free */
        }
        case N_LAM: {
            Scope s = { t->lam.param, sc };
            return tlam(to_db(t->lam.body, &s, nglob));
        }
        case N_APP:
            return tapp(to_db(t->app.fun, sc, nglob), to_db(t->app.arg, sc, nglob));
    }
    return NULL;
}

/* ===========================================================================
 * Unused-binder elision: a re-indexing pass over the de Bruijn term.
 *
 * to_db numbers every binder.  But at runtime we only allocate an environment
 * cell for binders that are actually *used*; an unused binder gets no cell, no
 * argument evaluation, and is tagged T_LAMU.  This pass:
 *   1. decides, for each lambda, whether its parameter occurs in the body
 *      (`uses`), and
 *   2. re-numbers every variable so its index counts only the *present* (used)
 *      binders between it and its own binder — matching the runtime Env, where
 *      elided binders contribute nothing.
 *
 * `PScope` is the stack of enclosing binders (innermost first) carrying each
 * one's present/absent decision.
 * ===========================================================================*/

/* Does de Bruijn index `k` (relative to t's top) occur anywhere in `t`? */
static int uses(Tm *t, int k) {
    switch (t->tag) {
        case T_VAR:  return t->ix == k;
        case T_GLB:
        case T_FREE: return 0;
        case T_LAM:
        case T_LAMU: return uses(t->lam.body, k + 1);
        case T_APP:  return uses(t->app.fun, k) || uses(t->app.arg, k);
    }
    return 0;
}

typedef struct PScope { int present; struct PScope *next; } PScope;

static Tm *lower(Tm *t, PScope *ps) {
    switch (t->tag) {
        case T_VAR: {
            /* runtime index = number of present binders strictly inside ours */
            int rt = 0; PScope *s = ps;
            for (int k = 0; k < t->ix; k++) { if (s->present) rt++; s = s->next; }
            return tnode(T_VAR, rt);
        }
        case T_GLB:
        case T_FREE: return tnode(t->tag, t->ix);
        case T_LAM: {
            int used = uses(t->lam.body, 0);
            PScope s = { used, ps };
            Tm *b = lower(t->lam.body, &s);
            return used ? tlam(b) : tlamu(b);
        }
        case T_APP:
            return tapp(lower(t->app.fun, ps), lower(t->app.arg, ps));
        default:
            return t;   /* T_LAMU: to_db never emits it */
    }
}

/* ===========================================================================
 * Runtime values — the semantic domain.
 *
 * A value `V` is a single 64-bit word, tagged in its low bits:
 *
 *   ...000   CLOSURE     pointer to Clo{ Tm *body; Env *env; }
 *   ...001   APP-NEUTRAL pointer to AppCell{ V arg; V next; }
 *   ..x011   HEAD-NEUTRAL  immediate (no allocation):
 *                            bit 3      = 1 if a free variable, 0 if bound
 *                            bits 4..   = free index, or bound de Bruijn LEVEL
 *
 * A "neutral" is a stuck computation: a variable applied to some arguments,
 * e.g. `x a b`.  It is represented as a cons-list of AppCells (one per applied
 * argument) terminating in an immediate head word.  Consequences:
 *   - applying a neutral to one more argument is a single 16-byte allocation;
 *   - a bare head (a variable with no arguments) costs no allocation at all.
 *
 * The low-bit layout is chosen so the hot test is trivial:
 *   isclo(v)  ==  (v & 1) == 0     closures have low bit 0; both neutral kinds
 *                                  have low bit 1.
 * ===========================================================================*/
typedef uint64_t V;
typedef struct Env     { V val; struct Env *next; } Env;       /* environment cons cell */
typedef struct Clo     { Tm *body; Env *env;      } Clo;       /* a closure */
typedef struct AppCell { V arg;  V next;          } AppCell;   /* one neutral spine link */

static inline int isclo(V v)  { return (v & 1) == 0; }         /* closure vs neutral */
static inline int ishead(V v) { return (v & 2) != 0; }         /* (on a neutral) head vs app-cell */

/* A closure has low bit 0.  We steal bit 1 as the "unused-binder" flag: when
 * set, the closure's lambda never references its parameter, so applying it
 * neither evaluates the argument nor allocates an environment cell.  ralloc16
 * is 16-byte aligned, so the low 3 bits are always free; asclo masks them off.
 * (ishead/ascell only ever see neutrals, low bit 1, so bit 1 is unambiguous.) */
static inline V       mkclo(Tm *b, Env *e)   { Clo *c = ralloc16(); c->body = b; c->env = e; return (uintptr_t)c; }
static inline V       mkclou(Tm *b, Env *e)  { Clo *c = ralloc16(); c->body = b; c->env = e; return (uintptr_t)c | 2; }
static inline int     clo_unused(V v)        { return (v & 2) != 0; }
static inline Clo    *asclo(V v)             { return (Clo *)(uintptr_t)(v & ~7ULL); }
static inline AppCell*ascell(V v)            { return (AppCell *)(uintptr_t)(v & ~7ULL); }
static inline V       mkappneu(V arg, V nxt) { AppCell *c = ralloc16(); c->arg = arg; c->next = nxt; return (uintptr_t)c | 1; }

/* Immediate head constructors and accessors. */
#define HEAD_BOUND(lvl)  (((uint64_t)(uint32_t)(lvl)  << 4) | 3ULL)   /* tag 011 */
#define HEAD_FREE(fidx)  (((uint64_t)(uint32_t)(fidx) << 4) | 11ULL)  /* tag 1011 */
static inline int     head_is_free(V v) { return (v & 8) != 0; }
static inline int32_t head_num(V v)     { return (int32_t)(v >> 4); }

/* Evaluated globals and free-variable neutrals, indexed by T_GLB / T_FREE. */
static V *g_gval;
static V *g_fval;

static V eval(Tm *t, Env *env);

/* ---------------------------------------------------------------------------
 * eval_atom: evaluate a non-application inline.
 *
 * Function arguments are very often a variable, global, or lambda.  Handling
 * those here avoids a real recursive `eval` call (and its stack frame) for the
 * common case; only nested applications fall through to the full evaluator.
 * This is one of the largest single speedups in the engine.
 * -------------------------------------------------------------------------- */
static inline V eval_atom(Tm *t, Env *env) {
    switch (t->tag) {
        case T_VAR: { Env *e = env; for (int i = t->ix; i > 0; i--) e = e->next; return e->val; }
        case T_GLB:  return g_gval[t->ix];
        case T_FREE: return g_fval[t->ix];
        case T_LAM:  return mkclo(t->lam.body, env);
        case T_LAMU: return mkclou(t->lam.body, env);
        default:     return eval(t, env);             /* T_APP */
    }
}

/* ---------------------------------------------------------------------------
 * eval: reduce a term to a value (weak-head normal form in the value domain),
 * evaluating arguments strictly.
 *
 * The `for(;;) ... continue;` loop is a manual tail call: when a beta-step
 * sends us to a new (term, env), we loop instead of recursing, so spines of
 * applications and chains of beta-reductions run in constant C-stack space.
 * -------------------------------------------------------------------------- */
static V eval(Tm *t, Env *env) {
    for (;;) {
        switch (t->tag) {
            case T_VAR: { Env *e = env; for (int i = t->ix; i > 0; i--) e = e->next; return e->val; }
            case T_GLB:  return g_gval[t->ix];
            case T_FREE: return g_fval[t->ix];
            case T_LAM:  return mkclo(t->lam.body, env);
            case T_LAMU: return mkclou(t->lam.body, env);
            case T_APP: {
                Tm *fn = t->app.fun;

                /* ---- single application (the hot path, kept exactly lean) ---- */
                if (fn->tag != T_APP) {
                    /* Syntactic redex (\.body) arg: reduce with no closure. */
                    if (fn->tag == T_LAM) {
                        V a = eval_atom(t->app.arg, env);
                        beta_count++;
                        Env *ne = ralloc16(); ne->val = a; ne->next = env;
                        env = ne; t = fn->lam.body;
                        continue;
                    }
                    /* Syntactic redex with an UNUSED binder: drop the argument
                     * unevaluated and add no environment cell. */
                    if (fn->tag == T_LAMU) {
                        beta_count++;
                        t = fn->lam.body;             /* env unchanged */
                        continue;
                    }
                    V fv = eval_atom(fn, env);
                    if (isclo(fv)) {
                        Clo *c = asclo(fv); beta_count++;
                        if (clo_unused(fv)) {         /* argument is dead */
                            env = c->env; t = c->body;
                            continue;
                        }
                        V av = eval_atom(t->app.arg, env);
                        Env *ne = ralloc16(); ne->val = av; ne->next = c->env;
                        env = ne; t = c->body;
                        continue;
                    }
                    V av = eval_atom(t->app.arg, env);
                    return mkappneu(av, fv);
                }

                /* ---- multi-argument spine: arity uncurrying ----
                 * Collect the left spine  ((h a0) a1) ... a(n-1)  and bind the
                 * arguments by *peeling* successive lambda binders in local
                 * registers, so intermediate partial applications are never
                 * built as closures.  Partial- and over-application both fall
                 * out of the loop conditions. */
                Tm *args[64]; int n = 0; Tm *hh = t;
                while (hh->tag == T_APP && n < 64) { args[n++] = hh->app.arg; hh = hh->app.fun; }
                int i = n - 1;                        /* args[n-1] is applied first */

                Tm *body; Env *e; int unused;
                if (hh->tag == T_LAM)       { body = hh->lam.body; e = env; unused = 0; }
                else if (hh->tag == T_LAMU) { body = hh->lam.body; e = env; unused = 1; }
                else {
                    V hv = eval_atom(hh, env);        /* head may still be T_APP if n hit 64 */
                    if (!isclo(hv)) {                 /* stuck head: build a neutral spine */
                        for (; i >= 0; i--) { V av = eval_atom(args[i], env); hv = mkappneu(av, hv); }
                        return hv;
                    }
                    Clo *c = asclo(hv); body = c->body; e = c->env; unused = clo_unused(hv);
                }

                /* Peel binders against the pending arguments. */
                for (;;) {
                    beta_count++;
                    if (!unused) {
                        V av = eval_atom(args[i], env);
                        Env *ne = ralloc16(); ne->val = av; ne->next = e; e = ne;
                    }
                    i--;
                    if (i < 0) { t = body; env = e; break; }   /* fully applied → tail-loop body */
                    if (body->tag == T_LAM)  { body = body->lam.body; unused = 0; continue; }
                    if (body->tag == T_LAMU) { body = body->lam.body; unused = 1; continue; }
                    /* body isn't a lambda but arguments remain: reduce it, then
                     * keep applying the rest to the result. */
                    V hv = eval(body, e);
                    if (!isclo(hv)) {
                        for (; i >= 0; i--) { V av = eval_atom(args[i], env); hv = mkappneu(av, hv); }
                        return hv;
                    }
                    Clo *c = asclo(hv); body = c->body; e = c->env; unused = clo_unused(hv);
                }
                continue;                             /* evaluate the peeled (t, env) */
            }
        }
    }
}

/* Apply an already-evaluated value to an already-evaluated argument.  Used by
 * read-back when it pushes a fresh variable under a binder. */
static V apply(V fv, V av) {
    if (isclo(fv)) {
        Clo *c = asclo(fv); beta_count++;
        if (clo_unused(fv)) return eval(c->body, c->env);   /* arg is dead */
        Env *ne = ralloc16(); ne->val = av; ne->next = c->env;
        return eval(c->body, ne);
    }
    return mkappneu(av, fv);
}

/* ===========================================================================
 * Read-back ("quote"): turn a value back into a normal-form de Bruijn term.
 *
 * To go under a closure we apply it to a fresh neutral variable standing for
 * the new binder (named by its de Bruijn LEVEL = current depth), recurse on the
 * body, and wrap the result in a lambda.  A neutral becomes its head applied to
 * the quoted arguments.
 *
 * This is done iteratively with an explicit heap stack of pending sub-jobs, so
 * a deep result (say a 2^25-node Church numeral, a spine 33M deep) cannot
 * overflow the C stack.  Each job writes its produced node through `slot`, the
 * Tm* location where the parent expects its child.
 * ===========================================================================*/
typedef struct { V v; int depth; Tm **slot; } QTask;

static Tm *quote(V v0, int depth0) {
    Tm *root = NULL;
    size_t cap = 4096, sp = 0;
    QTask *st = malloc(cap * sizeof *st);
    st[sp++] = (QTask){ v0, depth0, &root };

    while (sp) {
        QTask q = st[--sp];
        V v = q.v;

        if (isclo(v)) {
            /* \x. <body>, where x is a fresh variable at level q.depth */
            Tm *lam = tmalloc(); lam->tag = T_LAM; *q.slot = lam;
            V arg = HEAD_BOUND(q.depth);
            if (sp + 1 > cap) { cap *= 2; st = realloc(st, cap * sizeof *st); }
            st[sp++] = (QTask){ apply(v, arg), q.depth + 1, &lam->lam.body };
        } else {
            /* Neutral: rebuild  head a0 a1 ... a(k-1).  The spine list is in
             * reverse (outermost link is the last-applied argument); we thread
             * the function position inward so the head lands innermost. */
            Tm **slot = q.slot;
            V cur = v;
            while (!ishead(cur)) {
                AppCell *c = ascell(cur);
                Tm *app = tmalloc(); app->tag = T_APP; *slot = app;
                if (sp + 1 > cap) { cap *= 2; st = realloc(st, cap * sizeof *st); }
                st[sp++] = (QTask){ c->arg, q.depth, &app->app.arg };
                slot = &app->app.fun;
                cur = c->next;
            }
            /* The head: a free var prints by name (encoded as ix = -(idx+2)),
             * a bound var converts its LEVEL back to an index for this depth. */
            Tm *hd = tmalloc(); hd->tag = T_VAR;
            hd->ix = head_is_free(cur) ? -(head_num(cur) + 2)
                                       : (q.depth - head_num(cur) - 1);
            *slot = hd;
        }
    }
    free(st);
    return root;
}

/* ===========================================================================
 * Output.
 * ===========================================================================*/
/* Print a normal-form term with generated variable names (a, b, c, ...). */
static void print_db(Tm *t, int depth) {
    switch (t->tag) {
        case T_VAR:
            if (t->ix < -1) fputs(g_free[-(t->ix) - 2], stdout);    /* free var: by name */
            else            printf("%c", 'a' + (depth - 1 - t->ix) % 26);
            break;
        case T_LAM:
            printf("\\%c.", 'a' + depth % 26);
            print_db(t->lam.body, depth + 1);
            break;
        case T_APP:
            putchar('('); print_db(t->app.fun, depth); putchar(' ');
            print_db(t->app.arg, depth); putchar(')');
            break;
        default: break;
    }
}

/* If `t` is a Church numeral \f.\x. f (f ... x), return its value, else -1.
 * In de Bruijn, f has index 1 and x has index 0 inside the body. */
static long church_value(Tm *t) {
    if (t->tag != T_LAM) return -1;
    Tm *b = t->lam.body;
    if (b->tag != T_LAM) return -1;
    Tm *body = b->lam.body;
    long n = 0;
    while (body->tag == T_APP) {
        if (body->app.fun->tag != T_VAR || body->app.fun->ix != 1) return -1;
        body = body->app.arg;
        n++;
    }
    return (body->tag == T_VAR && body->ix == 0) ? n : -1;
}

/* ===========================================================================
 * Driver.
 * ===========================================================================*/
static char *read_all(FILE *f) {
    size_t cap = 1 << 16, len = 0;
    char *b = malloc(cap);
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (len + 1 >= cap) { cap *= 2; b = realloc(b, cap); }
        b[len++] = (char)c;
    }
    b[len] = 0;
    return b;
}

int main(int argc, char **argv) {
    FILE *f = stdin;
    if (argc > 1) { f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 1; } }
    char *src = read_all(f);

    /* Parse, then compile globals and body to de Bruijn form. */
    Parser ps = { src };
    NTerm *body = parse_program(&ps);
    Tm **gtm = malloc(sizeof(Tm *) * (g_ndef ? g_ndef : 1));
    for (int i = 0; i < g_ndef; i++) gtm[i] = lower(to_db(g_gdef[i], NULL, i), NULL);
    Tm *btm = lower(to_db(body, NULL, g_ndef), NULL);

    /* Each free variable evaluates to a bare neutral head (no allocation). */
    g_fval = malloc(sizeof(V) * (g_nfree ? g_nfree : 1));
    for (int i = 0; i < g_nfree; i++) g_fval[i] = HEAD_FREE(i);

    /* --- timed region: evaluate globals, evaluate body, read back --- */
    clock_t t0 = clock();
    g_gval = malloc(sizeof(V) * (g_ndef ? g_ndef : 1));
    for (int i = 0; i < g_ndef; i++) g_gval[i] = eval(gtm[i], NULL);
    V v = eval(btm, NULL);
    Tm *nf = quote(v, 0);
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;

    long cv = church_value(nf);
    if (cv >= 0) printf("church %ld\n", cv);
    else { print_db(nf, 0); putchar('\n'); }

    fprintf(stderr, "[clam] %llu beta in %.3fs = %.2f Mβ/s\n",
            (unsigned long long)beta_count, secs,
            secs > 0 ? beta_count / secs / 1e6 : 0.0);
    return 0;
}
