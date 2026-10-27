/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <inttypes.h>
#include <iterator>
#include <type_traits>
#include <utility>

#include <esp_err.h>
#include <esp_log.h>

#include <driver/i2c_master.h>
#include <driver/i2s_std.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace es7210_detail {

// Buffer accepted by es7210::read(): a contiguous container of signed integral
// samples wide enough for the configured I2S data width. Requiring the exact
// value_type* return of data() rejects const containers (the driver writes
// through the pointer) and proxy-style accessors.
template<typename T>
concept sample_container = requires(T &t) {
    typename T::value_type;
    { t.size() } -> std::same_as<size_t>;
    { t.data() } -> std::same_as<typename T::value_type *>;
} && std::is_integral_v<typename T::value_type> && std::is_signed_v<typename T::value_type>;

}   // namespace es7210_detail

/**
 * C++ driver for the ES7210 high-performance, quad-channel ADC with I2S output.
 *
 * This class is not thread-safe: start(), stop(), set_gain() and set_mute()
 * perform non-atomic I2C read-modify-write sequences and set_sample_rate()
 * performs a multi-register write sequence, so concurrent calls to the control
 * methods must be serialized by the caller. A partial failure of any of these
 * sequences can leave the chip in an intermediate configuration that the
 * cached object state does not reflect (get_sample_rate() reports the last
 * successfully published rate). read() only touches the I2S data path and may
 * safely be called from a different task than the control methods.
 *
 * The destructor and move assignment access the I2C bus (the destructor also
 * runs stop(), which can issue up to 17 transactions with a 100 ms timeout
 * each), so destroying or move-assigning an object must not race with another
 * task using it.
 */

static_assert(__cplusplus >= 202002L, "es7210 requires C++20 or later");

template<i2s_mclk_multiple_t MclkMultiple = I2S_MCLK_MULTIPLE_256, bool I2s32Bit = false>
class es7210
{
    static constexpr const char *TAG = "es7210";

    // ES7210 register map
    static constexpr uint8_t REG_RESET = 0x00;
    static constexpr uint8_t REG_CLOCK_OFF = 0x01;
    static constexpr uint8_t REG_MAINCLK = 0x02;
    static constexpr uint8_t REG_MASTER_CLK = 0x03;
    static constexpr uint8_t REG_LRCK_DIVH = 0x04;
    static constexpr uint8_t REG_LRCK_DIVL = 0x05;
    static constexpr uint8_t REG_POWER_DOWN = 0x06;
    static constexpr uint8_t REG_OSR = 0x07;
    static constexpr uint8_t REG_MODE_CONFIG = 0x08;
    static constexpr uint8_t REG_TIME_CONTROL0 = 0x09;
    static constexpr uint8_t REG_TIME_CONTROL1 = 0x0A;
    static constexpr uint8_t REG_SDP_INTERFACE1 = 0x11;
    static constexpr uint8_t REG_SDP_INTERFACE2 = 0x12;
    static constexpr uint8_t REG_AUTOMUTE = 0x13;
    static constexpr uint8_t REG_MUTERANGE_L = 0x14;
    static constexpr uint8_t REG_MUTERANGE_R = 0x15;
    static constexpr uint8_t REG_ADC34_HPF2 = 0x20;
    static constexpr uint8_t REG_ADC34_HPF1 = 0x21;
    static constexpr uint8_t REG_ADC12_HPF1 = 0x22;
    static constexpr uint8_t REG_ADC12_HPF2 = 0x23;
    static constexpr uint8_t REG_CHIP_ID1 = 0x3D;
    static constexpr uint8_t REG_CHIP_ID0 = 0x3E;
    static constexpr uint8_t REG_VERSION = 0x3F;
    static constexpr uint8_t REG_ANALOG = 0x40;
    static constexpr uint8_t REG_MIC12_BIAS = 0x41;
    static constexpr uint8_t REG_MIC34_BIAS = 0x42;
    static constexpr uint8_t REG_MIC1_GAIN = 0x43;
    static constexpr uint8_t REG_MIC2_GAIN = 0x44;
    static constexpr uint8_t REG_MIC3_GAIN = 0x45;
    static constexpr uint8_t REG_MIC4_GAIN = 0x46;
    static constexpr uint8_t REG_MIC1_POWER = 0x47;
    static constexpr uint8_t REG_MIC2_POWER = 0x48;
    static constexpr uint8_t REG_MIC3_POWER = 0x49;
    static constexpr uint8_t REG_MIC4_POWER = 0x4A;
    static constexpr uint8_t REG_MIC12_POWER = 0x4B;
    static constexpr uint8_t REG_MIC34_POWER = 0x4C;

