/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#include <doctest/doctest.h>
#include "es7210.hpp"
#include "mock_bus.hpp"

using namespace mock_bus;

namespace {

// Handles that pass the constructor's null checks but are never dereferenced:
// the I2C/I2S driver entry points are redirected to the mocks in
// mock_bus.cpp (components/utils/mocks) by the linker --wrap options in its
// CMakeLists.txt.
const auto kBus = reinterpret_cast<i2c_master_bus_handle_t>(0x1);
const auto kRx = reinterpret_cast<i2s_chan_handle_t>(0x2);

// ES7210 register addresses (mirrors the private constants in es7210.hpp).
constexpr uint8_t REG_RESET = 0x00;
constexpr uint8_t REG_CLOCK_OFF = 0x01;
constexpr uint8_t REG_MAINCLK = 0x02;
constexpr uint8_t REG_MASTER_CLK = 0x03;
constexpr uint8_t REG_LRCK_DIVH = 0x04;
constexpr uint8_t REG_LRCK_DIVL = 0x05;
constexpr uint8_t REG_POWER_DOWN = 0x06;
constexpr uint8_t REG_OSR = 0x07;
constexpr uint8_t REG_MODE_CONFIG = 0x08;
constexpr uint8_t REG_TIME_CONTROL0 = 0x09;
constexpr uint8_t REG_TIME_CONTROL1 = 0x0A;
constexpr uint8_t REG_SDP_INTERFACE1 = 0x11;
constexpr uint8_t REG_SDP_INTERFACE2 = 0x12;
constexpr uint8_t REG_MUTERANGE_L = 0x14;
constexpr uint8_t REG_MUTERANGE_R = 0x15;
constexpr uint8_t REG_ADC34_HPF2 = 0x20;
constexpr uint8_t REG_ADC34_HPF1 = 0x21;
constexpr uint8_t REG_ADC12_HPF1 = 0x22;
constexpr uint8_t REG_ADC12_HPF2 = 0x23;
constexpr uint8_t REG_CHIP_ID1 = 0x3D;
constexpr uint8_t REG_CHIP_ID0 = 0x3E;
constexpr uint8_t REG_VERSION = 0x3F;
constexpr uint8_t REG_ANALOG = 0x40;
constexpr uint8_t REG_MIC12_BIAS = 0x41;
constexpr uint8_t REG_MIC34_BIAS = 0x42;
constexpr uint8_t REG_MIC1_GAIN = 0x43;
constexpr uint8_t REG_MIC2_GAIN = 0x44;
constexpr uint8_t REG_MIC3_GAIN = 0x45;
constexpr uint8_t REG_MIC4_GAIN = 0x46;
constexpr uint8_t REG_MIC1_POWER = 0x47;
constexpr uint8_t REG_MIC2_POWER = 0x48;
constexpr uint8_t REG_MIC3_POWER = 0x49;
constexpr uint8_t REG_MIC4_POWER = 0x4A;
constexpr uint8_t REG_MIC12_POWER = 0x4B;
constexpr uint8_t REG_MIC34_POWER = 0x4C;

// Container whose size() overflows read()'s size/sample-width guard. Kept at
// namespace scope (rather than local to the test) so the -Werror build does
// not complain about a locally defined value_type typedef.
struct huge_buffer
{
    using value_type = int16_t;
    int16_t data_[1]{};
    int16_t *data()
    {
        return data_;
    }
    size_t size() const
    {
        return static_cast<size_t>(-1) - 1;
    }
};

// Helper that forwards to the es7210 constructor. All the arguments used in
// the tests below fail validation before any hardware access happens.
void
make_es7210(i2c_master_bus_handle_t bus, i2s_chan_handle_t rx, uint8_t addr = 0x40)
{
    es7210<>(bus, rx, 16000, 0x0F, addr);
}

TEST_CASE("es7210 is movable but not copyable")
{
    static_assert(!std::is_copy_constructible_v<es7210<>>);
    static_assert(!std::is_copy_assignable_v<es7210<>>);
    static_assert(std::is_move_constructible_v<es7210<>>);
    static_assert(std::is_nothrow_move_constructible_v<es7210<>>);
    static_assert(std::is_nothrow_move_assignable_v<es7210<>>);
}

TEST_CASE("exception derives from std::exception")
{
    static_assert(std::is_base_of_v<std::exception, es7210<>::exception>);
    static_assert(std::is_move_constructible_v<es7210<>::exception>);
    static_assert(std::is_move_assignable_v<es7210<>::exception>);
}

TEST_CASE("exception includes the message and the error name")
{
    es7210<>::exception plain("boom");
    CHECK(std::strcmp(plain.what(), "es7210: boom") == 0);

    es7210<>::exception with_err("i2c failed", ESP_ERR_INVALID_ARG);
    std::string msg(with_err.what());
    CHECK(msg.find("es7210: i2c failed") == 0);
    CHECK(msg.find("ESP_ERR_INVALID_ARG") != std::string::npos);
}

TEST_CASE("constructor rejects a null I2C bus handle")
{
    CHECK_THROWS_AS(make_es7210(nullptr, nullptr), es7210<>::exception);
}

TEST_CASE("constructor rejects a null I2S RX handle")
{
    CHECK_THROWS_AS(make_es7210(reinterpret_cast<i2c_master_bus_handle_t>(0x1), nullptr),
                    es7210<>::exception);
}

TEST_CASE("constructor rejects an invalid I2C address")
{
    CHECK_THROWS_AS(make_es7210(reinterpret_cast<i2c_master_bus_handle_t>(0x1),
                                reinterpret_cast<i2s_chan_handle_t>(0x2),
                                0x20),
                    es7210<>::exception);
}

TEST_CASE("constructor reports i2c_master_bus_add_device failures")
{
    mock_i2c_reset();
    mock_i2c_fail_add_device(true);
    CHECK_THROWS_AS(es7210<>(kBus, kRx), es7210<>::exception);
    mock_i2c_fail_add_device(false);
}

TEST_CASE("constructor removes the I2C device when initialization fails")
{
    mock_i2c_reset();
    mock_i2c_fail_writes(true);
    CHECK_THROWS_AS(es7210<>(kBus, kRx), es7210<>::exception);
    mock_i2c_fail_writes(false);
    CHECK(mock_i2c_rm_device_count() == 1);
}

TEST_CASE("constructor tolerates an rm_device failure during init cleanup")
{
    mock_i2c_reset();
    mock_i2c_fail_writes(true);
    mock_i2c_fail_rm_device(true);
    CHECK_THROWS_AS(es7210<>(kBus, kRx), es7210<>::exception);
    mock_i2c_fail_writes(false);
    mock_i2c_fail_rm_device(false);
    CHECK(mock_i2c_rm_device_count() == 0);
}

TEST_CASE("constructor registers the I2C device and configures the ADC")
{
    mock_i2c_reset();
    {
        es7210<> adc(kBus, kRx);
        CHECK(mock_i2c_add_device_count() == 1);
        CHECK(mock_i2c_last_device_address() == 0x40);
        CHECK(adc.get_sample_rate() == 16000);
        // init_device() reset sequence.
        CHECK(mock_i2c_wrote(REG_RESET, 0xff));
        CHECK(mock_i2c_wrote(REG_RESET, 0x41));
        // init_device() configuration.
        CHECK(mock_i2c_wrote(REG_CLOCK_OFF, 0x3f));
        CHECK(mock_i2c_wrote(REG_TIME_CONTROL0, 0x30));
        CHECK(mock_i2c_wrote(REG_TIME_CONTROL1, 0x30));
        CHECK(mock_i2c_wrote(REG_ADC12_HPF2, 0x2a));
        CHECK(mock_i2c_wrote(REG_ADC12_HPF1, 0x0a));
        CHECK(mock_i2c_wrote(REG_ADC34_HPF2, 0x0a));
        CHECK(mock_i2c_wrote(REG_ADC34_HPF1, 0x2a));
        CHECK(mock_i2c_wrote(REG_MODE_CONFIG, 0x00));
        CHECK(mock_i2c_wrote(REG_MASTER_CLK, 0x00));
        CHECK(mock_i2c_wrote(REG_MIC12_BIAS, 0x70));
        CHECK(mock_i2c_wrote(REG_MIC34_BIAS, 0x70));
        CHECK(mock_i2c_wrote(REG_SDP_INTERFACE1, 0x60));
        // 16 kHz @ MCLK 256 (4.096 MHz) coefficient.
        CHECK(mock_i2c_wrote(REG_MAINCLK, 0xc1));
        CHECK(mock_i2c_wrote(REG_OSR, 0x20));
        CHECK(mock_i2c_wrote(REG_LRCK_DIVH, 0x01));
        CHECK(mock_i2c_wrote(REG_LRCK_DIVL, 0x00));
    }
    CHECK(mock_i2c_rm_device_count() == 1);
}

TEST_CASE("constructor rejects an invalid microphone mask")
{
    CHECK_THROWS_AS(es7210<>(kBus, kRx, 16000, 0xF0), es7210<>::exception);
    CHECK_THROWS_AS(es7210<>(kBus, kRx, 16000, 0), es7210<>::exception);
}

TEST_CASE("constructor rejects a zero SCL speed")
{
    CHECK_THROWS_AS(es7210<>(kBus, kRx, 16000, 0x0F, 0x40, 0), es7210<>::exception);
}

TEST_CASE("constructor accepts the 0x43 address")
{
    mock_i2c_reset();
    {
        es7210<> adc(kBus, kRx, 16000, 0x0F, 0x43);
        CHECK(mock_i2c_last_device_address() == 0x43);
    }
    CHECK(mock_i2c_rm_device_count() == 1);
}

TEST_CASE("is_7210 and get_version read the chip registers")
{
    mock_i2c_reset();
    {
        es7210<> adc(kBus, kRx);

        mock_i2c_set_reg(REG_CHIP_ID0, 0x10);
        mock_i2c_set_reg(REG_CHIP_ID1, 0x72);
        CHECK(adc.is_7210());

        mock_i2c_set_reg(REG_CHIP_ID0, 0x00);
        CHECK(!adc.is_7210());

        // Both reads succeed but the second ID does not match.
        mock_i2c_set_reg(REG_CHIP_ID0, 0x10);
        mock_i2c_set_reg(REG_CHIP_ID1, 0x00);
        CHECK(!adc.is_7210());

        mock_i2c_set_reg(REG_VERSION, 0x10);
        CHECK(adc.get_version() == 0x10);
    }
}

TEST_CASE("start powers up the microphones and select_microphones configures each one")
{
    mock_i2c_reset();
    {
        es7210<> adc(kBus,
                     kRx,
                     16000,
                     es7210<>::MIC1 | es7210<>::MIC2 | es7210<>::MIC3 | es7210<>::MIC4);
        adc.start();
        // Power-up sequence.
        CHECK(mock_i2c_wrote(REG_CLOCK_OFF, 0x3f));
        CHECK(mock_i2c_wrote(REG_POWER_DOWN, 0x00));
        CHECK(mock_i2c_wrote(REG_MIC1_POWER, 0x08));
        CHECK(mock_i2c_wrote(REG_MIC2_POWER, 0x08));
        CHECK(mock_i2c_wrote(REG_MIC3_POWER, 0x08));
        CHECK(mock_i2c_wrote(REG_MIC4_POWER, 0x08));
        CHECK(mock_i2c_wrote(REG_ANALOG, 0x43));
        // select_microphones(): 4 mics -> TDM mode.
        CHECK(mock_i2c_wrote(REG_SDP_INTERFACE2, 0x02));
        CHECK(mock_i2c_wrote(REG_MIC12_POWER, 0x00));
        CHECK(mock_i2c_wrote(REG_MIC34_POWER, 0x00));
        // A second start is a no-op.
        adc.start();
    }
}

TEST_CASE("select_microphones with fewer than 3 mics disables TDM mode")
{
    mock_i2c_reset();
    {
        es7210<> adc(kBus, kRx, 16000, es7210<>::MIC1);
        adc.start();
        CHECK(mock_i2c_wrote(REG_SDP_INTERFACE2, 0x00));
    }
    mock_i2c_reset();
    {
        // MIC2 alone: enables MIC2 and counts it, TDM stays off.
        es7210<> adc(kBus, kRx, 16000, es7210<>::MIC2);
        adc.start();
        CHECK(mock_i2c_wrote(REG_SDP_INTERFACE2, 0x00));
        CHECK(mock_i2c_wrote(REG_MIC2_GAIN, 0x10));
    }
    mock_i2c_reset();
    {
        // MIC3 alone.
        es7210<> adc(kBus, kRx, 16000, es7210<>::MIC3);
        adc.start();
        CHECK(mock_i2c_wrote(REG_SDP_INTERFACE2, 0x00));
        CHECK(mock_i2c_wrote(REG_MIC3_GAIN, 0x10));
    }
    mock_i2c_reset();
    {
        // MIC4 alone.
        es7210<> adc(kBus, kRx, 16000, es7210<>::MIC4);
        adc.start();
        CHECK(mock_i2c_wrote(REG_SDP_INTERFACE2, 0x00));
        CHECK(mock_i2c_wrote(REG_MIC4_GAIN, 0x10));
    }
    mock_i2c_reset();
    {
        // Two mics: MIC1 and MIC2 both enabled, count stays below 3.
        es7210<> adc(kBus, kRx, 16000, es7210<>::MIC1 | es7210<>::MIC2);
        adc.start();
        CHECK(mock_i2c_wrote(REG_SDP_INTERFACE2, 0x00));
    }
}

TEST_CASE("stop powers the ADC down and a second stop is a no-op")
{
    mock_i2c_reset();
    {
        es7210<> adc(kBus, kRx, 16000, es7210<>::MIC1 | es7210<>::MIC2);
        adc.start();
        adc.stop();
        CHECK(mock_i2c_wrote(REG_MIC1_POWER, 0xff));
        CHECK(mock_i2c_wrote(REG_MIC2_POWER, 0xff));
        CHECK(mock_i2c_wrote(REG_MIC3_POWER, 0xff));
        CHECK(mock_i2c_wrote(REG_MIC4_POWER, 0xff));
        CHECK(mock_i2c_wrote(REG_MIC12_POWER, 0xff));
        CHECK(mock_i2c_wrote(REG_MIC34_POWER, 0xff));
        CHECK(mock_i2c_wrote(REG_ANALOG, 0xc0));
        CHECK(mock_i2c_wrote(REG_CLOCK_OFF, 0x7f));
        CHECK(mock_i2c_wrote(REG_POWER_DOWN, 0x07));
        // A second stop is a no-op.
        adc.stop();
    }
}

TEST_CASE("read returns the samples delivered by the I2S mock")
{
    mock_i2c_reset();
    {
        es7210<> adc(kBus, kRx);
        std::vector<int16_t> samples{ 1, -2, 3, -4 };
        mock_i2s_set_read_data(samples.data(), samples.size() * sizeof(int16_t));
        std::vector<int16_t> buf(4);
        size_t read_bytes = adc.read(buf);
        CHECK(read_bytes == samples.size() * sizeof(int16_t));
        CHECK(std::memcmp(buf.data(), samples.data(), read_bytes) == 0);
        // The mock consumed the data: a second read returns zero bytes.
        read_bytes = adc.read(buf);
        CHECK(read_bytes == 0);
    }
}

TEST_CASE("read rejects an empty buffer")
{
    mock_i2c_reset();
    {
        es7210<> adc(kBus, kRx);
        std::vector<int16_t> buf;
        CHECK_THROWS_AS((void) adc.read(buf), es7210<>::exception);
    }
}

TEST_CASE("read rejects a buffer too large for the sample width")
{
    // The overflow guard trips before any I2S transfer, so the oversized
    // container never has its data() dereferenced. The container is defined
    // at namespace scope so the -Werror build does not complain about a
    // locally-defined, mostly compile-time-used value_type typedef.
    mock_i2c_reset();
    {
        es7210<> adc(kBus, kRx);
        huge_buffer buf;
        CHECK_THROWS_AS((void) adc.read(buf), es7210<>::exception);
    }
}

TEST_CASE("move assignment stops a started device before removing it")
{
    mock_i2c_reset();
    unsigned rm_before = mock_i2c_rm_device_count();
    {
        auto a = es7210<>(kBus, kRx);
        auto b = es7210<>(kBus, kRx);
        b.start();
        b = std::move(a);
        // b was started: the move assignment stopped b's own device before
        // removing it.
        CHECK(mock_i2c_wrote(REG_POWER_DOWN, 0x07));
        CHECK(mock_i2c_rm_device_count() == rm_before + 1);
    }
}

TEST_CASE("move assignment tolerates a stop failure")
{
    mock_i2c_reset();
    {
        auto a = es7210<>(kBus, kRx);
        auto b = es7210<>(kBus, kRx);
        b.start();
        mock_i2c_fail_writes(true);
        // The failed stop is swallowed (move assignment is noexcept) and
        // b's device is still removed.
        b = std::move(a);
        mock_i2c_fail_writes(false);
        CHECK(mock_i2c_rm_device_count() == 1);
    }
}

TEST_CASE("read reports I2S failures")
{
    mock_i2c_reset();
    {
        es7210<> adc(kBus, kRx);
        std::vector<int16_t> buf(4);
        mock_i2s_fail_reads(true);
        CHECK_THROWS_AS((void) adc.read(buf), es7210<>::exception);
        mock_i2s_fail_reads(false);
    }
}

TEST_CASE("set_gain applies the gain to every selected microphone")
{
    mock_i2c_reset();
    {
        es7210<> adc(kBus,
                     kRx,
                     16000,
                     es7210<>::MIC1 | es7210<>::MIC2 | es7210<>::MIC3 | es7210<>::MIC4);
        adc.set_gain(es7210<>::GAIN_30DB);
        CHECK(mock_i2c_wrote(REG_MIC1_GAIN, 0x0A));
        CHECK(mock_i2c_wrote(REG_MIC2_GAIN, 0x0A));
        CHECK(mock_i2c_wrote(REG_MIC3_GAIN, 0x0A));
        CHECK(mock_i2c_wrote(REG_MIC4_GAIN, 0x0A));
    }
    mock_i2c_reset();
    {
        // Single-mic selections hit the individual gain branches.
        es7210<> adc2(kBus, kRx, 16000, es7210<>::MIC2);
        adc2.set_gain(es7210<>::GAIN_3DB);
        CHECK(mock_i2c_wrote(REG_MIC2_GAIN, 0x01));
    }
    mock_i2c_reset();
    {
        es7210<> adc3(kBus, kRx, 16000, es7210<>::MIC3);
        adc3.set_gain(es7210<>::GAIN_3DB);
        CHECK(mock_i2c_wrote(REG_MIC3_GAIN, 0x01));
    }
    mock_i2c_reset();
    {
        es7210<> adc4(kBus, kRx, 16000, es7210<>::MIC4);
        adc4.set_gain(es7210<>::GAIN_3DB);
        CHECK(mock_i2c_wrote(REG_MIC4_GAIN, 0x01));
    }
}

TEST_CASE("set_gain before start is preserved by the power-up sequence")
{
    mock_i2c_reset();
    {
        es7210<> adc(kBus, kRx, 16000, es7210<>::MIC1 | es7210<>::MIC2);
        adc.set_gain(es7210<>::GAIN_30DB);
        CHECK(mock_i2c_wrote(REG_MIC1_GAIN, 0x0A));
        CHECK(mock_i2c_wrote(REG_MIC2_GAIN, 0x0A));
        adc.start();
        // select_microphones() re-applies the remembered gain, not GAIN_0DB.
        CHECK(mock_i2c_wrote(REG_MIC1_GAIN, 0x1A));
        CHECK(mock_i2c_wrote(REG_MIC2_GAIN, 0x1A));
    }
    mock_i2c_reset();
    {
        // The default gain (0 dB) is applied on start when set_gain() was
        // never called.
        es7210<> adc(kBus, kRx, 16000, es7210<>::MIC1);
        adc.start();
        CHECK(mock_i2c_wrote(REG_MIC1_GAIN, 0x10));
    }
}

TEST_CASE("set_mute writes the mute bits")
{
    mock_i2c_reset();
    {
        es7210<> adc(kBus, kRx);
        adc.set_mute(true);
        CHECK(mock_i2c_wrote(REG_MUTERANGE_L, 0x03));
        CHECK(mock_i2c_wrote(REG_MUTERANGE_R, 0x03));
        adc.set_mute(false);
        CHECK(mock_i2c_wrote(REG_MUTERANGE_L, 0x00));
        CHECK(mock_i2c_wrote(REG_MUTERANGE_R, 0x00));
    }
}

TEST_CASE("set_sample_rate applies the matching coefficient and rejects unknown rates")
{
    mock_i2c_reset();
    {
        es7210<> adc(kBus, kRx);
        // 48 kHz @ MCLK 256 (12.288 MHz) coefficient.
        adc.set_sample_rate(48000);
        CHECK(adc.get_sample_rate() == 48000);
        CHECK(mock_i2c_wrote(REG_MAINCLK, 0xc1));
        CHECK(mock_i2c_wrote(REG_OSR, 0x20));
        CHECK(mock_i2c_wrote(REG_LRCK_DIVH, 0x01));
        CHECK(mock_i2c_wrote(REG_LRCK_DIVL, 0x00));
        // No matching coefficient for 44101 Hz @ MCLK 256.
        CHECK_THROWS_AS(adc.set_sample_rate(44101), es7210<>::exception);
    }
}

TEST_CASE("I2C read and write failures are reported")
{
    mock_i2c_reset();
    {
        es7210<> adc(kBus, kRx);

        mock_i2c_fail_reads(true);
        CHECK_THROWS_AS((void) adc.is_7210(), es7210<>::exception);
        CHECK_THROWS_AS((void) adc.get_version(), es7210<>::exception);
        CHECK_THROWS_AS(adc.set_mute(true), es7210<>::exception);
        CHECK_THROWS_AS(adc.set_gain(es7210<>::GAIN_0DB), es7210<>::exception);
        mock_i2c_fail_reads(false);

        mock_i2c_fail_writes(true);
        CHECK_THROWS_AS(adc.set_mute(false), es7210<>::exception);
        CHECK_THROWS_AS(adc.set_sample_rate(16000), es7210<>::exception);
        CHECK_THROWS_AS(adc.start(), es7210<>::exception);
        mock_i2c_fail_writes(false);

        // stop() only touches the bus when the ADC is started.
        adc.start();
        mock_i2c_fail_writes(true);
        CHECK_THROWS_AS(adc.stop(), es7210<>::exception);
        mock_i2c_fail_writes(false);
    }
}

TEST_CASE("destructor removes the I2C device even when stop fails")
{
    mock_i2c_reset();
    {
        es7210<> adc(kBus, kRx);
        adc.start();
        // The stop sequence performs an I2C write that fails; the destructor
        // must swallow the exception and still remove the device.
        mock_i2c_fail_writes(true);
    }
    CHECK(mock_i2c_rm_device_count() == 1);
    mock_i2c_fail_writes(false);
}

TEST_CASE("destructor removes the I2C device even when rm_device fails")
{
    mock_i2c_reset();
    {
        es7210<> adc(kBus, kRx);
        // rm_device failure is logged, not thrown (destructor is noexcept).
        mock_i2c_fail_rm_device(true);
    }
    CHECK(mock_i2c_rm_device_count() == 0);
    mock_i2c_fail_rm_device(false);
}

TEST_CASE("move construction and move assignment")
{
    mock_i2c_reset();
    unsigned rm_before = mock_i2c_rm_device_count();
    {
        auto a = es7210<>(kBus, kRx);
        a.start();

        auto b = es7210<>(kBus, kRx);
        // Move-assign: b's device is removed and a's device is stolen.
        b = std::move(a);
        CHECK(mock_i2c_rm_device_count() == rm_before + 1);

        // a is moved-from: its destructor must not remove the device again.
        unsigned rm_after_assign = mock_i2c_rm_device_count();

        auto c(std::move(b));
        CHECK(mock_i2c_rm_device_count() == rm_after_assign);

        // The stolen device still works.
        c.set_mute(true);
        CHECK(mock_i2c_wrote(REG_MUTERANGE_L, 0x03));
    }
    // c's destructor removes the device that a originally registered.
    CHECK(mock_i2c_rm_device_count() == rm_before + 2);
}

TEST_CASE("move assignment tolerates an rm_device failure")
{
    mock_i2c_reset();
    {
        auto a = es7210<>(kBus, kRx);
        auto b = es7210<>(kBus, kRx);
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
    {
        auto a = es7210<>(kBus, kRx);
        auto b = es7210<>(kBus, kRx);
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
        a.set_mute(true);
        CHECK(mock_i2c_wrote(REG_MUTERANGE_L, 0x03));
    }
    // a's destructor removes the device that a originally registered.
    CHECK(mock_i2c_rm_device_count() == 2);
}

TEST_CASE("all sample-rate configurations in the coefficient table are reachable")
{
    // The default template instantiation uses MCLK multiple 256, so it only
    // reaches the 4.096 MHz @ 16 kHz and 12.288 MHz @ 48 kHz entries. The
    // instantiations below exercise the remaining (mclk, lrck) pairs. MCLK
    // multiples that are not named in i2s_mclk_multiple_t select additional
    // coefficient-table entries; the clang analyzer flags those casts, so
    // they are guarded out of the clang build like in the es8311 tests.
    mock_i2c_reset();
    {
        es7210<I2S_MCLK_MULTIPLE_512> adc(kBus, kRx, 8000);
        CHECK(adc.get_sample_rate() == 8000);
    }
    mock_i2c_reset();
    {
        es7210<I2S_MCLK_MULTIPLE_1024> adc(kBus, kRx, 11025);
        CHECK(adc.get_sample_rate() == 11025);
    }
    mock_i2c_reset();
    {
        es7210<I2S_MCLK_MULTIPLE_1024> adc(kBus, kRx, 12000);
        CHECK(adc.get_sample_rate() == 12000);
    }
    mock_i2c_reset();
    {
        es7210<I2S_MCLK_MULTIPLE_256> adc(kBus, kRx, 16000);
        CHECK(adc.get_sample_rate() == 16000);
    }
    mock_i2c_reset();
    {
        es7210<I2S_MCLK_MULTIPLE_1024> adc(kBus, kRx, 16000);
        CHECK(adc.get_sample_rate() == 16000);
    }
    mock_i2c_reset();
    {
        es7210<I2S_MCLK_MULTIPLE_768> adc(kBus, kRx, 16000);
        CHECK(adc.get_sample_rate() == 16000);
    }
    mock_i2c_reset();
    {
        es7210<I2S_MCLK_MULTIPLE_512> adc(kBus, kRx, 22050);
        CHECK(adc.get_sample_rate() == 22050);
    }
    mock_i2c_reset();
    {
        es7210<I2S_MCLK_MULTIPLE_512> adc(kBus, kRx, 24000);
        CHECK(adc.get_sample_rate() == 24000);
    }
    mock_i2c_reset();
    {
        es7210<I2S_MCLK_MULTIPLE_256> adc(kBus, kRx, 32000);
        CHECK(adc.get_sample_rate() == 32000);
    }
    mock_i2c_reset();
    {
        es7210<I2S_MCLK_MULTIPLE_384> adc(kBus, kRx, 32000);
        CHECK(adc.get_sample_rate() == 32000);
    }
    mock_i2c_reset();
    {
        es7210<I2S_MCLK_MULTIPLE_512> adc(kBus, kRx, 32000);
        CHECK(adc.get_sample_rate() == 32000);
    }
    mock_i2c_reset();
    {
        es7210<I2S_MCLK_MULTIPLE_256> adc(kBus, kRx, 44100);
        CHECK(adc.get_sample_rate() == 44100);
    }
    mock_i2c_reset();
    {
        es7210<I2S_MCLK_MULTIPLE_256> adc(kBus, kRx, 48000);
        CHECK(adc.get_sample_rate() == 48000);
    }
    mock_i2c_reset();
    {
        es7210<I2S_MCLK_MULTIPLE_256> adc(kBus, kRx, 64000);
        CHECK(adc.get_sample_rate() == 64000);
    }
    mock_i2c_reset();
    {
        es7210<I2S_MCLK_MULTIPLE_128> adc(kBus, kRx, 88200);
        CHECK(adc.get_sample_rate() == 88200);
    }
    mock_i2c_reset();
    {
        es7210<I2S_MCLK_MULTIPLE_128> adc(kBus, kRx, 96000);
        CHECK(adc.get_sample_rate() == 96000);
    }
#ifndef __clang__
    mock_i2c_reset();
    {
        es7210<static_cast<i2s_mclk_multiple_t>(1536)> adc(kBus, kRx, 8000);
        CHECK(adc.get_sample_rate() == 8000);
    }
    mock_i2c_reset();
    {
        es7210<static_cast<i2s_mclk_multiple_t>(2048)> adc(kBus, kRx, 8000);
        CHECK(adc.get_sample_rate() == 8000);
    }
    mock_i2c_reset();
    {
        es7210<static_cast<i2s_mclk_multiple_t>(2400)> adc(kBus, kRx, 8000);
        CHECK(adc.get_sample_rate() == 8000);
    }
    mock_i2c_reset();
    {
        es7210<static_cast<i2s_mclk_multiple_t>(1600)> adc(kBus, kRx, 12000);
        CHECK(adc.get_sample_rate() == 12000);
    }
    mock_i2c_reset();
    {
        es7210<static_cast<i2s_mclk_multiple_t>(1200)> adc(kBus, kRx, 16000);
        CHECK(adc.get_sample_rate() == 16000);
    }
    mock_i2c_reset();
    {
        es7210<static_cast<i2s_mclk_multiple_t>(800)> adc(kBus, kRx, 24000);
        CHECK(adc.get_sample_rate() == 24000);
    }
    mock_i2c_reset();
    {
        es7210<static_cast<i2s_mclk_multiple_t>(600)> adc(kBus, kRx, 32000);
        CHECK(adc.get_sample_rate() == 32000);
    }
    mock_i2c_reset();
    {
        es7210<static_cast<i2s_mclk_multiple_t>(400)> adc(kBus, kRx, 48000);
        CHECK(adc.get_sample_rate() == 48000);
    }
    mock_i2c_reset();
    {
        es7210<static_cast<i2s_mclk_multiple_t>(300)> adc(kBus, kRx, 64000);
        CHECK(adc.get_sample_rate() == 64000);
    }
    mock_i2c_reset();
    {
        es7210<static_cast<i2s_mclk_multiple_t>(200)> adc(kBus, kRx, 96000);
        CHECK(adc.get_sample_rate() == 96000);
    }
#endif
}

TEST_CASE("the 32-bit I2S instantiation configures and reads 32-bit samples")
{
    mock_i2c_reset();
    {
        es7210<I2S_MCLK_MULTIPLE_256, true> adc(kBus, kRx, 48000);
        // 32-bit I2S format.
        CHECK(mock_i2c_wrote(REG_SDP_INTERFACE1, 0x80));

        std::vector<int32_t> samples{ 1, -2, 3, -4 };
        mock_i2s_set_read_data(samples.data(), samples.size() * sizeof(int32_t));
        std::vector<int32_t> buf(4);
        size_t read_bytes = adc.read(buf);
        CHECK(read_bytes == samples.size() * sizeof(int32_t));
        CHECK(std::memcmp(buf.data(), samples.data(), read_bytes) == 0);
    }
}

TEST_CASE("start can be retried after a partial power-up failure")
{
    mock_i2c_reset();
    {
        es7210<> adc(kBus, kRx);
        // Let the first two writes (CLOCK_OFF, POWER_DOWN) through and fail
        // the third (MIC1_POWER): the sequence throws after the chip was
        // already touched.
        mock_i2c_fail_writes_times(1, 2);
        CHECK_THROWS_AS(adc.start(), es7210<>::exception);
        // The failed start must not turn the retry into a silent no-op.
        adc.start();
        CHECK(mock_i2c_wrote(REG_POWER_DOWN, 0x00));
        CHECK(mock_i2c_wrote(REG_ANALOG, 0x43));
    }
}

TEST_CASE("stop powers down a device left half started by a failed start")
{
    mock_i2c_reset();
    {
        es7210<> adc(kBus, kRx);
        mock_i2c_fail_writes_times(1, 2);
        CHECK_THROWS_AS(adc.start(), es7210<>::exception);
        // A failed start is still cleaned up by stop().
        adc.stop();
        CHECK(mock_i2c_wrote(REG_ANALOG, 0xc0));
        CHECK(mock_i2c_wrote(REG_POWER_DOWN, 0x07));
    }
}

TEST_CASE("a moved-from object is inert and reports itself invalid")
{
    mock_i2c_reset();
    {
        auto a = es7210<>(kBus, kRx);
        auto b(std::move(a));
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
        CHECK(!a.valid());
        CHECK(b.valid());
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
        CHECK_THROWS_AS(a.start(), es7210<>::exception);
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
        CHECK_THROWS_AS(a.stop(), es7210<>::exception);
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
        CHECK_THROWS_AS((void) a.is_7210(), es7210<>::exception);
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
        CHECK_THROWS_AS((void) a.get_version(), es7210<>::exception);
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
        CHECK_THROWS_AS(a.set_mute(true), es7210<>::exception);
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
        CHECK_THROWS_AS(a.set_gain(es7210<>::GAIN_0DB), es7210<>::exception);
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
        CHECK_THROWS_AS(a.set_sample_rate(16000), es7210<>::exception);
        std::vector<int16_t> buf(4);
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
        CHECK_THROWS_AS((void) a.read(buf), es7210<>::exception);
        // get_sample_rate() only reads the cached value.
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
        CHECK(a.get_sample_rate() == 16000);
    }
}

TEST_CASE("set_sample_rate keeps the old rate on a partial write failure")
{
    mock_i2c_reset();
    {
        es7210<> adc(kBus, kRx, 16000);
        CHECK(adc.get_sample_rate() == 16000);
        // Let the REG_MAINCLK write through and fail the REG_OSR one.
        mock_i2c_fail_writes_times(1, 1);
        CHECK_THROWS_AS(adc.set_sample_rate(64000), es7210<>::exception);
        // The cached rate is unchanged even though REG_MAINCLK was written:
        // the hardware and get_sample_rate() disagree until the next
        // successful set_sample_rate().
        CHECK(adc.get_sample_rate() == 16000);
        CHECK(mock_i2c_wrote(REG_MAINCLK, 0x81));
    }
}

TEST_CASE("read is callable on a const es7210")
{
    mock_i2c_reset();
    {
        es7210<> adc(kBus, kRx);
        const es7210<> &cref = adc;
        std::vector<int16_t> buf(4);
        CHECK(cref.read(buf) == 0);
    }
}

TEST_CASE("unsupported sample rate is rejected by the constructor")
{
    mock_i2c_reset();
    // 44101 Hz @ MCLK 256 has no matching coefficient-table entry.
    CHECK_THROWS_AS(es7210<>(kBus, kRx, 44101), es7210<>::exception);
    CHECK(mock_i2c_rm_device_count() == 1);
}

}   // namespace
