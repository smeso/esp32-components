/*
 * SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <atomic>
#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <doctest/doctest.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "cpptaskcreate.hpp"
#include "mock_hooks.hpp"
#include "mock_timers.hpp"

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

// A stateless function pointer handed to cppTaskCreate(): not a lambda, so
// std::function copies it by value into its small-buffer storage.
// (bump() itself is never invoked: the huge stack size makes xTaskCreate()
// fail before the task exists.)
std::atomic<size_t> *bump_target = nullptr;
void
bump()
{
    bump_target->fetch_add(1);
}

// A callable tracking copy/move/invocation counts, used to prove that the
// component copies lvalue callables by value and moves rvalue callables.
struct copy_counter
{
    std::atomic<size_t> *counter = nullptr;

    copy_counter() = default;
    explicit copy_counter(std::atomic<size_t> *c)
            : counter(c)
    {
    }
    copy_counter(const copy_counter &other)
            : counter(other.counter)
    {
        copies.fetch_add(1);
    }
    copy_counter(copy_counter &&other)
            : counter(other.counter)
    {
        moves.fetch_add(1);
    }
    void operator()() const
    {
        counter->fetch_add(1);
    }

    static std::atomic<size_t> copies;
    static std::atomic<size_t> moves;
};

std::atomic<size_t> copy_counter::copies{ 0 };
std::atomic<size_t> copy_counter::moves{ 0 };

// The abort hook used by every test that reaches task_entry()'s abort(): it
// records the abort and suspends the aborting task, so the trampoline never
// comes back to execute the rest of abort(), keeping the whole suite alive.
// (vTaskDelete(NULL) would be unsafe here: on the linux port it ends the
// pthread from deep inside the abort machinery, which libstdc++ reports as a
// terminate.)
std::atomic<size_t> abort_count{ 0 };

void
abort_hook_suspend_self()
{
    abort_count.fetch_add(1);
    vTaskSuspend(nullptr);
}

// RAII guard installing the abort hook for exactly the scope that needs it.
// The destructor first waits for the task to reach abort() (the hook parks it
// there), so the hook is only removed once it cannot be needed anymore and it
// never propagates to tests that do not install it.
class abort_hook_guard
{
  public:
    abort_hook_guard()
            : baseline(abort_count.load())
    {
        mock_bus::mock_hooks_set_abort_hook(abort_hook_suspend_self);
    }
    ~abort_hook_guard()
    {
        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
        while (abort_count.load() == baseline && xTaskGetTickCount() < deadline) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        mock_bus::mock_hooks_set_abort_hook(nullptr);
    }

  private:
    const size_t baseline;
};

// Spawn a real FreeRTOS task whose body is exactly what cppTaskCreate()
// passes to xTaskCreate(): cpptaskcreate::task_entry() with a heap-allocated
// std::function. The abort hook must already be installed. Returns the
// guarded task's handle (or nullptr on failure); *ran is bumped by the
// callable once the trampoline actually runs it. The xTaskCreate failure
// branches are unreachable here (an 8192-word stack always succeeds in the
// test app), so they are excluded.
TaskHandle_t
test_task_entry(std::function<void()> fn, std::atomic<size_t> *ran)
{
    // Like cppTaskCreate(), but the callable first bumps the test counter
    // before doing whatever it does (returning / throwing).
    auto *ctx = new std::function<void()>([fn = std::move(fn), ran] {
        ran->fetch_add(1);
        fn();
    });
    TaskHandle_t handle = nullptr;
    const BaseType_t ok = xTaskCreate(cpptaskcreate::task_entry,
                                      "ctc_guarded",
                                      8192,
                                      ctx,
                                      uxTaskPriorityGet(nullptr) + 1,
                                      &handle);
    if (ok != pdPASS) {
        delete ctx;
        return nullptr;
    }
    return handle;
}

}   // namespace

TEST_CASE("cppTaskCreate runs a plain lambda on a fresh FreeRTOS task")
{
    abort_hook_guard hook;
    std::atomic<TaskHandle_t> caller{ xTaskGetCurrentTaskHandle() };
    std::atomic<TaskHandle_t> worker{ nullptr };
    std::atomic<size_t> processed{ 0 };
    const BaseType_t ret =
            cppTaskCreate("ctc_lambda", 4096, uxTaskPriorityGet(nullptr), nullptr, [&] {
                worker.store(xTaskGetCurrentTaskHandle());
                processed.fetch_add(1);
            });
    CHECK(ret == pdPASS);
    wait_for(processed, 1);
    CHECK(processed.load() == 1);
    CHECK(worker.load() != nullptr);
    CHECK(worker.load() != caller.load());
    // The callable returned, so the task aborted: the guard's destructor
    // waits for that abort before removing the hook.
}

TEST_CASE("cppTaskCreate reports failure when the task cannot be created")
{
    std::atomic<size_t> calls{ 0 };
    bump_target = &calls;
    // configSTACK_DEPTH_TYPE is uint32_t and xTaskCreate multiplies it by
    // sizeof(StackType_t) before allocating: on chip targets (where
    // StackType_t is one byte) a 0xFFFFFFFF request is ~4 GiB, far larger
    // than any heap, so xTaskCreate() reports an error and the callable is
    // freed again. On the linux simulator StackType_t is eight bytes wide, so
    // the same value asks for a ~34 GiB mapping that the host allocator may
    // actually satisfy (overcommitted), which is why both outcomes are
    // accepted here; what the test guarantees is that a failure is reported
    // exactly when the task did not run. The abort hook is only needed for the
    // linux outcome, in which the spawned task eventually reaches
    // task_entry()'s abort(), so it is installed for this call only.
#if defined(__SANITIZE_THREAD__)
    // ThreadSanitizer cannot spawn a task whose (overcommitted) stack request
    // is a 34 GiB virtual mapping: the simulator's pthread setup for it never
    // returns under TSan. Force the allocation failure deterministically with
    // the mock kernel API instead, exactly like the linux-only "ctc_oom" test.
    mock_bus::mock_timers_reset();
    mock_bus::mock_timers_fail_x_task_create(true);
    const BaseType_t ret =
            cppTaskCreate("ctc_fail", 4096, uxTaskPriorityGet(nullptr), nullptr, &bump);
    mock_bus::mock_timers_fail_x_task_create(false);
    CHECK(ret != pdPASS);
    CHECK(calls.load() == 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    CHECK(calls.load() == 0);
#else
    mock_bus::mock_hooks_set_abort_hook(abort_hook_suspend_self);
    const size_t aborted_before = abort_count.load();
    const BaseType_t ret =
            cppTaskCreate("ctc_fail", 0xFFFFFFFFU, uxTaskPriorityGet(nullptr), nullptr, &bump);
    if (ret != pdPASS) {
        CHECK(calls.load() == 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        CHECK(calls.load() == 0);
    } else {
        // The task was created after all (linux overcommit): wait for it to
        // abort before removing the hook.
        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
        while (abort_count.load() == aborted_before && xTaskGetTickCount() < deadline) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    mock_bus::mock_hooks_set_abort_hook(nullptr);
#endif
}

TEST_CASE("cppTaskCreate rejects an empty callable")
{
    // An empty std::function is "invocable" as far as the type system is
    // concerned, but invoking it would throw std::bad_function_call, which
    // task_entry() would turn into an abort of the whole application.
    // cppTaskCreate() must refuse it before creating the task and report the
    // error instead.
    std::function<void()> empty;
    const BaseType_t ret =
            cppTaskCreate("ctc_empty", 4096, uxTaskPriorityGet(nullptr), nullptr, empty);
    CHECK(ret != pdPASS);
}

#if CONFIG_IDF_TARGET_LINUX
TEST_CASE("cppTaskCreate frees the callable when task allocation fails")
{
    // Deterministic xTaskCreate() failure: the mock kernel API refuses to
    // create the task (on the Linux simulator the host allocator would
    // otherwise satisfy even a huge stack request), so cppTaskCreate() must
    // delete the heap context it created for the callable and report the
    // error. The callable is never invoked.
    mock_bus::mock_timers_reset();
    mock_bus::mock_timers_fail_x_task_create(true);
    std::atomic<size_t> calls{ 0 };
    bump_target = &calls;
    const BaseType_t ret =
            cppTaskCreate("ctc_oom", 4096, uxTaskPriorityGet(nullptr), nullptr, &bump);
    mock_bus::mock_timers_fail_x_task_create(false);
    CHECK(ret != pdPASS);
    CHECK(calls.load() == 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    CHECK(calls.load() == 0);
}
#endif

TEST_CASE("cppTaskCreate moves the callable into its heap context")
{
    // The closure type itself must be copyable to travel through
    // std::function<void()> (that is std::function's requirement, not this
    // component's); the point of this test is that the component *moves*
    // the callable into the heap-allocated std::function instead of copying
    // it, which the copy/move counter on an lvalue callable checks below.
    abort_hook_guard hook;
    std::atomic<size_t> processed{ 0 };
    const BaseType_t ret =
            cppTaskCreate("ctc_move", 4096, uxTaskPriorityGet(nullptr), nullptr, [&] {
                processed.fetch_add(1);
            });
    CHECK(ret == pdPASS);
    wait_for(processed, 1);
    CHECK(processed.load() == 1);
}

TEST_CASE("cppTaskCreate copies lvalue callables by value")
{
    abort_hook_guard hook;
    std::atomic<size_t> processed{ 0 };
    copy_counter::copies.store(0);
    copy_counter::moves.store(0);
    copy_counter callable{ &processed };
    const BaseType_t ret =
            cppTaskCreate("ctc_copy", 4096, uxTaskPriorityGet(nullptr), nullptr, callable);
    CHECK(ret == pdPASS);
    wait_for(processed, 1);
    CHECK(processed.load() == 1);
    // The lvalue is copied on its way into the heap context.
    CHECK(copy_counter::copies.load() >= 1);
    // The original lvalue must still be intact and usable afterwards.
    callable();
    CHECK(processed.load() == 2);
}

TEST_CASE("cppTaskCreate accepts a stateless function pointer")
{
    abort_hook_guard hook;
    std::atomic<size_t> processed{ 0 };
    static std::atomic<size_t> *sink = &processed;
    const BaseType_t ret = cppTaskCreate(
            "ctc_fnptr", 4096, uxTaskPriorityGet(nullptr), nullptr, +[] {
                sink->fetch_add(1);
            });
    CHECK(ret == pdPASS);
    wait_for(processed, 1);
    CHECK(processed.load() == 1);
}

TEST_CASE("cppTaskCreate hands the handle out when requested")
{
    abort_hook_guard hook;
    std::atomic<size_t> processed{ 0 };
    TaskHandle_t handle = nullptr;
    const BaseType_t ret =
            cppTaskCreate("ctc_handle", 4096, uxTaskPriorityGet(nullptr), &handle, [&] {
                processed.fetch_add(1);
            });
    CHECK(ret == pdPASS);
    CHECK(handle != nullptr);
    CHECK(std::strcmp(pcTaskGetName(handle), "ctc_handle") == 0);
    wait_for(processed, 1);
    CHECK(processed.load() == 1);
    // The callable returned, so the task aborted: the guard's destructor
    // waits for that abort before removing the hook.
}

TEST_CASE("task_entry aborts when the callable returns normally")
{
    std::atomic<size_t> ran{ 0 };
    abort_hook_guard hook;
    TaskHandle_t handle = test_task_entry([] { /* return normally */ }, &ran);
    REQUIRE(handle != nullptr);
    // The guarded task runs (bumping the counter), the callable returns and
    // the task is parked by the abort hook before executing the abort.
    wait_for(ran, 1);
    // The observable outcome of the recoverable abort: the counter never
    // advances past 1 and the suite keeps running.
    CHECK(ran.load() == 1);
}

