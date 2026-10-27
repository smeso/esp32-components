/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include <esp_log.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "cpptaskcreate.hpp"

/**
 * Generic task queue for invocable work items.
 *
 * The posted items (e.g. std::function<void()> closures) are posted to a
 * FreeRTOS queue that is consumed by a dedicated worker task, so the
 * posted work never runs in the caller's context (e.g. a timer or ISR
 * context): the worker invokes each item itself.
 *
 * post() is safe to call from both task and ISR context. The item is
 * move-assigned into one of a pool of default-constructed items and only
 * the item pointer travels through the FreeRTOS queue, so posting never
 * blocks. The pool holds queue_len + 1 items, so the memory cost of a
 * queue is (queue_len + 1) * sizeof(T). Posting is not allocation-free:
 * the caller materialises the by-value argument and the move assignment
 * into the pool slot may allocate (std::function does whenever its closure
 * does not fit the small-object buffer), so post() allocates exactly as
 * much as the caller's T does. When posting from an ISR, T's move
 * assignment runs in ISR context: it must be safe to call from there and
 * must not throw (see "Exceptions" below).
 *
 * Requirements on T: default-constructible, move-constructible (from an
 * rvalue reference), move-assignable (from an rvalue reference) and
 * invocable as an lvalue (callable with no arguments, e.g.
 * std::function<void()>). Copyability and trivial copyability are not
 * required. If T is contextually convertible to bool, items that compare
 * equal to false are treated as empty and skipped (std::function is such a
 * type: this is what prevents std::bad_function_call); a T without a bool
 * conversion is always invoked.
 *
 * Exceptions: an item that throws while it runs is caught by the worker
 * and logged; it never propagates out of the worker task. An empty
 * (default-constructed) item (e.g. an empty std::function) is skipped
 * instead of being invoked, so it can never cause std::bad_function_call.
 * Items should still avoid throwing, because nothing distinguishes a
 * handled exception from a successful run. post() itself may also
 * propagate an exception when T's move assignment throws: this is only
 * allowed in task context (an exception thrown out of an ISR is undefined
 * behaviour, so in ISR context post() contains the exception and drops the
 * item, returning false).
 *
 * drain() blocks until the worker has processed every item posted before
 * the call: it is implemented with a semaphore-giving sentinel item posted
 * through post() itself, so it inherits post()'s context restrictions
 * (task context only: it blocks) and must never be called from an item
 * executed by this queue's own worker (the sentinel would never run). It
 * also requires T to be constructible from the internal callback closure
 * (true for std::function<void()> and other type-erased invocables, false
 * for most custom functors). It throws when the sentinel cannot be
 * enqueued (full queue) or when destruction starts after the enqueue and
 * the destructor drops the sentinel before the worker runs it.
 *
 * Lifetime: the destructor performs a cooperative shutdown. It first stops
 * accepting new items (post() returns false from then on) and waits for the
 * post() calls that are already in flight to return, then drops the items
 * that are still queued without running them, then waits for the worker to
 * finish the item it may already be executing and acknowledge the shutdown
 * (the worker then deletes its own task), so no worker code can touch the
 * object after the destructor finishes. Destruction may therefore block for
 * as long as the in-flight item takes, it must not be called from an item
 * that is (or could be) executed by this queue's own worker (the
 * acknowledgment would never come and the destructor would deadlock), and
 * the scheduler must be running and not suspended (the shutdown hands
 * control to the worker and blocks on it, which asserts if called from a
 * scheduler-suspended section). Destroying a queue before the scheduler has
 * been started is supported: the worker task, which never ran, is then
 * deleted directly. A producer racing the destructor is safe: post() stops
 * enqueuing as soon as destruction starts, but a producer that keeps
 * calling post() without checking the return value keeps the destructor
 * waiting until it stops, so producers should stop posting (or at least
 * honor the false return) when the queue is being destroyed; ISR producers
 * must also be deregistered before the object is destroyed.
 */
