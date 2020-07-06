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
 * = Stackful Async Subroutines =
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

#include <stddef.h> /* NULL, offsetof */

#ifdef _MSC_VER
    /* silent MSVC's warning on unnamed type definition in parentheses for _ASYNC_COMPUTE_OFFSET because it's valid C */
    #pragma warning(disable : 4116)
#endif

#ifdef ASYNC_DEBUG
    #include <stdio.h> /* fprintf, stderr */
#endif

/*
 * The async computation status
 */
typedef enum ASYNC_EVT {
    ASYNC_INIT, ASYNC_CONT, ASYNC_DONE
} async;

typedef enum ASYNC_ERR {
    ASYNC_OK = 0, ASYNC_ENOMEM = 12, ASYNC_ECANCELED = 42, ASYNC_EINVAL_STATE
} async_error;

#define _ASYNC_FLAG_SHEDULED (0x1) /* 0b1 */
#define _ASYNC_FLAG_MUST_CANCEL (0x1 << 1) /* 0b10 */

/*
 * Core async type to imply empty locals when creating new coro
 */
typedef char ASYNC_NONE;

typedef struct astate *s_astate;


/*
 * Core async type, every async function must follow this signature.
 */
typedef async (*AsyncCallback)(struct astate *);

typedef void (*AsyncCancelCallback)(struct astate *);

/*
 * Figures out proper offset from struct beginning to T_b
 * in order to allocate struct capable storing both Types a and b in one go
 * and prevent unaligned memory access
 */
#define _ASYNC_COMPUTE_OFFSET(T_a, T_b)\
  offsetof(struct{T_a a; T_b b;}, b)

#define async_arr_t(T)\
  struct { T *data; size_t length, capacity; }

struct astate {
    /* user-accessible values: */
    void *args; /* args to be passed along with state to the async function */
    void *locals; /* function's stack pointer (locals_t) to be passed with state to the async function */
    async_error err; /* ASYNC_OK(0) if state has no errors, other async_error otherwise, also might be a custom error code defined by function that sets errno itself */
    /* internal numeric values: */
    size_t _refcnt; /* reference count number of functions still using this state. 1 by default, because coroutine owns itself too. If number of references is 0, the state becomes invalid and will be freed by the event loop soon */
    unsigned int _async_k; /* current execution state. ASYNC_EVT if <= ASYNC_DONE and number of line in the function otherwise (means that state (or its function) is still running) */
    char _flags; /* default event loop functions use first 2 bit flags: FLAG_SHEDULED and FLAG_MUST_CANCEL, custom event loop might support more */
    /* containers: */
    AsyncCallback _func; /* function to be called by the event loop */
    AsyncCancelCallback _cancel; /* function to be called in case of cancelling state, can be NULL */
    s_astate _next; /* child state used by fawait */

    async_arr_t(void*) _allocs; /* array of memory blocks allocated by async_alloc and managed by the event loop */

    #ifdef ASYNC_DEBUG
    const char *debug_taskname; /* must never be explicitly initialized */
    #endif
};

struct async_event_loop {

    void (*init)(void);

    void (*destroy)(void);

    struct astate *(*add_task)(struct astate *state);

    struct astate **(*add_tasks)(size_t n, struct astate **states);

    void (*run_forever)(void);

    void (*run_until_complete)(struct astate *main_state);

    /* Main tasks queue */
    async_arr_t(struct astate *) events_queue;
    /* Helper stack to keep track of vacant indices, allows to avoid slow array
    * slicing when there's a lot of tasks with a cost of bigger memory footprint */
    async_arr_t(size_t) vacant_queue;
};

extern struct async_event_loop *async_default_event_loop;

#define ASYNC_INCREF(coro) coro->_refcnt++

#define ASYNC_DECREF(coro) coro->_refcnt--

#define ASYNC_XINCREF(coro) if(coro) ASYNC_INCREF(coro)

#define ASYNC_XDECREF(coro) if(coro) ASYNC_DECREF(coro)


/*
 * Mark the start of an async subroutine
 * Unknown continuation values now set async_errno to ASYNC_EINVAL_STATE.
 */
