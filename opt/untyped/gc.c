/* gc.c — lambda normalizer with a cache-resident copying GC.
 *
 * Bottleneck found by measurement: the environment machine allocates one Env
 * cell per beta and never frees it, so a fib(25) run streams ~6.6 GB of
 * never-reused memory.  Reads of captured environments then miss cache.
 *
 * Fix:
 *   - Reify the machine state (explicit continuation stack) so all GC roots
 *     are discoverable.  No C recursion in the hot eval loop.
 *   - A semispace copying (Cheney) GC sized to fit L3.  The live set of a
 *     strict reduction is tiny (call depth), so the working region recycles a
 *     few MB and stays cache-resident; the heap grows only if live data
 *     genuinely demands it (e.g. materializing a huge normal form).
 *
 * All managed cells are a uniform 24 bytes: [header][word1][word2].
 * Checked against nbe.c / baseline.c for correctness.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <time.h>

static uint64_t beta_count = 0;

/* ============================= arena (immortal: terms, names, AST) ====== */
typedef struct Block { struct Block *next; size_t used, cap; char *mem; } Block;
typedef struct { Block *head; } Arena;
static Arena g_arena;
static void *arena_alloc(Arena *a, size_t n) {
    n = (n + 15) & ~(size_t)15;
    Block *b = a->head;
    if (!b || b->used + n > b->cap) {
        size_t cap = b ? b->cap * 2 : (size_t)1 << 22;
        if (cap < n) cap = n;
        b = malloc(sizeof *b); b->mem = malloc(cap);
        b->used = 0; b->cap = cap; b->next = a->head; a->head = b;
    }
    void *p = b->mem + b->used; b->used += n; return p;
}
#define NEW(T) ((T *)arena_alloc(&g_arena, sizeof(T)))

/* ============================================================ named AST */
typedef enum { N_VAR, N_LAM, N_APP } NTag;
typedef struct NTerm { NTag tag; union { char *var;
    struct { char *param; struct NTerm *body; } lam;
    struct { struct NTerm *fun, *arg; } app; }; } NTerm;
static NTerm *nvar(char *s){NTerm*t=NEW(NTerm);t->tag=N_VAR;t->var=s;return t;}
static NTerm *nlam(char *p,NTerm*b){NTerm*t=NEW(NTerm);t->tag=N_LAM;t->lam.param=p;t->lam.body=b;return t;}
static NTerm *napp(NTerm*f,NTerm*a){NTerm*t=NEW(NTerm);t->tag=N_APP;t->app.fun=f;t->app.arg=a;return t;}

/* ================================================================ parser */
typedef struct { const char *p; } Parser;
static void skip(Parser *ps){ for(;;){ while(isspace((unsigned char)*ps->p))ps->p++;
    if(*ps->p=='#'){while(*ps->p&&*ps->p!='\n')ps->p++;} else break; } }
static int is_ident(int c){return isalnum(c)||c=='_'||c=='\''||c=='?';}
static char *parse_ident(Parser *ps){ skip(ps); const char*s=ps->p;
    while(is_ident((unsigned char)*ps->p))ps->p++;
    size_t n=(size_t)(ps->p-s); char*r=arena_alloc(&g_arena,n+1);
    memcpy(r,s,n); r[n]=0; return r; }
static int starts_atom(char c){return c=='\\'||c=='('||is_ident((unsigned char)c);}
static int peek_kw(Parser *ps,const char*kw){ skip(ps); size_t n=strlen(kw);
    return strncmp(ps->p,kw,n)==0 && !is_ident((unsigned char)ps->p[n]); }
static NTerm *parse_term(Parser *ps);
static NTerm *parse_atom(Parser *ps){
    skip(ps); char c=*ps->p;
    if(c=='('){ ps->p++; NTerm*t=parse_term(ps); skip(ps); if(*ps->p==')')ps->p++; return t; }
    if(c=='\\'){ ps->p++; char*names[64]; int n=0;
        for(;;){ skip(ps); if(*ps->p=='.'){ps->p++;break;} names[n++]=parse_ident(ps); }
        NTerm*body=parse_term(ps);
        for(int i=n-1;i>=0;i--) body=nlam(names[i],body); return body; }
    char*id=parse_ident(ps);
    if(strcmp(id,"let")==0){ char*name=parse_ident(ps); skip(ps); if(*ps->p=='=')ps->p++;
        NTerm*val=parse_term(ps); if(peek_kw(ps,"in"))parse_ident(ps);
        NTerm*body=parse_term(ps); return napp(nlam(name,body),val); }
    return nvar(id);
}
static NTerm *parse_term(Parser *ps){ NTerm*t=parse_atom(ps);
    for(;;){ skip(ps); if(!starts_atom(*ps->p))break; if(peek_kw(ps,"in"))break;
        t=napp(t,parse_atom(ps)); } return t; }
