#define null NULL
#define for_to(i, to) for (s32 = 0; i < to; ++i)
#define from_to(i, from, to) for(s32 = from; i < to; ++i)
#define for_rto(i, to) for (s32 = to-1; i >= 0; --i)
#define from_rto(i, from, to) for(s32 = from; i > to; --i)

typedef char s8;
typedef short s16;
typedef int s32;
typedef long long s64;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef float f32;
typedef double f64;

typedef struct Term Term;

struct Term {
  union {
    // Var, church-encoded
    u32 var;

    // Abs
    Term* body;

    // App
    struct {
      Term *abs, *arg;
    };
  };
};

enum TermType {
  LAMBDA_VAR,
  LAMBDA_ABS,
  LAMBDA_APP,
};

#define term_settype(term, type) (Term*)((u64)term |= type)
#define term_gettype(term) (TermType)((u64)term & 0b111)
#define term_deref(term) ((Term*)((u64)term&(~0b111)))
#define term_get(term, get) term_deref(term)->get

int main(int argc, char** argv) {
    Term test = {0};
    Term* a = &test;
}
