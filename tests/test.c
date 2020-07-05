#include "async2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define test_section(desc)        \
    {                             \
        printf("--- %s\n", desc); \
    }                             \
    (void) 0

#define test_assert(cond)                                                     \
    {                                                                         \
        int pass__ = cond;                                                    \
        printf("[%s] %s:%d: ", pass__ ? "PASS" : "FAIL", __FILE__, __LINE__); \
        printf((strlen(#cond) > 50 ? "%.100s...\n" : "%s\n"), #cond);         \
        if (pass__) {                                                         \
            pass_count++;                                                     \
        } else {                                                              \
            fail_count++;                                                     \
        }                                                                     \
    }                                                                         \
    (void) 0

#define test_print_res()                                                          \
    {                                                                             \
        printf("------------------------------------------------------------\n"); \
        printf("-- Results:   %3d Total    %3d Passed    %3d Failed       --\n",  \
               pass_count + fail_count, pass_count, fail_count);                  \
        printf("------------------------------------------------------------\n"); \
    }                                                                             \
    (void) 0

int pass_count = 0;
int fail_count = 0;

static async cancellable(s_astate state) {
    int *res = state->args;
    async_begin(state);
    if (res != NULL) {
        *res = 0;
    }
    async_end;
}

static async errno_produce(s_astate state) {
    int *err = state->args;
    async_begin(state);
    struct astate *child_state = async_new(cancellable, NULL, ASYNC_NONE);
    if (child_state)
        async_cancel(child_state);
    fawait(child_state){
    }
    *err = async_errno;
    async_end;
}

static void cancellable_c(s_astate state) {
    int *res = state->args;
    *res = 42;
}

static async add(s_astate state) {
    int *res = state->args;
    async_begin(state);
    async_yield;
    *res += 1;
    async_end;
}

typedef struct {
    struct astate *states[3];
} gatherable_stack;

static async gatherable(s_astate state) {
    gatherable_stack *stack = state->locals;
    int *res = state->args;
    async_begin(state);
    stack->states[0] = async_new(add, res, ASYNC_NONE);
    stack->states[1] = async_new(add, res, ASYNC_NONE);
    stack->states[2] = async_new(add, res, ASYNC_NONE);
    fawait(async_gather(3, stack->states)) {
        if (async_errno == ASYNC_ENOMEM) {
            async_free_coros_(3, stack->states);
        }
    }
    fawait(async_vgather(3,
                         async_new(add, res, ASYNC_NONE),
                         async_new(add, res, ASYNC_NONE),
                         async_new(add, res, ASYNC_NONE))
                         ){
    }
    async_end;
}

static async cycle_counter(s_astate state) {
    int *res = state->args;
    async_begin(state);
    while (1) {
        *res += 1;
        async_yield;
    }
    async_end;
}

s_astate count_cycles(int *res){
    s_astate state;
    ASYNC_PREPARE_NOARGS(cycle_counter, state, ASYNC_NONE, NULL, fail);
    state->args = res;
    return state;
    fail:
    return NULL;
}

static async yielder(s_astate state) {
    async_begin(state);
    async_yield;
    async_yield;
    async_yield;
    async_end;
}


static async waiter(s_astate state) {
    int *res = state->args;
    s_astate st;
    async_begin(state);
    fawait(async_wait_for((st = async_sleep(1000)), 0)){
        if (async_errno == ASYNC_ENOMEM){
            async_free_coro_(st);
        }
    }
    *res = async_errno;
    fawait(async_wait_for(st = async_sleep(2), 10)){
        if (async_errno == ASYNC_ENOMEM){
            async_free_coro_(st);
        }
    }
    async_end;
}

#define container_of(ptr, type, member) \
    (type *) ((char *) (ptr) - offsetof(type, member))

typedef struct {
    struct async_event_loop _base;
    int add_counter;
} better_loop;


static s_astate my_addtask(struct astate *state) {
    better_loop *bloop = container_of(async_get_event_loop(), better_loop, _base);
    s_astate res = async_default_event_loop->add_task(state);
    if (res) bloop->add_counter++;
    return res;
}


int main(void) {
    struct async_event_loop *loop;
    srand((unsigned int) time(NULL));
    loop = async_get_event_loop();
    {
        struct astate *arr[3] = {async_sleep(0), async_sleep(0), async_sleep(0)};
        struct astate **a_res;
        test_section("add_task[s]");
        loop->init();
        a_res = async_create_tasks(3, arr);
        test_assert(!!async_create_task(async_sleep(0)));
        test_assert(a_res != NULL);
        if (!a_res) {
            async_free_coros_(3, arr);
        }
        test_assert(loop->events_queue.length == 4);
        loop->destroy();
    }

    {
        struct astate *state;
        int res = 1;
        test_section("async_cancel");
        loop->init();
        test_assert((state = async_create_task(async_new(cancellable, &res, ASYNC_NONE))) != NULL);
        if (state) {
            async_set_on_cancel(state, cancellable_c);
            async_cancel(state);
        }
        loop->run_forever();
        test_assert(res == 42);
        loop->destroy();
    }

    {
        int err = 0;
        struct astate *state = async_new(errno_produce, &err, ASYNC_NONE);
        test_section("async_errno");
        loop->init();
        loop->run_until_complete(state);
        test_assert(err == ASYNC_ECANCELED);
        loop->destroy();
    }

    {
        int sum = 0;
        struct astate *state = async_new(gatherable, &sum, gatherable_stack);
        test_section("async_[v]gather");
        loop->init();
        loop->run_until_complete(state);
        test_assert(sum == 6);
        loop->destroy();
    }

    {
        time_t st;
        int i;
        double diff;
        test_section("loop->run_until_complete");
        loop->init();
        for (i = 0; i < 10; i++) {
            async_create_task(async_sleep(1000));
        }
        time(&st);
        loop->run_until_complete(async_sleep(1));
        diff = difftime(time(NULL), st);
        test_assert(1 <= diff && diff <= 2);
        loop->destroy();
    }

    {
        int err = 0;
        test_section("async_wait_for");
        loop->init();
        loop->run_until_complete(async_new(waiter, &err, ASYNC_NONE));
        test_assert(err == ASYNC_ECANCELED);
        loop->destroy();
    }

    {
        int n_event_loop_cycles = 0;
        struct astate *state = async_new(yielder, NULL, ASYNC_NONE);
        test_section("async_yield + loop cycles");
        loop->init();
        async_create_task(count_cycles(&n_event_loop_cycles));
        loop->run_until_complete(state);
        test_assert(n_event_loop_cycles == 3);
        loop->destroy();
    }

    {
        better_loop bloop = {*async_default_event_loop, 0};
        ((struct async_event_loop *) &bloop)->add_task = my_addtask;
        test_section("custom event loop");
        async_set_event_loop((struct async_event_loop *) &bloop);
        loop = async_get_event_loop();
        async_create_task(async_sleep(0));
        async_create_task(async_sleep(0));
        async_create_task(async_sleep(0));
        async_create_task(async_sleep(0));
        async_create_task(async_sleep(0));
        async_create_task(NULL);
        test_assert((container_of(loop, better_loop, _base))->add_counter == 5);
        loop->destroy();
        async_set_event_loop(async_default_event_loop);
        loop = async_get_event_loop();
        loop->init();
        test_assert(!!async_create_task(async_sleep(0)));
        loop->destroy();
    }
    test_print_res();
    return fail_count != 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}