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

#ifdef NDEBUG
    #define ON_DEBUG(statement) \
        (void) 0
#else
    #define ON_DEBUG(statement) \
        do { statement } while (0)
#endif

/*
 * event loop member functions declaration
 */
static struct astate *async_loop_create_task_(struct astate *state);

static struct astate **async_loop_create_tasks_(size_t n, struct astate * const states[]);

static void async_loop_init_(void);

static void async_loop_run_forever_(void);

static void async_loop_run_until_complete_(struct astate *amain);

static void async_loop_close_(void);

static void async_loop_stop_(void);

static int async_all_(size_t n, struct astate * const states[]);

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
    async_loop_create_task_,        \
    async_loop_create_tasks_,       \
    async_loop_run_forever_,        \
    async_loop_run_until_complete_, \
    async_loop_malloc_,             \
    async_loop_realloc_,            \
    async_loop_calloc_,             \
    async_loop_free_,               \
    NULL, NULL,                     \
    0,                              \
    0

/* Init default event loop, custom event loop should create own initializer instead. */
static struct async_event_loop async_default_event_loop_ = {
    STD_EVENT_LOOP_INIT
};

static const struct async_event_loop async_default_event_loop_copy_ = {
    STD_EVENT_LOOP_INIT
};

static struct async_event_loop *event_loop = &async_default_event_loop_;

const struct async_event_loop * const * const async_loop_ptr = (const struct async_event_loop * const * const) &event_loop;


static int async_all_(size_t n, struct astate * const states[]) { /* Returns false if at least one state is NULL */
    while (n--) {
        if (states[n] == NULL) { return 0; }
    }
    return 1;
}

/*
 * this union will have the largest basic data type offset needed for its any element to be aligned,
 * it's not guaranteed to work with extension types (like 128 bit integers), nor to be the same as alignof(max_align_t)
 */

typedef struct memblock_header {
    union {
/* some embedded compilers or the ones like C65 don't support floats */
#if !defined ASYNC_NO_FLOATS
        long double a;
#endif
#if defined __STDC_VERSION__ && __STDC_VERSION__ > 199901L
        long long int b; /* long in modern compilers should be the same size and alignment as long long, but if we have c99, include it just in case */
#endif
        long int c;
        size_t d;
        void *e;
        void (*f)(void);
    } Align;

    struct memblock_header *prev, *next;
    void *ref;
} memblock_header;


#define MEMBLOCK_FREED     0xF3EE
#define MEMBLOCK_ALLOC     0xBEEF
#define MEMBLOCK_FREELATER 0xBAEE

#define MEMBLOCK_HEADER_FREE(header)                                                                \
    do {                                                                                            \
        /* this assertion is unlikely to work because of how modern libs free memory */             \
        assert((ignored_"Double free of async_alloc memory", (header)->Align.c != MEMBLOCK_FREED)); \
        assert((ignored_"Given memory address wasn't allocated with async_alloc",                   \
                (header)->Align.c == MEMBLOCK_ALLOC || (header)->Align.c == MEMBLOCK_FREELATER));   \
        ON_DEBUG({                                                                                  \
            (header)->Align.c = MEMBLOCK_FREED;                                                     \
        });                                                                                         \
        free((header)->ref);                                                                        \
        event_loop->free(header);                                                                   \
    } while (0)

/* Free astate, its allocs and invalidate it completely */
#define ASTATE_FREE(state)                           \
    do {                                             \
        memblock_header *_header;                    \
        if ((state)->_runner->destr) {               \
            (state)->_runner->destr(state);          \
        }                                            \
        _header = (state)->_allocs;                  \
        while (_header) {                            \
            memblock_header *_hnext = _header->next; \
            MEMBLOCK_HEADER_FREE(_header);           \
            _header = _hnext;                        \
        }                                            \
        event_loop->free(state);                     \
    } while (0)

