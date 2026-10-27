/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

#include <esp_attr.h>
#include <esp_err.h>
#include <esp_log.h>

#include <driver/gpio.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/timers.h>

namespace button_detail {

/**
 * Default TaskQueue type for button: callbacks are invoked directly from
 * the FreeRTOS timer daemon task instead of being posted to a worker task.
 */
struct no_task_queue
{};

/**
 * Context passed to the GPIO ISR.
 *
 * It carries the debounce timer to reset on every edge (or the function to
 * pend when debouncing is disabled), the button's "active" flag and the
 * arguments they need. Kept as a plain struct so the ISR does not need to
 * touch private button members (xtensa toolchains misplace literals of
 * member functions, which breaks the link).
 */
struct button_isr_arg
{
    TimerHandle_t debounce_timer;
    void (*pend)(void *, uint32_t);
    void *pend_arg;
    const std::atomic<bool> *active;
};

/**
 * GPIO ISR - arms the debounce timer, or evaluates immediately when
 * debouncing is disabled (debounce_ms == 0).
 *
 * A free function instead of a member: xtensa toolchains place the literals
 * of member functions in flash, which breaks the link
 * ("dangerous relocation: l32r: literal placed after use").
 *
 * It only touches the button_isr_arg contents, never the button object
 * itself, and re-checks the button's active flag before arming anything:
 * stop() clears the flag before removing the handler, so an interrupt that
 * was dispatched just before the removal can still tell that the button is
 * being torn down and drop the edge. The handler must be inline: a static
 * function in a header would give every including translation unit its own
 * copy (and -Wunused-function noise in the ones that never create a button).
 */
inline void IRAM_ATTR
button_isr_handler(void *arg)
{
    auto *isr_arg = static_cast<button_isr_arg *>(arg);
    if (!isr_arg->active->load(std::memory_order_acquire)) {
        return;
    }
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (isr_arg->debounce_timer != nullptr) {
        const BaseType_t reset_ok =
                xTimerResetFromISR(isr_arg->debounce_timer, &xHigherPriorityTaskWoken);
        if (reset_ok == pdFALSE) {   // GCOVR_EXCL_BR_LINE
            // GCOVR_EXCL_START
            ESP_DRAM_LOGE(DRAM_STR("button_isr"),
                          "timer command queue full, interrupt callback dropped");
            // GCOVR_EXCL_STOP
        }
    } else {
        const BaseType_t pend_ok = xTimerPendFunctionCallFromISR(
                isr_arg->pend, isr_arg->pend_arg, 0, &xHigherPriorityTaskWoken);
        if (pend_ok == pdFALSE) {   // GCOVR_EXCL_BR_LINE
            // GCOVR_EXCL_START
            ESP_DRAM_LOGE(DRAM_STR("button_isr"),
                          "timer command queue full, interrupt callback dropped");
            // GCOVR_EXCL_STOP
        }
    }
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);   // GCOVR_EXCL_LINE
}

/**
 * Process-wide flag telling whether the GPIO ISR service is installed.
 *
 * The service is global to the GPIO driver and can be installed only once:
 * calling gpio_install_isr_service() again logs "GPIO isr service already
 * installed" as an error and returns ESP_ERR_INVALID_STATE. Every button
 * installs the service in start(), so from the second button on (or from the
 * second start() of the same button) that error would be printed for no
 * reason; the flag lets start() skip the call in that case.
 *
 * Returned by reference instead of being a plain local static so the tests
 * can clear it when they inject gpio_install_isr_service() failures.
 */
inline std::atomic<bool> &
isr_service_installed_state()
{
    static std::atomic<bool> installed{ false };
    return installed;
}

/**
 * Install the GPIO ISR service, at most once per process.
 *
 * Returns ESP_OK if an earlier call already installed it (the driver is not
 * called again, so nothing is logged) or the error reported by the driver.
 * ESP_ERR_INVALID_STATE means the service was installed by somebody else
 * (another driver, or another button racing this one from another task) and
 * is reported as a success as well: the service is there, which is all
 * start() needs. Failures are not remembered, so a later start() retries.
 *
 * The service is expected to stay installed: a caller uninstalling it behind
 * this wrapper's back would make the next gpio_isr_handler_add() fail.
 */
