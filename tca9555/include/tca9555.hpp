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
#include <map>
#include <memory>
#include <utility>

#include <esp_err.h>
#include <esp_log.h>

#include <driver/i2c_master.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/timers.h>

#include "task_queue.hpp"

/**
 * C++ driver for the TCA9555 16-bit I2C GPIO expander.
 *
 * Two 8-bit ports (Port 0 = P00-P07, Port 1 = P10-P17).
 *
 * I2C addresses: 0x20..0x27 (default 0x20, A2-A0 pins select).
 *
 * Threading model: instances are not thread-safe, guard concurrent access
 * externally. Every method runs its own I2C transactions and the single-bit
 * setters (pin_mode(), digital_write(), the single-pin set_polarity()) are
 * read-modify-write pairs, so two tasks updating different bits of the same
 * register (or the same register twice) can lose one of the two updates.
 */
class tca9555
{
    static constexpr const char *TAG = "tca9555";
    static constexpr uint32_t I2C_TIMEOUT_MS = 100;
    /* Attempts made to remove a device from the bus. The removal can fail
     * transiently (e.g. the bus is busy) and a handle that is dropped can
     * never be removed again, so retry it while it is still known. */
    static constexpr unsigned RM_DEVICE_ATTEMPTS = 3;

    static constexpr uint8_t REG_INPUT_0 = 0x00;
    static constexpr uint8_t REG_INPUT_1 = 0x01;
    static constexpr uint8_t REG_OUTPUT_0 = 0x02;
    static constexpr uint8_t REG_OUTPUT_1 = 0x03;
    static constexpr uint8_t REG_POLARITY_0 = 0x04;
    static constexpr uint8_t REG_POLARITY_1 = 0x05;
    static constexpr uint8_t REG_CONFIG_0 = 0x06;
    static constexpr uint8_t REG_CONFIG_1 = 0x07;

  public:
    /** Number of pins per port. */
    static constexpr uint8_t PINS_PER_PORT = 8;
    /** Total number of pins. */
    static constexpr uint8_t NUM_PINS = 16;

    /** Pin mask for one of the two 8-bit ports. */
    using port_mask = uint8_t;