/* prepend is used because we don't want tasks to run in the same loop they were added */
static void async_loop_prepend_(struct astate *state){
    struct async_event_loop *loop = event_loop;
    if (loop->n_tasks == 0) {
        loop->head = loop->tail = state;
    } else {
        loop->head->_prev = state;
        state->_next = loop->head;
        loop->head = state;
    }
    loop->n_tasks++;
}


static void async_loop_remove_(struct astate *state){
    struct async_event_loop *loop = event_loop;
    assert((ignored_"Loop is already empty, state was already removed or never added", loop->n_tasks != 0));
    assert(state != NULL);

    if (state == loop->tail)
        loop->tail = loop->tail->_prev;
    else if (state->_next)
        state->_next->_prev = state->_prev;

    if (state == loop->head)
        loop->head = loop->head->_next;
    else if (state->_prev)
        state->_prev->_next = state->_next;

    state->_prev = state->_next = NULL;
    loop->n_tasks--;
}

static void async_loop_once_(void) {
    struct astate *state = event_loop->head;

    while (state) {
        struct astate *next = state->_next;

        if (state->_refcnt == 0) {
            async_loop_remove_(state);
            ASTATE_FREE(state);
        } else if (!async_is_done(state) && (!state->_child || async_is_done(state->_child))) {
            /* no awaited child or child is done, run parent */
            state->_runner->coro(state);
        }
        state = next;
    }
}

/* todo: run forever loop */
static void async_loop_run_forever_(void) {
    async_set_flag_(event_loop, ASYNC_LOOP_FLAG_RUNNING);

    async_unset_flag_(event_loop, ASYNC_LOOP_FLAG_RUNNING);
}

static void async_loop_run_until_complete_(struct astate *amain) {
    if (amain == NULL) { return; }

    async_set_flag_(event_loop, ASYNC_LOOP_FLAG_RUNNING);

    ASYNC_INCREF(amain);
    event_loop->create_task(amain);

    while (!async_is_done(amain)) {
        async_loop_once_();
    }

    ASYNC_DECREF(amain);

    async_unset_flag_(event_loop, ASYNC_LOOP_FLAG_RUNNING);
    if (amain->_refcnt == 0) {
        async_loop_remove_(amain);
        ASTATE_FREE(amain);
    }
}

static void async_loop_init_(void) {
    /* todo: loop constructor */
}

static void async_loop_close_(void) {
    /* todo: loop destructor */
}

static void async_loop_stop_(void){
    async_unset_flag_(event_loop, ASYNC_LOOP_FLAG_RUNNING);
}

#define async_set_scheduled(task) (async_set_flag_((task), ASYNC_TASK_FLAG_SCHEDULED))

#define async_is_scheduled(task) (async_get_flag_(task, ASYNC_TASK_FLAG_SCHEDULED))

static struct astate *async_loop_create_task_(struct astate *state) {
    if (state == NULL) { return NULL; }

    if (!async_is_scheduled(state)) {
        async_loop_prepend_(state);
        async_set_scheduled(state);
    }
    return state;
}


static struct astate **async_loop_create_tasks_(size_t n, struct astate * const states[]) {
    size_t i;
    if (!(states && async_all_(n, states))) {
        return NULL;
    }

    for (i = 0; i < n; i++) {
        if (!async_is_scheduled(states[i])) {
            async_loop_prepend_(states[i]);
            async_set_scheduled(states[i]);
        }
    }
    return (struct astate **)states;
}

struct astate *async_new_task_(const async_runner *runner, void *args) {
    struct astate *state = event_loop->calloc(1, sizeof(*state) + runner->sizes.task_addsize);
    if (state == NULL) { return NULL; }

#ifdef ASYNC_DEBUG
    state->stack = (runner->sizes.stack_offset)
                      ? ((char *) state) + runner->sizes.stack_offset
                      : NULL;
#else
    state->stack = ((char *) state) + runner->sizes.stack_offset;
#endif

