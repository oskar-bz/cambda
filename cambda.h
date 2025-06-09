#pragma once

#include "..\\lib\\ol.h"

typedef struct cbState cbState;
typedef struct cbValue cbValue;
typedef enum cbInstructionKind cbInstructionKind;
typedef struct cbInstruction cbInstruction;
typedef struct cbFn cbFn;
typedef struct cbSpan cbSpan;
typedef struct cbError cbError;
typedef struct cbScope cbScope;
typedef struct cbExpression cbExpression;
typedef bool cbNativeFunction(cbState *ctx);
typedef struct cbType cbType;
typedef struct cbArg cbArg;

struct cbValue {
  union {
    i64 int_;
    double float_;
    olStr *str_;
    cbFn *fn_;
    u64 hash_;
  };
  enum {
    VALUE_HASH,
    VALUE_TRUE,
    VALUE_FALSE,
    VALUE_NIL,
    VALUE_INT,
    VALUE_FLOAT,
    VALUE_STR,
    VALUE_FN,
  } kind;
};

enum cbInstructionKind {
  INS_LIST,
  INS_CONS,
  INS_CAR,
  INS_CDR,
  INS_SETCAR,
  INS_SETCDR,
  INS_CALL,
  INS_CALL_CFUNC,
  INS_PUSHTRUE,
  INS_PUSHFALSE,
  INS_PUSHNIL,
  INS_PUSHHASH,
  INS_PUSHINT,
  INS_PUSHFLOAT,
  INS_POPTRUE,
  INS_POPFALSE,
  INS_POPNIL,
  INS_POPHASH,
  INS_POPINT,
  INS_POPFLOAT,
  INS_DUP,
  INS_SCOPE_PUSH,
  INS_DEF,
  INS_GET,
  INS_SCOPE_POP,
  INS_ADDI,
  INS_ADDF,
  INS_ADDS,
  INS_SUBI,
  INS_SUBF,
  INS_MULI,
  INS_MULF,
  INS_DIVI,
  INS_DIVF,
  INS_MODI,
  INS_MODF,
  INS_NOT,
  INS_AND,
  INS_OR,
  INS_XOR,
  INS_JUMP,
  INS_CJUMP,
};

#define cbSpan_make(line_, col_, len_)                                         \
  (cbSpan) { .line = line_, .col = col_, .len = len_ }

struct cbSpan {
  u32 line;
  u32 col;
  u32 len;
};

enum cbTypeKind {
  TK_TEMP,
  TK_CONCRETE,
  TK_FN,
};

struct cbType {
  struct {
    u64 is_fn : 1;
    u64 is_temp : 1;
    u64 id : 62; // name for concrete type, else id for typevar
  };
  
  struct {
    cbType *from;
    cbType *to;
  } fn;
};

struct cbArg {
  cbType type;
  u64 name;
};

struct cbExpression {
  enum cbExpressionKind {
    E_INVALID,
    EVAR,
    EKEYW,
    EAPP,
    EAPP_UNRESOLVED,
    ELIT,
    ELET,
    EDO,
    EIF,
  } kind;
  union {
    struct {
      u64 hash;
    } var;

    struct {
      union {
        cbFn *fn;
        cbExpression *fnexpr;
      };
      olArray_of(cbType) supplied_args;
    } app;

    struct {
      cbExpression *expr;
      u64 hash;
    } let;

    struct {
      cbExpression *cond;
      cbExpression *if_true;
      cbExpression *if_false;
    } iff;

    struct {
      olArray_of(cbExpression) exprs;
    } do_;

    cbValue lit;
  };
  cbType type;
  cbSpan loc;
};

struct cbInstruction {
  cbInstructionKind kind;
  union {
    double float_;
    u64 u64_;
    i64 i64_;
    cbFn *cbFn_;
  };
};

struct cbFn {
  bool is_native;
  union {
    cbNativeFunction *cfunc_;
    olArray_of(cbInstruction) ibody;
    cbExpression *ebody;
  };
  u64 name_hash;
  cbSpan loc;
  olArray_of(cbArg) args;
  cbType return_type;
};

struct cbScope {
  olMap_of(cbValue) env;
  cbScope *prev;
};

struct cbState {
  olArena main_arena;
  olMap string_map;
  olArray stack;
  olArray main;
  cbExpression *program;
  olArray *cur_body;
  cbScope *scope;
  olStr *content;
  char *cur;
  cbSpan cur_loc;
  olArray loc_stack;
};

struct cbError {
  cbSpan loc;
  olStr *err_str;
  enum cbErrorKind {
    ERROR_OK,
    ERROR_MISSING_RPAREN,
    ERROR_EMPTY_KEYWORD,
    ERROR_OUT_OF_MEM,
    ERROR_UNEXPECTED_CHAR,
    ERROR_EXPECTED_SIGN_AFTER_E,
    ERROR_EXPONENT_AFTER_COMMA,
    ERROR_NOT_CALLABLE,
    ERROR_UNEXPECTED_EOF,
    ERROR_VAR_NOT_FOUND,
    ERROR_WRONG_ARGS,
    ERROR_EXPECTED_IDENT,
    ERROR_INCOMPATIBLE_TYPES,
    ERROR_LEN,
  } kind;
};

#define cbError_make(error_, line_, col_, len_)                                \
  (cbError) {                                                                  \
    .loc.line = line_, .loc.col = col_, .loc.len = len_, .kind = error_        \
  }
#define cbError_ok() cbError_make(ERROR_OK, 0, 0, 0)
#define cbError_OutOfMem() cbError_make(ERROR_OUT_OF_MEM, 0, 0, 0)
#define cbError_EOF()                                                          \
  (cbError) { .loc = ctx->cur_loc, .kind = ERROR_UNEXPECTED_EOF }

cbState cb_init();
cbError cb_eval(cbState *ctx, olStr *content);
void cb_reset(cbState *ctx);
void cb_deinit(cbState *ctx);
void cb_print_error(cbState *ctx, cbError err, bool verbose, olStr *filename);
void cb_print_ast(cbState *ctx);
