/* typed.c — a typed lambda-calculus interpreter.
 *
 * Type system: Hindley-Milner (let-polymorphism) with full type inference.
 *   - Algorithm W, destructive unification (union-find on mutable tyvars).
 *   - Efficient generalization via Remy/Kiselyov *levels* (no env scan).
 *   - Turing-complete: `let rec` / recursive bindings.
 *   - No annotations required; `\x:T. e` annotations are optional and checked.
 *
 * Performance (the inference path is the optimization target):
 *   - Arena (bump) allocation for all nodes; no per-node malloc/free.
 *     Transient inference types live in a scratch arena that is reset after
 *     each statement, so steady-state inference allocates ~nothing.
 *   - Identifiers are *interned*; variable comparison is a pointer compare.
 *   - A resolve pass rewrites every variable to a de Bruijn index (locals) or
 *     a global slot, so inference and evaluation do O(1) array lookups instead
 *     of scanning an association list. No string compares in the hot loop.
 *   - Instantiation is copy-free for monomorphic schemes (shares nodes).
 *
 * Evaluation: types erase. A strict (CBV) environment-closure machine runs the
 * underlying untyped term; recursion is a self-referential closure / cell.
 *
 * Build:  clang -O2 -o typed.exe typed.c     (or gcc)
 * Run:    typed.exe                 REPL (try :help)
 *         typed.exe file.tl         run a file
 *         typed.exe --no-eval f.tl  type-check only
 *         typed.exe --bench N f.tl  re-run inference on the final expression
 *                                   N times and report throughput
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
static int     g_incomplete;   /* parse hit EOF mid-statement (REPL continues) */

static void fail(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(g_err, sizeof g_err, fmt, ap);
    va_end(ap);
    longjmp(g_jmp, 1);
}

/* ===================================================================== */
/*  ARENA ALLOCATOR                                                       */
/* ===================================================================== */
/* Bump allocation from a chain of large blocks. `reset` keeps the first
 * block and frees the rest, so a scratch arena recycles its memory. */
typedef struct ABlk { struct ABlk *next; size_t off, cap; } ABlk;
typedef struct { ABlk *first, *cur; size_t blksz; } Arena;

static ABlk *ablk_new(size_t cap) {
    ABlk *b = malloc(sizeof(ABlk) + cap);
    if (!b) { perror("malloc"); exit(1); }
    b->next = NULL; b->off = 0; b->cap = cap;
    return b;
}
static void arena_init(Arena *a, size_t blksz) {
    a->blksz = blksz; a->first = a->cur = ablk_new(blksz);
}
static void *arena_alloc_slow(Arena *a, size_t n) {   /* n already aligned */
    size_t cap = n > a->blksz ? n : a->blksz;
    ABlk *nb = ablk_new(cap);
    a->cur->next = nb; a->cur = nb;
    nb->off = n;
    return (char *)(nb + 1);
}
static inline void *arena_alloc(Arena *a, size_t n) {
    n = (n + 15) & ~(size_t)15;                    /* 16-byte align */
    ABlk *b = a->cur;
    size_t off = b->off;
    if (off + n <= b->cap) { b->off = off + n; return (char *)(b + 1) + off; }
    return arena_alloc_slow(a, n);
}
static void arena_reset(Arena *a) {
    ABlk *b = a->first->next;
    while (b) { ABlk *nx = b->next; free(b); b = nx; }
    a->first->next = NULL; a->first->off = 0; a->cur = a->first;
}

static Arena g_perm;   /* AST, annotations, interned strings, global schemes */
static Arena g_tmp;    /* transient inference types (reset per statement)     */
static Arena g_val;    /* runtime values (persist; never reset)               */
static Arena *g_TA = &g_tmp;   /* arena that mk() (type nodes) draws from      */

static char *perm_strdup(const char *s, size_t n) {
    char *p = arena_alloc(&g_perm, n + 1);
    memcpy(p, s, n); p[n] = 0; return p;
}

/* ===================================================================== */
/*  STRING INTERNING                                                      */
/* ===================================================================== */
/* A Sym is a canonical, unique pointer for a spelling; compare with ==.   */
typedef const char *Sym;

static Sym  *g_itab;      /* open-addressing set of interned strings */
static int   g_icap, g_icnt;

static unsigned str_hash(const char *s, size_t n) {
    unsigned h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    return h;
}
static void intern_grow(void) {
    int oldcap = g_icap; Sym *old = g_itab;
    g_icap = oldcap ? oldcap * 2 : 1024;
    g_itab = calloc(g_icap, sizeof(Sym));
    for (int i = 0; i < oldcap; i++) {
        if (!old[i]) continue;
        unsigned h = str_hash(old[i], strlen(old[i])) & (g_icap - 1);
        while (g_itab[h]) h = (h + 1) & (g_icap - 1);
        g_itab[h] = old[i];
    }
    free(old);
}
static Sym intern(const char *s, size_t n) {
    if ((g_icnt + 1) * 2 >= g_icap) intern_grow();
    unsigned h = str_hash(s, n) & (g_icap - 1);
    while (g_itab[h]) {
        if (strlen(g_itab[h]) == n && memcmp(g_itab[h], s, n) == 0) return g_itab[h];
        h = (h + 1) & (g_icap - 1);
    }
    Sym sym = perm_strdup(s, n);
    g_itab[h] = sym; g_icnt++;
    return sym;
}

/* ===================================================================== */
/*  TYPES                                                                 */
/* ===================================================================== */
/* T_VAR is an *unbound* variable (carries id+level); once solved it becomes a
 * T_LINK forwarding to its value. Splitting the two lets the link pointer share
 * storage with id/level, shrinking the node from 40 to 32 bytes. */
