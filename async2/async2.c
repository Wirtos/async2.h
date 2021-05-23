/*
Copyright (c) 2020 Wirtos

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
 */
#include "async2.h"
#include <assert.h>
#include <stdarg.h> /* va_start, va_end, va_arg, va_list */
#include <stdlib.h> /* ma|re|calloc, free */
#include <string.h> /* memset, memmove */
#include <time.h>   /* clock_t, CLOCKS_PER_SEC */

#define ignored_  (void)

/*
 * event loop member functions declaration
 */
static struct astate *async_loop_add_task_(struct astate *state);

static struct astate **async_loop_add_tasks_(size_t n, struct astate **states);

static void async_loop_init_(void);

static void async_loop_run_forever_(void);

static void async_loop_run_until_complete_(struct astate *main);

static void async_loop_close_(void);

static void async_loop_stop_(void);

/* todo: optionally switch to arena allocator? */

static void async_loop_free_(void *ptr){
    free(ptr);
}

static void *async_loop_malloc_(size_t size){
    return malloc(size);
}

static void *async_loop_realloc_(void *block, size_t size){
    return realloc(block, size);
}

static void *async_loop_calloc_(size_t count, size_t size){
    return calloc(count, size);
}

#define STD_EVENT_LOOP_INIT         \
    async_loop_init_,               \
    async_loop_stop_,               \
    async_loop_close_,              \
    async_loop_add_task_,           \
    async_loop_add_tasks_,          \
    async_loop_run_forever_,        \
    async_loop_run_until_complete_, \
    async_loop_malloc_,             \
    async_loop_realloc_,            \
    async_loop_calloc_,             \
    async_loop_free_,               \
    {NULL, 0, 0},                   \
    {NULL, 0, 0},                   \
    0

/* Init default event loop, custom event loop should create own initializer instead. */
static struct async_event_loop async_standard_event_loop_ = {
    STD_EVENT_LOOP_INIT
};

const struct async_event_loop async_default_event_loop = {
    STD_EVENT_LOOP_INIT
};

static struct async_event_loop *event_loop = &async_standard_event_loop_;

/* array is inspired by rxi's vec: https://github.com/rxi/vec */
static int async_arr_expand_(char **data, const size_t *len, size_t *capacity, size_t memsz, size_t n_memb) {
    void *mem;
    size_t n, needed;

    needed = *len + n_memb;
    if (needed > *capacity) {
        n = (*capacity == 0) ? 1 : *capacity << 1;
        while (needed > n) { /* Calculate power of 2 for new capacity */
            n <<= 1;
        }
        mem = event_loop->realloc(*data, n * memsz);
        if (mem == NULL) return 0;
        *data = mem;
        *capacity = n;
    }
    return 1;
}


static void async_arr_splice_(
    char **data, const size_t *len, const size_t *capacity,
    size_t memsz, size_t start, size_t count) {
    (void) capacity;
    memmove(*data + start * memsz,
        *data + (start + count) * memsz,
        (*len - start - count) * memsz);
}

#define async_arr_init(arr) \
    (void)(memset((arr), 0, sizeof(*(arr))), (arr)->data = NULL)

#define async_arr_destroy(arr)             \
    (                                      \
            event_loop->free((arr)->data), \
            async_arr_init(arr)            \
    )

#define async_arr_push(arr, val)                                \
    (                                                           \
            async_arr_expand_(async_arr_unpack_(arr), 1)        \
                    ? ((arr)->data[(arr)->length++] = (val), 1) \
                    : 0                                         \
    )

#define async_arr_reserve(arr, n) \
    async_arr_expand_(async_arr_unpack_(arr), n)

#define async_arr_unpack_(arr) \
    (char **) &(arr)->data, &(arr)->length, &(arr)->capacity, sizeof(*(arr)->data)

#define async_arr_splice(arr, start, count)                          \
    (                                                                \
            async_arr_splice_(async_arr_unpack_(arr), start, count), \
            (arr)->length -= (count)                                 \
    )

