#include "cambda.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>

const u64 TRUE_HASH = 0x5b5c98ef514dbfa5;
const u64 FALSE_HASH = 0xb5fae2c14238b978;
const u64 NIL_HASH = 0x2146ba19257dc6ac;

const u64 DEFINE_HASH = 0x4c56130c429d6d92;
const u64 LET_HASH = 0x127284191dcc577a;
const u64 MACRO_HASH = 0x12c837a52b5d72b3;
const u64 LAMBDA_HASH = 0x826b4caaf325324a;
const u64 IF_HASH = 0x08b73007b55c3e26;
const u64 CAR_HASH = 0xf5e305190ce49fc1;
const u64 CDR_HASH = 0xf5ecf3190cecd5b0;
const u64 CONS_HASH = 0x0bca0591195d8188;
const u64 LIST_HASH = 0xbf779aad69748141;
const u64 MATCH_HASH = 0xc3bfe3a4fe4c13f6;

#define cur() *(ctx->cur)

char* cbInstructionKindStrings[] = {
    "INS_LIST",
    "INS_CONS",
    "INS_CAR",
    "INS_CDR",
    "INS_SETCAR",
    "INS_SETCDR",
    "INS_CALL",
    "INS_CALL_CFUNC",
    "INS_PUSHTRUE",
    "INS_PUSHFALSE",
    "INS_PUSHNIL",
    "INS_PUSHHASH",
    "INS_PUSHINT",
    "INS_PUSHFLOAT",
    "INS_POPTRUE",
    "INS_POPFALSE",
    "INS_POPNIL",
    "INS_POPHASH",
    "INS_POPINT",
    "INS_POPFLOAT",
    "INS_DUP",
    "INS_SCOPE_PUSH",
    "INS_DEF",
    "INS_GET",
    "INS_SCOPE_POP",
    "INS_ADDI",
    "INS_ADDF",
    "INS_ADDS",
    "INS_SUBI",
    "INS_SUBF",
    "INS_MULI",
    "INS_MULF",
    "INS_DIVI",
    "INS_DIVF",
    "INS_MODI",
    "INS_MODF",
    "INS_NOT",
    "INS_AND",
    "INS_OR",
    "INS_XOR",
    "INS_JUMP",
    "INS_CJUMP",
};

static bool advance(cbState* ctx) {
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

static bool skip_whitespace(cbState* ctx) {
    while (*(ctx->cur) == ' ' || *(ctx->cur) == '\t') {
        bool eof = advance(ctx);
        if (eof) return true;
    }
    return false;
}

static inline bool is_valid_ident(char c) {
    return (c >= 'a' && c <= 'z')
        || (c >= 'A' && c <= 'Z')
        ||  c == '_'
        ||  c == '?'
        ||  c == '/'
        ||  c == '-';
}

static olStr* parse_ident(cbState* ctx) {
    char* start = ctx->cur;
    while (is_valid_ident(*ctx->cur)) {
        advance(ctx);
    }
    u32 len = ptr_diff(ctx->cur, start);
    return olStr_make(start, len);
}

static void loc_start(cbState* ctx) {
    cbSpan* s = (cbSpan*)olArray_push(&ctx->loc_stack);
    *s = ctx->cur_loc;
}

static cbSpan loc_end(cbState* ctx) {
    if (ctx->loc_stack.used == 0) {
        return ctx->cur_loc;
    }
    cbSpan old = olArray_pop(&ctx->loc_stack, cbSpan);
    if (old.line == ctx->cur_loc.line) {
        old.len = ctx->cur_loc.col - old.col;
    } else {
        old.len = UINT32_MAX;
    }
    return old;
}

static cbError emit(cbState* ctx, cbInstructionKind kind) {
    cbInstruction* ins = olArray_push(ctx->cur_body);
    if (ins == null) {
        return cbError_OutOfMem();
    }
    ins->kind = kind;
    return cbError_ok();
}

#define emit_val_fn(suffix, type) \
static cbError emit##suffix(cbState* ctx, cbInstructionKind kind, type val) { \
    cbInstruction* ins = olArray_push(ctx->cur_body);                         \
    if (ins == null) {                                                        \
        return cbError_OutOfMem();                                            \
    }                                                                         \
    ins->kind = kind;                                                         \
    ins->type##_ = val;                                                     \
    return cbError_ok();                                                      \
}

