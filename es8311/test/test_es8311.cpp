/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include <doctest/doctest.h>
#include "es8311.hpp"
#include "mock_bus.hpp"

using namespace mock_bus;

namespace {

// Handles that pass the constructor's null checks but are never dereferenced:
// the I2C/I2S driver entry points are redirected to the mocks in
// mock_bus.cpp (components/utils/mocks) by the linker --wrap options in its
// CMakeLists.txt.
const auto kBus = reinterpret_cast<i2c_master_bus_handle_t>(0x1);
const auto kTx = reinterpret_cast<i2s_chan_handle_t>(0x2);

// ES8311 register addresses (mirrors the private constants in es8311.hpp).
constexpr uint8_t REG_RESET = 0x00;
constexpr uint8_t REG_CLK1 = 0x01;
constexpr uint8_t REG_CLK2 = 0x02;
constexpr uint8_t REG_CLK3 = 0x03;
constexpr uint8_t REG_CLK4 = 0x04;
constexpr uint8_t REG_CLK5 = 0x05;
constexpr uint8_t REG_CLK6 = 0x06;
constexpr uint8_t REG_CLK7 = 0x07;
constexpr uint8_t REG_CLK8 = 0x08;
constexpr uint8_t REG_SDPIN = 0x09;
constexpr uint8_t REG_SDPOUT = 0x0A;
constexpr uint8_t REG_SYSTEM0B = 0x0B;
constexpr uint8_t REG_SYSTEM0C = 0x0C;
constexpr uint8_t REG_SYSTEM0D = 0x0D;
constexpr uint8_t REG_SYSTEM0E = 0x0E;
constexpr uint8_t REG_SYSTEM10 = 0x10;
constexpr uint8_t REG_SYSTEM11 = 0x11;
constexpr uint8_t REG_SYSTEM12 = 0x12;
constexpr uint8_t REG_SYSTEM13 = 0x13;
constexpr uint8_t REG_SYSTEM14 = 0x14;
constexpr uint8_t REG_ADC15 = 0x15;
constexpr uint8_t REG_ADC16 = 0x16;
constexpr uint8_t REG_ADC17 = 0x17;
constexpr uint8_t REG_ADC1B = 0x1B;
constexpr uint8_t REG_ADC1C = 0x1C;
constexpr uint8_t REG_DAC31 = 0x31;
constexpr uint8_t REG_DAC32 = 0x32;
constexpr uint8_t REG_DAC34 = 0x34;
constexpr uint8_t REG_DAC35 = 0x35;
constexpr uint8_t REG_DAC37 = 0x37;
constexpr uint8_t REG_GPIO44 = 0x44;
constexpr uint8_t REG_GPIO45 = 0x45;
constexpr uint8_t REG_I2C = 0xFA;
constexpr uint8_t REG_CHIP_ID1 = 0xFD;
constexpr uint8_t REG_CHIP_ID2 = 0xFE;
constexpr uint8_t REG_VERSION = 0xFF;

// Helper that forwards to the es8311 constructor. All the arguments used in
// the tests below fail validation before any hardware access happens.
void
make_es8311(i2c_master_bus_handle_t bus, i2s_chan_handle_t tx, uint8_t addr = 0x18)
{
    es8311<>(bus, tx, 44100, {}, {}, addr);
}

struct power_calls
{
    std::vector<bool> dac;
    std::vector<bool> adc;
};

template<typename T>
T
make_device(power_calls &calls, uint32_t rate = 44100, uint8_t addr = 0x18)
{
    return T(
            kBus,
            kTx,
            rate,
            [&calls](bool on) { calls.dac.push_back(on); },
            [&calls](bool on) { calls.adc.push_back(on); },
            addr);
}

// Long-lived callbacks storage for tests that do not inspect the power
// callbacks.
power_calls &
power_calls_holder()
{
    static power_calls calls;
    return calls;
}

// Container whose size() overflows write()'s size/conversion guards. Kept at
// namespace scope (rather than local to the test) so the -Werror build does
// not complain about a locally defined value_type typedef.
struct huge_i16_buffer
{
    using value_type = int16_t;
    int16_t data_[1]{};
    const int16_t *data() const
    {
        return data_;
    }
    size_t size() const
    {
        return static_cast<size_t>(-1) - 1;
    }
};

}   // namespace

TEST_CASE("template parameters are reflected in the driver")
{
    static_assert(es8311<>::mclk_multiple == 256);
    static_assert(es8311<I2S_MCLK_MULTIPLE_512, true>::mclk_multiple == 512);
    CHECK(es8311<>::mclk_multiple == 256);
    CHECK(es8311<I2S_MCLK_MULTIPLE_512, true>::mclk_multiple == 512);
}

TEST_CASE("only signed integral sample containers satisfy the concept")
{
    static_assert(es8311_detail::sample_container<std::vector<int16_t>>);
    static_assert(es8311_detail::sample_container<std::vector<int32_t>>);
    // Floating point types are signed but their raw IEEE-754 bit patterns are
    // not valid I2S samples.
    static_assert(!es8311_detail::sample_container<std::vector<float>>);
    static_assert(!es8311_detail::sample_container<std::vector<double>>);
    // Unsigned samples are rejected too.
    static_assert(!es8311_detail::sample_container<std::vector<uint16_t>>);
    CHECK(es8311_detail::sample_container<std::vector<int16_t>>);
    CHECK_FALSE(es8311_detail::sample_container<std::vector<float>>);
}

