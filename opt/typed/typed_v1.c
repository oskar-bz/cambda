/* typed.c — a typed lambda-calculus interpreter.
 *
 * Type system: Hindley-Milner (let-polymorphism) with full type inference.
 *   - Algorithm W with destructive unification (union-find on mutable tyvars).
 *   - Efficient generalization via Remy/Kiselyov *levels*: no environment scan.
 *   - Turing-complete: `let rec` / recursive bindings (no totality restriction).
 *   - No annotations required anywhere; `\x:T. e` annotations are optional.
 *
 * Evaluation: types erase. A strict (CBV) environment-closure machine evaluates
 * the underlying untyped term. Recursion is a self-referential closure.
 *
 * Build:  clang -O2 -o typed.exe typed.c     (or: gcc -O2 -o typed typed.c)
 * Run:    typed.exe file.tl                  (run a file of statements)
 *         typed.exe                          (REPL)
 *
 * Surface syntax:
 *   e ::= \x y. e | \x:T. e        lambda (curried, optional annotations)
 *       | let x = e in e           polymorphic let
 *       | let rec f = e in e       recursive let
 *       | if e then e else e
 *       | e e                      application
 *       | e + e | e - e | e * e    arithmetic
 *       | e == e | e < e | ...     comparison (on Int) -> Bool
 *       | n | true | false | x | ( e )
 *   T ::= Int | Bool | T -> T | ( T )
 *   Top-level statements: `let [rec] x = e`  or a bare expression. `--` comments.
 */

#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include <time.h>

/* ------------------------------------------------------------------ errors */
static jmp_buf g_jmp;
static char    g_err[512];

static void fail(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(g_err, sizeof g_err, fmt, ap);
    va_end(ap);
    longjmp(g_jmp, 1);
}

static void *xmalloc(size_t n) { void *p = malloc(n); if (!p) { perror("malloc"); exit(1); } return p; }
static char *xstrdup(const char *s) { char *p = xmalloc(strlen(s) + 1); strcpy(p, s); return p; }

/* ======================================================================== */
/*  TYPES                                                                    */
/* ======================================================================== */
/* A type is a mutable graph node. T_VAR is a unification variable: when
 * `link` is non-NULL it has been solved (union-find), otherwise it is unbound
 * and carries a `level` used for generalization. T_GEN is a quantified
 * variable appearing only inside a generalized scheme. */

typedef enum { T_VAR, T_GEN, T_ARROW, T_CON } TKind;

typedef struct Type Type;
struct Type {
    TKind kind;
    union {
        struct { Type *link; int id; int level; } var;  /* T_VAR */
        int gen_id;                                      /* T_GEN */
        struct { Type *from, *to; } arrow;               /* T_ARROW */
        const char *con;                                 /* T_CON  */
    };
};

static int g_var_id   = 0;   /* fresh tyvar names           */
static int g_level    = 1;   /* current generalization level */

static Type *mk(TKind k) { Type *t = xmalloc(sizeof *t); t->kind = k; return t; }

static Type *fresh_var(void) {
    Type *t = mk(T_VAR);
    t->var.link = NULL; t->var.id = g_var_id++; t->var.level = g_level;
    return t;
}
static Type *mk_arrow(Type *a, Type *b) { Type *t = mk(T_ARROW); t->arrow.from = a; t->arrow.to = b; return t; }
static Type *mk_con(const char *c)      { Type *t = mk(T_CON);   t->con = c; return t; }
static Type *mk_gen(int id)             { Type *t = mk(T_GEN);   t->gen_id = id; return t; }

static Type *T_INT, *T_BOOL;

/* Follow links (union-find find), with path compression. */
static Type *prune(Type *t) {
    if (t->kind == T_VAR && t->var.link) {
        t->var.link = prune(t->var.link);
        return t->var.link;
    }
    return t;
}

static void enter_level(void) { g_level++; }
static void exit_level(void)  { g_level--; }

/* ------------------------------------------------------- unification ----- */

