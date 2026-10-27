/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>

#include <doctest/doctest.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>

#include "button.hpp"
#include "mock_gpio.hpp"
#include "mock_timers.hpp"

using namespace mock_bus;

namespace {

constexpr gpio_num_t kPin = GPIO_NUM_0;

// Stand-in for a real task queue so the tests can exercise the TaskQueue
// template parameter without depending on task_queue; it only needs post()
// to accept the callback type.
template<typename T>
class mock_task_queue
{
  public:
    void post(T cb)
    {
        cb();
    }
};

// Wait until the predicate holds, or until a deadline elapses (so a failure
// fails the test instead of hanging the suite).
template<typename Pred>
void
wait_until(Pred pred, size_t timeout_ms = 3000)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (!pred() && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// Give the FreeRTOS timer daemon time to drain the command queue left behind
// by the previous test's button destructors (each destructor queues dozens of
// xTimerDelete/xTimerStop commands, and the command queue is small). Without
// this, rapid test execution can overflow the queue and make xTimerStart or
// the debounce timers fail, especially on slow QEMU targets.
void
settle_timers()
{
    vTaskDelay(pdMS_TO_TICKS(50));
    vTaskDelay(pdMS_TO_TICKS(50));
    vTaskDelay(pdMS_TO_TICKS(50));
}

// Stop the button and then let the timer daemon drain every command that
// still references the button's timers while the button object is alive. On
// multicore targets the daemon can otherwise process a stale reset after the
// button has been destroyed, dereferencing freed memory.
template<size_t MaxLongPresses, typename TaskQueue = button_detail::no_task_queue>
void
stop_and_drain(button<MaxLongPresses, TaskQueue> &btn)
{
    btn.stop();
    settle_timers();
}

// Simulate a button transition the way the GPIO interrupt would: update the
// mocked pin level and invoke the registered ISR handler, which arms the
// debounce timer.
void
drive_pin(int level)
{
    mock_gpio_set_level(kPin, level);
    gpio_isr_t handler = mock_gpio_isr_handler();
    REQUIRE(handler != nullptr);
    handler(mock_gpio_isr_arg());
}

// Clear the process-wide "ISR service installed" flag the button remembers,
// so the next start() calls gpio_install_isr_service() again. Needed by the
// tests injecting installation failures or an already-installed service.
void
reset_isr_service_state()
{
    button_detail::isr_service_installed_state().store(false);
}

}   // namespace

TEST_CASE("button is neither copyable nor movable")
{
    static_assert(!std::is_copy_constructible_v<button<>>);
    static_assert(!std::is_copy_assignable_v<button<>>);
    static_assert(!std::is_move_constructible_v<button<>>);
    static_assert(!std::is_move_assignable_v<button<>>);
}

TEST_CASE("button exception includes the message and the error name")
{
    button<>::exception plain("boom");
    CHECK(std::strcmp(plain.what(), "button: boom") == 0);

    button<>::exception with_err("gpio failed", ESP_ERR_INVALID_ARG);
    std::string msg(with_err.what());
    CHECK(msg.find("button: gpio failed") == 0);
    CHECK(msg.find("ESP_ERR_INVALID_ARG") != std::string::npos);
}

TEST_CASE("exception derives from std::exception")
{
    static_assert(std::is_base_of_v<std::exception, button<>::exception>);
}

TEST_CASE("task-queue instantiation exception includes the message")
{
    button<4, mock_task_queue<button<>::callback_t>>::exception plain("boom");
    const std::exception &plain_base = plain;
    CHECK(std::strcmp(plain_base.what(), "button: boom") == 0);

    button<4, mock_task_queue<button<>::callback_t>>::exception with_err("gpio failed",
                                                                         ESP_ERR_INVALID_ARG);
    const std::exception &with_err_base = with_err;
    std::string msg(with_err_base.what());
    CHECK(msg.find("button: gpio failed") == 0);
    CHECK(msg.find("ESP_ERR_INVALID_ARG") != std::string::npos);
}

TEST_CASE("start and stop are idempotent")
{
    settle_timers();
    mock_gpio_reset();
    {
        button<> btn(kPin);
        btn.start();
        btn.start();
        btn.stop();
        btn.stop();
    }
}

TEST_CASE("start reports gpio_config failures")
{
    settle_timers();
    mock_gpio_reset();
    {
        button<> btn(kPin);
        mock_gpio_fail_config(true);
        CHECK_THROWS_AS(btn.start(), button<>::exception);
        mock_gpio_fail_config(false);
    }
}

TEST_CASE("start reports gpio_install_isr_service failures")
{
    settle_timers();
    mock_gpio_reset();
    reset_isr_service_state();
    {
        button<> btn(kPin);
        mock_gpio_fail_install_isr_service(true);
        CHECK_THROWS_AS(btn.start(), button<>::exception);
        mock_gpio_fail_install_isr_service(false);
    }
}

TEST_CASE("start tolerates an already installed isr service")
{
    settle_timers();
    mock_gpio_reset();
    reset_isr_service_state();
    {
        button<> btn(kPin);
        // The real driver returns ESP_ERR_INVALID_STATE when the ISR service
        // has already been installed; start() must tolerate it.
        mock_gpio_install_isr_service_invalid_state(true);
        CHECK_NOTHROW(btn.start());
        mock_gpio_install_isr_service_invalid_state(false);
        CHECK_NOTHROW(btn.stop());
    }
}

TEST_CASE("the isr service is installed only once per process")
{
    settle_timers();
    mock_gpio_reset();
    reset_isr_service_state();
    {
        button<> first(kPin);
        button<> second(kPin);
        first.start();
        CHECK(mock_gpio_install_isr_service_calls() == 1);

        // The service is already installed: starting another button must not
        // call the driver again (which would log an error).
        second.start();
        CHECK(mock_gpio_install_isr_service_calls() == 1);

        second.stop();
        first.stop();

        // The service stays installed across stop() as well.
        first.start();
        CHECK(mock_gpio_install_isr_service_calls() == 1);
        first.stop();
    }
}

TEST_CASE("start reports gpio_isr_handler_add failures")
{
    settle_timers();
    mock_gpio_reset();
    {
        button<> btn(kPin);
        mock_gpio_fail_isr_handler_add(true);
        CHECK_THROWS_AS(btn.start(), button<>::exception);
        mock_gpio_fail_isr_handler_add(false);
    }
}

TEST_CASE("stop reports gpio_isr_handler_remove failures without throwing")
{
    settle_timers();
    mock_gpio_reset();
    {
        button<> btn(kPin);
        btn.start();
        mock_gpio_fail_isr_handler_remove(true);
        CHECK_NOTHROW(btn.stop());
        mock_gpio_fail_isr_handler_remove(false);
    }
}

TEST_CASE("callbacks must be registered before start")
{
    settle_timers();
    mock_gpio_reset();
    {
        button<4> btn(kPin);
        btn.start();
        CHECK_THROWS_AS(btn.on_press([] {}), button<4>::exception);
        CHECK_THROWS_AS(btn.on_release([] {}), button<4>::exception);
        CHECK_THROWS_AS(btn.add_longpress(100, [] {}), button<4>::exception);
        btn.stop();
    }
}

TEST_CASE("long-press threshold limit is enforced")
{
    settle_timers();
    mock_gpio_reset();
    {
        button<1> btn(kPin);
        btn.add_longpress(100, [] {});
        CHECK_THROWS_AS(btn.add_longpress(200, [] {}), button<1>::exception);
    }
}

TEST_CASE("long-press is disabled when MaxLongPresses is zero")
{
    settle_timers();
    mock_gpio_reset();
    {
        button<> btn(kPin);
        CHECK_THROWS_AS(btn.add_longpress(100, [] {}), button<>::exception);
    }
}

TEST_CASE("single-slot button presses and long-presses")
{
    settle_timers();
    mock_gpio_reset();
    mock_gpio_set_level(kPin, 1);
    {
        button<1> btn(kPin, true, false, 10);
        std::atomic<size_t> presses{ 0 };
        std::atomic<size_t> long_presses{ 0 };
        btn.on_press([&] { presses.fetch_add(1); });
        btn.add_longpress(30, [&] { long_presses.fetch_add(1); });
        btn.start();

        drive_pin(0);
        wait_until([&] { return presses.load() == 1; });
        CHECK(presses.load() == 1);
        wait_until([&] { return long_presses.load() == 1; });
        CHECK(long_presses.load() == 1);

        drive_pin(1);
        wait_until([&] { return presses.load() == 1; });
        CHECK(presses.load() == 1);

        stop_and_drain(btn);
    }
}

TEST_CASE("press and release callbacks fire after debounce")
{
    settle_timers();
    mock_gpio_reset();
    mock_gpio_set_level(kPin, 1);
    {
        button<> btn(kPin, true, false, 10);
        std::atomic<size_t> presses{ 0 };
        std::atomic<size_t> releases{ 0 };
        btn.on_press([&] { presses.fetch_add(1); });
        btn.on_release([&] { releases.fetch_add(1); });
        btn.start();

        // Idle: pin high with an active-low button.
        drive_pin(1);
        vTaskDelay(pdMS_TO_TICKS(20));
        CHECK(presses.load() == 0);

        // Pin goes low (button pressed): the debounce timer fires and the
        // press callback runs.
        drive_pin(0);
        wait_until([&] { return presses.load() == 1; });
        CHECK(presses.load() == 1);
        CHECK(releases.load() == 0);

        // Pin goes high again (button released): the release callback runs.
        drive_pin(1);
        wait_until([&] { return releases.load() == 1; });

        stop_and_drain(btn);
    }
}

TEST_CASE("debounce_ms zero disables debouncing and fires callbacks immediately")
{
    settle_timers();
    mock_gpio_reset();
    mock_gpio_set_level(kPin, 1);
    {
        button<> btn(kPin, true, false, 0);
        std::atomic<size_t> presses{ 0 };
        std::atomic<size_t> releases{ 0 };
        btn.on_press([&] { presses.fetch_add(1); });
        btn.on_release([&] { releases.fetch_add(1); });
        btn.start();

        drive_pin(0);
        wait_until([&] { return presses.load() == 1; });
        CHECK(presses.load() == 1);
        CHECK(releases.load() == 0);

        drive_pin(1);
        wait_until([&] { return releases.load() == 1; });
        CHECK(releases.load() == 1);

        stop_and_drain(btn);
    }
}

TEST_CASE("internal pull configuration is selected by active level")
{
    settle_timers();
    mock_gpio_reset();
    {
        // should_pull + active_low -> pull-up; should_pull + active-high
        // -> pull-down. These exercise the remaining pull branches in the
        // gpio_config_t initializer.
        button<> pull_up_btn(kPin, true, true, 10);
        pull_up_btn.start();
        pull_up_btn.stop();

        button<> pull_down_btn(kPin, false, true, 10);
        pull_down_btn.start();
        pull_down_btn.stop();
    }
}

TEST_CASE("active-high buttons are supported")
{
    settle_timers();
    mock_gpio_reset();
    mock_gpio_set_level(kPin, 0);
    {
        button<> btn(kPin, false, false, 10);
        std::atomic<size_t> presses{ 0 };
        btn.on_press([&] { presses.fetch_add(1); });
        btn.start();

        drive_pin(1);
        wait_until([&] { return presses.load() == 1; });
        CHECK(presses.load() == 1);

        stop_and_drain(btn);
    }
}

TEST_CASE("long-press fires after the threshold")
{
    settle_timers();
    mock_gpio_reset();
    mock_gpio_set_level(kPin, 1);
    {
        button<4> btn(kPin, true, false, 10);
        std::atomic<size_t> long_presses{ 0 };
        btn.on_press([] {});
        btn.add_longpress(30, [&] { long_presses.fetch_add(1); });
        btn.add_longpress(500, [&] { long_presses.fetch_add(1); });
        btn.start();

        drive_pin(0);
        vTaskDelay(pdMS_TO_TICKS(20));
        CHECK(long_presses.load() == 0);
        wait_until([&] { return long_presses.load() == 1; });
        // Only the short threshold fired; the 500 ms one is still pending.
        CHECK(long_presses.load() == 1);

        // Releasing the pin stops the long-press machinery.
        drive_pin(1);
        vTaskDelay(pdMS_TO_TICKS(40));
        CHECK(long_presses.load() == 1);

        stop_and_drain(btn);
    }
}

TEST_CASE("callbacks can be posted to a task queue")
{
    settle_timers();
    mock_gpio_reset();
    mock_gpio_set_level(kPin, 1);
    {
        auto queue = std::make_shared<mock_task_queue<button<>::callback_t>>();
        button<4, mock_task_queue<button<>::callback_t>> btn(kPin, true, false, 10, queue);
        std::atomic<size_t> presses{ 0 };
        btn.on_press([&] { presses.fetch_add(1); });
        btn.start();

        drive_pin(0);
        wait_until([&] { return presses.load() == 1; });
        CHECK(presses.load() == 1);

        stop_and_drain(btn);
    }
}

TEST_CASE("task queue type with null queue runs callbacks directly")
{
    settle_timers();
    mock_gpio_reset();
    mock_gpio_set_level(kPin, 1);
    {
        button<4, mock_task_queue<button<>::callback_t>> btn(kPin, true, false, 10);
        std::atomic<size_t> presses{ 0 };
        btn.on_press([&] { presses.fetch_add(1); });
        btn.start();

        drive_pin(0);
        wait_until([&] { return presses.load() == 1; });
        CHECK(presses.load() == 1);

        stop_and_drain(btn);
    }
}

TEST_CASE("empty callbacks are tolerated")
{
    settle_timers();
    mock_gpio_reset();
    mock_gpio_set_level(kPin, 1);
    {
        button<> btn(kPin, true, false, 10);
        // Press and release without registering callbacks: run() must
        // tolerate the empty std::function.
        btn.start();

        drive_pin(0);
        vTaskDelay(pdMS_TO_TICKS(40));
        drive_pin(1);
        vTaskDelay(pdMS_TO_TICKS(40));

        stop_and_drain(btn);
    }
}

TEST_CASE("an invalid gpio pin is rejected")
{
    settle_timers();
    mock_gpio_reset();
    {
        // The constructor validates the pin before anything can be armed,
        // so a bad pin never reaches gpio_config().
        CHECK_THROWS_AS((button<>(GPIO_NUM_NC)), button<>::exception);
        CHECK_NOTHROW((button<>(kPin)));
    }
}

TEST_CASE("debounce below the tick resolution is rejected")
{
    settle_timers();
    mock_gpio_reset();
    {
        // 1 ms is below the tick resolution with a 100 Hz tick.
        CHECK_THROWS_AS(button<>(kPin, true, false, 1), button<>::exception);
    }
}

TEST_CASE("long-press threshold below the tick resolution is rejected")
{
    settle_timers();
    mock_gpio_reset();
    {
        button<1> btn(kPin);
        CHECK_THROWS_AS(btn.add_longpress(0, [] {}), button<1>::exception);
        CHECK_THROWS_AS(btn.add_longpress(1, [] {}), button<1>::exception);
        // A valid threshold still works after the rejected ones.
        CHECK_NOTHROW(btn.add_longpress(30, [] {}));
    }
}

#if CONFIG_IDF_TARGET_LINUX
TEST_CASE("constructor reports xTimerCreate drain failures")
{
    settle_timers();
    mock_gpio_reset();
    mock_timers_reset();
    {
        // The drain timer is created after the debounce timer: let the
        // debounce timer succeed, then fail the drain timer creation.
        mock_timers_fail_x_timer_create_after(1);
        CHECK_THROWS_AS((button<>(kPin, true, false, 10)), button<>::exception);
        mock_timers_reset();
    }
}

TEST_CASE("constructor reports xTimerCreate debounce failures")
{
    settle_timers();
    mock_gpio_reset();
    mock_timers_reset();
    {
        // Fail the very first xTimerCreate() call: the debounce timer (the
        // first timer created when debounce_ms > 0) is nullptr after the
        // member initializers, so the constructor reports it.
        mock_timers_fail_x_timer_create_after(0);
        CHECK_THROWS_AS((button<>(kPin, true, false, 10)), button<>::exception);
        mock_timers_reset();
    }
}

TEST_CASE("constructor releases the drain timer when the debounce timer fails")
{
    settle_timers();
    mock_gpio_reset();
    mock_timers_reset();
    {
        // The debounce timer creation fails (first call), but the drain timer
        // and semaphore created afterwards succeed. The constructor must
        // still release them before throwing, because the destructor does not
        // run for an object whose constructor threw.
        mock_timers_fail_x_timer_create_after(0);
        CHECK_THROWS_AS((button<>(kPin, true, false, 10)), button<>::exception);
        CHECK(mock_timers_delete_count() == 1);
        mock_timers_reset();
    }
}

TEST_CASE("constructor reports xSemaphoreCreateBinary drain failures")
{
    settle_timers();
    mock_gpio_reset();
    mock_timers_reset();
    {
        // The drain semaphore creation fails after both timers succeeded:
        // the constructor must free the timers it created and throw.
        mock_timers_fail_x_queue_create(0);
        CHECK_THROWS_AS((button<>(kPin, true, false, 10)), button<>::exception);
        mock_timers_reset();
    }
}

TEST_CASE("add_longpress reports xTimerCreate failures")
{
    settle_timers();
    mock_gpio_reset();
    mock_timers_reset();
    {
        // The debounce and drain timers are created by the constructor
        // first; the long-press timer is the next xTimerCreate() call.
        mock_timers_fail_x_timer_create_after(2);
        button<1> btn(kPin, true, false, 10);
        CHECK_THROWS_AS(btn.add_longpress(30, [] {}), button<1>::exception);
        mock_timers_reset();
    }
}

TEST_CASE("a dropped long-press stop does not fire a spurious long-press")
{
    settle_timers();
    mock_gpio_reset();
    mock_gpio_set_level(kPin, 1);
    mock_timers_reset();
    {
        // Debouncing disabled: each edge evaluates the state immediately.
        button<1> btn(kPin, true, false, 0);
        std::atomic<size_t> long_presses{ 0 };
        btn.on_press([] {});
        btn.add_longpress(30, [&] { long_presses.fetch_add(1); });
        btn.start();

        // Press: the long-press timer is armed. Then make the disarm
        // (xTimerStop) fail, as it does when the timer command queue is
        // full: the callback runs on the daemon and cannot block on its own
        // command queue, so the stop is dropped and the timer stays armed.
        drive_pin(0);
        vTaskDelay(pdMS_TO_TICKS(10));
        mock_timers_fail_x_timer_stop(true);
        drive_pin(1);
        vTaskDelay(pdMS_TO_TICKS(60));
        mock_timers_fail_x_timer_stop(false);

        // The released button must not be reported as a long press.
        CHECK(long_presses.load() == 0);
        btn.stop();
    }
}

TEST_CASE("a long-press timer that survives stop does not fire")
{
    settle_timers();
    mock_gpio_reset();
    mock_gpio_set_level(kPin, 1);
    mock_timers_reset();
    {
        button<1> btn(kPin, true, false, 0);
        std::atomic<size_t> long_presses{ 0 };
        btn.on_press([] {});
        btn.add_longpress(30, [&] { long_presses.fetch_add(1); });
        btn.start();

        // Press, then stop with the stop commands dropped: the long-press
        // timer stays armed and fires after the button was stopped.
        drive_pin(0);
        vTaskDelay(pdMS_TO_TICKS(10));
        mock_timers_fail_x_timer_stop(true);
        btn.stop();
        vTaskDelay(pdMS_TO_TICKS(60));
        mock_timers_fail_x_timer_stop(false);

        CHECK(long_presses.load() == 0);
    }
}
#endif

TEST_CASE("stop waits for the debounce timer to expire")
{
    settle_timers();
    mock_gpio_reset();
    mock_gpio_set_level(kPin, 1);
    {
        button<> btn(kPin, true, false, 10);
        std::atomic<size_t> pressed{ 0 };
        btn.on_press([&] { pressed.fetch_add(1); });
        btn.start();

        drive_pin(0);
        // Give the debounce timer a chance to expire and run the callback
        // while stop() races against it: whatever the outcome, stop() must
        // return only after the daemon is done with the button.
        vTaskDelay(pdMS_TO_TICKS(5));
        btn.stop();
        settle_timers();
        CHECK(pressed.load() <= 1);
    }
}

TEST_CASE("stop waits for pended function calls when debouncing is disabled")
{
    settle_timers();
    mock_gpio_reset();
    mock_gpio_set_level(kPin, 1);
    {
        button<> btn(kPin, true, false, 0);
        std::atomic<size_t> pressed{ 0 };
        btn.on_press([&] {
            vTaskDelay(pdMS_TO_TICKS(20));
            pressed.fetch_add(1);
        });
        btn.start();

        drive_pin(0);
        // The pended callback blocks the daemon for a while: stop() must
        // wait for it to finish instead of leaving it running on a freed
        // object.
        btn.stop();
        CHECK(pressed.load() == 1);
    }
}

TEST_CASE("an interrupt in flight across stop is dropped")
{
    settle_timers();
    mock_gpio_reset();
    mock_gpio_set_level(kPin, 1);
    {
        button<> btn(kPin, true, false, 10);
        std::atomic<size_t> presses{ 0 };
        btn.on_press([&] { presses.fetch_add(1); });
        btn.start();

        // Capture the handler and its argument the way the GPIO driver would
        // have dispatched them; the handler may still be executing on the
        // other core when stop() removes it. Invoking it afterwards must not
        // re-arm the (stopped) debounce timer.
        gpio_isr_t handler = mock_gpio_isr_handler();
        void *arg = mock_gpio_isr_arg();
        REQUIRE(handler != nullptr);
        btn.stop();

        handler(arg);
        vTaskDelay(pdMS_TO_TICKS(30));
        CHECK(presses.load() == 0);
    }
}

TEST_CASE("a pended evaluation left over by stop is dropped")
{
    settle_timers();
    mock_gpio_reset();
    mock_gpio_set_level(kPin, 1);
    {
        // Debouncing disabled: a press would normally pend an evaluation.
        button<> btn(kPin, true, false, 0);
        std::atomic<size_t> presses{ 0 };
        btn.on_press([&] { presses.fetch_add(1); });
        btn.start();

        auto *isr_arg = static_cast<button_detail::button_isr_arg *>(mock_gpio_isr_arg());
        REQUIRE(isr_arg != nullptr);
        btn.stop();

        // Simulate the deferred evaluation of an edge that was dispatched
        // just before stop(): with the button stopped and the pin now
        // pressed it must still be dropped, not reported.
        mock_gpio_set_level(kPin, 0);
        isr_arg->pend(isr_arg->pend_arg, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        CHECK(presses.load() == 0);
    }
}

TEST_CASE("callback exceptions are contained and logged")
{
    settle_timers();
    mock_gpio_reset();
    mock_gpio_set_level(kPin, 1);
    {
        button<> btn(kPin, true, false, 0);
        std::atomic<size_t> calls{ 0 };
        btn.on_press([&] {
            calls.fetch_add(1);
            throw std::runtime_error("boom");
        });
        btn.start();

        drive_pin(0);
        // The pended function call would abort() if the exception escaped.
        vTaskDelay(pdMS_TO_TICKS(40));
        CHECK(calls.load() == 1);

        drive_pin(1);
        vTaskDelay(pdMS_TO_TICKS(40));
        btn.stop();
    }
}

TEST_CASE("non-std exceptions from callbacks are contained too")
{
    settle_timers();
    mock_gpio_reset();
    mock_gpio_set_level(kPin, 1);
    {
        button<> btn(kPin, true, false, 0);
        std::atomic<size_t> calls{ 0 };
        btn.on_press([&] {
            calls.fetch_add(1);
            throw 42;
        });
        btn.start();

        drive_pin(0);
        vTaskDelay(pdMS_TO_TICKS(40));
        CHECK(calls.load() == 1);

        drive_pin(1);
        vTaskDelay(pdMS_TO_TICKS(40));
        btn.stop();
    }
}
