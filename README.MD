[![Build Status](https://travis-ci.com/Wirtos/async2.h.svg?branch=master)](https://travis-ci.com/Wirtos/async2.h)
# async2.h - asynchronous, stackful subroutines
Taking inspiration from protothreads, async.h, coroutines.h and async/await as found in python 
this is an async/await/fawait/event loop implementation for C based on Duff's device.

# Features

1. It's 100% pure, portable C.
2. It uses 96 bytes of memory per state, but grants you seamless nesting abilities, error handling and stack management.
3. It's not dependent on an OS.
4. It's a bit simpler to understand than other implementations as async state/stack management is fully handled by the lib.
5. You can't preserve local variables across function calls, but the library provides a way to store them persistently (see [practices](#practices))

# API

Types|Description
-----|-------
__AsyncCallback__| pointer to an async function with signature: `async funcname(struct astate *state)`
__AsyncCancelCallback__| pointer to a cancel function with signature: `void funcname(struct astate *state)`
__ASYNC_NONE__|Type to imply empty function stack(locals) when creating new coro with `async_new`, typedef for `char`
__async_error__|Enum type with async errors: ASYNC_OK, ASYNC_ENOMEM, ASYNC_ECANCELED, ASYNC_EINVAL_STATE

Return type|Function/Macro|Description
----|-----------|-------------
struct async_event_loop *|*async_get_event_loop(void)*|Get current event loop
void|*async_set_event_loop(struct async_event_loop \*loop)*|Set custom event loop
void|*loop->init(void)*|Init new event loop
void|*loop->destroy(void)*|Destroy inited event loop, cancel and destroy all the tasks inside
void|*loop->run_forever(void)*|Block and run event loop until there's no uncompleted tasks
void|*loop->loop_run_until_complete(s_astate main_coro)*|Block and run event loop until main_coro completes. Can be called any number of times preserving uncompleted tasks state, frees main_coro if it has no ownership acquired after exiting
MACRO_BLOCK|*async_begin(state)*|Mark the beginning of an async subroutine
MACRO_BLOCK|*async_end*|Mark the end of an async subroutine
s_astate|*async_create_task(s_astate coro)*|Add task to the event loop without blocking current progress, returns NULL and frees coro on failure
s_astate \*|async_create_tasks(size_t n, s_astate \*coros)*|Add n tasks from array of states to the event loop without blocking current progress, does nothing and returns NULL if one of the coros is NULL or if there's not enough memory to add them
MACRO_BLOCK|*fawait(s_astate coro){ }*|Add task to the event loop and block progress until `coro` is done executing. Sets async_errno: ASYNC_ECANCELED if `coro` task was cancelled, ASYNC_ENOMEM and frees `coro` if there's not enough memory to create task, any custom error code, ASYNC_OK otherwise. Code inside curly braces only executes if `coro` sets async_errno, on async_errno==ASYNC_OK braces are simply ignored.
MACRO_BLOCK|*async_yield*|Yield execution until it's invoked again
MACRO_BLOCK|*await(cond)*|Block progress until `cond` is true
MACRO_BLOCK|*await_while(cond)*|Block progress while `cond` is true
s_astate|*async_new(AsyncCallback func, void \*args, T_locals)*|Returns a new coro from function (function must follow AsyncCallback signature) with args and stack memory capable to hold type passed to T_locals: int, struct, array, custom type or ASYNC_NONE if you don't need stack memory at all
s_astate|*async_gather(size_t n, s_astate \*array_of_coros)*|Gathers together few coros to run them in parallel into single coro and returns it. If gather() is cancelled, all submitted coros (that have not completed yet) are also cancelled. 
s_astate|*async_vgather(size_t n, ...)*|Variadic version of async_gather, expects coros to be passed directly (no need to cleanup them on failure)
s_astate|*async_sleep(double delay)*|Block execution for `delay` seconds, precision differs from platform to platform but you can easily provide own implementation with non-portable timers when needed
s_astate|*async_wait_for(s_astate coro, double timeout)*| Wait for the coro to complete with a timeout. Cancel it otherwise and set async_erro to ASYNC_ECANCELED.
void \*|*async_alloc(size_t size)* | Allocate memory automatically managed by the event loop, no need to free by yourself
void|*async_free(void \*ptr)* | Free `ptr` allocated with async_alloc without waiting for event loop to do it for you. Can be useful in long running tasks, but malloc with cancel function is preferable and faster anyway.
int|*int async_free_later_(struct astate \*state, void \*mem)*|returns true if memory block was successfully queued and will be freed later by the event loop
MACRO_BLOCK|*async_cancel(s_astate coro)*| Cancels current coro. Note, that if coro operates some custom memory not allocated with async_alloc it'll result in a memory leak.
int|*async_cancelled(s_astate coro)*|Returns true if coro was cancelled, false otherwise
MACRO_BLOCK|*async_on_cancel(AsyncCancelCallback cancel_func)*|Set cancel function to be called on cancellation for current async function. cancel_func must follow AsyncCancelCallback type signature
MACRO_BLOCK|*async_set_on_cancel(s_astate coro, AsyncCancelCallback cancel_func)*|Same as function above, but takes coro instead of using current function
MACRO_BLOCK|*async_exit*|Terminate the current async subroutine
int |*async_done(state)*|Returns true if async subroutine has completed execution, otherwise false
async_error |*async_errno*|Macro that expands to the value of the type `async_err` of the current async function, can be assigned too
const char \*|*async_strerror(async_err err)*|Returns string representation of async_errno value
void|*async_free_coro_(s_astate coro)*|free coro's memory, should be never used manually until dealing with states manually or when creating custom event loop, ignores NULL
void|*async_free_coros_(size_t n, s_astate \*coros)*|free n coros in array ignoring NULL pointers
## Ownership of references system
### (handled automatically by fawait/wait_for/gather coros, manual use only)
###### Some future api methods might use su_state as input coro type, explicitly indicating that it steals current ownership, in such case user mustn't access passed s_astate object or should INCREF ownership manually once more before transferring ownership to the method.

Function|Description
--------|-----------
*ASYNC_INCREF(coro)*|Mark coro as "currently in use", so it won't be deleted automatically. Must be followed by ASYNC_DECREF somewhere else in the async function/cancel function". 
*ASYNC_DECREF(coro)*|Mark coro as "isn't used anymore". Note, that it won't be deleted immediately if other functions used INCREF on this coro too
*ASYNC_PREPARE_NOARGS(async_callback, state, locals_t, cancel_f, err_label)*|Prepare adapter function without allocating memory for args, in case of faulty allocation goto err_label;
*ASYNC_PREPARE(async_callback, state, args_size, locals_t, cancel_f, err_label)*|Prepare adapter function
# Practices 
- How can I return value/values from the function?

Pass pointer to value as args when creating new coroutine with async_new. Then assign to pointer inside function body:
```C
async f(state){
    async_begin(state);
    int *res = state->args; /* args here is a pointer to int a (can be a pointer to any c object) */
    *res = 42;
    ...
}
...
int a;
fawait(async_new(f, &a, ASYNC_NONE)){ 
    printf("error %s happened\n", async_strerror(async_errno));
}
```

We can replace int * with, say, struct * if we want multiple return values of different types. Any type pointer is fine.

- What should I do if I need local variables?
---
Just create a struct with all needed variables and then pass struct as a type when creating new coro. See examples/example.c and short example below.


# Examples
## A simple example to run two coroutines in parallel
```C
#include <stdio.h>
#include "async2.h"

struct amain_stack {
    int i;
};

async amain(s_astate state) {
    struct amain_stack *locals = state->locals;
    char *args = state->args;
    async_begin(state);
            /* Note, that locals->i assignment to 0 is happening inside async_begin block. 
             * Don't assign them outside! */
            for (locals->i = 0; locals->i < 3; locals->i++) {
                printf("task %s: iteration №%d\n", args, locals->i + 1);
                /* Let other tasks run with yield. Similar to python's await asyncio.sleep(0) usage. */
                async_yield;
            }
    async_end;
}

int main(void) {
    struct async_event_loop *loop = async_get_event_loop();
    loop->init();
    async_create_task(async_new(amain, "task 1", struct amain_stack)); /* Allocate enough memory to hold locals of type struct amain_stack */
    async_create_task(async_new(amain, "task 2", struct amain_stack));
    loop->run_forever();
    loop->destroy();
}
```
## Code inside braces only executes when coroutine returns with non-zero async_errno code
```c
fawait(async_func(10)){
    if(async_errno == ASYNC_ENOMEM){
        /* Handle memory error... */
    }
} else {
    /* Coroutine finished without errors */
}
```
## Manual task ownership and error check
```c
locals->task = async_create_task(coro());
ASYNC_XINCREF(locals->task); /* Acquire ownership for task so it won't be immediately deleted after completion */
/* Do stuff... */
if(locals->task){
    await(async_done(locals->task));
    if(locals->task->err != ASYNC_OK){
        /* Handle task error manually */
    }
}

ASYNC_XDECREF(locals->task);
```
## Don't create libraries with async_new usage!
- Always wrap coroutines in such way as asyncio_sleep or asyncio_wait_for so their use will be very simple(no need to use async_new on them as they prepare), such way you can provide custom async functions that can take any arguments, but return type still will be limited to s_astate. ASYNC_PREPARE and ASYNC_PREPARE_NOARGS are pretty helpful for such function wrapping.
- Use underlying dev methods as async_new_coro_ and async_alloc_ in order to overcome userland limitations in creating async functions
- Always wrap all the used tasks with ownership, otherwise you risk to get invalid pointer access. Remember: one INCREF - one DECREF
- Provide cancel functions for your friendly methods, so even cancelled function won't break ownership and coro will be properly deleted.
- Use async_alloc(_) to manage dynamic memory

# Caveats

1. As with protothreads, you have to be very careful with switch
   statements and manually-created local variables(variables not stored in `locals`) within an async subroutine. Generally best to avoid them.
2. As with protothreads, you can't make blocking system calls and preserve
   the async semantics. These must be changed into non-blocking calls that
   test a condition.
3. You can't use setjump and longjump functions amid await because async2 alters usual function flow, so given code is fine:
```c
setjmp(var);
... 
longjump(var);
...
await(global == 42);
```
but this one will be an undefined behavior:
```c
setjmp(var);
... 
await(global == 42);
...
longjump(var);
```
That means no way you can use C exceptions libraries like exceptions4c to wrap fawait or await or yield calls, use async_errno and fawait braces instead.

