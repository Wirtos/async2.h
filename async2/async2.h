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
 * this is an async/await/event loop implementation for C based on Duff's device.
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
    #define ASYNC_ZUTIL_ON_DEBUG_(expr) (void)(expr)
#else
    #define ASYNC_ZUTIL_ON_DEBUG_(expr) (void)0
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

#define async_set_flag_(obj, flag)   (void) ((obj)->_flags |=  (flag))
#define async_unset_flag_(obj, flag) (void) ((obj)->_flags &= ~(flag))
#define async_get_flag_(obj, flag)          ((obj)->_flags &   (flag))

typedef enum {
    ASYNC_TASK_FLAG_SCHEDULED = 1u << 0,
    ASYNC_TASK_FLAG_CANCELLED = 1u << 1
} async_task_flags;

typedef enum {
    ASYNC_LOOP_FLAG_RUNNING = 1u << 0,
    ASYNC_LOOP_FLAG_CLOSED  = 1u << 1
} async_loop_flags;

typedef struct astate *p_astate;


/*
 * Core async type, every async function must follow this signature.
 */
typedef async (*AsyncCallback)(struct astate *);

typedef void (*AsyncDestructorCallback)(struct astate *);

typedef int (*AsyncCancelCallback)(struct astate *);

#ifdef _MSC_VER
    /* silent MSVC's warning on unnamed type definition in parentheses because it's valid C */
    #pragma warning(disable : 4116)
#endif

#define ASYNC_ZUTIL_SPACK_a_(T_a)       struct { struct astate st; T_a a; }

#define ASYNC_ZUTIL_SPACK_ab_(T_a, T_b) struct { struct astate st; T_a a; T_b b; }


/*
 * Initializers for Async_Runner::sizes
 */
#define ASYNC_RUNNER_PURE_INIT() \
    {                            \
        0,                       \
        0,                       \
        0,                       \
        0                        \
    }

#define ASYNC_RUNNER_STACK_INIT(T_stack)                              \
    {                                                                 \
        offsetof(ASYNC_ZUTIL_SPACK_a_(T_stack), a),                   \
        0,                                                            \
        0,                                                            \
        sizeof(ASYNC_ZUTIL_SPACK_a_(T_stack)) - sizeof(struct astate) \
    }

#define ASYNC_RUNNER_ARGS_INIT(T_args)                               \
    {                                                                \
        0,                                                           \
        sizeof(T_args),                                              \
        offsetof(ASYNC_ZUTIL_SPACK_a_(T_args), a),                   \
        sizeof(ASYNC_ZUTIL_SPACK_a_(T_args)) - sizeof(struct astate) \
    }

#define ASYNC_RUNNER_FULL_INIT(T_stack, T_args)                                \
    {                                                                          \
        offsetof(ASYNC_ZUTIL_SPACK_ab_(T_stack, T_args), a),                   \
        sizeof(T_args),                                                        \
        offsetof(ASYNC_ZUTIL_SPACK_ab_(T_stack, T_args), b),                   \
        sizeof(ASYNC_ZUTIL_SPACK_ab_(T_stack, T_args)) - sizeof(struct astate) \
    }

