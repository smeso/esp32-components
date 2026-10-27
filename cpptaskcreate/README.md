# cpptaskcreate

Header-only C++ drop-in replacement for `xTaskCreate()`: `cppTaskCreate()`
accepts any C++ callable (function, lambda, bind expression, functor, ...)
instead of a plain `void (*)(void *)` and a `void *` argument. The callable is
moved onto the heap and invoked by a plain trampoline that runs as the
FreeRTOS task, so `std::function<void()>`-style state (including the captured
state of move-only lambdas, which travels through an owning smart pointer so
the closure itself stays copyable) travels with the task safely.

cppTaskCreate makes it possible to use exceptions in top-level task functions
without enabling CONFIG_FREERTOS_TASK_FUNCTION_WRAPPER.

The trampoline guarantees that control never escapes back into the
(nonexistent) caller's frame:

- the callable returning normally is logged and aborts,
- an uncaught exception is logged and aborts.

Either way the callable's context is freed before aborting, so nothing leaks
along those paths. Note that abort() only happens when the task body is
violating its contract; a task that wants to end must do it explicitly (as
always in FreeRTOS): calling `vTaskDelete(NULL)` from inside the callable
works on every target. On the Linux simulator the port ends such a task with
`pthread_exit()`, i.e. a forced unwind, which the trampoline forwards
(`catch (abi::__forced_unwind &) { throw; }`) instead of letting it be
swallowed by its catch-all clause (libstdc++ aborts the whole process with
`FATAL: exception not rethrown` when that happens).

The callable's heap context is released along every path. It is owned by a
`std::unique_ptr` inside the trampoline, which destroys it on the paths that
unwind the trampoline (the two abort paths and, on the Linux simulator, the
forced unwind above); on real hardware — where `vTaskDelete()` tears a task
down without unwinding its C++ stack — the context is additionally registered
as a FreeRTOS thread local storage pointer whose deletion callback frees it
whenever the task is deleted. That registration happens in `cppTaskCreate()`
(right after the task is created, before the handle is returned), so a task
that is deleted before it ever runs still releases its context; the abort path
unregisters it before releasing the context, so the kernel can never free it a
second time.

The callable's destructor therefore runs in kernel context (from
`prvDeleteTCB()`, on the stack of the task that deletes its owner, or of the
idle task when the owner deleted itself) with the scheduler lock held: it — and
the destructors of everything the callable captured — must be short and
non-blocking, and must not throw (the callback is `noexcept`, so a throwing
destructor terminates the process). No `ESP_LOG*` or FreeRTOS blocking call
from a destructor.

By-reference captures still dangle exactly as they would in plain C if the
referenced objects do not outlive the task, and a `this` captured from a
member function is just such a reference; capture by value (or through an
owned `shared_ptr`) whenever the referenced object's lifetime is not
guaranteed to span the task's.

The complete API is documented in
[`include/cpptaskcreate.hpp`](include/cpptaskcreate.hpp).

## Requirements

- ESP-IDF >= 6.0
- C++ exceptions enabled (`CONFIG_COMPILER_CXX_EXCEPTIONS`): the trampoline
  needs a working try/catch to contain the callable's exceptions. A missing
  option is a compile error.
- FreeRTOS thread local storage deletion callbacks enabled
  (`CONFIG_FREERTOS_TLSP_DELETION_CALLBACKS`, the default): on chip targets they
  release the callable's context whenever a task is deleted, including a task
  deleted before it ever runs. A missing option is a compile error on every
  target but the Linux simulator (which does not use the callbacks).
- At least two thread local storage pointers
  (`CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS`, default 1): the component
  registers its context at a non-zero index because index 0 is reserved by
  ESP-IDF for the pthreads API. The exact index is the
  `CPPTASKCREATE_TLS_INDEX` Kconfig option (default 1): raise it if another
  component already uses index 1.

## Example

`examples/counter_task` starts three counting tasks from a single lambda
factory, each counting at its own pace with move-only state captured by
value, and verifies the totals. Every task ends with `vTaskDelete(NULL)`, as a
task must. The example only needs FreeRTOS, so it does not depend on any other
component.

## License

Apache-2.0