#ifdef ASYNC_DEBUG
#define async_begin(k)                                     \
    struct astate *_async_p = k;                           \
    fprintf(stderr, "<ADEBUG> Entered '%s'\n", __func__);  \
    switch(_async_p->_async_k) {                           \
    case ASYNC_INIT:                                       \
        fprintf(stderr, "<ADEBUG> Begin '%s'\n", __func__);\
        _async_p->debug_taskname = __func__
#else
#define async_begin(k)                      \
    struct astate *_async_p = k;            \
    switch(_async_p->_async_k) {            \
    case ASYNC_INIT: (void)0
#endif

/*
 * Mark the end of a async subroutine
 */
#ifdef ASYNC_DEBUG
#define async_end                                                                                        \
    _async_p->_async_k=ASYNC_DONE;                                                                       \
    ASYNC_DECREF(_async_p);                                                                              \
    fprintf(stderr, "<ADEBUG> Ended '%s'\n", __func__);                                                  \
    /* fall through */                                                                                   \
    case ASYNC_DONE:                                                                                     \
        return ASYNC_DONE;                                                                               \
    default:                                                                                             \
        async_errno = ASYNC_EINVAL_STATE;                                                              \
        fprintf(stderr, "<ADEBUG> WARNING: %s: %s(%d)\n", async_strerror(async_errno), __FILE__, __LINE__);\
        return ASYNC_DONE;} (void) 0
#else
#define async_end                           \
    _async_p->_async_k = ASYNC_DONE;        \
    ASYNC_DECREF(_async_p);                 \
    /* fall through */                      \
    case ASYNC_DONE:                        \
        return ASYNC_DONE;                  \
    default:                                \
        async_errno = ASYNC_EINVAL_STATE; \
        return ASYNC_DONE;} (void) 0
#endif

/*
 * Wait while the condition succeeds (optional)
 *
 * Continuation state is now callee-saved like protothreads which avoids
 * duplicate writes from the caller-saved design.
 */
#ifdef ASYNC_DEBUG
#define await_while(cond)                                                                                            \
    _async_p->_async_k = __LINE__; /* fall through */  case __LINE__:                                                \
    if (cond) return (fprintf(stderr, "<ADEBUG> Awaited in '%s' %s(%d)\n", __func__, __FILE__, __LINE__), ASYNC_CONT)
#else
#define await_while(cond)                                             \
    _async_p->_async_k = __LINE__; /* fall through */  case __LINE__: \
    if (cond) return ASYNC_CONT
#endif
/*
 * Wait until the condition succeeds
 */
#define await(cond) await_while(!(cond))

/*
 * Yield execution
 */
#ifdef ASYNC_DEBUG
#define async_yield _async_p->_async_k = __LINE__; fprintf(stderr, "<ADEBUG> Yielded in '%s' %s(%d)\n", __func__, __FILE__, __LINE__); return ASYNC_CONT; /* fall through */ case __LINE__: (void)0
#else
#define async_yield _async_p->_async_k = __LINE__; return ASYNC_CONT; /* fall through */ case __LINE__: (void)0
#endif
/*
 * Exit the current async subroutine
 */
#ifdef ASYNC_DEBUG
#define async_exit _async_p->_async_k = ASYNC_DONE; ASYNC_DECREF(_async_p); fprintf(stderr, "<ADEBUG> Exited from '%s' %s(%d)\n", __func__, __FILE__, __LINE__); return ASYNC_DONE
#else
#define async_exit _async_p->_async_k = ASYNC_DONE; ASYNC_DECREF(_async_p); return ASYNC_DONE
#endif
/*
 * Cancels running coroutine
 */
#define async_cancel(coro) ((coro)->_flags |= _ASYNC_FLAG_MUST_CANCEL)

/*
 * returns 1 if function was cancelled
 */
#define async_cancelled(coro) (!!((coro)->_flags & _ASYNC_FLAG_MUST_CANCEL))

/*
 * Check if async subroutine is done
 */
