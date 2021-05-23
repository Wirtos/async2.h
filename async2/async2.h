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

#ifdef ASYNC_DEBUG
    #include <stdio.h> /* fprintf, stderr */
#endif

/*
 * The async computation status
 */
typedef enum {
    ASYNC_INIT, ASYNC_CONT, ASYNC_DONE
} async;

typedef enum {
    ASYNC_OK = 0, ASYNC_ENOMEM = 12,
    ASYNC_ECANCELLED = 42, ASYNC_EINVAL_STATE
} async_error;

#define async_set_flag(obj, flag)   (void)((obj)->_flags |=  (flag))
#define async_unset_flag(obj, flag) (void)((obj)->_flags &= ~(flag))
#define async_get_flag(obj, flag)         ((obj)->_flags &   (flag))

typedef enum {
    ASYNC_TASK_FLAG_SCHEDULED = 1u << 0,
    ASYNC_TASK_FLAG_CANCELLED = 1u << 1
} async_task_flags;

typedef enum {
    ASYNC_LOOP_FLAG_RUNNING = 1u << 0,
    ASYNC_LOOP_FLAG_CLOSED  = 1u << 1
} async_loop_flags;

typedef struct astate *s_astate;


/*
 * Core async type, every async function must follow this signature.
 */
typedef async (*AsyncCallback)(struct astate *);

typedef void (*AsyncDestructorCallback)(struct astate *);

#ifdef _MSC_VER
    /* silent MSVC's warning on unnamed type definition in parentheses because it's valid C */
    #pragma warning(disable : 4116)
#endif

/*
 * Initializes *_size fields for Async_Runner
 */
#define ASYNC_RUNNER_STACK_INIT(T_stack)                                   \
  {                                                                        \
      sizeof(T_stack), offsetof(struct {struct astate a; T_stack b; }, b), \
      0, 0,                                                                \
      sizeof(struct {struct astate a; T_stack b; })                        \
  }

#define ASYNC_RUNNER_ARGS_INIT(T_args)                                      \
  {                                                                         \
      0, 0,                                                                 \
      sizeof(T_args), offsetof(struct {struct astate a; T_args b; }, b),    \
      sizeof(struct {struct astate a; T_args b; })                          \
  }

#define ASYNC_RUNNER_STACK_ARGS_INIT(T_stack, T_args)                                 \
    {                                                                                 \
        sizeof(T_stack), offsetof(struct {struct astate a; T_stack b; }, b),          \
        sizeof(T_args), offsetof(struct {struct astate a; T_stack b; T_args c; }, c), \
        sizeof(struct {struct astate a; T_stack b; T_args c; })                       \
    }

#define async_vec_t(T)\
  struct { T *data; size_t length, capacity; }

typedef struct {
    AsyncCallback coro;
    AsyncDestructorCallback destr;
    struct {
        size_t stack_size;
        size_t stack_offset;
        size_t args_size;
        size_t args_offset;
        size_t task_size;
    } sizes;
} async_runner;

struct astate {
    /*  public: */
    void *args; /* args to be passed along with state to the async function */
    void *stack; /* function's stack pointer (T_stack) to be passed with state to the async function */
    async_error err; /* ASYNC_OK(0) if state has no errors, other async_error otherwise, also might be a custom error code defined by function that sets errno itself */

    /* protected: */
    async_task_flags _flags; /* default event loop functions use first 2 bit flags: FLAG_SCHEDULED and FLAG_MUST_CANCEL, custom event loop might support more */
    unsigned int _async_k; /* current execution state. ASYNC_EVT if <= ASYNC_DONE and number of line in the function otherwise (means that state (or its function) is still running) */
    unsigned int _refcnt; /* reference count number of functions still using this state. 1 by default, because coroutine owns itself too. If number of references is 0, the state becomes invalid and will be freed by the event loop soon */
    /* containers: */
    const async_runner *_runner;
    struct astate *_child; /* child state used by fawait */

    async_vec_t(void *) _allocs; /* array of memory blocks allocated by async_alloc and managed by the event loop */