typedef struct {
    AsyncCallback coro;
    AsyncDestructorCallback destr;
    AsyncCancelCallback cancel;
    struct {
        size_t stack_offset; /* offset from the base of a struct to stack */
        size_t args_size;    /* size of args without padding */
        size_t args_offset;  /* offset from the base of a struct to args */
        size_t task_addsize; /* additional task size for optional stack and args, includes padding, might be 0
                              *  computed as [stack_pad + stack + args_pre_pad + args + args_post_pad] */
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
    struct astate *_child; /* child state used by await */
    struct astate *_prev, *_next;
    void *_allocs;
    #ifdef ASYNC_DEBUG
    const char *_debug_taskname_; /* must never be explicitly used */
    #endif
};

typedef struct async_event_loop {

    void (*init)(void);

    void (*stop)(void);

    void (*close)(void);

    struct astate *(*create_task)(struct astate *state);

    struct astate **(*create_tasks)(size_t n, struct astate * const states[]);

    void (*run_forever)(void);

    void (*run_until_complete)(struct astate *main_state);

    void *(*malloc)(size_t size);

    void *(*realloc)(void *ptr, size_t size);

    void *(*calloc)(size_t count, size_t size);

    void (*free)(void *ptr);

    /* Main tasks queue */
    struct astate *head, *tail;
    size_t n_tasks;

    async_loop_flags _flags;
} async_event_loop;

extern const struct async_event_loop * const * const async_loop_ptr;

#ifdef ASYNC_DIRECT_LOOP
    #undef ASYNC_DIRECT_LOOP
    #define ASYNC_DIRECT_LOOP (*async_loop_ptr)
#else
    #define ASYNC_DIRECT_LOOP (async_get_event_loop())
#endif

/* manual ownership control */
#define ASYNC_INCREF(coro)  (void) ((coro)->_refcnt++)

#define ASYNC_DECREF(coro)  (void) ((coro)->_refcnt--)

#define ASYNC_XINCREF(coro) (void) ((coro) ? ASYNC_INCREF(coro) : (void)0)

#define ASYNC_XDECREF(coro) (void) (((coro) && (coro)->_refcnt != 0) ? ASYNC_DECREF(coro) : (void)0)

/*
 * Mark the start of an async subroutine
 * Unknown continuation values now set async_errno to ASYNC_EINVAL_STATE.
 */

#define async_begin(st)                                                                                            \
    { /* beginning of the block in case user has some statements before async body */                              \
        struct astate *_async_ctx_ = st;                                                                           \
        ASYNC_ZUTIL_ON_DEBUG_(fprintf(stderr, "<ADEBUG> Entered '%s'\n", __func__));                               \
        switch (_async_ctx_->_async_k) {                                                                           \
            case ASYNC_INIT:                                                                                       \
                ASYNC_ZUTIL_ON_DEBUG_(                                                                             \
                    (_async_ctx_->_debug_taskname_ = __func__, fprintf(stderr, "<ADEBUG> Begin '%s'\n", __func__)) \
                );

/*
 * Mark the end of a async subroutine
 */
#define async_end                                                                                                  \
        async_exit;                                                                                                \
        /* fall through */                                                                                         \
        case ASYNC_DONE:                                                                                           \
            ASYNC_ZUTIL_ON_DEBUG_(                                                                                 \
                fprintf(                                                                                           \
                    stderr, "<ADEBUG> WARNING: task is already done, but its coro was called: %s(%d)\n",           \
                    __FILE__, __LINE__                                                                             \
                )                                                                                                  \
            );                                                                                                     \
            return ASYNC_DONE;                                                                                     \
        default:                                                                                                   \
            async_errno = ASYNC_EINVAL_STATE;                                                                      \
            ASYNC_ZUTIL_ON_DEBUG_(                                                                                 \
                fprintf(stderr, "<ADEBUG> WARNING: %s: %s(%d)\n", async_strerror(async_errno), __FILE__, __LINE__) \
            );                                                                                                     \
            return ASYNC_DONE;                                                                                     \
        } /* close async_begin's switch */                                                                         \
    } /* close async body block */                                                                                 \
    (void)0

/*
 * todo: do while bodies might be made optional in case compiler can't optimise empty loops.
 *  They exist only to prevent code like "if(cond) async_macro() else ..." from breaking with cryptic error.
 * */

/*
 * Wait while the condition succeeds (optional)
 *
 * Continuation state is now callee-saved like protothreads which avoids
 * duplicate writes from the caller-saved design.
 */
#define await_while(cond)                                                                          \
    do {                                                                                           \
        while (cond) {                                                                             \
            ASYNC_ZUTIL_ON_DEBUG_(                                                                 \
                fprintf(stderr, "<ADEBUG> Awaited in '%s' %s(%d)\n", __func__, __FILE__, __LINE__) \
            );                                                                                     \
            async_yield;                                                                           \
        }                                                                                          \
    } while (0)

/*
 * Wait until the condition succeeds
 */
#define await_until(cond) await_while(!(cond))

/*
 * Yield execution
 */
#define async_yield                                                                                      \
    do {                                                                                                 \
        ASYNC_ZUTIL_ON_DEBUG_(                                                                           \
            fprintf(stderr, "<ADEBUG> Yielded in '%s' %s(%d)\n", __func__, __FILE__, __LINE__)           \
        );                                                                                               \
        _async_ctx_ ->_async_k = __LINE__; return ASYNC_CONT; /* fall through */ case __LINE__:          \
        (void)0;                                                                                         \
    } while (0)

/*
 * Exit the current async subroutine
 */
#define async_exit                                                                               \
    return (                                                                                     \
        _async_ctx_ ->_async_k = ASYNC_DONE,                                                     \
        ASYNC_DECREF(_async_ctx_),                                                               \
        ASYNC_ZUTIL_ON_DEBUG_(                                                                   \
            fprintf(stderr, "<ADEBUG> Exited from '%s' %s(%d)\n", __func__, __FILE__,  __LINE__) \
        ),                                                                                       \
        ASYNC_DONE                                                                               \
    )

/*
 * Cancels running task and children recursively while state is their only owner
 */
#define async_cancel(task) async_cancel_(task)

/*
 * returns 1 if function was cancelled
 */
#define async_is_cancelled(task) ((task)->err == ASYNC_ECANCELLED)

/*
 * Check if async subroutine is done
 */
#define async_is_done(task) ((task)->_async_k == ASYNC_DONE)


/*
 * Create a new task
 */
#define async_new(arunner, args) async_new_task_((arunner), (args))

/*
 * Schedule task from new state object
 */

#define async_create_task(task) (ASYNC_DIRECT_LOOP->create_task(task))


/*
 * Create tasks from array of states
 */

#define async_create_tasks(n, tasks) ((ASYNC_DIRECT_LOOP)->create_tasks(n, tasks))

/*
 * Get async_error code for current execution state. Can be used to check for errors after await()
 */
#define async_errno (_async_ctx_->err)

/*
 * Create task and wait until it succeeds. Resets async_errno and sets it.
 */

#define await(task)                                                    \
    do {                                                               \
        if ((_async_ctx_->_child = async_create_task(task)) != NULL) { \
            ASYNC_INCREF(_async_ctx_->_child);                         \
            if(!async_is_done(_async_ctx_->_child)){                   \
                async_yield;                                           \
            }                                                          \
                                                                       \
            async_errno = _async_ctx_->_child->err;                    \
            ASYNC_DECREF(_async_ctx_->_child);                         \
            _async_ctx_->_child = NULL;                                \
        } else {                                                       \
            async_errno = ASYNC_ENOMEM;                                \
        }                                                              \
    } while (0)
/*
 * Allocate memory that'll be freed automatically after async function ends.
 * Allows to avoid setting a separate destructor
 */
#define async_alloc(size) async_alloc_(_async_ctx_, size)

#define async_free(ptr) async_free_(_async_ctx_, ptr)

#define async_free_later(ptr) async_free_later_(_async_ctx_, ptr)


/*
 * Run few variadic tasks in parallel
 */
struct astate *async_vgather(size_t n, ...);

/*
 * Does the same, but takes array and number of array elements.
 * Arr must not be freed before this task is done.
 */
struct astate *async_gather(size_t n, struct astate * const states[]);

/*
 * Block for `delay` seconds
 */
struct astate *async_sleep(double delay);

/*
 * Execute function in `timeout` seconds or cancel it if timeout was reached.
 */
struct astate *async_wait_for(struct astate *child, double timeout);

const struct async_event_loop *async_get_event_loop(void);

void async_set_event_loop(struct async_event_loop *);

void async_run(struct astate *amain);

/*
 * Internal functions, use with caution! (At least read the code)
 */
struct astate *async_new_task_(const async_runner *runner, void *args);

void async_args_destructor(struct astate *state);

void async_free_task_(struct astate *state);

void async_free_tasks_(size_t n, struct astate * const states[]);

int async_cancel_(struct astate *state);

void *async_alloc_(struct astate *state, size_t size);

void async_free_(struct astate *state, void *ptr);

int async_free_later_(struct astate *state, void *ptr);

const char *async_strerror(async_error err);

#endif