typedef enum { T_VAR, T_LINK, T_GEN, T_ARROW, T_CON } TKind;
typedef struct Type Type;
struct Type {
    /* memoization: a node carries the id of the last traversal that visited it
     * (`stamp`) and that traversal's result for it (`memo`). Because inferred
     * types are shared DAGs, this makes occurs/generalize/instantiate visit
     * each node once instead of expanding the (exponentially larger) tree. */
    Type    *memo;
    union {
        Type *link;                          /* T_LINK: forwarding pointer  */
        struct { int id; int level; } var;   /* T_VAR:  unbound variable    */
        struct { Type *from, *to; } arrow;   /* T_ARROW */
        int gen_id;                          /* T_GEN  */
        const char *con;                     /* T_CON  */
    };
    unsigned      stamp;
    unsigned char kind;
};
_Static_assert(sizeof(Type) == 32, "Type node should pack to 32 bytes");

static int      g_var_id = 0;
static int      g_level  = 1;
static unsigned g_stamp  = 0;   /* bumped at the start of each memoized pass */

/* Every Type is exactly 32 bytes (a multiple of 16), so a node allocator can
 * bump by a constant and skip the general alignment math. */
static inline Type *alloc_type(void) {
    Arena *a = g_TA; ABlk *b = a->cur; size_t off = b->off;
    if (off + 32 <= b->cap) { b->off = off + 32; return (Type *)((char *)(b + 1) + off); }
    return arena_alloc_slow(a, 32);
}
static inline Type *mk(TKind k) { Type *t = alloc_type(); t->kind = k; t->stamp = 0; return t; }
static inline Type *fresh_var(void) {
    Type *t = mk(T_VAR);
    t->var.id = g_var_id++; t->var.level = g_level;
    return t;
}
static inline Type *mk_arrow(Type *a, Type *b) { Type *t = mk(T_ARROW); t->arrow.from = a; t->arrow.to = b; return t; }
static inline Type *mk_con(const char *c)      { Type *t = mk(T_CON);   t->con = c; return t; }
static inline Type *mk_gen(int id)             { Type *t = mk(T_GEN);   t->gen_id = id; return t; }

static Type *T_INT, *T_BOOL;

static Type *prune_slow(Type *t) {                 /* t is known T_LINK */
    Type *r = t->link;
    if (r->kind == T_LINK) { r = prune_slow(r); t->link = r; }
    return r;
}
static inline Type *prune(Type *t) {
    return t->kind == T_LINK ? prune_slow(t) : t;
}
static void enter_level(void) { g_level++; }
static void exit_level(void)  { g_level--; }

/* ----------------------------------------------------- unification ----- */
/* occurs check + level lowering, memoized so a shared subterm is scanned once */
static void occurs_rec(Type *var, Type *t) {
    t = prune(t);
    if (t->stamp == g_stamp) return;
    t->stamp = g_stamp;
    if (t->kind == T_VAR) {
        if (t == var) fail("occurs check: cannot construct the infinite type");
        if (t->var.level > var->var.level) t->var.level = var->var.level;
    } else if (t->kind == T_ARROW) {
        occurs_rec(var, t->arrow.from);
        occurs_rec(var, t->arrow.to);
    }
}
static void occurs(Type *var, Type *t) { ++g_stamp; occurs_rec(var, t); }

static char *show(Type *t);

static void unify(Type *a, Type *b) {
    a = prune(a); b = prune(b);
    if (a == b) return;
    if (a->kind == T_VAR) { occurs(a, b); a->kind = T_LINK; a->link = b; return; }
    if (b->kind == T_VAR) { occurs(b, a); b->kind = T_LINK; b->link = a; return; }
    if (a->kind == T_ARROW && b->kind == T_ARROW) {
        unify(a->arrow.from, b->arrow.from);
        unify(a->arrow.to,   b->arrow.to);
        return;
    }
    if (a->kind == T_CON && b->kind == T_CON && strcmp(a->con, b->con) == 0) return;
    char *sa = show(a), *sb = show(b);
    fail("type mismatch: cannot unify %s with %s", sa, sb);
}

/* --------------------------------------------- generalize / instantiate */
/* Returns a scheme; quantifies vars deeper than the current level. Memoized,
 * so a shared subterm is rewritten once and stays shared in the result DAG. */
static Type *gen_rec(Type *t) {
    t = prune(t);
    if (t->stamp == g_stamp) return t->memo;
    Type *r;
    switch (t->kind) {
        case T_VAR:   r = (t->var.level > g_level) ? mk_gen(t->var.id) : t; break;
        case T_ARROW: { Type *f = gen_rec(t->arrow.from), *g = gen_rec(t->arrow.to); r = mk_arrow(f, g); break; }
        default:      r = t; break;
    }
    t->stamp = g_stamp; t->memo = r;
    return r;
}
static Type *generalize(Type *t) { ++g_stamp; return gen_rec(t); }

/* Map gen_id -> fresh var. One reused global (instantiate is not reentrant), so
 * there is no per-call stack frame; inline buffer for the common (few type
 * vars) case, grows into the scratch arena for large schemes. */
