/* jit.c — a bytecode evaluator for the lambda calculus.
 * ===========================================================================
 *
 * Where clam.c walks the de Bruijn term tree, this engine first *compiles* the
 * program to a flat bytecode for a small strict abstract machine (a
 * call-by-value cousin of the ZINC/ZAM machine used by OCaml's bytecode), then
 * executes that bytecode in a tight dispatch loop.  Benefits over tree-walking:
 *   - no per-node pointer chasing; instructions are contiguous (I-cache);
 *   - proper tail calls for application spines (constant control-stack space);
 *   - a `GRAB` super-instruction reduces `(\.b) a` with no closure allocation.
 *
 * It shares clam.c's value representation (tagged 64-bit values, 16-byte cells,
 * immediate neutral heads) and its iterative read-back, so results are
 * identical; only the evaluator core differs.
 *
 * Roadmap (optimized iteratively): correct switch VM -> computed-goto threading
 * -> super-instructions.
 * ===========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <time.h>

static uint64_t beta_count = 0;

/* ============================ arena (immortal: AST, names, bytecode) ===== */
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

/* fast 16-byte bump allocator for runtime cells (Env, Clo, AppCell) */
static char *R_ptr, *R_end;
static void R_grow(void){ size_t cap=(size_t)1<<30; char*m=malloc(cap); R_ptr=m; R_end=m+cap; }
static inline void *ralloc16(void){ if(R_ptr>=R_end) R_grow(); void*p=R_ptr; R_ptr+=16; return p; }
/* fast 32-byte bump allocator for read-back Tm nodes */
static char *TM_ptr, *TM_end;
static void TM_grow(void){ size_t cap=(size_t)1<<30; char*m=malloc(cap); TM_ptr=m; TM_end=m+cap; }
static inline void *tmalloc(void){ if(TM_ptr>=TM_end) TM_grow(); void*p=TM_ptr; TM_ptr+=32; return p; }

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

/* =========================================================== values =====
 * Same representation as clam.c:
 *   ...000  closure   -> Clo{ uint64_t code; Env* env; }  (code = bytecode addr)
 *   ...001  app-neut  -> AppCell{ V arg; V next; }
 *   ..x011  head-neut -> immediate (free? + level/index)
 */
typedef uint64_t V;
typedef struct Env     { V val; struct Env *next; } Env;
typedef struct Clo     { uint64_t code; Env *env; } Clo;     /* code = pc into bytecode */
typedef struct AppCell { V arg; V next; } AppCell;
static inline int isclo(V v){ return (v&1)==0; }
static inline int ishead(V v){ return (v&2)!=0; }
static inline V       mkclo(uint64_t code, Env*e){ Clo*c=ralloc16(); c->code=code; c->env=e; return (uintptr_t)c; }
static inline Clo    *asclo(V v){ return (Clo*)(uintptr_t)v; }
static inline AppCell*ascell(V v){ return (AppCell*)(uintptr_t)(v&~7ULL); }
static inline V       mkappneu(V arg,V nxt){ AppCell*c=ralloc16(); c->arg=arg; c->next=nxt; return (uintptr_t)c|1; }
#define HEAD_BOUND(lvl)  (((uint64_t)(uint32_t)(lvl)<<4) | 3ULL)
#define HEAD_FREE(fidx)  (((uint64_t)(uint32_t)(fidx)<<4) | 11ULL)
static inline int     head_is_free(V v){ return (v&8)!=0; }
static inline int32_t head_num(V v){ return (int32_t)(v>>4); }
static V *g_gval, *g_fval;

/* ============================================================= bytecode ==
 * Stack abstract machine, accumulator `acc`:
 *   VAR n   acc = env[n]               GLB n  acc = g_gval[n]
 *   FRE n   acc = g_fval[n]            CLO a  acc = closure{code=a, env}
 *   PUSH    vstack.push(acc)
 *   APPLY   f = vstack.pop(); arg = acc;
 *           closure -> push return (pc,env), enter body
 *           neutral -> acc = neutral applied to arg
 *   TAILAPPLY  like APPLY but in tail position (no return frame on closure;
 *              on neutral, returns to caller)
 *   GRAB    env = cons(acc, env)       -- reduce (\.b) a with no closure
 *   RET     restore (pc,env) from return stack (acc is the result)
 *   HALT    stop; acc is the final value
 *
 * `(f a b)` compiles to  <f> PUSH <a> APPLY PUSH <b> APPLY.
 */