template<typename T>
    requires std::invocable<T &> && std::is_default_constructible_v<T>
class task_queue
{
    static_assert(std::is_move_constructible_v<T>,
                  "T must be move-constructible (constructible from an rvalue reference)");
    static_assert(std::is_move_assignable_v<T>,
                  "T must be move-assignable (assignable from an rvalue reference)");

    static constexpr const char *TAG = "task_queue";

  public:
    class exception final : public std::exception
    {
      public:
        explicit exception(const char *msg) noexcept
        {
            std::snprintf(msg_buf_, sizeof(msg_buf_), "%s: %s", TAG, msg);
        }
        ~exception() override = default;
        const char *what() const noexcept override   // GCOVR_EXCL_BR_LINE
        {
            // GCOVR_EXCL_LINE: what() is inlined into its callers at -Os, so
            // this outlined definition is never recorded as executed.
            return msg_buf_;   // GCOVR_EXCL_LINE
        }

      private:
        char msg_buf_[128]{};
    };

    explicit task_queue(const char *name = "task_queue",
                        UBaseType_t queue_len = 16,
                        uint32_t stack_bytes = 4096,
                        UBaseType_t priority = 5)

            : pool_len_(checked_pool_len(queue_len))
            // GCOVR_EXCL_START: allocator-throw edges of the member
            // initializers (bad_alloc is not handled explicitly)
            , items_(std::make_unique<T[]>(pool_len_))
            , occupied_(std::make_unique<std::atomic<bool>[]>(pool_len_))
            , queue_(xQueueCreate(queue_len, sizeof(T *)))
            , done_(xQueueCreate(1, sizeof(T *)))
    {                                                  // GCOVR_EXCL_STOP
        if (queue_ == nullptr || done_ == nullptr) {   // GCOVR_EXCL_BR_LINE
            if (queue_ != nullptr) {                   // GCOVR_EXCL_LINE
                vQueueDelete(queue_);                  // GCOVR_EXCL_LINE
                queue_ = nullptr;                      // GCOVR_EXCL_LINE
            }   // GCOVR_EXCL_LINE
            if (done_ != nullptr) {    // GCOVR_EXCL_LINE
                vQueueDelete(done_);   // GCOVR_EXCL_LINE
                done_ = nullptr;       // GCOVR_EXCL_LINE
            }   // GCOVR_EXCL_LINE
            throw exception("xQueueCreate failed");   // GCOVR_EXCL_LINE
        }

        // GCOVR_EXCL_START: the never-taken branches of the cppTaskCreate
        // check below (its internal allocation can also throw)
        BaseType_t created = pdFAIL;
        try {
            created = cppTaskCreate(
                    name, stack_bytes, priority, &worker_handle_, [this] { worker(this); });
        } catch (...) {
            /* cppTaskCreate() allocates the callable's context and may throw
             * (std::bad_alloc): the raw queue handles have no owner, so they
             * would leak once the exception leaves the constructor. */
            vQueueDelete(queue_);
            vQueueDelete(done_);
            queue_ = nullptr;
            done_ = nullptr;
            throw;
        }
        if (created != pdPASS) {
            vQueueDelete(queue_);
            vQueueDelete(done_);
            queue_ = nullptr;
            done_ = nullptr;
            throw exception("cppTaskCreate failed");
        }
        // GCOVR_EXCL_STOP
    }   // GCOVR_EXCL_BR_LINE: constructor EH cleanup edge

    /** Cooperative shutdown: stop accepting posts, drop queued items,
     * wait for the worker. Waits for post() calls already in flight. */
    ~task_queue() noexcept
    {
        stop_worker();
        if (queue_ != nullptr) {   // GCOVR_EXCL_BR_LINE
            vQueueDelete(queue_);
            queue_ = nullptr;
        }
        if (done_ != nullptr) {   // GCOVR_EXCL_BR_LINE
            vQueueDelete(done_);
            done_ = nullptr;
        }
    }   // GCOVR_EXCL_BR_LINE: destructor EH cleanup edge