#define MAXG 4096
static char *g_gname[MAXG]; static NTerm *g_gdef[MAXG]; static int g_ndef=0;
static NTerm *parse_program(Parser *ps){
    for(;;){ if(!peek_kw(ps,"let")) break; parse_ident(ps);
        char*name=parse_ident(ps); skip(ps); if(*ps->p=='=')ps->p++;
        NTerm*val=parse_term(ps); if(peek_kw(ps,"in"))parse_ident(ps);
        g_gname[g_ndef]=name; g_gdef[g_ndef]=val; g_ndef++; }
    return parse_term(ps);
}

/* ====================================================== de Bruijn terms */
typedef enum { T_VAR, T_GLB, T_FREE, T_LAM, T_APP } TTag;
typedef struct Tm { TTag tag; union { int ix;
    struct { struct Tm *body; } lam;
    struct { struct Tm *fun, *arg; } app; }; } Tm;
static Tm *tnode(TTag tg,int i){Tm*t=NEW(Tm);t->tag=tg;t->ix=i;return t;}
static Tm *tlam(Tm*b){Tm*t=NEW(Tm);t->tag=T_LAM;t->lam.body=b;return t;}
static Tm *tapp(Tm*f,Tm*a){Tm*t=NEW(Tm);t->tag=T_APP;t->app.fun=f;t->app.arg=a;return t;}
static char *g_free[1024]; static int g_nfree=0;
static int free_index(char*name){ for(int i=0;i<g_nfree;i++) if(strcmp(g_free[i],name)==0) return i;
    g_free[g_nfree]=name; return g_nfree++; }
typedef struct Scope { char *name; struct Scope *next; } Scope;
static Tm *to_db(NTerm *t, Scope *sc, int nglob){
    switch(t->tag){
        case N_VAR:{ int i=0;
            for(Scope*s=sc;s;s=s->next,i++) if(strcmp(s->name,t->var)==0) return tnode(T_VAR,i);
            for(int g=nglob-1;g>=0;g--) if(strcmp(g_gname[g],t->var)==0) return tnode(T_GLB,g);
            return tnode(T_FREE,free_index(t->var)); }
        case N_LAM:{ Scope s={t->lam.param,sc}; return tlam(to_db(t->lam.body,&s,nglob)); }
        case N_APP: return tapp(to_db(t->app.fun,sc,nglob),to_db(t->app.arg,sc,nglob));
    }
    return NULL;
}

/* ====================================================== managed heap =====
 * Value V = tagged pointer (bit0: 0=closure, 1=neutral) to a 24-byte cell.
 * Cell: [hdr][w1][w2].  hdr = type<<1 (bit0=0), or forwarding addr|1 in GC.
 */
typedef uint64_t V;
enum { OT_ENV, OT_CLO, OT_NEU, OT_SPINE };
typedef struct { uint64_t hdr, w1, w2; } Cell;          /* 24 bytes */

typedef struct { char *base, *ptr, *end; size_t cap; } Space;
static Space g_sp[2]; static int g_cur;

#define CELL 24
static inline Cell *cell_at(void *p){ return (Cell*)p; }

/* runtime accessors */
static inline int  isclo(V v){ return (v & 1) == 0; }
static inline Cell*ascell(V v){ return (Cell*)(uintptr_t)(v & ~1ULL); }
static inline V    tagclo(Cell*c){ return (uintptr_t)c; }
static inline V    tagneu(Cell*c){ return (uintptr_t)c | 1ULL; }

/* ---- roots ---- */
typedef enum { K_ARG, K_APP } KKind;
typedef struct { KKind kind; Tm *arg; Cell *env; V fval; } KFrame;
static KFrame *g_K; static int g_ksize, g_kcap;
static V   *g_gval; static int g_gdone = 0;   /* globals computed so far */
static V   *g_fval;
static Cell *g_envroot;                        /* live Env register at safepoint */
static V    g_tmp[8]; static int g_ntmp = 0;   /* ad-hoc temp roots */
/* quote task stack (roots while quoting) */
typedef struct { V v; int depth; Tm **slot; } QTask;
static QTask *g_qs; static int g_qsize = 0, g_qcap = 0;

