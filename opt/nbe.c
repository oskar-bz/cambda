/* nbe.c — Normalization by Evaluation.
 *
 * The baseline copies whole subterms on every beta-step (capture-avoiding
 * substitution), which is asymptotically terrible.  NbE instead:
 *
 *   - converts the term to de Bruijn indices (no names, no alpha-renaming);
 *   - evaluates into a *semantic domain* of closures: a lambda becomes a
 *     closure capturing its environment, and beta-reduction just extends an
 *     environment instead of rewriting the term.  Arguments are shared, not
 *     copied;
 *   - reads the value back ("quote") into a normal-form term, going under
 *     binders by evaluating the body against a fresh neutral variable.
 *
 * This makes redexes like id^n collapse in O(n) instead of O(n^2).
 *
 * Everything is bump-allocated from arenas; nothing is freed until exit.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <time.h>

static uint64_t beta_count = 0;

/* ============================================================== arena */
typedef struct Block { struct Block *next; size_t used, cap; char *mem; } Block;

typedef struct { Block *head; } Arena;

static Arena g_arena;

static void *arena_alloc(Arena *a, size_t n) {
    n = (n + 15) & ~(size_t)15;
    Block *b = a->head;
    if (!b || b->used + n > b->cap) {
        size_t cap = b ? b->cap * 2 : (size_t)1 << 22;
        if (cap < n) cap = n;
        b = malloc(sizeof *b);
        b->mem = malloc(cap);
        b->used = 0;
        b->cap = cap;
        b->next = a->head;
        a->head = b;
    }
    void *p = b->mem + b->used;
    b->used += n;
    return p;
}
#define NEW(T) ((T *)arena_alloc(&g_arena, sizeof(T)))

/* ============================================================= named AST */
typedef enum { N_VAR, N_LAM, N_APP } NTag;
typedef struct NTerm {
    NTag tag;
    union {
        char *var;
        struct { char *param; struct NTerm *body; } lam;
        struct { struct NTerm *fun, *arg; } app;
    };
} NTerm;

static NTerm *nvar(char *s) { NTerm *t = NEW(NTerm); t->tag = N_VAR; t->var = s; return t; }
static NTerm *nlam(char *p, NTerm *b) { NTerm *t = NEW(NTerm); t->tag = N_LAM; t->lam.param = p; t->lam.body = b; return t; }
static NTerm *napp(NTerm *f, NTerm *a) { NTerm *t = NEW(NTerm); t->tag = N_APP; t->app.fun = f; t->app.arg = a; return t; }

/* ============================================================== parser */
typedef struct { const char *p; } Parser;

static void skip(Parser *ps) {
    for (;;) {
        while (isspace((unsigned char)*ps->p)) ps->p++;
        if (*ps->p == '#') { while (*ps->p && *ps->p != '\n') ps->p++; }
        else break;
    }
}
static int is_ident(int c) { return isalnum(c) || c == '_' || c == '\'' || c == '?'; }

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
static int peek_kw(Parser *ps, const char *kw) {
    skip(ps);
    size_t n = strlen(kw);
    return strncmp(ps->p, kw, n) == 0 && !is_ident((unsigned char)ps->p[n]);
}

static NTerm *parse_term(Parser *ps);

static NTerm *parse_atom(Parser *ps) {
    skip(ps);
    char c = *ps->p;
    if (c == '(') {
        ps->p++;
        NTerm *t = parse_term(ps);
        skip(ps);
        if (*ps->p == ')') ps->p++;
        return t;
    }
    if (c == '\\') {
        ps->p++;
        char *names[64]; int n = 0;
        for (;;) { skip(ps); if (*ps->p == '.') { ps->p++; break; } names[n++] = parse_ident(ps); }
        NTerm *body = parse_term(ps);
        for (int i = n - 1; i >= 0; i--) body = nlam(names[i], body);
        return body;
    }
    char *id = parse_ident(ps);
    if (strcmp(id, "let") == 0) {
        char *name = parse_ident(ps);
        skip(ps); if (*ps->p == '=') ps->p++;
        NTerm *val = parse_term(ps);
        if (peek_kw(ps, "in")) parse_ident(ps);
        NTerm *body = parse_term(ps);
        return napp(nlam(name, body), val);
    }
    return nvar(id);
}
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

