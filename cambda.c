#include "cambda.h"
#include "lib/ol.h"

#define cacur() *ctx->cur

CaExpr NULLEXPR = (CaExpr){.kind = EXPR_INVALID};
olStr NULLSTR = (olStr){.len=0};

CaState cambda_init() {
  ol_init();
  CaState result = {0};
  return result;
}

bool advance(CaState* ctx) {
  switch (*(ctx->cur)) {
  case 0:
    return true;
  case '\r': {
    ctx->cur_loc.col = 1;
    ctx->cur_loc.line += 1;
    ctx->cur++;
    if (*(ctx->cur) == 0) {
      return true;
    }
    ctx->cur++;
    return *(ctx->cur) == 0;
  }
  case '\n': {
    ctx->cur_loc.col = 1;
    ctx->cur_loc.line += 1;
    ctx->cur++;
    return *(ctx->cur) == 0;
  }
  }
  ctx->cur++;
  ctx->cur_loc.col += 1;
  return false;
}

static bool skip_whitespace(CaState *ctx) {
  while (cacur() == ' ' || cacur() == '\t' || cacur() == '\n' ||
         cacur() == '\r') {
    bool eof = advance(ctx);
    if (eof)
      return true;
  }
  return false;
}

static inline bool is_valid_ident(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
         c == '?' || c == '/' || c == '-' || c == '+' || c == '*' || c == '%';
}

static void loc_start(CaState *ctx) {
  CaSpan *s = (CaSpan*)olArray_push(&ctx->loc_stack);
  *s = ctx->cur_loc;
}

static CaSpan loc_end(CaState *ctx) {
  if (ctx->loc_stack.used == 0) {
    return ctx->cur_loc;
  }
  CaSpan old = olArray_pop(&ctx->loc_stack, CaSpan);
  if (old.line == ctx->cur_loc.line) {
    old.len = ctx->cur_loc.col - old.col;
  } else {
    old.len = UINT32_MAX;
  }
  return old;
}

static olStr* parse_ident(CaState *ctx, CaError *out_err) {
  loc_start(ctx);

  char *start = ctx->cur;
  while (is_valid_ident(*ctx->cur)) {
    advance(ctx);
  }
  u32 len = ptr_diff(ctx->cur, start);
  if (len == 0) {
    return &NULLSTR;
  }
  olStr *i = olStr_make(start, len);
  return i;
}

#define SKIP_WHITESPACE_OR_RETURN() \
    if (skip_whitespace(ctx)) { \
      *out_err = CaError_here(CAERR_UNEXPECTED_EOF, 1); \
      return &NULLEXPR; \
    }

CaExpr* cambda_parse_rec(CaState* ctx, CaError* out_err) {
  *out_err = CaError_ok();
  skip_whitespace(ctx);
  if (cacur() == '\\') {
    // abs
    SKIP_WHITESPACE_OR_RETURN();
    olStr* ident
  } else if (cacur() == '(') {
    // app
  } else if (is_valid_ident(cacur())) {
    // atom
  } else if (0 == strcmp(ctx->cur, "let")) {
    // let expr
    ctx->cur += 3;
    SKIP_WHITESPACE_OR_RETURN();
    olStr* ident = parse_ident(ctx, out_err);
    SKIP_WHITESPACE_OR_RETURN();
    if (cacur() != '=') {
      *out_err = CaError_make(CAERR_EXPECTED_EQ, ctx->cur_loc.col, ctx->cur_loc.line, 1);
      return &NULLEXPR;
    }
    CaExpr* e = cambda_parse_rec(ctx, out_err);
    if (out_err->kind != CAERR_OK) return &NULLEXPR;
    
    CaExpr* target = olMap_insert(&ctx->symbols, ident);
    memcpy_s(target, sizeof(CaExpr), e, sizeof(CaExpr));
    return target;
  }
  return &NULLEXPR;
}

CaExpr* cambda_parse(CaState* ctx, olStr* str) {
  ctx->cur = str->data;
  ctx->arena = olArena_make(16384);
  CaError err;
  return cambda_parse_rec(ctx, &err);  
}

CaExpr* cambda_eval(CaState* ctx, CaExpr* expr);
void cambda_deinit(CaState* ctx);
