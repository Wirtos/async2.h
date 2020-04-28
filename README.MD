# async2.h - asynchronous, stackfull subroutines

Taking inspiration from protothreads, async.h, coroutines.h and async/await as found in python 
this is an async/await/fawait/event loop implementation for C based on Duff's device.

# Features

1. It's 100% pure, portable C.
2. It uses 40 bytes of memory per state, but grants you greater nesting abilities and stack management.
3. It's not dependent on an OS.
4. It's a bit simpler to understand than other implementations as async state/stack management is fully handled by the lib.
5. You can't preserve local variables across function calls, but lib provides a way to store them persistently(see async_new function)

# API

Types|Description
-----|-------
__async (\*async_callback)(struct astate \*, void \*, void \*)__| pointer to function with signature: `async funcname(struct astate *state, void *args, void *locals)`
__ASYNC\_NOLOCALS__|Type to imply empty function stack(locals) when creating new coro with `async_new`.

Function|Description
--------|-----------
*async_loop_init()*|Init new event loop
*async_loop_destroy()*|Destroy inited event loop
*async_loop_run_forever()*|Block and run event loop until there's no uncompleted tasks
*async_loop_run_until_complete(main_coro)*|Block and run event loop until main_coro completes. Can be called any number of times preserving uncompleted tasks state
*async_begin(state)*|Mark the beginning of an async subroutine
*async_end*|Mark the end of an async subroutine
*async_yield*|Yield execution until it's invoked again
*await(cond)*|Block progress until `cond` is true
*await_while(cond)*|Block progress while `cond` is true
*async_new(func, args, locals)*|Returns a new coro from function(function must follow async_callback signature) with args and stack memory capable to hold type passed to locals: int, struct, array, custom type or ASYNC_NOLOCALS if you don't need stack memory at all
*async_gather(n_coros, struct astate\*\* array_of_coros)*|Gathers together few coros to run them in parallel into single coro and returns it
*async_vgather(n_coros, ...)*|Variadic version of async_gather, expects coros to be passed directly
*fawait(coro)*|Add task to the event loop and block progress until coro is done executing
*async_create_task(coro)*|Add task to the event loop without blocking current progress
*async_cancel(coro)*|UNSAFE. cancels current coro. Note, that if coro operates some custom memory inside it'll result in a memory leak.
*async_exit*|Terminate the current async subroutine
*async_done(state)*|Returns true if async subroutine has completed execution, otherwise false

# Practices 
- How can I return value/values from the function?
---
Pass pointer to value as args when creating new coroutine with async_new. Then assign to pointer inside function body:
```C
async f(state, int *args, void *locals){
    async_begin(state);
    *args = 42;
    ...
}
...
int a;
create_task(async_new(f, &a, ASYNC_NOLOCALS));
run();
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

async amain(struct astate *state, char *args, struct amain_stack *locals) {
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
    async_loop_init();
    async_create_task(async_new(amain, "task 1", struct amain_stack)); /* Allocate enough memory to hold locals of type struct amain_stack */
    async_create_task(async_new(amain, "task 2", struct amain_stack));
    async_loop_run_forever();
    async_loop_destroy();
}
```

# Caveats

1. Due to compile-time bug, MSVC requires changing:
    `Project Properties > Configuration Properties > C/C++ > General > Debug Information Format`
   From "Program Database for Edit And Continue" to "Program Database".
2. As with protothreads, you have to be very careful with switch
   statements and manually-created local variables(variables not stored in `locals`) within an async subroutine. Generally best to avoid them.
3. As with protothreads, you can't make blocking system calls and preserve
   the async semantics. These must be changed into non-blocking calls that
   test a condition.