TEST_CASE("es8311 is movable but not copyable")
{
    static_assert(!std::is_copy_constructible_v<es8311<>>);
    static_assert(!std::is_copy_assignable_v<es8311<>>);
    static_assert(std::is_move_constructible_v<es8311<>>);
    static_assert(std::is_nothrow_move_constructible_v<es8311<>>);
    static_assert(std::is_nothrow_move_assignable_v<es8311<>>);
}

TEST_CASE("exception derives from std::exception")
{
    static_assert(std::is_base_of_v<std::exception, es8311<>::exception>);
    static_assert(std::is_move_constructible_v<es8311<>::exception>);
    static_assert(std::is_move_assignable_v<es8311<>::exception>);
    static_assert(std::is_copy_constructible_v<es8311<>::exception>);
}

TEST_CASE("exception includes the message and the error name")
{
    es8311<>::exception plain("boom");
    CHECK(std::strcmp(plain.what(), "es8311: boom") == 0);

    es8311<>::exception with_err("i2c failed", ESP_ERR_INVALID_ARG);
    std::string msg(with_err.what());
    CHECK(msg.find("es8311: i2c failed") == 0);
    CHECK(msg.find("ESP_ERR_INVALID_ARG") != std::string::npos);
}

TEST_CASE("constructor rejects a null I2C bus handle")
{
    CHECK_THROWS_AS(make_es8311(nullptr, nullptr), es8311<>::exception);
}

TEST_CASE("constructor rejects a null I2S TX handle")
{
    CHECK_THROWS_AS(make_es8311(reinterpret_cast<i2c_master_bus_handle_t>(0x1), nullptr),
                    es8311<>::exception);
}

TEST_CASE("constructor rejects an invalid I2C address")
{
    CHECK_THROWS_AS(make_es8311(reinterpret_cast<i2c_master_bus_handle_t>(0x1),
                                reinterpret_cast<i2s_chan_handle_t>(0x2),
                                0x20),
                    es8311<>::exception);
}

TEST_CASE("constructor reports i2c_master_bus_add_device failures")
{
    mock_i2c_reset();
    mock_i2c_fail_add_device(true);
    CHECK_THROWS_AS(es8311<>(kBus, kTx), es8311<>::exception);
    mock_i2c_fail_add_device(false);
}

TEST_CASE("make_converted_buffer allocates the requested number of elements")
{
    std::vector<int16_t> src{};
    auto buf = es8311_detail::make_converted_buffer(src, 8);
    CHECK(buf.size() == 8);
    static_assert(std::is_same_v<decltype(buf), std::vector<int16_t>>);

    struct custom_buffer
    {
        using value_type = uint32_t;
    };
    auto fallback = es8311_detail::make_converted_buffer(custom_buffer{}, 4);
    CHECK(fallback.size() == 4);
    static_assert(std::is_same_v<decltype(fallback), std::vector<uint32_t>>);
}

TEST_CASE("constructor rejects a zero I2C SCL speed")
{
    CHECK_THROWS_AS((es8311<>(kBus, kTx, 44100, {}, {}, 0x18, 0)), es8311<>::exception);
}

TEST_CASE("constructor registers the I2C device")
{
    mock_i2c_reset();
    {
        power_calls calls;
        auto dev = make_device<es8311<>>(calls);
        CHECK(mock_i2c_add_device_count() == 1);
        CHECK(mock_i2c_last_device_address() == 0x18);
        CHECK(dev.get_sample_rate() == 44100);
        CHECK(dev.get_output_channels() == 2);
        CHECK(calls.dac.empty());
        CHECK(calls.adc.empty());
    }
    CHECK(mock_i2c_rm_device_count() == 1);
}

TEST_CASE("constructor accepts the 0x19 address")
{
    mock_i2c_reset();
    {
        power_calls calls;
        auto dev = make_device<es8311<>>(calls, 44100, 0x19);
        CHECK(mock_i2c_last_device_address() == 0x19);
    }
}