/* Occurs check + level adjustment: when we are about to bind `var`, scan `t`.
 * If `var` itself appears -> infinite type. Any unbound var deeper than `var`
 * must be lowered to var's level so it is not wrongly generalized later. */
static void occurs_adjust(Type *var, Type *t) {
    t = prune(t);
    if (t->kind == T_VAR) {
        if (t == var) fail("occurs check: cannot construct the infinite type");
        if (t->var.level > var->var.level) t->var.level = var->var.level;
    } else if (t->kind == T_ARROW) {
        occurs_adjust(var, t->arrow.from);
        occurs_adjust(var, t->arrow.to);
    }
}

static char *show(Type *t);  /* fwd */

static void unify(Type *a, Type *b) {
    a = prune(a); b = prune(b);
    if (a == b) return;
    if (a->kind == T_VAR) { occurs_adjust(a, b); a->var.link = b; return; }
    if (b->kind == T_VAR) { occurs_adjust(b, a); b->var.link = a; return; }
    if (a->kind == T_ARROW && b->kind == T_ARROW) {
        unify(a->arrow.from, b->arrow.from);
        unify(a->arrow.to,   b->arrow.to);
        return;
    }
    if (a->kind == T_CON && b->kind == T_CON && strcmp(a->con, b->con) == 0) return;
    /* show() uses rotating buffers so both calls in one fail() are valid. */
    char *sa = show(a), *sb = show(b);
    fail("type mismatch: cannot unify %s with %s", sa, sb);
}

/* ------------------------------------------------- generalize / instantiate */

/* Generalize: turn every unbound var deeper than the current level into a
 * quantified T_GEN. Levels make this O(size of type), not O(env). */
static Type *generalize(Type *t) {
    t = prune(t);
    switch (t->kind) {
        case T_VAR:
            return (t->var.level > g_level) ? mk_gen(t->var.id) : t;
        case T_ARROW:
            return mk_arrow(generalize(t->arrow.from), generalize(t->arrow.to));
        default:
            return t;
    }
}

/* tiny gen_id -> fresh-var map for one instantiation */
typedef struct { int id[128]; Type *v[128]; int n; } IMap;

static Type *imap_get(IMap *m, int id) {
    for (int i = 0; i < m->n; i++) if (m->id[i] == id) return m->v[i];
    if (m->n == 128) fail("too many quantified type variables");
    Type *fv = fresh_var();
    m->id[m->n] = id; m->v[m->n] = fv; m->n++;
    return fv;
}

/* Instantiate a scheme: replace each T_GEN by a fresh var at the current level. */
static Type *inst_rec(Type *t, IMap *m) {
    t = prune(t);
    switch (t->kind) {
        case T_GEN:   return imap_get(m, t->gen_id);
        case T_ARROW: return mk_arrow(inst_rec(t->arrow.from, m), inst_rec(t->arrow.to, m));
        default:      return t;  /* T_CON, or unbound T_VAR shared as-is */
    }
}
static Type *instantiate(Type *scheme) { IMap m = { .n = 0 }; return inst_rec(scheme, &m); }

/* --------------------------------------------------------- pretty-print --- */
/* Name unbound vars by pointer identity and quantified vars by gen_id, both
 * drawing from one shared letter sequence so `a -> a` prints consistently
 * even though generalize() makes a fresh T_GEN node per occurrence. */
typedef struct { void *keys[128]; int n; } NameCtx;

static int name_index(NameCtx *nc, void *key) {
    for (int i = 0; i < nc->n; i++) if (nc->keys[i] == key) return i;
    nc->keys[nc->n] = key;
    return nc->n++;
}

static void emit_name(char **p, char *end, int idx) {
    char b[8];
    if (idx < 26) { b[0] = (char)('a' + idx); b[1] = 0; }
    else snprintf(b, sizeof b, "t%d", idx);
    char *s = b;
    while (*s && *p < end - 1) *(*p)++ = *s++;
}

static void emit(char **p, char *end, const char *s) {
    while (*s && *p < end - 1) *(*p)++ = *s++;
}