typedef struct { int n, cap; int ik[32]; Type *iv[32]; int *k; Type **v; } IMap;
static IMap g_imap;
static inline void imap_reset(void) { g_imap.n = 0; g_imap.cap = 32; g_imap.k = g_imap.ik; g_imap.v = g_imap.iv; }
static Type *imap_get(int id) {
    int n = g_imap.n; int *k = g_imap.k;
    for (int i = 0; i < n; i++) if (k[i] == id) return g_imap.v[i];
    if (n == g_imap.cap) {
        int nc = g_imap.cap * 2;
        int   *nk = arena_alloc(&g_tmp, nc * sizeof *nk);
        Type **nv = arena_alloc(&g_tmp, nc * sizeof *nv);
        memcpy(nk, g_imap.k, n * sizeof *nk);
        memcpy(nv, g_imap.v, n * sizeof *nv);
        g_imap.k = nk; g_imap.v = nv; g_imap.cap = nc;
    }
    Type *fv = fresh_var();
    g_imap.k[n] = id; g_imap.v[n] = fv; g_imap.n = n + 1;
    return fv;
}
/* Memoized + copy-free: monomorphic subtrees return the original node (zero
 * allocation); polymorphic subtrees are copied once and shared in the result. */
static Type *inst_rec(Type *t, int *changed) {
    /* Dispatch on kind directly (no separate prune branch): arrow/con/gen nodes
     * are never links; only a leaf var can be, handled by the T_LINK case. */
    switch (t->kind) {
        case T_LINK: return inst_rec(prune_slow(t), changed);
        case T_GEN:  *changed = 1; return imap_get(t->gen_id);
        case T_ARROW:
            if (t->stamp == g_stamp) {            /* visited this pass */
                if (t->memo) { *changed = 1; return t->memo; }  /* was copied */
                return t;                                       /* was unchanged */
            }
            { int c = 0;
              Type *f = inst_rec(t->arrow.from, &c);
              Type *g = inst_rec(t->arrow.to,   &c);
              t->stamp = g_stamp;
              if (c) { *changed = 1; return t->memo = mk_arrow(f, g); }
              t->memo = NULL; return t;
            }
        default: return t;                        /* T_VAR (free), T_CON */
    }
}
static Type *instantiate(Type *scheme) {
    ++g_stamp; imap_reset(); int c = 0;
    return inst_rec(scheme, &c);
}

