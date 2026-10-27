/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <limits>
#include <utility>

#include <sys/time.h>

#include <esp_err.h>
#include <esp_log.h>

#include <driver/i2c_master.h>

/**
 * C++ driver for the NXP PCF85063A real-time clock (RTC) over I2C.
 *
 * I2C address: 0x51.
 *
 * Time registers hold BCD-coded values; the year register counts from
 * YEAR_OFFSET (1970).
 *
 * Weekday (dotw) encoding: 0 = Sunday ... 6 = Saturday.
 *
 * Instances are not thread-safe: guard concurrent access externally, and make
 * sure interrupt_handler() never runs concurrently with any other method of
 * the same instance.
 *
 * A moved-from instance is inert: it no longer owns an I2C device and every
 * method that would touch the bus throws pcf85063a::exception.
 *
 * @tparam YEAR_OFFSET  Year the RTC year counter counts from (1970).
 * @tparam CAP_SEL      Select the 12.5 pF internal load capacitance.
 */
template<uint16_t YEAR_OFFSET = 1970, bool CAP_SEL = true>
class pcf85063a
{
    static constexpr const char *TAG = "pcf85063a";
    static constexpr uint32_t I2C_TIMEOUT_MS = 100;
    static constexpr uint8_t I2C_ADDR = 0x51;

    // Control & status registers
    static constexpr uint8_t REG_CTRL_1 = 0x00;
    static constexpr uint8_t REG_CTRL_2 = 0x01;
    static constexpr uint8_t REG_OFFSET = 0x02;
    static constexpr uint8_t REG_RAM = 0x03;

    // Time & date registers
    static constexpr uint8_t REG_SECONDS = 0x04;
    static constexpr uint8_t REG_MINUTES = 0x05;
    static constexpr uint8_t REG_HOURS = 0x06;
    static constexpr uint8_t REG_DAYS = 0x07;
    static constexpr uint8_t REG_WEEKDAYS = 0x08;
    static constexpr uint8_t REG_MONTHS = 0x09;
    static constexpr uint8_t REG_YEARS = 0x0A;

    // Alarm registers
    static constexpr uint8_t REG_SECOND_ALARM = 0x0B;
    static constexpr uint8_t REG_MINUTE_ALARM = 0x0C;
    static constexpr uint8_t REG_HOUR_ALARM = 0x0D;
    static constexpr uint8_t REG_DAY_ALARM = 0x0E;
    static constexpr uint8_t REG_WEEKDAY_ALARM = 0x0F;

    // Timer registers
    static constexpr uint8_t REG_TIMER_VAL = 0x10;
    static constexpr uint8_t REG_TIMER_MODE = 0x11;

    // Control_1 bits
    static constexpr uint8_t CTRL1_EXT_TEST = 0x80;
    static constexpr uint8_t CTRL1_STOP = 0x20;
    // Datasheet-prescribed magic byte written to Control_1 to trigger a
    // software reset. Not a bit mask: never use it in read-modify-write
    // sequences.
    static constexpr uint8_t CTRL1_RESET_SEQUENCE = 0x58;
    static constexpr uint8_t CTRL1_CIE = 0x04;
    static constexpr uint8_t CTRL1_12_24 = 0x02;
    static constexpr uint8_t CTRL1_CAP_SEL = 0x01;

    // Control_2 bits
    static constexpr uint8_t CTRL2_AIE = 0x80;
    static constexpr uint8_t CTRL2_AF = 0x40;
    static constexpr uint8_t CTRL2_MI = 0x20;
    static constexpr uint8_t CTRL2_HMI = 0x10;
    static constexpr uint8_t CTRL2_TF = 0x08;

    // Timer_Mode bits
    static constexpr uint8_t TIMER_CLK_MASK = 0x18;           // bits 4-3: clock frequency
    static constexpr uint8_t TIMER_ENABLE = 0x04;             // bit 2: timer enable
    static constexpr uint8_t TIMER_INTERRUPT_ENABLE = 0x02;   // bit 1: timer interrupt enable

    // Timer clock frequencies in Hz
    static constexpr double CLK_4096_HZ_FREQ = 4096.0;
    static constexpr double CLK_64_HZ_FREQ = 64.0;
    static constexpr double CLK_1_HZ_FREQ = 1.0;
    static constexpr double CLK_1_60_HZ_FREQ = 1.0 / 60.0;

    // Alarm enable bit: set to disable the corresponding alarm field
    static constexpr uint8_t ALARM_DISABLED = 0x80;

    // Oscillator stop flag in the Seconds register
    static constexpr uint8_t SECONDS_OS = 0x80;

    // Longest register burst the driver transfers (the seven time registers).
    static constexpr size_t MAX_BURST_LEN = 7;

    // Bounded retries for the read-modify-write loops that race with the
    // continuously counting RTC.
    static constexpr int OS_CLEAR_MAX_ATTEMPTS = 8;
    static constexpr int DATETIME_READ_MAX_ATTEMPTS = 8;

    // Offset register: LSB weight in milli-ppm (1e-3 ppm) for each mode. The
    // datasheet weights are 4.34 ppm (normal) and 4.069 ppm (coarse); keeping
    // the extra three digits avoids losing precision in the integer
    // accumulation.
    static constexpr int64_t OFFSET_STEP_NORMAL_MPPM = 4340;
    static constexpr int64_t OFFSET_STEP_COARSE_MPPM = 4069;
    static constexpr uint8_t OFFSET_MODE = 0x80;
    static constexpr int64_t OFFSET_MAX = 63;
    static constexpr int64_t OFFSET_MIN = -64;
    // Largest single correction accepted by set_drift_correction(), in ppm:
    // any more would saturate the Offset register anyway and is clamped here
    // so that the ppm -> milli-ppm conversion below cannot overflow.
    static constexpr int64_t OFFSET_LIMIT_PPM =
            OFFSET_STEP_NORMAL_MPPM * (OFFSET_MAX + 1) / 1000 + 1;

