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
#include "vec.h"
#include <stdarg.h>

static vec_t(astate *) async_events_queue_; /* singletone vector of async states */

/* Free astate and its allocs completely */
#define STATE_FREE(state, index_var)                                                \
    free(state->_locals);                                                           \
    for (index_var = 0; index_var < state->_n_allocs; index_var++) {                \
        free(state->_allocs[index_var]);                                            \
    }                                                                               \
    free(state->_allocs);                                                           \
    free(state)


#define ASYNC_LOOP_BODY                                                                 \
    int i;                                                                              \
    size_t aindex;                                                                      \
    astate *state;                                                                      \
    for (i = 0; i < async_events_queue_.length; i++) {                                  \
        state = async_events_queue_.data[i];                                            \
        if (state->_ref_cnt == 0) {                                                      \
            if (!async_done(state) && state->_cancel) {                                 \
                state->_cancel(state, state->_args, state->_locals);                    \
            }                                                                           \
        STATE_FREE(state, aindex);                                                      \
        vec_splice(&async_events_queue_, i, 1);                                         \
        i--;                                                                            \
        } else if (state->must_cancel) {                                                \
            if (state->err == ASYNC_ERR_CANCELLED) {                                    \
                /* Task is already cancelled. Let it live until no references left. */  \
                continue;                                                               \
            }                                                                           \
            if (!async_done(state)) {                                                   \
                async_DECREF(state);                                                    \
                if(state->_cancel != NULL){                                             \
                    state->_cancel(state, state->_args, state->_locals);                \
                }                                                                       \
                state->err = ASYNC_ERR_CANCELLED;                                       \
                state->_async_k = ASYNC_DONE;                                           \
            }                                                                           \
            if (state->_next) {                                                         \
                if (!async_done(state->_next)){                                         \
                    async_DECREF(state->_next);                                         \
                }                                                                       \
                state->_next->must_cancel = 1;                                          \
            }                                                                           \
        } else {                                                                        \
            /* Nothing special to do with this function, let it run */                  \
            state->_func(state, state->_args, state->_locals);                          \
        }                                                                               \
    } (void)0


void async_loop_run_forever_(void) {
    while (async_events_queue_.length > 0) {
        ASYNC_LOOP_BODY;
    }
}


void async_loop_run_until_complete_(struct astate *main) {
    size_t aindex;
    while (main->_func(main, main->_args, main->_locals) != ASYNC_DONE) {
        ASYNC_LOOP_BODY;
    }
    STATE_FREE(main, aindex);
}

void async_loop_destroy_(void) {
    int i;
    size_t aindex;
    struct astate *state;
    for (i = 0; i < async_events_queue_.length; i++) {
        state = async_events_queue_.data[i];
        if (state->_cancel) {
            state->_cancel(state, state->_args, state->_locals);
        }
        STATE_FREE(state, aindex);
    }
    vec_deinit(&async_events_queue_);
}

struct astate *async_loop_add_task_(struct astate *state) {
    int res;
    if (state == NULL) {
        return NULL;
    }
    if (!state->is_scheduled) {
        state->is_scheduled = 1;
        res = vec_push(&async_events_queue_, state);
        if (res == -1) {
            return NULL;
        }
    }
    return state;
}

struct astate *async_new_coro_(async_callback child_f, void *args, size_t stack_size) {
    struct astate *state = calloc(1, sizeof(*state));
    if (state == NULL) {
        return NULL;
    }
    state->_locals = calloc(1, stack_size);
    if (state->_locals == NULL) {
        free(state);
        return NULL;
    }
    state->_func = child_f;
    state->_args = args;
    state->_ref_cnt = 1;
    state->_async_k = ASYNC_INIT;
    return state;
}

void async_loop_init_(void) { vec_init(&async_events_queue_); }

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
        async_DECREF(locals->arr_coros[i]);
        async_cancel(locals->arr_coros[i]);
    }
}

static async async_gatherer(struct astate *state, void *args, void *locals_) {
    gathered_stack *locals = locals_;
    size_t i, x;
    int done;
    async_begin(state);
            (void) args;
            while (1) {
                done = 1;
                for (i = 0; i < locals->n_coros; i++) {
                    if (!async_done(locals->arr_coros[i])) {
                        done = 0;
                    } else { /* Remove coroutine from list of tracked coros */
                        async_DECREF(locals->arr_coros[i]);
                        locals->n_coros--;
                        for (x = i; x < locals->n_coros; x++) {
                            locals->arr_coros[x] = locals->arr_coros[x + 1];
                        }
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

    state = async_new_coro_(async_gatherer, NULL, sizeof(*stack) + sizeof(state) * n);
    if (state == NULL) {
        return NULL;
    }
    async_set_on_cancel(state, async_gathered_cancel);

    stack = state->_locals;
    stack->n_coros = n;
    stack->arr_coros = (struct astate **) (stack + 1);

    va_start(v_args, n);
    for (i = 0; i < n; i++) {
        stack->arr_coros[i] = va_arg(v_args, struct astate *);
        async_INCREF(stack->arr_coros[i]);
        async_loop_add_task_(stack->arr_coros[i]);
    }
    va_end(v_args);
    return state;
}


struct astate *async_gather(size_t n, struct astate **arr_) {
    struct astate *state;
    gathered_stack *stack;
    size_t i;

    state = async_new_coro_(async_gatherer, NULL, sizeof(*stack));
    if (state == NULL) {
        return NULL;
    }
    async_set_on_cancel(state, async_gathered_cancel);

    stack = state->_locals;
    stack->n_coros = n;
    stack->arr_coros = arr_;
    for (i = 0; i < n; i++) {
        async_INCREF(stack->arr_coros[i]);
        async_loop_add_task_(stack->arr_coros[i]);
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
    if (state == NULL) {
        return NULL;
    }
    stack = state->_locals;
    stack->sec = delay;
    return state;
}

typedef struct {
    time_t sec;
    struct astate *child;
} waiter_stack;

static void async_waiter_cancel(struct astate *state, void *args, void *locals_) {
    waiter_stack *locals = locals_;
    (void) args;
    (void) state;
    async_DECREF(locals->child);
}

static async async_waiter(struct astate *state, void *args, void *locals_) {
    waiter_stack *locals = locals_;
    async_begin(state);
            (void) args;
            async_create_task(locals->child);
            fawait(async_sleep(locals->sec));
            if (!async_done(locals->child)) {
                state->err = ASYNC_ERR_CANCELLED;
                async_cancel(locals->child);
            }
            async_DECREF(locals->child);

    async_end;
}

struct astate *async_wait_for(struct astate *state_, time_t timeout) {
    struct astate *state;
    waiter_stack *stack;
    state = async_new_coro_(async_waiter, NULL, sizeof(waiter_stack));
    if (state == NULL) {
        return NULL;
    }
    async_set_on_cancel(state, async_waiter_cancel);
    stack = state->_locals;
    stack->child = state_;
    stack->sec = timeout;
    async_INCREF(state_);
    return state;
}

void *async_alloc_(struct astate *state, size_t size) {
    size_t n;
    void *mem, **temp;
    if (state->_allocs_capacity < state->_n_allocs + 1) {
        n = (state->_allocs_capacity == 0) ? 1 : state->_allocs_capacity << 1;
        temp = realloc(state->_allocs, sizeof(*state->_allocs) * n);
        if (temp == NULL) { return NULL; }
        state->_allocs_capacity = n;
        state->_allocs = temp;
    }

    mem = malloc(size);
    if (mem == NULL) { return NULL; }
    state->_allocs[state->_n_allocs++] = mem;
    return mem;
}