    static constexpr uint32_t I2C_TIMEOUT_MS = 100;

    struct coeff_div
    {
        uint32_t mclk;
        uint32_t lrck;
        uint8_t ss_ds;   // not used by this driver (kept for upstream diffing)
        uint8_t adc_div;
        uint8_t dll;
        uint8_t doubler;
        uint8_t osr;
        uint8_t mclk_src;   // not used: this driver is hardwired to slave mode
        uint32_t lrck_h;
        uint32_t lrck_l;
    };

    static constexpr coeff_div coeff_table[] = {
        //  mclk      lrck    ss_ds adc_div dll doubler osr  mclk_src lrckh  lrckl
        /* 8k */
        { 12288000, 8000, 0x00, 0x03, 0x01, 0x00, 0x20, 0x00, 0x06, 0x00 },
        { 16384000, 8000, 0x00, 0x04, 0x01, 0x00, 0x20, 0x00, 0x08, 0x00 },
        { 19200000, 8000, 0x00, 0x1e, 0x00, 0x01, 0x28, 0x00, 0x09, 0x60 },
        { 4096000, 8000, 0x00, 0x01, 0x01, 0x00, 0x20, 0x00, 0x02, 0x00 },
        /* 11.025k */
        { 11289600, 11025, 0x00, 0x02, 0x01, 0x00, 0x20, 0x00, 0x01, 0x00 },
        /* 12k */
        { 12288000, 12000, 0x00, 0x02, 0x01, 0x00, 0x20, 0x00, 0x04, 0x00 },
        { 19200000, 12000, 0x00, 0x14, 0x00, 0x01, 0x28, 0x00, 0x06, 0x40 },
        /* 16k */
        { 4096000, 16000, 0x00, 0x01, 0x01, 0x01, 0x20, 0x00, 0x01, 0x00 },
        { 19200000, 16000, 0x00, 0x0a, 0x00, 0x00, 0x1e, 0x00, 0x04, 0x80 },
        { 16384000, 16000, 0x00, 0x02, 0x01, 0x00, 0x20, 0x00, 0x04, 0x00 },
        { 12288000, 16000, 0x00, 0x03, 0x01, 0x01, 0x20, 0x00, 0x03, 0x00 },
        /* 22.05k */
        { 11289600, 22050, 0x00, 0x01, 0x01, 0x00, 0x20, 0x00, 0x02, 0x00 },
        /* 24k */
        { 12288000, 24000, 0x00, 0x01, 0x01, 0x00, 0x20, 0x00, 0x02, 0x00 },
        { 19200000, 24000, 0x00, 0x0a, 0x00, 0x01, 0x28, 0x00, 0x03, 0x20 },
        /* 32k */
        { 8192000, 32000, 0x00, 0x01, 0x01, 0x01, 0x20, 0x00, 0x01, 0x00 },
        { 12288000, 32000, 0x00, 0x03, 0x00, 0x00, 0x20, 0x00, 0x01, 0x80 },
        { 16384000, 32000, 0x00, 0x01, 0x01, 0x00, 0x20, 0x00, 0x02, 0x00 },
        { 19200000, 32000, 0x00, 0x05, 0x00, 0x00, 0x1e, 0x00, 0x02, 0x58 },
        /* 44.1k */
        { 11289600, 44100, 0x00, 0x01, 0x01, 0x01, 0x20, 0x00, 0x01, 0x00 },
        /* 48k */
        { 12288000, 48000, 0x00, 0x01, 0x01, 0x01, 0x20, 0x00, 0x01, 0x00 },
        { 19200000, 48000, 0x00, 0x05, 0x00, 0x01, 0x28, 0x00, 0x01, 0x90 },
        /* 64k */
        { 16384000, 64000, 0x01, 0x01, 0x01, 0x00, 0x20, 0x00, 0x01, 0x00 },
        { 19200000, 64000, 0x00, 0x05, 0x00, 0x01, 0x1e, 0x00, 0x01, 0x2c },
        /* 88.2k */
        { 11289600, 88200, 0x01, 0x01, 0x01, 0x01, 0x20, 0x00, 0x00, 0x80 },
        /* 96k */
        { 12288000, 96000, 0x01, 0x01, 0x01, 0x01, 0x20, 0x00, 0x00, 0x80 },
        { 19200000, 96000, 0x01, 0x05, 0x00, 0x01, 0x28, 0x00, 0x00, 0xc8 },
    };