inline esp_err_t
install_isr_service()
{
    if (isr_service_installed_state().load(std::memory_order_acquire))
        return ESP_OK;
    const esp_err_t err = gpio_install_isr_service(0);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        isr_service_installed_state().store(true, std::memory_order_release);
        return ESP_OK;
    }
    return err;
}

}   // namespace button_detail

/**
 * Button handler - debounces a single push-button and provides
 * press, release and multiple long-press callbacks.
 *
 * The button logic (active-low vs active-high) and internal pull
 * configuration are set via constructor arguments.
 *
 * The GPIO interrupt is installed in start() and torn down in stop().
 * This avoids having the interrupt active during construction.
 * The GPIO ISR service itself is global and, being installable only once,
 * is installed by the first start() of the process and then left installed:
 * later starts (of this or any other button) only add their own handler.
 * No error is logged by the redundant gpio_install_isr_service() calls
 * anymore.
 *
 * Threading model: the owner thread calls the constructor, start(), stop(),
 * on_press(), on_release() and add_longpress(); the FreeRTOS timer daemon
 * task runs the debouncing and long-press logic and invokes the user
 * callbacks (directly, or via the task queue when one is provided).
 * on_press(), on_release() and add_longpress() must not race with start()
 * or stop(): they refuse to run once the button is started, but no lock
 * serializes them against a concurrent start()/stop() on another thread.
 * User callbacks run on the timer daemon task (or the worker task of the
 * provided task queue) and must not call back into the button object.
 *
 * stop() clears the ISR "active" flag, removes the GPIO handler and then
 * drains the timer daemon task twice, with the timers stopped in between:
 * every command and every pending evaluation queued for this button has been
 * processed by the time it returns. Draining before stopping the timers
 * makes a debounce evaluation that was already running when the flag was
 * cleared order its long-press resets before the stops, instead of behind
 * them; if one still slips through, the long-press callback re-checks the
 * flag and drops it. No callback can therefore touch the object after stop()
 * returns. stop() and the destructor must therefore never be called from the
 * timer daemon task or from a user callback (the drains would deadlock) and
 * the free of the timers in the destructor relies on the drain, not on the
 * block time of xTimerDelete().
 *
 * Residual risk: the GPIO driver removes the handler under a spinlock but
 * does not wait for an ISR that is already running, so an interrupt taken by
 * the other CPU just before stop() removed the handler can still be
 * executing when the destructor frees the timers. The re-checked active flag
 * makes such an ISR drop the edge unless it already read the flag as set,
 * and the window is bounded by the (short, non-blocking) ISR body, but there
 * is no driver API to acknowledge an in-flight ISR: the object's timers must
 * not be freed while an edge could still be serviced (stopping from a task
 * that shares the CPU the GPIO interrupt is routed to, or disabling the
 * interrupt on that CPU before the removal, would close it completely).
 *
 * Constructor arguments:
 *   gpio_pin    – the GPIO number to monitor
 *   active_low  – true if the button reads low when pressed (default: true)
 *   should_pull – enable internal pull resistor (default: false).
 *                 When true and active_low is true → pull-up;
 *                 when true and active_low is false → pull-down.
 *   debounce_ms – debounce window in ms (default: 50).
 *                 Set to 0 to disable debouncing entirely: every edge pends
 *                 an evaluation instead of arming a timer. A pended
 *                 evaluation reads the level when the daemon runs it, so it
 *                 still coalesces: two edges closer together than the
 *                 dispatch delay are seen only at their final level, and a
 *                 pulse that is over before the evaluation runs is dropped.
 *   task_queue  – optional task_queue to run callbacks on instead of
 *                 invoking them directly from the timer context.
 *                 Pass the task_queue type as the TaskQueue template
 *                 parameter and its shared_ptr as the last constructor
 *                 argument (default: nullptr)
 *
 * Template parameters:
 *   MaxLongPresses – maximum simultaneous long-press thresholds
 *                    (default: 0, which disables long-press support and
 *                    lets the compiler eliminate all long-press code)
 *   TaskQueue      – task_queue type used to run callbacks. Defaults to
 *                    button_detail::no_task_queue, which invokes callbacks
 *                    directly from the timer context. A task_queue is only
 *                    instantiated when a non-default type is used, so the
 *                    button component does not depend on task_queue.
 */