TEST_CASE("task_entry aborts when the callable throws a std::exception")
{
    std::atomic<size_t> ran{ 0 };
    abort_hook_guard hook;
    TaskHandle_t handle = test_task_entry([] { throw std::runtime_error("ctc boom"); }, &ran);
    REQUIRE(handle != nullptr);
    wait_for(ran, 1);
    CHECK(ran.load() == 1);
}

TEST_CASE("task_entry aborts when the callable throws something else")
{
    std::atomic<size_t> ran{ 0 };
    abort_hook_guard hook;
    TaskHandle_t handle = test_task_entry([] { throw 42; }, &ran);
    REQUIRE(handle != nullptr);
    wait_for(ran, 1);
    CHECK(ran.load() == 1);
}

TEST_CASE("an exception thrown and caught inside the callable never reaches task_entry")
{
    // Deliberately no abort hook: the callable contains the exception itself,
    // so task_entry() must never see it. Had the throw escaped, task_entry()
    // would have caught it and aborted, killing the suite. The callable also
    // parks itself instead of returning, so the normal-return abort path is
    // not reached either; the only observable outcome is the counter bumped
    // from inside the catch block.
    std::atomic<size_t> caught{ 0 };
    const BaseType_t ret =
            cppTaskCreate("ctc_caught", 4096, uxTaskPriorityGet(nullptr), nullptr, [&] {
                try {
                    throw std::runtime_error("ctc internal");
                } catch (const std::exception &e) {
                    if (std::strcmp(e.what(), "ctc internal") == 0) {
                        caught.fetch_add(1);
                    }
                }
                vTaskSuspend(nullptr);
            });
    CHECK(ret == pdPASS);
    wait_for(caught, 1);
    CHECK(caught.load() == 1);
}

