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
#include <string.h>
#include <stdlib.h>

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

#define async_arr_destroy(arr)        \
    (                                 \
        free((arr)->data),            \
        async_arr_init(arr)           \
    )

#define async_arr_push(arr, val)                            \
    (                                                       \
        async_arr_expand_(async_arr_unpack_(arr), 1)        \
            ? ((arr)->data[(arr)->length++] = (val), 1)     \
            : 0                                             \
    )

#define async_arr_reserve(arr, n) (async_arr_expand_(async_arr_unpack_(arr), n))

#define async_arr_unpack_(arr)\
  (char**)&(arr)->data, &(arr)->length, &(arr)->capacity, sizeof(*(arr)->data)

#define async_arr_splice(arr, start, count)                           \
    (                                                                 \
        async_arr_splice_(async_arr_unpack_(arr), start, count),      \
        (arr)->length -= (count)                                      \
    )

static int async_all_(size_t n, struct astate **states) { /* Returns false if at least one state is NULL */
    while (n--) { if (states[n] == NULL) { return 0; }}
    return 1;
}

static async_arr_(astate *) async_events_queue_; /* singleton array of async states */

/* Free astate, its allocs and invalidate it completely */
#define STATE_FREE(state)                                 \
    free(state->_locals);                                 \
    while (state->_allocs.length--) {                     \
        free(state->_allocs.data[state->_allocs.length]); \
    }                                                     \
    async_arr_destroy(&state->_allocs);                   \
    free(state)


#define ASYNC_LOOP_HEAD \
    size_t i;           \
    struct astate *state

#define ASYNC_LOOP_BODY_BEGIN                                                          \
    for (i = 0; i < async_events_queue_.length; i++) {                                 \
        state = async_events_queue_.data[i];                                           \
        if (state->_ref_cnt == 0) {                                                    \
            if (!async_done(state) && state->_cancel) {                                \
                state->_cancel(state, state->_args, state->_locals);                   \
            }                                                                          \
            STATE_FREE(state);                                                         \
            async_arr_splice(&async_events_queue_, i, 1);                              \
            i--;                                                                       \
        } else if (state->must_cancel) {                                               \
            if (state->err == ASYNC_ERR_CANCELLED) {                                   \
                /* Task is already cancelled. Let it live until no references left. */ \
                continue;                                                              \
            }                                                                          \
            if (!async_done(state)) {                                                  \
                ASYNC_DECREF(state);                                                   \
                if (state->_cancel != NULL) {                                          \
                    state->_cancel(state, state->_args, state->_locals);               \
                }                                                                      \
            }                                                                          \
            if (state->_next) {                                                        \
                ASYNC_DECREF(state->_next);                                            \
                async_cancel(state->_next);                                            \
            }                                                                          \
            state->err = ASYNC_ERR_CANCELLED;                                          \
            state->_async_k = ASYNC_DONE;                                              \
        }

#define ASYNC_LOOP_BODY_END \
    }                       \
    (void) 0

#define ASYNC_LOOP_RUNNER_BODY                                     \
    ASYNC_LOOP_BODY_BEGIN                                          \
    else if (!async_done(state)) {                                 \
        /* Nothing special to do with this function, let it run */ \
        state->_func(state, state->_args, state->_locals);         \
    }                                                              \
    ASYNC_LOOP_BODY_END


#define ASYNC_LOOP_DESTRUCTOR_BODY                                \
    ASYNC_LOOP_BODY_BEGIN                                         \
    else if (!async_cancelled(state)) {                           \
        /* Nothing special to do with this function, cancel it */ \
        async_cancel(state);                                      \
        i--;                                                      \
    }                                                             \
    ASYNC_LOOP_BODY_END

void async_loop_run_forever_(void) {
    ASYNC_LOOP_HEAD;
    while (async_events_queue_.length > 0) {
        ASYNC_LOOP_RUNNER_BODY;
    }
}


void async_loop_run_until_complete_(struct astate *main) {
    ASYNC_LOOP_HEAD;
    while (main->_func(main, main->_args, main->_locals) != ASYNC_DONE) {
        ASYNC_LOOP_RUNNER_BODY;
    }
    STATE_FREE(main);
}

void async_loop_destroy_(void) {
    ASYNC_LOOP_HEAD;
    while (async_events_queue_.length > 0) {
        ASYNC_LOOP_DESTRUCTOR_BODY;
    }
    async_arr_destroy(&async_events_queue_);
}

struct astate *async_loop_add_task_(struct astate *state) {
    if (state == NULL) {
        return NULL;
    }
    if (!state->is_scheduled) {
        state->is_scheduled = 1;
        if (!async_arr_push(&async_events_queue_, state)) { return NULL; }
    }
    return state;
}

struct astate **async_loop_add_tasks_(size_t n, struct astate **states) {
    size_t i;
    if (states == NULL || !async_all_(n, states) || !async_arr_reserve(&async_events_queue_, n)) { return NULL; }
    for (i = 0; i < n; i++) {
        if (!states[i]->is_scheduled) {
            /* push would never fail here as we've reserved enough memory already, no need to check the return value */
            async_arr_push(&async_events_queue_, states[i]);
        }
    }
    return states;
}

struct astate *async_new_coro_(AsyncCallback child_f, void *args, size_t stack_size) {
    struct astate *state;