TEST_CASE("power_up configures DAC and ADC and notifies callbacks")
{
    mock_i2c_reset();
    power_calls calls;
    {
        auto dev = make_device<es8311<>>(calls);
        // A freshly constructed device is already powered down: no-op.
        dev.power_down();

        dev.power_up();
        CHECK(calls.dac == std::vector<bool>{ true });
        CHECK(calls.adc == std::vector<bool>{ true });

        // I2C soft reset of all registers.
        CHECK(mock_i2c_wrote(REG_I2C, 0x01));
        CHECK(mock_i2c_wrote(REG_I2C, 0x00));
        // I2C reliability workaround.
        CHECK(mock_i2c_wrote(REG_GPIO44, 0x08));
        // Power-on.
        CHECK(mock_i2c_wrote(REG_RESET, 0x80));
        // DAC + ADC clocks.
        CHECK(mock_i2c_wrote(REG_CLK1, 0x3F));
        // I2S input/output format, unmuted.
        CHECK(mock_i2c_wrote(REG_SDPIN, 0x0C));
        CHECK(mock_i2c_wrote(REG_SDPOUT, 0x0C));
        // DAC enabled, ADC enabled, analog powered up.
        CHECK(mock_i2c_wrote(REG_SYSTEM12, 0x00));
        CHECK(mock_i2c_wrote(REG_SYSTEM0D, 0x01));
        // Power-up register defaults.
        CHECK(mock_i2c_wrote(REG_SYSTEM0B, 0x00));
        CHECK(mock_i2c_wrote(REG_SYSTEM0C, 0x00));
        CHECK(mock_i2c_wrote(REG_SYSTEM10, 0x1F));
        CHECK(mock_i2c_wrote(REG_SYSTEM11, 0x7F));
        CHECK(mock_i2c_wrote(REG_SYSTEM13, 0x10));
        CHECK(mock_i2c_wrote(REG_GPIO45, 0x00));
        // ADC path registers.
        CHECK(mock_i2c_wrote(REG_ADC15, 0x40));
        CHECK(mock_i2c_wrote(REG_ADC16, 0x24));
        // REG_ADC17 shares the REG_DAC32 scale (0x00 = -95.5 dB, 0xBF = 0 dB)
        // and is left at the minimum after power-up.
        CHECK(mock_i2c_wrote(REG_ADC17, 0x00));
        CHECK(mock_i2c_wrote(REG_ADC1B, 0x0A));
        CHECK(mock_i2c_wrote(REG_ADC1C, 0x6A));
        // 44.1 kHz at MCLK 256: 12.288 MHz coefficient.
        CHECK(mock_i2c_wrote(REG_CLK2, 0x00));
        CHECK(mock_i2c_wrote(REG_CLK3, 0x10));
        CHECK(mock_i2c_wrote(REG_CLK4, 0x10));
        CHECK(mock_i2c_wrote(REG_CLK5, 0x00));
        CHECK(mock_i2c_wrote(REG_CLK6, 0x03));
        CHECK(mock_i2c_wrote(REG_CLK7, 0x00));
        CHECK(mock_i2c_wrote(REG_CLK8, 0xFF));

        // A second power_up is a no-op.
        dev.power_up();
        CHECK(calls.dac == std::vector<bool>{ true });
        CHECK(calls.adc == std::vector<bool>{ true });
        CHECK(dev.get_output_channels() == 2);
    }
}

TEST_CASE("power_up_dac_only and power_up_adc_only")
{
    mock_i2c_reset();
    power_calls calls;
    {
        auto dev = make_device<es8311<>>(calls);
        dev.power_up_dac_only();
        CHECK(calls.dac == std::vector<bool>{ true });
        CHECK(calls.adc.empty());
        // DAC-only clock config.
        CHECK(mock_i2c_wrote(REG_CLK1, 0x35));
        CHECK(mock_i2c_wrote(REG_SDPIN, 0x0C));
        CHECK(mock_i2c_wrote(REG_SYSTEM12, 0x00));
        CHECK(mock_i2c_wrote(REG_SYSTEM0D, 0x31));
        CHECK(!mock_i2c_wrote(REG_SDPOUT, 0x0C));

        dev.power_down();
        CHECK(calls.dac == std::vector<bool>{ true, false });
        // power_down notifies both callbacks even in DAC-only mode.
        CHECK(calls.adc == std::vector<bool>{ false });

        dev.power_up_adc_only();
        CHECK(calls.dac == std::vector<bool>{ true, false });
        CHECK(calls.adc == std::vector<bool>{ false, true });
        // ADC-only clock config.
        CHECK(mock_i2c_wrote(REG_CLK1, 0x3A));
        CHECK(mock_i2c_wrote(REG_SDPOUT, 0x0C));
        CHECK(mock_i2c_wrote(REG_SYSTEM0D, 0x09));

        // power_up on a device that is already up is a no-op.
        dev.power_up();
        CHECK(calls.adc == std::vector<bool>{ false, true });
    }
}

TEST_CASE("the clock dividers are programmed in DAC-only and ADC-only mode")
{
    mock_i2c_reset();
    {
        // 18.432 MHz MCLK at 32 kHz uses adc_div = dac_div = 3.
        es8311<I2S_MCLK_MULTIPLE_576> dev(kBus, kTx, 32000);
        dev.power_up_dac_only();
        // REG_CLK5 carries both dividers (bits [7:4] ADC, [3:0] DAC): the
        // DAC-only path must still program the DAC divider nibble.
        CHECK(mock_i2c_wrote(REG_CLK5, 0x02));

        dev.power_down();
        dev.power_up_adc_only();
        CHECK(mock_i2c_wrote(REG_CLK5, 0x20));
    }
}

TEST_CASE("power_down powers the DAC/ADC down and notifies callbacks")
{
    mock_i2c_reset();
    power_calls calls;
    {
        auto dev = make_device<es8311<>>(calls);
        dev.power_up();
        dev.power_down();
        CHECK(calls.dac == std::vector<bool>{ true, false });
        CHECK(calls.adc == std::vector<bool>{ true, false });
        CHECK(mock_i2c_wrote(REG_DAC31, 0x60));      // mute
        CHECK(mock_i2c_wrote(REG_SYSTEM0E, 0xC0));   // disable ADC analog/output stage
        CHECK(mock_i2c_wrote(REG_SYSTEM0D, 0xF8));   // power down ADC/DAC analog
        CHECK(mock_i2c_wrote(REG_SYSTEM12, 0xFF));   // disable DAC
        CHECK(mock_i2c_wrote(REG_SYSTEM14, 0x00));   // clear ADC volume
        CHECK(mock_i2c_wrote(REG_RESET, 0x00));      // power off
        CHECK(mock_i2c_wrote(REG_CLK1, 0x00));       // disable clocks
        CHECK(mock_i2c_wrote(REG_I2C, 0x01));        // reset registers
        CHECK(mock_i2c_wrote(REG_I2C, 0x00));
        // The callbacks run before the full power-down register sequence.
        CHECK(mock_i2c_writes().back().reg == REG_I2C);
        CHECK(mock_i2c_writes().back().val == 0x00);

        // A second power_down is a no-op.
        dev.power_down();
        CHECK(calls.dac == std::vector<bool>{ true, false });
        CHECK(calls.adc == std::vector<bool>{ true, false });
    }
}