#define async_arr_pop(arr) \
    (arr)->data[--(arr)->length]

static int async_all_(size_t n, struct astate **states) { /* Returns false if at least one state is NULL */
    while (n--) {
        if (states[n] == NULL) { return 0; }
    }
    return 1;
}

/* Free astate, its allocs and invalidate it completely */
#define STATE_FREE(state)                                                                 \
    {                                                                                     \
        if ((state)->_runner->destr) (state)->_runner->destr(state);                      \
        while ((state)->_allocs.length--) {                                               \
            async_get_event_loop()->free((state)->_allocs.data[(state)->_allocs.length]); \
        }                                                                                 \
        async_arr_destroy(&(state)->_allocs);                                             \
        async_get_event_loop()->free(state);                                              \
    }                                                                                     \
    (void) 0


#define ASYNC_LOOP_RUNNER_BLOCK_NOREFS                          \
    if (state->_refcnt == 0) {                                  \
        STATE_FREE(state);                                      \
        if (async_arr_push(&event_loop->_vacant_queue, i)) {    \
            event_loop->_events_queue.data[i] = NULL;           \
        } else {                                                \
            async_arr_splice(&event_loop->_events_queue, i, 1); \
            i--;                                                \
        }                                                       \
    }

/*
 * This block is faster than runner because it knows that you don't need event loop anymore,
 * so it can just free and NULL all states without references inside events queue
 */
#define ASYNC_LOOP_DESTRUCTOR_BLOCK_NOREFS        \
    if (state->_refcnt == 0) {                    \
        STATE_FREE(state);                        \
        event_loop->_events_queue.data[i] = NULL; \
        event_loop->_vacant_queue.length++;       \
    }

#define ASYNC_LOOP_RUNNER_BLOCK_CANCELLED                                   \
    else if (state->err != ASYNC_ECANCELLED && async_is_cancelled(state)) { \
        if (!async_is_done(state)) {                                        \
            ASYNC_DECREF(state);                                            \
        }                                                                   \
        if (state->_child) {                                                \
            ASYNC_DECREF(state->_child);                                    \
            async_cancel(state->_child);                                    \
        }                                                                   \
        state->err = ASYNC_ECANCELLED;                                      \
        state->_async_k = ASYNC_DONE;                                       \
    }

#define ASYNC_LOOP_BLOCK_BEGIN                               \
    for (i = 0; i < event_loop->_events_queue.length; i++) { \
        state = event_loop->_events_queue.data[i];           \
        if (state == NULL) {                                 \
            continue;                                        \
        }

/* Parent while loop breaks if all array elements are vacant (NULL'ed) */
#define ASYNC_LOOP_BLOCK_END \
    }(void)0

#define ASYNC_LOOP_RUNNER_BLOCK                                                           \
    size_t i;                                                                             \
    struct astate *state;                                                                 \
    ASYNC_LOOP_BLOCK_BEGIN                                                                \
                                                                                          \
    ASYNC_LOOP_RUNNER_BLOCK_NOREFS                                                        \
    ASYNC_LOOP_RUNNER_BLOCK_CANCELLED                                                     \
    else if (!async_is_done(state) && (!state->_child || async_is_done(state->_child))) { \
        /* Nothing special to do with this function, let it run */                        \
        state->_runner->coro(state);                                                      \
    }                                                                                     \
                                                                                          \
    ASYNC_LOOP_BLOCK_END

#define ASYNC_LOOP_DESTRUCTOR_BLOCK                               \
    size_t i;                                                     \
    struct astate *state;                                         \
    ASYNC_LOOP_BLOCK_BEGIN                                        \
                                                                  \
    ASYNC_LOOP_DESTRUCTOR_BLOCK_NOREFS                            \
    ASYNC_LOOP_RUNNER_BLOCK_CANCELLED                             \
    else if (!async_is_cancelled(state)) {                        \
        /* Nothing special to do with this function, cancel it */ \
        async_cancel(state);                                      \
        i--;                                                      \
    }                                                             \
                                                                  \
    ASYNC_LOOP_BLOCK_END

