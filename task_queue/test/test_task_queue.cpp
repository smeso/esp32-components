/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <atomic>
#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <type_traits>

#include <doctest/doctest.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

// Must come after FreeRTOS.h: esp_freertos_hooks.h pulls in
// freertos/portmacro.h directly, which needs configNUMBER_OF_CORES (defined by
// FreeRTOSConfig.h through FreeRTOS.h) to decide whether to define
// portYIELD_CORE().
#include <esp_freertos_hooks.h>

#include "cpptaskcreate.hpp"
#include "mock_hooks.hpp"
#include "mock_timers.hpp"
#include "task_queue.hpp"

namespace {

// Wait until the atomic counter reaches the expected value, or until a 2 s
// deadline elapses (so a failure fails the test instead of hanging the
// suite).
void
wait_for(const std::atomic<size_t> &value, size_t expected)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
    while (value.load() < expected && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// esp_freertos_tick_cb_t is a plain function pointer, so the queue address is
// kept in a static that the tick hook reaches through this free function.
task_queue<std::function<void()>> *isr_queue = nullptr;

// The tick hook fires on every tick; the flag makes it post exactly once per
// registration so tests can assert on a precise processed count.
std::atomic<bool> isr_posted{ false };

// The hook posts a counting item: it must be a non-empty invocable, since the
// worker skips empty std::function items.
std::atomic<size_t> *isr_processed = nullptr;

void
post_from_tick_hook()
{
    if (isr_queue != nullptr && !isr_posted.exchange(true)) {
        std::atomic<size_t> *counter = isr_processed;
        (void) isr_queue->post([counter] { counter->fetch_add(1); });
    }
}

}   // namespace

TEST_CASE("task_queue is neither copyable nor movable")
{
    static_assert(!std::is_copy_constructible_v<task_queue<std::function<void()>>>);
    static_assert(!std::is_copy_assignable_v<task_queue<std::function<void()>>>);
    static_assert(!std::is_move_constructible_v<task_queue<std::function<void()>>>);
    static_assert(!std::is_move_assignable_v<task_queue<std::function<void()>>>);
}

TEST_CASE("exception derives from std::exception")
{
    static_assert(
            std::is_base_of_v<std::exception, task_queue<std::function<void()>>::exception>);
}

TEST_CASE("exception includes the message")
{
    task_queue<std::function<void()>>::exception e("boom");
    CHECK(std::strcmp(e.what(), "task_queue: boom") == 0);
}

TEST_CASE("posted items run on the worker task, not on the caller")
{
    std::atomic<TaskHandle_t> caller{ xTaskGetCurrentTaskHandle() };
    std::atomic<TaskHandle_t> worker{ nullptr };
    std::atomic<size_t> processed{ 0 };
    {
        task_queue<std::function<void()>> q("tq_ctx", 8);
        CHECK(q.post([&] {
            worker.store(xTaskGetCurrentTaskHandle());
            processed.fetch_add(1);
        }));
        wait_for(processed, 1);
        CHECK(processed.load() == 1);
    }
    CHECK(worker.load() != nullptr);
    CHECK(worker.load() != caller.load());
}

TEST_CASE("posted invocable items are invoked")
{
    std::atomic<size_t> processed{ 0 };
    {
        task_queue<std::function<void()>> q("tq_fn", 8);
        CHECK(q.post([&] { processed.fetch_add(1); }));
        CHECK(q.post([&] { processed.fetch_add(1); }));
        wait_for(processed, 2);
        CHECK(processed.load() == 2);
    }
}

TEST_CASE("post reports whether the item was enqueued")
{
    task_queue<std::function<void()>> q("tq_bool", 8);
    CHECK(q.post([] {}));
}

TEST_CASE("post drops items when the queue is full")
{
    std::atomic<size_t> processed{ 0 };
    std::atomic<size_t> entered{ 0 };
    SemaphoreHandle_t gate = xSemaphoreCreateBinary();
    REQUIRE(gate != nullptr);
    {
        // The worker blocks inside the first item, so the queue fills up
        // and the excess posts are dropped.
        task_queue<std::function<void()>> q("tq_full", 2, 4096, 5);
        CHECK(q.post([&] {
            if (entered.fetch_add(1) == 0) {
                xSemaphoreTake(gate, portMAX_DELAY);
            }
            processed.fetch_add(1);
        }));
        wait_for(entered, 1);
        CHECK(q.post([&] { processed.fetch_add(1); }));
        CHECK(q.post([&] { processed.fetch_add(1); }));
        CHECK_FALSE(q.post([] {}));   // queue full: this item is dropped and logged
        xSemaphoreGive(gate);
        wait_for(processed, 3);
        CHECK(processed.load() == 3);
    }
    vSemaphoreDelete(gate);
}

TEST_CASE("post drops items in ISR context and reports it without logging")
{
    // The ISR path of post() must never log, so the drop is only
    // reported through the false return value. The worker blocks inside
    // the first item, so the queue fills up and the excess posts are
    // dropped.
    std::atomic<size_t> processed{ 0 };
    std::atomic<size_t> entered{ 0 };
    SemaphoreHandle_t gate = xSemaphoreCreateBinary();
    REQUIRE(gate != nullptr);
    {
        task_queue<std::function<void()>> q("tq_full_isr", 2, 4096, 5);
        CHECK(q.post([&] {
            if (entered.fetch_add(1) == 0) {
                xSemaphoreTake(gate, portMAX_DELAY);
            }
            processed.fetch_add(1);
        }));
        wait_for(entered, 1);
        mock_bus::mock_hooks_set_in_isr(true);
        CHECK(q.post([&] { processed.fetch_add(1); }));
        CHECK(q.post([&] { processed.fetch_add(1); }));
        CHECK_FALSE(q.post([] {}));
        mock_bus::mock_hooks_set_in_isr(false);
        xSemaphoreGive(gate);
        wait_for(processed, 3);
        CHECK(processed.load() == 3);
    }
    vSemaphoreDelete(gate);
}

TEST_CASE("exceptions thrown by items are contained in the worker")
{
    std::atomic<size_t> processed{ 0 };
    task_queue<std::function<void()>> q("tq_exc", 8);
    CHECK(q.post([&] {
        processed.fetch_add(1);
        throw std::runtime_error("item boom");
    }));
    CHECK(q.post([&] { processed.fetch_add(1); }));
    wait_for(processed, 2);
    CHECK(processed.load() == 2);
}

TEST_CASE("unknown exceptions thrown by items are contained too")
{
    std::atomic<size_t> processed{ 0 };
    task_queue<std::function<void()>> q("tq_exc2", 8);
    CHECK(q.post([&] {
        processed.fetch_add(1);
        throw 42;
    }));
    CHECK(q.post([&] { processed.fetch_add(1); }));
    wait_for(processed, 2);
    CHECK(processed.load() == 2);
}

TEST_CASE("empty items are skipped instead of being invoked")
{
    std::atomic<size_t> processed{ 0 };
    {
        task_queue<std::function<void()>> q("tq_empty", 8);
        std::function<void()> empty;
        CHECK(q.post(empty));
        CHECK(q.post([&] { processed.fetch_add(1); }));
        wait_for(processed, 1);
        CHECK(processed.load() == 1);
    }
}

TEST_CASE("items still queued when the queue is destroyed are dropped")
{
    std::atomic<size_t> processed{ 0 };
    std::atomic<size_t> entered{ 0 };
    {
        // The worker starts item 1 and delays inside it: the destructor
        // must drop the still-queued items 2 and 3, then wait for the
        // in-flight item 1 to finish before freeing the object.
        task_queue<std::function<void()>> q("tq_drain", 4, 4096, 5);
        CHECK(q.post([&] {
            entered.fetch_add(1);
            vTaskDelay(pdMS_TO_TICKS(100));
            processed.fetch_add(1);
        }));
        wait_for(entered, 1);
        CHECK(q.post([] {}));
        CHECK(q.post([] {}));
    }
    CHECK(entered.load() == 1);
    CHECK(processed.load() == 1);
}

TEST_CASE("destructor waits for a post blocked in the item's move assignment")
{
    struct slow_move
    {
        SemaphoreHandle_t started;
        SemaphoreHandle_t release;
        slow_move() = default;
        slow_move(SemaphoreHandle_t s, SemaphoreHandle_t r)
                : started(s)
                , release(r)
        {
        }
        slow_move(const slow_move &) = delete;
        slow_move(slow_move &&other)
                : started(other.started)
                , release(other.release)
        {
        }
        slow_move &operator=(slow_move &&other)
        {
            started = other.started;
            release = other.release;
            /* Announce the assignment and then park inside it while the
             * test task is about to run the destructor: the destructor
             * must wait for this in-flight post() to finish before
             * touching anything. */
            xSemaphoreGive(started);
            xSemaphoreTake(release, portMAX_DELAY);
            return *this;
        }
        void operator()() const
        {
        }
    };
    struct poster_args_t
    {
        task_queue<slow_move> *q;
        SemaphoreHandle_t started;
        SemaphoreHandle_t release;
    };
    struct giver_args_t
    {
        SemaphoreHandle_t go;
        SemaphoreHandle_t release;
    };
    SemaphoreHandle_t started = xSemaphoreCreateBinary();
    SemaphoreHandle_t release = xSemaphoreCreateBinary();
    SemaphoreHandle_t go = xSemaphoreCreateBinary();
    REQUIRE(started != nullptr);
    REQUIRE(release != nullptr);
    REQUIRE(go != nullptr);
    {
        task_queue<slow_move> q("tq_slowmove", 4);
        poster_args_t poster_args{ &q, started, release };
        giver_args_t giver_args{ go, release };
        REQUIRE(cppTaskCreate("tq_park_post",
                              2048,
                              uxTaskPriorityGet(nullptr),
                              nullptr,
                              [&poster_args] {
                                  CHECK(poster_args.q->post(slow_move{ poster_args.started,
                                                                       poster_args.release }));
                                  vTaskSuspend(nullptr);
                              }) == pdPASS);
        REQUIRE(cppTaskCreate(
                        "tq_give", 2048, uxTaskPriorityGet(nullptr), nullptr, [&giver_args] {
                            xSemaphoreTake(giver_args.go, portMAX_DELAY);
                            xSemaphoreGive(giver_args.release);
                            vTaskSuspend(nullptr);
                        }) == pdPASS);
        // Let the poster park inside the move assignment of its item.
        CHECK(xSemaphoreTake(started, portMAX_DELAY) == pdTRUE);
        // Arm the giver, then let the destructor run: it must wait for the
        // parked post() (the giver hands the item over from its tick
        // delay), and the parked item is then dropped like any other
        // still-queued item.
        xSemaphoreGive(go);
    }
    vSemaphoreDelete(started);
    vSemaphoreDelete(release);
    vSemaphoreDelete(go);
}
TEST_CASE("post racing the destructor backs off instead of crashing")
{
    std::atomic<size_t> accepted{ 0 };
    std::atomic<size_t> rejected{ 0 };
    std::atomic<size_t> exited{ 0 };
    std::atomic<size_t> entered{ 0 };
    // Heap-allocated so the destructor runs while a poster task is still
    // posting: without the stopping/posting synchronization this is a
    // use-after-free on the FreeRTOS queues and the item pool.
    auto *q = new task_queue<std::function<void()>>("tq_race", 64);
    // Keep the worker busy inside a long item, so the destructor (which
    // joins it) stays in progress long enough for the poster task, which
    // wakes every tick, to observe post() refusing to enqueue.
    CHECK(q->post([&] {
        entered.fetch_add(1);
        vTaskDelay(pdMS_TO_TICKS(100));
    }));
    wait_for(entered, 1);
    // A large queue and only ~20 poster rounds before the destructor starts
    // keep the queue from filling up, so the poster's rejection can only
    // come from the shutdown, not from a full queue. The poster parks itself
    // at the end instead of returning, since returning into cppTaskCreate()'s
    // trampoline would abort.
    TaskHandle_t poster = nullptr;
    REQUIRE(cppTaskCreate("tq_poster", 2048, uxTaskPriorityGet(nullptr), &poster, [&] {
                for (;;) {
                    if (q->post([] {})) {
                        accepted.fetch_add(1);
                    } else {
                        rejected.fetch_add(1);
                        break;
                    }
                    vTaskDelay(1);
                }
                exited.fetch_add(1);
                vTaskSuspend(nullptr);
            }) == pdPASS);
    vTaskDelay(pdMS_TO_TICKS(20));
    delete q;
    // The poster exits at its first rejected post, which happens while the
    // destructor is still joining the long in-flight item.
    wait_for(exited, 1);
    CHECK(accepted.load() > 0);
    CHECK(rejected.load() == 1);
}

TEST_CASE("post is safe from ISR context")
{
    std::atomic<size_t> processed{ 0 };
    task_queue<std::function<void()>> q("tq_isr", 8);
    isr_queue = &q;
    isr_posted = false;
    isr_processed = &processed;
    // The tick hook runs in the SysTick interrupt context, so posting from
    // it exercises the xPortInIsrContext() path of post().
    REQUIRE(esp_register_freertos_tick_hook(post_from_tick_hook) == ESP_OK);
    wait_for(processed, 1);
    CHECK(processed.load() == 1);
    esp_deregister_freertos_tick_hook(post_from_tick_hook);
    // Give any tick ISR that already grabbed the hook a chance to finish
    // before the queue is destroyed.
    vTaskDelay(pdMS_TO_TICKS(10));
}

TEST_CASE("post from ISR context with an equal-priority worker does not yield")
{
    // A worker at the same priority as this task: a send from the tick ISR
    // does not request a yield, so portYIELD_FROM_ISR() takes its no-op
    // branch. This task busy-waits (instead of calling vTaskDelay()) so the
    // tick always interrupts this task rather than the idle task, keeping the
    // no-yield outcome deterministic.
    std::atomic<size_t> processed{ 0 };
    task_queue<std::function<void()>> q("tq_isr_eq", 8, 4096, uxTaskPriorityGet(nullptr));
    isr_queue = &q;
    isr_posted = false;
    isr_processed = &processed;
    REQUIRE(esp_register_freertos_tick_hook(post_from_tick_hook) == ESP_OK);
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
    while (processed.load() == 0 && xTaskGetTickCount() < deadline) {
        // Busy-wait for the tick ISR to post and the worker to process.
    }
    CHECK(processed.load() == 1);
    esp_deregister_freertos_tick_hook(post_from_tick_hook);
    // Give any tick ISR that already grabbed the hook a chance to finish
    // before the queue is destroyed.
    vTaskDelay(pdMS_TO_TICKS(10));
}

TEST_CASE("post wraps around the item pool when scanning for a free slot")
{
    std::atomic<size_t> processed{ 0 };
    std::atomic<size_t> entered{ 0 };
    SemaphoreHandle_t gate = xSemaphoreCreateBinary();
    SemaphoreHandle_t gate2 = xSemaphoreCreateBinary();
    REQUIRE(gate != nullptr);
    REQUIRE(gate2 != nullptr);
    {
        // The pool holds queue_len + 1 items and the round-robin start
        // index keeps advancing across posts: with the worker parked in
        // the second gate (item 2 in flight, item 3 still queued), the
        // next post starts at an occupied slot and must scan past the
        // end of the pool to find the freed first slot.
        task_queue<std::function<void()>> q("tq_wrap", 2, 4096, 5);
        std::function<void()> item = [&] {
            const size_t n = entered.fetch_add(1) + 1;
            if (n == 1) {
                xSemaphoreTake(gate, portMAX_DELAY);
            } else if (n == 2) {
                xSemaphoreTake(gate2, portMAX_DELAY);
            }
            processed.fetch_add(1);
        };
        CHECK(q.post(item));
        wait_for(entered, 1);
        CHECK(q.post(item));
        CHECK(q.post(item));
        CHECK_FALSE(q.post(item));
        // Item 1 finishes, freeing the first item; item 2 parks in gate2.
        xSemaphoreGive(gate);
        wait_for(entered, 2);
        // Start index is now 1 and slots 1 and 2 are still occupied, so
        // this post can only succeed by wrapping around to slot 0.
        CHECK(q.post(item));
        xSemaphoreGive(gate2);
        wait_for(processed, 2);
        CHECK(q.post(item));
        wait_for(processed, 3);
        CHECK(q.post(item));
        wait_for(processed, 6);
        CHECK(processed.load() == 6);
    }
    vSemaphoreDelete(gate);
    vSemaphoreDelete(gate2);
}

TEST_CASE("post rolls the slot back when the item's move assignment throws")
{
    struct throw_on_move
    {
        throw_on_move() = default;
        throw_on_move(const throw_on_move &) = delete;
        throw_on_move(throw_on_move &&) = default;
        throw_on_move &operator=(throw_on_move &&)
        {
            throw std::runtime_error("move boom");
        }
        void operator()() const
        {
        }
    };
    task_queue<throw_on_move> q("tq_throwmove", 4);
    // More attempts than pool slots (queue_len + 1): if a throwing post()
    // did not release its slot, the pool would be exhausted after the
    // first few attempts and the later (exhausted) posts would return
    // false instead of throwing.
    for (size_t attempt = 0; attempt < 3 * (4 + 1); ++attempt) {
        CHECK_THROWS_AS(q.post(throw_on_move{}), std::exception);
    }
    CHECK(std::strcmp(task_queue<throw_on_move>::exception("boom").what(),
                      "task_queue: boom") == 0);
}

TEST_CASE("a zero queue_len is rejected with a specific message")
{
    CHECK_THROWS_WITH_AS((task_queue<std::function<void()>>("tq_zero", 0)),
                         "task_queue: queue_len must be greater than zero",
                         task_queue<std::function<void()>>::exception);
}

TEST_CASE("post drops when the queue is full with no item dispatched")
{
    // A lower-priority worker does not run between the two posts, so the
    // queue (queue_len == 1) is full while no item is being dispatched and
    // the pool still has a free slot: the second post takes that slot and
    // then fails the 0-timeout send. This is the queue-full branch, as
    // opposed to the pool-exhaustion branch exercised by the tests above.
    std::atomic<size_t> processed{ 0 };
    {
        task_queue<std::function<void()>> q("tq_full_nodispatch", 1, 4096, tskIDLE_PRIORITY);
        CHECK(q.post([&] { processed.fetch_add(1); }));
        CHECK_FALSE(q.post([&] { processed.fetch_add(1); }));
        // Yield so the lower-priority worker can run the one queued item.
        vTaskDelay(pdMS_TO_TICKS(50));
        CHECK(processed.load() == 1);
    }
}

TEST_CASE("the pool gives a dispatched item's resources back")
{
    std::atomic<size_t> processed{ 0 };
    std::weak_ptr<int> weak;
    {
        task_queue<std::function<void()>> q("tq_release", 4);
        {
            auto held = std::make_shared<int>(1);
            weak = held;
            CHECK(q.post([held] {}));
        }
        // Only the item stored in the pool slot still owns the shared_ptr
        // now. Once the worker has dispatched it, the slot must have
        // dropped its resources, which the following item observes (the
        // worker resets a slot before it receives the next item).
        CHECK(q.post([&] {
            CHECK(weak.expired());
            processed.fetch_add(1);
        }));
        wait_for(processed, 1);
        CHECK(processed.load() == 1);
    }
}

TEST_CASE("drain waits for the items posted before it was invoked")
{
    std::atomic<size_t> processed{ 0 };
    std::atomic<size_t> entered{ 0 };
    SemaphoreHandle_t gate = xSemaphoreCreateBinary();
    SemaphoreHandle_t go = xSemaphoreCreateBinary();
    REQUIRE(gate != nullptr);
    REQUIRE(go != nullptr);
    {
        // The worker parks inside item 1 while items 2 and 3 pile up: the
        // drain must wait for all of them (the parked item included), not
        // just for the queue to empty out at some point.
        task_queue<std::function<void()>> q("tq_drain_wait", 8, 4096, 5);
        CHECK(q.post([&] {
            if (entered.fetch_add(1) == 0) {
                xSemaphoreTake(gate, portMAX_DELAY);
            }
            processed.fetch_add(1);
        }));
        wait_for(entered, 1);
        CHECK(q.post([&] { processed.fetch_add(1); }));
        CHECK(q.post([&] { processed.fetch_add(1); }));
        // Unpark the worker from a helper task after a tick, so drain()
        // really blocks on the semaphore instead of sailing through.
        struct giver_args_t
        {
            SemaphoreHandle_t go;
            SemaphoreHandle_t gate;
        };
        giver_args_t giver_args{ go, gate };
        auto r = cppTaskCreate(
                "tq_drain_give", 2048, uxTaskPriorityGet(nullptr), nullptr, [&giver_args] {
                    xSemaphoreTake(giver_args.go, portMAX_DELAY);
                    vTaskDelay(pdMS_TO_TICKS(20));
                    xSemaphoreGive(giver_args.gate);
                    vTaskSuspend(nullptr);
                });
        REQUIRE(r == pdPASS);
        xSemaphoreGive(go);
        q.drain();
        CHECK(processed.load() == 3);
    }
    vSemaphoreDelete(gate);
    vSemaphoreDelete(go);
}

TEST_CASE("drain throws when its sentinel cannot be posted")
{
    std::atomic<size_t> processed{ 0 };
    std::atomic<size_t> entered{ 0 };
    SemaphoreHandle_t gate = xSemaphoreCreateBinary();
    REQUIRE(gate != nullptr);
    {
        // The worker blocks inside the first item, so the queue fills up
        // and drain()'s sentinel post is dropped: drain() must report it
        // instead of blocking forever on a semaphore that is never given.
        task_queue<std::function<void()>> q("tq_drain_full", 2, 4096, 5);
        CHECK(q.post([&] {
            if (entered.fetch_add(1) == 0) {
                xSemaphoreTake(gate, portMAX_DELAY);
            }
            processed.fetch_add(1);
        }));
        wait_for(entered, 1);
        CHECK(q.post([&] { processed.fetch_add(1); }));
        CHECK(q.post([&] { processed.fetch_add(1); }));
        CHECK_FALSE(q.post([] {}));
        CHECK_THROWS_AS(q.drain(), task_queue<std::function<void()>>::exception);
        // Once the queue has room again, drain() completes normally.
        xSemaphoreGive(gate);
        wait_for(processed, 3);
        CHECK(processed.load() == 3);
        q.drain();
        CHECK(processed.load() == 3);
    }
    vSemaphoreDelete(gate);
}

TEST_CASE("post from ISR context handles the yield request")
{
    // A same-priority worker: the FromISR send does not request a yield, so
    // portYIELD_FROM_ISR() takes its no-op path.
    std::atomic<size_t> processed{ 0 };
    {
        task_queue<std::function<void()>> q(
                "tq_isr_same", 8, 4096, uxTaskPriorityGet(nullptr));
        mock_bus::mock_hooks_set_in_isr(true);
        CHECK(q.post([&] { processed.fetch_add(1); }));
        mock_bus::mock_hooks_set_in_isr(false);
        wait_for(processed, 1);
        CHECK(processed.load() == 1);
    }
    // A higher-priority worker: the FromISR send requests a yield, so
    // portYIELD_FROM_ISR() switches to the worker before post() returns.
    {
        std::atomic<size_t> yielded{ 0 };
        task_queue<std::function<void()>> q(
                "tq_isr_yield", 8, 4096, uxTaskPriorityGet(nullptr) + 1);
        CHECK(q.post([&] { yielded.fetch_add(1); }));
        // The higher-priority worker runs to completion and only yields the
        // CPU back to this task when it blocks on receive again, so once
        // wait_for() returns it is guaranteed to be waiting on the queue.
        wait_for(yielded, 1);
        mock_bus::mock_hooks_set_in_isr(true);
        CHECK(q.post([&] { yielded.fetch_add(1); }));
        mock_bus::mock_hooks_set_in_isr(false);
        wait_for(yielded, 2);
        CHECK(yielded.load() == 2);
    }
}

#if CONFIG_IDF_TARGET_LINUX
TEST_CASE("constructor cleans up when the done queue cannot be created")
{
    // The first queue (the item queue) is created, the second one (the done
    // queue) fails: the constructor must delete the first and report the
    // failure instead of leaking it.
    mock_bus::mock_timers_reset();
    mock_bus::mock_timers_fail_x_queue_create(1);
    CHECK_THROWS_AS((task_queue<std::function<void()>>("tq_oom_done", 4)),
                    task_queue<std::function<void()>>::exception);
    mock_bus::mock_timers_reset();
}

TEST_CASE("constructor cleans up when the item queue cannot be created")
{
    // The very first queue creation fails; the done queue still succeeds, so
    // the cleanup must delete the done queue and report the failure.
    mock_bus::mock_timers_reset();
    mock_bus::mock_timers_fail_x_queue_create(0);
    CHECK_THROWS_AS((task_queue<std::function<void()>>("tq_oom_queue", 4)),
                    task_queue<std::function<void()>>::exception);
    mock_bus::mock_timers_reset();
}

TEST_CASE("constructor reports when the worker task cannot be created")
{
    // The queues are created but cppTaskCreate() fails to spawn the worker:
    // the constructor must delete both queues and throw.
    mock_bus::mock_timers_reset();
    mock_bus::mock_timers_fail_x_task_create(true);
    CHECK_THROWS_AS((task_queue<std::function<void()>>("tq_oom_worker", 4)),
                    task_queue<std::function<void()>>::exception);
    mock_bus::mock_timers_fail_x_task_create(false);
}

TEST_CASE("drain reports when its semaphore cannot be created")
{
    task_queue<std::function<void()>> q("tq_drain_oom", 4);
    // drain() creates a binary semaphore to wait on: make that creation fail
    // and check that drain() reports it instead of blocking forever.
    mock_bus::mock_timers_reset();
    mock_bus::mock_timers_fail_x_queue_create(0);
    CHECK_THROWS_AS(q.drain(), task_queue<std::function<void()>>::exception);
    mock_bus::mock_timers_reset();
    // The queue is still usable after the failed drain.
    CHECK(q.post([] {}));
    q.drain();
}
#endif
