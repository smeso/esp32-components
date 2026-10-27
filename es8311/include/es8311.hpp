/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <inttypes.h>
#include <iterator>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include <esp_err.h>
#include <esp_log.h>

#include <driver/i2c_master.h>
#include <driver/i2s_std.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/**
 * C++ driver for the ES8311 low-power audio DAC with integrated I2S interface.
 */

namespace es8311_detail {

template<typename T>
concept has_allocator = requires(const T &t) {
    typename T::allocator_type;
    typename T::allocator_type::value_type;
    { t.get_allocator() } -> std::same_as<typename T::allocator_type>;
} && std::same_as<typename T::allocator_type::value_type, typename T::value_type>;

template<typename T>
concept sample_container = requires(const T &t) {
    typename T::value_type;
    { t.size() } -> std::same_as<size_t>;
    { t.data() } -> std::same_as<const typename T::value_type *>;
} && std::is_integral_v<typename T::value_type> && std::is_signed_v<typename T::value_type>;

template<typename T>
auto
make_converted_buffer(const T &src, size_t n)   // GCOVR_EXCL_BR_LINE
{
    if constexpr (has_allocator<T>) {
        return std::vector<typename T::value_type, typename T::allocator_type>(
                n, src.get_allocator());
    } else {
        return std::vector<typename T::value_type>(n);
    }
}

}   // namespace es8311_detail

static_assert(__cplusplus >= 202002L, "es8311 requires C++20 or later");

/**
 * C++ driver for the ES8311 low-power audio DAC with integrated I2S interface.
 *
 * This class is not thread-safe: set_mute(), voice_fade() and
 * power_up()/power_down() perform non-atomic I2C read-modify-write sequences
 * and drc_config() and the power_up() variants issue multi-register write
 * sequences, so concurrent calls to the control methods must be serialized by
 * the caller. set_output_channels() mutates state read by write() and must be
 * called before audio streaming starts: write() snapshots the channel count
 * so a concurrent change cannot corrupt a conversion, but the value actually
 * used is unspecified. write() only touches the I2S data path and may safely
 * be called from a different task than the control methods.
 *
 * The power callbacks are invoked by the power_up()/power_down() family. They
 * must not throw: an exception is logged and swallowed so that it cannot
 * corrupt the power state machine. power_down() always notifies both
 * callbacks, so a DAC-only/ADC-only power-up followed by power_down() sends a
 * false to the callback that never observed a true.
 *
 * power_up() re-initializes the chip and leaves the DAC digital volume at its
 * minimum (-95.5 dB, silenced): set_volume() must be called afterwards to get
 * any output. The ADC digital volume is programmed to -95.5 dB.
 *
 * Every method that touches the chip propagates es8311::exception on an I2C
 * failure. The destructor and move assignment access the I2C bus and run the
 * full power-down sequence (up to a dozen transactions with a 100 ms timeout
 * each), so destroying or move-assigning an object must not race with another
 * task using it and can block for up to about a second on a wedged bus.
 */
template<i2s_mclk_multiple_t MclkMultiple = I2S_MCLK_MULTIPLE_256, bool I2s32Bit = false>
class es8311
{
    static constexpr const char *TAG = "es8311";

    // ES8311 register map
    static constexpr uint8_t REG_RESET = 0x00;
    static constexpr uint8_t REG_CLK1 = 0x01;
    static constexpr uint8_t REG_CLK2 = 0x02;
    static constexpr uint8_t REG_CLK3 = 0x03;
    static constexpr uint8_t REG_CLK4 = 0x04;
    static constexpr uint8_t REG_CLK5 = 0x05;
    static constexpr uint8_t REG_CLK6 = 0x06;
    static constexpr uint8_t REG_CLK7 = 0x07;
    static constexpr uint8_t REG_CLK8 = 0x08;
    static constexpr uint8_t REG_SDPIN = 0x09;
    static constexpr uint8_t REG_SDPOUT = 0x0A;
    static constexpr uint8_t REG_SYSTEM0B = 0x0B;
    static constexpr uint8_t REG_SYSTEM0C = 0x0C;
    static constexpr uint8_t REG_SYSTEM0D = 0x0D;
    static constexpr uint8_t REG_SYSTEM0E = 0x0E;
    static constexpr uint8_t REG_SYSTEM10 = 0x10;
    static constexpr uint8_t REG_SYSTEM11 = 0x11;
    static constexpr uint8_t REG_SYSTEM12 = 0x12;
    static constexpr uint8_t REG_SYSTEM13 = 0x13;
    static constexpr uint8_t REG_SYSTEM14 = 0x14;
    static constexpr uint8_t REG_ADC15 = 0x15;
    static constexpr uint8_t REG_ADC16 = 0x16;
    static constexpr uint8_t REG_ADC17 = 0x17;
    static constexpr uint8_t REG_ADC1B = 0x1B;
    static constexpr uint8_t REG_ADC1C = 0x1C;
    static constexpr uint8_t REG_DAC31 = 0x31;
    static constexpr uint8_t REG_DAC32 = 0x32;
    static constexpr uint8_t REG_DAC34 = 0x34;
    static constexpr uint8_t REG_DAC35 = 0x35;
    static constexpr uint8_t REG_DAC37 = 0x37;
    static constexpr uint8_t REG_GPIO44 = 0x44;
    static constexpr uint8_t REG_GPIO45 = 0x45;
    static constexpr uint8_t REG_I2C = 0xFA;
    static constexpr uint8_t REG_CHIP_ID1 = 0xFD;
    static constexpr uint8_t REG_CHIP_ID2 = 0xFE;
    static constexpr uint8_t REG_VERSION = 0xFF;