template<size_t MaxLongPresses = 0, typename TaskQueue = button_detail::no_task_queue>
class button
{
    static constexpr const char *TAG = "button";

  public:
    class exception final : public std::exception
    {
      public:
        explicit exception(const char *msg, esp_err_t err = ESP_OK) noexcept
        {
            if (err != ESP_OK) {
                char err_buf[32];
                esp_err_to_name_r(err, err_buf, sizeof(err_buf));
                std::snprintf(msg_buf_, sizeof(msg_buf_), "%s: %s: %s", TAG, msg, err_buf);
            } else {
                std::snprintf(msg_buf_, sizeof(msg_buf_), "%s: %s", TAG, msg);
            }
        }
        ~exception() override = default;
        const char *what() const noexcept override   // GCOVR_EXCL_BR_LINE
        {
            return msg_buf_;
        }

      private:
        char msg_buf_[128]{};
    };

    using callback_t = std::function<void()>;

    button(gpio_num_t gpio_pin,
           bool active_low = true,
           bool should_pull = false,
           uint32_t debounce_ms = 50,
           std::shared_ptr<TaskQueue> task_queue = nullptr)
            : gpio_pin_(validate_pin(gpio_pin))
            , active_low_(active_low)
            , should_pull_(should_pull)
            , task_queue_(std::move(task_queue))
            , debounce_timer_(
                      debounce_ms > 0   // GCOVR_EXCL_BR_LINE
                              ?
                              xTimerCreate("btn_db",                      // GCOVR_EXCL_BR_LINE
                                           debounce_ticks(debounce_ms),   // GCOVR_EXCL_BR_LINE
                                           pdFALSE,
                                           this,          // GCOVR_EXCL_BR_LINE
                                           debounce_cb)   // GCOVR_EXCL_BR_LINE
                              :
                              nullptr)
            , longpress_timers_{}
            , longpress_cbs_{}
            , longpress_names_{}
            , num_longpresses_(0)
            , press_cb_(nullptr)     // GCOVR_EXCL_BR_LINE
            , release_cb_(nullptr)   // GCOVR_EXCL_BR_LINE
            , pressed_(false)        // GCOVR_EXCL_BR_LINE
            , started_(false)        // GCOVR_EXCL_BR_LINE
    {
        drain_timer_ = xTimerCreate("btn_drain",   // GCOVR_EXCL_BR_LINE
                                    1,             // GCOVR_EXCL_BR_LINE
                                    pdFALSE,       // GCOVR_EXCL_BR_LINE
                                    this,          // GCOVR_EXCL_BR_LINE
                                    drain_cb);     // GCOVR_EXCL_BR_LINE
        if (drain_timer_ == nullptr) {             // GCOVR_EXCL_BR_LINE
            // GCOVR_EXCL_START: drain timer creation failure
            if (debounce_timer_ != nullptr)
                delete_timer(debounce_timer_);
            throw exception("xTimerCreate drain failed");
            // GCOVR_EXCL_STOP
        }
        drain_sem_ = xSemaphoreCreateBinary();   // GCOVR_EXCL_BR_LINE
        if (drain_sem_ == nullptr) {             // GCOVR_EXCL_BR_LINE
            // GCOVR_EXCL_START: drain semaphore creation failure
            delete_timer(drain_timer_);
            drain_timer_ = nullptr;
            if (debounce_timer_ != nullptr)
                delete_timer(debounce_timer_);
            throw exception("xSemaphoreCreate drain failed");
            // GCOVR_EXCL_STOP
        }
        if (debounce_ms > 0 && debounce_timer_ == nullptr) {   // GCOVR_EXCL_BR_LINE
            // GCOVR_EXCL_START: debounce timer creation failure; the drain
            // timer and semaphore created above must be released here, the
            // destructor is not run for an object whose constructor throws.
            delete_timer(drain_timer_);
            drain_timer_ = nullptr;
            vSemaphoreDelete(drain_sem_);
            drain_sem_ = nullptr;
            throw exception("xTimerCreate debounce failed");
            // GCOVR_EXCL_STOP
        }
        // The active flag is what the ISR checks before touching the
        // debounce timer: it is only meaningful once start() installed the
        // handler, and stop() clears it before removing the handler.
        isr_arg_ = { debounce_timer_, &button::debounce_pend, this, &active_ };
    }   // GCOVR_EXCL_BR_LINE: constructor EH cleanup edge