TEST_CASE("sample rate is reported and unsupported rates are rejected")
{
    mock_i2c_reset();
    {
        auto dev = make_device<es8311<>>(power_calls_holder(), 48000);
        CHECK(dev.get_sample_rate() == 48000);
    }
    // 44101 Hz has no matching coefficient-table entry; the rate is only
    // validated when the device is powered up.
    {
        auto dev = make_device<es8311<>>(power_calls_holder(), 44101);
        CHECK_THROWS_AS(dev.power_up(), es8311<>::exception);
        // The partially configured chip still needs a power-down, but the
        // failed power-up must not turn the retry into a silent no-op.
        CHECK(!dev.is_powered_down());
        CHECK_THROWS_AS(dev.power_up(), es8311<>::exception);
    }
}

TEST_CASE("power_up can be retried after a transient failure")
{
    mock_i2c_reset();
    power_calls calls;
    {
        auto dev = make_device<es8311<>>(calls);
        // Fail the first register write: the power-up sequence throws after
        // the chip was already touched.
        mock_i2c_fail_writes_times(1);
        CHECK_THROWS_AS(dev.power_up(), es8311<>::exception);
        CHECK(!dev.is_powered_down());
        // The retry must reconfigure the chip and notify the callbacks once.
        dev.power_up();
        CHECK(calls.dac == std::vector<bool>{ true });
        CHECK(calls.adc == std::vector<bool>{ true });
        CHECK(mock_i2c_wrote(REG_RESET, 0x80));
        CHECK(mock_i2c_wrote(REG_SYSTEM0D, 0x01));
        // Now that it is configured, power_up is a no-op again.
        dev.power_up();
        CHECK(calls.dac == std::vector<bool>{ true });
    }
}

TEST_CASE("a throwing power callback cannot corrupt the power state machine")
{
    mock_i2c_reset();
    {
        std::vector<bool> dac;
        std::vector<bool> adc;
        es8311<> dev(
                kBus,
                kTx,
                44100,
                [&dac](bool on) {
                    dac.push_back(on);
                    if (!on) {
                        throw std::runtime_error("dac callback failed");
                    }
                },
                [&adc](bool on) { adc.push_back(on); });
        dev.power_up();
        CHECK(dac == std::vector<bool>{ true });
        CHECK(adc == std::vector<bool>{ true });

        // The DAC callback throws while powering down: the exception is
        // swallowed, the ADC callback still runs and the full register
        // sequence is executed.
        CHECK_NOTHROW(dev.power_down());
        CHECK(dac == std::vector<bool>{ true, false });
        CHECK(adc == std::vector<bool>{ true, false });
        CHECK(mock_i2c_wrote(REG_RESET, 0x00));
        CHECK(dev.is_powered_down());
        // The device is actually down, so a retry really reconfigures it.
        dev.power_up();
        CHECK(mock_i2c_wrote(REG_RESET, 0x80));
    }
}

TEST_CASE("power_up rejects a sample rate that would overflow the MCLK product")
{
    mock_i2c_reset();
    {
        auto dev = make_device<es8311<>>(power_calls_holder(), UINT32_MAX);
        CHECK_THROWS_AS(dev.power_up(), es8311<>::exception);
        CHECK(!dev.is_powered_down());
    }
}

TEST_CASE("volume getters and setters")
{
    mock_i2c_reset();
    {
        auto dev = make_device<es8311<>>(power_calls_holder());

        dev.set_volume_raw(0x00);
        CHECK(dev.get_volume_raw() == 0x00);
        CHECK(dev.get_volume() == 0);
        CHECK(mock_i2c_wrote(REG_DAC32, 0x00));

        dev.set_volume_raw(0x80);
        CHECK(dev.get_volume_raw() == 0x80);
        CHECK(dev.get_volume() == 50);
        CHECK(mock_i2c_wrote(REG_DAC32, 0x80));

        dev.set_volume_raw(0xFF);
        CHECK(dev.get_volume_raw() == 0xFF);
        CHECK(dev.get_volume() == 100);

        dev.set_volume(50);
        CHECK(dev.get_volume() == 50);
        CHECK(dev.get_volume_raw() == 0x80);
        dev.set_volume(0);
        CHECK(dev.get_volume() == 0);
        dev.set_volume(100);
        CHECK(dev.get_volume() == 100);
        // Values above 100 are clamped.
        dev.set_volume(150);
        CHECK(dev.get_volume() == 100);
        CHECK(dev.get_volume_raw() == 0xFF);
    }
}

