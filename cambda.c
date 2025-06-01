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

const cbType NULLTYPE = (cbType) {.id = 0, .name = 0};
cbExpression NULLEXPR = (cbExpression) {.kind = E_INVALID };

#define cur() *(ctx->cur)
#define check_eof(maybe_eof) if(maybe_eof) return cbError_EOF();
#define check_eof2(maybe_eof) if(maybe_eof) { *out_err = cbError_EOF(); return null; }
#define check_eof3(maybe_eof, returnval) if(maybe_eof) { *out_err = cbError_EOF(); return returnval; }
#define arena() (&ctx->main_arena)

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

const char* cbErrorKind_strings[ERROR_LEN] = {
    "ERROR_OK",
    "ERROR_MISSING_RPAREN",
    "ERROR_EMPTY_KEYWORD",
    "ERROR_OUT_OF_MEM",
    "ERROR_UNEXPECTED_CHAR",
    "ERROR_EXPECTED_SIGN_AFTER_E",
    "ERROR_EXPONENT_AFTER_COMMA",
    "ERROR_NOT_CALLABLE",
    "ERROR_UNEXPECTED_EOF",
    "ERROR_VAR_NOT_FOUND",
};

// Warning: ownership of str will be transferred to the map
u64 cb_register_string(cbState* ctx, olStr* str) {
    u64 hash = fnv1a(str);
    olStr** place = olMap_inserth(&ctx->string_map, hash);
    *place = str;
    return hash;
}