enum { OP_VAR, OP_GLB, OP_FRE, OP_CLO, OP_PUSH, OP_APPLY, OP_TAILAPPLY, OP_GRAB, OP_RET, OP_HALT,
       /* super-instructions: a value-producing op fused with PUSH / APPLY /
        * TAILAPPLY, halving the dispatch count of the common `f a` shapes. */
       OP_PUSHVAR, OP_PUSHGLB, OP_PUSHFRE, OP_PUSHCLO,
       OP_APPVAR,  OP_APPGLB,  OP_APPFRE,  OP_APPCLO,
       OP_TAPPVAR, OP_TAPPGLB, OP_TAPPFRE, OP_TAPPCLO,
       /* two-operand fusion: apply an atom function directly to an atom
        * argument with no value stack.  The op encodes (funkind, argkind);
        * the function index is in this instr's `arg`, the argument index in
        * the following slot's `arg` (so both stay full 32-bit).
        * Order within each group: fun in {VAR,GLB} x arg in {VAR,GLB,FRE}. */
       OP_A_VV, OP_A_VG, OP_A_VF, OP_A_GV, OP_A_GG, OP_A_GF,
       OP_T_VV, OP_T_VG, OP_T_VF, OP_T_GV, OP_T_GG, OP_T_GF };
#define OP_A_BASE OP_A_VV
#define OP_T_BASE OP_T_VV
typedef struct { int32_t op, arg; } Instr;
static Instr *code; static int ncode, capcode;
static int emit(int op,int arg){ if(ncode>=capcode){ capcode=capcode?capcode*2:4096; code=realloc(code,capcode*sizeof*code); }
    code[ncode].op=op; code[ncode].arg=arg; return ncode++; }

/* worklist of lambda bodies still to be compiled (and the CLO instr to patch) */
static Tm **wl_body; static int *wl_patch, wln, wlcap;
static void wl_add(Tm*b,int idx){ if(wln>=wlcap){ wlcap=wlcap?wlcap*2:1024;
    wl_body=realloc(wl_body,wlcap*sizeof*wl_body); wl_patch=realloc(wl_patch,wlcap*sizeof*wl_patch); }
    wl_body[wln]=b; wl_patch[wln]=idx; wln++; }

/* compile term t; if `tail`, the value is the result of the enclosing body
 * (emit RET / use TAILAPPLY); otherwise leave the value in acc and fall
 * through. */