static void async_loop_run_forever_(void) {
    async_set_flag(event_loop, ASYNC_LOOP_FLAG_RUNNING);
    while (event_loop->_events_queue.length > 0 /* todo: && event_loop->_events_queue.length > event_loop->_vacant_queue.length*/) {
        ASYNC_LOOP_RUNNER_BLOCK;
    }
    async_unset_flag(event_loop, ASYNC_LOOP_FLAG_RUNNING);
}

static void async_loop_run_until_complete_(struct astate *main) {
    if (main == NULL) {
        return;
    }
    async_set_flag(event_loop, ASYNC_LOOP_FLAG_RUNNING);
    while (main->_runner->coro(main) != ASYNC_DONE) {
       ASYNC_LOOP_RUNNER_BLOCK;
    }
    async_unset_flag(event_loop, ASYNC_LOOP_FLAG_RUNNING);
    if (main->_refcnt == 0) {
        STATE_FREE(main);
    }
}

static void async_loop_init_(void) {
    async_arr_init(&event_loop->_events_queue);
    async_arr_init(&event_loop->_vacant_queue);
}

static void async_loop_close_(void) {
    assert((ignored_"Loop mustn't be running during loop->close call", !async_get_flag(event_loop, ASYNC_LOOP_FLAG_RUNNING)));
    while (event_loop->_events_queue.length > 0 /* todo: && event_loop->_events_queue.length > event_loop->_vacant_queue.length */) {
        ASYNC_LOOP_DESTRUCTOR_BLOCK;
    }
    async_arr_destroy(&event_loop->_events_queue);
    async_arr_destroy(&event_loop->_vacant_queue);
    event_loop = NULL;
}

static void async_loop_stop_(void){
    async_unset_flag(event_loop, ASYNC_LOOP_FLAG_RUNNING);
}

#define async_set_sheduled(task) (async_set_flag((task), ASYNC_TASK_FLAG_SCHEDULED))

#define async_is_sheduled(task) (async_get_flag(task, ASYNC_TASK_FLAG_SCHEDULED))

static struct astate *async_loop_add_task_(struct astate *state) {
    size_t i;
    if (state == NULL) return NULL;

    if (!async_is_sheduled(state)) {
        if (event_loop->_vacant_queue.length > 0) {
            i = async_arr_pop(&event_loop->_vacant_queue);
            event_loop->_events_queue.data[i] = state;
        } else {
            if (!async_arr_push(&event_loop->_events_queue, state)) {
                STATE_FREE(state);
                return NULL;
            }
        }
        async_set_sheduled(state);
    }
    return state;
}



static struct astate **async_loop_add_tasks_(size_t n, struct astate **states) {
    size_t i;
    if (states == NULL || !async_all_(n, states) || !async_arr_reserve(&event_loop->_events_queue, n)) { return NULL; }
    for (i = 0; i < n; i++) {
        if (!async_is_sheduled(states[i])) {
            /* push would never fail here as we've reserved enough memory already, no need to check the return value */
            async_arr_push(&event_loop->_events_queue, states[i]);
            async_set_sheduled(states[i]);
        }
    }
    return states;
}

struct astate *async_new_task_(const async_runner *runner, void *args) {
    struct astate *state = event_loop->calloc(1, runner->sizes.task_size ? runner->sizes.task_size : sizeof(*state));
    if (state == NULL) { return NULL; }

    state->stack = (runner->sizes.stack_size)
                      ? ((char *) state) + runner->sizes.stack_offset
                      : NULL;

    state->args = (runner->sizes.args_size)
                    ? (assert((ignored_ "Pointer to args must be non-NULL", args)),
                       memcpy(((char *) state) + runner->sizes.args_offset, args, runner->sizes.args_size))
                    : args;