olStr* cb_get_string(cbState* ctx, u64 hash) {
    olStr** result = olMap_geth(&ctx->string_map, hash);
    if (result) {
        return *result;
    }
    return null;
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

void cb_print_error(cbState* ctx, cbError err, bool verbose, olStr* filename) {
    olLog_set_color(COLOR_GREY);
    olLog_print_time();

    olLog_set_color(COLOR_RED);
    olLog_set_bold();
    printf(" ERROR ");
    olLog_reset_bold();

    olLog_set_color(COLOR_GREY);
    printf("%s:%d:%d ", filename->data, err.loc.line, err.loc.col);
    olLog_reset();

    if (err.err_str->len > 0) {
        printf("%s\n", err.err_str->data);
    } else {
        printf("%s\n", cbErrorKind_strings[err.kind]);
    }
    // TODO: pretty print, show code lines, allow hints
}

void cb_print_indent(u32 indent) {
    for (int i = 0; i < indent*2; i += 1) {
        printf(" ");        
    }
}

void cb_print_node(cbState* ctx, cbExpression* cur, u32 indent) {
    cb_print_indent(indent);
    switch (cur->kind) {
    case E_INVALID: {
        printf("<invalid> ");
      } break;
    case EVAR:
        printf("%s", cb_get_string(ctx, cur->var.hash)->data);
        break;
    case EKEYW:
        printf(":%s", cb_get_string(ctx, cur->lit.hash_)->data);
        break;
    case EAPP:
    case EAPP_UNRESOLVED:
    case EABS:
        printf("TODO");
        break;
    case ELIT: {
        switch (cur->lit.kind) {
        case VALUE_HASH:
          olStr* s = cb_get_string(ctx, cur->lit.hash_);
          if (s) {
              printf(":%s", s->data);
          } else {
            printf("#%llx", cur->lit.hash_);
          }
        case VALUE_TRUE:
          printf("true");
          break;
        case VALUE_FALSE:
          printf("false");
          break;
        case VALUE_NIL:
          printf("nil");
          break;
        case VALUE_INT:
          printf("%lld", cur->lit.int_);
          break;
        case VALUE_FLOAT:
          printf("%f", cur->lit.float_);
          break;
        case VALUE_STR:
          printf("'%s'", cur->lit.str_->data);
          break;
        case VALUE_FN:
          printf("<%s '%s'>", cur->lit.fn_->is_native ? "function " : "cfunc", cur->lit.fn_->name->data);          
          break;
        }
      } break;
    }
}

void cb_print_ast(cbState* ctx) {
    cb_print_node(ctx, ctx->program, 0);
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

static cbValue* scope_geth(cbState* ctx, u64 key) {
    cbScope* cur_scope = ctx->scope;
    do {
        cbValue* result = (cbValue*)olMap_geth(&cur_scope->env, key);
        if (result != null) {
            return result;
        }
        cur_scope = cur_scope->prev;
    } while (cur_scope);
    return null;
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

static cbExpression* parse_ident(cbState* ctx, cbError* out_err) {
    loc_start(ctx);
    
    char* start = ctx->cur;
    while (is_valid_ident(*ctx->cur)) {
        advance(ctx);
    }
    u32 len = ptr_diff(ctx->cur, start);
    olStr* i = olStr_make(start, len);
    cb_register_string(ctx, i);
    
    cbExpression* result = olArena_alloc(&ctx->main_arena, sizeof(cbExpression));
    result->loc = loc_end(ctx);
    result->kind = EVAR;
    result->var.hash = fnv1a(i);
    *out_err = cbError_ok();
    return result;
}

static cbExpression* parse_number(cbState* ctx, cbError* out_err) {
    i64 int_val = 0;
    double float_val = 0;
    bool is_neg = false;
    bool is_float = false;
    if (cur() == '-') {
        is_neg = true;
        check_eof3(advance(ctx), &NULLEXPR);
    }

    while (cur() >= '0' && cur() <= '9') {
        int_val *= 10;
        int_val += cur() - '0';
        advance(ctx);
    }

    if (cur() == '.') {
        check_eof3(advance(ctx), &NULLEXPR);
        float_val = (double)int_val;
        is_float = true;
        if (cur() == 'e') {
            *out_err = errorc(ERROR_EXPONENT_AFTER_COMMA, ctx->cur_loc, "Unexpected 'e' after comma.");
            return &NULLEXPR;
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
        check_eof3(advance(ctx), &NULLEXPR);
        bool div = false;
        if (cur() == '+') advance(ctx);
        else if (cur() == '-') {
            div = true;
            check_eof3(advance(ctx), &NULLEXPR);
        } else {
            *out_err = errorc(ERROR_EXPECTED_SIGN_AFTER_E, ctx->cur_loc, "Expected a sing (+ or -) after e in number");
            return &NULLEXPR;
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

    *out_err = cbError_ok();
    if (is_float) {
        if (is_neg) float_val *= -1;
        cbExpression* result = olArena_alloc(arena(), sizeof(cbExpression));
        result->kind = ELIT;
        result->lit.kind = VALUE_FLOAT;
        result->lit.float_ = float_val;
        return result;
    } else {
        if (is_neg) int_val *= -1;
        cbExpression* result = olArena_alloc(arena(), sizeof(cbExpression));
        result->kind = ELIT;
        result->lit.kind = VALUE_INT;
        result->lit.int_ = int_val;
        return result;
    }
}

static cbExpression* cb_parse(cbState* ctx, cbError* out_err) {
    *out_err = cbError_ok();
    while (true) {
        skip_whitespace(ctx);
        if (cur() == '(') {
            check_eof3(advance(ctx), &NULLEXPR);
            
            cbExpression* e = olArena_alloc(arena(), sizeof(cbExpression));
            // defer resolution until typechecking
            e->kind = EAPP_UNRESOLVED;
            
            // parse fn to call
            loc_start(ctx);
            e->app.fnexpr = cb_parse(ctx, out_err);
            if (out_err->kind != ERROR_OK) {
                return &NULLEXPR;
            }
            
            // parse args
            e->app.supplied_args = olArray_make(sizeof(cbExpression), 5);
            while (cur() != ')') {
                cbExpression* arg = cb_parse(ctx, out_err);
                if (out_err->kind != ERROR_OK) {
                    return &NULLEXPR;
                }
                cbExpression* dest = olArray_push(&e->app.supplied_args);
                memcpy_s(dest, sizeof(cbExpression), arg, sizeof(cbExpression));
                olArena_free_last(arena()); // free parsed argument
            }
            e->loc = loc_end(ctx);
            return e;
        } else if (is_valid_ident(cur())) {
            cbExpression* e = parse_ident(ctx, out_err);
            if (e->var.hash == TRUE_HASH) {
                e->kind = ELIT;
                e->lit.kind = VALUE_TRUE;
            } else if (e->var.hash == FALSE_HASH) {
                e->kind = ELIT;
                e->lit.kind = VALUE_FALSE;
            } else if (e->var.hash == NIL_HASH) {
                e->kind = ELIT;
                e->lit.kind = VALUE_NIL;
            }
            return e;
        } else if (cur() == ':') {
            if (advance(ctx)) {
                *out_err = cbError_EOF();
                return &NULLEXPR;
            }
            cbExpression* e = parse_ident(ctx, out_err);
            e->kind = ELIT;
            e->lit.hash_ = e->var.hash;
            return e;
        } else if ((cur() >= '0' && cur() <= '9') || cur() == '-') {
            return parse_number(ctx, out_err);
        } else {
            *out_err = errorf(ERROR_UNEXPECTED_CHAR, ctx->cur_loc, "Unexpected char '%c'", cur());
            return &NULLEXPR;
        }
    }
}

cbState cb_init() {
    cbState result;
    result.main_arena = olArena_make(1024*100); // 100 KB
    result.string_map = olMap_makeof(olStr*);
    result.stack = olArray_makeof(cbValue, 128); 
    result.main = olArray_makeof(cbInstruction, 256);
    result.cur_body = &result.main;
    result.content = null;
    result.cur = null;
    result.cur_loc = cbSpan_make(0,0,1);
    result.loc_stack = olArray_makeof(cbSpan, 5);
    olLog_init();
    return result;
}

cbError cb_eval(cbState* ctx, olStr* content) {
    ctx->cur_body = &ctx->main;
    ctx->content = content;
    ctx->cur = content->data;
    ctx->cur_loc = cbSpan_make(1,1,1);
    scope_push(ctx);
    cbError err;
    cbExpression* program = cb_parse(ctx, &err);
    if (err.kind != ERROR_OK) {
        cb_reset(ctx);        
        return err;
    }
    scope_pop(ctx);
    return err;
}

void cb_reset(cbState* ctx) {
    printf("Resetting\n");
}

void cb_deinit(cbState* ctx);