static void compile(Tm *t, int tail);
/* Emit "evaluate t, then PUSH it" — fused into one op when t is an atom. */
static void emit_push(Tm *t){
    switch(t->tag){
        case T_VAR:  emit(OP_PUSHVAR,t->ix); break;
        case T_GLB:  emit(OP_PUSHGLB,t->ix); break;
        case T_FREE: emit(OP_PUSHFRE,t->ix); break;
        case T_LAM:{ int idx=emit(OP_PUSHCLO,0); wl_add(t->lam.body,idx); break; }
        default:     compile(t,0); emit(OP_PUSH,0); break;   /* nested application */
    }
}
/* Emit "evaluate arg t, then (TAIL)APPLY the pushed function" — fused for atoms. */
static void emit_apply(Tm *t, int tail){
    switch(t->tag){
        case T_VAR:  emit(tail?OP_TAPPVAR:OP_APPVAR, t->ix); break;
        case T_GLB:  emit(tail?OP_TAPPGLB:OP_APPGLB, t->ix); break;
        case T_FREE: emit(tail?OP_TAPPFRE:OP_APPFRE, t->ix); break;
        case T_LAM:{ int idx=emit(tail?OP_TAPPCLO:OP_APPCLO, 0); wl_add(t->lam.body,idx); break; }
        default:     compile(t,0); emit(tail?OP_TAILAPPLY:OP_APPLY,0); break;
    }
}
static void compile(Tm *t, int tail){
    switch(t->tag){
        case T_VAR:  emit(OP_VAR,t->ix);  if(tail) emit(OP_RET,0); break;
        case T_GLB:  emit(OP_GLB,t->ix);  if(tail) emit(OP_RET,0); break;
        case T_FREE: emit(OP_FRE,t->ix);  if(tail) emit(OP_RET,0); break;
        case T_LAM:{ int idx=emit(OP_CLO,0); wl_add(t->lam.body,idx); if(tail) emit(OP_RET,0); break; }
        case T_APP:{
            Tm*f=t->app.fun;
            if(f->tag==T_LAM){                         /* (\.b) a : GRAB, no closure */
                compile(t->app.arg,0);
                emit(OP_GRAB,0);
                compile(f->lam.body,tail);             /* body inherits tail position */
            } else {
                Tm *a = t->app.arg;
                int fk = f->tag==T_VAR ? 0 : f->tag==T_GLB ? 1 : -1;
                int ak = a->tag==T_VAR ? 0 : a->tag==T_GLB ? 1 : a->tag==T_FREE ? 2 : -1;
                if(fk>=0 && ak>=0){              /* atom function applied to atom arg */
                    emit((tail?OP_T_BASE:OP_A_BASE) + fk*3 + ak, f->ix);
                    emit(0, a->ix);             /* second slot: argument index */
                } else {
                    emit_push(f);
                    emit_apply(a, tail);
                }
            }
            break;
        }
    }
}

/* ============================================================= VM stacks */
static V    *vstack; static int vsp, vcap;       /* saved function values */
typedef struct { int pc; Env *env; } RFrame;
static RFrame *rstack; static int rsp, rcap;     /* return continuations */
static inline void vpush(V x){ if(vsp>=vcap){ vcap=vcap?vcap*2:1<<16; vstack=realloc(vstack,vcap*sizeof*vstack);} vstack[vsp++]=x; }
static inline void rpush(int pc, Env*e){ if(rsp>=rcap){ rcap=rcap?rcap*2:1<<16; rstack=realloc(rstack,rcap*sizeof*rstack);} rstack[rsp].pc=pc; rstack[rsp].env=e; rsp++; }

/* Execute starting at `pc` in `env`, returning when the return stack drops to
 * the level it had on entry (RET of the entered body) or on HALT.
 *
 * Dispatch is computed-goto threaded: each handler ends by jumping straight to
 * the next handler, which predicts far better than a single switch. */