    #ifdef ASYNC_DEBUG
    const char *_debug_taskname; /* must never be explicitly initialized */
    #endif
};

struct async_event_loop {

    void (*init)(void);

    void (*stop)(void);

    void (*close)(void);

    struct astate *(*add_task)(struct astate *state);

    struct astate **(*add_tasks)(size_t n, struct astate **states);

    void (*run_forever)(void);

    void (*run_until_complete)(struct astate *main_state);

    void *(*malloc)(size_t size);

    void *(*realloc)(void *ptr, size_t size);

    void *(*calloc)(size_t count, size_t size);

    void (*free)(void *ptr);

    /* Main tasks queue */
    async_vec_t(struct astate *) _events_queue;
    /* Helper stack to keep track of vacant indices, allows to avoid slow array
    * slicing when there's a lot of tasks with a cost of bigger memory footprint */
    async_vec_t(size_t) _vacant_queue;

    async_loop_flags _flags;
};

extern const struct async_event_loop async_default_event_loop;

#define ASYNC_INCREF(coro) (void) ((coro)->_refcnt++)

#define ASYNC_DECREF(coro) (void) ((coro)->_refcnt--)

#define ASYNC_XINCREF(coro) if(coro) ASYNC_INCREF(coro)

#define ASYNC_XDECREF(coro) if(coro) ASYNC_DECREF(coro)


/*
 * Mark the start of an async subroutine
 * Unknown continuation values now set async_errno to ASYNC_EINVAL_STATE.
 */
#ifdef ASYNC_DEBUG
    #define async_begin(st)                                     \
        struct astate *_async_ctx = st;                         \
        fprintf(stderr, "<ADEBUG> Entered '%s'\n", __func__);   \
        switch(_async_ctx->_async_k) {                          \
            case ASYNC_INIT:                                    \
            fprintf(stderr, "<ADEBUG> Begin '%s'\n", __func__); \
            _async_ctx->_debug_taskname = __func__
#else
    #define async_begin(st)               \
        struct astate *_async_ctx = st;   \
        switch(_async_ctx->_async_k) {    \
            case ASYNC_INIT: (void)0
#endif

/*
 * Mark the end of a async subroutine
 */
#ifdef ASYNC_DEBUG
    #define async_end                                                                                           \
        _async_ctx->_async_k = ASYNC_DONE;                                                                      \
        ASYNC_DECREF(_async_ctx);                                                                               \
        fprintf(stderr, "<ADEBUG> Exited '%s'\n", __func__);                                                    \
        /* fall through */                                                                                      \
        case ASYNC_DONE:                                                                                        \
            return ASYNC_DONE;                                                                                  \
        default:                                                                                                \
            async_errno = ASYNC_EINVAL_STATE;                                                                   \
            fprintf(stderr, "<ADEBUG> WARNING: %s: %s(%d)\n", async_strerror(async_errno), __FILE__, __LINE__); \
            return ASYNC_DONE;                                                                                  \
        }                                                                                                       \
        (void) 0
#else
    #define async_end                         \
        _async_ctx->_async_k = ASYNC_DONE;    \
        ASYNC_DECREF(_async_ctx);             \
        /* fall through */                    \
        case ASYNC_DONE:                      \
            return ASYNC_DONE;                \
        default:                              \
            async_errno = ASYNC_EINVAL_STATE; \
            return ASYNC_DONE;                \
        }                                     \
        (void) 0
#endif

/*
 * Wait while the condition succeeds (optional)
 *
 * Continuation state is now callee-saved like protothreads which avoids
 * duplicate writes from the caller-saved design.
 */
#ifdef ASYNC_DEBUG
    #define await_while(cond)                                                                                                        \
        _async_ctx->_async_k = __LINE__; /* fall through */  case __LINE__:                                                          \
        if (cond) return (fprintf(stderr, "<ADEBUG> Awaited in '%s' %s(%d)\n", __func__, __FILE__, _async_ctx->_async_k), ASYNC_CONT)