    /** Identifies which port a pin belongs to. */
    enum class port : uint8_t
    {
        port0 = 0,
        port1 = 1
    };

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
                std::snprintf(msg_buf_,
                              sizeof(msg_buf_),
                              "%s: %s",
                              TAG,
                              msg);   // GCOVR_EXCL_BR_LINE
            }
        }
        ~exception() override = default;
        const char *what() const noexcept override   // GCOVR_EXCL_BR_LINE
        {
            return msg_buf_;   // GCOVR_EXCL_BR_LINE
        }

      private:
        char msg_buf_[128]{};
    };

    explicit tca9555(i2c_master_bus_handle_t bus_handle,
                     uint8_t addr = 0x20,
                     uint32_t scl_speed_hz = 100000)
    {
        if (bus_handle == nullptr) {
            throw exception("bus_handle is null");
        }
        if (addr < 0x20 || addr > 0x27) {
            throw exception("TCA9555 I2C address must be in range 0x20-0x27");
        }
        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr,
            .scl_speed_hz = scl_speed_hz,
            .scl_wait_us = 0,
            .flags = {},
        };
        esp_err_t err = i2c_master_bus_add_device(bus_handle, &cfg, &dev_);
        if (err != ESP_OK) {
            throw exception("i2c_master_bus_add_device failed", err);
        }
        ESP_LOGI(TAG, "TCA9555 at 0x%02X initialized", addr);
    }

    ~tca9555() noexcept
    {
        remove_device(dev_);
    }

    tca9555(const tca9555 &) = delete;
    tca9555 &operator=(const tca9555 &) = delete;

    tca9555(tca9555 &&other) noexcept
            : dev_{ std::exchange(other.dev_, nullptr) }   // GCOVR_EXCL_BR_LINE
    {
    }

    tca9555 &operator=(tca9555 &&other) noexcept
    {
        if (this != &other) {
            remove_device(dev_);
            dev_ = std::exchange(other.dev_, nullptr);   // GCOVR_EXCL_BR_LINE
        }
        return *this;   // GCOVR_EXCL_BR_LINE
    }

    /** Set direction of a single pin (false = output, true = input). */
    void pin_mode(port p, uint8_t pin, bool input)
    {
        if (pin >= PINS_PER_PORT) {
            throw exception("pin out of range");
        }
        uint8_t reg = config_reg(p);
        uint8_t val = read_reg(reg);
        if (input) {
            val |= (1 << pin);
        } else {
            val &= ~(1 << pin);
        }
        write_reg(reg, val);
    }

    /** Set direction of all pins on a port (1 = input, 0 = output). */
    void port_mode(port p, port_mask inputs)
    {
        write_reg(config_reg(p), inputs);   // GCOVR_EXCL_BR_LINE
    }

    /** Write a single pin high or low. */
    void digital_write(port p, uint8_t pin, bool high)
    {
        if (pin >= PINS_PER_PORT) {
            throw exception("pin out of range");
        }
        uint8_t reg = output_reg(p);
        uint8_t val = read_reg(reg);
        if (high) {
            val |= (1 << pin);
        } else {
            val &= ~(1 << pin);
        }
        write_reg(reg, val);
    }

    /** Write an entire output port at once. */
    void write_port(port p, port_mask val)
    {
        write_reg(output_reg(p), val);   // GCOVR_EXCL_BR_LINE
    }

    /** Read a single pin (true = high). */
    [[nodiscard]] bool digital_read(port p, uint8_t pin) const
    {
        if (pin >= PINS_PER_PORT) {
            throw exception("pin out of range");
        }
        return (read_reg(input_reg(p)) >> pin) & 1;   // GCOVR_EXCL_BR_LINE
    }

    /** Read all 8 pins of a port. */
    [[nodiscard]] port_mask read_port(port p) const
    {
        return read_reg(input_reg(p));   // GCOVR_EXCL_BR_LINE
    }

    /** Set polarity inversion mask for a port (inverts input bits). */
    void set_polarity(port p, port_mask mask)
    {
        write_reg(polarity_reg(p), mask);   // GCOVR_EXCL_BR_LINE
    }

    /** Set polarity inversion for a single pin. */
    void set_polarity(port p, uint8_t pin, bool invert)
    {
        if (pin >= PINS_PER_PORT) {
            throw exception("pin out of range");
        }
        uint8_t reg = polarity_reg(p);
        uint8_t val = read_reg(reg);
        if (invert) {
            val |= (1 << pin);
        } else {
            val &= ~(1 << pin);
        }
        write_reg(reg, val);
    }

  private:
    i2c_master_dev_handle_t dev_{};

    /**
     * Remove a device from the bus. The destructor and the move assignment
     * are noexcept, so a failure can only be logged; retrying it here is the
     * last chance to release the driver allocation, because a handle that is
     * dropped can never be removed again.
     */
    static void remove_device(i2c_master_dev_handle_t dev) noexcept
    {
        if (dev == nullptr) {
            return;
        }
        for (unsigned attempt = 0; attempt < RM_DEVICE_ATTEMPTS; ++attempt) {
            const esp_err_t err = i2c_master_bus_rm_device(dev);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "TCA9555 deinitialized");
                return;
            }
            char err_buf[32];
            esp_err_to_name_r(err, err_buf, sizeof(err_buf));
            ESP_LOGE(TAG,
                     "i2c_master_bus_rm_device failed (%u/%u): %s",
                     attempt + 1,
                     RM_DEVICE_ATTEMPTS,
                     err_buf);
        }
    }

    /* One register per port and register type, indexed by the port: the
     * helpers stay branch-free, which keeps them out of the coverage of the
     * callers they are inlined into. */
    static constexpr uint8_t CONFIG_REGS[2]{ REG_CONFIG_0, REG_CONFIG_1 };
    static constexpr uint8_t OUTPUT_REGS[2]{ REG_OUTPUT_0, REG_OUTPUT_1 };
    static constexpr uint8_t INPUT_REGS[2]{ REG_INPUT_0, REG_INPUT_1 };
    static constexpr uint8_t POLARITY_REGS[2]{ REG_POLARITY_0, REG_POLARITY_1 };

    [[nodiscard]] static constexpr uint8_t config_reg(port p)
    {
        return CONFIG_REGS[static_cast<uint8_t>(p)];   // GCOVR_EXCL_BR_LINE
    }

    [[nodiscard]] static constexpr uint8_t output_reg(port p)
    {
        return OUTPUT_REGS[static_cast<uint8_t>(p)];   // GCOVR_EXCL_BR_LINE
    }

    [[nodiscard]] static constexpr uint8_t input_reg(port p)
    {
        return INPUT_REGS[static_cast<uint8_t>(p)];   // GCOVR_EXCL_BR_LINE
    }

    [[nodiscard]] static constexpr uint8_t polarity_reg(port p)
    {
        return POLARITY_REGS[static_cast<uint8_t>(p)];   // GCOVR_EXCL_BR_LINE
    }

    /**
     * The device handle is null only in a moved-from object: every I/O
     * helper rejects it instead of handing a nullptr to the I2C driver.
     */
    void require_device() const
    {
        if (dev_ == nullptr) {
            throw exception("no I2C device (moved-from object)");
        }
    }

    [[nodiscard]] uint8_t read_reg(uint8_t reg) const
    {
        require_device();
        uint8_t val;
        esp_err_t err = i2c_master_transmit_receive(dev_, &reg, 1, &val, 1, I2C_TIMEOUT_MS);
        if (err != ESP_OK) {
            throw exception("read_reg failed", err);
        }
        return val;
    }

    void write_reg(uint8_t reg, uint8_t val)
    {
        require_device();
        uint8_t buf[2] = { reg, val };
        esp_err_t err = i2c_master_transmit(dev_, buf, sizeof(buf), I2C_TIMEOUT_MS);
        if (err != ESP_OK) {
            throw exception("write_reg failed", err);
        }
    }
};