    ~button() noexcept
    {
        stop();

        if (debounce_timer_ != nullptr) {   // GCOVR_EXCL_BR_LINE
            delete_timer(debounce_timer_);
        }

        if constexpr (MaxLongPresses != 0) {
            for (size_t i = 0; i < num_longpresses_; ++i) {
                if (longpress_timers_[i] != nullptr)   // GCOVR_EXCL_BR_LINE
                    delete_timer(longpress_timers_[i]);
            }
        }

        if (drain_timer_ != nullptr) {   // GCOVR_EXCL_BR_LINE
            delete_timer(drain_timer_);
        }
        if (drain_sem_ != nullptr) {   // GCOVR_EXCL_BR_LINE
            vSemaphoreDelete(drain_sem_);
        }
    }   // GCOVR_EXCL_BR_LINE: destructor EH cleanup edge

    button(const button &) = delete;
    button &operator=(const button &) = delete;
    button(button &&) = delete;
    button &operator=(button &&) = delete;

    /** Start the GPIO interrupt. Safe to call multiple times. */
    void start()
    {
        if (started_)
            return;
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << static_cast<unsigned>(gpio_pin_),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = (!should_pull_ ? GPIO_PULLUP_DISABLE :
                           active_low_   ? GPIO_PULLUP_ENABLE :
                                           GPIO_PULLUP_DISABLE),
            .pull_down_en = (!should_pull_ ? GPIO_PULLDOWN_DISABLE :
                             active_low_   ? GPIO_PULLDOWN_DISABLE :
                                             GPIO_PULLDOWN_ENABLE),
            .intr_type = GPIO_INTR_ANYEDGE,
        };
        esp_err_t err = gpio_config(&io_conf);
        if (err != ESP_OK)
            throw exception("gpio_config failed", err);
        err = button_detail::install_isr_service();
        if (err != ESP_OK) {
            gpio_reset_pin(gpio_pin_);
            throw exception("gpio_install_isr_service failed", err);
        }
        // Mark the button active before the handler is added: the handler
        // cannot run before gpio_isr_handler_add() enables the pin interrupt,
        // and an edge latched while adding it must find the flag already set.
        active_.store(true, std::memory_order_release);
        stopped_.store(false, std::memory_order_release);
        err = gpio_isr_handler_add(gpio_pin_, button_detail::button_isr_handler, &isr_arg_);
        if (err != ESP_OK) {
            active_.store(false, std::memory_order_release);
            stopped_.store(true, std::memory_order_release);
            gpio_reset_pin(gpio_pin_);
            throw exception("gpio_isr_handler_add failed", err);
        }
        started_ = true;
    }

    /** Stop the GPIO interrupt. Safe to call multiple times. */
    void stop() noexcept
    {
        if (!started_)
            return;
        // Clear the flag first: it makes the ISR drop any further edge and
        // the long-press callback drop a timer that is still armed, so
        // nothing can fire after the drains below.
        active_.store(false, std::memory_order_release);
        esp_err_t err = gpio_isr_handler_remove(gpio_pin_);
        if (err != ESP_OK) {
            char err_buf[32];
            esp_err_to_name_r(err, err_buf, sizeof(err_buf));   // GCOVR_EXCL_BR_LINE
            ESP_LOGE(TAG, "gpio_isr_handler_remove failed: %s", err_buf);   // GCOVR_EXCL_LINE
        }
        // Quiesce the daemon before stopping the timers: a debounce
        // evaluation that is already running when the flag is cleared has
        // read it as set and can still arm a long-press timer, and that arm
        // would otherwise be queued behind the xTimerStop() commands below
        // and leave the timer armed. The first drain waits for such an
        // evaluation to finish; the timers are then stopped and the second
        // drain waits for the stop commands to be processed.
        wait_for_callbacks();
        stop_timers();
        wait_for_callbacks();
        pressed_.store(false, std::memory_order_relaxed);
        // Only now is a callback impossible: the drains have processed
        // everything queued before the flag was cleared and the timers are
        // stopped, so a later evaluation can only be a stray one and bails.
        stopped_.store(true, std::memory_order_release);
        started_ = false;
    }

    /** Register callback for button press (after debounce). */
    void on_press(callback_t cb)
    {
        if (started_)
            throw exception("cannot set callback while button is started");
        press_cb_ = std::move(cb);
    }

    /** Register callback for button release (after debounce). */
    void on_release(callback_t cb)
    {
        if (started_)
            throw exception("cannot set callback while button is started");
        release_cb_ = std::move(cb);
    }

    /**
     * Add a long-press threshold.
     *
     * The callback fires once when the button has been held for \p ms
     * without being released.
     *
     * \throws exception if the button is started, no slots are available,
     *         or timer creation fails.
     */
    void add_longpress(uint32_t ms, callback_t cb)
    {
        if constexpr (MaxLongPresses == 0) {
            throw exception("long-press support disabled (MaxLongPresses == 0)");
        } else {
            if (started_)
                throw exception("cannot add long-press while button is started");

            if (num_longpresses_ >= MaxLongPresses)
                throw exception("maximum number of long-press thresholds reached");

            size_t idx = num_longpresses_;

            // Convert before touching the slot: longpress_ticks() may throw
            // (threshold below the tick resolution) and the slot must not be
            // left holding a callback that was never registered.
            const TickType_t ticks = longpress_ticks(ms);

            std::snprintf(longpress_names_[idx].data(),
                          longpress_names_[idx].size(),
                          "btn_lp%d_%zu",
                          gpio_pin_,
                          idx);

            longpress_cbs_[idx] = std::move(cb);
            longpress_timers_[idx] = xTimerCreate(   // GCOVR_EXCL_BR_LINE
                    longpress_names_[idx].data(),    // GCOVR_EXCL_BR_LINE
                    ticks,
                    pdFALSE,
                    this,                                            // GCOVR_EXCL_BR_LINE
                    longpress_cb);                                   // GCOVR_EXCL_BR_LINE
            if (longpress_timers_[idx] == nullptr) {                 // GCOVR_EXCL_BR_LINE
                longpress_cbs_[idx] = nullptr;                       // GCOVR_EXCL_LINE
                throw exception("xTimerCreate long-press failed");   // GCOVR_EXCL_LINE
            }

            num_longpresses_ = idx + 1;
        }
    }

  private:
    gpio_num_t gpio_pin_;
    bool active_low_;
    bool should_pull_;
    std::shared_ptr<TaskQueue> task_queue_;
    TimerHandle_t debounce_timer_;
    TimerHandle_t drain_timer_;
    SemaphoreHandle_t drain_sem_;
    button_detail::button_isr_arg isr_arg_{};
    std::array<TimerHandle_t, MaxLongPresses> longpress_timers_{};
    std::array<callback_t, MaxLongPresses> longpress_cbs_{};
    std::array<std::array<char, 24>, MaxLongPresses> longpress_names_{};
    size_t num_longpresses_;
    callback_t press_cb_;
    callback_t release_cb_;
    // Shared with the ISR and the daemon task: atomic. True while the button
    // is started and the debounce timer may still be armed.
    std::atomic<bool> active_{ false };
    // Set once stop() has drained the daemon, so a debounce evaluation that a
    // stray (in-flight ISR) reset re-arms after stop() returned bails out
    // instead of firing callbacks. Cleared again by start().
    std::atomic<bool> stopped_{ true };
    // Only written by the daemon and by stop(); longpress_cb() reads it (and
    // is ordered after active_) to drop a long-press whose stop command was
    // dropped, so it is atomic too.
    std::atomic<bool> pressed_;
    bool started_;

    /** Check the pin number before shifting it into a bit mask. */
    static gpio_num_t validate_pin(gpio_num_t pin)
    {
        if (!GPIO_IS_VALID_GPIO(pin))   // GCOVR_EXCL_BR_LINE
            throw exception("invalid GPIO pin");
        return pin;
    }

    /**
     * Convert milliseconds to a usable timer period. The conversion is done
     * in 64-bit arithmetic on purpose: pdMS_TO_TICKS() multiplies in
     * TickType_t, which is 32-bit on ESP32, so a period above
     * portMAX_DELAY / configTICK_RATE_HZ * 1000 ms wraps around and silently
     * yields an arbitrary (usually shorter) period. A zero result (rounding a
     * short interval below the tick resolution, or ms == 0) is invalid for
     * FreeRTOS timers and would silently disable the delay, so it is reported
     * as an error. No upper-bound check is needed: configTICK_RATE_HZ is at
     * most 1000, so the result never exceeds a uint32_t ms input, i.e. it
     * always fits in a 32-bit TickType_t.
     */
    static TickType_t debounce_ticks(uint32_t ms)
    {
        const uint64_t ticks = static_cast<uint64_t>(ms) * configTICK_RATE_HZ / 1000;
        if (ticks == 0)
            throw exception("debounce_ms is below the tick resolution");
        return static_cast<TickType_t>(ticks);
    }

    /** Same as debounce_ticks() for long-press thresholds. */
    static TickType_t longpress_ticks(uint32_t ms)
    {
        const uint64_t ticks = static_cast<uint64_t>(ms) * configTICK_RATE_HZ / 1000;
        if (ticks == 0)
            throw exception("long-press threshold is below the tick resolution");
        return static_cast<TickType_t>(ticks);
    }

    /**
     * Delete a timer. The block time only bounds the wait for space in the
     * timer command queue: xTimerDelete() returns once the delete command has
     * been enqueued, not once the daemon has processed it, so this does not by
     * itself guarantee that the timer's callback can no longer fire (the
     * daemon processes expired timers before the queued delete). The timers
     * are made safe to delete by stop()'s drain handshake, not by this wait.
     */
    static void delete_timer(TimerHandle_t timer) noexcept
    {
        if (xTimerDelete(timer, portMAX_DELAY) != pdPASS) {   // GCOVR_EXCL_BR_LINE
            ESP_LOGE(TAG, "xTimerDelete failed");             // GCOVR_EXCL_LINE
        }
    }

    /**
     * Run a callback directly or post it to the task queue if provided.
     * Takes the callback by reference: taking it by value would copy the
     * std::function (possibly allocating) on every event. Only the post()
     * path needs an owned copy, and that copy is the only thing here that can
     * throw, so it is contained instead of terminating through noexcept.
     */
    void run(const callback_t &cb) noexcept   // GCOVR_EXCL_BR_LINE
    {
        if (cb == nullptr)   // GCOVR_EXCL_BR_LINE
            return;
        if constexpr (std::is_same_v<TaskQueue, button_detail::no_task_queue>) {
            invoke(cb);
        } else {
            if (task_queue_ != nullptr) {
                // GCOVR_EXCL_START: the copy post() needs can throw
                // std::bad_alloc; fail the event instead of terminating.
                try {
                    task_queue_->post(cb);
                } catch (const std::exception &e) {
                    ESP_LOGE(TAG, "task queue post failed: %s", e.what());
                } catch (...) {
                    ESP_LOGE(TAG, "task queue post failed");
                }
                // GCOVR_EXCL_STOP
            } else {
                invoke(cb);
            }
        }
    }

    /** Runs state evaluation from the timer daemon task when debouncing is
     *  disabled, so callbacks are never invoked from ISR context. */
    static void debounce_pend(void *arg, uint32_t) noexcept
    {
        auto *self = static_cast<button *>(arg);
        self->evaluate_state();
    }

    /** Debounce timer callback - evaluates the stable pin state. */
    static void debounce_cb(TimerHandle_t xTimer) noexcept
    {
        auto *self = static_cast<button *>(pvTimerGetTimerID(xTimer));
        if (self == nullptr)   // GCOVR_EXCL_BR_LINE
            return;
        self->evaluate_state();
    }

    /** Read the pin and fire callbacks if the pressed state changed. */
    void evaluate_state() noexcept
    {
        if (stopped_.load(std::memory_order_acquire))
            return;

        int level = gpio_get_level(gpio_pin_);
        bool now_pressed = (level == (active_low_ ? 0 : 1));

        if (now_pressed != pressed_.load(std::memory_order_relaxed)) {
            pressed_.store(now_pressed, std::memory_order_relaxed);
            if (now_pressed) {
                // Arm the long-press timers before running the press
                // callback: a callback that runs on a worker task delays the
                // arming (and therefore the long-press window) otherwise.
                // stop() stops the timers after draining this callback, so a
                // timer armed here is still stopped before stop() returns.
                if constexpr (MaxLongPresses != 0) {
                    for (size_t i = 0; i < num_longpresses_; ++i) {
                        auto *t = longpress_timers_[i];    // GCOVR_EXCL_BR_LINE
                        if (t != nullptr &&                // GCOVR_EXCL_BR_LINE
                            xTimerReset(t, 0) != pdPASS)   // GCOVR_EXCL_BR_LINE
                        {
                            // GCOVR_EXCL_START
                            ESP_LOGE(TAG, "timer command queue full, long-press dropped");
                            // GCOVR_EXCL_STOP
                        }
                    }
                }
                run(press_cb_);   // GCOVR_EXCL_BR_LINE
            } else {
                if constexpr (MaxLongPresses != 0) {
                    for (size_t i = 0; i < num_longpresses_; ++i) {
                        auto *t = longpress_timers_[i];   // GCOVR_EXCL_BR_LINE
                        if (t != nullptr &&               // GCOVR_EXCL_BR_LINE
                            xTimerStop(t, 0) != pdPASS)   // GCOVR_EXCL_BR_LINE
                        {
                            // GCOVR_EXCL_START
                            ESP_LOGE(TAG, "timer command queue full, long-press still armed");
                            // GCOVR_EXCL_STOP
                        }
                    }
                }
                run(release_cb_);   // GCOVR_EXCL_BR_LINE
            }
        }
    }

    /**
     * Long-press timer callback - fires once after held for threshold.
     */
    static void longpress_cb(TimerHandle_t xTimer) noexcept
    {
        if constexpr (MaxLongPresses == 0)
            return;

        auto *self = static_cast<button *>(pvTimerGetTimerID(xTimer));
        if (self == nullptr)   // GCOVR_EXCL_BR_LINE
            return;
        if (!self->active_.load(std::memory_order_acquire))   // GCOVR_EXCL_BR_LINE
            return;
        if (!self->pressed_.load(std::memory_order_relaxed))   // GCOVR_EXCL_BR_LINE
            return;

        /* Find which longpress timer fired */
        for (size_t i = 0; i < self->num_longpresses_; ++i) {   // GCOVR_EXCL_BR_LINE
            if (self->longpress_timers_[i] == xTimer &&         // GCOVR_EXCL_BR_LINE
                self->longpress_cbs_[i] != nullptr) {           // GCOVR_EXCL_BR_LINE
                self->run(self->longpress_cbs_[i]);             // GCOVR_EXCL_BR_LINE
                return;
            }
        }
    }

    /**
     * Stop the debounce and long-press timers. Called from stop() only, i.e.
     * from a task that is not the timer daemon: the portMAX_DELAY block time
     * makes the command wait for space in the timer command queue instead of
     * being dropped when it is full, so a stop cannot be silently lost (the
     * daemon itself must not block on that queue, which is why
     * evaluate_state() uses a zero block time).
     */
    void stop_timers() noexcept
    {
        if (debounce_timer_ != nullptr &&                   // GCOVR_EXCL_BR_LINE
            xTimerStop(debounce_timer_, portMAX_DELAY) ==   // GCOVR_EXCL_BR_LINE
                    pdFALSE)                                // GCOVR_EXCL_BR_LINE
        {
            // GCOVR_EXCL_START
            ESP_LOGE(TAG, "xTimerStop debounce failed");
            // GCOVR_EXCL_STOP
        }
        if constexpr (MaxLongPresses != 0) {
            for (size_t i = 0; i < num_longpresses_; ++i) {
                auto *t = longpress_timers_[i];                  // GCOVR_EXCL_BR_LINE
                if (t != nullptr &&                              // GCOVR_EXCL_BR_LINE
                    xTimerStop(t, portMAX_DELAY) == pdFALSE) {   // GCOVR_EXCL_BR_LINE
                    // GCOVR_EXCL_START
                    ESP_LOGE(TAG, "xTimerStop long-press failed");
                    // GCOVR_EXCL_STOP
                }
            }
        }
    }

    /**
     * Wait until the timer daemon task has processed everything queued for
     * this button, so that no debounce evaluation, long-press callback or
     * pended function call can touch the object once this returns. The drain
     * timer carries no work: when it expires, every command queued before it
     * has already been dequeued by the daemon, because FreeRTOS timer
     * commands are executed in FIFO order.
     *
     * Called with the scheduler running only (started_ implies a start()
     * call, which cannot complete before the scheduler runs).
     */
    void wait_for_callbacks() noexcept
    {
        if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {   // GCOVR_EXCL_BR_LINE
            // Without a running scheduler the daemon has never executed
            // anything for this button, so there is nothing to wait for
            // (and blocking would be illegal anyway).
            return;
        }
        // Clear a possible leftover token before arming the drain timer.
        xSemaphoreTake(drain_sem_, 0);
        if (xTimerReset(drain_timer_, portMAX_DELAY) != pdPASS) {   // GCOVR_EXCL_BR_LINE
            // GCOVR_EXCL_START: drain timer arm failure
            ESP_LOGE(TAG, "xTimerReset drain failed");
            return;
            // GCOVR_EXCL_STOP
        }
        xSemaphoreTake(drain_sem_, portMAX_DELAY);
    }

    /** Drain marker expiry - see wait_for_callbacks(). */
    static void drain_cb(TimerHandle_t xTimer) noexcept
    {
        // Nothing to do: the expiry itself is the confirmation that the
        // daemon has processed every command queued before this one. Wake
        // the waiting owner thread, if any.
        auto *self = static_cast<button *>(pvTimerGetTimerID(xTimer));
        if (self != nullptr) {   // GCOVR_EXCL_BR_LINE
            xSemaphoreGive(self->drain_sem_);
        }
    }

    /** Dispatch a callback, containing any exception it may throw. */
    void invoke(const callback_t &cb) noexcept
    {
        try {   // GCOVR_EXCL_START
            cb();
        } catch (const std::exception &e) {
            ESP_LOGE(TAG, "callback threw: %s", e.what());
        } catch (...) {
            ESP_LOGE(TAG, "callback threw an unknown exception");
        }   // GCOVR_EXCL_STOP
    }
};