/* ---- Cheney copy ---- */
static char *gc_free;
static void *copy_cell(void *p){
    if(!p) return NULL;
    Cell *o = (Cell*)p;
    if(o->hdr & 1) return (void*)(uintptr_t)(o->hdr & ~1ULL);   /* forwarded */
    Cell *n = (Cell*)gc_free; gc_free += CELL;
    n->hdr = o->hdr; n->w1 = o->w1; n->w2 = o->w2;
    o->hdr = (uintptr_t)n | 1ULL;
    return n;
}
static V copy_v(V v){ uint64_t t = v & 1; return ((uint64_t)(uintptr_t)copy_cell(ascell(v))) | t; }

static void cheney_scan(char *start){
    char *scan = start;
    while(scan < gc_free){
        Cell *o = (Cell*)scan;
        switch(o->hdr >> 1){
            case OT_ENV:   o->w1 = copy_v(o->w1); o->w2 = (uintptr_t)copy_cell((void*)o->w2); break;
            case OT_CLO:   o->w2 = (uintptr_t)copy_cell((void*)o->w2); break; /* w1=Tm* immortal */
            case OT_NEU:   o->w2 = (uintptr_t)copy_cell((void*)o->w2); break; /* w1=packed */
            case OT_SPINE: o->w1 = copy_v(o->w1); o->w2 = (uintptr_t)copy_cell((void*)o->w2); break;
        }
        scan += CELL;
    }
}
static size_t gc_collect(size_t want_cap){
    int to = 1 - g_cur;
    if(g_sp[to].cap < want_cap){
        free(g_sp[to].base);
        g_sp[to].base = malloc(want_cap);
        g_sp[to].cap  = want_cap;
    }
    gc_free = g_sp[to].base;
    char *start = gc_free;
    /* forward all roots */
    if(g_envroot) g_envroot = copy_cell(g_envroot);
    for(int i=0;i<g_ntmp;i++)  g_tmp[i]  = copy_v(g_tmp[i]);
    for(int i=0;i<g_ksize;i++){ KFrame*f=&g_K[i];
        if(f->kind==K_ARG) f->env = copy_cell(f->env);
        else               f->fval = copy_v(f->fval); }
    for(int i=0;i<g_gdone;i++) g_gval[i] = copy_v(g_gval[i]);
    for(int i=0;i<g_nfree;i++) g_fval[i] = copy_v(g_fval[i]);
    for(int i=0;i<g_qsize;i++) g_qs[i].v = copy_v(g_qs[i].v);
    cheney_scan(start);
    g_sp[to].ptr = gc_free; g_sp[to].end = g_sp[to].base + g_sp[to].cap;
    g_cur = to;
    return (size_t)(gc_free - g_sp[to].base);   /* live bytes */
}
/* ensure room for `cells` 24-byte allocations; GC and/or grow as needed */
static void reserve(int cells){
    size_t need = (size_t)cells * CELL;
    if(g_sp[g_cur].ptr + need <= g_sp[g_cur].end) return;
    size_t cap = g_sp[g_cur].cap;
    for(;;){
        size_t live = gc_collect(cap);
        if(g_sp[g_cur].ptr + need <= g_sp[g_cur].end && live <= cap/2) return;
        cap = (live + need) * 4 + (1u<<20);     /* grow */
        if(cap <= g_sp[g_cur].cap) cap = g_sp[g_cur].cap * 2;
    }
}
static inline Cell *halloc(int type){
    Cell *c = (Cell*)g_sp[g_cur].ptr; g_sp[g_cur].ptr += CELL;
    c->hdr = (uint64_t)type << 1; return c;
}
static inline V mkclo(Tm*body, Cell*env){ Cell*c=halloc(OT_CLO); c->w1=(uintptr_t)body; c->w2=(uintptr_t)env; return tagclo(c); }
static inline Cell *mkenv(V val, Cell*next){ Cell*c=halloc(OT_ENV); c->w1=val; c->w2=(uintptr_t)next; return c; }
static inline V mkneu(int32_t lvl,int32_t fidx,Cell*sp){ Cell*c=halloc(OT_NEU);
    c->w1=((uint64_t)(uint32_t)lvl<<32)|(uint32_t)fidx; c->w2=(uintptr_t)sp; return tagneu(c); }
static inline V mkspine(V val, Cell*next){ Cell*c=halloc(OT_SPINE); c->w1=val; c->w2=(uintptr_t)next; return (uintptr_t)c; }

