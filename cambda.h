#pragma once
#include "lib/ol.h"

typedef struct CaExpr CaExpr;
typedef struct CaType CaType;
typedef struct CaState CaState;
typedef struct CaError CaError;
typedef struct CaSpan CaSpan;

typedef enum CaErrorKind CaErrorKind;
typedef enum CaExprKind CaExprKind;

#define CaSpan_make(line_, col_, len_)                                         \
  (CaSpan) { .line = line_, .col = col_, .len = len_ }

struct CaSpan {
  u32 line;
  u32 col;
  u32 len;
};

enum CaErrorKind {
  CAERR_OK,
  CAERR_EXPECTED_EQ,
  CAERR_UNEXPECTED_EOF,
};

struct CaError {
  CaErrorKind kind;
  CaSpan loc;
};

#define CaError_make(error_, line_, col_, len_)                                \
  (CaError) {                                                                  \
    .loc.line = line_, .loc.col = col_, .loc.len = len_, .kind = error_        \
  }
#define CaError_here(error_, len_) (CaError) {.loc.line=ctx->cur_loc.line, .loc.col=ctx->cur_loc.col, .loc.len=len_, .kind=error_}

#define CaError_ok() CaError_make(CAERR_OK, 0, 0, 0)
#define CaError_OutOfMem() CaError_make(CAERR_OUT_OF_MEM, 0, 0, 0)
#define CaError_EOF()                                                          \
  (CaError) { .loc = ctx->cur_loc, .kind = CAERR_UNEXPECTED_EOF }
  
struct CaType {
  u64 id;
};

enum CaExprKind {
  EXPR_INVALID,
};

struct CaExpr {
  CaExprKind kind;
  CaSpan loc;
  union {
    struct {
      olHash hash;
    } var;
    
    struct {
      olHash param;
      CaExpr* expr;
    } abs;
    
    struct {
      CaExpr* fn;
      CaExpr* arg;
    } app;
  };
  CaType type;
};

struct CaState {
  char* cur;
  olArena arena;
  CaSpan cur_loc;
  olArray loc_stack;
  olMap_of(CaExpr) symbols;
};

CaState cambda_init(); 
CaExpr* cambda_parse(CaState* ctx, olStr* str);
CaExpr* cambda_eval(CaState* ctx, CaExpr* expr);
void cambda_deinit(CaState* ctx);