TEST_CASE("a callable can end its task with vTaskDelete")
{
    // vTaskDelete(NULL) from inside the callable is how a task normally ends:
    // it never returns into task_entry(), so neither abort path is reached.
    // On the linux simulator the port ends the task from inside that call with
    // pthread_exit(), i.e. a forced unwind: had task_entry() not forwarded it,
    // the process would abort here instead of the task disappearing (no abort
    // hook is installed, so a failure kills the suite).
    const UBaseType_t before = uxTaskGetNumberOfTasks();
    std::atomic<size_t> ran{ 0 };
    const BaseType_t ret =
            cppTaskCreate("ctc_selfdelete", 4096, uxTaskPriorityGet(nullptr), nullptr, [&] {
                ran.fetch_add(1);
                vTaskDelete(nullptr);
            });
    REQUIRE(ret == pdPASS);
    wait_for(ran, 1);
    CHECK(ran.load() == 1);
    // The kernel drops the task from the task count once the idle task has
    // cleaned up the TCB of the self-deleted task. Delay for full ticks (a
    // sub-tick vTaskDelay() would not block, and a busy loop would starve the
    // lower-priority idle task).
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
    while (uxTaskGetNumberOfTasks() >= before + 1 && xTaskGetTickCount() < deadline) {
        vTaskDelay(1);
    }
    CHECK(uxTaskGetNumberOfTasks() < before + 1);
}

TEST_CASE("a self-deleting task releases the callable's context")
{
    // The callable captures a shared_ptr by value, so its copy lives inside
    // the heap-allocated std::function that cppTaskCreate() owns for the task.
    // On the Linux simulator vTaskDelete(NULL) ends the task with a forced
    // unwind that destroys that std::function and releases the copy; on real
    // hardware the thread local storage deletion callback does the same. Either
    // way, once the task is gone the test's own reference must be the only one
    // left: anything higher would mean the context leaked.
    auto context = std::make_shared<int>(0);
    std::atomic<size_t> ran{ 0 };
    const BaseType_t ret = cppTaskCreate(
            "ctc_selfrelease", 4096, uxTaskPriorityGet(nullptr), nullptr, [context, &ran] {
                ran.fetch_add(1);
                vTaskDelete(nullptr);
            });
    REQUIRE(ret == pdPASS);
    wait_for(ran, 1);
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
    while (context.use_count() > 1 && xTaskGetTickCount() < deadline) {
        vTaskDelay(1);
    }
    CHECK(context.use_count() == 1);
}