    state->_runner = runner;
    state->_child = NULL;
    state->_allocs.data = NULL;
    state->_refcnt = 1; /* State has 1 reference set as function "owns" itself until exited or cancelled */
    /* state->_async_k = ASYNC_INIT; state is already ASYNC_INIT because calloc */
    return state;
}

void async_free_task_(struct astate *state) {
    if (state != NULL) {
        STATE_FREE(state);
    }
}

void async_free_tasks_(size_t n, struct astate *states[]) {
    while (n--) {
        if (states[n]) STATE_FREE(states[n]);
    }
}

#if 0
typedef struct {
    async_arr_t(struct astate *) arr_coros;
} gathered_stack;

static void async_gatherer_cancel(struct astate *state) {
    gathered_stack *locals = state->locals;
    size_t i;
    for (i = 0; i < locals->arr_coros.length; i++) {
        if (!locals->arr_coros.data[i]) continue;
        ASYNC_DECREF(locals->arr_coros.data[i]);
        async_cancel(locals->arr_coros.data[i]);
    }
}

static async async_gatherer(struct astate *state) {
    gathered_stack *locals = state->locals;
    size_t i;
    struct astate *child;
    async_begin(state);
            while (1) {
                for (i = 0; i < locals->arr_coros.length; i++) {
                    child = locals->arr_coros.data[i];
                    if (!child) continue;
                    if (!async_done(child)) {
                        goto cont;
                    } else { /* NULL coroutine in the list of tracked coros */
                        ASYNC_DECREF(child);
                        locals->arr_coros.data[i] = NULL;
                    }
                }
                break;
                cont :
                {
                    async_yield;
                }
            }
    async_end;
}

struct astate *async_vgather(size_t n, ...) {
    va_list v_args;
    gathered_stack *stack;
    struct astate *state;
    size_t i;

    ASYNC_PREPARE_NOARGS(async_gatherer, state, gathered_stack, async_gatherer_cancel, fail);

    stack = state->locals;
    async_arr_init(&stack->arr_coros);
    if (!async_arr_reserve(&stack->arr_coros, n) || !async_free_later_(state, stack->arr_coros.data)) {
        goto fail;
    }

        va_start(v_args, n);
    for (i = 0; i < n; i++) {
        stack->arr_coros.data[i] = va_arg(v_args, struct astate *);
    }
        va_end(v_args);
    stack->arr_coros.length = n;
    if (!async_create_tasks(n, stack->arr_coros.data)) {
        goto fail;
    }
    for (i = 0; i < n; i++) {
        ASYNC_INCREF(stack->arr_coros.data[i]);
    }
    return state;

    fail:
    if (state) {
        async_arr_destroy(&stack->arr_coros);
        STATE_FREE(state);
    }
        va_start(v_args, n);
    for (i = 0; i < n; i++) {
        state = va_arg(v_args, struct astate *);
        if (state) STATE_FREE(state);
    }
        va_end(v_args);
    return NULL;
}

struct astate *async_gather(size_t n, struct astate **states) {
    struct astate *state;
    gathered_stack *stack;
    size_t i;

    ASYNC_PREPARE_NOARGS(async_gatherer, state, gathered_stack, async_gatherer_cancel, fail);
    stack = state->locals;
    stack->arr_coros.capacity = n;
    stack->arr_coros.length = n;
    stack->arr_coros.data = states;
    if (!async_create_tasks(n, states)) {
        STATE_FREE(state);
        return NULL;
    }
    for (i = 0; i < n; i++) {
        ASYNC_INCREF(stack->arr_coros.data[i]);
    }
    return state;
    fail:
    return NULL;
}

typedef struct {
    double sec;
    clock_t start;
} sleeper_stack;

static async async_sleeper(struct astate *state) {
    sleeper_stack *locals = state->locals;
    async_begin(state);
            locals->start = clock();
            await_while((double) (clock() - locals->start) / CLOCKS_PER_SEC < locals->sec);
    async_end;
}

