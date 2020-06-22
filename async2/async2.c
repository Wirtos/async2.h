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
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/*
 * event loop member functions declaration
 */
static struct astate *async_loop_add_task_(struct astate *state);

static struct astate **async_loop_add_tasks_(size_t n, struct astate **states);

static void async_loop_init_(void);

static void async_loop_run_forever_(void);

static void async_loop_run_until_complete_(struct astate *main);

static void async_loop_destroy_(void);

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
        mem = realloc(*data, n * memsz);
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
    memset((arr), 0, sizeof(*(arr)))

#define async_arr_destroy(arr) \
    (                          \
            free((arr)->data), \
            async_arr_init(arr)\
    )

#define async_arr_push(arr, val)                                \
    (                                                           \
            async_arr_expand_(async_arr_unpack_(arr), 1)        \
                    ? ((arr)->data[(arr)->length++] = (val), 1) \
                    : 0                                         \
    )

#define async_arr_reserve(arr, n) async_arr_expand_(async_arr_unpack_(arr), n)

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

/* Init default event loop, custom event loop should create own initializer instead. */
static struct async_event_loop async_standard_event_loop_ = {
        async_loop_init_,
        async_loop_destroy_,
        async_loop_add_task_,
        async_loop_add_tasks_,
        async_loop_run_forever_,
        async_loop_run_until_complete_,
        {0, 0, 0},
        {0, 0, 0}, /* fill array structs with zeros */
};

struct async_event_loop *async_default_event_loop = &async_standard_event_loop_;

static struct async_event_loop *event_loop = &async_standard_event_loop_;


/* Free astate, its allocs and invalidate it completely */
#define STATE_FREE(state)                                         \
    {                                                             \
        free((state)->locals);                                    \
        while ((state)->_allocs.length--) {                       \
            free((state)->_allocs.data[(state)->_allocs.length]); \
        }                                                         \
        async_arr_destroy(&(state)->_allocs);                     \
        free(state);                                              \
    } (void) 0


#define ASYNC_LOOP_HEAD   \
    size_t i;             \
    struct astate *state; \
    int all_vacant /* bool used by ASYNC_BODY_END so if all states are NULL'ed we will break from parent loop */

#define ASYNC_LOOP_RUNNER_BLOCK_NOREFS                         \
    if (state->_ref_cnt == 0) {                                \
        if (!async_done(state) && state->_cancel != NULL) {    \
            state->_cancel(state);                             \
        }                                                      \
        STATE_FREE(state);                                     \
        if (async_arr_push(&event_loop->vacant_queue, i)) {    \
            event_loop->events_queue.data[i] = NULL;           \
        } else {                                               \
            async_arr_splice(&event_loop->events_queue, i, 1); \
            i--;                                               \
        }                                                      \
    }

/*
 * This block is faster than runner because it knows that you don't need event loop anymore,
 * so it can just free and NULL all states without references inside events queue
 */
#define ASYNC_LOOP_DESTRUCTOR_BLOCK_NOREFS                  \
    if (state->_ref_cnt == 0) {                             \
        if (!async_done(state) && state->_cancel != NULL) { \
            state->_cancel(state);                          \
        }                                                   \
        STATE_FREE(state);                                  \
        event_loop->events_queue.data[i] = NULL;            \
    }

#define ASYNC_LOOP_RUNNER_BLOCK_CANCELLED                               \
    else if (state->err != ASYNC_ERR_CANCELLED && state->must_cancel) { \
        if (!async_done(state)) {                                       \
            ASYNC_DECREF(state);                                        \
            if (state->_cancel != NULL) {                               \
                state->_cancel(state);                                  \
            }                                                           \
        }                                                               \
        if (state->_next) {                                             \
            ASYNC_DECREF(state->_next);                                 \
            async_cancel(state->_next);                                 \
        }                                                               \
        state->err = ASYNC_ERR_CANCELLED;                               \
        state->_async_k = ASYNC_DONE;                                   \
    }

#define ASYNC_LOOP_BODY_BEGIN                               \
    all_vacant = 1;                                         \
    for (i = 0; i < event_loop->events_queue.length; i++) { \
        state = event_loop->events_queue.data[i];           \
        if (state == NULL) {                                \
            continue;                                       \
        } else if (all_vacant) {                            \
            all_vacant = 0;                                 \
        }


/* Parent while loop breaks if all array elements are vacant (NULL'ed) */
#define ASYNC_LOOP_BODY_END \
    }                       \
    if (all_vacant) break