TEST_CASE("ADC volume getters and setters")
{
    mock_i2c_reset();
    {
        auto dev = make_device<es8311<>>(power_calls_holder());

        // Like the DAC volume, the ADC digital volume is left at the minimum
        // after power-up but remains programmable at runtime.
        dev.set_adc_volume_raw(0x00);
        CHECK(dev.get_adc_volume_raw() == 0x00);
        CHECK(dev.get_adc_volume() == 0);
        CHECK(mock_i2c_wrote(REG_ADC17, 0x00));

        dev.set_adc_volume_raw(0x80);
        CHECK(dev.get_adc_volume_raw() == 0x80);
        CHECK(dev.get_adc_volume() == 50);

        dev.set_adc_volume(50);
        CHECK(dev.get_adc_volume() == 50);
        CHECK(dev.get_adc_volume_raw() == 0x80);

        // Values above 100 are clamped.
        dev.set_adc_volume(150);
        CHECK(dev.get_adc_volume() == 100);
        CHECK(dev.get_adc_volume_raw() == 0xFF);
        CHECK(mock_i2c_wrote(REG_ADC17, 0xFF));
    }
}

TEST_CASE("mute state is read and written")
{
    mock_i2c_reset();
    {
        auto dev = make_device<es8311<>>(power_calls_holder());

        mock_i2c_set_reg(REG_DAC31, 0x00);
        CHECK(!dev.get_mute());
        dev.set_mute(true);
        CHECK(dev.get_mute());
        CHECK(mock_i2c_wrote(REG_DAC31, 0x60));

        dev.set_mute(false);
        CHECK(!dev.get_mute());
        CHECK(mock_i2c_wrote(REG_DAC31, 0x00));
    }
}

TEST_CASE("drc_config writes the DRC registers")
{
    mock_i2c_reset();
    {
        auto dev = make_device<es8311<>>(power_calls_holder());

        dev.drc_config(true, 3, 5, 7);
        CHECK(mock_i2c_wrote(REG_DAC35, 0x57));
        CHECK(mock_i2c_wrote(REG_DAC34, 0x83));

        // drc_config() writes the winsize bits directly (there is no
        // read-modify-write of REG_DAC34): the previously programmed winsize is
        // overwritten by the value passed to this call.
        dev.drc_config(false, 0, 0, 0);
        CHECK(mock_i2c_wrote(REG_DAC35, 0x00));
        CHECK(mock_i2c_wrote(REG_DAC34, 0x00));

        // Out-of-range parameters are rejected, not silently truncated.
        CHECK_THROWS_AS(dev.drc_config(true, 16, 0, 0), es8311<>::exception);
        CHECK_THROWS_AS(dev.drc_config(true, 0, 16, 0), es8311<>::exception);
        CHECK_THROWS_AS(dev.drc_config(true, 0, 0, 16), es8311<>::exception);
    }
}

TEST_CASE("voice_fade sets the fade bits")
{
    mock_i2c_reset();
    {
        auto dev = make_device<es8311<>>(power_calls_holder());
        dev.voice_fade(5);
        CHECK(mock_i2c_wrote(REG_DAC37, 0x50));
        dev.voice_fade(0);
        CHECK(mock_i2c_wrote(REG_DAC37, 0x00));
        // Values above 15 are rejected, not silently truncated.
        CHECK_THROWS_AS(dev.voice_fade(16), es8311<>::exception);
    }
}

TEST_CASE("is_8311 and get_version read the chip registers")
{
    mock_i2c_reset();
    {
        auto dev = make_device<es8311<>>(power_calls_holder());

        mock_i2c_set_reg(REG_CHIP_ID1, 0x83);
        mock_i2c_set_reg(REG_CHIP_ID2, 0x11);
        CHECK(dev.is_8311());

        mock_i2c_set_reg(REG_CHIP_ID1, 0x00);
        CHECK(!dev.is_8311());

        // Both reads succeed but the second ID does not match.
        mock_i2c_set_reg(REG_CHIP_ID1, 0x83);
        mock_i2c_set_reg(REG_CHIP_ID2, 0x00);
        CHECK(!dev.is_8311());

        mock_i2c_set_reg(REG_VERSION, 0x10);
        CHECK(dev.get_version() == 0x10);
    }
}

TEST_CASE("write sends audio over I2S when the channel count matches")
{
    mock_i2c_reset();
    {
        auto dev = make_device<es8311<>>(power_calls_holder());
        std::vector<int16_t> samples{ 1, 2, 3, 4 };
        size_t written = dev.write(samples);
        CHECK(written == samples.size() * sizeof(int16_t));
        CHECK(mock_i2s_last_size() == samples.size() * sizeof(int16_t));
        const auto &data = mock_i2s_last_data();
        CHECK(data.size() == samples.size() * sizeof(int16_t));
        CHECK(std::memcmp(data.data(), samples.data(), data.size()) == 0);

        // The write history accessor reflects the constructor's I2C device
        // registration.
        CHECK(mock_i2c_writes().empty());
    }
}

TEST_CASE("write rejects a zero channel count and partial frames")
{
    mock_i2c_reset();
    {
        auto dev = make_device<es8311<>>(power_calls_holder());
        std::vector<int16_t> samples{ 1, 2, 3 };
        CHECK_THROWS_AS((void) dev.write(samples, 0), es8311<>::exception);
        CHECK_THROWS_AS((void) dev.write(samples, 2), es8311<>::exception);
        // The same buffer is fine as mono input.
        CHECK_NOTHROW((void) dev.write(samples, 1));
    }
}

