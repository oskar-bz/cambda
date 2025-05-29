#include "cambda.h"


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

static cbError cb_parse(cbState* ctx) {
    while (true) {
        skip_whitespace(ctx);
        switch (*(ctx->cur)) {
            case '(': {
                advance(ctx);
                skip_whitespace(ctx);
                olStr* ident = parse_ident(ctx);
                // TODO
                // parse & compile arguments
                // switch on function name (for hardcoded functions)
            } break;
        }
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
    result.cur_loc = cbSpan_make(0,0,0);
    scope_push(&result);
    return result;
}

cbError cb_eval(cbState* ctx, olStr* content) {
     ctx->cur_body = &ctx->main;
     ctx->content = content;
     ctx->cur = content->data;
     ctx->cur_loc = cbSpan_make(1,1,0);
     return cb_parse(ctx);
}

void cb_deinit(cbState* ctx);
