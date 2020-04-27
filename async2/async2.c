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
#include <stdbool.h>

static vec_t(astate *) async_events_queue_; // singletone vector of async states

void async_loop_run_forever_(void) {
    while (async_events_queue_.length > 0) {
        astate *state;
        for (size_t i = 0; i < async_events_queue_.length; i++) {
            state = async_events_queue_.data[i];
            if (async_done(state)) {
                free(state);
                vec_splice(&async_events_queue_, i, 1);
                i--;
            } else {
//                puts(state->args);
                state->f(state, state->args, state->locals);
            }
        }
    }
}

void async_loop_run_until_complete_(struct astate *main) {
    async_loop_add_task_(main);
    while (!async_done(main)) {
        astate *state;
        for (size_t i = 0; i < async_events_queue_.length; i++) {
            state = async_events_queue_.data[i];
            if (async_done(state)) {
                free(state);
                vec_splice(&async_events_queue_, i, 1);
                i--;
            } else {
                state->f(state, state->args, state->locals);
            }
        }
    }
}

void async_loop_destroy_(void) {
    int i;
    astate *state;
    vec_foreach(&async_events_queue_, state, i) {
            free(state);
        }
    vec_deinit(&async_events_queue_);
}

struct astate *async_loop_add_task_(struct astate *state) {
    vec_push(&async_events_queue_, state);
    return state;
}

struct astate *async_new_task_(async_callback child_f, void *args, size_t stack_size) {
    struct astate *state = malloc(sizeof(*state) + stack_size);
    async_init(state);
    state->locals = state + 1;
    state->f = child_f;
    state->args = args;
    state->next = NULL;
    return state;
}

void async_loop_init_(void) {
    vec_init(&async_events_queue_);
}

typedef struct {
    struct astate **arr_coros;
    size_t n_coros;
} gathered_stack;


static async async_gathered(struct astate *state, void *args, void *locals) {
    gathered_stack *stack = locals;
    (void) args;
    async_begin(state);
            for (size_t i = 0; i < stack->n_coros; i++) {
                async_loop_add_task_(stack->arr_coros[i]);
            }
            while (true) {
                bool done = true;
                for (size_t i = 0; i < stack->n_coros; i++) {
                    if (!async_done(stack->arr_coros[i])) {
                        done = false;
                    } else { /* Remove coroutine from list of tracked coros */
                        stack->n_coros--;
                        for (size_t x = i; x < stack->n_coros; x++) {
                            stack->arr_coros[x] = stack->arr_coros[x + 1];
                        }
                    }
                }
                if (done) {
                    async_exit;
                } else {
                    async_yield;
                }
            }
    async_end;
}


struct astate *async_vgather_(size_t n, ...) {
    va_list v_args;
    gathered_stack *stack;
    struct astate *state;

    state = async_new_task_(async_gathered, NULL, sizeof(gathered_stack) + sizeof(struct astate *) * n);
    stack = state->locals;
    stack->n_coros = n;
    stack->arr_coros = (struct astate **) (stack + 1);

    va_start(v_args, n);
    for (size_t i = 0; i < n; i++) {
        struct astate *s = va_arg(v_args, struct astate *);
        stack->arr_coros[i] = s;
    }
    va_end(v_args);
    return state;
}


struct astate *async_gather_(size_t n, struct astate **arr_) {
    struct astate *state;
    gathered_stack *stack;
    state = async_new_task_(async_gathered, NULL, sizeof(gathered_stack));
    stack = state->locals;
    stack->n_coros = n;
    stack->arr_coros = arr_;
    return state;
}
