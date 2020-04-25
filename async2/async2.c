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

vec_t(astate *) async_events_queue_; // singletone vector of async states

void async_loop_run_forever_(void) {
    while (async_events_queue_.length > 0) {
        int i;
        astate *state, *parent;
        vec_foreach(&async_events_queue_, state, i) {
                if (async_done(state) || state->f(state, state->args, state->locals)) {
                    free(state);
                    vec_splice(&async_events_queue_, i, 1);
                } else {
                    while ((parent = state, state = state->next)) {
                        if ((async_done(state) || state->f(state, state->args, state->locals))) {
                            parent->next = NULL;
                            free(state);
                            break;
                        }
                    }
                }
            }
    }
}


void async_loop_destroy_(void) {
    vec_deinit(&async_events_queue_);
}


struct astate *async_create_task_(async_callback child_f, void *args, size_t stack_size) {
    struct astate *state = malloc(sizeof(*state) + stack_size);
    async_init(state);
    state->locals = state + 1;
    state->f = child_f;
    state->args = args;
    state->next = NULL;
    return state;
}

struct astate *async_create_main_task_(async_callback main_f, void *args, size_t stack_size) {
    struct astate *state;
    state = async_create_task_(main_f, args, stack_size);
    vec_push(&async_events_queue_, state);
    return state;
}

void async_loop_init_(void) {
    vec_init(&async_events_queue_);
}

async async_gathered(struct astate *state, void *args, void *locals) {
    struct astate **arr = args, **begin = arr;
    struct astate *child;
    async_begin(state);
            while (true) {
                bool done = true;
                while (*arr) {
                    child = *arr;
                    if (!async_done(child) && !child->f(child, child->args, child->locals)) {
                        done = false;
                    }
                    arr++;
                }
                if (done) {
                    break;
                } else {
                    async_yield;
                }
            }
            arr = begin;
            while (*arr) { free(*arr++); }
            free(begin);
    async_end;
}


struct astate *async_vgather_(size_t n, ...) {
    va_list v_args;
    struct astate **arr;

    arr = malloc(sizeof(*arr) * (n + 1));
    arr[n] = NULL;
    va_start(v_args, n);
    for (size_t i = 0; i < n; i++) {
        arr[i] = va_arg(v_args, struct astate *);
    }
    va_end(v_args);
    return async_create_task_(async_gathered, arr, 1);
}

struct astate *async_gather_(size_t n, struct astate *const *arr_) {
    struct astate **arr;

    arr = malloc(sizeof(*arr) * (n + 1));
    arr[n] = NULL;
    for (size_t i = 0; i < n; i++) {
        arr[i] = arr_[i];
    }
    return async_create_task_(async_gathered, arr, 1);
}
