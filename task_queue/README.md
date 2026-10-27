# task_queue

Generic C++ task queue for invocable work items: the posted items (e.g.
`std::function<void()>` closures) are posted to a FreeRTOS queue that is
consumed by a dedicated worker task, so the posted work never runs in the
caller's context (e.g. a timer or ISR context): the worker invokes each item
itself. `post()` is safe to call from both task and ISR context and returns
`false` when the queue is full (the item is dropped: posting never blocks);
the requirement is only that `T` is default-constructible, move-constructible
and move-assignable. Exceptions thrown by items are contained in the worker
(logged, not propagated) and the destructor lets the worker finish the item
in flight and delete its own task instead of killing it, so the object must
not be destroyed from one of its own items. `post()` is also safe to race with
the destructor: once destruction starts it stops enqueuing and returns
`false`. The complete API is documented in
[`include/task_queue.hpp`](include/task_queue.hpp). `drain()` blocks until
the worker has processed every item posted before the call, so it can be
used to synchronize with the queue from task context.

## Requirements

- ESP-IDF >= 6.0
- `freertos`

## Example

`examples/worker_pool` builds a pool of worker queues, posts data blocks to
them and verifies that the posted work runs on the dedicated worker tasks
(and not in the caller's context). The example only needs FreeRTOS, so it
does not depend on any other component.

## License

Apache-2.0