/* ====================================================== de Bruijn terms */
typedef enum { T_VAR, T_LAM, T_APP } TTag;
typedef struct Tm {
    TTag tag;
    union {
        int ix;                                   /* T_VAR: de Bruijn index */
        struct { struct Tm *body; } lam;
        struct { struct Tm *fun, *arg; } app;
    };
} Tm;

static Tm *tvar(int i) { Tm *t = NEW(Tm); t->tag = T_VAR; t->ix = i; return t; }
static Tm *tlam(Tm *b) { Tm *t = NEW(Tm); t->tag = T_LAM; t->lam.body = b; return t; }
static Tm *tapp(Tm *f, Tm *a) { Tm *t = NEW(Tm); t->tag = T_APP; t->app.fun = f; t->app.arg = a; return t; }

/* free-variable context: free vars become indices just outside all binders */
static char *g_free[256];
static int   g_nfree = 0;

/* scope: linked list of bound names, innermost first */
typedef struct Scope { char *name; struct Scope *next; } Scope;

static int free_index(char *name) {
    for (int i = 0; i < g_nfree; i++)
        if (strcmp(g_free[i], name) == 0) return i;
    g_free[g_nfree] = name;
    return g_nfree++;
}

static Tm *to_db(NTerm *t, Scope *sc, int depth) {
    switch (t->tag) {
        case N_VAR: {
            int i = 0;
            for (Scope *s = sc; s; s = s->next, i++)
                if (strcmp(s->name, t->var) == 0) return tvar(i);
            /* free variable: index = depth + slot */
            return tvar(depth + free_index(t->var));
        }
        case N_LAM: {
            Scope s = { t->lam.param, sc };
            return tlam(to_db(t->lam.body, &s, depth + 1));
        }
        case N_APP:
            return tapp(to_db(t->app.fun, sc, depth), to_db(t->app.arg, sc, depth));
    }
    return NULL;
}

/* ============================================================ semantics */
typedef struct Val Val;
typedef struct Env { Val *val; struct Env *next; } Env;
typedef struct Spine { Val *val; struct Spine *next; } Spine;  /* reversed: head=last arg */

typedef enum { V_LAM, V_NE } VTag;
struct Val {
    VTag tag;
    union {
        struct { Tm *body; Env *env; } lam;
        struct { int lvl; char *name; Spine *sp; } ne;  /* name!=NULL => free */
    };
};

static Env *cons(Val *v, Env *e) { Env *n = NEW(Env); n->val = v; n->next = e; return n; }

static Val *vlam(Tm *body, Env *env) {
    Val *v = NEW(Val); v->tag = V_LAM; v->lam.body = body; v->lam.env = env; return v;
}
static Val *vne(int lvl, char *name, Spine *sp) {
    Val *v = NEW(Val); v->tag = V_NE; v->ne.lvl = lvl; v->ne.name = name; v->ne.sp = sp; return v;
}

static Val *eval(Tm *t, Env *env);

static Val *apply(Val *f, Val *a) {
    if (f->tag == V_LAM) {
        beta_count++;
        return eval(f->lam.body, cons(a, f->lam.env));
    }
    /* neutral: extend spine */
    Spine *s = NEW(Spine); s->val = a; s->next = f->ne.sp;
    return vne(f->ne.lvl, f->ne.name, s);
}

static Val *eval(Tm *t, Env *env) {
    for (;;) {
        switch (t->tag) {
            case T_VAR: {
                Env *e = env;
                for (int i = t->ix; i > 0; i--) e = e->next;
                return e->val;
            }
            case T_LAM:
                return vlam(t->lam.body, env);
            case T_APP: {
                Val *f = eval(t->app.fun, env);
                Val *a = eval(t->app.arg, env);
                if (f->tag == V_LAM) {           /* tail-call the body */
                    beta_count++;
                    env = cons(a, f->lam.env);
                    t = f->lam.body;
                    continue;
                }
                Spine *s = NEW(Spine); s->val = a; s->next = f->ne.sp;
                return vne(f->ne.lvl, f->ne.name, s);
            }
        }
    }
}