emit_val_fn(f, float);
emit_val_fn(h, u64);
emit_val_fn(i, i64);

static cbError emitfn(cbState *ctx, cbInstructionKind kind, cbFn* val) {
    cbInstruction *ins = olArray_push(ctx->cur_body);
    if (ins == ((void *)0)) {
    return (cbError){
        .loc.line = 0, .loc.col = 0, .loc.len = 0, .kind = ERROR_OUT_OF_MEM};
    }
    ins->kind = kind;
    ins->cbFn_ = val;
    return (cbError){.loc.line = 0, .loc.col = 0, .loc.len = 0, .kind = ERROR_OK};
};

static cbError errorc(enum cbErrorKind kind, cbSpan loc, const char* msg) {
    cbError result;
    result.kind = kind;
    result.loc = loc;
    result.err_str = olStr_make(msg, 0);
    return result;
}

static cbError error(enum cbErrorKind kind, cbSpan loc, olStr* msg) {
    cbError result;
    result.kind = kind;
    result.loc = loc;
    result.err_str = msg;
    return result;
}

static cbError errorf(enum cbErrorKind kind, cbSpan loc, char* format, ...) {
    va_list args;
    va_start(args, format);
    char buffer[512];
    u32 len = vsprintf_s(buffer, 512, format, args);
    olStr* msg = olStr_make(buffer, len);
    return error(kind, loc, msg);
    va_end(args);
}

static cbError gen_number(cbState* ctx) {
    i64 int_val = 0;
    double float_val = 0;
    bool is_neg = false;
    bool is_float = false;
    if (cur() == '-') {
        is_neg = true;
        advance(ctx);
    }

    while (cur() >= '0' && cur() <= '9') {
        int_val *= 10;
        int_val += cur() - '0';
        advance(ctx);
    }

    if (cur() == '.') {
        advance(ctx);
        float_val = (double)int_val;
        is_float = true;
        if (cur() == 'e') {
            return errorc(ERROR_EXPONENT_AFTER_COMMA, ctx->cur_loc, "Unexpected 'e' after comma.");
        }
        u64 decimals = 0; u32 count = 0;
        while (cur() >= '0' && cur() <= '9') {
            decimals *= 10;
            decimals += cur() - '0';
            count++;
            advance(ctx);
        }
        double frac = (double)decimals / pow(10, count);
        float_val += frac;
    }

    if (cur() == 'e') {
        advance(ctx);
        bool div = false;
        if (cur() == '+') advance(ctx);
        else if (cur() == '-') {
            div = true;
            advance(ctx);
        } else {
            return errorc(ERROR_EXPECTED_SIGN_AFTER_E, ctx->cur_loc, "Expected a sing (+ or -) after e in number");
        }
        u64 exponent = 0;
        while (cur() >= '0' && cur() <= '9') {
            exponent *= 10;
            exponent += cur() - '0';
            advance(ctx);
        }
        if (is_float) {
            if (div) {
                float_val /= pow(10, exponent);
            } else {
                float_val *= pow(10, exponent);
            }
        } else {
            if (div) {
                int_val /= pow(10, exponent);
            } else {
                int_val *= pow(10, exponent);
            }
        }
    }

    if (is_float) {
        if (is_neg) float_val *= -1;
        return emitf(ctx, INS_PUSHFLOAT, float_val);
    } else {
        if (is_neg) int_val *= -1;
        return emiti(ctx, INS_PUSHINT, int_val);
    }
}

void scope_push(cbState* ctx) {
    cbScope* new_scope = malloc(sizeof(cbScope));
    new_scope->prev = ctx->scope;
    new_scope->env = olMap_makeof(cbValue);
    ctx->scope = new_scope;
}

void scope_pop(cbState* ctx) {
    cbScope* sc = ctx->scope;
    ctx->scope = sc->prev;
    free(sc);
}

static cbValue* scope_get(cbState* ctx, olStr* key) {
    cbScope* cur_scope = ctx->scope;
    do {
        cbValue* result = (cbValue*)olMap_get(&cur_scope->env, key);
        if (result != null) {
            return result;
        }
        cur_scope = cur_scope->prev;
    } while (cur_scope);
    return null;
}

