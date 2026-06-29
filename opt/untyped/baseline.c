/* baseline.c — readable lambda calculus normalizer.
 *
 * Strategy: classic, obviously-correct approach.
 *   - Terms are an AST with string variable names.
 *   - Reduction is normal-order (leftmost-outermost) to full normal form.
 *   - Substitution is capture-avoiding via fresh-variable renaming.
 *
 * This is intentionally simple, not fast. It is the reference oracle that the
 * optimized evaluators are checked against.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <time.h>

static uint64_t beta_count = 0;

/* ------------------------------------------------------------------ terms */
typedef enum { VAR, LAM, APP } Tag;

typedef struct Term {
    Tag tag;
    union {
        char *var;
        struct { char *param; struct Term *body; } lam;
        struct { struct Term *fun, *arg; } app;
    };
} Term;

static Term *mk(Tag t) {
    Term *x = malloc(sizeof *x);
    x->tag = t;
    return x;
}
static Term *mkvar(const char *s) { Term *t = mk(VAR); t->var = strdup(s); return t; }
static Term *mklam(char *p, Term *b) { Term *t = mk(LAM); t->lam.param = p; t->lam.body = b; return t; }
static Term *mkapp(Term *f, Term *a) { Term *t = mk(APP); t->app.fun = f; t->app.arg = a; return t; }

/* --------------------------------------------------------------- parser */
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
    char *r = malloc(n + 1);
    memcpy(r, s, n);
    r[n] = 0;
    return r;
}

static Term *parse_term(Parser *ps);

static int starts_atom(char c) {
    return c == '\\' || c == '(' || is_ident((unsigned char)c);
}

/* Peek: is the next token the keyword `kw`? Does not consume. */
static int peek_kw(Parser *ps, const char *kw) {
    skip(ps);
    const char *q = ps->p;
    size_t n = strlen(kw);
    if (strncmp(q, kw, n) != 0) return 0;
    return !is_ident((unsigned char)q[n]);
}

static Term *parse_atom(Parser *ps) {
    skip(ps);
    char c = *ps->p;
    if (c == '(') {
        ps->p++;
        Term *t = parse_term(ps);
        skip(ps);
        if (*ps->p == ')') ps->p++;
        return t;
    }
    if (c == '\\' || c == 'L') {  /* L as ascii lambda alias is not used; only \ */
        if (c == '\\') {
            ps->p++;
            /* collect binders until '.' */
            char *names[64]; int n = 0;
            for (;;) {
                skip(ps);
                if (*ps->p == '.') { ps->p++; break; }
                names[n++] = parse_ident(ps);
            }
            Term *body = parse_term(ps);
            for (int i = n - 1; i >= 0; i--) body = mklam(names[i], body);
            return body;
        }
    }
    /* identifier or let */
    char *id = parse_ident(ps);
    if (strcmp(id, "let") == 0) {
        free(id);
        char *name = parse_ident(ps);
        skip(ps);
        if (*ps->p == '=') ps->p++;
        Term *val = parse_term(ps);
        skip(ps);
        /* expect 'in' */
        if (peek_kw(ps, "in")) { char *kw = parse_ident(ps); free(kw); }
        Term *body = parse_term(ps);
        /* let x = v in b   ==>   (\x. b) v   */
        return mkapp(mklam(name, body), val);
    }
    return (Term *){ mkvar(id) };
}

static Term *parse_term(Parser *ps) {
    Term *t = parse_atom(ps);
    for (;;) {
        skip(ps);
        if (!starts_atom(*ps->p)) break;
        if (peek_kw(ps, "in")) break;   /* `in` terminates the enclosing term */
        Term *a = parse_atom(ps);
        t = mkapp(t, a);
    }
    return t;
}

/* ------------------------------------------------------- free variables */
static int occurs_free(const char *x, Term *t) {
    switch (t->tag) {
        case VAR: return strcmp(t->var, x) == 0;
        case LAM: return strcmp(t->lam.param, x) != 0 && occurs_free(x, t->lam.body);
        case APP: return occurs_free(x, t->app.fun) || occurs_free(x, t->app.arg);
    }
    return 0;
}