/* =========================================================== machine ==== */
static void kpush(KKind k, Tm*arg, Cell*env, V fval){
    if(g_ksize>=g_kcap){ g_kcap = g_kcap?g_kcap*2:4096; g_K=realloc(g_K,g_kcap*sizeof*g_K); }
    KFrame*f=&g_K[g_ksize++]; f->kind=k; f->arg=arg; f->env=env; f->fval=fval;
}
/* run term t in env to a value (WHNF in the value domain), strict in args */
static V run(Tm *t, Cell *env){
    int base = g_ksize;
    V val = 0;
    enum { EVAL, RET } mode = EVAL;
    for(;;){
        if(mode == EVAL){
            if(g_sp[g_cur].ptr + 4*CELL > g_sp[g_cur].end){ g_envroot=env; reserve(4); env=g_envroot; g_envroot=NULL; }
            switch(t->tag){
                case T_VAR:{ Cell*e=env; for(int i=t->ix;i>0;i--) e=(Cell*)e->w2; val=e->w1; mode=RET; break; }
                case T_GLB:  val=g_gval[t->ix]; mode=RET; break;
                case T_FREE: val=g_fval[t->ix]; mode=RET; break;
                case T_LAM:  val=mkclo(t->lam.body, env); mode=RET; break;
                case T_APP:
                    if(t->app.fun->tag==T_LAM){          /* (\.b) a : syntactic redex */
                        kpush(K_APP, NULL, NULL, mkclo(t->app.fun->lam.body, env));
                        t=t->app.arg;                    /* eval arg next, stay EVAL */
                    } else {
                        kpush(K_ARG, t->app.arg, env, 0);
                        t=t->app.fun;                    /* eval function */
                    }
                    break;
            }
        } else { /* RET: value in `val` */
            if(g_ksize==base) return val;
            KFrame*f=&g_K[g_ksize-1];
            if(f->kind==K_ARG){
                Cell*e=f->env; f->kind=K_APP; f->fval=val; f->env=NULL;
                t=f->arg; env=e; mode=EVAL;
            } else {
                V fv=f->fval; g_ksize--;
                if(isclo(fv)){
                    Cell*c=ascell(fv); beta_count++;
                    if(g_sp[g_cur].ptr + CELL > g_sp[g_cur].end){ g_tmp[0]=fv; g_tmp[1]=val; g_ntmp=2; reserve(1); fv=g_tmp[0]; val=g_tmp[1]; g_ntmp=0; c=ascell(fv); }
                    env = mkenv(val, (Cell*)c->w2);
                    t = (Tm*)c->w1; mode=EVAL;
                } else {
                    if(g_sp[g_cur].ptr + 2*CELL > g_sp[g_cur].end){ g_tmp[0]=fv; g_tmp[1]=val; g_ntmp=2; reserve(2); fv=g_tmp[0]; val=g_tmp[1]; g_ntmp=0; }
                    Cell*nc=ascell(fv);
                    int32_t lvl=(int32_t)(nc->w1>>32), fidx=(int32_t)(nc->w1&0xffffffff);
                    Cell*newsp=(Cell*)(uintptr_t)mkspine(val,(Cell*)nc->w2);
                    val = mkneu(lvl,fidx,newsp);
                    mode=RET;
                }
            }
        }
    }
}
static V apply(V fv, V av){
    if(isclo(fv)){
        g_tmp[0]=fv; g_tmp[1]=av; g_ntmp=2; reserve(1); fv=g_tmp[0]; av=g_tmp[1]; g_ntmp=0;
        Cell*c=ascell(fv); beta_count++;
        Cell*env=mkenv(av,(Cell*)c->w2);
        return run((Tm*)c->w1, env);
    }
    g_tmp[0]=fv; g_tmp[1]=av; g_ntmp=2; reserve(2); fv=g_tmp[0]; av=g_tmp[1]; g_ntmp=0;
    Cell*nc=ascell(fv);
    int32_t lvl=(int32_t)(nc->w1>>32), fidx=(int32_t)(nc->w1&0xffffffff);
    Cell*newsp=(Cell*)(uintptr_t)mkspine(av,(Cell*)nc->w2);
    return mkneu(lvl,fidx,newsp);
}