    task_queue(const task_queue &) = delete;
    task_queue &operator=(const task_queue &) = delete;
    task_queue(task_queue &&) = delete;
    task_queue &operator=(task_queue &&) = delete;

    /**
     * Enqueue an item. Safe to call from ISR or task context, and safe to
     * race with the destructor: once destruction starts, post() stops
     * enqueuing and returns false. Also returns false when the item cannot
     * be enqueued (the queue is full): the item is then dropped, keeping
     * the caller unblocked (non-blocking 0-timeout trade-off). The drop is
     * also logged via esp_rom_printf, which is safe from ISRs. In task
     * context post() may propagate an exception from T's move assignment;
     * in ISR context that exception is contained and the item is dropped.
     */
    bool post(T item)
    {
        const BaseType_t in_isr = xPortInIsrContext();
        /* Fast path: once destruction started, stop immediately. */
        if (stopping_.load()) {   // GCOVR_EXCL_BR_LINE
            return false;
        }
        /* Register as in flight: the destructor sets stopping_ and then
         * waits for posting_ to drain before it touches the queues, so
         * the whole body below runs either entirely before the
         * destructor starts draining or entirely after post() has been
         * refused. Both sides use sequentially consistent ordering:
         * either post() observes the destructor's store and refuses, or
         * the destructor's counter load is guaranteed to observe this
         * increment, so it waits until this post() is done with the
         * queues. */
        posting_.fetch_add(1);   // GCOVR_EXCL_BR_LINE: atomic RMW edge

        // GCOVR_EXCL_START: the re-check losing the race to the destructor
        // is timing-dependent and cannot be covered deterministically.
        if (stopping_.load()) {
            leave_posting();
            return false;
        }
        // GCOVR_EXCL_STOP

        /* The slot is claimed inside the try, so it can be released on the
         * unwinding path: pool_len_ is the "no slot claimed" sentinel. */
        size_t i = pool_len_;
        try {
            i = acquire_slot();
            if (i == pool_len_) {   // GCOVR_EXCL_BR_LINE: drop path is tested
                /* esp_rom_printf is ISR-safe, unlike the regular log library
                 * (which can take locks and is not callable from ISRs). The
                 * format is a full log-style line because esp_rom_printf has
                 * no level or tag handling of its own. */
                // GCOVR_EXCL_START: esp_rom_printf's exception edge is
                // never taken (it cannot throw) but the compiler still
                // emits it, and the call spans more than one source line,
                // so the single-line exclusion markers cannot cover it.
                esp_rom_printf("E (auto) task_queue: task queue full, item dropped\n");
                // GCOVR_EXCL_STOP
                leave_posting();
                return false;
            }
            T &slot = items_[i];   // GCOVR_EXCL_BR_LINE
            /* GCOVR_EXCL_START: the item's move-assign edges are covered
             * where they are exercised (test "post rolls the slot back
             * when the item's move assignment throws"); the gcovr block
             * markers must be outside the try body's range report. */
            slot = std::move(item);
            /* GCOVR_EXCL_STOP */
            T *p = &slot;
            BaseType_t ok;
            if (in_isr) {   // GCOVR_EXCL_BR_LINE
                BaseType_t hp = pdFALSE;
                ok = xQueueSendFromISR(queue_, &p, &hp);   // GCOVR_EXCL_BR_LINE
                portYIELD_FROM_ISR(hp);                    // GCOVR_EXCL_BR_LINE
            } else {
                ok = xQueueSend(queue_, &p, 0);   // GCOVR_EXCL_BR_LINE
            }
            if (ok != pdTRUE) {   // GCOVR_EXCL_BR_LINE
                /* Reachable: the queue can be full while the pool still has
                 * a free slot. The pool holds queue_len + 1 items and the
                 * extra one covers the item that is being dispatched, not
                 * an item that is still queued: with queue_len queued items
                 * and nothing being dispatched the queue is full but one
                 * slot is free, so the send above can fail. Drop the item
                 * and report the full queue like the pool-exhaustion path
                 * above. */
                // GCOVR_EXCL_START: esp_rom_printf's exception edge is never
                // taken (it cannot throw) but the compiler still emits it.
                esp_rom_printf("E (auto) task_queue: task queue full, item dropped\n");
                // GCOVR_EXCL_STOP
                release_slot(i);
                leave_posting();
                return false;
            }
            leave_posting();
            return true;
        } catch (...) {
            /* The item's move assignment threw: release the slot that was
             * claimed and unwind the in-flight accounting, or the
             * destructor would wait forever for a poster that is already
             * gone and the pool would silently lose a slot per throw. */
            if (i !=
                pool_len_) {   // GCOVR_EXCL_BR_LINE: a throwing move implies a claimed slot
                release_slot(i);
            }
            leave_posting();
            if (in_isr) {   // GCOVR_EXCL_BR_LINE: only a throwing ISR post reaches this
                /* An exception must never unwind through an interrupt
                 * frame: contain it and report the drop through the false
                 * return value (T's move assignment is documented to be
                 * non-throwing when posting from an ISR). */
                return false;   // GCOVR_EXCL_LINE
            }
            throw;
        }
    }