struct astate *async_sleep(double delay) {
    struct astate *state;
    sleeper_stack *stack;
    if (delay == 0) {
        ASYNC_PREPARE_NOARGS(async_yielder, state, ASYNC_NONE, NULL, fail);
    } else {
        ASYNC_PREPARE_NOARGS(async_sleeper, state, sleeper_stack, NULL, fail);
        stack = state
            ->locals; /* Yet another predefined locals trick for mere optimisation, use async_alloc_ in real adapter functions instead. */
        stack->sec = delay;
    }
    return state;
    fail:
    return NULL;
}

typedef struct {
    double sec;
    clock_t start;
} waiter_stack;

static void async_waiter_cancel(struct astate *state) {
    struct astate *child = state->args;
    if (child == NULL) return;
    if (async_create_task(child)) {
        if (!async_done(child)) {
            async_cancel(child);
        }
        ASYNC_DECREF(child);
    }
}

static async async_waiter(struct astate *state) {
    waiter_stack *locals = state->locals;
    struct astate *child = state->args;
    async_begin(state);
            if (!async_create_task(child)) {
                state->args = NULL;
                async_errno = ASYNC_ENOMEM;
                async_exit;
            }
            locals->start = clock();
            await_while(!async_done(child) && (double) (clock() - locals->start) / CLOCKS_PER_SEC < locals->sec);
            if (!async_done(child)) {
                async_errno = ASYNC_ECANCELED;
                async_cancel(child);
            }
            ASYNC_DECREF(child);
    async_end;
}

struct astate *async_wait_for(struct astate *child, double timeout) {
    struct astate *state;
    waiter_stack *stack;
    if (child == NULL) { return NULL; }
    ASYNC_PREPARE_NOARGS(async_waiter, state, waiter_stack, async_waiter_cancel, fail);
    stack = state->locals; /* Predefine locals. This trick can be used to create friendly methods. */
    state->args = child;
    stack->sec = timeout;
    ASYNC_INCREF(child);
    return state;
    fail:
        STATE_FREE(child);
    return NULL;
}

#endif

void *async_alloc_(struct astate *state, size_t size) {
    void *mem;
    if (state == NULL) { return NULL; }
    mem = event_loop->malloc(size);
    if (mem == NULL) { return NULL; }
    if (!async_arr_push(&state->_allocs, mem)) {
        event_loop->free(mem);
        return NULL;
    }
    return mem;
}

int async_free_(struct astate *state, void *mem) {
    size_t i;
    void *obj;

    i = state->_allocs.length;
    while (i--) {
        obj = state->_allocs.data[i];
        if (obj == mem) {
            event_loop->free(obj);
            async_arr_splice(&state->_allocs, i, 1);
            return 1;
        }
    }
    return 0;
}

int async_free_later_(struct astate *state, void *mem) {
    if (mem == NULL || !async_arr_push(&state->_allocs, mem)) return 0;
    return 1;
}

struct async_event_loop *async_get_event_loop(void) {
    return event_loop;
}

void async_set_event_loop(struct async_event_loop *loop) {
    event_loop = loop;
}

const char *async_strerror(async_error err) {
    switch (err) {
        case ASYNC_OK:
            return "OK";
        case ASYNC_ENOMEM:
            return "MEMORY ALLOCATION ERROR";
        case ASYNC_ECANCELLED:
            return "COROUTINE WAS CANCELLED";
        case ASYNC_EINVAL_STATE:
            return "INVALID STATE WAS PASSED TO COROUTINE";
        default:
            return "UNKNOWN ERROR";
    }
}
void async_args_destructor(struct astate *state) {
    event_loop->free(state->args);
}
void async_run(struct astate *state) {
    if(async_get_flag(event_loop, ASYNC_LOOP_FLAG_CLOSED)){
        event_loop->init();
    }
    event_loop->run_until_complete(state);
    event_loop->close();
}
