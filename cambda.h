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

struct cbValue {
    union {
        i64 int_;
        double float_;
        olStr* str_;
        cbFn* fn_;
    };
    enum {
        VALUE_TRUE,
        VALUE_FALSE,
        VALUE_NIL,
        VALUE_INT,
        VALUE_FLOAT,
        VALUE_STR,
        VALUE_FN,
        VALUE_CFUNC,
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

typedef bool cbNativeFunction(cbState* ctx);

struct cbInstruction {
    cbInstructionKind kind;
    union {
        double float_;
        u64 u64_;
        i64 i64_;
        cbFn* cbFn_;
    };
};

#define cbSpan_make(line_, col_, len_) (cbSpan){.line=line_,.col=col_,.len=len_}

struct cbSpan {
    u32 line;
    u32 col;
    u32 len;
};

struct cbFn {
    union {
        cbNativeFunction* cfunc_;
        olArray_of(cbInstruction) body;
    };
    olStr* name;
    cbSpan loc;
    olArray_of(cbType) args;
    olArray_of(cbType) return_vals;
};

struct cbScope {
    olMap_of(cbValue) env;
    cbScope* prev;
};

struct cbState {
    olArray stack;
    olArray main;
    olArray* cur_body;
    cbScope* scope;
    olStr* content;
    char* cur;
    cbSpan cur_loc;
    olArray loc_stack;
};

struct cbError {
    cbSpan loc;
    olStr* err_str;
    enum cbErrorKind {
        ERROR_OK,
        ERROR_MISSING_RPAREN,
        ERROR_EMPTY_KEYWORD,
        ERROR_OUT_OF_MEM,
        ERROR_UNEXPECTED_CHAR,
        ERROR_EXPECTED_SIGN_AFTER_E,
        ERROR_EXPONENT_AFTER_COMMA,
        ERROR_NOT_CALLABLE,
    } kind ;
};

#define cbError_make(error_, line_,col_,len_) (cbError) {.loc.line=line_,.loc.col=col_,.loc.len=len_,.kind=error_}
#define cbError_ok() cbError_make(ERROR_OK, 0, 0, 0) 
#define cbError_OutOfMem() cbError_make(ERROR_OUT_OF_MEM, 0, 0, 0) 

cbState cb_init();
cbError cb_eval(cbState* ctx, olStr* content);
void cb_deinit(cbState* ctx);