    /**
     * Block until every item posted before this call has been processed by
     * the worker. It posts a callback that gives a semaphore created here
     * and then waits for it, so the call only returns after the worker has
     * run everything that was already queued (items posted concurrently by
     * other tasks may or may not be included). Not callable from ISR
     * context (it blocks) and not from an item executed by this queue's
     * own worker (it would deadlock). Throws when the sentinel cannot be
     * enqueued or when destruction races the call and drops the sentinel.
     */
    void drain()
    {
        SemaphoreHandle_t done = xSemaphoreCreateBinary();
        if (done == nullptr) {                                  // GCOVR_EXCL_BR_LINE
            throw exception("xSemaphoreCreateBinary failed");   // GCOVR_EXCL_LINE
        }
        /* Own the semaphore so it is also released on the throwing paths
         * (post() propagating T's exception, the failures below). */
        sem_owner guard{ done };
        const bool posted = post([done] { xSemaphoreGive(done); });   // GCOVR_EXCL_BR_LINE
        if (!posted) {
            /* The queue is being destroyed or is full: the callback never
             * runs, so waiting for it would block forever. */
            throw exception("drain failed to post the sentinel");
        }
        /* Bounded wait: post() returning true does not guarantee that the
         * worker will ever run the sentinel, because the destructor may
         * start right after the enqueue and drop it. Poll the semaphore
         * and re-check stopping_ so a racing shutdown cannot hang drain()
         * forever. */
        while (xSemaphoreTake(done, pdMS_TO_TICKS(100)) != pdTRUE) {   // GCOVR_EXCL_BR_LINE
            // GCOVR_EXCL_START: only reached when the 100 ms timeout expires,
            // i.e. while the destructor is dropping the sentinel.
            if (!stopping_.load()) {
                continue;
            }
            if (xSemaphoreTake(done, 0) == pdTRUE) {
                /* The worker did run the sentinel before the shutdown. */
                return;
            }
            /* Destruction is in progress and has probably dropped the
             * sentinel. Deliberately do not delete the semaphore: the
             * worker may have received the sentinel already and be about to
             * give it, so deleting it first would be a use-after-free. The
             * leak is the safe outcome of this misuse. */
            guard.release();
            throw exception("drain interrupted by destruction");
            // GCOVR_EXCL_STOP
        }
    }

  private:
    struct sem_deleter
    {
        void operator()(SemaphoreHandle_t sem) const noexcept
        {
            vSemaphoreDelete(sem);
        }
    };
    using sem_owner = std::unique_ptr<std::remove_pointer_t<SemaphoreHandle_t>, sem_deleter>;