static V run(int pc, Env *env){
    static const void *tbl[] = {
        &&L_VAR,&&L_GLB,&&L_FRE,&&L_CLO,&&L_PUSH,
        &&L_APPLY,&&L_TAILAPPLY,&&L_GRAB,&&L_RET,&&L_HALT,
        &&L_PUSHVAR,&&L_PUSHGLB,&&L_PUSHFRE,&&L_PUSHCLO,
        &&L_APPVAR,&&L_APPGLB,&&L_APPFRE,&&L_APPCLO,
        &&L_TAPPVAR,&&L_TAPPGLB,&&L_TAPPFRE,&&L_TAPPCLO,
        &&L_A_VV,&&L_A_VG,&&L_A_VF,&&L_A_GV,&&L_A_GG,&&L_A_GF,
        &&L_T_VV,&&L_T_VG,&&L_T_VF,&&L_T_GV,&&L_T_GG,&&L_T_GF };
    int rbase = rsp;
    V acc = 0, av, fn;
    Instr in;
    #define NEXT() do { in = code[pc++]; goto *tbl[in.op]; } while(0)
    #define ENVAT(n) ({ Env*e=env; for(int i=(n);i>0;i--) e=e->next; e->val; })
    #define ARG2 (code[pc++].arg)              /* read the fused second operand slot */
    NEXT();

    /* value into accumulator */
    L_VAR:  acc=ENVAT(in.arg); NEXT();
    L_GLB:  acc=g_gval[in.arg]; NEXT();
    L_FRE:  acc=g_fval[in.arg]; NEXT();
    L_CLO:  acc=mkclo((uint64_t)in.arg, env); NEXT();

    /* push a function value (plain + fused) */
    L_PUSH:    vpush(acc); NEXT();
    L_PUSHVAR: vpush(ENVAT(in.arg)); NEXT();
    L_PUSHGLB: vpush(g_gval[in.arg]); NEXT();
    L_PUSHFRE: vpush(g_fval[in.arg]); NEXT();
    L_PUSHCLO: vpush(mkclo((uint64_t)in.arg, env)); NEXT();

    /* GRAB: reduce (\.b) a with no closure */
    L_GRAB: { Env*ne=ralloc16(); ne->val=acc; ne->next=env; env=ne; beta_count++; } NEXT();

    /* APPLY family: set `fn` (function) and `av` (argument), then jump to the
     * shared APPLY_REG / TAPP_REG.  The single-fused ops take the function off
     * the value stack; the two-operand APP2/TAPP2 ops read both inline. */
    L_APPLY:   av=acc;                         fn=vstack[--vsp]; goto APPLY_REG;
    L_APPVAR:  av=ENVAT(in.arg);               fn=vstack[--vsp]; goto APPLY_REG;
    L_APPGLB:  av=g_gval[in.arg];              fn=vstack[--vsp]; goto APPLY_REG;
    L_APPFRE:  av=g_fval[in.arg];              fn=vstack[--vsp]; goto APPLY_REG;
    L_APPCLO:  av=mkclo((uint64_t)in.arg,env); fn=vstack[--vsp]; goto APPLY_REG;
    L_A_VV: fn=ENVAT(in.arg);   av=ENVAT(ARG2);   goto APPLY_REG;
    L_A_VG: fn=ENVAT(in.arg);   av=g_gval[ARG2];  goto APPLY_REG;
    L_A_VF: fn=ENVAT(in.arg);   av=g_fval[ARG2];  goto APPLY_REG;
    L_A_GV: fn=g_gval[in.arg];  av=ENVAT(ARG2);   goto APPLY_REG;
    L_A_GG: fn=g_gval[in.arg];  av=g_gval[ARG2];  goto APPLY_REG;
    L_A_GF: fn=g_gval[in.arg];  av=g_fval[ARG2];  goto APPLY_REG;
    APPLY_REG:
        if(isclo(fn)){ Clo*c=asclo(fn); beta_count++;
            rpush(pc, env);
            Env*ne=ralloc16(); ne->val=av; ne->next=c->env; env=ne; pc=(int)c->code;
        } else acc=mkappneu(av,fn);
        NEXT();

    L_TAILAPPLY: av=acc;                         fn=vstack[--vsp]; goto TAPP_REG;
    L_TAPPVAR:   av=ENVAT(in.arg);               fn=vstack[--vsp]; goto TAPP_REG;
    L_TAPPGLB:   av=g_gval[in.arg];              fn=vstack[--vsp]; goto TAPP_REG;
    L_TAPPFRE:   av=g_fval[in.arg];              fn=vstack[--vsp]; goto TAPP_REG;
    L_TAPPCLO:   av=mkclo((uint64_t)in.arg,env); fn=vstack[--vsp]; goto TAPP_REG;
    L_T_VV: fn=ENVAT(in.arg);   av=ENVAT(ARG2);   goto TAPP_REG;
    L_T_VG: fn=ENVAT(in.arg);   av=g_gval[ARG2];  goto TAPP_REG;
    L_T_VF: fn=ENVAT(in.arg);   av=g_fval[ARG2];  goto TAPP_REG;
    L_T_GV: fn=g_gval[in.arg];  av=ENVAT(ARG2);   goto TAPP_REG;
    L_T_GG: fn=g_gval[in.arg];  av=g_gval[ARG2];  goto TAPP_REG;
    L_T_GF: fn=g_gval[in.arg];  av=g_fval[ARG2];  goto TAPP_REG;
    TAPP_REG:
        if(isclo(fn)){ Clo*c=asclo(fn); beta_count++;
            Env*ne=ralloc16(); ne->val=av; ne->next=c->env; env=ne; pc=(int)c->code;
            NEXT();
        }
        acc=mkappneu(av,fn);
        if(rsp==rbase) return acc;
        { RFrame f=rstack[--rsp]; pc=f.pc; env=f.env; }
        NEXT();
    L_RET:
        if(rsp==rbase) return acc;
        { RFrame f=rstack[--rsp]; pc=f.pc; env=f.env; }
        NEXT();
    L_HALT: return acc;
    #undef NEXT
    #undef ENVAT
    #undef ARG2
}
static V apply(V fv, V av){
    if(isclo(fv)){ Clo*c=asclo(fv); beta_count++;
        Env*ne=ralloc16(); ne->val=av; ne->next=c->env; return run((int)c->code, ne); }
    return mkappneu(av,fv);
}

