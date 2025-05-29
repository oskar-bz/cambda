#include "cambda.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>

const u64 TRUE_HASH = 0x5b5c98ef514dbfa5;
const u64 FALSE_HASH = 0xb5fae2c14238b978;
const u64 NIL_HASH = 0x2146ba19257dc6ac;

#define cur() *(ctx->cur)

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
            return error(ERROR_EXPONENT_AFTER_COMMA, ctx->cur_loc, "Unexpected 'e' after comma.");
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
            return error(ERROR_EXPECTED_SIGN_AFTER_E, ctx->cur_loc, "Expected a sing (+ or -) after e in number");
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

static cbError gen_expr(cbState* ctx) {
    skip_whitespace(ctx);
    if (*ctx->cur == '(') {
        advance(ctx);
        skip_whitespace(ctx);
        olStr* ident = parse_ident(ctx);
        // gen args
        while (*ctx->cur != ')') {
            gen_expr(ctx);
        }
        advance(ctx);
        cbValue* fn = (cbValue*)olMap_get(&ctx->scope->env, ident);
        if (fn == null) {
            
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
}

static cbError cb_parse(cbState* ctx) {
    while (true) {
        gen_expr(ctx);
    }
    return cbError_ok();
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