/**
 * Button manager for all 16 pins of a TCA9555 GPIO expander.
 *
 * A single instance monitors all input pins (configured externally on the
 * TCA9555 via pin_mode/port_mode). It can be interrupt-driven: the
 * application must call handle_interrupt() from an external interrupt handler
 * (e.g. a GPIO ISR connected to the TCA9555 INT# pin). But also work in polling
 * mode with start_tick_timer(). FreeRTOS timers handle debouncing and long-press
 * detection.
 *
 * Only pins that have at least one callback configured (press, release or
 * long-press) are monitored: handle_interrupt() and tick() completely ignore
 * the other pins. Every event found by both paths is evaluated when the debounce
 * window expires (or immediately when it is disabled) and the callbacks are
 * posted to a task queue, whose worker task runs them.
 *
 * Threading model: the owner thread calls the constructor, start(), stop(),
 * start_tick_timer(), the event registration methods and handle_interrupt();
 * the FreeRTOS timer daemon task runs the debouncing and long-press logic and
 * the task queue worker runs the callbacks. The event registration methods
 * refuse to run once the object is started, but no lock serializes them (nor
 * start()/stop(), nor handle_interrupt()) against a concurrent call from
 * another task: guard those calls externally. User callbacks run on the worker
 * task of the task queue and must not call back into this object.
 *
 * stop() clears the started flag, drains the timer daemon, stops the timers,
 * drains the daemon again and finally drains the task queue, so no callback
 * can touch the object once it has returned. stop() and the destructor must
 * therefore never be called from a user callback (the task queue drain would
 * wait for the sentinel behind the callback that is running it) and the
 * destructor is only meaningful with the scheduler running (without it, no
 * callback can have been queued and nothing is drained).
 *
 * Template parameters:
 *   Tca9555T       – the instantiated tca9555 type
 *   MaxLongPresses – max simultaneous long-press thresholds per pin
 *                    (default: 0, i.e. long-press support is compiled out;
 *                    set it to 1 or more to enable long-press detection)
 *
 * Constructor arguments:
 *   ActiveLow   – default active-low setting for every pin; override per pin
 *                 with set_active_low() (default: true)
 *   DebounceMs  – debounce window in ms (default: 50).
 *                 Set to 0 to disable debouncing entirely: every pin
 *                 transition observed by handle_interrupt()/tick() is
 *                 evaluated immediately, without arming a timer. The
 *                 callback is still posted to the task queue, and because the
 *                 evaluation compares the level with the pin's current state,
 *                 several edges between two calls are coalesced into the net
 *                 level and a pulse that returns to the previous level is
 *                 dropped.
 *                 A window below the tick resolution (which would round down
 *                 to a zero-length timer period) is rejected when a callback
 *                 is registered for a pin.
 *   TaskQueue   – optional task_queue to run callbacks and ticks on. If not
 *                 given, a dedicated task_queue is created internally
 *                 (default: nullptr)
 */
template<typename Tca9555T, std::size_t MaxLongPresses = 0>
class tca9555_button
{
    static constexpr const char *TAG = "tca9555_button";
    static constexpr uint8_t PINS_PER_PORT = Tca9555T::PINS_PER_PORT;
    /* Delay between the drain() retries in stop() and in the destructor. */
    static constexpr uint32_t DRAIN_RETRY_MS = 10;
    /* Upper bound of those retries: an item that is still queued when the
     * bound is reached captures this object when the worker finally runs it,
     * so the bound is a last resort that turns a hang into a leak. Half a
     * second of retries is far more than a worker needs to consume one item,
     * while a queue that stays full longer (an item that blocks, or a broken
     * worker) would otherwise keep the owner blocked forever. */
    static constexpr unsigned DRAIN_MAX_ATTEMPTS = 50;