/* ====================================================== read-back (quote) */
typedef struct { V v; int depth; Tm **slot; } QTask;
static Tm *quote(V v0,int depth0){
    Tm*root=NULL; size_t cap=4096,sp=0; QTask*st=malloc(cap*sizeof*st);
    st[sp++]=(QTask){v0,depth0,&root};
    while(sp){
        QTask q=st[--sp]; V v=q.v;
        if(isclo(v)){
            Tm*lam=tmalloc(); lam->tag=T_LAM; *q.slot=lam;
            V arg=HEAD_BOUND(q.depth);
            if(sp+1>cap){cap*=2;st=realloc(st,cap*sizeof*st);}
            st[sp++]=(QTask){apply(v,arg),q.depth+1,&lam->lam.body};
        } else {
            Tm**slot=q.slot; V cur=v;
            while(!ishead(cur)){
                AppCell*c=ascell(cur);
                Tm*app=tmalloc();app->tag=T_APP;*slot=app;
                if(sp+1>cap){cap*=2;st=realloc(st,cap*sizeof*st);}
                st[sp++]=(QTask){c->arg,q.depth,&app->app.arg};
                slot=&app->app.fun; cur=c->next;
            }
            Tm*hd=tmalloc(); hd->tag=T_VAR;
            hd->ix = head_is_free(cur) ? -(head_num(cur)+2) : (q.depth - head_num(cur) - 1);
            *slot=hd;
        }
    }
    free(st); return root;
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
    FILE*f=stdin; if(argc>1){ f=fopen(argv[1],"rb"); if(!f){perror(argv[1]);return 1;} }
    char*src=read_all(f); Parser ps={src};
    NTerm*body=parse_program(&ps);
    Tm**gtm=malloc(sizeof(Tm*)*(g_ndef?g_ndef:1));
    for(int i=0;i<g_ndef;i++) gtm[i]=to_db(g_gdef[i],NULL,i);
    Tm*btm=to_db(body,NULL,g_ndef);

    /* compile: globals, then body, each ending in HALT; then all lambda bodies */
    int *gstart=malloc(sizeof(int)*(g_ndef?g_ndef:1));
    for(int i=0;i<g_ndef;i++){ gstart[i]=ncode; compile(gtm[i],0); emit(OP_HALT,0); }
    int bstart=ncode; compile(btm,0); emit(OP_HALT,0);
    for(int w=0; w<wln; w++){ int addr=ncode; code[wl_patch[w]].arg=addr; compile(wl_body[w],1); }

    g_fval=malloc(sizeof(V)*(g_nfree?g_nfree:1));
    for(int i=0;i<g_nfree;i++) g_fval[i]=HEAD_FREE(i);

    clock_t t0=clock();
    g_gval=malloc(sizeof(V)*(g_ndef?g_ndef:1));
    for(int i=0;i<g_ndef;i++) g_gval[i]=run(gstart[i],NULL);
    V v=run(bstart,NULL);
    Tm*nf=quote(v,0);
    double secs=(double)(clock()-t0)/CLOCKS_PER_SEC;
    long cv=church_value(nf);
    if(cv>=0) printf("church %ld\n",cv); else { print_db(nf,0); putchar('\n'); }
    fprintf(stderr,"[jit] %llu beta in %.3fs = %.2f Mβ/s\n",
            (unsigned long long)beta_count,secs, secs>0?beta_count/secs/1e6:0.0);
    return 0;
}