    /** Validate the queue length (a zero-length queue is rejected by
     * xQueueCreate(), so report it up front) and return the matching pool
     * length. */
    static size_t checked_pool_len(UBaseType_t queue_len)
    {
        if (queue_len == 0) {
            throw exception("queue_len must be greater than zero");
        }
        return static_cast<size_t>(queue_len) + 1;
    }

    /** Dedicated task: consumes the queue and runs the posted items. */
    static void worker(void *arg)
    {
        auto *self = static_cast<task_queue *>(arg);
        T *p = nullptr;
        while (xQueueReceive(self->queue_, &p, portMAX_DELAY) ==   // GCOVR_EXCL_BR_LINE
               pdTRUE) {
            if (p == nullptr) {
                /* nullptr is the quit sentinel sent by the destructor. */
                break;
            }
            self->dispatch(p);
            self->recycle_slot(self->slot_of(p));
        }
        /* Acknowledge the quit sentinel only now: the destructor waits
         * for this, so it cannot free the object while an item is still
         * being dispatched or cleaned up above. Then delete this task:
         * nothing touches the task_queue object after the acknowledgment,
         * so the destructor is free to delete the queues and the item pool
         * while this task is being torn down. On the Linux simulator the
         * port ends the task from inside vTaskDelete() with pthread_exit(),
         * a forced unwind that cppTaskCreate()'s trampoline forwards so the
         * thread terminates cleanly. */
        xQueueSend(self->done_, &p, 0);
        vTaskDelete(nullptr);
    }   // GCOVR_EXCL_LINE: vTaskDelete() does not return to the task

    /**
     * Stop the worker: drop the items that are still queued without
     * running them, send a nullptr sentinel behind them and wait for the
     * worker to acknowledge it. The acknowledgment is sent after the
     * worker's receive loop ends and just before the worker deletes its
     * own task, so the destructor waits for it instead of killing it
     * mid-item.
     */
    void stop_worker()
    {
        /* Stop accepting new posts: from here on post() returns false
         * without touching the queues (sequentially consistent, paired
         * with the check/increment/re-check sequence in post()). */
        stopping_.store(true);

        // GCOVR_EXCL_START: the scheduler is always running in the test
        // runner, so this path cannot be covered there. The posting_ drain
        // is done here too: the accounting is independent of the scheduler,
        // so a producer (an ISR, or another host thread on the simulator) is
        // joined even before the scheduler starts.
        if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
            /* Task delays and yields are unavailable before the scheduler
             * starts, so spin: the producers above make progress without
             * this task yielding. The worker never ran, so there is nothing
             * queued and no acknowledgment to wait for. Delete it directly
             * instead of leaving it behind to run (if the scheduler is
             * started afterwards) against the queues and the item pool
             * freed by the destructor. The callable's heap context
             * allocated by cppTaskCreate() cannot be reclaimed here, but
             * the task, its stack and the use-after-free hazard are gone. */
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 14
            /* GCC's static analyzer does not model the producer (ISR or
             * host thread) that clears posting_, so it reports this
             * intentional busy-wait as an infinite loop (CWE-835). */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-infinite-loop"
#endif
            while (posting_.load() != 0) {
                /* spin */
            }
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 14
#pragma GCC diagnostic pop
#endif
            if (worker_handle_ != nullptr) {
                vTaskDelete(worker_handle_);
                worker_handle_ = nullptr;
            }
            return;
        }
        // GCOVR_EXCL_STOP