#define ASYNC_LOOP_RUNNER_BODY                                     \
    ASYNC_LOOP_BODY_BEGIN                                          \
    ASYNC_LOOP_RUNNER_BLOCK_NOREFS                                 \
    ASYNC_LOOP_RUNNER_BLOCK_CANCELLED                              \
    else if (!async_done(state)) {                                 \
        /* Nothing special to do with this function, let it run */ \
        state->_func(state);                                       \
    }                                                              \
    ASYNC_LOOP_BODY_END


#define ASYNC_LOOP_DESTRUCTOR_BODY                                \
    ASYNC_LOOP_BODY_BEGIN                                         \
    ASYNC_LOOP_DESTRUCTOR_BLOCK_NOREFS                            \
    ASYNC_LOOP_RUNNER_BLOCK_CANCELLED                             \
    else if (!async_cancelled(state)) {                           \
        /* Nothing special to do with this function, cancel it */ \
        async_cancel(state);                                      \
        i--;                                                      \
    }                                                             \
    ASYNC_LOOP_BODY_END

static void async_loop_run_forever_(void) {
    ASYNC_LOOP_HEAD;
    while (event_loop->events_queue.length > 0) {
        ASYNC_LOOP_RUNNER_BODY;
    }
}


static void async_loop_run_until_complete_(struct astate *main) {
    ASYNC_LOOP_HEAD;
    if (main == NULL) {
        return;
    }
    while (main->_func(main) != ASYNC_DONE) {
        ASYNC_LOOP_RUNNER_BODY;
    }
    if (main->_ref_cnt == 0) {
        STATE_FREE(main);
    }
}

static void async_loop_init_(void) {
    async_arr_init(&event_loop->events_queue);
    async_arr_init(&event_loop->vacant_queue);
}

static void async_loop_destroy_(void) {
    ASYNC_LOOP_HEAD;
    while (event_loop->events_queue.length > 0) {
        ASYNC_LOOP_DESTRUCTOR_BODY;
    }
    async_arr_destroy(&event_loop->events_queue);
    async_arr_destroy(&event_loop->vacant_queue);
}

static struct astate *async_loop_add_task_(struct astate *state) {
    size_t i;
    if (state == NULL) return NULL;

    if (!state->is_scheduled) {
        if (event_loop->vacant_queue.length > 0) {
            i = async_arr_pop(&event_loop->vacant_queue);
            event_loop->events_queue.data[i] = state;
        } else {
            if (!async_arr_push(&event_loop->events_queue, state)) {
                STATE_FREE(state);
                return NULL;
            }
        }
        state->is_scheduled = 1;
    }
    return state;
}

static struct astate **async_loop_add_tasks_(size_t n, struct astate **states) {
    size_t i;
    if (states == NULL || !async_all_(n, states) || !async_arr_reserve(&event_loop->events_queue, n)) { return NULL; }
    for (i = 0; i < n; i++) {
        if (!states[i]->is_scheduled) {
            /* push would never fail here as we've reserved enough memory already, no need to check the return value */
            async_arr_push(&event_loop->events_queue, states[i]);
        }
    }
    return states;
}

struct astate *async_new_coro_(AsyncCallback child_f, void *args, size_t stack_size) {
    struct astate *state;

    state = calloc(1, sizeof(*state));
    if (state == NULL) { return NULL; }
    state->locals = calloc(1, stack_size);
    if (state->locals == NULL) {
        free(state);
        return NULL;
    }
    state->_func = child_f;
    state->args = args;
    state->_ref_cnt = 1; /* State has 1 reference set as function "owns" itself until exited or cancelled */
    /* state->_async_k = ASYNC_INIT; state is already ASYNC_INIT because calloc */
    return state;
}

void async_free_coro_(struct astate *state) {
    if (state != NULL) {
        STATE_FREE(state);
    }
}

void async_free_coros_(size_t n, struct astate **states) {
    while (n--) {
        if (states[n]) STATE_FREE(states[n]);
    }
}

typedef struct {
    async_arr_(struct astate *) coros;
} gathered_stack;

static void async_gatherer_cancel(struct astate *state) {
    gathered_stack *locals = state->locals;
    size_t i;
    for (i = 0; i < locals->coros.length; i++) {
        ASYNC_DECREF(locals->coros.data[i]);
        async_cancel(locals->coros.data[i]);
    }
}

