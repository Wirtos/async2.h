#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include "async2.h"

struct f1_stack {
    int i;
    int res;
};

async f4(struct astate *state, void *args, void *locals) {
    async_begin(state);
            puts("f4 call 1");
            async_yield;
            puts("f4 call 2");
    async_end;
}


async f3(struct astate *state, int *args, int *locals) {
    int *i = locals;
    int *res = args; /* Pointer assignment from locals or args is fine outside async_begin, but value assignment isn't. */
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


async f1(struct astate *state, char *args, struct f1_stack *locals) {
    async_begin(state);
            for (locals->i = 0; locals->i < 3; locals->i++) {
                printf("f0 %s %d\n", args, (locals->i) + 1);
                fawait(async_new(f3, &locals->res, int)); /* Create new coro from f3 and wait until it completes. */
                printf("Result: %d\n", locals->res);
            }
            fawait(async_vgather(2, async_new(f4, NULL, ASYNC_NOLOCALS), async_new(f4, NULL, ASYNC_NOLOCALS)));
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