/* prec: 0 = top, 1 = right of arrow ok, 2 = left of arrow (atoms only) */
static void show_rec(Type *t, NameCtx *nc, char **p, char *end, int prec) {
    t = prune(t);
    switch (t->kind) {
        case T_CON: emit(p, end, t->con); break;
        case T_VAR: emit_name(p, end, name_index(nc, t)); break;
        case T_GEN: /* key by gen_id (offset to never collide with pointers) */
            emit_name(p, end, name_index(nc, (void *)(intptr_t)(t->gen_id + 1))); break;
        case T_ARROW:
            if (prec >= 2) emit(p, end, "(");
            show_rec(t->arrow.from, nc, p, end, 2);
            emit(p, end, " -> ");
            show_rec(t->arrow.to, nc, p, end, 1);
            if (prec >= 2) emit(p, end, ")");
            break;
    }
}

static char *show(Type *t) {
    static char bufs[4][512];
    static int  which = 0;
    char *buf = bufs[which++ & 3];
    NameCtx nc = { .n = 0 };
    char *p = buf, *end = buf + 512;
    show_rec(t, &nc, &p, end, 0);
    *p = 0;
    return buf;
}

/* ======================================================================== */
/*  AST                                                                      */
/* ======================================================================== */
typedef enum { E_VAR, E_INT, E_BOOL, E_LAM, E_APP, E_LET, E_IF } EKind;

typedef struct Expr Expr;
struct Expr {
    EKind kind;
    union {
        char *var;
        long  i;
        int   b;
        struct { char *param; Type *ann; Expr *body; } lam;   /* ann may be NULL */
        struct { Expr *fn, *arg; } app;
        struct { int rec; char *name; Expr *rhs, *body; } let;
        struct { Expr *c, *t, *e; } iff;
    };
};

static Expr *E(EKind k) { Expr *e = xmalloc(sizeof *e); e->kind = k; return e; }
static Expr *e_var(char *n)  { Expr *e = E(E_VAR);  e->var = n; return e; }
static Expr *e_int(long n)   { Expr *e = E(E_INT);  e->i = n; return e; }
static Expr *e_bool(int b)   { Expr *e = E(E_BOOL); e->b = b; return e; }
static Expr *e_app(Expr *f, Expr *a) { Expr *e = E(E_APP); e->app.fn = f; e->app.arg = a; return e; }
static Expr *e_binop(const char *op, Expr *l, Expr *r) {
    return e_app(e_app(e_var(xstrdup(op)), l), r);
}

/* ======================================================================== */
/*  LEXER                                                                    */
/* ======================================================================== */
typedef enum {
    TK_EOF, TK_INT, TK_IDENT,
    TK_LET, TK_REC, TK_IN, TK_IF, TK_THEN, TK_ELSE, TK_TRUE, TK_FALSE,
    TK_LAMBDA, TK_DOT, TK_LPAREN, TK_RPAREN, TK_EQ, TK_COLON, TK_ARROW,
    TK_SEMI, TK_OP
} TKtype;

typedef struct { TKtype kind; char text[64]; long ival; int bol; } Tok;

static const char *g_src;
static int         g_pos;
static Tok         cur;

static int kw(const char *s, TKtype *out) {
    if (!strcmp(s, "let"))   { *out = TK_LET;   return 1; }
    if (!strcmp(s, "rec"))   { *out = TK_REC;   return 1; }
    if (!strcmp(s, "in"))    { *out = TK_IN;    return 1; }
    if (!strcmp(s, "if"))    { *out = TK_IF;    return 1; }
    if (!strcmp(s, "then"))  { *out = TK_THEN;  return 1; }
    if (!strcmp(s, "else"))  { *out = TK_ELSE;  return 1; }
    if (!strcmp(s, "true"))  { *out = TK_TRUE;  return 1; }
    if (!strcmp(s, "false")) { *out = TK_FALSE; return 1; }
    return 0;
}