    state = calloc(1, sizeof(*state));
    if (state == NULL) { return NULL; }
    state->_locals = calloc(1, stack_size);
    if (state->_locals == NULL) {
        free(state);
        return NULL;
    }
    state->_func = child_f;
    state->_args = args;
    state->_ref_cnt = 1; /* State has 1 reference set as function "owns" itself until exited or cancelled */
    /* state->_async_k = ASYNC_INIT; state is already ASYNC_INIT because calloc */
    return state;
}

void async_loop_init_(void) { async_arr_init(&async_events_queue_); }

typedef struct {
    struct astate **arr_coros;
    size_t n_coros;
} gathered_stack;

void async_gathered_cancel(struct astate *state, void *args, void *locals_) {
    gathered_stack *locals = locals_;
    size_t i;
    (void) state;
    (void) args;
    for (i = 0; i < locals->n_coros; i++) {
        ASYNC_DECREF(locals->arr_coros[i]);
        async_cancel(locals->arr_coros[i]);
    }
}

static async async_gatherer(struct astate *state, void *args, void *locals_) {
    gathered_stack *locals = locals_;
    size_t i;
    int done;
    async_begin(state);
            (void) args;
            while (1) {
                done = 1;
                for (i = 0; i < locals->n_coros; i++) {
                    if (!async_done(locals->arr_coros[i])) {
                        done = 0;
                    } else { /* Remove coroutine from list of tracked coros */
                        ASYNC_DECREF(locals->arr_coros[i]);
                        locals->n_coros--;
                        memmove(&locals->arr_coros[i], &locals->arr_coros[i + 1],
                                sizeof(*locals->arr_coros) * (locals->n_coros - i));
                        i--;
                    }
                }
                if (done) {
                    break;
                } else {
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

    state = async_new_coro_(async_gatherer, NULL, sizeof(*stack));
    if (state == NULL) { return NULL; }

    async_set_on_cancel(state, async_gathered_cancel);
    stack = state->_locals;
    stack->n_coros = n;
    stack->arr_coros = async_alloc_(state, sizeof(state) * n);
    if (stack->arr_coros == NULL) {
        STATE_FREE(state);
        return NULL;
    }

    va_start(v_args, n);
    for (i = 0; i < n; i++) {
        stack->arr_coros[i] = va_arg(v_args, struct astate *);
    }

    if (!async_loop_add_tasks_(n, stack->arr_coros)) {
        for (i = 0; i < n; i++) {
            if (stack->arr_coros[i] != NULL) {
                STATE_FREE(stack->arr_coros[i]);
            }
        }
        STATE_FREE(state);
    }
    for (i = 0; i < n; i++) {
        ASYNC_INCREF(stack->arr_coros[i]);
    }

    va_end(v_args);
    return state;
}


struct astate *async_gather(size_t n, struct astate **states) {
    struct astate *state;
    gathered_stack *stack;
    size_t i;

    state = async_new_coro_(async_gatherer, NULL, sizeof(*stack));
    if (state == NULL) { return NULL; }
    async_set_on_cancel(state, async_gathered_cancel);

    stack = state->_locals;
    stack->n_coros = n;
    stack->arr_coros = states;
    if (!async_loop_add_tasks_(n, states)) {
        STATE_FREE(state);
        return NULL;
    }
    for (i = 0; i < n; i++) {
        ASYNC_INCREF(stack->arr_coros[i]);
    }
    return state;
}

typedef struct {
    time_t timer;
    time_t sec;
} sleeper_stack;


static async async_sleeper(struct astate *state, void *args, void *locals_) {
    sleeper_stack *locals = locals_;
    async_begin(state);
            (void) args;
            locals->timer = time(NULL);
            await(locals->timer + locals->sec <= time(NULL));
    async_end;
}

struct astate *async_sleep(time_t delay) {
    struct astate *state;
    sleeper_stack *stack;

    state = async_new_coro_(async_sleeper, NULL, sizeof(*stack));
    if (state == NULL) { return NULL; }
    stack = state->_locals;
    stack->sec = delay;
    return state;
}

typedef struct {
    time_t sec;
} waiter_stack;

static void async_waiter_cancel(struct astate *state, void *args, void *locals) {
    struct astate *child = args;
    (void) state;
    (void) locals;
    async_cancel(child);
    ASYNC_DECREF(child);
}

static async async_waiter(struct astate *state, void *args, void *locals_) {
    waiter_stack *locals = locals_;
    struct astate *child = args;
    async_begin(state);
            (void) args;
            if (!async_create_task(child)) {
                async_errno = ASYNC_ERR_NOMEM;
                async_exit;
            }
            fawait(async_sleep(locals->sec));
            if (!async_done(child)) {
                async_errno = ASYNC_ERR_CANCELLED;
                async_cancel(child);
            }
            ASYNC_DECREF(child);
    async_end;
}

struct astate *async_wait_for(struct astate *state_, time_t timeout) {
    struct astate *state;
    waiter_stack *stack;
    if (state_ == NULL) { return NULL; }
    state = async_new_coro_(async_waiter, state_, sizeof(*stack));
    if (state == NULL) { return NULL; }
    async_set_on_cancel(state, async_waiter_cancel);
    stack = state->_locals; /* Predefine locals. This trick can be used to create friendly methods. */
    stack->sec = timeout;
    ASYNC_INCREF(state_);
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
    while (i--){
        obj = state->_allocs.data[i];
        if (obj == mem) {
            free(obj);
            async_arr_splice(&state->_allocs, i, 1);
            return 1;
        }
    }
    return 0;
}