static cbValue* scope_set(cbState* ctx, olStr* key) {
    return olMap_insert(&ctx->scope->env, key);
}

static cbError gen_expr(cbState* ctx) {
    skip_whitespace(ctx);
    if (*ctx->cur == '(') {
        advance(ctx);
        skip_whitespace(ctx);
        loc_start(ctx);
        olStr* ident = parse_ident(ctx);
        cbSpan loc = loc_end(ctx);
        u64 hash = fnv1a(ident);
        // gen args
        while (cur() != ')') {
            gen_expr(ctx);
        }
        advance(ctx);
        cbValue* fn = scope_get(ctx, ident);        
        // check if it is a builtin function
        if (hash == DEFINE_HASH)        {
            
        } else if (hash == LET_HASH)    {
            
        } else if (hash == MACRO_HASH)  {
            
        } else if (hash == LAMBDA_HASH) {
            
        } else if (hash == IF_HASH)     {
            
        } else if (hash == CAR_HASH)    {
            emit(ctx, INS_CAR);
        } else if (hash == CDR_HASH)    {
            emit(ctx, INS_CDR);
        } else if (hash == CONS_HASH)   {
            emit(ctx, INS_CONS);
        } else if (hash == LIST_HASH)   {
            emit(ctx, INS_LIST);
        } else if (hash == MATCH_HASH)  {
        } 
        else if (fn->kind == VALUE_CFUNC) {
            emitfn(ctx, INS_CALL_CFUNC, fn->fn_);
        } else if (fn->kind == VALUE_FN) {
            emitfn(ctx, INS_CALL, fn->fn_);
        } else {
            // TODO: call an expression like this
            // ((get-fn foo) arg1 arg2)
            return errorf(ERROR_NOT_CALLABLE, loc, "'%s' is not callable");
        }
    } else if (is_valid_ident(*ctx->cur)) {
        loc_start(ctx);
        olStr* maybe_true = parse_ident(ctx);
        cbSpan loc = loc_end(ctx);
        
        u64 hash = fnv1a(maybe_true);
        if (hash == TRUE_HASH) {
            return emit(ctx, INS_PUSHTRUE);
        } else if (hash == FALSE_HASH) {
            return emit(ctx, INS_PUSHFALSE);
        } else if (hash == NIL_HASH) {
            return emit(ctx, INS_PUSHNIL);
        } else {
            return emith(ctx, INS_PUSHHASH, hash);
        }
    } else if (*ctx->cur == ':') {
        loc_start(ctx);
        olStr* keyw = parse_ident(ctx);
        cbSpan loc = loc_end(ctx);
        if (keyw->len == 0) {
            return errorc(ERROR_EMPTY_KEYWORD, loc, "An identifier must follow after ':', e.g. ':keyword'");
        }
        return emith(ctx, INS_PUSHHASH, fnv1a(keyw));
    } else if ((*ctx->cur >= '0' && *ctx->cur <= '9') || *ctx->cur == '-') {
        return gen_number(ctx);
    } else {
        return errorf(ERROR_UNEXPECTED_CHAR, ctx->cur_loc, "Unexpected char '%c'", *ctx->cur);
    }
    return cbError_ok();
}

static cbError cb_parse(cbState* ctx) {
    while (true) {
        gen_expr(ctx);
    }
    return cbError_ok();
}

cbState cb_init() {
    cbState result;
    result.stack = olArray_makeof(cbValue, 128); 
    result.main = olArray_makeof(cbInstruction, 256);
    result.cur_body = &result.main;
    result.content = null;
    result.cur = null;
    result.cur_loc = cbSpan_make(0,0,1);
    result.loc_stack = olArray_makeof(cbSpan, 5);
    scope_push(&result);
    return result;
}

cbError cb_eval(cbState* ctx, olStr* content) {
     ctx->cur_body = &ctx->main;
     ctx->content = content;
     ctx->cur = content->data;
     ctx->cur_loc = cbSpan_make(1,1,1);
     return cb_parse(ctx);
}

void cb_deinit(cbState* ctx);