TEST_CASE("write is callable on a const es8311")
{
    mock_i2c_reset();
    {
        auto dev = make_device<es8311<>>(power_calls_holder());
        const es8311<> &cref = dev;
        std::vector<int16_t> samples{ 1, 2 };
        CHECK(cref.write(samples) == samples.size() * sizeof(int16_t));
    }
}

TEST_CASE("set_output_channels rejects zero")
{
    mock_i2c_reset();
    {
        auto dev = make_device<es8311<>>(power_calls_holder());
        CHECK_THROWS_AS(dev.set_output_channels(0), es8311<>::exception);
        dev.set_output_channels(4);
        CHECK(dev.get_output_channels() == 4);
    }
}

TEST_CASE("write converts the channel count")
{
    mock_i2c_reset();
    {
        auto dev = make_device<es8311<>>(power_calls_holder());

        // Mono input is padded to stereo with zeros.
        std::vector<int16_t> mono{ 10, 20 };
        size_t written = dev.write(mono, 1);
        CHECK(written == 4 * sizeof(int16_t));
        const auto &padded = mock_i2s_last_data();
        CHECK(padded.size() == 4 * sizeof(int16_t));
        const auto *padded_s16 = reinterpret_cast<const int16_t *>(padded.data());
        CHECK(padded_s16[0] == 10);
        CHECK(padded_s16[1] == 0);
        CHECK(padded_s16[2] == 20);
        CHECK(padded_s16[3] == 0);

        // Stereo input is downmixed to mono by dropping the second channel.
        dev.set_output_channels(1);
        std::vector<int16_t> stereo{ 1, 2, 3, 4 };
        written = dev.write(stereo, 2);
        CHECK(written == 2 * sizeof(int16_t));
        const auto &mono_out = mock_i2s_last_data();
        CHECK(mono_out.size() == 2 * sizeof(int16_t));
        const auto *mono_out_s16 = reinterpret_cast<const int16_t *>(mono_out.data());
        CHECK(mono_out_s16[0] == 1);
        CHECK(mono_out_s16[1] == 3);
    }
}

TEST_CASE("write reports I2S failures")
{
    mock_i2c_reset();
    {
        auto dev = make_device<es8311<>>(power_calls_holder());
        std::vector<int16_t> samples{ 1, 2 };
        mock_i2s_fail_writes(true);
        CHECK_THROWS_AS((void) dev.write(samples), es8311<>::exception);
        mock_i2s_fail_writes(false);
    }
}

TEST_CASE("write reports I2S failures on the conversion path")
{
    mock_i2c_reset();
    {
        auto dev = make_device<es8311<>>(power_calls_holder());
        std::vector<int16_t> mono{ 1, 2 };
        mock_i2s_fail_writes(true);
        CHECK_THROWS_AS((void) dev.write(mono, 1), es8311<>::exception);
        mock_i2s_fail_writes(false);
    }
}

TEST_CASE("write reports a partial transfer failure after logging the byte count")
{
    mock_i2c_reset();
    {
        auto dev = make_device<es8311<>>(power_calls_holder());
        std::vector<int16_t> samples{ 1, 2, 3, 4 };
        // The driver accepted the first 4 bytes and then failed: the write
        // reports the failure but keeps the partial byte count visible to
        // the log.
        mock_i2s_fail_writes_partial(4);
        CHECK_THROWS_AS((void) dev.write(samples), es8311<>::exception);
    }
}

TEST_CASE("write reports a partial transfer failure on the conversion path")
{
    mock_i2c_reset();
    {
        auto dev = make_device<es8311<>>(power_calls_holder());
        std::vector<int16_t> mono{ 1, 2 };
        mock_i2s_fail_writes_partial(2);
        CHECK_THROWS_AS((void) dev.write(mono, 1), es8311<>::exception);
    }
}

TEST_CASE("write rejects a frame conversion overflow")
{
    mock_i2c_reset();
    {
        auto dev = make_device<es8311<>>(power_calls_holder());
        // Upsampling a huge stereo buffer to 4 output channels would
        // overflow the frame count: the guard trips before the conversion
        // buffer is allocated.
        dev.set_output_channels(4);
        huge_i16_buffer buf;
        CHECK_THROWS_AS((void) dev.write(buf, 2), es8311<>::exception);
    }
}

TEST_CASE("write rejects a buffer whose byte size would overflow")
{
    mock_i2c_reset();
    {
        auto dev = make_device<es8311<>>(power_calls_holder());
        // No channel conversion is needed, but the byte count of the huge
        // buffer would wrap: the fast path must reject it before handing a
        // bogus length to the I2S driver.
        huge_i16_buffer buf;
        CHECK_THROWS_AS((void) dev.write(buf, 2), es8311<>::exception);
    }
}

TEST_CASE("I2C read and write failures are reported")
{
    mock_i2c_reset();
    {
        auto dev = make_device<es8311<>>(power_calls_holder());

        mock_i2c_fail_reads(true);
        CHECK_THROWS_AS((void) dev.get_volume_raw(), es8311<>::exception);
        CHECK_THROWS_AS((void) dev.get_mute(), es8311<>::exception);
        CHECK_THROWS_AS(dev.set_mute(true), es8311<>::exception);
        CHECK_THROWS_AS((void) dev.is_8311(), es8311<>::exception);
        CHECK_THROWS_AS((void) dev.get_version(), es8311<>::exception);
        mock_i2c_fail_reads(false);

        mock_i2c_fail_writes(true);
        CHECK_THROWS_AS(dev.set_volume_raw(10), es8311<>::exception);
        CHECK_THROWS_AS(dev.set_volume(10), es8311<>::exception);
        CHECK_THROWS_AS(dev.set_mute(false), es8311<>::exception);
        CHECK_THROWS_AS(dev.drc_config(true, 0, 0, 0), es8311<>::exception);
        CHECK_THROWS_AS(dev.voice_fade(1), es8311<>::exception);
        CHECK_THROWS_AS(dev.power_up(), es8311<>::exception);
        mock_i2c_fail_writes(false);
    }
}