    static constexpr uint32_t I2C_TIMEOUT_MS = 100;

    struct coeff_div
    {
        uint32_t mclk;
        uint32_t rate;
        uint8_t pre_div;
        uint8_t pre_multi;
        uint8_t adc_div;
        uint8_t dac_div;
        uint8_t fs_mode;
        uint8_t lrck_h;
        uint8_t lrck_l;
        uint8_t bclk_div;
        uint8_t adc_osr;
        uint8_t dac_osr;
    };

    static constexpr coeff_div coeff_table[] = {
        //  mclk     rate   pre mult adc dac fs lrch lrcl bck adc_osr dac_osr
        /* 8k */
        { 12288000, 8000, 6, 1, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x20 },
        { 18432000, 8000, 3, 2, 3, 3, 0, 0x05, 0xFF, 24, 0x10, 0x20 },
        { 16384000, 8000, 8, 1, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x20 },
        { 8192000, 8000, 4, 1, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x20 },
        { 6144000, 8000, 3, 1, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x20 },
        { 4096000, 8000, 2, 1, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x20 },
        { 3072000, 8000, 1, 1, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x20 },
        { 2048000, 8000, 1, 1, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x20 },
        { 1536000, 8000, 3, 4, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x20 },
        { 1024000, 8000, 1, 2, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x20 },
        /* 11.025k */
        { 11289600, 11025, 4, 1, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x20 },
        { 5644800, 11025, 2, 1, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x20 },
        { 2822400, 11025, 1, 1, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x20 },
        { 1411200, 11025, 1, 2, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x20 },
        /* 12k */
        { 12288000, 12000, 4, 1, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x20 },
        { 6144000, 12000, 2, 1, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x20 },
        { 3072000, 12000, 1, 1, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x20 },
        { 1536000, 12000, 1, 2, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x20 },
        /* 16k */
        { 12288000, 16000, 3, 1, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x20 },
        { 18432000, 16000, 3, 2, 3, 3, 0, 0x02, 0xFF, 12, 0x10, 0x20 },
        { 16384000, 16000, 4, 1, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x20 },
        { 8192000, 16000, 2, 1, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x20 },
        { 6144000, 16000, 3, 2, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x20 },
        { 4096000, 16000, 1, 1, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x20 },
        { 3072000, 16000, 3, 4, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x20 },
        { 2048000, 16000, 1, 2, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x20 },
        { 1536000, 16000, 3, 8, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x20 },
        { 1024000, 16000, 1, 4, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x20 },
        /* 22.05k */
        { 11289600, 22050, 2, 1, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        { 5644800, 22050, 1, 1, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        { 2822400, 22050, 1, 2, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        { 1411200, 22050, 1, 4, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        /* 24k */
        { 12288000, 24000, 2, 1, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        { 18432000, 24000, 3, 1, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        { 6144000, 24000, 1, 1, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        { 3072000, 24000, 1, 2, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        { 1536000, 24000, 1, 4, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        /* 32k */
        { 12288000, 32000, 3, 2, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        { 18432000, 32000, 3, 4, 3, 3, 0, 0x02, 0xFF, 12, 0x10, 0x10 },
        { 16384000, 32000, 2, 1, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        { 8192000, 32000, 1, 1, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        { 6144000, 32000, 3, 4, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        { 4096000, 32000, 1, 2, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        { 3072000, 32000, 3, 8, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        { 2048000, 32000, 1, 4, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        { 1536000, 32000, 3, 8, 1, 1, 1, 0x00, 0x7F, 2, 0x10, 0x10 },
        { 1024000, 32000, 1, 8, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        /* 44.1k */
        { 11289600, 44100, 1, 1, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        { 5644800, 44100, 1, 2, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        { 2822400, 44100, 1, 4, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        { 1411200, 44100, 1, 8, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        /* 48k */
        { 12288000, 48000, 1, 1, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        { 18432000, 48000, 3, 2, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        { 6144000, 48000, 1, 2, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        { 3072000, 48000, 1, 4, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        { 1536000, 48000, 1, 8, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        /* 64k */
        { 12288000, 64000, 3, 4, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        { 18432000, 64000, 3, 4, 3, 3, 1, 0x01, 0x7F, 6, 0x10, 0x10 },
        { 16384000, 64000, 1, 1, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        { 8192000, 64000, 1, 2, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        { 6144000, 64000, 1, 4, 3, 3, 1, 0x01, 0x7F, 6, 0x10, 0x10 },
        { 4096000, 64000, 1, 4, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        { 3072000, 64000, 1, 8, 3, 3, 1, 0x01, 0x7F, 6, 0x10, 0x10 },
        { 2048000, 64000, 1, 8, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        { 1536000, 64000, 1, 8, 1, 1, 1, 0x00, 0xBF, 3, 0x18, 0x18 },
        { 1024000, 64000, 1, 8, 1, 1, 1, 0x00, 0x7F, 2, 0x10, 0x10 },
        /* 88.2k */
        { 11289600, 88200, 1, 2, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        { 5644800, 88200, 1, 4, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        { 2822400, 88200, 1, 8, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        { 1411200, 88200, 1, 8, 1, 1, 1, 0x00, 0x7F, 2, 0x10, 0x10 },
        /* 96k */
        { 24576000, 96000, 2, 2, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        { 12288000, 96000, 1, 2, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        { 18432000, 96000, 3, 4, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        { 6144000, 96000, 1, 4, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        { 3072000, 96000, 1, 8, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x10 },
        { 1536000, 96000, 1, 8, 1, 1, 1, 0x00, 0x7F, 2, 0x10, 0x10 },
    };

  public:
    static constexpr auto mclk_multiple = static_cast<std::size_t>(MclkMultiple);

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
     * @param bus_handle     Already initialized I2C master bus handle
     * @param tx_handle_arg  Already initialized I2S TX channel handle
     * @param sample_rate    Sample rate in Hz
     * @param dac_power_cb   Called with true after the DAC is powered up and
     *                       with false before the DAC is powered down. Must not
     *                       throw: an exception is logged and swallowed.
     * @param adc_power_cb   Called with true after the ADC is powered up and
     *                       with false before the ADC is powered down. Must not
     *                       throw: an exception is logged and swallowed.
     * @param addr           I2C device address
     * @param scl_speed_hz   I2C SCL clock speed in Hz
     */
    explicit es8311(i2c_master_bus_handle_t bus_handle,
                    i2s_chan_handle_t tx_handle_arg,
                    uint32_t sample_rate = 44100,
                    std::function<void(bool)> dac_power_cb = {},
                    std::function<void(bool)> adc_power_cb = {},
                    uint8_t addr = 0x18,
                    uint32_t scl_speed_hz = 100000)
            : tx_handle_(tx_handle_arg)
            , sample_rate_(sample_rate)
            , addr_(addr)
            , scl_speed_hz_(scl_speed_hz)
            , dac_power_cb_(std::move(dac_power_cb))
            , adc_power_cb_(std::move(adc_power_cb))
            , powered_down_(true)
    {
        if (bus_handle == nullptr) {
            throw exception("bus_handle is null");
        }
        if (tx_handle_ == nullptr) {
            throw exception("tx_handle is null");
        }
        if (addr_ != 0x18 && addr_ != 0x19) {
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
        esp_err_t err =
                i2c_master_bus_add_device(bus_handle, &cfg, &i2c_dev_);   // GCOVR_EXCL_BR_LINE
        if (err != ESP_OK) {
            throw exception("i2c_master_bus_add_device failed", err);
        }

        ESP_LOGI(TAG,   // GCOVR_EXCL_BR_LINE
                 "ES8311 at 0x%02X initialized (powered down)",
                 addr_);
    }

    ~es8311() noexcept
    {
        if (i2c_dev_ == nullptr)
            return;
        try {
            power_down();
        } catch (const std::exception &e) {   // GCOVR_EXCL_LINE: branch
            ESP_LOGW(TAG, "power_down failed in destructor: %s", e.what());
        } catch (...) {   // GCOVR_EXCL_LINE: power_down throws std::exception only
            ESP_LOGW(TAG, "power_down failed in destructor");   // GCOVR_EXCL_LINE
        }
        esp_err_t err = i2c_master_bus_rm_device(i2c_dev_);
        if (err != ESP_OK) {
            char err_buf[32];
            esp_err_to_name_r(err, err_buf, sizeof(err_buf));
            ESP_LOGE(TAG, "i2c_master_bus_rm_device failed: %s", err_buf);
        } else {
            ESP_LOGI(TAG, "ES8311 deinitialized");
        }
    }

    es8311(const es8311 &) = delete;
    es8311 &operator=(const es8311 &) = delete;
    es8311(es8311 &&other) noexcept
            : i2c_dev_(std::exchange(other.i2c_dev_, nullptr))
            , tx_handle_(std::exchange(other.tx_handle_, nullptr))
            , sample_rate_(other.sample_rate_)
            , addr_(other.addr_)
            , scl_speed_hz_(other.scl_speed_hz_)
            , output_channels_(std::exchange(other.output_channels_, 2))
            , dac_power_cb_(std::move(other.dac_power_cb_))
            , adc_power_cb_(std::move(other.adc_power_cb_))
            , powered_down_(std::exchange(other.powered_down_, true))
            , configured_(std::exchange(other.configured_, false))
    {
    }
    es8311 &operator=(es8311 &&other) noexcept
    {
        if (this != &other) {
            if (i2c_dev_ != nullptr) {
                if (!powered_down_) {
                    try {
                        power_down();
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
            tx_handle_ = std::exchange(other.tx_handle_, nullptr);
            sample_rate_ = other.sample_rate_;
            addr_ = other.addr_;
            scl_speed_hz_ = other.scl_speed_hz_;
            output_channels_ = std::exchange(other.output_channels_, 2);
            dac_power_cb_ = std::move(other.dac_power_cb_);
            adc_power_cb_ = std::move(other.adc_power_cb_);
            powered_down_ = std::exchange(other.powered_down_, true);
            configured_ = std::exchange(other.configured_, false);
        }
        return *this;
    }

    /**
     * Whether this object still owns an initialized device.
     *
     * A moved-from object is inert: valid() returns false and every method
     * that would touch the bus throws es8311::exception.
     */
    [[nodiscard]] bool valid() const noexcept
    {
        // i2c_dev_ and tx_handle_ are set together by the constructor and
        // nulled together by the move operations, so checking one is enough.
        return i2c_dev_ != nullptr;   // GCOVR_EXCL_BR_LINE: outlined EH edge
    }

    /** Get sample rate */
    [[nodiscard]] uint32_t get_sample_rate() const noexcept
    {
        return sample_rate_;   // GCOVR_EXCL_BR_LINE
    }

    /** Get output channels */
    [[nodiscard]] uint8_t get_output_channels() const noexcept
    {
        return output_channels_;   // GCOVR_EXCL_BR_LINE
    }

    /**
     * Set the number of interleaved channels produced by write().
     *
     * This must match the number of slots configured on the I2S TX channel
     * (e.g. 4 for a TDM setup). It may be any value > 0. It must be called
     * before audio streaming starts: write() snapshots the value, so a
     * concurrent change cannot corrupt a conversion but the value used is
     * unspecified.
     */
    void set_output_channels(uint8_t ch)
    {
        if (ch == 0) {   // GCOVR_EXCL_BR_LINE
            throw exception("invalid output channel count");
        }
        output_channels_ = ch;   // GCOVR_EXCL_BR_LINE
    }   // GCOVR_EXCL_LINE

    /** Get current DAC volume (raw 0-255). */
    [[nodiscard]] uint8_t get_volume_raw() const
    {
        return read_reg(REG_DAC32);   // GCOVR_EXCL_BR_LINE
    }

    /** Set DAC volume (raw 0-255). */
    void set_volume_raw(uint8_t vol)
    {
        write_reg(REG_DAC32, vol);   // GCOVR_EXCL_BR_LINE
    }

    /** Get DAC volume in percent (0-100). */
    [[nodiscard]] uint8_t get_volume() const
    {
        return static_cast<uint8_t>((static_cast<uint16_t>(get_volume_raw()) * 100U + 127U) /
                                    255U);
    }

    /** Set DAC volume in percent (0-100). */
    void set_volume(uint8_t pct)
    {
        pct = pct > 100 ? 100 : pct;
        set_volume_raw(static_cast<uint8_t>(                  // GCOVR_EXCL_BR_LINE
                (static_cast<uint16_t>(pct) * 255U + 50U) /   // GCOVR_EXCL_BR_LINE
                100U));                                       // GCOVR_EXCL_BR_LINE
    }

    /** Get ADC digital volume (raw 0-255). */
    [[nodiscard]] uint8_t get_adc_volume_raw() const
    {
        return read_reg(REG_ADC17);   // GCOVR_EXCL_BR_LINE
    }

    /** Set ADC digital volume (raw 0-255). */
    void set_adc_volume_raw(uint8_t vol)
    {
        write_reg(REG_ADC17, vol);   // GCOVR_EXCL_BR_LINE
    }

    /** Get ADC volume in percent (0-100). */
    [[nodiscard]] uint8_t get_adc_volume() const
    {
        return static_cast<uint8_t>(
                (static_cast<uint16_t>(get_adc_volume_raw()) * 100U + 127U) / 255U);
    }

    /** Set ADC volume in percent (0-100). */
    void set_adc_volume(uint8_t pct)
    {
        pct = pct > 100 ? 100 : pct;
        set_adc_volume_raw(static_cast<uint8_t>(              // GCOVR_EXCL_BR_LINE
                (static_cast<uint16_t>(pct) * 255U + 50U) /   // GCOVR_EXCL_BR_LINE
                100U));                                       // GCOVR_EXCL_BR_LINE
    }

    /** Get mute state. */
    [[nodiscard]] bool get_mute() const
    {
        return (read_reg(REG_DAC31) & 0x60) != 0;   // GCOVR_EXCL_BR_LINE
    }

    /** Set mute state. */
    void set_mute(bool mute)
    {
        uint8_t val = read_reg(REG_DAC31);
        if (mute)
            val |= 0x60;
        else
            val &= 0x9F;
        write_reg(REG_DAC31, val);
    }

    /**
     * Configure DRC (Dynamic Range Control).
     *
     * @param enable    true to enable DRC
     * @param winsize   Window size (0..15): cnt_timer[DRC_WINSIZE];
     *                  0=0.25dB/2LRCK ... 15=0.25dB/65536LRCK
     * @param maxlevel  Target max level (0..15): 0=-30.1dB ... 15=-6.0dB
     * @param minlevel  Target min level (0..15): 0=-30.1dB ... 15=-6.0dB
     *
     * All the parameters are validated and the function throws on values
     * outside their documented range.
     */
    void drc_config(bool enable, uint8_t winsize, uint8_t maxlevel, uint8_t minlevel)
    {
        if (winsize > 0x0F || maxlevel > 0x0F || minlevel > 0x0F) {
            throw exception("invalid DRC parameters");
        }
        uint8_t reg35 = (maxlevel << 4) | minlevel;
        write_reg(REG_DAC35, reg35);

        uint8_t reg34 = winsize;
        if (enable)
            reg34 |= 0x80;
        write_reg(REG_DAC34, reg34);
    }

    /**
     * Set voice fade (0..15, higher is slower).
     *
     * @throw es8311::exception if fade is greater than 15
     */
    void voice_fade(uint8_t fade)
    {
        if (fade > 0x0F) {
            throw exception("invalid voice fade value");
        }
        uint8_t reg = read_reg(REG_DAC37);
        reg &= 0x0F;
        reg |= fade << 4;
        write_reg(REG_DAC37, reg);
    }

    /**
     * Write audio samples to the I2S output. Blocking.
     *
     * When the number of input channels differs from the configured output
     * channel count a temporary conversion buffer is heap-allocated for the
     * duration of the call, so this path is not allocation-free.
     *
     * @param buf       Container of signed integral samples whose value_type
     *                  matches the I2S data width (16 or 32 bit)
     * @param channels  Number of interleaved channels in buf (must not be 0)
     * @param timeout   FreeRTOS ticks to wait for the I2S driver; the default
     *                  portMAX_DELAY blocks until all the data is accepted
     * @return Number of bytes actually written (not samples)
     *
     * @throw es8311::exception if channels is 0, if the buffer is not a whole
     *        number of frames, if the buffer is too large, if the frame
     *        conversion cannot be allocated, or if the I2S write fails.
     */
    template<es8311_detail::sample_container T>
    [[nodiscard]] size_t write(const T &buf,
                               uint8_t channels = 2,
                               TickType_t timeout = portMAX_DELAY) const
    {
        using value_type = typename T::value_type;
        static_assert(sizeof(value_type) == (I2s32Bit ? 4 : 2),
                      "value_type must match I2S data width");
        if (tx_handle_ == nullptr) {
            throw exception("tx_handle is null");
        }
        if (channels == 0) {
            throw exception("invalid channel count");
        }
        const size_t samples = buf.size();
        if (samples % channels != 0) {
            throw exception("buffer is not a whole number of frames");
        }
        // Snapshot the configured channel count: a concurrent
        // set_output_channels() must not be able to change the value between
        // the conversion buffer allocation and the indexing loop below.
        const uint8_t out_channels = output_channels_;
        if (channels != out_channels) {
            const size_t frames = samples / channels;
            if (frames > SIZE_MAX / out_channels) {             // GCOVR_EXCL_BR_LINE
                throw exception("frame conversion overflow");   // GCOVR_EXCL_LINE
            }   // GCOVR_EXCL_BR_LINE
            auto converted = convert_channels(buf, frames, channels, out_channels);
            size_t written = 0;
            esp_err_t err = i2s_channel_write(               // GCOVR_EXCL_BR_LINE
                    tx_handle_,                              // GCOVR_EXCL_BR_LINE
                    converted.data(),                        // GCOVR_EXCL_BR_LINE
                    converted.size() * sizeof(value_type),   // GCOVR_EXCL_BR_LINE
                    &written,
                    timeout);
            if (err != ESP_OK) {
                if (written > 0) {   // GCOVR_EXCL_BR_LINE
                    ESP_LOGW(TAG,    // GCOVR_EXCL_LINE
                             "i2s_channel_write failed after %zu bytes",
                             written);   // GCOVR_EXCL_LINE
                }   // GCOVR_EXCL_BR_LINE
                throw exception("i2s_channel_write failed", err);
            }
            return written;
        }
        if (samples > SIZE_MAX / sizeof(value_type)) {   // GCOVR_EXCL_BR_LINE
            throw exception("buffer size overflow");     // GCOVR_EXCL_LINE
        }   // GCOVR_EXCL_BR_LINE
        size_t written = 0;
        esp_err_t err = i2s_channel_write(      // GCOVR_EXCL_BR_LINE
                tx_handle_,                     // GCOVR_EXCL_BR_LINE
                buf.data(),                     // GCOVR_EXCL_BR_LINE
                samples * sizeof(value_type),   // GCOVR_EXCL_BR_LINE
                &written,
                timeout);
        if (err != ESP_OK) {
            if (written > 0) {   // GCOVR_EXCL_BR_LINE
                ESP_LOGW(TAG,
                         "i2s_channel_write failed after %zu bytes",
                         written);   // GCOVR_EXCL_LINE
            }   // GCOVR_EXCL_BR_LINE
            throw exception("i2s_channel_write failed", err);
        }
        return written;
    }

    /**
     * Power down the ES8311.
     *
     * powered_down_ is set before the register sequence and the power
     * callbacks run before it: if a callback or a write fails midway the chip
     * may be left partially configured, but the device is considered powered
     * down and a subsequent power_up() (or the destructor) still performs a
     * full re-initialization/power-down. The callbacks are therefore invoked
     * at most once per power cycle.
     */
    void power_down()
    {
        if (powered_down_)
            return;
        powered_down_ = true;
        configured_ = false;
        notify_power(dac_power_cb_, false);
        notify_power(adc_power_cb_, false);
        set_mute(true);
        // REG_SYSTEM0E: disable ADC analog, disable output stage
        write_reg(REG_SYSTEM0E, 0xC0);
        // REG_SYSTEM0D: power down ADC / DAC analog
        write_reg(REG_SYSTEM0D, 0xF8);
        // REG_SYSTEM12: disable DAC
        write_reg(REG_SYSTEM12, 0xFF);
        // REG_SYSTEM14: clear ADC volume
        write_reg(REG_SYSTEM14, 0x00);
        // REG_RESET (0x00): power off
        write_reg(REG_RESET, 0x00);
        // REG_CLK1: disable clocks
        write_reg(REG_CLK1, 0x00);
        // REG_I2C (0xFA): reset all registers to default
        write_reg(REG_I2C, 0x01);
        write_reg(REG_I2C, 0x00);
        ESP_LOGI(TAG, "ES8311 powered down");
    }

    /**
     * Power up the ES8311 after a previous power_down.
     *
     * May be retried after a failure: a partially failed power-up leaves the
     * object powered down as far as power_up() is concerned, while the
     * destructor (and power_down()) still reset the touched chip.
     */
    void power_up()
    {
        if (configured_)
            return;
        init_device(sample_rate_, mode::dac_adc);
        configured_ = true;
        notify_power(adc_power_cb_, true);
        notify_power(dac_power_cb_, true);
    }

    /** Power up the ES8311 DAC after a previous power_down. */
    void power_up_dac_only()
    {
        if (configured_)
            return;
        init_device(sample_rate_, mode::dac);
        configured_ = true;
        notify_power(dac_power_cb_, true);
    }

    /** Power up the ES8311 ADC after a previous power_down. */
    void power_up_adc_only()
    {
        if (configured_)
            return;
        init_device(sample_rate_, mode::adc);
        configured_ = true;
        notify_power(adc_power_cb_, true);
    }

    /** Check if the device is an ES8311 by reading chip ID registers. */
    [[nodiscard]] bool is_8311() const
    {
        return read_reg(REG_CHIP_ID1) == 0x83 && read_reg(REG_CHIP_ID2) == 0x11;
    }

    /** Check whether the codec is currently powered down. */
    [[nodiscard]] bool is_powered_down() const noexcept
    {
        return powered_down_;   // GCOVR_EXCL_BR_LINE
    }

    /** Get the chip version. */
    [[nodiscard]] uint8_t get_version() const
    {
        return read_reg(REG_VERSION);   // GCOVR_EXCL_BR_LINE
    }

  private:
    /** Which audio path to configure. */
    enum class mode : uint8_t
    {
        dac,
        adc,
        dac_adc,
    };

    i2c_master_dev_handle_t i2c_dev_{};
    i2s_chan_handle_t tx_handle_{};
    uint32_t sample_rate_{};
    uint8_t addr_{};
    uint32_t scl_speed_hz_{};
    uint8_t output_channels_{ 2 };
    std::function<void(bool)> dac_power_cb_{};
    std::function<void(bool)> adc_power_cb_{};
    bool powered_down_{ true };
    bool configured_{};

    void require_initialized() const
    {
        if (!valid()) {
            throw exception("device is not initialized");
        }
    }

    [[nodiscard]] uint8_t read_reg(uint8_t reg) const
    {
        require_initialized();
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
        require_initialized();
        uint8_t buf[2] = { reg, val };
        esp_err_t err = i2c_master_transmit(i2c_dev_, buf, sizeof(buf), I2C_TIMEOUT_MS);
        if (err != ESP_OK) {
            throw exception("write_reg failed", err);
        }
    }

    // Invoke a power callback, if any. Callbacks that throw must not be able
    // to corrupt the power state machine, so their exception is logged and
    // swallowed here.
    static void notify_power(const std::function<void(bool)> &cb, bool on) noexcept
    {
        if (!cb) {
            return;
        }
        try {
            cb(on);
        } catch (const std::exception &e) {                        // GCOVR_EXCL_LINE: branch
            ESP_LOGW(TAG, "power callback threw: %s", e.what());   // GCOVR_EXCL_LINE
        } catch (...) {                                            // GCOVR_EXCL_LINE: branch
            ESP_LOGW(TAG, "power callback threw");                 // GCOVR_EXCL_LINE
        }
    }

    // Allocate and fill the channel-converted buffer used by write() when the
    // input and output channel counts differ. The caller guarantees that
    // frames * out_channels does not overflow.
    template<es8311_detail::sample_container T>
    [[nodiscard]] static auto convert_channels(const T &buf,
                                               size_t frames,
                                               uint8_t channels,
                                               uint8_t out_channels)
    {
        using value_type = typename T::value_type;
        auto converted = [&]() {
            try {
                return es8311_detail::make_converted_buffer(             // GCOVR_EXCL_BR_LINE
                        buf,                                             // GCOVR_EXCL_BR_LINE
                        frames * out_channels);                          // GCOVR_EXCL_BR_LINE
            } catch (const std::bad_alloc &) {                           // GCOVR_EXCL_LINE
                throw exception("frame conversion allocation failed");   // GCOVR_EXCL_LINE
            } catch (const std::length_error &) {                        // GCOVR_EXCL_LINE
                throw exception("frame conversion allocation failed");   // GCOVR_EXCL_LINE
            }
        }();
        // Insert padding between channels or skip them
        const uint8_t min_channels = std::min(channels, out_channels);
        const value_type *src = buf.data();
        for (size_t f = 0; f < frames; ++f) {
            for (uint8_t c = 0; c < min_channels; ++c) {   // GCOVR_EXCL_BR_LINE
                converted[f * out_channels + c] =
                        src[f * channels + c];   // GCOVR_EXCL_BR_LINE
            }   // GCOVR_EXCL_BR_LINE
        }
        return converted;
    }

    void set_sample_rate(uint32_t rate, mode m)
    {
        constexpr auto mc = static_cast<uint32_t>(MclkMultiple);
        if (rate > UINT32_MAX / mc) {
            throw exception("unsupported sample rate");
        }
        const uint32_t mclk = rate * mc;
        constexpr size_t num_rates = std::size(coeff_table);
        size_t idx = num_rates;
        for (size_t i = 0; i < num_rates; i++) {
            if (coeff_table[i].rate == rate && coeff_table[i].mclk == mclk) {
                idx = i;
                break;
            }
        }
        if (idx == num_rates) {
            throw exception("unsupported sample rate");
        }

        const auto &coeff = coeff_table[idx];
        uint8_t regv;

        const bool adc = m == mode::adc || m == mode::dac_adc;
        const bool dac = m == mode::dac || m == mode::dac_adc;

        // REG_CLK2: set pre_div (MCLK prescaler divider, bits [7:5]) and
        // pre_multi (MCLK frequency multiplier, bits [4:3])
        regv = read_reg(REG_CLK2);
        regv &= 0x07;
        regv |= (coeff.pre_div - 1) << 5;
        uint8_t pre_multi_bits = 0;
        switch (coeff.pre_multi) {
        case 2:
            pre_multi_bits = 1;
            break;
        case 4:
            pre_multi_bits = 2;
            break;
        case 8:
            pre_multi_bits = 3;
            break;
        default:
            break;
        }
        regv |= pre_multi_bits << 3;
        write_reg(REG_CLK2, regv);

        // REG_CLK5: set the ADC clock divider (bits [7:4]) and the DAC clock
        // divider (bits [3:0]). The register carries both dividers, so it must
        // be programmed whenever either path is enabled: a DAC-only power-up
        // needs the DAC divider too.
        regv = 0;
        if (adc) {
            regv |= (coeff.adc_div - 1) << 4;
        }
        if (dac) {
            regv |= coeff.dac_div - 1;
        }
        write_reg(REG_CLK5, regv);

        if (adc) {
            // REG_CLK3: set fs_mode (sample rate mode, bit 6) and ADC oversampling rate (bits
            // [5:0])
            regv = read_reg(REG_CLK3);
            regv &= 0x80;
            regv |= coeff.fs_mode << 6;
            regv |= coeff.adc_osr;
            write_reg(REG_CLK3, regv);
        }

        if (dac) {
            // REG_CLK4: set DAC oversampling rate (bits [6:0])
            regv = read_reg(REG_CLK4);
            regv &= 0x80;
            regv |= coeff.dac_osr;
            write_reg(REG_CLK4, regv);
        }

        // REG_CLK7 / REG_CLK8: set LRCK divider
        regv = read_reg(REG_CLK7);
        regv &= 0xC0;
        regv |= coeff.lrck_h;
        write_reg(REG_CLK7, regv);
        write_reg(REG_CLK8, coeff.lrck_l);

        // REG_CLK6: set BCLK divider (bits [4:0])
        regv = read_reg(REG_CLK6);
        regv &= 0xE0;
        if (coeff.bclk_div < 19) {
            regv |= coeff.bclk_div - 1;
        } else {
            regv |= coeff.bclk_div;
        }
        write_reg(REG_CLK6, regv);

        ESP_LOGI(TAG, "Sample rate set to %" PRIu32 " Hz", rate);
    }

    void init_device(uint32_t sample_rate, mode m)
    {
        // From the first register access on the chip is considered live: a
        // failure half-way through must still let the destructor (and a
        // subsequent power_down()) perform the full power-down sequence
        // instead of silently dropping a live device. configured_ is left
        // false so that a retried power_up() re-runs the sequence instead of
        // becoming a silent no-op.
        powered_down_ = false;
        init_device_impl(sample_rate, m);
    }

    void init_device_impl(uint32_t sample_rate, mode m)
    {
        const bool adc = m == mode::adc || m == mode::dac_adc;
        const bool dac = m == mode::dac || m == mode::dac_adc;
        // REG_I2C (0xFA): reset all registers to default
        write_reg(REG_I2C, 0x01);
        // REG_I2C (0xFA): de-assert reset
        write_reg(REG_I2C, 0x00);
        // REG_GPIO44: set I2C_WL: undocumented feature to improve I2C reliability
        write_reg(REG_GPIO44, 0x08);
        // Second time's the charm
        write_reg(REG_GPIO44, 0x08);
        // REG_CLK1: enable MCLK and BCLK
        write_reg(REG_CLK1, 0x30);
        // REG_CLK2: clear clock dividers to defaults
        write_reg(REG_CLK2, 0x00);
        if (adc) {
            // REG_CLK3: set ADC OSR to default (no fs_mode bits)
            write_reg(REG_CLK3, 0x10);
            // REG_ADC16: ADC gain and sync
            write_reg(REG_ADC16, 0x24);
        }
        if (dac) {
            // REG_CLK4: set DAC OSR to default
            write_reg(REG_CLK4, 0x10);
        }
        // REG_CLK5: clear ADC/DAC clock divider to 1
        write_reg(REG_CLK5, 0x00);
        // REG_SYSTEM0B: PWRUP defaults
        write_reg(REG_SYSTEM0B, 0x00);
        // REG_SYSTEM0C: PWRUP defaults
        write_reg(REG_SYSTEM0C, 0x00);
        // REG_SYSTEM10: bias current setting
        write_reg(REG_SYSTEM10, 0x1F);
        // REG_SYSTEM11: bias voltage setting
        write_reg(REG_SYSTEM11, 0x7F);
        // REG_RESET (0x00): power-on
        write_reg(REG_RESET, 0x80);

        if (dac && adc) {
            // REG_CLK1: Turn on DAC and ADC clocks
            write_reg(REG_CLK1, 0x3f);
        } else if (dac) {
            // REG_CLK1: Turn on DAC clocks
            write_reg(REG_CLK1, 0x35);
        } else {
            // REG_CLK1: Turn on ADC clocks
            write_reg(REG_CLK1, 0x3a);
        }

        // REG_CLK6: BCLK not inverted
        auto regv = read_reg(REG_CLK6);
        regv &= ~0x20;
        write_reg(REG_CLK6, regv);

        set_sample_rate(sample_rate, m);

        if (dac) {
            // REG_SDPIN (0x09): I2S input, I2S format, unmute, left to DAC
            write_reg(REG_SDPIN, I2s32Bit ? 0x10 : 0x0C);
            // REG_SYSTEM13: Enable output to HP drive
            write_reg(REG_SYSTEM13, 0x10);
            // REG_SYSTEM12: Enable DAC
            write_reg(REG_SYSTEM12, 0x00);
            // REG_DAC37: Bypass DAC equalizer and disable DAC softramp
            write_reg(REG_DAC37, 0x08);
            // REG_DAC32: DAC digital volume (-95.5 dB)
            write_reg(REG_DAC32, 0x00);
        }
        if (adc) {
            // REG_SDPOUT (0x0A): I2S output, I2S format, unmute
            write_reg(REG_SDPOUT, I2s32Bit ? 0x10 : 0x0C);
            // REG_ADC1B: ADC analog automute gain and HPFS1
            write_reg(REG_ADC1B, 0x0A);
            // REG_ADC1C: ADC Equalizer bypass and dynamic HPF enabled
            write_reg(REG_ADC1C, 0x6A);
            // REG_GPIO44: Enable digital feedback ADC + DACR
            write_reg(REG_GPIO44, 0x58);
            // REG_ADC17: ADC digital volume, 0 dB reference (same scale as
            // REG_DAC32: 0x00 = -95.5 dB, 0xBF = 0 dB)
            write_reg(REG_ADC17, 0x00);
            // REG_SYSTEM0E: Enable analog PGA, enable ADC modulator, low power mode
            write_reg(REG_SYSTEM0E, 0x02);
            // REG_SYSTEM14: ADC PGA GAIN 30db, MIC selection
            write_reg(REG_SYSTEM14, 0x1A);
            // REG_ADC15: ADC soft ramp 0.25dB/32LRCK
            write_reg(REG_ADC15, 0x40);
        }

        if (dac && adc) {
            // REG_SYSTEM0D: power up analog circuits
            write_reg(REG_SYSTEM0D, 0x01);
        } else if (dac) {
            // REG_SYSTEM0D: power up analog circuits
            write_reg(REG_SYSTEM0D, 0x31);
        } else {
            // REG_SYSTEM0D: power up analog circuits
            write_reg(REG_SYSTEM0D, 0x09);
        }

        // REG_GPIO45: I2S LRCK pull up ON
        write_reg(REG_GPIO45, 0x00);

        if (dac) {
            ESP_LOGI(TAG, "ES8311 DAC initialized");
        }
        if (adc) {
            ESP_LOGI(TAG, "ES8311 ADC initialized");
        }
    }
};