/* readback (quote) a value into a normal-form de Bruijn term.
 *
 * Iterative with an explicit heap stack so that deep normal forms (e.g. a
 * Church numeral 2^25, a right-nested spine 33M deep) don't blow the C stack.
 * Result nodes come from the arena; no per-node malloc.
 *
 * Free-variable heads are encoded as T_VAR ix = -(slot+2) so the printer can
 * recover the name; bound neutral heads convert level -> index. */
typedef struct { Val *v; int depth; Tm **slot; } QTask;

static Tm *quote(Val *v0, int depth0) {
    Tm *root = NULL;
    size_t cap = 4096, sp = 0;
    QTask *st = malloc(cap * sizeof *st);
    st[sp++] = (QTask){ v0, depth0, &root };

    while (sp) {
        QTask q = st[--sp];
        Val *v = q.v;
        if (v->tag == V_LAM) {
            Val *arg = vne(q.depth, NULL, NULL);        /* fresh neutral var */
            Tm *lam = NEW(Tm); lam->tag = T_LAM;
            *q.slot = lam;
            if (sp + 1 > cap) { cap *= 2; st = realloc(st, cap * sizeof *st); }
            st[sp++] = (QTask){ apply(v, arg), q.depth + 1, &lam->lam.body };
        } else {
            /* neutral: build  head a0 a1 ... a_{n-1}  from the reversed spine.
             * Spine head = last arg; create outer apps first, threading the
             * function position inward until it lands on the head var. */
            Tm **slot = q.slot;
            for (Spine *s = v->ne.sp; s; s = s->next) {
                Tm *app = NEW(Tm); app->tag = T_APP;
                *slot = app;
                if (sp + 1 > cap) { cap *= 2; st = realloc(st, cap * sizeof *st); }
                st[sp++] = (QTask){ s->val, q.depth, &app->app.arg };
                slot = &app->app.fun;
            }
            if (v->ne.name) *slot = tvar(-(free_index(v->ne.name) + 2));
            else            *slot = tvar(q.depth - v->ne.lvl - 1);
        }
    }
    free(st);
    return root;
}

/* =========================================================== printing */
static void print_db(Tm *t, int depth) {
    switch (t->tag) {
        case T_VAR:
            if (t->ix < -1) fputs(g_free[-(t->ix) - 2], stdout);
            else printf("%c", 'a' + (depth - 1 - t->ix) % 26);
            break;
        case T_LAM:
            printf("\\%c.", 'a' + depth % 26);
            print_db(t->lam.body, depth + 1);
            break;
        case T_APP:
            putchar('(');
            print_db(t->app.fun, depth);
            putchar(' ');
            print_db(t->app.arg, depth);
            putchar(')');
            break;
    }
}

/* church numeral detection on de Bruijn normal form: \.\. 1 (1 (... 0)) */
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
    if (body->tag == T_VAR && body->ix == 0) return n;
    return -1;
}

/* =============================================================== main */
static char *read_all(FILE *f) {
    size_t cap = 1 << 16, len = 0;
    char *buf = malloc(cap);
    int c;
    while ((c = fgetc(f)) != EOF) { if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); } buf[len++] = (char)c; }
    buf[len] = 0;
    return buf;
}

int main(int argc, char **argv) {
    FILE *f = stdin;
    if (argc > 1) { f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 1; } }
    char *src = read_all(f);
    Parser ps = { src };
    NTerm *named = parse_term(&ps);
    Tm *t = to_db(named, NULL, 0);

    /* initial environment: free vars as neutrals, outermost-first => tail */
    Env *env = NULL;
    for (int i = g_nfree - 1; i >= 0; i--)
        env = cons(vne(0, g_free[i], NULL), env);

    clock_t t0 = clock();
    Val *v = eval(t, env);
    Tm *nf = quote(v, 0);
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;

    long cv = church_value(nf);
    if (cv >= 0) printf("church %ld\n", cv);
    else { print_db(nf, 0); putchar('\n'); }
    fprintf(stderr, "[nbe] %llu beta in %.3fs = %.2f Mβ/s\n",
            (unsigned long long)beta_count, secs,
            secs > 0 ? beta_count / secs / 1e6 : 0.0);
    return 0;
}