/* ----------------------------------------------------- pretty-print --- */
typedef struct { void *keys[256]; int n; } NameCtx;
static int name_index(NameCtx *nc, void *key) {
    for (int i = 0; i < nc->n; i++) if (nc->keys[i] == key) return i;
    nc->keys[nc->n] = key; return nc->n++;
}
static int g_show_budget;
static void emit(char **p, char *end, const char *s) { while (*s && *p < end - 1) *(*p)++ = *s++; }
static void emit_name(char **p, char *end, int idx) {
    char b[8];
    if (idx < 26) { b[0] = (char)('a' + idx); b[1] = 0; }
    else snprintf(b, sizeof b, "t%d", idx);
    emit(p, end, b);
}
static void show_rec(Type *t, NameCtx *nc, char **p, char *end, int prec) {
    if (--g_show_budget < 0) { emit(p, end, "..."); return; }   /* guard huge DAG types */
    t = prune(t);
    switch (t->kind) {
        case T_CON: emit(p, end, t->con); break;
        case T_VAR: emit_name(p, end, name_index(nc, t)); break;
        case T_GEN: emit_name(p, end, name_index(nc, (void *)(intptr_t)(t->gen_id + 1))); break;
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
    static char bufs[4][512]; static int which = 0;
    char *buf = bufs[which++ & 3];
    NameCtx nc; nc.n = 0;
    char *p = buf, *end = buf + 512;
    g_show_budget = 4000;
    show_rec(t, &nc, &p, end, 0); *p = 0;
    return buf;
}

/* ===================================================================== */
/*  AST                                                                   */
/* ===================================================================== */
typedef enum { E_VAR, E_LOCAL, E_GLOBAL, E_INT, E_BOOL, E_LAM, E_APP, E_LET, E_IF } EKind;
typedef struct Expr Expr;
struct Expr {
    EKind kind;
    union {
        struct { Sym name; int idx; } v;   /* E_VAR(name) -> E_LOCAL(idx)/E_GLOBAL(idx) */
        long i; int b;
        struct { Sym param; Type *ann; Expr *body; } lam;
        struct { Expr *fn, *arg; } app;
        struct { int rec; Sym name; Expr *rhs, *body; } let;
        struct { Expr *c, *t, *e; } iff;
    };
};

static Expr *E(EKind k) { Expr *e = arena_alloc(&g_perm, sizeof *e); e->kind = k; return e; }
static Expr *e_app(Expr *f, Expr *a) { Expr *e = E(E_APP); e->app.fn = f; e->app.arg = a; return e; }
static Expr *e_binop(Sym op, Expr *l, Expr *r) {
    Expr *v = E(E_VAR); v->v.name = op;
    return e_app(e_app(v, l), r);
}

/* ===================================================================== */
/*  LEXER                                                                 */
/* ===================================================================== */
typedef enum {
    TK_EOF, TK_INT, TK_IDENT,
    TK_LET, TK_REC, TK_IN, TK_IF, TK_THEN, TK_ELSE, TK_TRUE, TK_FALSE,
    TK_LAMBDA, TK_DOT, TK_LPAREN, TK_RPAREN, TK_EQ, TK_COLON, TK_ARROW,
    TK_SEMI, TK_OP
} TKtype;
typedef struct { TKtype kind; char text[64]; long ival; int bol; } Tok;

static const char *g_src;
static int         g_pos;
static int         g_paren;   /* parenthesis nesting; newlines inside () are not separators */
static Tok         cur;

static int kw(const char *s, TKtype *out) {
    if (!strcmp(s,"let")){*out=TK_LET;return 1;}    if (!strcmp(s,"rec")){*out=TK_REC;return 1;}
    if (!strcmp(s,"in")){*out=TK_IN;return 1;}      if (!strcmp(s,"if")){*out=TK_IF;return 1;}
    if (!strcmp(s,"then")){*out=TK_THEN;return 1;}  if (!strcmp(s,"else")){*out=TK_ELSE;return 1;}
    if (!strcmp(s,"true")){*out=TK_TRUE;return 1;}  if (!strcmp(s,"false")){*out=TK_FALSE;return 1;}
    return 0;
}
static void advance(void) {
    const char *s = g_src; int nl = 0;
    for (;;) {
        char c = s[g_pos];
        if (c == '\n') { nl = 1; g_pos++; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { g_pos++; continue; }
        if (c == '-' && s[g_pos+1] == '-') { while (s[g_pos] && s[g_pos] != '\n') g_pos++; continue; }
        break;
    }
    cur.bol = nl && g_paren == 0;   /* only a top-level newline ends a statement */
    char c = s[g_pos];
    if (c == 0) { cur.kind = TK_EOF; return; }
    if (c >= '0' && c <= '9') {
        long v = 0; while (s[g_pos] >= '0' && s[g_pos] <= '9') v = v*10 + (s[g_pos++]-'0');
        cur.kind = TK_INT; cur.ival = v; return;
    }
    if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_') {
        int n = 0;
        while (((c=s[g_pos])>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_') {
            if (n < 63) cur.text[n++] = c; g_pos++;
        }
        cur.text[n] = 0;
        TKtype k; cur.kind = kw(cur.text, &k) ? k : TK_IDENT; return;
    }
    g_pos++;
    switch (c) {
        case '\\': cur.kind = TK_LAMBDA; return;
        case '.':  cur.kind = TK_DOT;    return;
        case '(':  cur.kind = TK_LPAREN; g_paren++; return;
        case ')':  cur.kind = TK_RPAREN; if (g_paren) g_paren--; return;
        case ':':  cur.kind = TK_COLON;  return;
        case ';':  cur.kind = TK_SEMI;   return;
        case '+': case '*': cur.kind = TK_OP; cur.text[0]=c; cur.text[1]=0; return;
        case '=': if (s[g_pos]=='='){g_pos++;cur.kind=TK_OP;strcpy(cur.text,"==");} else cur.kind=TK_EQ; return;
        case '-': if (s[g_pos]=='>'){g_pos++;cur.kind=TK_ARROW;} else {cur.kind=TK_OP;cur.text[0]='-';cur.text[1]=0;} return;
        case '<': cur.kind=TK_OP; if (s[g_pos]=='='){g_pos++;strcpy(cur.text,"<=");} else strcpy(cur.text,"<"); return;
        case '>': cur.kind=TK_OP; if (s[g_pos]=='='){g_pos++;strcpy(cur.text,">=");} else strcpy(cur.text,">"); return;
    }
    fail("unexpected character '%c'", c);
}
static void expect(TKtype k, const char *what) {
    if (cur.kind != k) { if (cur.kind == TK_EOF) g_incomplete = 1; fail("parse error: expected %s", what); }
    advance();
}

/* ===================================================================== */
/*  PARSER  (pure: produces AST with names, no resolution/evaluation)     */
/* ===================================================================== */
static Expr *parse_expr(void);

static Type *parse_type(void);
static Type *parse_type_atom(void) {
    if (cur.kind == TK_LPAREN) { advance(); Type *t = parse_type(); expect(TK_RPAREN, ")"); return t; }
    if (cur.kind == TK_IDENT) {
        if (!strcmp(cur.text,"Int"))  { advance(); return T_INT;  }
        if (!strcmp(cur.text,"Bool")) { advance(); return T_BOOL; }
        fail("unknown type '%s' (annotations support Int, Bool, ->)", cur.text);
    }
    if (cur.kind == TK_EOF) g_incomplete = 1;
    fail("parse error: expected a type"); return NULL;
}
static Type *parse_type(void) {
    Type *a = parse_type_atom();
    if (cur.kind == TK_ARROW) { advance(); return mk_arrow(a, parse_type()); }
    return a;
}

static int starts_atom(void) {
    if (cur.bol) return 0;   /* a newline ends a statement */
    switch (cur.kind) {
        case TK_INT: case TK_IDENT: case TK_TRUE: case TK_FALSE: case TK_LPAREN: return 1;
        default: return 0;
    }
}
static Sym tok_sym(void) { return intern(cur.text, strlen(cur.text)); }

static Expr *parse_atom(void) {
    switch (cur.kind) {
        case TK_INT:   { Expr *e = E(E_INT);  e->i = cur.ival; advance(); return e; }
        case TK_TRUE:  { Expr *e = E(E_BOOL); e->b = 1; advance(); return e; }
        case TK_FALSE: { Expr *e = E(E_BOOL); e->b = 0; advance(); return e; }
        case TK_IDENT: { Expr *e = E(E_VAR); e->v.name = tok_sym(); advance(); return e; }
        case TK_LPAREN:{ advance(); Expr *e = parse_expr(); expect(TK_RPAREN, ")"); return e; }
        default:
            if (cur.kind == TK_EOF) g_incomplete = 1;
            fail("parse error: expected an expression"); return NULL;
    }
}
static Expr *parse_app(void) {
    Expr *e = parse_atom();
    while (starts_atom()) e = e_app(e, parse_atom());
    return e;
}
static Expr *parse_mul(void) {
    Expr *e = parse_app();
    while (cur.kind==TK_OP && !strcmp(cur.text,"*")) { Sym op=tok_sym(); advance(); e=e_binop(op,e,parse_app()); }
    return e;
}
static Expr *parse_add(void) {
    Expr *e = parse_mul();
    while (cur.kind==TK_OP && (!strcmp(cur.text,"+")||!strcmp(cur.text,"-"))) { Sym op=tok_sym(); advance(); e=e_binop(op,e,parse_mul()); }
    return e;
}
static Expr *parse_cmp(void) {
    Expr *e = parse_add();
    while (cur.kind==TK_OP && (!strcmp(cur.text,"==")||!strcmp(cur.text,"<")||!strcmp(cur.text,"<=")||
                               !strcmp(cur.text,">")||!strcmp(cur.text,">="))) {
        Sym op=tok_sym(); advance(); e=e_binop(op,e,parse_add());
    }
    return e;
}
static Expr *parse_lambda(void) {
    expect(TK_LAMBDA, "\\");
    Sym names[64]; Type *anns[64]; int n = 0;
    while (cur.kind == TK_IDENT) {
        if (n == 64) fail("too many lambda parameters");
        names[n] = tok_sym(); advance();
        anns[n] = (cur.kind == TK_COLON) ? (advance(), parse_type()) : NULL;
        n++;
    }
    if (n == 0) fail("parse error: lambda needs a parameter");
    expect(TK_DOT, ".");
    Expr *body = parse_expr();
    for (int i = n-1; i >= 0; i--) {
        Expr *l = E(E_LAM); l->lam.param = names[i]; l->lam.ann = anns[i]; l->lam.body = body; body = l;
    }
    return body;
}
static Expr *parse_if(void) {
    expect(TK_IF, "if"); Expr *c = parse_expr();
    expect(TK_THEN, "then"); Expr *t = parse_expr();
    expect(TK_ELSE, "else"); Expr *e = parse_expr();
    Expr *r = E(E_IF); r->iff.c=c; r->iff.t=t; r->iff.e=e; return r;
}
static Expr *parse_let_expr(void) {
    expect(TK_LET, "let");
    int rec = 0; if (cur.kind==TK_REC) { rec=1; advance(); }
    if (cur.kind != TK_IDENT) fail("parse error: expected name after let");
    Sym name = tok_sym(); advance();
    expect(TK_EQ, "=");
    Expr *rhs = parse_expr();
    expect(TK_IN, "in");
    Expr *body = parse_expr();
    Expr *e = E(E_LET); e->let.rec=rec; e->let.name=name; e->let.rhs=rhs; e->let.body=body; return e;
}
static Expr *parse_expr(void) {
    switch (cur.kind) {
        case TK_LAMBDA: return parse_lambda();
        case TK_IF:     return parse_if();
        case TK_LET:    return parse_let_expr();
        default:        return parse_cmp();
    }
}

/* ===================================================================== */
/*  GLOBALS (slot table) + name resolution                                */
/* ===================================================================== */
static Sym    *g_gname;
static Type  **g_gscheme;
static struct Value **g_gvalue;   /* fwd */
static int     g_nglob, g_capglob;

static int global_lookup(Sym name) {
    for (int i = 0; i < g_nglob; i++) if (g_gname[i] == name) return i;  /* sym == sym */
    return -1;
}
static int global_define(Sym name) {
    int s = global_lookup(name);
    if (s >= 0) return s;
    if (g_nglob == g_capglob) {
        g_capglob = g_capglob ? g_capglob*2 : 64;
        g_gname   = realloc(g_gname,   g_capglob*sizeof *g_gname);
        g_gscheme = realloc(g_gscheme, g_capglob*sizeof *g_gscheme);
        g_gvalue  = realloc(g_gvalue,  g_capglob*sizeof *g_gvalue);
    }
    s = g_nglob++;
    g_gname[s] = name; g_gscheme[s] = NULL; g_gvalue[s] = NULL;
    return s;
}

/* Resolve E_VAR -> E_LOCAL(de Bruijn) / E_GLOBAL(slot), in place. */
static Sym g_scope[1 << 16];
static int g_sdepth;

static void resolve(Expr *e) {
    switch (e->kind) {
        case E_VAR: {
            Sym nm = e->v.name;
            for (int i = g_sdepth - 1; i >= 0; i--)
                if (g_scope[i] == nm) { e->kind = E_LOCAL; e->v.idx = g_sdepth - 1 - i; return; }
            int s = global_lookup(nm);
            if (s < 0) fail("unbound variable: %s", nm);
            e->kind = E_GLOBAL; e->v.idx = s; return;
        }
        case E_INT: case E_BOOL: case E_LOCAL: case E_GLOBAL: return;
        case E_LAM:
            g_scope[g_sdepth++] = e->lam.param; resolve(e->lam.body); g_sdepth--; return;
        case E_APP:
            resolve(e->app.fn); resolve(e->app.arg); return;
        case E_IF:
            resolve(e->iff.c); resolve(e->iff.t); resolve(e->iff.e); return;
        case E_LET:
            if (e->let.rec) {
                g_scope[g_sdepth++] = e->let.name;
                resolve(e->let.rhs); resolve(e->let.body);
                g_sdepth--;
            } else {
                resolve(e->let.rhs);
                g_scope[g_sdepth++] = e->let.name;
                resolve(e->let.body);
                g_sdepth--;
            }
            return;
    }
}

/* ===================================================================== */
/*  TYPE INFERENCE  (Algorithm W, index-addressed environment)            */
/* ===================================================================== */
typedef struct { Type *ty; int poly; } Local;
static Local g_locals[1 << 16];
static int   g_nl;

static Type *infer(Expr *e) {
    switch (e->kind) {
        case E_INT:  return T_INT;
        case E_BOOL: return T_BOOL;
        case E_LOCAL: {
            Local L = g_locals[g_nl - 1 - e->v.idx];
            return L.poly ? instantiate(L.ty) : L.ty;
        }
        case E_GLOBAL: return instantiate(g_gscheme[e->v.idx]);
        case E_LAM: {
            Type *pt = e->lam.ann ? e->lam.ann : fresh_var();
            g_locals[g_nl].ty = pt; g_locals[g_nl].poly = 0; g_nl++;
            Type *rt = infer(e->lam.body);
            g_nl--;
            return mk_arrow(pt, rt);
        }
        case E_APP: {
            Type *tf = prune(infer(e->app.fn));
            Type *ta = infer(e->app.arg);
            if (tf->kind == T_ARROW) {        /* known function: peel directly, */
                unify(ta, tf->arrow.from);    /* no fresh var, no arrow node     */
                return tf->arrow.to;
            }
            Type *tr = fresh_var();
            unify(tf, mk_arrow(ta, tr));
            return tr;
        }
        case E_IF: {
            unify(infer(e->iff.c), T_BOOL);
            Type *tt = infer(e->iff.t);
            Type *te = infer(e->iff.e);
            unify(tt, te);
            return tt;
        }
        case E_LET: {
            int p = g_nl;
            if (e->let.rec) {
                enter_level();
                Type *tv = fresh_var();
                g_locals[p].ty = tv; g_locals[p].poly = 0; g_nl++;
                Type *t1 = infer(e->let.rhs);
                unify(tv, t1);
                exit_level();
                g_locals[p].ty = generalize(t1); g_locals[p].poly = 1;  /* re-bind for body */
            } else {
                enter_level();
                Type *t1 = infer(e->let.rhs);
                exit_level();
                g_locals[p].ty = generalize(t1); g_locals[p].poly = 1; g_nl++;
            }
            Type *r = infer(e->let.body);
            g_nl--;
            return r;
        }
        default: fail("internal: bad expr kind"); return NULL;
    }
}

/* ===================================================================== */
/*  EVALUATOR  (de Bruijn closures; types erased)                         */
/* ===================================================================== */
typedef enum { OP_ADD, OP_SUB, OP_MUL, OP_EQ, OP_LT, OP_LE, OP_GT, OP_GE } Op;
typedef struct Value Value;
typedef struct VEnv { Value *v; struct VEnv *next; } VEnv;
typedef enum { V_INT, V_BOOL, V_CLO, V_PRIM } VKind;
struct Value {
    VKind kind;
    union {
        long i; int b;
        struct { Expr *body; VEnv *env; } clo;
        struct { Op op; int got; long a0; } prim;
    };
};
static Value *valloc(void) { return arena_alloc(&g_val, sizeof(Value)); }
static Value *v_int(long i)  { Value *v = valloc(); v->kind=V_INT;  v->i=i; return v; }
static Value *v_bool(int b)  { Value *v = valloc(); v->kind=V_BOOL; v->b=b; return v; }
static Value *v_prim(Op op)  { Value *v = valloc(); v->kind=V_PRIM; v->prim.op=op; v->prim.got=0; v->prim.a0=0; return v; }
static VEnv  *vcons(Value *val, VEnv *next) { VEnv *n = arena_alloc(&g_val, sizeof *n); n->v=val; n->next=next; return n; }

static Value *eval(VEnv *env, Expr *e);

static Value *apply(Value *f, Value *a) {
    if (f->kind == V_CLO) return eval(vcons(a, f->clo.env), f->clo.body);
    /* V_PRIM (binary) */
    if (f->prim.got == 0) { Value *v = v_prim(f->prim.op); v->prim.got=1; v->prim.a0=a->i; return v; }
    long x = f->prim.a0, y = a->i;
    switch (f->prim.op) {
        case OP_ADD: return v_int(x+y);  case OP_SUB: return v_int(x-y);  case OP_MUL: return v_int(x*y);
        case OP_EQ:  return v_bool(x==y); case OP_LT: return v_bool(x<y);  case OP_LE: return v_bool(x<=y);
        case OP_GT:  return v_bool(x>y);  case OP_GE: return v_bool(x>=y);
    }
    return NULL;
}
static Value *eval(VEnv *env, Expr *e) {
    switch (e->kind) {
        case E_INT:  return v_int(e->i);
        case E_BOOL: return v_bool(e->b);
        case E_LOCAL: { VEnv *p = env; for (int i = e->v.idx; i; i--) p = p->next; return p->v; }
        case E_GLOBAL: {
            Value *v = g_gvalue[e->v.idx];
            if (!v) fail("runtime: uninitialized recursive value");
            return v;
        }
        case E_LAM: { Value *v = valloc(); v->kind=V_CLO; v->clo.body=e->lam.body; v->clo.env=env; return v; }
        case E_APP: { Value *f = eval(env, e->app.fn); Value *a = eval(env, e->app.arg); return apply(f, a); }
        case E_IF:  { Value *c = eval(env, e->iff.c); return eval(env, c->b ? e->iff.t : e->iff.e); }
        case E_LET:
            if (e->let.rec) {
                VEnv *cell = vcons(NULL, env);
                cell->v = eval(cell, e->let.rhs);
                return eval(cell, e->let.body);
            } else {
                Value *rv = eval(env, e->let.rhs);
                return eval(vcons(rv, env), e->let.body);
            }
        default: fail("internal: bad expr kind"); return NULL;
    }
}
static void print_value(Value *v) {
    switch (v->kind) {
        case V_INT:  printf("%ld", v->i); break;
        case V_BOOL: printf("%s", v->b ? "true" : "false"); break;
        default:     printf("<function>"); break;
    }
}

/* ===================================================================== */
/*  PRIMITIVES                                                            */
/* ===================================================================== */
static void def_prim(const char *name, Type *ty, Op op) {
    int s = global_define(intern(name, strlen(name)));
    g_gscheme[s] = ty;
    g_gvalue[s]  = v_prim(op);
}
static void init_globals(void) {
    arena_init(&g_perm, 1u << 20);
    arena_init(&g_tmp,  1u << 20);
    arena_init(&g_val,  1u << 20);
    g_TA = &g_perm;                          /* shared constants live in perm */
    T_INT  = mk_con("Int");
    T_BOOL = mk_con("Bool");
    Type *iii = mk_arrow(T_INT, mk_arrow(T_INT, T_INT));
    Type *iib = mk_arrow(T_INT, mk_arrow(T_INT, T_BOOL));
    def_prim("+",iii,OP_ADD); def_prim("-",iii,OP_SUB); def_prim("*",iii,OP_MUL);
    def_prim("==",iib,OP_EQ); def_prim("<",iib,OP_LT);  def_prim("<=",iib,OP_LE);
    def_prim(">",iib,OP_GT);  def_prim(">=",iib,OP_GE);
    g_TA = &g_tmp;
}
static int g_nprim;

/* ===================================================================== */
/*  STATEMENTS                                                            */
/* ===================================================================== */
typedef struct { int is_binding; int rec; Sym name; Expr *e; } Stmt;

static int g_eval_on = 1;   /* off for :type and --no-eval */

/* infer a closed expression and return a generalized scheme for display */
static Type *infer_display(Expr *e) {
    g_nl = 0;
    enter_level();
    Type *t = infer(e);
    exit_level();
    return generalize(t);
}

static void exec_binding(int rec, Sym name, Expr *rhs) {
    int slot;
    g_sdepth = 0;
    if (rec) { slot = global_define(name); resolve(rhs); }
    else     { resolve(rhs); }

    /* --- inference --- */
    g_nl = 0;
    Type *t1;
    enter_level();
    if (rec) {
        Type *tv = fresh_var();
        g_gscheme[slot] = tv;            /* self-reference is monomorphic here */
        t1 = infer(rhs);
        unify(tv, t1);
    } else {
        t1 = infer(rhs);
    }
    exit_level();
    g_TA = &g_perm;                       /* the scheme must outlive the reset */
    Type *scheme = generalize(t1);
    g_TA = &g_tmp;
    if (!rec) slot = global_define(name);
    g_gscheme[slot] = scheme;

    /* --- evaluation --- */
    if (g_eval_on) {
        if (rec) { g_gvalue[slot] = NULL; g_gvalue[slot] = eval(NULL, rhs); }
        else       g_gvalue[slot] = eval(NULL, rhs);
    }

    printf("%s : %s", name, show(scheme));
    if (g_eval_on) { printf(" = "); print_value(g_gvalue[slot]); }
    printf("\n");
    arena_reset(&g_tmp);
}

static void exec_expr(Expr *e) {
    g_sdepth = 0; resolve(e);
    Type *scheme = infer_display(e);
    if (g_eval_on) { Value *v = eval(NULL, e); print_value(v); printf(" : %s\n", show(scheme)); }
    else           { printf("_ : %s\n", show(scheme)); }
    arena_reset(&g_tmp);
}

/* parse a whole source buffer into a statement list (no resolve/exec) */
static Stmt *parse_program(const char *src, int *out_n) {
    g_src = src; g_pos = 0; g_paren = 0; g_incomplete = 0;
    g_TA = &g_perm;                       /* annotations belong to the AST */
    advance();
    int cap = 16, n = 0;
    Stmt *st = malloc(cap * sizeof *st);
    while (cur.kind != TK_EOF) {
        if (cur.kind == TK_SEMI) { advance(); continue; }
        if (n == cap) { cap *= 2; st = realloc(st, cap * sizeof *st); }
        if (cur.kind == TK_LET) {
            advance();
            int rec = 0; if (cur.kind==TK_REC){rec=1;advance();}
            if (cur.kind != TK_IDENT) { if (cur.kind==TK_EOF) g_incomplete=1; fail("parse error: expected name after let"); }
            Sym name = tok_sym(); advance();
            expect(TK_EQ, "=");
            Expr *rhs = parse_expr();
            if (cur.kind == TK_IN) {                 /* it was a let-expression */
                advance();
                Expr *body = parse_expr();
                Expr *le = E(E_LET); le->let.rec=rec; le->let.name=name; le->let.rhs=rhs; le->let.body=body;
                st[n].is_binding=0; st[n].e=le; n++;
            } else {                                  /* top-level binding */
                st[n].is_binding=1; st[n].rec=rec; st[n].name=name; st[n].e=rhs; n++;
            }
        } else {
            st[n].is_binding=0; st[n].e = parse_expr(); n++;
        }
    }
    g_TA = &g_tmp;
    *out_n = n;
    return st;
}

static void exec_stmt(Stmt *s) {
    if (s->is_binding) exec_binding(s->rec, s->name, s->e);
    else               exec_expr(s->e);
}

/* parse + execute a buffer (used by file mode and the REPL) */
static void run_source(const char *src) {
    int n; Stmt *st = parse_program(src, &n);
    for (int i = 0; i < n; i++) exec_stmt(&st[i]);
    free(st);
}

/* ===================================================================== */
/*  BENCHMARK                                                             */
/* ===================================================================== */
static void run_bench(int iters, const char *src) {
    int n; Stmt *st = parse_program(src, &n);
    if (n == 0) { fprintf(stderr, "bench: empty input\n"); return; }
    /* execute all but the last statement to set up globals */
    for (int i = 0; i < n - 1; i++) exec_stmt(&st[i]);
    Stmt *last = &st[n-1];
    if (last->is_binding) { fprintf(stderr, "bench: final statement must be an expression\n"); return; }
    g_sdepth = 0; resolve(last->e);

    /* warm up */
    for (int i = 0; i < 3; i++) { g_nl=0; enter_level(); infer(last->e); exit_level(); arena_reset(&g_tmp); }

    clock_t t0 = clock();
    for (int i = 0; i < iters; i++) {
        g_nl = 0;
        enter_level(); infer(last->e); exit_level();
        arena_reset(&g_tmp);
    }
    clock_t t1 = clock();
    double secs = (double)(t1 - t0) / CLOCKS_PER_SEC;
    double us   = secs * 1e6 / iters;
    printf("inference: %d iters in %.3f s  =  %.2f us/infer  =  %.0f infers/sec\n",
           iters, secs, us, iters / secs);
    free(st);
}

/* ===================================================================== */
/*  REPL                                                                  */
/* ===================================================================== */
static void print_banner(void) {
    printf("typed lambda calculus  (Hindley-Milner)\n");
    printf("type :help for commands, :quit to exit\n");
}
static void print_help(void) {
    printf("  <expr>            infer the type of an expression and evaluate it\n");
    printf("  let x = e         define a top-level binding (let rec for recursion)\n");
    printf("  :type <expr>      show the inferred type without evaluating   (:t)\n");
    printf("  :env              list all top-level bindings and their types\n");
    printf("  :load <file>      run a file of statements                    (:l)\n");
    printf("  :reset            forget all user bindings\n");
    printf("  :help             this message                               (:h :?)\n");
    printf("  :quit             leave the REPL                             (:q)\n");
    printf("Lines ending mid-expression continue on the next line (... prompt).\n");
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc(n + 1);
    size_t r = fread(buf, 1, n, f); buf[r] = 0; fclose(f);
    return buf;
}

static void cmd_env(void) {
    for (int i = 0; i < g_nglob; i++)
        printf("  %-12s : %s\n", g_gname[i], show(g_gscheme[i]));
}
static void cmd_type(const char *arg) {
    int n; Stmt *st = parse_program(arg, &n);
    if (n != 1 || st[0].is_binding) { fprintf(stderr, "usage: :type <expression>\n"); free(st); return; }
    g_sdepth = 0; resolve(st[0].e);
    Type *scheme = infer_display(st[0].e);
    printf(": %s\n", show(scheme));
    arena_reset(&g_tmp);
    free(st);
}
static void reset_user_bindings(void) {
    g_nglob = g_nprim;   /* prims occupy the first slots; drop the rest */
}

/* returns 1 to continue, 0 to quit */
static int handle_command(const char *line) {
    while (*line == ' ') line++;
    const char *arg = line;
    while (*arg && *arg != ' ') arg++;
    size_t cl = arg - line;
    while (*arg == ' ') arg++;
    #define IS(s) (cl == strlen(s) && !strncmp(line, s, cl))
    if (IS(":quit") || IS(":q")) return 0;
    if (IS(":help") || IS(":h") || IS(":?")) { print_help(); return 1; }
    if (IS(":env"))  { cmd_env(); return 1; }
    if (IS(":type") || IS(":t")) { cmd_type(arg); return 1; }
    if (IS(":load") || IS(":l")) {
        if (!*arg) { fprintf(stderr, "usage: :load <file>\n"); return 1; }
        char *src = read_file(arg); run_source(src); free(src); return 1;
    }
    if (IS(":reset")) { reset_user_bindings(); printf("bindings cleared\n"); return 1; }
    #undef IS
    fprintf(stderr, "unknown command (try :help)\n");
    return 1;
}

static void repl(void) {
    print_banner();
    char buf[1 << 16]; size_t len = 0;
    int cont = 0;   /* are we continuing an incomplete statement? */
    for (;;) {
        printf(cont ? "... " : "> "); fflush(stdout);
        char line[1 << 14];
        if (!fgets(line, sizeof line, stdin)) break;
        if (!cont && line[0] == ':') {
            char *nl = strpbrk(line, "\r\n"); if (nl) *nl = 0;
            if (!handle_command(line)) break;
            continue;
        }
        size_t ll = strlen(line);
        if (len + ll + 1 >= sizeof buf) { fprintf(stderr, "input too long\n"); len = 0; cont = 0; continue; }
        memcpy(buf + len, line, ll); len += ll; buf[len] = 0;

        if (setjmp(g_jmp)) {
            if (g_incomplete) { cont = 1; continue; }   /* need more input */
            fprintf(stderr, "error: %s\n", g_err);
            len = 0; cont = 0; continue;
        }
        int n; Stmt *st = parse_program(buf, &n);       /* may longjmp if incomplete */
        for (int i = 0; i < n; i++) exec_stmt(&st[i]);
        free(st);
        len = 0; cont = 0;
    }
    printf("\n");
}

int main(int argc, char **argv) {
    init_globals();
    g_nprim = g_nglob;
    if (setjmp(g_jmp)) { fprintf(stderr, "error: %s\n", g_err); return 1; }

    if (argc >= 4 && !strcmp(argv[1], "--bench")) {
        int iters = atoi(argv[2]);
        char *src = read_file(argv[3]);
        run_bench(iters > 0 ? iters : 1000, src);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "--no-eval")) {
        g_eval_on = 0;
        if (argc < 3) { fprintf(stderr, "usage: typed --no-eval <file>\n"); return 1; }
        char *src = read_file(argv[2]); run_source(src); return 0;
    }
    if (argc >= 2) { char *src = read_file(argv[1]); run_source(src); return 0; }

    repl();
    return 0;
}