  public:
    /** Date and time value. */
    struct datetime
    {
        uint16_t year;    // absolute year, e.g. 2024
        uint8_t month;    // 1-12
        uint8_t day;      // 1-31
        uint8_t dotw;     // 0=Sunday ... 6=Saturday
        uint8_t hour;     // 0-23
        uint8_t minute;   // 0-59
        uint8_t second;   // 0-59
    };

    class exception final : public std::exception
    {
      public:
        /**
         * @param msg  Message to prepend to the driver tag. A null pointer is
         *             treated as an empty message.
         * @param err  ESP-IDF error code whose name is appended to \p msg.
         *
         * Messages longer than the internal 128-byte buffer are truncated.
         */
        explicit exception(const char *msg, esp_err_t err = ESP_OK) noexcept
        {
            const char *text = msg != nullptr ? msg : "";
            if (err != ESP_OK) {
                char err_buf[32];
                esp_err_to_name_r(err, err_buf, sizeof(err_buf));
                std::snprintf(msg_buf_, sizeof(msg_buf_), "%s: %s: %s", TAG, text, err_buf);
            } else {
                std::snprintf(msg_buf_, sizeof(msg_buf_), "%s: %s", TAG, text);
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

    /**
     * @param bus_handle     Already initialized I2C master bus handle
     * @param scl_speed_hz   I2C SCL clock speed in Hz (must be non-zero)
     */
    explicit pcf85063a(i2c_master_bus_handle_t bus_handle, uint32_t scl_speed_hz = 100000)
    {
        if (bus_handle == nullptr) {
            throw exception("bus_handle is null");
        }
        if (scl_speed_hz == 0) {
            throw exception("scl_speed_hz must be non-zero");
        }
        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = I2C_ADDR,
            .scl_speed_hz = scl_speed_hz,
            .scl_wait_us = 0,
            .flags = {},
        };
        esp_err_t err =
                i2c_master_bus_add_device(bus_handle, &cfg, &dev_);   // GCOVR_EXCL_BR_LINE
        if (err != ESP_OK) {
            throw exception("i2c_master_bus_add_device failed", err);
        }
        ESP_LOGI(TAG,   // GCOVR_EXCL_BR_LINE
                 "PCF85063A at 0x%02X initialized",
                 static_cast<unsigned>(I2C_ADDR));   // GCOVR_EXCL_BR_LINE
    }

    ~pcf85063a() noexcept
    {
        if (dev_ == nullptr) {
            return;
        }
        esp_err_t err = i2c_master_bus_rm_device(dev_);
        if (err != ESP_OK) {
            char err_buf[32];
            esp_err_to_name_r(err, err_buf, sizeof(err_buf));
            ESP_LOGE(TAG, "i2c_master_bus_rm_device failed: %s", err_buf);
        } else {
            ESP_LOGI(TAG, "PCF85063A deinitialized");
        }
    }

    pcf85063a(const pcf85063a &) = delete;
    pcf85063a &operator=(const pcf85063a &) = delete;

    pcf85063a(pcf85063a &&other) noexcept
            : dev_{ std::exchange(other.dev_, nullptr) }
            , has_last_sync_{ std::exchange(other.has_last_sync_, false) }
            , last_esp_s_{ other.last_esp_s_ }
            , alarm_cb_{ std::move(other.alarm_cb_) }
            , timer_cb_{ std::move(other.timer_cb_) }
    {
    }

    pcf85063a &operator=(pcf85063a &&other) noexcept
    {
        if (this != &other) {
            if (dev_ != nullptr) {
                esp_err_t err = i2c_master_bus_rm_device(dev_);
                if (err != ESP_OK) {
                    char err_buf[32];
                    esp_err_to_name_r(err, err_buf, sizeof(err_buf));
                    ESP_LOGE(TAG, "move assign: i2c_master_bus_rm_device failed: %s", err_buf);
                }
            }
            dev_ = std::exchange(other.dev_, nullptr);
            // The drift baseline belongs to the device that was just removed,
            // not to the one being adopted: carry it over with the handle.
            has_last_sync_ = std::exchange(other.has_last_sync_, false);
            last_esp_s_ = other.last_esp_s_;
            alarm_cb_ = std::move(other.alarm_cb_);
            timer_cb_ = std::move(other.timer_cb_);
        }
        return *this;
    }

    /**
     * Start the RTC: 24h format, internal load capacitance 12.5pF, clock running.
     * Safe to call multiple times.
     */
    void init()
    {
        write_reg(REG_CTRL_1, (CAP_SEL ? CTRL1_CAP_SEL : 0));
        const uint8_t val = read_reg(REG_CTRL_1);
        if ((val & CTRL1_STOP) != 0) {
            throw exception("unable to start RTC");
        }
        ESP_LOGI(TAG, "RTC clock is running");
    }

    /**
     * Issue a software reset of the RTC.
     *
     * A software reset restores all registers to their default values, so the
     * load capacitance selection (CAP_SEL) is re-applied afterwards by
     * re-running init(). The drift-measurement baseline is discarded as well,
     * because the time registers no longer hold the value the RTC was set to.
     */
    void reset()
    {
        write_reg(REG_CTRL_1, CTRL1_RESET_SEQUENCE);
        has_last_sync_ = false;
        ESP_LOGI(TAG, "software reset issued");
        init();
    }

    /** Stop the RTC clock (time keeping freezes). */
    void stop()
    {
        update_reg_bit(REG_CTRL_1, CTRL1_STOP, CTRL1_STOP);   // GCOVR_EXCL_BR_LINE
    }

    /** Start the RTC clock after a previous stop(). */
    void start()
    {
        update_reg_bit(REG_CTRL_1, CTRL1_STOP, 0);   // GCOVR_EXCL_BR_LINE
    }

    /**
     * Check whether the oscillator has stopped (e.g. after power loss) and
     * clear the flag.
     *
     * The flag lives in the Seconds register, together with the continuously
     * counting BCD seconds value. The register is re-read and the two values
     * are only written back when they still agree, so a second boundary
     * elapsing between the read and the write cannot move the clock back; the
     * whole sequence is retried a bounded number of times and an exception is
     * thrown if the flag never clears (e.g. because the oscillator is dead).
     */
    [[nodiscard]] bool oscillator_stop_flag()
    {
        for (int attempt = 0; attempt < OS_CLEAR_MAX_ATTEMPTS;
             ++attempt) {   // GCOVR_EXCL_LINE: gcov never attributes the loop entry
            const uint8_t val = read_reg(REG_SECONDS);
            if ((val & SECONDS_OS) == 0) {
                return false;
            }
            if (read_reg(REG_SECONDS) != val) {   // GCOVR_EXCL_BR_LINE
                continue;                         // GCOVR_EXCL_LINE
            }
            const uint8_t cleared = static_cast<uint8_t>(val & ~SECONDS_OS);
            write_reg(REG_SECONDS, cleared);
            if ((read_reg(REG_SECONDS) & SECONDS_OS) != 0) {   // GCOVR_EXCL_BR_LINE
                continue;
            }
            return true;
        }
        throw exception("unable to clear the oscillator stop flag");   // GCOVR_EXCL_LINE
    }

    /**
     * Set the current date and time.
     *
     * The date must be a valid calendar date (e.g. 2024-02-31 is rejected).
     * The caller's \p t.dotw is ignored: the weekday is always recomputed
     * from the date.
     *
     * While writing, the clock is stopped (which also resets the internal
     * divider chain) and started again, as recommended by the datasheet. If
     * the time write fails, the clock is restarted before the original
     * exception is propagated; a failure of the restart itself is logged and
     * does not mask the original exception.
     */
    void set_datetime(const datetime &t)
    {
        if (t.year < YEAR_OFFSET || t.year >= YEAR_OFFSET + 100) {
            throw exception("year out of supported range (millennium bug)");
        }
        if (t.month < 1 || t.month > 12 || t.day < 1 ||
            t.day > days_in_month(t.year, t.month) || t.hour > 23 || t.minute > 59 ||
            t.second > 59) {
            throw exception("invalid date and time");
        }
        const uint8_t buf[7] = {
            dec_to_bcd(t.second),
            dec_to_bcd(t.minute),
            dec_to_bcd(t.hour),
            dec_to_bcd(t.day),
            day_of_week(t.year, t.month, t.day),   // binary, not BCD
            dec_to_bcd(t.month),
            dec_to_bcd(static_cast<uint8_t>(t.year - YEAR_OFFSET)),
        };
        update_reg_bit(REG_CTRL_1, CTRL1_STOP, CTRL1_STOP);
        try {
            write_regs(REG_SECONDS, buf, sizeof(buf));
        } catch (...) {
            try {
                update_reg_bit(REG_CTRL_1, CTRL1_STOP, 0);
            } catch (...) {     // GCOVR_EXCL_LINE
                ESP_LOGE(TAG,   // GCOVR_EXCL_LINE
                         "failed to restart the RTC after a failed time write");
            }   // GCOVR_EXCL_LINE
            throw;
        }
        update_reg_bit(REG_CTRL_1, CTRL1_STOP, 0);
        ESP_LOGI(TAG, "date and time set");
    }

    /**
     * Set the ESP32 system time on the RTC and use the difference between the
     * two clocks to measure and compensate the RTC drift.
     *
     * On every call the current ESP32 time is written to the RTC and the time
     * of the update is saved. Starting from the second call the drift of the
     * RTC since the previous update is measured and compensated through the
     * Offset register. The RTC interval is measured against the value that was
     * actually written at the previous update (not against the pre-update
     * reading), so the offset corrected then does not leak into the measured
     * rate.
     */
    void set_datetime_with_drift()
    {
        timeval tv{};
        const datetime rtc_now = get_datetime();
        if (gettimeofday(&tv, nullptr) != 0) {        // GCOVR_EXCL_BR_LINE
            throw exception("gettimeofday failed");   // GCOVR_EXCL_LINE
        }
        const datetime esp_now = epoch_to_datetime(tv.tv_sec);
        set_datetime(esp_now);

        const int64_t esp_now_s = tv.tv_sec;
        if (has_last_sync_) {
            const int64_t esp_delta = esp_now_s - last_esp_s_;
            const int64_t rtc_delta = datetime_to_epoch(rtc_now) - last_esp_s_;
            if (esp_delta > 0) {
                // Positive drift means the RTC ran fast; a positive offset
                // value slows the clock down.
                const int64_t drift_ppm = (rtc_delta - esp_delta) * 1000000 / esp_delta;
                set_drift_correction(drift_ppm);
                ESP_LOGI(TAG,
                         "RTC drift since last sync: %lld ppm",
                         static_cast<long long>(drift_ppm));
            }
        }
        last_esp_s_ = esp_now_s;
        has_last_sync_ = true;
    }

    /**
     * Read the current date and time.
     *
     * The register block is read twice and only returned when both reads agree,
     * so a second (or larger) boundary crossed in the middle of a burst cannot
     * yield a torn mixture of old and new fields. The oscillator-stop flag is
     * checked and makes the read throw, and every field must be valid BCD and
     * fall inside its calendar range; garbage reads (e.g. after a brown-out)
     * throw instead of silently returning an invalid time.
     */
    [[nodiscard]] datetime get_datetime() const
    {
        for (int attempt = 0;   // GCOVR_EXCL_LINE: gcov never attributes the loop entry
             attempt < DATETIME_READ_MAX_ATTEMPTS;   // GCOVR_EXCL_LINE
             ++attempt) {
            uint8_t first[7];
            uint8_t second[7];
            read_regs(REG_SECONDS, first, sizeof(first));
            read_regs(REG_SECONDS, second, sizeof(second));
            if (std::equal(std::begin(first),   // GCOVR_EXCL_BR_LINE
                           std::end(first),
                           std::begin(second))) {
                return decode_datetime(first);
            }
        }
        throw exception("unable to read a consistent date and time");   // GCOVR_EXCL_LINE
    }

    /** Read the current date and time from the RTC and set the ESP32 system clock. */
    void get_datetime_and_set()
    {
        const datetime t = get_datetime();
        timeval tv{};
        tv.tv_sec = static_cast<time_t>(datetime_to_epoch(t));
        tv.tv_usec = 0;
        if (settimeofday(&tv, nullptr) != 0) {
            throw exception("settimeofday failed");
        }
        ESP_LOGI(TAG, "system time set from RTC");
    }

    /**
     * Read the current date and time from the RTC and smoothly adjust the
     * ESP32 system clock towards it using adjtime() instead of setting it
     * immediately.
     *
     * The current sub-second position of the system clock is included in the
     * delta, which is normalized to a canonical timeval (0 <= tv_usec < 1e6).
     */
    void get_datetime_and_adjust()
    {
        timeval now{};
        const datetime t = get_datetime();
        if (gettimeofday(&now, nullptr) != 0) {       // GCOVR_EXCL_BR_LINE
            throw exception("gettimeofday failed");   // GCOVR_EXCL_LINE
        }
        const int64_t delta_us = datetime_to_epoch(t) * 1000000 -
                                 (static_cast<int64_t>(now.tv_sec) * 1000000 + now.tv_usec);
        int64_t delta_s = delta_us / 1000000;
        int64_t delta_rem = delta_us % 1000000;
        if (delta_rem < 0) {
            delta_rem += 1000000;
            --delta_s;
        }
        timeval delta{};
        delta.tv_sec = static_cast<time_t>(delta_s);
        delta.tv_usec = static_cast<suseconds_t>(delta_rem);
        if (adjtime(&delta, nullptr) != 0) {
            throw exception("adjtime failed");
        }
        ESP_LOGI(TAG, "system time adjustment scheduled from RTC");
    }

    /** Disable the alarm interrupt and clear any pending alarm flag. */
    void disable_alarm()
    {
        update_reg_bit(REG_CTRL_2, CTRL2_AIE, 0);
        uint8_t buf[5] = {
            ALARM_DISABLED, ALARM_DISABLED, ALARM_DISABLED, ALARM_DISABLED, ALARM_DISABLED,
        };
        write_regs(REG_SECOND_ALARM, buf, sizeof(buf));
        update_reg_bit(REG_CTRL_2, CTRL2_AF, 0);
    }

    /** Read the alarm flag without modifying it (true = alarm matched). */
    [[nodiscard]] bool alarm_flag() const
    {
        return (read_reg(REG_CTRL_2) & CTRL2_AF) != 0;   // GCOVR_EXCL_BR_LINE
    }

    /**
     * Check the alarm flag (true = alarm matched) and clear it.
     *
     * The flag is cleared only after it has been observed set, so a "check
     * then act" sequence can use this method without losing the event when
     * the flag is not set. This method writes to the device, so it is not
     * const.
     */
    [[nodiscard]] bool check_and_clear_alarm_flag()
    {
        if (!alarm_flag()) {
            return false;
        }
        update_reg_bit(REG_CTRL_2, CTRL2_AF, 0);
        return true;
    }

    /**
     * Set the alarm to match second, minute and hour of \p t.
     * Day and weekday alarms are disabled.
     *
     * \p cb is invoked from interrupt_handler() when the alarm interrupt fires.
     */
    void set_alarm_any_day(const datetime &t, std::function<void()> cb = {})
    {
        if (t.hour > 23 || t.minute > 59 || t.second > 59) {
            throw exception("invalid alarm time");
        }
        set_alarm(t, false, std::move(cb));
        ESP_LOGI(TAG, "alarm set");
    }

    /**
     * Set the alarm to match second, minute, hour and day of month of \p t.
     * The weekday alarm is disabled.
     *
     * The day-of-month alarm fires on that calendar day in every month; the
     * months that do not have it (e.g. the 31st in February) are simply
     * skipped by the chip.
     *
     * \p cb is invoked from interrupt_handler() when the alarm interrupt fires.
     */
    void set_alarm_day(const datetime &t, std::function<void()> cb = {})
    {
        set_alarm(t, true, std::move(cb));
        ESP_LOGI(TAG, "alarm set (day of month)");
    }

    /**
     * Read the configured alarm.
     *
     * Fields whose alarm is disabled are returned with their ALARM_DISABLED
     * (0x80) bit masked off, so they are indistinguishable from real values:
     * use alarm registers semantics, or configure the alarm through
     * set_alarm_*(), when the enabled fields matter. \p year and \p month are
     * always left at 0 because the alarm does not carry them.
     */
    [[nodiscard]] datetime get_alarm() const
    {
        uint8_t buf[5];
        read_regs(REG_SECOND_ALARM, buf, sizeof(buf));
        datetime t{};
        t.second = bcd_to_dec(buf[0] & 0x7F);
        t.minute = bcd_to_dec(buf[1] & 0x7F);
        t.hour = bcd_to_dec(buf[2] & 0x3F);
        t.day = bcd_to_dec(buf[3] & 0x3F);
        t.dotw = buf[4] & 0x07;   // binary, not BCD
        return t;
    }
    /** Timer clock frequency selection. */
    enum class timer_clock : uint8_t
    {
        CLK_4096_HZ = 0x00,   // 4.096 kHz
        CLK_64_HZ = 0x08,     // 64 Hz
        CLK_1_HZ = 0x10,      // 1 Hz
        CLK_1_60_HZ = 0x18,   // 1/60 Hz
    };

    /**
     * Set the countdown timer.
     *
     * The timer counts down from \p val and, when it reaches zero, it is
     * automatically reloaded with \p val. \p val is in the range 1..0xFF: a
     * zero-length countdown is rejected because the chip's behavior with
     * Timer_value = 0 is not clearly specified. The timer period is
     * val / frequency, where the frequency is selected by \p clk.
     *
     * The timer is disabled before Timer_value is reprogrammed, as the
     * datasheet requires, and re-enabled afterwards.
     *
     * \p cb is invoked from interrupt_handler() when the timer interrupt fires.
     */
    void set_timer(uint8_t val, timer_clock clk, std::function<void()> cb = {})
    {
        if (val == 0) {
            throw exception("timer value must be non-zero");
        }
        // Stop the timer before replacing Timer_value: writing it while TE is
        // set is not allowed by the datasheet. Timer_Mode is fully rewritten
        // afterwards (only the clock bits), then enable_timer() sets TE/TIE.
        update_reg_bit(REG_TIMER_MODE, TIMER_ENABLE, 0);
        write_reg(REG_TIMER_VAL, val);
        write_reg(REG_TIMER_MODE, static_cast<uint8_t>(clk));
        enable_timer();
        timer_cb_ = std::move(cb);
        ESP_LOGI(TAG,
                 "timer set: value=%u clock=0x%02X",
                 static_cast<unsigned>(val),
                 static_cast<unsigned>(static_cast<uint8_t>(clk)));
    }

    /** Disable the timer and its interrupt, leaving the timer flag intact. */
    void disable_timer()
    {
        update_reg_bit(REG_TIMER_MODE, TIMER_ENABLE | TIMER_INTERRUPT_ENABLE, 0);
        ESP_LOGI(TAG, "timer disabled");
    }

    /** Read the timer flag without modifying it (true = countdown reached zero). */
    [[nodiscard]] bool timer_flag() const
    {
        return (read_reg(REG_CTRL_2) & CTRL2_TF) != 0;   // GCOVR_EXCL_BR_LINE
    }

    /**
     * Check the timer flag (true = the timer countdown reached zero) and
     * clear it.
     *
     * The flag is cleared only after it has been observed set, so a "check
     * then act" sequence can use this method without losing the event when
     * the flag is not set. This method writes to the device, so it is not
     * const.
     */
    [[nodiscard]] bool check_and_clear_timer_flag()
    {
        if (!timer_flag()) {
            return false;
        }
        update_reg_bit(REG_CTRL_2, CTRL2_TF, 0);
        return true;
    }

    /**
     * Read the current timer countdown and return it in seconds.
     *
     * While the timer is running this is the remaining time, not the period
     * the timer was programmed with.
     */
    [[nodiscard]] double get_timer() const
    {
        const uint8_t val = read_reg(REG_TIMER_VAL);
        const uint8_t clk = read_reg(REG_TIMER_MODE) & TIMER_CLK_MASK;
        return val / clock_freq_hz(static_cast<timer_clock>(clk));
    }

    /**
     * Set the countdown timer to \p seconds, choosing the value and clock that
     * best approximate the requested duration. \p seconds must be greater
     * than zero and large enough to be representable (a zero-length countdown
     * is rejected because the chip's behavior with Timer_value = 0 is not
     * clearly specified). The timer period is REG_TIMER_VAL / frequency, with
     * the frequency selected by the Timer_Mode clock bits. Only durations that
     * fit in the 1..255 timer value are supported; anything else throws.
     *
     * \p cb is invoked from interrupt_handler() when the timer interrupt fires.
     */
    void set_timer(double seconds, std::function<void()> cb = {})
    {
        const double max_seconds = std::numeric_limits<uint8_t>::max() / CLK_1_60_HZ_FREQ;
        if (!(seconds > 0.0 && seconds <= max_seconds)) {
            throw exception("timer length out of supported range");
        }

        static constexpr std::array<timer_clock, 4> clocks{
            timer_clock::CLK_1_HZ,
            timer_clock::CLK_64_HZ,
            timer_clock::CLK_4096_HZ,
            timer_clock::CLK_1_60_HZ,
        };
        double best_err = std::numeric_limits<double>::infinity();
        uint8_t best_val = 0;
        timer_clock best_clk = timer_clock::CLK_1_HZ;
        for (const timer_clock clk : clocks) {
            const double freq = clock_freq_hz(clk);
            const double val = std::round(seconds * freq);
            if (val > std::numeric_limits<uint8_t>::max()) {
                continue;
            }
            const double err = std::abs(val / freq - seconds);
            if (err < best_err) {   // GCOVR_EXCL_BR_LINE
                best_err = err;
                best_val = static_cast<uint8_t>(val);
                best_clk = clk;
            }
        }
        // best_val stays 0 both when no clock could represent the duration and
        // when every clock rounds down to zero, i.e. when the request is too
        // short to program a valid non-zero Timer_value.
        if (best_val == 0) {
            throw exception("timer length too short to represent");
        }
        set_timer(best_val, best_clk, std::move(cb));
        ESP_LOGI(TAG,
                 "timer set: %g s (value=%u clock=0x%02X)",
                 seconds,
                 static_cast<unsigned>(best_val),
                 static_cast<unsigned>(static_cast<uint8_t>(best_clk)));
    }

    /**
     * Handle an interrupt detected on the INT pin.
     *
     * Reads the alarm and timer flags, invokes the corresponding callbacks
     * registered with set_alarm_*() and set_timer(), and clears the flags
     * before running the callbacks. Callbacks that throw are caught and
     * logged, so one misbehaving callback can never prevent the other flag
     * from being handled nor propagate out of this method.
     * Not an ISR: call it from regular task context, e.g. from a GPIO ISR
     * that only notifies a task.
     */
    void interrupt_handler()
    {
        if (check_and_clear_alarm_flag()) {
            if (alarm_cb_) {
                // GCOVR_EXCL_START: EH cleanup edges of the catch-all guards
                try {
                    alarm_cb_();
                } catch (const std::exception &e) {
                    ESP_LOGE(TAG, "alarm callback threw: %s", e.what());
                } catch (...) {
                    ESP_LOGE(TAG, "alarm callback threw an exception");
                }
                // GCOVR_EXCL_STOP

            } else {
                ESP_LOGW(TAG, "alarm flag set but no alarm callback registered");
            }
        }
        if (check_and_clear_timer_flag()) {
            if (timer_cb_) {
                // GCOVR_EXCL_START: EH cleanup edges of the catch-all guards
                try {
                    timer_cb_();
                } catch (const std::exception &e) {
                    ESP_LOGE(TAG, "timer callback threw: %s", e.what());
                } catch (...) {
                    ESP_LOGE(TAG, "timer callback threw an exception");
                }
                // GCOVR_EXCL_STOP

            } else {
                ESP_LOGW(TAG, "timer flag set but no timer callback registered");
            }
        }
    }

    /** Format a datetime as "YYYY.MM.DD W HH:MM:SS" into \p buf. */
    static void to_str(char *buf, size_t len, const datetime &t)
    {
        if (buf == nullptr || len == 0) {
            return;
        }
        std::snprintf(buf,
                      len,
                      "%04u.%02u.%02u %u %02u:%02u:%02u",
                      static_cast<unsigned>(t.year),
                      static_cast<unsigned>(t.month),
                      static_cast<unsigned>(t.day),
                      static_cast<unsigned>(t.dotw),
                      static_cast<unsigned>(t.hour),
                      static_cast<unsigned>(t.minute),
                      static_cast<unsigned>(t.second));
    }

    /**
     * Program the Offset register.
     *
     * \p ppm is a drift correction in parts per million: a positive value makes
     * the clock run slower. MODE = 0 (normal, one correction every two hours)
     * has an LSB weight of 4.34 ppm, MODE = 1 (coarse, one correction every 4
     * minutes) has an LSB weight of 4.069 ppm; the offset is a two's
     * complement value in the range +63..-64 LSB. The previously programmed
     * offset is read back, converted to ppm using its own mode and accumulated
     * with the new correction, so successive calls add up instead of replacing
     * each other; the mode that best approximates the accumulated drift is
     * then selected.
     */
    void set_drift_correction(int64_t ppm)
    {
        // Read the previously programmed offset (signed 7-bit two's complement)
        // and convert it to milli-ppm using its mode so the accumulation is
        // correct even when the mode changes in between.
        const uint8_t prev_reg = read_reg(REG_OFFSET);
        const int64_t prev_lsb = (prev_reg & 0x7F) - ((prev_reg & 0x40) != 0 ? 0x80 : 0);
        const int64_t prev_mppm =
                prev_lsb * ((prev_reg & OFFSET_MODE) != 0 ? OFFSET_STEP_COARSE_MPPM :
                                                            OFFSET_STEP_NORMAL_MPPM);
        const int64_t total_mppm =
                prev_mppm + std::clamp(ppm, -OFFSET_LIMIT_PPM, OFFSET_LIMIT_PPM) * 1000;

        const auto clamp_lsb = [](int64_t v) {
            if (v > OFFSET_MAX) {
                return OFFSET_MAX;
            }
            if (v < OFFSET_MIN) {   // GCOVR_EXCL_BR_LINE
                return OFFSET_MIN;
            }
            return v;
        };
        const int64_t lsb0 = clamp_lsb(round_div(total_mppm, OFFSET_STEP_NORMAL_MPPM));
        const int64_t lsb1 = clamp_lsb(round_div(total_mppm, OFFSET_STEP_COARSE_MPPM));
        const int64_t err0 = std::abs(total_mppm - lsb0 * OFFSET_STEP_NORMAL_MPPM);
        const int64_t err1 = std::abs(total_mppm - lsb1 * OFFSET_STEP_COARSE_MPPM);
        const bool mode1 = err1 < err0;
        const int64_t lsb = mode1 ? lsb1 : lsb0;

        const uint8_t reg = static_cast<uint8_t>(lsb & 0x7F) | (mode1 ? OFFSET_MODE : 0);
        write_reg(REG_OFFSET, reg);
        ESP_LOGI(
                TAG,
                "drift correction: %lld ppm -> MODE=%d OFFSET=%lld (0x%02X, accumulated from %lld LSB / %lld ppm)",
                static_cast<long long>(ppm),
                mode1 ? 1 : 0,
                static_cast<long long>(lsb),
                static_cast<unsigned>(reg),
                static_cast<long long>(prev_lsb),
                static_cast<long long>(prev_mppm / 1000));
    }

    /** Clear the drift correction by setting the Offset register to 0. */
    void reset_drift_correction()
    {
        write_reg(REG_OFFSET, 0);
        ESP_LOGI(TAG, "drift correction reset");
    }

  private:
    i2c_master_dev_handle_t dev_{};
    bool has_last_sync_{ false };
    int64_t last_esp_s_{ 0 };
    std::function<void()> alarm_cb_{};
    std::function<void()> timer_cb_{};

    /** Throw if this instance no longer owns an I2C device (moved-from). */
    void require_device() const
    {
        if (dev_ == nullptr) {
            throw exception("operation on a moved-from instance");
        }
    }

    /** Enable the alarm interrupt and clear any pending alarm flag. */
    void enable_alarm()
    {
        uint8_t val = read_reg(REG_CTRL_2);
        val |= CTRL2_AIE;
        val &= ~CTRL2_AF;
        write_reg(REG_CTRL_2, val);
    }

    /**
     * Program the alarm registers: second, minute and hour from \p t, with the
     * day-of-month alarm enabled only when \p use_day is true and the weekday
     * alarm always disabled.
     *
     * Validates the time fields (and \p t.day when the day-of-month alarm is
     * used) before programming anything. The alarm interrupt is disabled and
     * the pending flag cleared around the register writes, so an alarm that
     * would match while the registers are being reprogrammed is not silently
     * dropped.
     */
    void set_alarm(const datetime &t, bool use_day, std::function<void()> cb)
    {
        if (t.hour > 23 || t.minute > 59 || t.second > 59 ||   // GCOVR_EXCL_BR_LINE
            (use_day && (t.day < 1 || t.day > 31))) {          // GCOVR_EXCL_BR_LINE
            throw exception("invalid alarm time");
        }
        update_reg_bit(REG_CTRL_2, CTRL2_AIE, 0);
        uint8_t buf[5] = {
            static_cast<uint8_t>(dec_to_bcd(t.second) & ~ALARM_DISABLED),
            static_cast<uint8_t>(dec_to_bcd(t.minute) & ~ALARM_DISABLED),
            static_cast<uint8_t>(dec_to_bcd(t.hour) & ~ALARM_DISABLED),
            static_cast<uint8_t>(use_day ? dec_to_bcd(t.day) & ~ALARM_DISABLED :
                                           ALARM_DISABLED),
            ALARM_DISABLED,   // weekday alarm disabled
        };
        write_regs(REG_SECOND_ALARM, buf, sizeof(buf));
        enable_alarm();
        alarm_cb_ = std::move(cb);
    }

    /** Enable the timer interrupt and clear any pending timer flag. */
    void enable_timer()
    {
        uint8_t val = read_reg(REG_TIMER_MODE);
        val |= TIMER_ENABLE | TIMER_INTERRUPT_ENABLE;
        write_reg(REG_TIMER_MODE, val);
        val = read_reg(REG_CTRL_2);
        val &= ~CTRL2_TF;
        write_reg(REG_CTRL_2, val);
    }

    /** Frequency in Hz of a timer clock selection. */
    [[nodiscard]] static constexpr double clock_freq_hz(timer_clock clk)
    {
        switch (clk) {
        case timer_clock::CLK_4096_HZ:
            return CLK_4096_HZ_FREQ;
        case timer_clock::CLK_64_HZ:
            return CLK_64_HZ_FREQ;
        case timer_clock::CLK_1_60_HZ:
            return CLK_1_60_HZ_FREQ;
        default:
            return CLK_1_HZ_FREQ;
        }
    }

    /** Weekday (0=Sunday ... 6=Saturday) using Sakamoto's method. */
    [[nodiscard]] static constexpr uint8_t day_of_week(uint16_t year,
                                                       uint8_t month,
                                                       uint8_t day)
    {
        static constexpr uint8_t t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
        if (month < 1 || month > 12) {               // GCOVR_EXCL_BR_LINE
            throw exception("month out of range");   // GCOVR_EXCL_LINE
        }
        uint16_t y = year;
        if (month < 3) {
            --y;
        }
        return static_cast<uint8_t>((y + y / 4 - y / 100 + y / 400 + t[month - 1] + day) % 7);
    }

    [[nodiscard]] static constexpr bool is_leap_year(uint16_t year)
    {
        return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;   // GCOVR_EXCL_BR_LINE
    }

    /** Number of days in a month (1-12) of the given year. */
    [[nodiscard]] static constexpr uint8_t days_in_month(uint16_t year, uint8_t month)
    {
        static constexpr uint8_t days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        if (month < 1 || month > 12) {               // GCOVR_EXCL_BR_LINE
            throw exception("month out of range");   // GCOVR_EXCL_LINE
        }
        if (month == 2 && is_leap_year(year)) {
            return 29;
        }
        return days[month - 1];
    }

    /** Days since 1970-01-01 for a Gregorian date (Howard Hinnant's algorithm). */
    [[nodiscard]] static constexpr int64_t days_from_civil(uint16_t year,
                                                           uint8_t month,
                                                           uint8_t day)
    {
        const int64_t y = static_cast<int64_t>(year) - (month <= 2 ? 1 : 0);
        // The negative-year era correction is required by the reference
        // algorithm even though the driver only ever passes modern years.
        const int64_t era = (y >= 0 ? y : y - 399) / 400;   // GCOVR_EXCL_BR_LINE
        const int64_t yoe = y - era * 400;
        const int64_t doy =
                (153 * (static_cast<int64_t>(month) + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
        const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + doe - 719468;
    }

    /** Gregorian date from days since 1970-01-01 (Howard Hinnant's algorithm). */
    static constexpr void civil_from_days(int64_t z,
                                          uint16_t &year,
                                          uint8_t &month,
                                          uint8_t &day)
    {
        z += 719468;
        const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
        const uint64_t doe = static_cast<uint64_t>(z - era * 146097);
        const uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        const int64_t y = static_cast<int64_t>(yoe) + era * 400;
        const uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const uint64_t mp = (5 * doy + 2) / 153;
        day = static_cast<uint8_t>(doy - (153 * mp + 2) / 5 + 1);
        month = static_cast<uint8_t>(mp + (mp < 10 ? 3 : -9));
        const int64_t full_year = y + (month <= 2 ? 1 : 0);
        // The caller must stay within the representable year range (e.g. the
        // system clock being converted in epoch_to_datetime() is sane);
        // silently wrapping to 16 bits would produce garbage dates.
        if (full_year < 0 || full_year > std::numeric_limits<uint16_t>::max()) {
            throw exception("date conversion: year out of range");
        }
        year = static_cast<uint16_t>(full_year);
    }

    /** Seconds since the Unix epoch for a datetime (UTC). */
    [[nodiscard]] static constexpr int64_t datetime_to_epoch(const datetime &t)
    {
        return days_from_civil(t.year, t.month, t.day) * 86400 + t.hour * 3600 +
               t.minute * 60 + t.second;
    }

    /** Datetime (UTC) from seconds since the Unix epoch. */
    [[nodiscard]] static constexpr datetime epoch_to_datetime(int64_t epoch)
    {
        int64_t days = epoch / 86400;
        int64_t rem = epoch % 86400;
        if (rem < 0) {
            rem += 86400;
            --days;
        }
        uint16_t year;
        uint8_t month;
        uint8_t day;
        civil_from_days(days, year, month, day);
        datetime t{};
        t.year = year;
        t.month = month;
        t.day = day;
        t.dotw = day_of_week(year, month, day);
        t.hour = static_cast<uint8_t>(rem / 3600);
        t.minute = static_cast<uint8_t>((rem % 3600) / 60);
        t.second = static_cast<uint8_t>(rem % 60);
        return t;
    }

    /** Decode and validate a raw seven-register time/date block. */
    [[nodiscard]] static datetime decode_datetime(const uint8_t (&buf)[7])
    {
        if ((buf[0] & SECONDS_OS) != 0) {
            throw exception("oscillator stop flag set: the RTC time is not trustworthy");
        }
        datetime t{};
        t.second = bcd_to_dec_checked(buf[0] & 0x7F);
        t.minute = bcd_to_dec_checked(buf[1] & 0x7F);
        t.hour = bcd_to_dec_checked(buf[2] & 0x3F);
        t.day = bcd_to_dec_checked(buf[3] & 0x3F);
        t.dotw = buf[4] & 0x07;   // binary, not BCD
        t.month = bcd_to_dec_checked(buf[5] & 0x1F);
        t.year = static_cast<uint16_t>(bcd_to_dec_checked(buf[6]) + YEAR_OFFSET);

        // GCOVR_EXCL_BR_START: compound validation, every sub-condition is
        // exercised through set_datetime()/get_datetime() tests.
        const bool valid = t.month >= 1 && t.month <= 12 && t.day >= 1 &&
                           t.day <= days_in_month(t.year, t.month) && t.hour <= 23 &&
                           t.minute <= 59 && t.second <= 59 && t.dotw <= 6;
        // GCOVR_EXCL_BR_STOP

        if (!valid) {
            throw exception("invalid date and time read from RTC");
        }
        return t;
    }

    /** Round to nearest, ties away from zero. */
    [[nodiscard]] static constexpr int64_t round_div(int64_t a, int64_t b)
    {
        if (a >= 0) {
            return (a + b / 2) / b;
        }
        return -((-a + b / 2) / b);
    }

    [[nodiscard]] static constexpr uint8_t dec_to_bcd(uint8_t val)
    {
        if (val > 99) {                                  // GCOVR_EXCL_BR_LINE
            throw exception("value out of BCD range");   // GCOVR_EXCL_LINE
        }
        return static_cast<uint8_t>((val / 10 * 16) + (val % 10));
    }

    [[nodiscard]] static constexpr uint8_t bcd_to_dec(uint8_t val)
    {
        return static_cast<uint8_t>((val / 16 * 10) + (val % 16));
    }

    /** Like bcd_to_dec(), but rejects a byte whose nibbles are not valid BCD. */
    [[nodiscard]] static uint8_t bcd_to_dec_checked(uint8_t val)
    {
        if ((val & 0x0F) > 9 || ((val >> 4) & 0x0F) > 9) {
            throw exception("invalid BCD value read from RTC");
        }
        return bcd_to_dec(val);
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

    void write_reg(uint8_t reg, uint8_t val) const
    {
        require_device();
        uint8_t buf[2] = { reg, val };
        esp_err_t err = i2c_master_transmit(dev_, buf, sizeof(buf), I2C_TIMEOUT_MS);
        if (err != ESP_OK) {
            throw exception("write_reg failed", err);
        }
    }

    void update_reg_bit(uint8_t reg, uint8_t update_bits, uint8_t data) const
    {
        uint8_t val = read_reg(reg);
        val = (val & ~update_bits) | (update_bits & data);
        write_reg(reg, val);
    }

    void read_regs(uint8_t reg, uint8_t *buf, size_t len) const
    {
        require_device();
        if (len > MAX_BURST_LEN) {                          // GCOVR_EXCL_BR_LINE
            throw exception("read_regs: too many bytes");   // GCOVR_EXCL_LINE
        }
        esp_err_t err = i2c_master_transmit_receive(dev_, &reg, 1, buf, len, I2C_TIMEOUT_MS);
        if (err != ESP_OK) {
            throw exception("read_regs failed", err);
        }
    }

    void write_regs(uint8_t reg, const uint8_t *buf, size_t len) const
    {
        require_device();
        std::array<uint8_t, MAX_BURST_LEN + 1> tx{};
        if (len > MAX_BURST_LEN) {                           // GCOVR_EXCL_BR_LINE
            throw exception("write_regs: too many bytes");   // GCOVR_EXCL_LINE
        }
        tx[0] = reg;
        std::copy_n(buf, len, tx.begin() + 1);
        esp_err_t err = i2c_master_transmit(dev_, tx.data(), len + 1, I2C_TIMEOUT_MS);
        if (err != ESP_OK) {
            throw exception("write_regs failed", err);
        }
    }
};