static async async_gatherer(struct astate *state) {
    gathered_stack *locals = state->locals;
    size_t i;
    struct astate *child;
    async_begin(state);
            while (1) {
                for (i = 0; i < locals->coros.length; i++) {
                    child = locals->coros.data[i];
                    if (!async_done(child)) {
                        goto cont;
                    } else { /* Remove coroutine from the list of tracked coros */
                        ASYNC_DECREF(child);
                        async_arr_splice(&locals->coros, i, 1);
                        i--;
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

    state = async_new(async_gatherer, NULL, gathered_stack);
    if (state == NULL) {
        va_start(v_args, n);
        for (i = 0; i < n; i++) {
            state = va_arg(v_args, struct astate *);
            if (state) STATE_FREE(state);
        }
        va_end(v_args);
        return NULL;
    }

    stack = state->locals;
    async_arr_init(&stack->coros);
    if (!async_arr_reserve(&stack->coros, n) || !async_free_later_(state, stack->coros.data)) {
        async_arr_destroy(&stack->coros);
        goto fail;
    }

    va_start(v_args, n);
    for (i = 0; i < n; i++) {
        stack->coros.data[i] = va_arg(v_args, struct astate *);
    }
    va_end(v_args);
    stack->coros.length = n;
    if (!async_create_tasks(n, stack->coros.data)) {
        for (i = 0; i < n; i++) {
            if (stack->coros.data[i]) STATE_FREE(stack->coros.data[i]);
        }
        fail:
    STATE_FREE(state);
        return NULL;
    }
    for (i = 0; i < n; i++) {
        ASYNC_INCREF(stack->coros.data[i]);
    }
    return state;
}


struct astate *async_gather(size_t n, struct astate **states) {
    struct astate *state;
    gathered_stack *stack;
    size_t i;

    ASYNC_PREPARE_NOARGS(async_gatherer, state, gathered_stack, async_gatherer_cancel);
    stack = state->locals;
    stack->coros.capacity = n;
    stack->coros.length = n;
    stack->coros.data = states;
    if (!async_create_tasks(n, states)) {
        STATE_FREE(state);
        return NULL;
    }
    for (i = 0; i < n; i++) {
        ASYNC_INCREF(stack->coros.data[i]);
    }
    return state;
}

typedef struct {
    time_t sec, end;
} sleeper_stack;


static async async_sleeper(struct astate *state) {
    sleeper_stack *locals = state->locals;
    async_begin(state);
            locals->end = time(NULL);
            await_while(difftime(time(NULL), locals->end) < locals->sec);
    async_end;
}

struct astate *async_sleep(time_t delay) {
    struct astate *state;
    sleeper_stack *stack;

    ASYNC_PREPARE_NOARGS(async_sleeper, state, sleeper_stack, NULL);
    stack = state->locals; /* Yet another predefined locals trick for mere optimisation, use async_alloc_ in real adapter functions instead. */
    stack->sec = delay;
    return state;
}

typedef struct {
    time_t sec, end;
} waiter_stack;

static void async_waiter_cancel(struct astate *state) {
    struct astate *child = state->args;
    if (child == NULL) return;
    if (child->is_scheduled) {
        if (!async_done(child)) {
            async_cancel(child);
        }
        ASYNC_DECREF(child);
    } else {
        STATE_FREE(child);
    }
}

static async async_waiter(struct astate *state) {
    waiter_stack *locals = state->locals;
    struct astate *child = state->args;
    async_begin(state);
            if (!async_create_task(child)) {
                state->args = NULL;
                async_errno = ASYNC_ERR_NOMEM;
                async_exit;
            }
            locals->end = time(NULL);
            await_while(!async_done(child) && difftime(time(NULL), locals->end) < locals->sec);
            if (!async_done(child)) {
                async_errno = ASYNC_ERR_CANCELLED;
                async_cancel(child);
            }
            ASYNC_DECREF(child);
    async_end;
}

struct astate *async_wait_for(struct astate *child, time_t timeout) {
    struct astate *state;
    waiter_stack *stack;
    if (child == NULL) { return NULL; }
    ASYNC_PREPARE_NOARGS(async_waiter, state, waiter_stack, async_waiter_cancel);
    stack = state->locals; /* Predefine locals. This trick can be used to create friendly methods. */
    state->args = child;
    stack->sec = timeout;
    ASYNC_INCREF(child);
    return state;
}

void *async_alloc_(struct astate *state, size_t size) {
    void *mem;
    if (state == NULL) { return NULL; }
    mem = malloc(size);
    if (mem == NULL) { return NULL; }
    if (!async_arr_push(&state->_allocs, mem)) {
        free(mem);
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
            free(obj);
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