static void advance(void) {
    const char *s = g_src;
    int saw_nl = 0;
    /* skip whitespace and -- line comments, remembering newlines */
    for (;;) {
        char c = s[g_pos];
        if (c == '\n') { saw_nl = 1; g_pos++; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { g_pos++; continue; }
        if (c == '-' && s[g_pos + 1] == '-') { while (s[g_pos] && s[g_pos] != '\n') g_pos++; continue; }
        break;
    }
    cur.bol = saw_nl;
    char c = s[g_pos];
    if (c == 0) { cur.kind = TK_EOF; return; }

    if (c >= '0' && c <= '9') {
        long v = 0;
        while (s[g_pos] >= '0' && s[g_pos] <= '9') v = v * 10 + (s[g_pos++] - '0');
        cur.kind = TK_INT; cur.ival = v; return;
    }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
        int n = 0;
        while (((c = s[g_pos]) >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_') {
            if (n < 63) cur.text[n++] = c;
            g_pos++;
        }
        cur.text[n] = 0;
        TKtype k;
        if (kw(cur.text, &k)) cur.kind = k; else cur.kind = TK_IDENT;
        return;
    }
    g_pos++;
    switch (c) {
        case '\\': cur.kind = TK_LAMBDA; return;
        case '.':  cur.kind = TK_DOT;    return;
        case '(':  cur.kind = TK_LPAREN; return;
        case ')':  cur.kind = TK_RPAREN; return;
        case ':':  cur.kind = TK_COLON;  return;
        case ';':  cur.kind = TK_SEMI;   return;
        case '+': case '*':
            cur.kind = TK_OP; cur.text[0] = c; cur.text[1] = 0; return;
        case '=':
            if (s[g_pos] == '=') { g_pos++; cur.kind = TK_OP; strcpy(cur.text, "=="); }
            else cur.kind = TK_EQ;
            return;
        case '-':
            if (s[g_pos] == '>') { g_pos++; cur.kind = TK_ARROW; }
            else { cur.kind = TK_OP; cur.text[0] = '-'; cur.text[1] = 0; }
            return;
        case '<':
            cur.kind = TK_OP;
            if (s[g_pos] == '=') { g_pos++; strcpy(cur.text, "<="); } else strcpy(cur.text, "<");
            return;
        case '>':
            cur.kind = TK_OP;
            if (s[g_pos] == '=') { g_pos++; strcpy(cur.text, ">="); } else strcpy(cur.text, ">");
            return;
    }
    fail("unexpected character '%c'", c);
}

static void expect(TKtype k, const char *what) {
    if (cur.kind != k) fail("parse error: expected %s", what);
    advance();
}

/* ======================================================================== */
/*  PARSER  (recursive descent)                                              */
/* ======================================================================== */
static Expr *parse_expr(void);

static Type *parse_type(void);
static Type *parse_type_atom(void) {
    if (cur.kind == TK_LPAREN) { advance(); Type *t = parse_type(); expect(TK_RPAREN, ")"); return t; }
    if (cur.kind == TK_IDENT) {
        if (!strcmp(cur.text, "Int"))  { advance(); return T_INT;  }
        if (!strcmp(cur.text, "Bool")) { advance(); return T_BOOL; }
        fail("unknown type '%s' (only Int, Bool, -> are supported in annotations)", cur.text);
    }
    fail("parse error: expected a type");
    return NULL;
}
static Type *parse_type(void) {
    Type *a = parse_type_atom();
    if (cur.kind == TK_ARROW) { advance(); return mk_arrow(a, parse_type()); }
    return a;
}

static int starts_atom(void) {
    /* A newline ends a statement: never extend an application onto a new line. */
    if (cur.bol) return 0;
    switch (cur.kind) {
        case TK_INT: case TK_IDENT: case TK_TRUE: case TK_FALSE: case TK_LPAREN: return 1;
        default: return 0;
    }
}

static Expr *parse_atom(void) {
    switch (cur.kind) {
        case TK_INT:   { Expr *e = e_int(cur.ival); advance(); return e; }
        case TK_TRUE:  { advance(); return e_bool(1); }
        case TK_FALSE: { advance(); return e_bool(0); }
        case TK_IDENT: { Expr *e = e_var(xstrdup(cur.text)); advance(); return e; }
        case TK_LPAREN:{ advance(); Expr *e = parse_expr(); expect(TK_RPAREN, ")"); return e; }
        default: fail("parse error: expected an expression"); return NULL;
    }
}

static Expr *parse_app(void) {
    Expr *e = parse_atom();
    while (starts_atom()) e = e_app(e, parse_atom());
    return e;
}

/* precedence: mul (*)  >  add (+ -)  >  cmp (== < <= > >=) */
static Expr *parse_mul(void) {
    Expr *e = parse_app();
    while (cur.kind == TK_OP && !strcmp(cur.text, "*")) { advance(); e = e_binop("*", e, parse_app()); }
    return e;
}
static Expr *parse_add(void) {
    Expr *e = parse_mul();
    while (cur.kind == TK_OP && (!strcmp(cur.text, "+") || !strcmp(cur.text, "-"))) {
        char op[3]; strcpy(op, cur.text); advance(); e = e_binop(op, e, parse_mul());
    }
    return e;
}
static Expr *parse_cmp(void) {
    Expr *e = parse_add();
    while (cur.kind == TK_OP &&
           (!strcmp(cur.text, "==") || !strcmp(cur.text, "<") || !strcmp(cur.text, "<=") ||
            !strcmp(cur.text, ">")  || !strcmp(cur.text, ">="))) {
        char op[3]; strcpy(op, cur.text); advance(); e = e_binop(op, e, parse_add());
    }
    return e;
}

static Expr *parse_lambda(void) {
    expect(TK_LAMBDA, "\\");
    /* collect params (name, optional :type) */
    char  *names[32]; Type *anns[32]; int n = 0;
    while (cur.kind == TK_IDENT) {
        if (n == 32) fail("too many lambda parameters");
        names[n] = xstrdup(cur.text); advance();
        if (cur.kind == TK_COLON) { advance(); anns[n] = parse_type(); }
        else anns[n] = NULL;
        n++;
    }
    if (n == 0) fail("parse error: lambda needs at least one parameter");
    expect(TK_DOT, ".");
    Expr *body = parse_expr();
    for (int i = n - 1; i >= 0; i--) {
        Expr *l = E(E_LAM);
        l->lam.param = names[i]; l->lam.ann = anns[i]; l->lam.body = body;
        body = l;
    }
    return body;
}

static Expr *parse_if(void) {
    expect(TK_IF, "if");
    Expr *c = parse_expr();
    expect(TK_THEN, "then"); Expr *t = parse_expr();
    expect(TK_ELSE, "else"); Expr *e = parse_expr();
    Expr *r = E(E_IF); r->iff.c = c; r->iff.t = t; r->iff.e = e; return r;
}

static Expr *parse_let_expr(void) {
    expect(TK_LET, "let");
    int rec = 0;
    if (cur.kind == TK_REC) { rec = 1; advance(); }
    if (cur.kind != TK_IDENT) fail("parse error: expected name after let");
    char *name = xstrdup(cur.text); advance();
    expect(TK_EQ, "=");
    Expr *rhs = parse_expr();
    expect(TK_IN, "in");
    Expr *body = parse_expr();
    Expr *e = E(E_LET);
    e->let.rec = rec; e->let.name = name; e->let.rhs = rhs; e->let.body = body;
    return e;
}

static Expr *parse_expr(void) {
    switch (cur.kind) {
        case TK_LAMBDA: return parse_lambda();
        case TK_IF:     return parse_if();
        case TK_LET:    return parse_let_expr();
        default:        return parse_cmp();
    }
}

/* ======================================================================== */
/*  TYPE INFERENCE  (Algorithm W)                                            */
/* ======================================================================== */
typedef struct TEnv { char *name; Type *scheme; struct TEnv *next; } TEnv;

static TEnv *tenv_ext(TEnv *e, char *name, Type *scheme) {
    TEnv *n = xmalloc(sizeof *n); n->name = name; n->scheme = scheme; n->next = e; return n;
}
static Type *tenv_get(TEnv *e, const char *name) {
    for (; e; e = e->next) if (!strcmp(e->name, name)) return e->scheme;
    return NULL;
}

static Type *infer(TEnv *env, Expr *e) {
    switch (e->kind) {
        case E_INT:  return T_INT;
        case E_BOOL: return T_BOOL;
        case E_VAR: {
            Type *s = tenv_get(env, e->var);
            if (!s) fail("unbound variable: %s", e->var);
            return instantiate(s);
        }
        case E_LAM: {
            Type *pt = e->lam.ann ? e->lam.ann : fresh_var();
            TEnv *env2 = tenv_ext(env, e->lam.param, pt);
            Type *rt = infer(env2, e->lam.body);
            return mk_arrow(pt, rt);
        }
        case E_APP: {
            Type *tf = infer(env, e->app.fn);
            Type *ta = infer(env, e->app.arg);
            Type *tr = fresh_var();
            unify(tf, mk_arrow(ta, tr));
            return tr;
        }
        case E_IF: {
            unify(infer(env, e->iff.c), T_BOOL);
            Type *tt = infer(env, e->iff.t);
            Type *te = infer(env, e->iff.e);
            unify(tt, te);
            return tt;
        }
        case E_LET: {
            Type *t1;
            enter_level();
            if (e->let.rec) {
                Type *tv = fresh_var();
                TEnv *env1 = tenv_ext(env, e->let.name, tv);
                t1 = infer(env1, e->let.rhs);
                unify(tv, t1);
            } else {
                t1 = infer(env, e->let.rhs);
            }
            exit_level();
            Type *scheme = generalize(t1);
            TEnv *env2 = tenv_ext(env, e->let.name, scheme);
            return infer(env2, e->let.body);
        }
    }
    fail("internal: bad expr kind");
    return NULL;
}

/* ======================================================================== */
/*  EVALUATOR  (strict, environment-closure; types are erased)               */
/* ======================================================================== */
typedef enum { OP_ADD, OP_SUB, OP_MUL, OP_EQ, OP_LT, OP_LE, OP_GT, OP_GE } Op;

typedef struct Value Value;
typedef struct VEnv { char *name; Value *val; struct VEnv *next; } VEnv;

typedef enum { V_INT, V_BOOL, V_CLO, V_PRIM } VKind;
struct Value {
    VKind kind;
    union {
        long i;
        int  b;
        struct { char *param; Expr *body; VEnv *env; } clo;
        struct { Op op; int got; long a0; } prim;
    };
};

static Value *v_int(long i)  { Value *v = xmalloc(sizeof *v); v->kind = V_INT;  v->i = i; return v; }
static Value *v_bool(int b)  { Value *v = xmalloc(sizeof *v); v->kind = V_BOOL; v->b = b; return v; }
static Value *v_prim(Op op)  { Value *v = xmalloc(sizeof *v); v->kind = V_PRIM; v->prim.op = op; v->prim.got = 0; v->prim.a0 = 0; return v; }

static VEnv *venv_ext(VEnv *e, char *name, Value *val) {
    VEnv *n = xmalloc(sizeof *n); n->name = name; n->val = val; n->next = e; return n;
}
static Value *venv_get(VEnv *e, const char *name) {
    for (; e; e = e->next) if (!strcmp(e->name, name)) return e->val;
    return NULL;
}

static Value *eval(VEnv *env, Expr *e);

static Value *apply(Value *f, Value *a) {
    if (f->kind == V_CLO)
        return eval(venv_ext(f->clo.env, f->clo.param, a), f->clo.body);
    if (f->kind == V_PRIM) {
        if (f->prim.got == 0) {
            Value *v = v_prim(f->prim.op);
            v->prim.got = 1; v->prim.a0 = a->i;
            return v;
        }
        long x = f->prim.a0, y = a->i;
        switch (f->prim.op) {
            case OP_ADD: return v_int(x + y);
            case OP_SUB: return v_int(x - y);
            case OP_MUL: return v_int(x * y);
            case OP_EQ:  return v_bool(x == y);
            case OP_LT:  return v_bool(x <  y);
            case OP_LE:  return v_bool(x <= y);
            case OP_GT:  return v_bool(x >  y);
            case OP_GE:  return v_bool(x >= y);
        }
    }
    fail("runtime: applied a non-function");
    return NULL;
}

static Value *eval(VEnv *env, Expr *e) {
    switch (e->kind) {
        case E_INT:  return v_int(e->i);
        case E_BOOL: return v_bool(e->b);
        case E_VAR: {
            Value *v = venv_get(env, e->var);
            if (!v) fail("runtime: uninitialized recursive value '%s'", e->var);
            return v;
        }
        case E_LAM: {
            Value *v = xmalloc(sizeof *v);
            v->kind = V_CLO; v->clo.param = e->lam.param; v->clo.body = e->lam.body; v->clo.env = env;
            return v;
        }
        case E_APP: {
            Value *f = eval(env, e->app.fn);
            Value *a = eval(env, e->app.arg);
            return apply(f, a);
        }
        case E_IF: {
            Value *c = eval(env, e->iff.c);
            return eval(env, c->b ? e->iff.t : e->iff.e);
        }
        case E_LET: {
            if (e->let.rec) {
                VEnv *node = venv_ext(env, e->let.name, NULL);  /* placeholder */
                node->val = eval(node, e->let.rhs);             /* closure captures `node` */
                return eval(node, e->let.body);
            }
            Value *rv = eval(env, e->let.rhs);
            return eval(venv_ext(env, e->let.name, rv), e->let.body);
        }
    }
    fail("internal: bad expr kind");
    return NULL;
}

static void print_value(Value *v) {
    switch (v->kind) {
        case V_INT:  printf("%ld", v->i); break;
        case V_BOOL: printf("%s", v->b ? "true" : "false"); break;
        case V_CLO:  case V_PRIM: printf("<function>"); break;
    }
}

/* ======================================================================== */
/*  INITIAL ENVIRONMENTS  (primitive operators)                              */
/* ======================================================================== */
static TEnv *g_tenv;   /* top-level type env  */
static VEnv *g_venv;   /* top-level value env */

static void def_prim(const char *name, Type *ty, Op op) {
    g_tenv = tenv_ext(g_tenv, xstrdup(name), ty);
    g_venv = venv_ext(g_venv, xstrdup(name), v_prim(op));
}

static void init_globals(void) {
    T_INT  = mk_con("Int");
    T_BOOL = mk_con("Bool");
    Type *iii = mk_arrow(T_INT, mk_arrow(T_INT, T_INT));   /* Int -> Int -> Int  */
    Type *iib = mk_arrow(T_INT, mk_arrow(T_INT, T_BOOL));  /* Int -> Int -> Bool */
    def_prim("+",  iii, OP_ADD);
    def_prim("-",  iii, OP_SUB);
    def_prim("*",  iii, OP_MUL);
    def_prim("==", iib, OP_EQ);
    def_prim("<",  iib, OP_LT);
    def_prim("<=", iib, OP_LE);
    def_prim(">",  iib, OP_GT);
    def_prim(">=", iib, OP_GE);
}

/* ======================================================================== */
/*  STATEMENTS & DRIVER                                                      */
/* ======================================================================== */
/* A statement is either a top-level binding `let [rec] x = e`  (no `in`),
 * or a bare expression. We detect the difference after parsing the rhs. */

/* infer at a fresh level so the result type can be generalized for display */
static Type *infer_top(Expr *e) {
    enter_level();
    Type *t = infer(g_tenv, e);
    exit_level();
    return generalize(t);
}

static void run_binding(int rec, char *name, Expr *rhs) {
    /* type */
    Type *scheme;
    enter_level();
    Type *t1;
    if (rec) {
        Type *tv = fresh_var();
        TEnv *env1 = tenv_ext(g_tenv, name, tv);
        t1 = infer(env1, rhs);
        unify(tv, t1);
    } else {
        t1 = infer(g_tenv, rhs);
    }
    exit_level();
    scheme = generalize(t1);
    g_tenv = tenv_ext(g_tenv, name, scheme);
    /* value */
    Value *val;
    if (rec) {
        VEnv *node = venv_ext(g_venv, name, NULL);
        node->val = eval(node, rhs);
        val = node->val;
        g_venv = node;
    } else {
        val = eval(g_venv, rhs);
        g_venv = venv_ext(g_venv, name, val);
    }
    printf("%s : %s = ", name, show(scheme));
    print_value(val);
    printf("\n");
}

static void run_expr(Expr *e) {
    Type *scheme = infer_top(e);
    Value *v = eval(g_venv, e);
    print_value(v);
    printf(" : %s\n", show(scheme));
}

/* Parse and run one statement. Returns 0 at EOF. */
static int run_statement(void) {
    if (cur.kind == TK_SEMI) advance();
    if (cur.kind == TK_EOF) return 0;

    if (cur.kind == TK_LET) {
        advance();
        int rec = 0;
        if (cur.kind == TK_REC) { rec = 1; advance(); }
        if (cur.kind != TK_IDENT) fail("parse error: expected name after let");
        char *name = xstrdup(cur.text); advance();
        expect(TK_EQ, "=");
        Expr *rhs = parse_expr();
        if (cur.kind == TK_IN) {            /* it's an expression-let after all */
            advance();
            Expr *body = parse_expr();
            Expr *le = E(E_LET);
            le->let.rec = rec; le->let.name = name; le->let.rhs = rhs; le->let.body = body;
            run_expr(le);
        } else {                            /* top-level binding */
            run_binding(rec, name, rhs);
        }
    } else {
        run_expr(parse_expr());
    }
    return 1;
}

static void run_source(const char *src) {
    g_src = src; g_pos = 0;
    advance();
    while (cur.kind != TK_EOF) {
        if (!run_statement()) break;
    }
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = xmalloc(n + 1);
    size_t r = fread(buf, 1, n, f); buf[r] = 0;
    fclose(f);
    return buf;
}

static void repl(void) {
    char line[4096];
    printf("typed lambda calculus (Hindley-Milner).  Ctrl-D / Ctrl-Z to exit.\n");
    for (;;) {
        printf("> "); fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) break;
        if (setjmp(g_jmp)) { fprintf(stderr, "error: %s\n", g_err); continue; }
        g_src = line; g_pos = 0;
        advance();
        while (cur.kind != TK_EOF) if (!run_statement()) break;
    }
    printf("\n");
}

int main(int argc, char **argv) {
    init_globals();
    if (argc >= 4 && !strcmp(argv[1], "--bench")) {
        int iters = atoi(argv[2]);
        if (setjmp(g_jmp)) { fprintf(stderr, "error: %s\n", g_err); return 1; }
        char *src = read_file(argv[3]);
        g_src = src; g_pos = 0; advance();
        Expr *e = parse_expr();                 /* file must be one expression */
        for (int i = 0; i < 3; i++) infer(g_tenv, e);
        clock_t t0 = clock();
        for (int i = 0; i < iters; i++) infer(g_tenv, e);
        clock_t t1 = clock();
        double secs = (double)(t1 - t0) / CLOCKS_PER_SEC;
        printf("inference: %d iters in %.3f s  =  %.2f us/infer  =  %.0f infers/sec\n",
               iters, secs, secs * 1e6 / iters, iters / secs);
        return 0;
    }
    if (argc > 1) {
        if (setjmp(g_jmp)) { fprintf(stderr, "error: %s\n", g_err); return 1; }
        char *src = read_file(argv[1]);
        run_source(src);
        return 0;
    }
    repl();
    return 0;
}