#else
    #define await_while(cond)                                               \
        _async_ctx->_async_k = __LINE__; /* fall through */  case __LINE__: \
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
    #define async_yield _async_ctx->_async_k = __LINE__; fprintf(stderr, "<ADEBUG> Yielded in '%s' %s(%d)\n", __func__, __FILE__, __LINE__); return ASYNC_CONT; /* fall through */ case __LINE__: (void)0
#else
    #define async_yield _async_ctx->_async_k = __LINE__; return ASYNC_CONT; /* fall through */ case __LINE__: (void)0
#endif
/*
 * Exit the current async subroutine
 */
#ifdef ASYNC_DEBUG
    #define async_exit _async_ctx->_async_k = ASYNC_DONE; ASYNC_DECREF(_async_ctx); fprintf(stderr, "<ADEBUG> Exited from '%s' %s(%d)\n", __func__, __FILE__, __LINE__); return ASYNC_DONE
#else
    #define async_exit _async_ctx->_async_k = ASYNC_DONE; ASYNC_DECREF(_async_ctx); return ASYNC_DONE
#endif
/*
 * Cancels running task
 */
#define async_cancel(task) async_set_flag(task, ASYNC_TASK_FLAG_CANCELLED)

/*
 * returns 1 if function was cancelled
 */
#define async_is_cancelled(task) async_get_flag(task, ASYNC_TASK_FLAG_CANCELLED)

/*
 * Check if async subroutine is done
 */
#define async_is_done(task) ((task)->_async_k==ASYNC_DONE)


/*
 * Create a new task
 */
#define async_new(arunner, args)\
  async_new_task_((arunner), (args))

/*
 * Create task from task
 */
#define async_create_task(task) (async_get_event_loop()->add_task(task))

/*
 * Create tasks from array of states
 */
#define async_create_tasks(n, tasks) (async_get_event_loop()->add_tasks(n, tasks))

/*
 * Get async_error code for current execution state. Can be used to check for errors after fawait()
 */
#define async_errno (_async_ctx->err)

/*
 * Create task and wait until it succeeds. Resets async_errno and sets it.
 */

#define fawait(task)                                          \
        if ((_async_ctx->_child = async_create_task(task))) { \
            ASYNC_INCREF(_async_ctx->_child);                 \
            await(async_is_done(_async_ctx->_child));         \
            ASYNC_DECREF(_async_ctx->_child);                 \
            async_errno = _async_ctx->_child->err;            \
            _async_ctx->_child = NULL;                        \
        } else { async_errno = ASYNC_ENOMEM; }                \
        if(async_errno != ASYNC_OK)

/*
 * Allocate memory that'll be freed automatically after async function ends.
 * Allows to avoid setting a separate destructor
 */
#define async_alloc(size) async_alloc_(_async_ctx, size)

#define async_free(ptr) async_free_(_async_ctx, ptr)

#define async_free_later(ptr) async_free_later_(_async_ctx, ptr)

/*
 * Set function to be executed on function cancellation once. This version can be used inside the async function.
 * In this case cancel_func will be called only if async function has reached async_on_cancel statement
 * before async_cancel() was called on current state.
 */
#define async_on_cancel(cancel_func) async_set_on_cancel(_async_ctx, cancel_func)

/*
 * Run few variadic tasks in parallel
 */
struct astate *async_vgather(size_t n, ...);

/*
 * Does the same, but takes array and number of array elements.
 * Arr must not be freed before this task is done or cancelled.
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

void async_run(struct astate *state);

/*
 * Internal functions, use with caution! (At least read the code)
 */
struct astate *async_new_task_(const async_runner *runner, void *args);

void async_args_destructor(struct astate *state);

void async_free_task_(struct astate *state);

void async_free_tasks_(size_t n, struct astate *states[]);

void *async_alloc_(struct astate *state, size_t size);

int async_free_(struct astate *state, void *mem);

int async_free_later_(struct astate *state, void *mem);

const char *async_strerror(async_error err);

#endif