#define async_done(coro) ((coro)->_async_k==ASYNC_DONE)


/*
 * Create a new coro
 */
#define async_new(call_func, args, locals_t)\
  async_new_coro_((call_func), (args), sizeof(locals_t), _ASYNC_COMPUTE_OFFSET(struct astate, locals_t))

/*
 * Create task from coro
 */
#define async_create_task(coro) (async_get_event_loop()->add_task(coro))

/*
 * Create tasks from array of states
 */
#define async_create_tasks(n, coros) (async_get_event_loop()->add_tasks(n, coros))

/*
 * Get async_error code for current execution state. Can be used to check for errors after fawait()
 */
#define async_errno (_async_p->err)

/*
 * Create task and wait until the coro succeeds. Resets async_errno and sets it.
 */

#define fawait(coro)                               \
        _async_p->_next = async_create_task(coro); \
        if (_async_p->_next) {                     \
            ASYNC_INCREF(_async_p->_next);         \
            await(async_done(_async_p->_next));    \
            ASYNC_DECREF(_async_p->_next);         \
            async_errno = _async_p->_next->err;    \
            _async_p->_next = NULL;                \
        } else { async_errno = ASYNC_ENOMEM; }     \
        if(async_errno != ASYNC_OK)

/*
 * Initial preparation for adapter functions like async_sleep
 */
#define ASYNC_PREPARE_NOARGS(async_callback, state, locals_t, cancel_f, err_label) \
    (state) = async_new(async_callback, NULL, locals_t);                           \
    if (!(state)) goto err_label;                                                  \
    async_set_on_cancel(state, cancel_f)

#define ASYNC_PREPARE(async_callback, state, args_size, locals_t, cancel_f, err_label) \
    ASYNC_PREPARE_NOARGS(async_callback, state, locals_t, cancel_f);                   \
    if (args_size) {                                                                   \
        (state)->args = async_alloc_((state), args_size);                              \
        if (!state->args) {                                                            \
            async_free_coro_(state);                                                   \
            goto err_label;                                                            \
        }                                                                              \
    }(void) 0


/*
 * Allocate memory that'll be freed automatically after async function ends.
 * Allows to avoid async_cancel callback.
 */
#define async_alloc(size) async_alloc_(_async_p, size)

#define async_free(ptr) async_free_(_async_p, ptr)

#define async_free_later(ptr) async_free_later_(_async_p, ptr)

/*
 * Set function to be executed on function cancellation once. Can be used to free memory and finish some tasks.
 */
#define async_set_on_cancel(coro, cancel_func) (coro->_cancel=cancel_func)

/*
 * Set function to be executed on function cancellation once. This version can be used inside the async function.
 * In this case cancel_func will be called only if async function has reached async_on_cancel statement
 * before async_cancel() was called on current state.
 */
#define async_on_cancel(cancel_func) async_set_on_cancel(_async_p, cancel_func)

/*
 * Run few variadic tasks in parallel
 */
struct astate *async_vgather(size_t n, ...);

/*
 * Does the same, but takes array and number of array elements.
 * Arr must not be freed before this coro is done or cancelled.
 * arr will be modified inside the task, so pass a copy if you need original array to be unchanged.
 */
struct astate *async_gather(size_t n, struct astate **states);

/*
 * Block for `delay` seconds
 */
struct astate *async_sleep(double delay);

/*
 * Execute function in `timeout` seconds or cancel it if timeout was reached.
 */
struct astate *async_wait_for(struct astate *child, double timeout);

struct async_event_loop *async_get_event_loop(void);

void async_set_event_loop(struct async_event_loop *);

/*
 * Internal functions, use with caution! (At least read the code)
 */
struct astate *async_new_coro_(AsyncCallback child_f, void *args, size_t stack_size, size_t stack_offset);

void async_free_coro_(struct astate *state);

void async_free_coros_(size_t n, struct astate **states);

void *async_alloc_(struct astate *state, size_t size);

int async_free_(struct astate *state, void *mem);

int async_free_later_(struct astate *state, void *mem);

const char *async_strerror(async_error err);

#endif
