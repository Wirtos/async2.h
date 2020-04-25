#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include "async2.h"

struct f1_stack {
    int i;
    int res;
    struct astate **states;
};

async f3(struct astate *state, int *args, void *locals) {
    int *i = locals;
    int *res = args;
    async_begin(state);
            for (*i = 0; *i < 3; (*i)++) {
                printf("f3 %d\n", (*i) + 1);
                async_yield;
            }
            *res = rand() % INT_MAX;
    async_end;
}

async f2(struct astate *state, void *args, void *locals) {
    async_begin(state);
            puts("f2 call");
    async_end;
}

async f4(struct astate *state, void *args, void *locals) {
    async_begin(state);
            puts("f4 call 1");
            async_yield;
            puts("f4 call 2");
    async_end;
}


async f1(struct astate *state, char *args, struct f1_stack *locals) {
    int *i = &locals->i;
    int *res = &locals->res;
    async_begin(state);
            for (*i = 0; *i < 3; (*i)++) {
                printf("f0 %s %d\n", args, (*i) + 1);
                fawait(async_new(f3, res, int));
                printf("Result: %d\n", *res);
            }
            locals->states = malloc(sizeof(astate *) * 2);
            locals->states[0] = async_new(f4, NULL, ASYNC_NOLOCALS);
            locals->states[1] = async_new(f4, NULL, ASYNC_NOLOCALS);
            fawait(async_vgather(2, async_new(f4, NULL, ASYNC_NOLOCALS), async_new(f4, NULL, ASYNC_NOLOCALS)));
            fawait(async_gather(2, locals->states));
            free(locals->states);
    async_end;
}


int main() {
    async_loop_init();
    async_create_task(f1, "first", struct f1_stack);
    async_create_task(f2, NULL, ASYNC_NOLOCALS);
    async_create_task(f1, "second", struct f1_stack);
    async_loop_run_forever();

    async_loop_destroy();
    return 0;
}