TEST_CASE("destructor removes the I2C device even when power down fails")
{
    mock_i2c_reset();
    {
        power_calls calls;
        auto dev = make_device<es8311<>>(calls);
        dev.power_up();
        // rm_device failure is logged, not thrown (destructor is noexcept).
        mock_i2c_fail_rm_device(true);
    }
    CHECK(mock_i2c_rm_device_count() == 0);
    mock_i2c_fail_rm_device(false);
}

TEST_CASE("destructor tolerates a power_down failure")
{
    mock_i2c_reset();
    {
        power_calls calls;
        auto dev = make_device<es8311<>>(calls);
        dev.power_up();
        // The power-down sequence performs an I2C write that fails; the
        // destructor must swallow the exception and still remove the device.
        mock_i2c_fail_writes(true);
    }
    CHECK(mock_i2c_rm_device_count() == 1);
    mock_i2c_fail_writes(false);
}

TEST_CASE("move assignment tolerates an rm_device failure")
{
    mock_i2c_reset();
    power_calls calls;
    {
        auto a = make_device<es8311<>>(calls);
        auto b = make_device<es8311<>>(calls);
        mock_i2c_fail_rm_device(true);
        // The failed removal is logged, not thrown (move assignment is
        // noexcept); the device handle is still stolen from a.
        b = std::move(a);
        CHECK(mock_i2c_rm_device_count() == 0);
    }
    mock_i2c_fail_rm_device(false);
}

TEST_CASE("move assignment handles a moved-from target and self-assignment")
{
    mock_i2c_reset();
    power_calls calls;
    {
        auto a = make_device<es8311<>>(calls);
        auto b = make_device<es8311<>>(calls);
        // Move from a into b: b's device is removed and a's handle is stolen.
        b = std::move(a);
        CHECK(mock_i2c_rm_device_count() == 1);
        // a is moved-from (null handle): the assignment must not try to
        // remove it, but still steals b's handle back into a.
        a = std::move(b);
        CHECK(mock_i2c_rm_device_count() == 1);
        // Self-move assignment is a no-op and must not remove the device.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-move"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wself-move"
#endif
        a = std::move(a);
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
        CHECK(mock_i2c_rm_device_count() == 1);
        // The stolen device still works.
        a.set_volume_raw(0x33);
        CHECK(mock_i2c_wrote(REG_DAC32, 0x33));
    }
    // a's destructor removes the device that a originally registered.
    CHECK(mock_i2c_rm_device_count() == 2);
}

TEST_CASE("a moved-from object is inert and reports itself invalid")
{
    mock_i2c_reset();
    {
        auto a = make_device<es8311<>>(power_calls_holder());
        auto b(std::move(a));
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
        CHECK(!a.valid());
        CHECK(b.valid());
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
        CHECK_THROWS_AS((void) a.get_volume_raw(), es8311<>::exception);
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
        CHECK_THROWS_AS(a.set_volume_raw(0x10), es8311<>::exception);
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
        CHECK_THROWS_AS(a.set_mute(true), es8311<>::exception);
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
        CHECK_THROWS_AS((void) a.is_8311(), es8311<>::exception);
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
        CHECK_THROWS_AS(a.power_up(), es8311<>::exception);
        std::vector<int16_t> buf{ 1, 2 };
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
        CHECK_THROWS_AS((void) a.write(buf), es8311<>::exception);
        // The cached configuration is still readable.
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
        CHECK(a.get_sample_rate() == 44100);
    }
}

TEST_CASE("power_up on an already-powered device and empty callbacks")
{
    mock_i2c_reset();
    {
        // No power callbacks: power_up must tolerate the empty std::function.
        es8311<> dev(kBus, kTx);
        dev.power_up();
        // A second power_up on the already-powered device is a no-op.
        dev.power_up();

        dev.power_down();
        dev.power_up_dac_only();
        // power_up on the already-powered device is a no-op.
        dev.power_up();
        dev.power_up_dac_only();

        dev.power_down();
        dev.power_up_adc_only();
        // power_up on the already-powered device is a no-op.
        dev.power_up();
        dev.power_up_adc_only();
    }
}

TEST_CASE("move assignment powers down the device before releasing it")
{
    mock_i2c_reset();
    power_calls calls;
    {
        auto a = make_device<es8311<>>(calls);
        auto b = make_device<es8311<>>(calls);
        b.power_up();
        // The power-down performed by the move assignment (releasing b's
        // device) fails midway: the exception must be swallowed and the
        // device still released. power_down() notifies the callbacks before
        // the register sequence and marks the device powered down up front,
        // so they fire even when a write fails midway.
        mock_i2c_fail_writes(true);
        b = std::move(a);
        mock_i2c_fail_writes(false);
        CHECK(calls.dac == std::vector<bool>{ true, false });
        CHECK(calls.adc == std::vector<bool>{ true, false });
        // b's device is released and a's (powered-down) device is stolen.
        CHECK(mock_i2c_rm_device_count() == 1);
    }
}