    state->args = (runner->sizes.args_size)
                    ? (
                       assert((ignored_"Pointer to args must be non-NULL", args)),
                       memcpy(((char *) state) + runner->sizes.args_offset, args, runner->sizes.args_size)
                      )
                    : args;

    state->_runner = runner;
    state->_refcnt = 1; /* State has 1 reference set as function "owns" itself until exited or cancelled */
    /* state->_async_k = ASYNC_INIT; state is already ASYNC_INIT because calloc */

#if !defined ASYNC_NULL_ZERO_BITS
    /* pointer variables aren't guaranteed to be represented with all zero bits, assign them explicitly */
    state->_prev = state->_next = NULL;
    state->_child = NULL;
    state->_allocs = NULL;
#endif
    return state;
}

void async_free_task_(struct astate *state) {
    if (state != NULL) {
        ASTATE_FREE(state);
    }
}

void async_free_tasks_(size_t n, struct astate * const states[]) {
    while (n--) {
        if (states[n]) { ASTATE_FREE(states[n]); }
    }
}

struct gather_args {
    struct astate **states;
    size_t n;
};


static int gather_cancel(struct astate *st){
    struct gather_args *arg = st->args;
    size_t n = arg->n;

    /* remove ownership and cancel child functions left */
    if(n != (size_t)-1){
        while(n--){
            async_cancel(arg->states[n]);
            ASYNC_XDECREF(arg->states[n]);
        }
    }
    return 1;
}

static async gather_coro(struct astate *st) {
    struct gather_args *arg = st->args;
    async_begin(st);
    while (arg->n--) {
        await(arg->states[arg->n]);
        async_errno = ASYNC_OK;
        ASYNC_XDECREF(arg->states[arg->n]);
    }
    async_end;
}

struct astate *async_gather(size_t n, struct astate * const states[]) {
    static const async_runner gather_runner = {
        gather_coro, NULL, gather_cancel,
        ASYNC_RUNNER_ARGS_INIT(struct gather_args)};
    struct gather_args args;
    args.states = (struct astate **) states;
    args.n = n;
    while (n--) {
        async_create_task(states[n]);
        ASYNC_XINCREF(states[n]);
    }
    return async_create_task(async_new(&gather_runner, &args));
}


static void vgather_destr(struct astate *st) {
    struct gather_args *arg = st->args;
    event_loop->free(arg->states);
}

struct astate *async_vgather(size_t n, ...) {
    static const async_runner vgather_runner = {
        gather_coro, vgather_destr, gather_cancel,
        ASYNC_RUNNER_ARGS_INIT(struct gather_args)};
    struct gather_args args;
    va_list va_args;

    args.states = event_loop->malloc(sizeof(*args.states) * n);
    if (!args.states) return NULL;
    args.n = n;

    va_start(va_args, n);
    while (n--) {
        args.states[n] = async_create_task(va_arg(va_args, struct astate *));
        ASYNC_XINCREF(args.states[n]);
    }
    va_end(va_args);
    return async_create_task(async_new(&vgather_runner, &args));
}

#if 0
/* todo: any time-related operations require a separate heap queue in order to be efficient */
typedef struct {
    double sec;
    clock_t start;
} waiter_stack;

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



static void async_allocs_prepend_(struct astate *state, memblock_header *header){
    header->prev = NULL;
    header->next = state->_allocs;

    if(state->_allocs){
        memblock_header *head = state->_allocs;
        head->prev = header;
    }

    state->_allocs = header;
}

static void async_allocs_remove_(struct astate *state, memblock_header *header) {
    if(state->_allocs == header){
        state->_allocs = header->next;
    } else {
        header->prev->next = header->next;
    }

    if(header->next){
        header->next->prev = header->prev;
    }
}