/* ====================================================== readback (quote) */
static Tm *quote(V v0,int depth0){
    Tm*root=NULL;
    g_qcap=4096; g_qs=malloc(g_qcap*sizeof*g_qs); g_qsize=0;
    g_qs[g_qsize++]=(QTask){v0,depth0,&root};
    while(g_qsize){
        QTask q=g_qs[--g_qsize];        /* note: popped; re-rooted via g_tmp below */
        if(isclo(q.v)){
            Tm*lam=NEW(Tm); lam->tag=T_LAM; *q.slot=lam;
            g_tmp[0]=q.v; g_ntmp=1; reserve(1);        /* room for fresh var */
            V cv=g_tmp[0]; g_ntmp=0;
            V arg=mkneu(q.depth,-1,NULL);
            V body=apply(cv,arg);
            if(g_qsize>=g_qcap){ g_qcap*=2; g_qs=realloc(g_qs,g_qcap*sizeof*g_qs); }
            g_qs[g_qsize++]=(QTask){body,q.depth+1,&lam->lam.body};
        } else {
            Cell*n=ascell(q.v);
            int32_t lvl=(int32_t)(n->w1>>32), fidx=(int32_t)(n->w1&0xffffffff);
            Tm**slot=q.slot;
            for(Cell*s=(Cell*)n->w2; s; s=(Cell*)s->w2){
                Tm*app=NEW(Tm);app->tag=T_APP;*slot=app;
                if(g_qsize>=g_qcap){ g_qcap*=2; g_qs=realloc(g_qs,g_qcap*sizeof*g_qs); }
                g_qs[g_qsize++]=(QTask){s->w1,q.depth,&app->app.arg};
                slot=&app->app.fun;
            }
            if(fidx>=0) *slot=tnode(T_VAR,-(fidx+2));
            else        *slot=tnode(T_VAR,q.depth - lvl - 1);
        }
    }
    free(g_qs); g_qs=NULL; g_qsize=0;
    return root;
}

/* ============================================================== output */
static void print_db(Tm*t,int depth){
    switch(t->tag){
        case T_VAR: if(t->ix<-1) fputs(g_free[-(t->ix)-2],stdout);
                    else printf("%c",'a'+(depth-1-t->ix)%26); break;
        case T_LAM: printf("\\%c.",'a'+depth%26); print_db(t->lam.body,depth+1); break;
        case T_APP: putchar('('); print_db(t->app.fun,depth); putchar(' ');
                    print_db(t->app.arg,depth); putchar(')'); break;
        default: break;
    }
}
static long church_value(Tm*t){
    if(t->tag!=T_LAM)return -1; Tm*b=t->lam.body; if(b->tag!=T_LAM)return -1;
    Tm*body=b->lam.body; long n=0;
    while(body->tag==T_APP){ if(body->app.fun->tag!=T_VAR||body->app.fun->ix!=1)return -1;
        body=body->app.arg; n++; }
    return (body->tag==T_VAR&&body->ix==0)?n:-1;
}

/* ================================================================ main */
static char *read_all(FILE*f){ size_t cap=1<<16,len=0; char*b=malloc(cap); int c;
    while((c=fgetc(f))!=EOF){ if(len+1>=cap){cap*=2;b=realloc(b,cap);} b[len++]=(char)c; }
    b[len]=0; return b; }

int main(int argc,char**argv){
    size_t heap = 16u<<20;   /* 16 MB semispaces — fits L3, recycled hot */
    FILE*f=stdin; if(argc>1){ f=fopen(argv[1],"rb"); if(!f){perror(argv[1]);return 1;} }
    char*src=read_all(f); Parser ps={src};
    NTerm*body=parse_program(&ps);
    Tm**gtm=malloc(sizeof(Tm*)*(g_ndef?g_ndef:1));
    for(int i=0;i<g_ndef;i++) gtm[i]=to_db(g_gdef[i],NULL,i);
    Tm*btm=to_db(body,NULL,g_ndef);

    g_sp[0].base=malloc(heap); g_sp[0].cap=heap; g_sp[0].ptr=g_sp[0].base; g_sp[0].end=g_sp[0].base+heap;
    g_sp[1].base=malloc(heap); g_sp[1].cap=heap; g_cur=0;

    g_gval=malloc(sizeof(V)*(g_ndef?g_ndef:1));
    g_fval=malloc(sizeof(V)*(g_nfree?g_nfree:1));
    reserve(g_nfree+1);                       /* create all free neutrals w/o GC */
    for(int i=0;i<g_nfree;i++) g_fval[i]=mkneu(0,i,NULL);

    clock_t t0=clock();
    for(int i=0;i<g_ndef;i++){ g_gval[i]=run(gtm[i],NULL); g_gdone=i+1; }
    V v=run(btm,NULL);
    Tm*nf=quote(v,0);
    double secs=(double)(clock()-t0)/CLOCKS_PER_SEC;
    long cv=church_value(nf);
    if(cv>=0) printf("church %ld\n",cv); else { print_db(nf,0); putchar('\n'); }
    fprintf(stderr,"[gc] %llu beta in %.3fs = %.2f Mβ/s\n",
            (unsigned long long)beta_count,secs, secs>0?beta_count/secs/1e6:0.0);
    return 0;
}