TEST_CASE("move assignment powers down a powered-up destination device")
{
    mock_i2c_reset();
    power_calls calls;
    {
        auto a = make_device<es8311<>>(calls);
        auto b = make_device<es8311<>>(calls);
        b.power_up();
        // b's device is powered up: the move assignment powers it down
        // before releasing it, notifying the power callbacks.
        b = std::move(a);
        CHECK(calls.dac == std::vector<bool>{ true, false });
        CHECK(calls.adc == std::vector<bool>{ true, false });
        CHECK(mock_i2c_wrote(REG_RESET, 0x00));
        CHECK(mock_i2c_rm_device_count() == 1);
    }
}

TEST_CASE("move construction and move assignment")
{
    mock_i2c_reset();
    power_calls calls;
    unsigned rm_before = mock_i2c_rm_device_count();
    {
        auto a = make_device<es8311<>>(calls);
        a.power_up();

        auto b = make_device<es8311<>>(calls);
        // Move-assign: b's device is removed and a's device is stolen.
        b = std::move(a);
        CHECK(mock_i2c_rm_device_count() == rm_before + 1);

        // a is moved-from: its destructor must not remove the device again.
        unsigned rm_after_assign = mock_i2c_rm_device_count();

        auto c(std::move(b));
        CHECK(mock_i2c_rm_device_count() == rm_after_assign);

        // The stolen device still works.
        c.set_volume_raw(0x55);
        CHECK(mock_i2c_wrote(REG_DAC32, 0x55));
    }
    // c's destructor removes the device that a originally registered.
    CHECK(mock_i2c_rm_device_count() == rm_before + 2);
}

TEST_CASE("move operations transfer the per-instance configuration")
{
    SUBCASE("move construction")
    {
        mock_i2c_reset();
        power_calls calls;
        {
            auto a = make_device<es8311<>>(calls);
            a.set_output_channels(1);
            auto b = std::move(a);
            CHECK(b.get_output_channels() == 1);
        }
    }
    SUBCASE("move assignment")
    {
        mock_i2c_reset();
        power_calls calls;
        {
            auto a = make_device<es8311<>>(calls);
            auto b = make_device<es8311<>>(calls);
            b.set_output_channels(1);
            // b's state is replaced by a's.
            b = std::move(a);
            CHECK(b.get_output_channels() == 2);
        }
    }
}

TEST_CASE("all sample-rate configurations in the coefficient table are reachable")
{
#ifndef __clang__
    // These values are outside the enum's valid range, clang-tidy would complain.

    // MCLK multiples that are not named in i2s_mclk_multiple_t but select
    // additional coefficient-table entries (pre_multi == 8 and bclk_div >= 19).
    mock_i2c_reset();
    {
        // pre_multi == 8 branch; 96 is below the named MCLK multiples but
        // matches the 1.536 MHz @ 16 kHz coefficient-table entry. The clang
        // analyzer flags the cast, but the value is within the enum's storage
        // range and the GCC coverage build needs this instantiation.
        es8311<static_cast<i2s_mclk_multiple_t>(96)> dev(kBus, kTx, 16000);
        dev.power_up();
    }
    mock_i2c_reset();
    // MCLK multiple 2304 (mclk 18.432 MHz at 8 kHz) exercises the
    // bclk_div >= 19 branch.
    {
        es8311<static_cast<i2s_mclk_multiple_t>(2304)> dev(kBus, kTx, 8000);
        dev.power_up();
    }
#endif
    mock_i2c_reset();
    {
        // pre_multi == 4 (192 * 8000 = 1536000 = 8k * 192).
        es8311<I2S_MCLK_MULTIPLE_192> dev(kBus, kTx, 8000);
        dev.power_up();
    }
    mock_i2c_reset();
    {
        // pre_multi == 2.
        es8311<I2S_MCLK_MULTIPLE_384> dev(kBus, kTx, 16000);
        dev.power_up();
    }
}

TEST_CASE("the 32-bit I2S instantiation configures and writes 32-bit samples")
{
    mock_i2c_reset();
    {
        es8311<I2S_MCLK_MULTIPLE_512, true> dev(kBus, kTx, 32000);
        dev.power_up();
        // 32-bit I2S format.
        CHECK(mock_i2c_wrote(REG_SDPIN, 0x10));
        CHECK(mock_i2c_wrote(REG_SDPOUT, 0x10));

        std::vector<int32_t> samples{ 1, 2 };
        size_t written = dev.write(samples);
        CHECK(written == samples.size() * sizeof(int32_t));
        CHECK(mock_i2s_last_size() == samples.size() * sizeof(int32_t));

        // Mono to stereo conversion on the 32-bit path.
        std::vector<int32_t> mono{ 5 };
        written = dev.write(mono, 1);
        CHECK(written == 2 * sizeof(int32_t));
        const auto &padded = mock_i2s_last_data();
        CHECK(padded.size() == 2 * sizeof(int32_t));
        const auto *padded_s32 = reinterpret_cast<const int32_t *>(padded.data());
        CHECK(padded_s32[0] == 5);
        CHECK(padded_s32[1] == 0);
    }
}
