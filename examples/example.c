#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "async2.h"

struct f3_args {
    int res1;
    int res2;
};

struct f1_stack {
    int i;
    struct f3_args res;
    void *mem;
};

async f4(struct astate *state) {
    async_begin(state);
            puts("f4 call 1");
            async_yield;
            puts("f4 call 2");
    async_end;
}


async f3(struct astate *state) {
    int *i = state->locals;
    struct f3_args *res = state->args; /* Pointer assignment from locals or args is fine outside async_begin, but value assignment isn't. */
    async_begin(state);
            for (*i = 0; *i < 3; (*i)++) {
                printf("f3 %d\n", (*i) + 1);
                async_yield;
            }
            res->res1 = rand() % RAND_MAX;
            res->res2 = rand() % RAND_MAX;
    async_end;
}

async f2(struct astate *state) {
    async_begin(state);
            puts("f2 call");
    async_end;
}

async f1(struct astate *state) {
    struct f1_stack *locals = state->locals;
    char *text = state->args;

    async_begin(state);
            for (locals->i = 0; locals->i < 3; locals->i++) {
                printf("f0 %s %d\n", text, (locals->i) + 1);
                fawait(async_new(f3, &locals->res, int)); /* Create new coro from f3 and wait until it completes. */
                printf("Result: %d - %d\n", locals->res.res1, locals->res.res2);
            }
            fawait(async_vgather(2, async_new(f4, NULL, ASYNC_NOLOCALS), async_new(f4, NULL, ASYNC_NOLOCALS)));
            assert(async_errno == ASYNC_OK);
            fawait(async_sleep(1));
            assert(async_errno == ASYNC_OK);
            fawait(async_wait_for(async_sleep(1000000), 1));
            assert(async_errno == ASYNC_ERR_CANCELLED);
            locals->mem = async_alloc(512); /* This memory will be freed automatically after function end*/
    async_end;
}


int main() {
    async_loop_init(); /* Init event loop and create some tasks to run them later. */
    async_create_task(async_new(f1, "first", struct f1_stack));
    async_create_task(async_new(f2, NULL, ASYNC_NOLOCALS));
    async_create_task(async_new(f1, "second", struct f1_stack));
    async_loop_run_forever(); /* Block execution and run all tasks. */
    async_loop_destroy();
    return 0;
}
