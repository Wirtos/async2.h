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

#ifndef ASYNC2_H
#define ASYNC2_H
/*
 * = Stackfull Async Subroutines =
 *
 * Taking inspiration from protothreads, async.h, coroutines.h and async/await as found in python
 * this is an async/await/fawait/event loop implementation for C based on Duff's device.
 *
 * Features:
 *
 * 1. Subroutines can have persistent state that isn't just static state, because
 *    each async subroutine accepts its own struct it uses as a parameter, and
 *    the async state is stored there.
 * 2. Because of the more flexible state handling, async subroutines can be nested
 *    in tree-like fashion which permits fork/join concurrency patterns.
 * 3. Every function can use persistent stack across subroutine call handled by event loop.
 * 4. Event loop with tasks running coros.
 * Caveats:
 *
 * 1. Due to compile-time bug, MSVC requires changing:
 *      Project Properties > Configuration Properties > C/C++ > General > Debug Information Format
 *    from "Program Database for Edit And Continue" to "Program Database".
 * 2. As with protothreads, you have to be very careful with switch statements within an async
 *    subroutine. Generally best to avoid them.
 * 3. As with protothreads, you can't make blocking system calls and preserve the async semantics.
 *    These must be changed into non-blocking calls that test a condition.
 */

#include <stddef.h>

/*
 * The async computation status
 */
typedef enum ASYNC_EVT {
    ASYNC_INIT = 0, ASYNC_CONT = ASYNC_INIT, ASYNC_DONE = 1
} async;

/*
 * Declare the async state
 */
#define async_state unsigned _async_k

/*
 * Core async type to imply empty locals when creating new_async
 */
#define ASYNC_NOLOCALS ""

/*
 * Core async structure, backwards compatibility with first async.h.
 */
struct async {
    async_state;
};

typedef struct astate astate;


/*
 * Core async type, every async function must follow this signature.
 */
typedef async (*async_callback)(struct astate *, void *, void *);

struct astate {
    async_state;
    async_callback f;
    void *locals;
    void *args;
    astate *next;
};


/*
 * Mark the start of an async subroutine
 *
 * Unknown continuation values now restart the subroutine from the beginning.
 */
#define async_begin(k)                      \
    struct astate *_async_p = k;            \
    unsigned *_async_k = &(k)->_async_k;    \
    switch(*_async_k) { default:


/*
 * Mark the end of a async subroutine
 */
#define async_end            \
    *_async_k=ASYNC_DONE;    \
    case ASYNC_DONE:         \
    return ASYNC_DONE; }

/*
 * Wait until the condition succeeds
 */
#define await(cond) await_while(!(cond))

/*
 * Wait while the condition succeeds (optional)
 *
 * Continuation state is now callee-saved like protothreads which avoids
 * duplicate writes from the caller-saved design.
 */
#define await_while(cond)                   \
    *_async_k = __LINE__; case __LINE__:    \
    if (cond) return ASYNC_CONT

/*
 * Yield execution
 */
#define async_yield *_async_k = __LINE__; return ASYNC_CONT; case __LINE__:

/*
 * Exit the current async subroutine
 */
#define async_exit *_async_k = ASYNC_DONE; return ASYNC_DONE

/*
 * Initialize a new async computation
 */
#define async_init(state) (state)->_async_k=ASYNC_INIT

/*
 * Cancels running coroutine
 */
#define async_cancel(coro) coro->_async_k = ASYNC_DONE

/*
 * Check if async subroutine is done
 */
#define async_done(state) ((state)->_async_k==ASYNC_DONE)


/*
 * Run until there's no tasks left
 */
#define async_loop_run_forever() async_loop_run_forever_()

/*
 * Run until main coro succeeds
 */
#define async_loop_run_until_complete(main_coro) async_loop_run_until_complete_(main_coro)

/*
 * Destroy event loop and clear all memory, loop can be inited again
 */
#define async_loop_destroy() async_loop_destroy_()

/*
 * Init event loop
 */
#define async_loop_init() async_loop_init_()

/*
 * Create a new coro
 */
#define async_new(func, args, locals) async_new_task_((async_callback)func, args, sizeof(locals))

/*
 * Create task from coro
 */
#define async_create_task(coro) async_loop_add_task_(coro)

/*
 * Run few tasks in parallel, returns coro
 */
#define async_gather(n, arr_states) async_gather_(n, arr_states)

/*
 * Run few variadic tasks in parallel, returns coro
 */
#define async_vgather(n, ...) async_vgather_(n, __VA_ARGS__)

/*
 * Create task and wait until the coro succeeds
 */
#define fawait(coro)                                        \
    _async_p->next = async_create_task(coro);               \
    *_async_k = __LINE__; case __LINE__:                    \
    if (!async_done(_async_p->next)) return ASYNC_CONT


struct astate *async_vgather_(size_t n, ...);

struct astate *async_gather_(size_t n, struct astate **arr_);

struct astate *async_new_task_(async_callback child_f, void *args, size_t stack_size);

struct astate *async_loop_add_task_(struct astate *state);

void async_loop_init_(void);

void async_loop_run_forever_(void);

void async_loop_run_until_complete_(struct astate *main);

void async_loop_destroy_(void);

#endif