  public:
    /** MCLK multiple the I2S channel must be clocked with (see the template parameter). */
    static constexpr auto mclk_multiple = static_cast<std::size_t>(MclkMultiple);

    static constexpr uint8_t MIC1 = 0x01;
    static constexpr uint8_t MIC2 = 0x02;
    static constexpr uint8_t MIC3 = 0x04;
    static constexpr uint8_t MIC4 = 0x08;
    static constexpr uint8_t MIC_MASK = MIC1 | MIC2 | MIC3 | MIC4;

    enum gain_value : uint8_t
    {
        GAIN_0DB = 0,
        GAIN_3DB,
        GAIN_6DB,
        GAIN_9DB,
        GAIN_12DB,
        GAIN_15DB,
        GAIN_18DB,
        GAIN_21DB,
        GAIN_24DB,
        GAIN_27DB,
        GAIN_30DB,
        GAIN_33DB,
        GAIN_34_5DB,
        GAIN_36DB,
        GAIN_37_5DB,
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
                std::snprintf(msg_buf_, sizeof(msg_buf_), "%s: %s", TAG, msg);
            }
        }
        ~exception() override = default;
        exception(const exception &) = default;
        exception &operator=(const exception &) = default;
        exception(exception &&) noexcept = default;
        exception &operator=(exception &&) noexcept = default;
        const char *what() const noexcept override   // GCOVR_EXCL_BR_LINE
        {
            return msg_buf_;
        }

      private:
        char msg_buf_[128]{};
    };

    /**
     * @brief Construct ES7210 device.
     *
     * @param bus_handle     I2C master bus handle
     * @param rx_handle_arg  Already initialized I2S RX channel handle. The
     *                       caller must configure it with an MCLK multiple,
     *                       bit width and TDM slot count matching the
     *                       template parameters and the selected microphones
     *                       (3 or 4 mics use TDM mode).
     * @param sample_rate    Sample rate in Hz
     * @param mic_selected   Bitmask of selected microphones (MIC1|MIC2|MIC3|MIC4)
     * @param addr           I2C device address (0x40 to 0x43)
     * @param scl_speed_hz   I2C SCL clock speed in Hz
     */
    explicit es7210(i2c_master_bus_handle_t bus_handle,
                    i2s_chan_handle_t rx_handle_arg,
                    uint32_t sample_rate = 16000,
                    uint8_t mic_selected = MIC1 | MIC2 | MIC3 | MIC4,
                    uint8_t addr = 0x40,
                    uint32_t scl_speed_hz = 100000)
            : rx_handle_(rx_handle_arg)
            , sample_rate_(sample_rate)
            , mic_selected_(mic_selected)
            , addr_(addr)
            , scl_speed_hz_(scl_speed_hz)
    {
        if (bus_handle == nullptr) {
            throw exception("bus_handle is null");
        }
        if (rx_handle_ == nullptr) {
            throw exception("rx_handle is null");
        }
        if ((mic_selected_ & MIC_MASK) != mic_selected_ || mic_selected_ == 0) {
            throw exception("invalid microphone selection");
        }
        if (addr_ < 0x40 || addr_ > 0x43) {
            throw exception("invalid I2C address");
        }
        if (scl_speed_hz_ == 0) {
            throw exception("invalid I2C SCL speed");
        }

        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr_,
            .scl_speed_hz = scl_speed_hz_,
            .scl_wait_us = 0,
            .flags = {},
        };
        esp_err_t err = i2c_master_bus_add_device(bus_handle, &cfg, &i2c_dev_);
        if (err != ESP_OK) {
            throw exception("i2c_master_bus_add_device failed", err);
        }

        try {
            init_device();
            set_sample_rate(sample_rate);
        } catch (...) {
            // If the cleanup removal fails the device stays registered on the
            // bus but the only handle to it is dropped here, and the
            // constructor throws so no es7210 object exists to remove it: the
            // registration leaks until the bus is deleted.
            esp_err_t rm_err = i2c_master_bus_rm_device(i2c_dev_);   // GCOVR_EXCL_BR_LINE
            if (rm_err != ESP_OK) {
                char err_buf[32];
                esp_err_to_name_r(rm_err, err_buf, sizeof(err_buf));   // GCOVR_EXCL_BR_LINE
                ESP_LOGE(TAG,                                          // GCOVR_EXCL_BR_LINE
                         "i2c_master_bus_rm_device failed: %s",
                         err_buf);
            }
            i2c_dev_ = nullptr;
            throw;
        }

        ESP_LOGI(TAG, "ES7210 at 0x%02X initialized (%" PRIu32 " Hz)", addr_, sample_rate);
    }

    ~es7210() noexcept
    {
        if (i2c_dev_ == nullptr)
            return;
        try {
            stop();
        } catch (...) {
        }
        esp_err_t err = i2c_master_bus_rm_device(i2c_dev_);
        if (err != ESP_OK) {
            char err_buf[32];
            esp_err_to_name_r(err, err_buf, sizeof(err_buf));
            ESP_LOGE(TAG, "i2c_master_bus_rm_device failed: %s", err_buf);
        } else {
            ESP_LOGI(TAG, "ES7210 deinitialized");
        }
    }

    es7210(const es7210 &) = delete;
    es7210 &operator=(const es7210 &) = delete;
    es7210(es7210 &&other) noexcept
            : i2c_dev_(std::exchange(other.i2c_dev_, nullptr))
            , rx_handle_(std::exchange(other.rx_handle_, nullptr))
            , sample_rate_(other.sample_rate_)
            , mic_selected_(other.mic_selected_)
            , last_gain_(other.last_gain_)
            , addr_(other.addr_)
            , scl_speed_hz_(other.scl_speed_hz_)
            , started_(std::exchange(other.started_, false))
            , powered_down_(std::exchange(other.powered_down_, true))
    {
    }
    es7210 &operator=(es7210 &&other) noexcept
    {
        if (this != &other) {
            if (i2c_dev_ != nullptr) {
                if (!powered_down_) {
                    try {
                        stop();
                    } catch (...) {
                    }
                }
                esp_err_t err = i2c_master_bus_rm_device(i2c_dev_);
                if (err != ESP_OK) {
                    char err_buf[32];
                    esp_err_to_name_r(err, err_buf, sizeof(err_buf));
                    ESP_LOGE(TAG, "move assign: i2c_master_bus_rm_device failed: %s", err_buf);
                }
            }
            i2c_dev_ = std::exchange(other.i2c_dev_, nullptr);
            rx_handle_ = std::exchange(other.rx_handle_, nullptr);
            sample_rate_ = other.sample_rate_;
            mic_selected_ = other.mic_selected_;
            last_gain_ = other.last_gain_;
            addr_ = other.addr_;
            scl_speed_hz_ = other.scl_speed_hz_;
            started_ = std::exchange(other.started_, false);
            powered_down_ = std::exchange(other.powered_down_, true);
        }
        return *this;
    }

    /**
     * Whether this object still owns an initialized device.
     *
     * A moved-from object is inert: valid() returns false and every method
     * that would touch the bus (except get_sample_rate(), which only returns
     * the cached value) throws es7210::exception.
     */
    [[nodiscard]] bool valid() const noexcept
    {
        // i2c_dev_ and rx_handle_ are set together by the constructor and
        // nulled together by the move operations, so checking one is enough.
        return i2c_dev_ != nullptr;   // GCOVR_EXCL_BR_LINE: outlined EH edge
    }

    /** Get sample rate */
    [[nodiscard]] uint32_t get_sample_rate() const noexcept
    {
        return sample_rate_;   // GCOVR_EXCL_BR_LINE
    }

    /** Check if the device is an ES7210 by reading chip ID registers. */
    [[nodiscard]] bool is_7210() const
    {
        require_initialized();
        return read_reg(REG_CHIP_ID0) == 0x10 && read_reg(REG_CHIP_ID1) == 0x72;
    }

    /** Get the chip version. */
    [[nodiscard]] uint8_t get_version() const
    {
        require_initialized();
        return read_reg(REG_VERSION);   // GCOVR_EXCL_BR_LINE
    }

    /**
     * Start ADC recording (power up microphones and enable I2S).
     *
     * Must not be called before the FreeRTOS scheduler is running.
     * The chip is considered powered up as soon as the sequence starts, so a
     * failure halfway through does not make a retry a silent no-op: start()
     * throws leaving the object retryable while stop() (and the destructor)
     * still perform a full power-down.
     */
    void start()
    {
        require_initialized();
        if (started_)
            return;

        powered_down_ = false;
        write_reg(REG_CLOCK_OFF, 0x3f);
        write_reg(REG_POWER_DOWN, 0x00);
        write_reg(REG_MIC1_POWER, 0x08);
        write_reg(REG_MIC2_POWER, 0x08);
        write_reg(REG_MIC3_POWER, 0x08);
        write_reg(REG_MIC4_POWER, 0x08);
        write_reg(REG_ANALOG, 0x43);
        select_microphones(mic_selected_);

        vTaskDelay(pdMS_TO_TICKS(100));

        started_ = true;
        ESP_LOGI(TAG, "ES7210 started");
    }

    /**
     * Stop ADC recording (power down microphones).
     *
     * Also powers the chip down after a partially failed start(). If a write
     * fails halfway through, the object stays in the powered-up state so the
     * call can be retried.
     */
    void stop()
    {
        require_initialized();
        if (powered_down_)
            return;

        for (uint8_t i = 0; i < 4; i++) {
            update_reg_bit(REG_MIC1_GAIN + i, 0x10, 0x00);
        }
        write_reg(REG_MIC1_POWER, 0xff);
        write_reg(REG_MIC2_POWER, 0xff);
        write_reg(REG_MIC3_POWER, 0xff);
        write_reg(REG_MIC4_POWER, 0xff);
        write_reg(REG_MIC12_POWER, 0xff);
        write_reg(REG_MIC34_POWER, 0xff);
        write_reg(REG_ANALOG, 0xc0);
        write_reg(REG_CLOCK_OFF, 0x7f);
        write_reg(REG_POWER_DOWN, 0x07);

        started_ = false;
        powered_down_ = true;
        ESP_LOGI(TAG, "ES7210 stopped");
    }

    /**
     * Read audio data from the I2S input. Blocking.
     *
     * Only touches the I2S data path, so it may be called from a different
     * task than the control methods.
     *
     * @param buf     Container of signed samples whose value_type matches the
     *                I2S data width (16 or 32 bit)
     * @param timeout FreeRTOS ticks to wait for data
     * @return Number of bytes actually read (not samples)
     */
    template<es7210_detail::sample_container T>
    [[nodiscard]] size_t read(T &buf,                               // GCOVR_EXCL_BR_LINE
                              TickType_t timeout = portMAX_DELAY)   // GCOVR_EXCL_BR_LINE
            const
    {
        require_initialized();
        using value_type = typename T::value_type;
        static_assert(sizeof(value_type) == (I2s32Bit ? 4 : 2),
                      "value_type must match I2S data width");
        if (buf.size() == 0) {
            throw exception("empty buffer");
        }
        if (buf.size() > SIZE_MAX / sizeof(value_type)) {   // GCOVR_EXCL_BR_LINE
            throw exception("buffer too large");
        }
        size_t read_bytes = 0;
        // GCOVR_EXCL_BR_LINE: the EH edge of the library call is never
        // recorded by gcov, even though the failure path is exercised by the
        // "read reports I2S failures" test.
        esp_err_t err = i2s_channel_read(          // GCOVR_EXCL_BR_LINE
                rx_handle_,                        // GCOVR_EXCL_BR_LINE
                buf.data(),                        // GCOVR_EXCL_BR_LINE
                buf.size() * sizeof(value_type),   // GCOVR_EXCL_BR_LINE
                &read_bytes,                       // GCOVR_EXCL_BR_LINE
                timeout);                          // GCOVR_EXCL_BR_LINE
        if (err != ESP_OK) {
            throw exception("i2s_channel_read failed", err);
        }
        return read_bytes;
    }

    /**
     * Set gain for all selected microphones. May be called before start():
     * the gain is remembered and re-applied when the microphones are
     * powered up.
     */
    void set_gain(gain_value gain)
    {
        require_initialized();   // GCOVR_EXCL_BR_LINE: outlined EH edge
        last_gain_ = gain;
        apply_gain();   // GCOVR_EXCL_LINE
    }

  private:
    void apply_gain()
    {
        if ((mic_selected_ & MIC1) != 0)
            update_reg_bit(REG_MIC1_GAIN, 0x0F, last_gain_);
        if ((mic_selected_ & MIC2) != 0)
            update_reg_bit(REG_MIC2_GAIN, 0x0F, last_gain_);
        if ((mic_selected_ & MIC3) != 0)
            update_reg_bit(REG_MIC3_GAIN, 0x0F, last_gain_);
        if ((mic_selected_ & MIC4) != 0)
            update_reg_bit(REG_MIC4_GAIN, 0x0F, last_gain_);
    }

  public:
    /** Set mute state for all channels. */
    void set_mute(bool mute)
    {
        require_initialized();
        uint8_t val = mute ? 0x03 : 0x00;
        update_reg_bit(REG_MUTERANGE_L, 0x03, val);
        update_reg_bit(REG_MUTERANGE_R, 0x03, val);
        ESP_LOGI(TAG, "%s", mute ? "Muted" : "Unmuted");
    }

    /**
     * Set sample rate.
     *
     * Should be called before start(): it reprograms the clock dividers live,
     * and if a write fails halfway through the chip is left in an intermediate
     * clock configuration while get_sample_rate() keeps reporting the previous
     * rate.
     */
    void set_sample_rate(uint32_t rate)
    {
        require_initialized();
        constexpr auto mc = static_cast<uint32_t>(MclkMultiple);
        const uint32_t mclk = rate * mc;
        constexpr size_t num_rates = std::size(coeff_table);
        size_t idx = num_rates;
        for (size_t i = 0; i < num_rates; i++) {
            if (coeff_table[i].lrck == rate && coeff_table[i].mclk == mclk) {
                idx = i;
                break;
            }
        }
        if (idx == num_rates) {
            throw exception("unsupported sample rate");
        }

        const auto &coeff = coeff_table[idx];

        uint8_t regv;
        // REG_MAINCLK: adc_div, doubler, dll
        regv = coeff.adc_div;
        regv |= coeff.doubler << 6;
        regv |= coeff.dll << 7;
        write_reg(REG_MAINCLK, regv);

        // REG_OSR
        write_reg(REG_OSR, coeff.osr);

        // LRCK dividers
        write_reg(REG_LRCK_DIVH, static_cast<uint8_t>(coeff.lrck_h));
        write_reg(REG_LRCK_DIVL, static_cast<uint8_t>(coeff.lrck_l));

        sample_rate_ = rate;
        ESP_LOGI(TAG, "Sample rate set to %" PRIu32 " Hz", rate);
    }

  private:
    i2c_master_dev_handle_t i2c_dev_{};
    i2s_chan_handle_t rx_handle_{};
    uint32_t sample_rate_{};
    uint8_t mic_selected_{};
    gain_value last_gain_{ GAIN_0DB };
    uint8_t addr_{};
    uint32_t scl_speed_hz_{};
    bool started_{};
    bool powered_down_{ true };

    void require_initialized() const
    {
        if (!valid()) {
            throw exception("device is not initialized");
        }
    }

    [[nodiscard]] uint8_t read_reg(uint8_t reg) const
    {
        uint8_t val;
        esp_err_t err =
                i2c_master_transmit_receive(i2c_dev_, &reg, 1, &val, 1, I2C_TIMEOUT_MS);
        if (err != ESP_OK) {
            throw exception("read_reg failed", err);
        }
        return val;
    }

    void write_reg(uint8_t reg, uint8_t val)
    {
        uint8_t buf[2] = { reg, val };
        esp_err_t err = i2c_master_transmit(i2c_dev_, buf, sizeof(buf), I2C_TIMEOUT_MS);
        if (err != ESP_OK) {
            throw exception("write_reg failed", err);
        }
    }

    void update_reg_bit(uint8_t reg, uint8_t update_bits, uint8_t data)
    {
        uint8_t regv = read_reg(reg);
        regv = (regv & ~update_bits) | (update_bits & data);
        write_reg(reg, regv);
    }

    void select_microphones(uint8_t mics)
    {
        // Disable all gain and power first
        for (uint8_t i = 0; i < 4; i++) {
            update_reg_bit(REG_MIC1_GAIN + i, 0x10, 0x00);
        }
        write_reg(REG_MIC12_POWER, 0xff);
        write_reg(REG_MIC34_POWER, 0xff);

        if (mics & (MIC1 | MIC2)) {
            ESP_LOGI(TAG, "Enable MIC1/MIC2 group");
            update_reg_bit(REG_CLOCK_OFF, 0x0b, 0x00);
            write_reg(REG_MIC12_POWER, 0x00);
        }
        if (mics & MIC1) {
            ESP_LOGI(TAG, "Enable MIC1");
            update_reg_bit(REG_MIC1_GAIN, 0x10, 0x10);
        }
        if (mics & MIC2) {
            ESP_LOGI(TAG, "Enable MIC2");
            update_reg_bit(REG_MIC2_GAIN, 0x10, 0x10);
        }
        if (mics & (MIC3 | MIC4)) {
            ESP_LOGI(TAG, "Enable MIC3/MIC4 group");
            update_reg_bit(REG_CLOCK_OFF, 0x15, 0x00);
            write_reg(REG_MIC34_POWER, 0x00);
        }
        if (mics & MIC3) {
            ESP_LOGI(TAG, "Enable MIC3");
            update_reg_bit(REG_MIC3_GAIN, 0x10, 0x10);
        }
        if (mics & MIC4) {
            ESP_LOGI(TAG, "Enable MIC4");
            update_reg_bit(REG_MIC4_GAIN, 0x10, 0x10);
        }
        apply_gain();

        // Enable TDM mode if 3 or more mics are selected
        uint8_t mic_count = 0;
        if (mics & MIC1)
            mic_count++;
        if (mics & MIC2)
            mic_count++;
        if (mics & MIC3)
            mic_count++;
        if (mics & MIC4)
            mic_count++;

        if (mic_count >= 3) {
            write_reg(REG_SDP_INTERFACE2, 0x02);
            ESP_LOGI(TAG, "Enable TDM mode");
        } else {
            write_reg(REG_SDP_INTERFACE2, 0x00);
        }
    }

    void init_device()
    {
        // Reset
        write_reg(REG_RESET, 0xff);
        write_reg(REG_RESET, 0x41);
        // Clock off config
        write_reg(REG_CLOCK_OFF, 0x3f);
        // Set chip state cycle
        write_reg(REG_TIME_CONTROL0, 0x30);
        // Set power on state cycle
        write_reg(REG_TIME_CONTROL1, 0x30);
        // HPF quick setup
        write_reg(REG_ADC12_HPF2, 0x2a);
        write_reg(REG_ADC12_HPF1, 0x0a);
        write_reg(REG_ADC34_HPF2, 0x0a);
        write_reg(REG_ADC34_HPF1, 0x2a);
        // Slave mode (default)
        update_reg_bit(REG_MODE_CONFIG, 0x01, 0x00);
        // MCLK from pad
        update_reg_bit(REG_MASTER_CLK, 0x80, 0x00);
        // Analog setup
        write_reg(REG_MIC12_BIAS, 0x70);
        write_reg(REG_MIC34_BIAS, 0x70);
        write_reg(REG_OSR, 0x20);
        write_reg(REG_MAINCLK, 0xc1);
        // Set I2S format: I2S normal
        write_reg(REG_SDP_INTERFACE1, I2s32Bit ? 0x80 : 0x60);
    }
};