static int gensym = 0;
static char *fresh(const char *base) {
    char buf[64];
    snprintf(buf, sizeof buf, "%s$%d", base, gensym++);
    return strdup(buf);
}

static Term *copy(Term *t);

/* capture-avoiding substitution: t[x := s] */
static Term *subst(Term *t, const char *x, Term *s) {
    switch (t->tag) {
        case VAR:
            return strcmp(t->var, x) == 0 ? copy(s) : copy(t);
        case APP:
            return mkapp(subst(t->app.fun, x, s), subst(t->app.arg, x, s));
        case LAM:
            if (strcmp(t->lam.param, x) == 0) return copy(t);
            if (occurs_free(t->lam.param, s)) {
                char *nv = fresh(t->lam.param);
                Term *renamed = subst(t->lam.body, t->lam.param, mkvar(nv));
                return mklam(nv, subst(renamed, x, s));
            }
            return mklam(strdup(t->lam.param), subst(t->lam.body, x, s));
    }
    return NULL;
}

static Term *copy(Term *t) {
    switch (t->tag) {
        case VAR: return mkvar(t->var);
        case LAM: return mklam(strdup(t->lam.param), copy(t->lam.body));
        case APP: return mkapp(copy(t->app.fun), copy(t->app.arg));
    }
    return NULL;
}

/* ---------------------------------------------------- normal-order eval */
/* Reduce to weak head normal form by repeatedly contracting the head redex. */
static Term *whnf(Term *t) {
    for (;;) {
        if (t->tag == APP) {
            Term *f = whnf(t->app.fun);
            if (f->tag == LAM) {
                beta_count++;
                t = subst(f->lam.body, f->lam.param, t->app.arg);
            }
            else
                return mkapp(f, t->app.arg);
        } else {
            return t;
        }
    }
}

/* Full normal form: WHNF, then recurse under binders / into arguments. */
static Term *normalize(Term *t) {
    t = whnf(t);
    switch (t->tag) {
        case LAM: return mklam(strdup(t->lam.param), normalize(t->lam.body));
        case APP: return mkapp(normalize(t->app.fun), normalize(t->app.arg));
        default:  return t;
    }
}

/* ----------------------------------------------------------- printing */
static void print_term(Term *t) {
    switch (t->tag) {
        case VAR: fputs(t->var, stdout); break;
        case LAM:
            putchar('\\');
            fputs(t->lam.param, stdout);
            putchar('.');
            print_term(t->lam.body);
            break;
        case APP:
            putchar('(');
            print_term(t->app.fun);
            putchar(' ');
            print_term(t->app.arg);
            putchar(')');
            break;
    }
}

/* If t is a Church numeral \f.\x. f (f ... x), return its value, else -1. */
static long church_value(Term *t) {
    if (t->tag != LAM) return -1;
    char *f = t->lam.param;
    Term *b = t->lam.body;
    if (b->tag != LAM) return -1;
    char *x = b->lam.param;
    Term *body = b->lam.body;
    long n = 0;
    while (body->tag == APP) {
        if (body->app.fun->tag != VAR || strcmp(body->app.fun->var, f) != 0) return -1;
        body = body->app.arg;
        n++;
    }
    if (body->tag == VAR && strcmp(body->var, x) == 0) return n;
    return -1;
}

/* --------------------------------------------------------------- main */
static char *read_all(FILE *f) {
    size_t cap = 1 << 16, len = 0;
    char *buf = malloc(cap);
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
        buf[len++] = (char)c;
    }
    buf[len] = 0;
    return buf;
}

int main(int argc, char **argv) {
    FILE *f = stdin;
    if (argc > 1) {
        f = fopen(argv[1], "rb");
        if (!f) { perror(argv[1]); return 1; }
    }
    char *src = read_all(f);
    Parser ps = { src };
    Term *t = parse_term(&ps);
    clock_t t0 = clock();
    Term *nf = normalize(t);
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    long cv = church_value(nf);
    if (cv >= 0) printf("church %ld\n", cv);
    else { print_term(nf); putchar('\n'); }
    fprintf(stderr, "[baseline] %llu beta in %.3fs = %.2f Mβ/s\n",
            (unsigned long long)beta_count, secs,
            secs > 0 ? beta_count / secs / 1e6 : 0.0);
    return 0;
}