void *async_alloc_(struct astate *state, size_t size) {
    memblock_header *header;
    assert(state != NULL);

    header = event_loop->malloc(sizeof(*header) + size);
    if (header == NULL) { return NULL; }

    header->ref = NULL;
    ON_DEBUG({
        header->Align.c = MEMBLOCK_ALLOC;
    });

    async_allocs_prepend_(state, header);
    return header + 1;
}

void async_free_(struct astate *state, void *ptr) {
    memblock_header *header = (memblock_header *) ptr - 1; /* pointer element object after the bound is still valid */
    if (ptr == NULL) { return; }

    assert((ignored_"List of allocs for this state is empty", state->_allocs != NULL));
    assert((ignored_"Double free of async_alloc memory", header->Align.c != MEMBLOCK_FREED));

    async_allocs_remove_(state, header);

    MEMBLOCK_HEADER_FREE(header);
}

int async_free_later_(struct astate *state, void *ptr) {
    memblock_header *header;
    if (ptr == NULL) { return 0; }

    header = event_loop->malloc(sizeof(*header));
    if (header == NULL) { return 0; }

    header->ref = ptr;
    ON_DEBUG({
        header->Align.c = MEMBLOCK_FREELATER;
    });

    async_allocs_prepend_(state, header);
    return 1;
}

const struct async_event_loop *async_get_event_loop(void) {
    return event_loop;
}

void async_set_event_loop(struct async_event_loop *loop) {
    assert(!event_loop || async_get_flag_(event_loop, ASYNC_LOOP_FLAG_RUNNING));
    if(loop != NULL){
        event_loop = loop;
    } else {
        memcpy(&async_default_event_loop_, &async_default_event_loop_copy_, sizeof(async_default_event_loop_));
        event_loop = &async_default_event_loop_;
    }
}

const char *async_strerror(async_error err) {
    switch (err) {
        case ASYNC_OK:
            return "OK";
        case ASYNC_ENOMEM:
            return "MEMORY ALLOCATION FAILED";
        case ASYNC_ECANCELLED:
            return "TASK WAS CANCELLED";
        case ASYNC_EINVAL_STATE:
            return "INVALID STATE WAS PASSED TO RUNNER'S COROUTINE";
        default:
            return "UNKNOWN ERROR";
    }
}

void async_args_destructor(struct astate *state) {
    free(state->args);
}

void async_run(struct astate *amain) {
    event_loop->init();
    event_loop->run_until_complete(amain);
    event_loop->close();
}

#define ASYNC_CANCEL_SINGLE(state)                                  \
    (                                                               \
     ASYNC_DECREF(state),  /* decref its ownership of self */       \
     ((state)->_runner->cancel && !(state)->_runner->cancel(state)) \
         ? (                                                        \
            /* refused to cancel, restore refcnt */                 \
            ASYNC_INCREF(state),                                    \
            0                                                       \
           )                                                        \
         : (                                                        \
            (state)->err = ASYNC_ECANCELLED,                        \
            (state)->_async_k = ASYNC_DONE,                         \
            async_set_flag_(state, ASYNC_TASK_FLAG_CANCELLED),      \
            1                                                       \
           )                                                        \
    )

int async_cancel_(struct astate *state) {
    /* these are basic requirements so you shouldn't test for these in custom cancel code */
    /* already cancelled */
    if(async_get_flag_(state, ASYNC_TASK_FLAG_CANCELLED)) return 1;
    /* can't cancel finished task */
    if(async_is_done(state)) return 0;

    if (ASYNC_CANCEL_SINGLE(state)) {
        while (state->_child) {
            /* decref parent's ownership of child */
            ASYNC_DECREF(state->_child);
            /* it's done, or isn't only referenced by self or refuses to cancel */
            if (async_is_done(state->_child) || state->_refcnt != 1 || !ASYNC_CANCEL_SINGLE(state->_child)) {
                break;
            }
            /* one of the child tasks down the chain refused to cancel so we are done here */
            state = state->_child;
        }
        return 1;
    } else {
        return 0;
    }
}