        /* Wait for the post() calls that are already in flight: once
         * posting_ drains, no producer can race the queue drain below. */
        while (posting_.load() != 0) {
            vTaskDelay(1);
        }
        /* Drop the still-queued items without running them. The quit
         * sentinel does not travel through the item pool, so the slot
         * release here only gives the dropped items' resources back; it
         * is the receive loop above that makes room in the queue for the
         * sentinel send below. */
        T *p = nullptr;
        while (xQueueReceive(queue_, &p, 0) == pdTRUE) {
            release_slot(slot_of(p));
        }
        T *quit = nullptr;
        xQueueSend(queue_, &quit, portMAX_DELAY);
        T *done = nullptr;
        xQueueReceive(done_, &done, portMAX_DELAY);
    }

    /** Run one queued item, keeping all exceptions inside the worker. */
    void dispatch(T *item)
    {
        if constexpr (requires(T &t) { static_cast<bool>(t); }) {
            if (!static_cast<bool>(*item)) {
                ESP_LOGE(TAG, "empty invocable item skipped");
                return;
            }
        }

        // GCOVR_EXCL_START: EH cleanup edges of the catch-all guards
        try {
            (*item)();
        } catch (const std::exception &e) {
            ESP_LOGE(TAG, "item threw: %s", e.what());
        } catch (...) {
            ESP_LOGE(TAG, "item threw an unknown exception");
        }
        // GCOVR_EXCL_STOP
    }

    /**
     * Scan the item pool for a free slot: O(n) worst case, but
     * lock-free, allocation-free and ISR-safe. Returns pool_len_ when
     * the pool is exhausted (queue full).
     */
    size_t acquire_slot()
    {
        const size_t start = rr_.fetch_add(1, std::memory_order_relaxed) % pool_len_;
        for (size_t k = 0; k < pool_len_; ++k) {
            size_t i = start + k;
            if (i >= pool_len_)
                i -= pool_len_;
            bool expected = false;
            if (occupied_[i].compare_exchange_strong(
                        expected, true, std::memory_order_acq_rel)) {
                return i;
            }
        }
        return pool_len_;
    }

    /** Leave the in-flight-post accounting after a post() attempt. */
    void leave_posting()
    {
        posting_.fetch_sub(1);   // GCOVR_EXCL_BR_LINE: atomic RMW edge
    }

    /** Mark the slot as free again, leaving the item in it untouched. The
     * range check turns a stray sentinel pointer into a no-op instead of an
     * out-of-bounds store. */
    void release_slot(size_t i)
    {
        if (i < pool_len_) {   // GCOVR_EXCL_BR_LINE
            occupied_[i].store(false, std::memory_order_release);
        }
    }

    /** Give a dispatched slot's resources back and mark it free again. The
     * item is destroyed and default-constructed in place rather than
     * move-assigned from a temporary: T's move assignment may have side
     * effects (it is arbitrary user code) and may throw. A T whose default
     * constructor can throw is left untouched (the slot is still freed). */
    void recycle_slot(size_t i)
    {
        if (i >= pool_len_) {   // GCOVR_EXCL_BR_LINE
            return;
        }
        if constexpr (std::is_nothrow_default_constructible_v<T>) {
            items_[i].~T();
            ::new (static_cast<void *>(&items_[i])) T();
        }
        occupied_[i].store(false, std::memory_order_release);
    }

    /** Index of the item behind a pointer obtained from acquire_slot().
     * Returns pool_len_ for a pointer that is not in the pool (e.g. the
     * nullptr quit sentinel); release_slot() and recycle_slot() range-check
     * the result, so a stray pointer cannot index out of bounds. */
    size_t slot_of(T *p) const
    {
        if (p == nullptr) {     // GCOVR_EXCL_BR_LINE
            return pool_len_;   // GCOVR_EXCL_LINE: the worker breaks on nullptr first
        }
        return static_cast<size_t>(p - items_.get());
    }

    size_t pool_len_{ 0 };
    std::unique_ptr<T[]> items_;
    std::unique_ptr<std::atomic<bool>[]> occupied_;
    QueueHandle_t queue_{};
    QueueHandle_t done_{};
    TaskHandle_t worker_handle_{};
    std::atomic<size_t> rr_{ 0 };
    /* Destruction/post() synchronization: stopping_ is set by the
     * destructor before it drains the queues, and post() refuses to run
     * once it is set; posting_ counts the post() calls in flight and the
     * destructor waits for it to drain before touching the queues. */
    std::atomic<bool> stopping_{ false };
    std::atomic<size_t> posting_{ 0 };
};
