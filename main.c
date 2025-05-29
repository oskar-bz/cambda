#include <stdio.h>
#include "cambda.h"

extern char* cbInstructionKindStrings[];

static void print_body(olArray_of(cbInstruction)* body) {
    for (u32 i = 0; i < body->used; i+=1) {
        cbInstruction* ins = olArray_get(body, i);
        printf("    %s\n", cbInstructionKindStrings[ins->kind]);
    }
    printf("\n");
}

static void print_bytecode(cbState* ctx) {
    printf("body:\n");
    print_body(ctx->cur_body);
}

int main(int argc, char** argv) {
    cbState ctx = cb_init();

    char buf[512];
    scanf_s("%s", buf, sizeof(buf));
    
    olStr* content = olStr_make(buf, 0);
    cbError err = cb_eval(&ctx, content);
    print_bytecode(&ctx);
    return 0;
}