  public:
    using callback_t = std::function<void()>;
    using port = typename Tca9555T::port;

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
            return msg_buf_;
        }

      private:
        char msg_buf_[128]{};
    };

    explicit tca9555_button(std::shared_ptr<Tca9555T> dev,
                            bool active_low = true,
                            uint32_t debounce_ms = 50,
                            std::shared_ptr<task_queue<callback_t>> task_queue = nullptr)
            : dev_(std::move(dev))
            , task_queue_(task_queue != nullptr ?   // GCOVR_EXCL_BR_LINE
                                  std::move(task_queue) :
                                  std::make_shared<::task_queue<callback_t>>())
            , active_low_default_(active_low)
            , debounce_ms_(debounce_ms)
    {
        if (dev_ == nullptr) {
            throw exception("dev is null");
        }
        drain_timer_ = xTimerCreate("tca_drain",   // GCOVR_EXCL_BR_LINE
                                    1,             // GCOVR_EXCL_BR_LINE
                                    pdFALSE,       // GCOVR_EXCL_BR_LINE
                                    this,          // GCOVR_EXCL_BR_LINE
                                    drain_cb);     // GCOVR_EXCL_BR_LINE
        if (drain_timer_ == nullptr) {             // GCOVR_EXCL_BR_LINE
            // GCOVR_EXCL_START: drain timer creation failure (the drain is
            // the only thing created here: nothing to release)
            throw exception("xTimerCreate drain failed");
            // GCOVR_EXCL_STOP
        }
        drain_sem_ = xSemaphoreCreateBinary();   // GCOVR_EXCL_BR_LINE
        if (drain_sem_ == nullptr) {             // GCOVR_EXCL_BR_LINE
            // GCOVR_EXCL_START: drain semaphore creation failure
            delete_timer(drain_timer_);
            drain_timer_ = nullptr;
            throw exception("xSemaphoreCreate drain failed");
            // GCOVR_EXCL_STOP
        }
    }   // GCOVR_EXCL_BR_LINE: constructor EH cleanup edge

    ~tca9555_button() noexcept
    {
        stop();
        /* Nothing can post into the task queue anymore (the timers are
         * stopped and the daemon is drained by stop()): delete the timers and
         * drop what is still queued. Deleting waits for the timer command
         * queue rather than dropping the command, because a timer that
         * survives here keeps firing with a pointer to this object. */
        delete_timer(tick_timer_);
        tick_timer_ = nullptr;
        for (auto &entry : pins_) {
            pin_ctx &p = entry.second;
            delete_timer(p.debounce_timer_);
            if constexpr (MaxLongPresses > 0) {
                for (std::size_t j = 0; j < p.num_lp_; ++j) {
                    delete_timer(p.lp_timers_[j]);
                }
            }
        }
        drain_task_queue();
        delete_timer(drain_timer_);
        drain_timer_ = nullptr;
        if (drain_sem_ != nullptr) {   // GCOVR_EXCL_BR_LINE
            vSemaphoreDelete(drain_sem_);
            drain_sem_ = nullptr;
        }
    }   // GCOVR_EXCL_BR_LINE: destructor EH cleanup edge

    tca9555_button(const tca9555_button &) = delete;
    tca9555_button &operator=(const tca9555_button &) = delete;
    tca9555_button(tca9555_button &&) = delete;
    tca9555_button &operator=(tca9555_button &&) = delete;

    /**
     * Enable the button manager. Safe to call multiple times.
     *
     * The periodic tick timer is restarted if start_tick_timer() created one
     * before the previous stop() stopped it.
     */
    void start()
    {
        if (started_)
            return;
        started_ = true;
        if (tick_timer_ != nullptr) {
            // GCOVR_EXCL_START: needs a full timer command queue
            if (xTimerStart(tick_timer_, 0) != pdPASS) {
                ESP_LOGE(TAG, "timer command queue full, tick timer not restarted");
            }
            // GCOVR_EXCL_STOP
        }
    }

    /**
     * Disable the button manager. Safe to call multiple times.
     *
     * Once it returns, no callback can touch the object: the pending
     * evaluations are run (or dropped) by the task queue drain below.
     *
     * Must not be called from a user callback (see the class comment).
     */
    void stop() noexcept
    {
        if (!started_)
            return;
        started_ = false;
        /* Quiesce the daemon before stopping the timers: an evaluation that
         * is already running can still arm a long-press timer, and that arm
         * would otherwise be queued behind the xTimerStop() commands below
         * and leave the timer armed. The first drain waits for such an
         * evaluation to finish, the stops are then queued behind its timer
         * commands and the second drain waits for them to be processed. */
        wait_for_callbacks();
        stop_timers();
        wait_for_callbacks();
        drain_task_queue();
        for (auto &entry : pins_) {
            entry.second.stable_ = false;
        }
        /* The polling baseline is reset with the pin states: the first tick()
         * after a restart primes it again, so a level change that happens
         * while stopped is not mistaken for an already-known state. */
        tick_ready_ = false;
        last_raw_[0] = 0;
        last_raw_[1] = 0;
    }

    /**
     * Create a periodic timer to call tick() automatically.
     *
     * \param period_ms  Tick interval in milliseconds (at least one tick:
     *                   a shorter period is rejected).
     *                   The timer starts immediately.
     */
    void start_tick_timer(uint32_t period_ms)
    {
        if (!started_)
            throw exception("cannot start the tick timer before starting");
        const TickType_t ticks =
                timer_ticks(period_ms, "tick period is below the tick resolution");
        delete_timer(tick_timer_);
        tick_timer_ = xTimerCreate(   // GCOVR_EXCL_BR_LINE
                "tca_tick",
                ticks,
                pdTRUE,
                this,
                tick_timer_cb);                                  // GCOVR_EXCL_BR_LINE
        if (tick_timer_ == nullptr)                              // GCOVR_EXCL_BR_LINE
            throw exception("xTimerCreate tick timer failed");   // GCOVR_EXCL_LINE
        if (xTimerStart(tick_timer_, 0) != pdPASS) {             // GCOVR_EXCL_BR_LINE
            // GCOVR_EXCL_START: xTimerStart failure
            delete_timer(tick_timer_);
            tick_timer_ = nullptr;
            throw exception("xTimerStart tick timer failed");
            // GCOVR_EXCL_STOP
        }
    }

    /** Register press callback for a pin. Must be called before start(). */
    void on_press(port p, uint8_t pin, callback_t cb)
    {
        if (started_)
            throw exception("cannot set callback while started");
        if (pin >= PINS_PER_PORT)
            throw exception("pin out of range (0-7)");
        auto &c = get_pin(p, pin);
        ensure_debounce_timer(c);
        c.press_cb_ = std::move(cb);
        update_configured_mask(c, pin_index(p, pin));
    }

    /** Register release callback for a pin. Must be called before start(). */
    void on_release(port p, uint8_t pin, callback_t cb)
    {
        if (started_)
            throw exception("cannot set callback while started");
        if (pin >= PINS_PER_PORT)
            throw exception("pin out of range (0-7)");
        auto &c = get_pin(p, pin);
        ensure_debounce_timer(c);
        c.release_cb_ = std::move(cb);
        update_configured_mask(c, pin_index(p, pin));
    }

    /**
     * Add a long-press threshold for a pin.
     *
     * The callback fires once when the pin has been held for \p ms
     * without being released.
     *
     * Must be called before start().
     */
    void add_longpress(port p, uint8_t pin, uint32_t ms, callback_t cb)
    {
        if (started_)
            throw exception("cannot add long-press while started");
        if (pin >= PINS_PER_PORT)
            throw exception("pin out of range (0-7)");
        if constexpr (MaxLongPresses == 0) {
            throw exception("long-press not enabled (MaxLongPresses = 0)");
        } else {
            auto &c = get_pin(p, pin);
            if (c.num_lp_ >= MaxLongPresses)
                throw exception("maximum long-press thresholds reached");
            const TickType_t ticks =
                    timer_ticks(ms, "long-press threshold is below the tick resolution");
            ensure_debounce_timer(c);
            std::size_t idx = c.num_lp_;
            std::snprintf(c.lp_names_[idx].data(),
                          c.lp_names_[idx].size(),
                          "tca_lp_%u_%zu",
                          static_cast<unsigned>(pin_index(p, pin)),
                          idx);
            c.lp_cbs_[idx] = std::move(cb);
            c.lp_timers_[idx] = xTimerCreate(   // GCOVR_EXCL_BR_LINE
                    c.lp_names_[idx].data(),
                    ticks,
                    pdFALSE,
                    &c,
                    longpress_cb);                                   // GCOVR_EXCL_BR_LINE
            if (c.lp_timers_[idx] == nullptr) {                      // GCOVR_EXCL_BR_LINE
                c.lp_cbs_[idx] = nullptr;                            // GCOVR_EXCL_LINE
                throw exception("xTimerCreate long-press failed");   // GCOVR_EXCL_LINE
            }
            c.num_lp_++;
            update_configured_mask(c, pin_index(p, pin));
        }
    }

    /**
     * Configure whether a pin is active-low (true) or active-high (false).
     *
     * A pin is considered pressed when its raw level differs from this
     * setting. Each pin defaults to the value passed to the constructor.
     *
     * Must be called before start().
     */
    void set_active_low(port p, uint8_t pin, bool active_low)
    {
        if (started_)
            throw exception("cannot set active_low while started");
        if (pin >= PINS_PER_PORT)
            throw exception("pin out of range (0-7)");
        get_pin(p, pin).active_low_ = active_low;
    }

    /**
     * Interrupt handler - must be called externally by an interrupt
     * (e.g. a GPIO ISR connected to the TCA9555 INT# pin).
     *
     * Reads the input ports holding a monitored pin and resets the debounce
     * timer for any pin whose raw state changed.
     *
     * @note I2C operations are blocking and not strictly ISR-safe. On ESP-IDF,
     *       this should be called from a task context triggered by the ISR
     *       (e.g. via a semaphore or work queue) rather than directly in the ISR.
     */
    void handle_interrupt()
    {
        if (!started_)
            return;
        if (configured_mask_ == 0)
            return;

        uint8_t raw_ports[2];
        raw_ports[0] = read_port_masked(port::port0, port0_mask());
        raw_ports[1] = read_port_masked(port::port1, port1_mask());
        evaluate_ports(raw_ports);
    }

  private:
    /**
     * Polling fallback for when the interrupt line cannot be used.
     *
     * Called automatically when start_tick_timer() has been used. Reads the
     * input ports and, if any monitored pin has changed since the last tick,
     * runs the debounce logic with the values just read (no re-read).
     *
     * The level read by the first tick after start() (or after a stop()) is
     * the baseline: a pin that is already pressed when polling starts is not
     * reported (interrupt mode reports it, because it only knows the initial
     * "released" state).
     */
    void tick()
    {
        if (!started_)   // GCOVR_EXCL_BR_LINE
            return;
        if (configured_mask_ == 0)
            return;

        uint8_t raw_ports[2];
        raw_ports[0] = read_port_masked(port::port0, port0_mask());
        raw_ports[1] = read_port_masked(port::port1, port1_mask());

        if (!tick_ready_) {
            last_raw_[0] = raw_ports[0];
            last_raw_[1] = raw_ports[1];
            tick_ready_ = true;
            return;
        }

        if (raw_ports[0] != last_raw_[0] || raw_ports[1] != last_raw_[1]) {
            last_raw_[0] = raw_ports[0];
            last_raw_[1] = raw_ports[1];
            evaluate_ports(last_raw_);
        }
    }

    static void tick_timer_cb(TimerHandle_t xTimer) noexcept
    {
        auto *self = static_cast<tca9555_button *>(pvTimerGetTimerID(xTimer));
        if (self != nullptr)   // GCOVR_EXCL_BR_LINE
            self->queue_tick();
    }

    /** Queue a tick() on the task queue worker. */
    void queue_tick() noexcept
    {
        try {
            if (!task_queue_->post([this] { tick(); }))                // GCOVR_EXCL_BR_LINE
                ESP_LOGE(TAG, "task queue full, poll tick dropped");   // GCOVR_EXCL_BR_LINE
        } catch (...) {   // GCOVR_EXCL_START: post()'s move assignment threw
            ESP_LOGE(TAG, "task queue post failed");
        }   // GCOVR_EXCL_STOP
    }

    /**
     * Debounce logic shared by handle_interrupt() and tick().
     *
     * Resets the debounce timer for any configured pin whose raw state
     * changed, or evaluates the transition immediately when debouncing is
     * disabled (debounce_ms == 0). The caller supplies the port values so no
     * I2C transaction is repeated here.
     */
    void evaluate_ports(const uint8_t raw_ports[2])
    {
        for (auto &entry : pins_) {
            const uint8_t i = entry.first;
            if ((configured_mask_ & (1u << i)) == 0) {
                continue;
            }
            const bool raw = (raw_ports[i / PINS_PER_PORT] >> (i % PINS_PER_PORT)) & 1;
            pin_ctx &c = entry.second;

            if ((raw != c.active_low_) == c.stable_) {
                continue;
            }
            if (c.debounce_timer_ != nullptr) {
                // GCOVR_EXCL_START: needs a full timer command queue
                if (xTimerReset(c.debounce_timer_, 0) != pdPASS) {
                    ESP_LOGE(TAG, "timer command queue full, debounce not armed");
                }
                // GCOVR_EXCL_STOP
            } else {
                evaluate(c, raw);
            }
        }
    }

    struct pin_ctx
    {
        tca9555_button *owner_{};
        TimerHandle_t debounce_timer_{};
        callback_t press_cb_{};
        callback_t release_cb_{};
        std::array<TimerHandle_t, MaxLongPresses> lp_timers_{};
        std::array<callback_t, MaxLongPresses> lp_cbs_{};
        std::array<std::array<char, 24>, MaxLongPresses> lp_names_{};
        std::array<char, 16> name_{};
        uint8_t idx_{};
        uint8_t num_lp_{};
        bool stable_{};
        bool active_low_{};
    };

    /**
     * Post a callback to the task queue and report a dropped event.
     *
     * The callback is taken by reference: taking it by value would copy the
     * std::function (allocating whenever the stored callable does not fit the
     * small-buffer optimisation) on every event, and that copy is the only
     * thing here that can throw. post() needs its own copy; both are
     * contained because this also runs from the timer daemon task, where an
     * exception must not escape.
     */
    void run(const callback_t &cb) noexcept
    {
        if (cb == nullptr)   // GCOVR_EXCL_BR_LINE
            return;
        try {
            if (!task_queue_->post(cb))                            // GCOVR_EXCL_BR_LINE
                ESP_LOGE(TAG, "task queue full, event dropped");   // GCOVR_EXCL_BR_LINE
        } catch (...) {   // GCOVR_EXCL_START: post()'s move assignment threw
            ESP_LOGE(TAG, "task queue post failed");
        }   // GCOVR_EXCL_STOP
    }

    /**
     * Fire the press/release transition for a pin.
     *
     * \p raw is the pin level that triggered the evaluation: the immediate
     * path passes the level it just read, the debounce callback re-reads the
     * pin because the level can have moved back before the timer fired (the
     * return edge does not re-arm the timer). A stale evaluation is dropped
     * instead of toggling the state and firing a spurious callback.
     */
    void evaluate(pin_ctx &c, bool raw) noexcept
    {
        const bool now_pressed = (raw != c.active_low_);
        if (now_pressed == c.stable_) {
            return;
        }
        c.stable_ = now_pressed;
        if (now_pressed) {
            if constexpr (MaxLongPresses > 0) {
                for (std::size_t i = 0; i < c.num_lp_; ++i) {
                    TimerHandle_t t = c.lp_timers_[i];   // GCOVR_EXCL_BR_LINE
                    if (t != nullptr &&                  // GCOVR_EXCL_BR_LINE
                        xTimerReset(t, 0) != pdPASS)     // GCOVR_EXCL_BR_LINE
                    {
                        // GCOVR_EXCL_START
                        ESP_LOGE(TAG, "timer command queue full, long-press not armed");
                        // GCOVR_EXCL_STOP
                    }
                }
            }
            run(c.press_cb_);
        } else {
            if constexpr (MaxLongPresses > 0) {
                for (std::size_t i = 0; i < c.num_lp_; ++i) {
                    TimerHandle_t t = c.lp_timers_[i];   // GCOVR_EXCL_BR_LINE
                    if (t != nullptr &&                  // GCOVR_EXCL_BR_LINE
                        xTimerStop(t, 0) != pdPASS)      // GCOVR_EXCL_BR_LINE
                    {
                        // GCOVR_EXCL_START
                        ESP_LOGE(TAG, "timer command queue full, long-press still armed");
                        // GCOVR_EXCL_STOP
                    }
                }
            }
            run(c.release_cb_);
        }
    }

    /** Debounce timer callback - re-read the pin and evaluate it. */
    static void debounce_cb(TimerHandle_t xTimer) noexcept
    {
        auto *c = static_cast<pin_ctx *>(pvTimerGetTimerID(xTimer));
        if (c == nullptr || c->owner_ == nullptr)   // GCOVR_EXCL_BR_LINE
            return;                                 // GCOVR_EXCL_LINE
        c->owner_->read_and_evaluate(*c);
    }

    /** Read the pin of \p c and evaluate its transition. Runs in the timer
     *  daemon task, so the I2C read failure is contained here. */
    void read_and_evaluate(pin_ctx &c) noexcept
    {
        /* A debounce evaluation that the daemon had already dequeued when
         * stop() cleared the flag must not fire a callback: the timer that
         * armed it is stopped by stop() and the state it would use is being
         * reset there. */
        if (!started_)   // GCOVR_EXCL_BR_LINE
            return;      // GCOVR_EXCL_LINE
        // GCOVR_EXCL_START: the I2C read can fail; a failure must not unwind
        // through the kernel, so it is reported and the evaluation dropped
        try {
            evaluate(c, read_pin_level(c));
        } catch (const std::exception &e) {
            ESP_LOGE(TAG, "debounce: I2C read failed: %s", e.what());
        } catch (...) {
            ESP_LOGE(TAG, "debounce: I2C read failed");
        }
        // GCOVR_EXCL_STOP
    }

    static void longpress_cb(TimerHandle_t xTimer) noexcept
    {
        auto *c = static_cast<pin_ctx *>(pvTimerGetTimerID(xTimer));
        if (c == nullptr || c->owner_ == nullptr)   // GCOVR_EXCL_BR_LINE
            return;                                 // GCOVR_EXCL_LINE

        if constexpr (MaxLongPresses > 0) {
            for (std::size_t i = 0; i < c->num_lp_; ++i) {
                if (c->lp_timers_[i] == xTimer && c->lp_cbs_[i] != nullptr) {
                    c->owner_->run(c->lp_cbs_[i]);   // GCOVR_EXCL_BR_LINE
                    return;
                }
            }
        }
    }

    /** Drain marker expiry - see wait_for_callbacks(). */
    static void drain_cb(TimerHandle_t xTimer) noexcept
    {
        // Nothing to do: the expiry itself is the confirmation that the
        // daemon has processed every command queued before this one. Wake
        // the waiting owner thread, if any.
        auto *self = static_cast<tca9555_button *>(pvTimerGetTimerID(xTimer));
        if (self != nullptr) {   // GCOVR_EXCL_BR_LINE
            xSemaphoreGive(self->drain_sem_);
        }
    }

    std::shared_ptr<Tca9555T> dev_;
    std::shared_ptr<task_queue<callback_t>> task_queue_;
    bool active_low_default_{ true };
    uint32_t debounce_ms_{ 50 };
    std::map<uint8_t, pin_ctx> pins_;
    std::atomic<bool> started_{ false };
    uint16_t configured_mask_{};
    uint8_t last_raw_[2]{};
    bool tick_ready_{};
    TimerHandle_t tick_timer_{};
    TimerHandle_t drain_timer_{};
    SemaphoreHandle_t drain_sem_{};

    /** Recompute the configured-pin mask bit for \p pin after a callback change. */
    void update_configured_mask(const pin_ctx &c, uint8_t pin)
    {
        bool configured = static_cast<bool>(c.press_cb_) || static_cast<bool>(c.release_cb_);
        if constexpr (MaxLongPresses > 0) {
            for (std::size_t i = 0; i < c.num_lp_; ++i) {
                configured = configured || static_cast<bool>(c.lp_cbs_[i]);
            }
        }
        const uint16_t bit = static_cast<uint16_t>(1u << pin);
        if (configured) {
            configured_mask_ |= bit;
        } else {
            configured_mask_ &= static_cast<uint16_t>(~bit);
        }
    }

    /** Global index of a pin (0-7 on port 0, 8-15 on port 1). */
    [[nodiscard]] static constexpr uint8_t pin_index(port p, uint8_t pin)
    {
        return static_cast<uint8_t>(static_cast<uint8_t>(p) * PINS_PER_PORT + pin);
    }

    /** Mask of the configured pins of one port. */
    [[nodiscard]] uint8_t port0_mask() const
    {
        return static_cast<uint8_t>(configured_mask_ & 0xFF);
    }

    [[nodiscard]] uint8_t port1_mask() const
    {
        return static_cast<uint8_t>((configured_mask_ >> PINS_PER_PORT) & 0xFF);
    }

    /** Read one port, masked to the configured pins. An unconfigured port
     *  cannot hold a monitored pin and is not read at all, so polling and
     *  interrupt handling cost one I2C transaction per used port. */
    [[nodiscard]] uint8_t read_port_masked(port p, uint8_t mask) const
    {
        if (mask == 0) {
            return 0;
        }
        return static_cast<uint8_t>(dev_->read_port(p) & mask);   // GCOVR_EXCL_BR_LINE
    }

    /** Read the current level of the pin \p c refers to. */
    [[nodiscard]] bool read_pin_level(const pin_ctx &c) const
    {
        const auto p = static_cast<port>(c.idx_ / PINS_PER_PORT);
        return (dev_->read_port(p) >> (c.idx_ % PINS_PER_PORT)) & 1;
    }

    /** Convert a millisecond period into timer ticks, rejecting a value that
     *  rounds down to zero ticks: pdMS_TO_TICKS() computes the product in
     *  32-bit TickType_t arithmetic, so it wraps around for large periods and
     *  yields 0 for a period below the tick resolution, and FreeRTOS requires
     *  a strictly positive timer period. */
    [[nodiscard]] static TickType_t timer_ticks(uint32_t ms, const char *err_msg)
    {
        const uint64_t ticks = static_cast<uint64_t>(ms) * configTICK_RATE_HZ / 1000;
        if (ticks == 0) {
            throw exception(err_msg);
        }
        return static_cast<TickType_t>(ticks);
    }

    /**
     * Lazily create (if needed) and return the context for a pin.
     *
     * No timer is created here: a pin that is only touched by
     * set_active_low() is never monitored, so allocating a debounce timer
     * for it would waste a timer and load the (small) timer command queue.
     */
    pin_ctx &get_pin(port p, uint8_t pin)
    {
        const uint8_t idx = pin_index(p, pin);
        auto [it, created] = pins_.try_emplace(idx);
        pin_ctx &c = it->second;
        if (created) {
            c.owner_ = this;
            c.idx_ = idx;
            c.active_low_ = active_low_default_;
        }
        return c;
    }

    /** Create the debounce timer of \p c if debouncing is enabled and the
     *  timer does not exist yet. */
    void ensure_debounce_timer(pin_ctx &c)
    {
        if (debounce_ms_ == 0 || c.debounce_timer_ != nullptr) {
            return;
        }
        const TickType_t ticks =
                timer_ticks(debounce_ms_, "debounce_ms is below the tick resolution");
        std::snprintf(
                c.name_.data(), c.name_.size(), "tca_db_%u", static_cast<unsigned>(c.idx_));
        c.debounce_timer_ =                                    // GCOVR_EXCL_BR_LINE
                xTimerCreate(c.name_.data(),                   // GCOVR_EXCL_BR_LINE
                             ticks,                            // GCOVR_EXCL_BR_LINE
                             pdFALSE,                          // GCOVR_EXCL_BR_LINE
                             &c,                               // GCOVR_EXCL_BR_LINE
                             debounce_cb);                     // GCOVR_EXCL_BR_LINE
        if (c.debounce_timer_ == nullptr) {                    // GCOVR_EXCL_BR_LINE
            throw exception("xTimerCreate debounce failed");   // GCOVR_EXCL_LINE
        }
    }

    /** Stop one timer, waiting for space in the timer command queue: with a
     *  zero block time a full queue would drop the command and leave the
     *  timer armed. Runs from a task, never from the timer daemon. */
    static void stop_timer(TimerHandle_t timer) noexcept
    {
        if (timer == nullptr) {
            return;
        }
        if (xTimerStop(timer, portMAX_DELAY) != pdPASS) {   // GCOVR_EXCL_BR_LINE
            ESP_LOGE(TAG, "xTimerStop failed");             // GCOVR_EXCL_LINE
        }
    }

    /** Stop every timer created for this object. */
    void stop_timers() noexcept
    {
        stop_timer(tick_timer_);
        for (auto &entry : pins_) {
            pin_ctx &p = entry.second;
            stop_timer(p.debounce_timer_);
            if constexpr (MaxLongPresses > 0) {
                for (std::size_t j = 0; j < p.num_lp_; ++j) {
                    stop_timer(p.lp_timers_[j]);
                }
            }
        }
    }

    /** Delete one timer. Deleting is what makes the timer's callback
     *  impossible: a timer that survives here keeps firing with a pointer to
     *  an object that is being destroyed. */
    static void delete_timer(TimerHandle_t timer) noexcept
    {
        if (timer == nullptr) {
            return;
        }
        if (xTimerDelete(timer, portMAX_DELAY) != pdPASS) {   // GCOVR_EXCL_BR_LINE
            ESP_LOGE(TAG, "xTimerDelete failed");             // GCOVR_EXCL_LINE
        }
    }

    /**
     * Wait until the timer daemon task has processed everything queued for
     * this object, so that no debounce evaluation, long-press callback or
     * (expired) timer can touch it once this returns. The drain timer carries
     * no work: when it expires, every command queued before it has already
     * been processed, because timer commands are executed in FIFO order and
     * an expired timer is handled after the commands that precede it.
     *
     * Called with the scheduler running only (the timers can only be armed by
     * start()/start_tick_timer(), which require a running scheduler).
     */
    void wait_for_callbacks() noexcept
    {
        if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {   // GCOVR_EXCL_BR_LINE
            // Without a running scheduler the daemon has never executed
            // anything for this object, so there is nothing to wait for (and
            // blocking would be illegal anyway).
            return;   // GCOVR_EXCL_LINE
        }
        /* Clear a possible leftover token before arming the drain timer. */
        xSemaphoreTake(drain_sem_, 0);
        if (xTimerReset(drain_timer_, portMAX_DELAY) != pdPASS) {   // GCOVR_EXCL_BR_LINE
            // GCOVR_EXCL_START: only reachable without a running scheduler
            ESP_LOGE(TAG, "xTimerReset drain failed");
            return;
            // GCOVR_EXCL_STOP
        }
        xSemaphoreTake(drain_sem_, portMAX_DELAY);
    }

    /**
     * Run (or drop) everything the task queue holds, so that no item
     * capturing this object is left behind when it is destroyed or stopped.
     *
     * drain() throws while its sentinel cannot be enqueued (the queue is
     * full): the worker consumes the queued items and makes room again, so
     * retry a bounded number of times. Must not be called from an item that is
     * executed by this queue's own worker (the sentinel would never run), see
     * the class comment.
     */
    void drain_task_queue() noexcept
    {
        if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {   // GCOVR_EXCL_BR_LINE
            /* The worker task never ran: no item can have been queued (and
             * drain() would block forever). */
            return;   // GCOVR_EXCL_LINE
        }
        for (unsigned attempt = 0; attempt < DRAIN_MAX_ATTEMPTS; ++attempt) {
            try {
                task_queue_->drain();
                return;
            } catch (const std::exception &e) {   // GCOVR_EXCL_BR_LINE: drain() throws
                                                  // task_queue::exception only
                ESP_LOGW(TAG, "task_queue drain failed, retrying: %s", e.what());
            } catch (...) {   // GCOVR_EXCL_LINE: drain() throws std::exception only
                ESP_LOGW(TAG, "task_queue drain failed, retrying");   // GCOVR_EXCL_LINE
            }
            vTaskDelay(pdMS_TO_TICKS(DRAIN_RETRY_MS));
        }
        /* Giving up loses whatever is still queued, but a hang here would be
         * worse: an item cannot make room for the sentinel. */
        ESP_LOGE(TAG, "task queue did not drain, pending callbacks are lost");
    }
};
