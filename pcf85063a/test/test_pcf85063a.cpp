/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cstdint>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include <doctest/doctest.h>

#include <sys/time.h>

#include <esp_log.h>

#include "mock_bus.hpp"
#include "mock_time.hpp"
#include "pcf85063a.hpp"

using namespace mock_bus;

namespace {

// Handles that pass the constructor's null checks but are never dereferenced:
// the I2C driver entry points are redirected to the mocks in mock_bus.cpp
// (components/utils/mocks) by the linker --wrap options in its CMakeLists.txt.
const auto kBus = reinterpret_cast<i2c_master_bus_handle_t>(0x1);

// PCF85063A register addresses (mirrors the private constants in pcf85063a.hpp).
constexpr uint8_t REG_CTRL_1 = 0x00;
constexpr uint8_t REG_CTRL_2 = 0x01;
constexpr uint8_t REG_OFFSET = 0x02;
constexpr uint8_t REG_SECONDS = 0x04;
constexpr uint8_t REG_MINUTES = 0x05;
constexpr uint8_t REG_HOURS = 0x06;
constexpr uint8_t REG_DAYS = 0x07;
constexpr uint8_t REG_WEEKDAYS = 0x08;
constexpr uint8_t REG_MONTHS = 0x09;
constexpr uint8_t REG_YEARS = 0x0A;
constexpr uint8_t REG_SECOND_ALARM = 0x0B;
constexpr uint8_t REG_MINUTE_ALARM = 0x0C;
constexpr uint8_t REG_HOUR_ALARM = 0x0D;
constexpr uint8_t REG_DAY_ALARM = 0x0E;
constexpr uint8_t REG_WEEKDAY_ALARM = 0x0F;
constexpr uint8_t REG_TIMER_VAL = 0x10;
constexpr uint8_t REG_TIMER_MODE = 0x11;

constexpr uint8_t CTRL1_STOP = 0x20;
constexpr uint8_t CTRL2_AIE = 0x80;
constexpr uint8_t CTRL2_AF = 0x40;
constexpr uint8_t CTRL2_TF = 0x08;
constexpr uint8_t ALARM_DISABLED = 0x80;
constexpr uint8_t SECONDS_OS = 0x80;
constexpr uint8_t OFFSET_MODE = 0x80;

using rtc_t = pcf85063a<>;

rtc_t
make_rtc()
{
    return rtc_t(kBus);
}

rtc_t::datetime
dt_from_epoch(int64_t epoch)
{
    const time_t tt = static_cast<time_t>(epoch);
    struct tm tm_val{};
    gmtime_r(&tt, &tm_val);
    rtc_t::datetime t{};
    t.year = static_cast<uint16_t>(tm_val.tm_year + 1900);
    t.month = static_cast<uint8_t>(tm_val.tm_mon + 1);
    t.day = static_cast<uint8_t>(tm_val.tm_mday);
    t.hour = static_cast<uint8_t>(tm_val.tm_hour);
    t.minute = static_cast<uint8_t>(tm_val.tm_min);
    t.second = static_cast<uint8_t>(tm_val.tm_sec);
    return t;
}

void
set_datetime_regs(const rtc_t::datetime &t)
{
    mock_i2c_set_reg(REG_SECONDS, ((t.second / 10) << 4) | (t.second % 10));
    mock_i2c_set_reg(REG_MINUTES, ((t.minute / 10) << 4) | (t.minute % 10));
    mock_i2c_set_reg(REG_HOURS, ((t.hour / 10) << 4) | (t.hour % 10));
    mock_i2c_set_reg(REG_DAYS, ((t.day / 10) << 4) | (t.day % 10));
    mock_i2c_set_reg(REG_WEEKDAYS, t.dotw);
    mock_i2c_set_reg(REG_MONTHS, ((t.month / 10) << 4) | (t.month % 10));
    const uint8_t year = static_cast<uint8_t>(t.year - 1970);
    mock_i2c_set_reg(REG_YEARS, ((year / 10) << 4) | (year % 10));
}

}   // namespace

TEST_CASE("pcf85063a is movable but not copyable")
{
    static_assert(!std::is_copy_constructible_v<rtc_t>);
    static_assert(!std::is_copy_assignable_v<rtc_t>);
    static_assert(std::is_move_constructible_v<rtc_t>);
    static_assert(std::is_nothrow_move_constructible_v<rtc_t>);
    static_assert(std::is_nothrow_move_assignable_v<rtc_t>);
}

TEST_CASE("exception derives from std::exception")
{
    static_assert(std::is_base_of_v<std::exception, rtc_t::exception>);
}

TEST_CASE("exception includes the message and the error name")
{
    rtc_t::exception plain("boom");
    CHECK(std::strcmp(plain.what(), "pcf85063a: boom") == 0);

    rtc_t::exception with_err("i2c failed", ESP_ERR_INVALID_ARG);
    std::string msg(with_err.what());
    CHECK(msg.find("pcf85063a: i2c failed") == 0);
    CHECK(msg.find("ESP_ERR_INVALID_ARG") != std::string::npos);

    // A null message is treated as an empty one instead of dereferencing it.
    rtc_t::exception null_msg(nullptr);
    CHECK(std::strcmp(null_msg.what(), "pcf85063a: ") == 0);
}

TEST_CASE("constructor rejects a null I2C bus handle")
{
    CHECK_THROWS_AS(rtc_t(nullptr), rtc_t::exception);
}

TEST_CASE("constructor rejects a zero SCL speed")
{
    CHECK_THROWS_AS(rtc_t(kBus, 0), rtc_t::exception);
}

TEST_CASE("constructor registers the I2C device at address 0x51")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        CHECK(mock_i2c_add_device_count() == 1);
        CHECK(mock_i2c_last_device_address() == 0x51);
    }
    CHECK(mock_i2c_rm_device_count() == 1);
}

TEST_CASE("constructor reports i2c_master_bus_add_device failures")
{
    mock_i2c_reset();
    mock_i2c_fail_add_device(true);
    CHECK_THROWS_AS(make_rtc(), rtc_t::exception);
    mock_i2c_fail_add_device(false);
}

TEST_CASE("init starts the clock and clears the stop bit")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        // The full-byte write clears the previously set STOP bit while
        // programming the load capacitance.
        mock_i2c_set_reg(REG_CTRL_1, CTRL1_STOP);
        rtc.init();
        CHECK(mock_i2c_wrote(REG_CTRL_1, 0x01));
    }
}

TEST_CASE("init keeps the selected load capacitance")
{
    mock_i2c_reset();
    {
        pcf85063a<1970, true> rtc(kBus);
        rtc.init();
        CHECK(mock_i2c_wrote(REG_CTRL_1, 0x01));
    }
    mock_i2c_reset();
    {
        pcf85063a<1970, false> rtc(kBus);
        rtc.init();
        CHECK(mock_i2c_wrote(REG_CTRL_1, 0x00));
    }
    mock_i2c_reset();
    {
        // Null handle and zero speed rejection paths for the non-default
        // template instantiation as well.
        using rtc_false_t = pcf85063a<1970, false>;
        CHECK_THROWS_AS(rtc_false_t(nullptr), rtc_false_t::exception);
        CHECK_THROWS_AS(rtc_false_t(kBus, 0), rtc_false_t::exception);
    }
}

TEST_CASE("init reports an RTC that will not start")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        // The oscillator stop bit stays set even after clearing it (a stuck
        // bit): the driver must report that the clock is not running.
        mock_i2c_set_sticky_reg(REG_CTRL_1, CTRL1_STOP);
        CHECK_THROWS_AS(rtc.init(), rtc_t::exception);
    }
}

TEST_CASE("init is safe to call multiple times")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        rtc.init();
        rtc.init();
    }
}

TEST_CASE("reset issues a software reset and re-applies the capacitance")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        rtc.reset();
        // The datasheet magic byte 0x58 triggers the reset; a reset restores
        // the register defaults, so init() re-applies CAP_SEL (0x01).
        CHECK(mock_i2c_wrote(REG_CTRL_1, 0x58));
        CHECK(mock_i2c_wrote(REG_CTRL_1, 0x01));
    }
    mock_i2c_reset();
    {
        pcf85063a<1970, false> rtc(kBus);
        rtc.reset();
        CHECK(mock_i2c_wrote(REG_CTRL_1, 0x58));
        CHECK(mock_i2c_wrote(REG_CTRL_1, 0x00));
    }
}

TEST_CASE("stop and start toggle the stop bit")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        mock_i2c_set_reg(REG_CTRL_1, 0x00);
        rtc.stop();
        CHECK(mock_i2c_wrote(REG_CTRL_1, CTRL1_STOP));
        mock_i2c_set_reg(REG_CTRL_1, CTRL1_STOP);
        rtc.start();
        CHECK(mock_i2c_wrote(REG_CTRL_1, 0x00));
    }
}

TEST_CASE("oscillator_stop_flag reports and clears the OS bit")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        mock_i2c_set_reg(REG_SECONDS, SECONDS_OS);
        CHECK(rtc.oscillator_stop_flag());
        // Only the OS bit is cleared, the seconds value is preserved.
        CHECK(mock_i2c_wrote(REG_SECONDS, 0x00));
        CHECK(!rtc.oscillator_stop_flag());
    }
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        // A seconds value encoded in the same register must survive the
        // flag clearing.
        mock_i2c_set_reg(REG_SECONDS, SECONDS_OS | 0x12);
        CHECK(rtc.oscillator_stop_flag());
        CHECK(mock_i2c_wrote(REG_SECONDS, 0x12));
        CHECK(mock_i2c_get_reg(REG_SECONDS) == 0x12);
    }
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        // A register whose OS bit cannot be cleared (e.g. a dead oscillator):
        // the bounded retry loop must fail instead of spinning forever.
        mock_i2c_set_sticky_reg(REG_SECONDS, SECONDS_OS | 0x12);
        CHECK_THROWS_AS((void) rtc.oscillator_stop_flag(), rtc_t::exception);
    }
}

TEST_CASE("set_datetime validates the range")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        rtc_t::datetime t{};
        t.year = 2024;
        t.month = 1;
        t.day = 1;
        t.hour = 0;
        t.minute = 0;
        t.second = 0;
        CHECK_NOTHROW(rtc.set_datetime(t));
        t.year = 1969;
        CHECK_THROWS_AS(rtc.set_datetime(t), rtc_t::exception);
        t.year = 2071;
        CHECK_THROWS_AS(rtc.set_datetime(t), rtc_t::exception);
        t.year = 2024;
        t.month = 0;
        CHECK_THROWS_AS(rtc.set_datetime(t), rtc_t::exception);
        t.month = 13;
        CHECK_THROWS_AS(rtc.set_datetime(t), rtc_t::exception);
        t.month = 1;
        t.day = 0;
        CHECK_THROWS_AS(rtc.set_datetime(t), rtc_t::exception);
        t.day = 32;
        CHECK_THROWS_AS(rtc.set_datetime(t), rtc_t::exception);
        t.day = 1;
        // The caller's dotw is ignored (the weekday is recomputed), so an
        // invalid dotw does not throw.
        t.dotw = 7;
        CHECK_NOTHROW(rtc.set_datetime(t));
        t.dotw = 0;
        t.hour = 24;
        CHECK_THROWS_AS(rtc.set_datetime(t), rtc_t::exception);
        t.hour = 0;
        t.minute = 60;
        CHECK_THROWS_AS(rtc.set_datetime(t), rtc_t::exception);
        t.minute = 0;
        t.second = 60;
        CHECK_THROWS_AS(rtc.set_datetime(t), rtc_t::exception);
        t.second = 0;
        // Invalid calendar dates are rejected, including leap years.
        t.month = 2;
        t.day = 30;
        CHECK_THROWS_AS(rtc.set_datetime(t), rtc_t::exception);
        t.year = 2023;
        t.day = 29;
        CHECK_THROWS_AS(rtc.set_datetime(t), rtc_t::exception);
        t.year = 2024;
        t.day = 29;
        CHECK_NOTHROW(rtc.set_datetime(t));
        t.month = 4;
        t.day = 31;
        CHECK_THROWS_AS(rtc.set_datetime(t), rtc_t::exception);
        t.day = 30;
        CHECK_NOTHROW(rtc.set_datetime(t));
    }
}

TEST_CASE("set_datetime stops and restarts the clock while writing")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        rtc_t::datetime t{};
        t.year = 2024;
        t.month = 1;
        t.day = 1;
        CHECK_NOTHROW(rtc.set_datetime(t));
        CHECK(mock_i2c_wrote(REG_CTRL_1, CTRL1_STOP));
        CHECK(mock_i2c_wrote(REG_CTRL_1, 0x00));
    }
}

TEST_CASE("set_datetime restarts the clock even when the write fails")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        rtc_t::datetime t{};
        t.year = 2024;
        t.month = 1;
        t.day = 1;
        // The stop write succeeds, the time write fails: the catch block must
        // restart the clock before propagating. The restart write happens
        // while writes are still failing, so nothing is recorded here; the
        // recovery is verified below by re-enabling the last write only.
        mock_i2c_fail_writes(true);
        mock_i2c_fail_writes_times(1);
        CHECK_THROWS_AS(rtc.set_datetime(t), rtc_t::exception);
        mock_i2c_fail_writes(false);
        CHECK(mock_i2c_writes().empty());
    }
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        rtc_t::datetime t{};
        t.year = 2024;
        t.month = 1;
        t.day = 1;
        // The stop write (1st) succeeds, the time write (2nd) fails, and the
        // catch block still manages to restart the clock (3rd write).
        mock_i2c_fail_writes_times(1, 1);
        CHECK_THROWS_AS(rtc.set_datetime(t), rtc_t::exception);
        CHECK(mock_i2c_writes().size() == 2);
        CHECK(mock_i2c_writes()[0].reg == REG_CTRL_1);
        CHECK(mock_i2c_writes()[0].val == CTRL1_STOP);
        CHECK(mock_i2c_writes()[1].reg == REG_CTRL_1);
        CHECK(mock_i2c_writes()[1].val == 0x00);
    }
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        rtc_t::datetime t{};
        t.year = 2024;
        t.month = 1;
        t.day = 1;
        // Even when the restart write fails too, the original exception
        // (from the time write) is the one that propagates.
        mock_i2c_fail_writes_times(2, 1);
        CHECK_THROWS_AS(rtc.set_datetime(t), rtc_t::exception);
        CHECK(mock_i2c_writes().size() == 1);
    }
}

TEST_CASE("set_datetime writes the BCD registers")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        rtc_t::datetime t{};
        t.year = 2024;
        t.month = 12;
        t.day = 31;
        t.dotw = 2;
        t.hour = 23;
        t.minute = 59;
        t.second = 58;
        rtc.set_datetime(t);
        CHECK(mock_i2c_wrote(REG_SECONDS, 0x58));
        CHECK(mock_i2c_wrote(REG_MINUTES, 0x59));
        CHECK(mock_i2c_wrote(REG_HOURS, 0x23));
        CHECK(mock_i2c_wrote(REG_DAYS, 0x31));
        CHECK(mock_i2c_wrote(REG_WEEKDAYS, 2));
        CHECK(mock_i2c_wrote(REG_MONTHS, 0x12));
        // 2024 - 1970 = 54 -> BCD 0x54.
        CHECK(mock_i2c_wrote(REG_YEARS, 0x54));
    }
}

TEST_CASE("set_datetime computes the weekday")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        // 2024-01-01 was a Monday (dotw = 1).
        rtc_t::datetime t{};
        t.year = 2024;
        t.month = 1;
        t.day = 1;
        t.dotw = 5;   // ignored on write: the weekday is recomputed
        rtc.set_datetime(t);
        CHECK(mock_i2c_wrote(REG_WEEKDAYS, 1));
        // 2024-02-29 was a Thursday (dotw = 4).
        t.year = 2024;
        t.month = 2;
        t.day = 29;
        rtc.set_datetime(t);
        CHECK(mock_i2c_wrote(REG_WEEKDAYS, 4));
        // 2000-03-01 was a Wednesday (dotw = 3).
        t.year = 2000;
        t.month = 3;
        t.day = 1;
        rtc.set_datetime(t);
        CHECK(mock_i2c_wrote(REG_WEEKDAYS, 3));
    }
}

TEST_CASE("get_datetime decodes the BCD registers")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        rtc_t::datetime t{};
        t.year = 2024;
        t.month = 12;
        t.day = 31;
        t.dotw = 2;
        t.hour = 23;
        t.minute = 59;
        t.second = 58;
        set_datetime_regs(t);
        const rtc_t::datetime got = rtc.get_datetime();
        CHECK(got.year == 2024);
        CHECK(got.month == 12);
        CHECK(got.day == 31);
        CHECK(got.dotw == 2);
        CHECK(got.hour == 23);
        CHECK(got.minute == 59);
        CHECK(got.second == 58);
    }
}

TEST_CASE("get_datetime rejects a set oscillator-stop flag")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        rtc_t::datetime t{};
        t.year = 2024;
        t.month = 1;
        t.day = 1;
        set_datetime_regs(t);
        // The OS bit means the time registers survived a power loss and are
        // not trustworthy: the read must throw instead of returning the value.
        mock_i2c_set_reg(REG_SECONDS, SECONDS_OS | 0x12);
        CHECK_THROWS_AS((void) rtc.get_datetime(), rtc_t::exception);
    }
}

TEST_CASE("get_datetime rejects non-BCD register values")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        rtc_t::datetime t{};
        t.year = 2024;
        t.month = 1;
        t.day = 1;
        set_datetime_regs(t);
        // A non-BCD low nibble (month 0x1A) must be rejected as garbage, even
        // though its decoded value (26) would also fail the range check.
        mock_i2c_set_reg(REG_MONTHS, 0x1A);
        CHECK_THROWS_AS((void) rtc.get_datetime(), rtc_t::exception);
        mock_i2c_set_reg(REG_MONTHS, 0x01);
        // A non-BCD high nibble in the unmasked year register (0xA1) must be
        // rejected even though its decoded value (11) would look valid.
        mock_i2c_set_reg(REG_YEARS, 0xA1);
        CHECK_THROWS_AS((void) rtc.get_datetime(), rtc_t::exception);
    }
}

TEST_CASE("get_datetime rejects out-of-range register values")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        // A garbage month (BCD 0x99 -> 99, out of 1-12) must throw instead of
        // silently returning an invalid date.
        mock_i2c_set_reg(REG_MONTHS, 0x99);
        CHECK_THROWS_AS((void) rtc.get_datetime(), rtc_t::exception);
        // Invalid weekday (7 is outside the 0-6 encoding).
        mock_i2c_set_reg(REG_MONTHS, 0x01);
        mock_i2c_set_reg(REG_WEEKDAYS, 7);
        CHECK_THROWS_AS((void) rtc.get_datetime(), rtc_t::exception);
        mock_i2c_set_reg(REG_WEEKDAYS, 0);
        mock_i2c_set_reg(REG_SECONDS, 0x99);
        CHECK_THROWS_AS((void) rtc.get_datetime(), rtc_t::exception);
        mock_i2c_set_reg(REG_SECONDS, 0x00);
        mock_i2c_set_reg(REG_MINUTES, 0x99);
        CHECK_THROWS_AS((void) rtc.get_datetime(), rtc_t::exception);
        mock_i2c_set_reg(REG_MINUTES, 0x00);
        mock_i2c_set_reg(REG_HOURS, 0x99);
        CHECK_THROWS_AS((void) rtc.get_datetime(), rtc_t::exception);
        mock_i2c_set_reg(REG_HOURS, 0x00);
        mock_i2c_set_reg(REG_DAYS, 0x32);
        CHECK_THROWS_AS((void) rtc.get_datetime(), rtc_t::exception);
    }
}

TEST_CASE("get_datetime_and_set refuses to poison the clock with garbage")
{
    mock_i2c_reset();
    mock_time_reset();
    {
        auto rtc = make_rtc();
        rtc_t::datetime t{};
        t.year = 2024;
        t.month = 1;
        t.day = 1;
        set_datetime_regs(t);
        mock_i2c_set_reg(REG_MONTHS, 0x99);
        mock_time_set({ 1704067200, 0 });
        CHECK_THROWS_AS(rtc.get_datetime_and_set(), rtc_t::exception);
        CHECK(mock_time_settimeofday_count() == 0);
    }
    mock_time_reset();
}

TEST_CASE("get_datetime_and_set sets the system time")
{
    mock_i2c_reset();
    mock_time_reset();
    {
        auto rtc = make_rtc();
        rtc_t::datetime t{};
        t.year = 2024;
        t.month = 1;
        t.day = 1;
        t.hour = 0;
        t.minute = 0;
        t.second = 0;
        set_datetime_regs(t);
        mock_time_set({ 1704067200, 0 });
        rtc.get_datetime_and_set();
        CHECK(mock_time_settimeofday_count() == 1);
        const timeval set = mock_time_last_settimeofday();
        CHECK(set.tv_sec == 1704067200);
        CHECK(set.tv_usec == 0);
    }
    mock_time_reset();
}

TEST_CASE("get_datetime_and_set handles non-January dates")
{
    mock_i2c_reset();
    mock_time_reset();
    {
        auto rtc = make_rtc();
        rtc_t::datetime t{};
        t.year = 2024;
        t.month = 5;
        t.day = 15;
        t.hour = 0;
        t.minute = 0;
        t.second = 0;
        set_datetime_regs(t);
        mock_time_set({ 0, 0 });
        rtc.get_datetime_and_set();
        const timeval set = mock_time_last_settimeofday();
        CHECK(set.tv_sec == 1715731200);
    }
    mock_time_reset();
}

TEST_CASE("get_datetime_and_set reports settimeofday failures")
{
    mock_i2c_reset();
    mock_time_reset();
    {
        auto rtc = make_rtc();
        rtc_t::datetime t{};
        t.year = 2024;
        t.month = 1;
        t.day = 1;
        t.hour = 0;
        t.minute = 0;
        t.second = 0;
        set_datetime_regs(t);
        mock_time_set({ 0, 0 });
        mock_time_fail_settimeofday(true);
        CHECK_THROWS_AS(rtc.get_datetime_and_set(), rtc_t::exception);
        mock_time_fail_settimeofday(false);
    }
    mock_time_reset();
}

TEST_CASE("get_datetime_and_adjust schedules an adjtime correction")
{
    mock_i2c_reset();
    mock_time_reset();
    {
        auto rtc = make_rtc();
        // The RTC reads 2024-01-01 00:00:30, the ESP32 clock 00:00:00: the
        // adjustment must be +30 s and the time must not be set immediately.
        rtc_t::datetime t{};
        t.year = 2024;
        t.month = 1;
        t.day = 1;
        t.hour = 0;
        t.minute = 0;
        t.second = 30;
        set_datetime_regs(t);
        mock_time_set({ 1704067200, 0 });
        rtc.get_datetime_and_adjust();
        CHECK(mock_time_adjtime_count() == 1);
        const timeval delta = mock_time_last_adjtime();
        CHECK(delta.tv_sec == 30);
        CHECK(delta.tv_usec == 0);
        CHECK(mock_time_settimeofday_count() == 0);
    }
    mock_time_reset();
}

TEST_CASE("get_datetime_and_adjust schedules a negative correction")
{
    mock_i2c_reset();
    mock_time_reset();
    {
        auto rtc = make_rtc();
        // The RTC reads 2024-01-01 00:00:00, the ESP32 clock 00:00:30: the
        // adjustment must be -30 s.
        rtc_t::datetime t{};
        t.year = 2024;
        t.month = 1;
        t.day = 1;
        t.hour = 0;
        t.minute = 0;
        t.second = 0;
        set_datetime_regs(t);
        mock_time_set({ 1704067230, 0 });
        rtc.get_datetime_and_adjust();
        CHECK(mock_time_adjtime_count() == 1);
        const timeval delta = mock_time_last_adjtime();
        CHECK(delta.tv_sec == -30);
        CHECK(delta.tv_usec == 0);
    }
    mock_time_reset();
}

TEST_CASE("get_datetime_and_adjust reports adjtime failures")
{
    mock_i2c_reset();
    mock_time_reset();
    {
        auto rtc = make_rtc();
        rtc_t::datetime t{};
        t.year = 2024;
        t.month = 1;
        t.day = 1;
        t.hour = 0;
        t.minute = 0;
        t.second = 0;
        set_datetime_regs(t);
        mock_time_set({ 1704067200, 0 });
        mock_time_fail_adjtime(true);
        CHECK_THROWS_AS(rtc.get_datetime_and_adjust(), rtc_t::exception);
        mock_time_fail_adjtime(false);
    }
    mock_time_reset();
}

TEST_CASE("get_datetime_and_adjust includes the sub-second offset")
{
    mock_i2c_reset();
    mock_time_reset();
    {
        auto rtc = make_rtc();
        rtc_t::datetime t{};
        t.year = 2024;
        t.month = 1;
        t.day = 1;
        t.hour = 0;
        t.minute = 0;
        t.second = 0;
        set_datetime_regs(t);
        // The ESP32 clock is half a second ahead of the RTC: the adjustment is
        // -0.5 s, normalized to a canonical timeval {-1, 500000}.
        mock_time_set({ 1704067200, 500000 });
        rtc.get_datetime_and_adjust();
        const timeval delta = mock_time_last_adjtime();
        CHECK(delta.tv_sec == -1);
        CHECK(delta.tv_usec == 500000);
    }
    mock_time_reset();
}

TEST_CASE("set_datetime_with_drift sets the time and accumulates the offset")
{
    mock_i2c_reset();
    mock_time_reset();
    {
        auto rtc = make_rtc();
        const int64_t t0 = 1704067200;   // 2024-01-01 00:00:00 UTC
        // First sync: RTC and ESP32 agree, so no drift is measured.
        set_datetime_regs(dt_from_epoch(t0));
        mock_time_set({ t0, 0 });
        rtc.set_datetime_with_drift();
        CHECK(mock_time_settimeofday_count() == 0);
        // The RTC was written with the ESP32 time (2024 -> BCD 0x54).
        CHECK(mock_i2c_wrote(REG_YEARS, 0x54));

        // The RTC ran 5 ppm fast over 1e6 s: it reads 5 s more than the
        // interval the ESP32 clock elapsed (1e6 + 5 s), i.e. RTC interval =
        // 1000005 s while esp_delta = 1000000 s.
        const int64_t esp1 = t0 + 1000000;
        set_datetime_regs(dt_from_epoch(t0 + 1000005));
        mock_time_set({ esp1, 0 });
        rtc.set_datetime_with_drift();
        CHECK(mock_i2c_wrote(REG_OFFSET, 0x01));

        // A third sync with the same +5 ppm accumulates on top of the previous
        // offset: the total becomes 2 normal LSB.
        const int64_t esp2 = esp1 + 1000000;
        set_datetime_regs(dt_from_epoch(esp1 + 1000005));
        mock_time_set({ esp2, 0 });
        rtc.set_datetime_with_drift();
        CHECK(mock_i2c_wrote(REG_OFFSET, 0x02));
    }
    mock_time_reset();
}

TEST_CASE("set_datetime_with_drift ignores the pre-sync offset")
{
    mock_i2c_reset();
    mock_time_reset();
    {
        auto rtc = make_rtc();
        const int64_t t0 = 1704067200;   // 2024-01-01 00:00:00 UTC
        // The RTC is initially 60 s slow; the first sync overwrites it with
        // the ESP32 time (t0), so that is where its counter starts from.
        set_datetime_regs(dt_from_epoch(t0 - 60));
        mock_time_set({ t0, 0 });
        rtc.set_datetime_with_drift();

        // One hour later both clocks advanced by exactly 3600 s: the drift is
        // 0. Measuring the RTC interval against the pre-sync reading would
        // re-import the removed 60 s offset as a ~16666 ppm rate error and
        // program a bogus correction; the baseline must be the value written
        // to the chip instead.
        set_datetime_regs(dt_from_epoch(t0 + 3600));
        mock_time_set({ t0 + 3600, 0 });
        rtc.set_datetime_with_drift();
        CHECK(mock_i2c_wrote(REG_OFFSET, 0x00));
        CHECK(mock_i2c_get_reg(REG_OFFSET) == 0x00);
    }
    mock_time_reset();
}

TEST_CASE("set_datetime_with_drift skips the first measurement")
{
    mock_i2c_reset();
    mock_time_reset();
    {
        auto rtc = make_rtc();
        rtc_t::datetime t{};
        t.year = 2024;
        t.month = 1;
        t.day = 1;
        set_datetime_regs(t);
        mock_time_set({ 1704067200, 0 });
        rtc.set_datetime_with_drift();
        // The offset register is only touched when there is a drift to
        // correct (i.e. starting from the second sync).
        CHECK(!mock_i2c_wrote(REG_OFFSET, 0x00));
    }
    mock_time_reset();
}

TEST_CASE("set_datetime_with_drift does not divide by zero")
{
    mock_i2c_reset();
    mock_time_reset();
    {
        auto rtc = make_rtc();
        rtc_t::datetime t{};
        t.year = 2024;
        t.month = 1;
        t.day = 1;
        set_datetime_regs(t);
        mock_time_set({ 1704067200, 0 });
        rtc.set_datetime_with_drift();
        // Same ESP32 time on the second call: esp_delta == 0, the drift
        // correction must be skipped.
        rtc.set_datetime_with_drift();
        CHECK(mock_i2c_wrote(REG_YEARS, 0x54));
    }
    mock_time_reset();
}

TEST_CASE("set_datetime_with_drift converts non-January dates")
{
    mock_i2c_reset();
    mock_time_reset();
    {
        auto rtc = make_rtc();
        // 2024-05-15 00:00:00: a date with month > 2, exercising the other
        // side of the civil-calendar conversions.
        rtc_t::datetime t{};
        t.year = 2024;
        t.month = 5;
        t.day = 15;
        set_datetime_regs(t);
        mock_time_set({ 1715731200, 0 });
        rtc.set_datetime_with_drift();
        CHECK(mock_i2c_wrote(REG_YEARS, 0x54));
        CHECK(mock_i2c_wrote(REG_MONTHS, 0x05));
    }
    mock_time_reset();
}

TEST_CASE("set_datetime_with_drift handles dates before the Unix epoch")
{
    mock_i2c_reset();
    mock_time_reset();
    {
        auto rtc = make_rtc();
        // RTC reads 1969-12-31 23:59:59 (one second before the epoch), the
        // ESP32 time matches: the negative-epoch conversion path is used.
        // The resulting year (1969) is outside the RTC's supported range, so
        // set_datetime() rejects it.
        rtc_t::datetime t{};
        t.year = 1969;
        t.month = 12;
        t.day = 31;
        t.hour = 23;
        t.minute = 59;
        t.second = 59;
        set_datetime_regs(t);
        mock_time_set({ -1, 0 });
        CHECK_THROWS_AS(rtc.set_datetime_with_drift(), rtc_t::exception);
    }
    mock_time_reset();
}

TEST_CASE("set_datetime_with_drift handles very old dates")
{
    mock_i2c_reset();
    mock_time_reset();
    {
        auto rtc = make_rtc();
        // An ESP32 time well before the Unix epoch: the negative-day
        // civil_from_days() path is used. The resulting year is outside the
        // RTC's supported range, so set_datetime() rejects it.
        mock_time_set({ -62167219201LL, 0 });
        CHECK_THROWS_AS(rtc.set_datetime_with_drift(), rtc_t::exception);
    }
    mock_time_reset();
}

TEST_CASE("set_datetime_with_drift rejects years outside the 16-bit range")
{
    mock_i2c_reset();
    mock_time_reset();
    {
        auto rtc = make_rtc();
        // A corrupted system clock far outside the representable year range:
        // civil_from_days() must throw instead of silently wrapping the year.
        rtc_t::datetime t{};
        t.year = 2024;
        t.month = 1;
        t.day = 1;
        set_datetime_regs(t);
        mock_time_set({ INT64_MAX / 2, 0 });
        CHECK_THROWS_AS(rtc.set_datetime_with_drift(), rtc_t::exception);
        mock_time_set({ INT64_MIN / 2, 0 });
        CHECK_THROWS_AS(rtc.set_datetime_with_drift(), rtc_t::exception);
    }
    mock_time_reset();
}

TEST_CASE("disable_alarm disables the alarm and clears the flag")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        mock_i2c_set_reg(REG_CTRL_2, CTRL2_AIE | CTRL2_AF);
        rtc.disable_alarm();
        CHECK(mock_i2c_wrote(REG_CTRL_2, 0x00));
        for (uint8_t reg = REG_SECOND_ALARM; reg <= REG_WEEKDAY_ALARM; ++reg) {
            CHECK(mock_i2c_wrote(reg, ALARM_DISABLED));
        }
    }
}

TEST_CASE("alarm_flag reads the flag without clearing it")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        mock_i2c_set_reg(REG_CTRL_2, CTRL2_AF);
        CHECK(rtc.alarm_flag());
        CHECK(mock_i2c_writes().empty());
        mock_i2c_set_reg(REG_CTRL_2, 0x00);
        CHECK(!rtc.alarm_flag());
        CHECK(mock_i2c_writes().empty());
    }
}

TEST_CASE("check_and_clear_alarm_flag reports and clears the alarm flag")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        mock_i2c_set_reg(REG_CTRL_2, CTRL2_AF);
        CHECK(rtc.check_and_clear_alarm_flag());
        CHECK(mock_i2c_wrote(REG_CTRL_2, 0x00));
        CHECK(!rtc.check_and_clear_alarm_flag());
        // When the flag is not set, nothing is written.
        CHECK(mock_i2c_writes().size() == 1);
    }
}

TEST_CASE("set_alarm validates the time")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        rtc_t::datetime t{};
        t.hour = 24;
        CHECK_THROWS_AS(rtc.set_alarm_any_day(t), rtc_t::exception);
        t.hour = 0;
        t.minute = 60;
        CHECK_THROWS_AS(rtc.set_alarm_any_day(t), rtc_t::exception);
        t.minute = 0;
        t.second = 60;
        CHECK_THROWS_AS(rtc.set_alarm_any_day(t), rtc_t::exception);
    }
}

TEST_CASE("set_alarm_day validates the time and the day of month")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        rtc_t::datetime t{};
        t.day = 0;
        t.hour = 7;
        CHECK_THROWS_AS(rtc.set_alarm_day(t), rtc_t::exception);
        t.day = 32;
        CHECK_THROWS_AS(rtc.set_alarm_day(t), rtc_t::exception);
        t.day = 25;
        t.minute = 60;
        CHECK_THROWS_AS(rtc.set_alarm_day(t), rtc_t::exception);
        t.minute = 0;
        t.second = 60;
        CHECK_THROWS_AS(rtc.set_alarm_day(t), rtc_t::exception);
        t.second = 0;
        // Invalid times must not touch the registers or the callback.
        t.minute = 60;
        CHECK_THROWS_AS(rtc.set_alarm_day(t), rtc_t::exception);
        CHECK(mock_i2c_writes().empty());
    }
    mock_i2c_reset();
    {
        // The day alarm is only validated when the day match is actually
        // requested: set_alarm_any_day() ignores t.day.
        auto rtc = make_rtc();
        rtc_t::datetime t{};
        t.day = 32;
        t.hour = 7;
        CHECK_NOTHROW(rtc.set_alarm_any_day(t));
        t.minute = 60;
        CHECK_THROWS_AS(rtc.set_alarm_day(t), rtc_t::exception);
    }
}

TEST_CASE("set_alarm_any_day writes the alarm registers and enables the interrupt")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        rtc_t::datetime t{};
        t.hour = 7;
        t.minute = 30;
        t.second = 15;
        rtc.set_alarm_any_day(t);
        CHECK(mock_i2c_wrote(REG_SECOND_ALARM, 0x15));
        CHECK(mock_i2c_wrote(REG_MINUTE_ALARM, 0x30));
        CHECK(mock_i2c_wrote(REG_HOUR_ALARM, 0x07));
        CHECK(mock_i2c_wrote(REG_DAY_ALARM, ALARM_DISABLED));
        CHECK(mock_i2c_wrote(REG_WEEKDAY_ALARM, ALARM_DISABLED));
        // The interrupt is enabled and the pending flag cleared.
        CHECK(mock_i2c_wrote(REG_CTRL_2, CTRL2_AIE));
    }
}

TEST_CASE("set_alarm_day enables the day-of-month alarm")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        rtc_t::datetime t{};
        t.day = 25;
        t.hour = 7;
        t.minute = 30;
        t.second = 15;
        rtc.set_alarm_day(t);
        CHECK(mock_i2c_wrote(REG_DAY_ALARM, 0x25));
        CHECK(mock_i2c_wrote(REG_WEEKDAY_ALARM, ALARM_DISABLED));
    }
}

TEST_CASE("get_alarm decodes the alarm registers")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        mock_i2c_set_reg(REG_SECOND_ALARM, 0x15);
        mock_i2c_set_reg(REG_MINUTE_ALARM, 0x30);
        mock_i2c_set_reg(REG_HOUR_ALARM, 0x07);
        mock_i2c_set_reg(REG_DAY_ALARM, 0x25);
        mock_i2c_set_reg(REG_WEEKDAY_ALARM, 0x03);
        const rtc_t::datetime got = rtc.get_alarm();
        CHECK(got.second == 15);
        CHECK(got.minute == 30);
        CHECK(got.hour == 7);
        CHECK(got.day == 25);
        CHECK(got.dotw == 3);
    }
}

TEST_CASE("set_timer writes the value and mode registers and enables the timer")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        rtc.set_timer(42, rtc_t::timer_clock::CLK_64_HZ);
        CHECK(mock_i2c_wrote(REG_TIMER_VAL, 42));
        // The clock bits are written first, then the enable/interrupt bits.
        CHECK(mock_i2c_wrote(REG_TIMER_MODE, 0x08));
        CHECK(mock_i2c_wrote(REG_TIMER_MODE, 0x0E));
        // The pending timer flag is cleared in Control_2.
        CHECK(mock_i2c_wrote(REG_CTRL_2, 0x00));
    }
}

TEST_CASE("disable_timer clears the enable and interrupt bits")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        mock_i2c_set_reg(REG_TIMER_MODE, 0x1E);
        rtc.disable_timer();
        // The clock bits (0x18) are preserved; the enable and interrupt
        // enable bits are cleared.
        CHECK(mock_i2c_wrote(REG_TIMER_MODE, 0x18));
    }
}

TEST_CASE("timer_flag reads the flag without clearing it")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        mock_i2c_set_reg(REG_CTRL_2, CTRL2_TF);
        CHECK(rtc.timer_flag());
        CHECK(mock_i2c_writes().empty());
        mock_i2c_set_reg(REG_CTRL_2, 0x00);
        CHECK(!rtc.timer_flag());
        CHECK(mock_i2c_writes().empty());
    }
}

TEST_CASE("check_and_clear_timer_flag reports and clears the timer flag")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        mock_i2c_set_reg(REG_CTRL_2, CTRL2_TF);
        CHECK(rtc.check_and_clear_timer_flag());
        CHECK(mock_i2c_wrote(REG_CTRL_2, 0x00));
        CHECK(!rtc.check_and_clear_timer_flag());
        // When the flag is not set, nothing is written.
        CHECK(mock_i2c_writes().size() == 1);
    }
}

TEST_CASE("get_timer returns the timer length in seconds")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        mock_i2c_set_reg(REG_TIMER_VAL, 60);
        mock_i2c_set_reg(REG_TIMER_MODE, 0x10);
        CHECK(rtc.get_timer() == doctest::Approx(60.0));
        mock_i2c_set_reg(REG_TIMER_MODE, 0x08);
        CHECK(rtc.get_timer() == doctest::Approx(60.0 / 64.0));
        mock_i2c_set_reg(REG_TIMER_MODE, 0x00);
        CHECK(rtc.get_timer() == doctest::Approx(60.0 / 4096.0));
        mock_i2c_set_reg(REG_TIMER_MODE, 0x18);
        CHECK(rtc.get_timer() == doctest::Approx(60.0 * 60.0));
    }
}

TEST_CASE("set_timer in seconds selects the best clock")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        // Exactly one second: the 1 Hz clock with value 1.
        rtc.set_timer(1.0);
        CHECK(mock_i2c_wrote(REG_TIMER_VAL, 1));
        CHECK(mock_i2c_wrote(REG_TIMER_MODE, 0x10));
    }
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        // One minute: the 1 Hz clock with value 60 (the 1/60 Hz clock with
        // value 1 would be exactly one minute, but its smaller time base is
        // not selected: the loop picks the clock that minimizes the absolute
        // error and prefers the first (1 Hz) on ties).
        rtc.set_timer(60.0);
        CHECK(mock_i2c_wrote(REG_TIMER_VAL, 60));
        CHECK(mock_i2c_wrote(REG_TIMER_MODE, 0x10));
    }
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        // 10 seconds: 1 Hz clock with value 10.
        rtc.set_timer(10.0);
        CHECK(mock_i2c_wrote(REG_TIMER_VAL, 10));
        CHECK(mock_i2c_wrote(REG_TIMER_MODE, 0x10));
    }
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        // 10000 s: only the 1/60 Hz clock fits in 0..255 (167 * 60 = 10020 s);
        // the other clocks round to a value above 255 and are skipped.
        rtc.set_timer(10000.0);
        CHECK(mock_i2c_wrote(REG_TIMER_VAL, 167));
        CHECK(mock_i2c_wrote(REG_TIMER_MODE, 0x18));
    }
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        // A duration that cannot be represented by any clock.
        CHECK_THROWS_AS(rtc.set_timer(4.0 * 60.0 * 256.0), rtc_t::exception);
    }
}

TEST_CASE("set_timer in seconds rejects zero, negative and oversized durations")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        CHECK_THROWS_AS(rtc.set_timer(0.0), rtc_t::exception);
        CHECK_THROWS_AS(rtc.set_timer(-1.0), rtc_t::exception);
        CHECK_THROWS_AS(rtc.set_timer(1e6), rtc_t::exception);
        CHECK(mock_i2c_writes().empty());
    }
}

TEST_CASE("set_timer in seconds rejects a duration that rounds to zero")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        // For seconds < 0.5/4096 every clock rounds to zero: programming
        // Timer_value = 0 (the case the API documents as unsupported) must be
        // rejected instead of silently accepted.
        CHECK_THROWS_AS(rtc.set_timer(1e-4), rtc_t::exception);
        CHECK(mock_i2c_writes().empty());
    }
}

TEST_CASE("set_timer in raw mode rejects a zero value")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        // Both overloads share the same contract: a zero-length countdown is
        // not programmable.
        CHECK_THROWS_AS(rtc.set_timer(0, rtc_t::timer_clock::CLK_1_HZ), rtc_t::exception);
        CHECK(mock_i2c_writes().empty());
    }
}

TEST_CASE("interrupt_handler invokes the alarm and timer callbacks")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        bool alarm = false;
        bool timer = false;
        rtc_t::datetime t{};
        t.hour = 7;
        t.minute = 30;
        t.second = 15;
        rtc.set_alarm_any_day(t, [&] { alarm = true; });
        rtc.set_timer(1.0, [&] { timer = true; });
        mock_i2c_set_reg(REG_CTRL_2, CTRL2_AF | CTRL2_TF);
        rtc.interrupt_handler();
        CHECK(alarm);
        CHECK(timer);
    }
    mock_i2c_reset();
    {
        // Only the alarm flag is set: the timer branch must be skipped.
        auto rtc = make_rtc();
        bool alarm = false;
        rtc_t::datetime t{};
        t.hour = 7;
        t.minute = 30;
        t.second = 15;
        rtc.set_alarm_any_day(t, [&] { alarm = true; });
        mock_i2c_set_reg(REG_CTRL_2, CTRL2_AF);
        rtc.interrupt_handler();
        CHECK(alarm);
    }
    mock_i2c_reset();
    {
        // Only the timer flag is set: the alarm branch must be skipped.
        auto rtc = make_rtc();
        bool timer = false;
        rtc.set_timer(1.0, [&] { timer = true; });
        mock_i2c_set_reg(REG_CTRL_2, CTRL2_TF);
        rtc.interrupt_handler();
        CHECK(timer);
    }
}

TEST_CASE("interrupt_handler warns when no callback is registered")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        mock_i2c_set_reg(REG_CTRL_2, CTRL2_AF | CTRL2_TF);
        rtc.interrupt_handler();
    }
}

TEST_CASE("interrupt_handler contains throwing callbacks")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        rtc_t::datetime t{};
        t.hour = 7;
        t.minute = 30;
        t.second = 15;
        rtc.set_alarm_any_day(t, [] { throw std::runtime_error("alarm boom"); });
        rtc.set_timer(1.0, [] { throw 42; });
        mock_i2c_set_reg(REG_CTRL_2, CTRL2_AF | CTRL2_TF);
        // Both callbacks run even though each one throws, and the exception
        // never escapes interrupt_handler().
        CHECK_NOTHROW(rtc.interrupt_handler());
        CHECK(mock_i2c_get_reg(REG_CTRL_2) == 0);
    }
    mock_i2c_reset();
    {
        // A throwing alarm callback must not prevent the timer callback from
        // running, and vice versa: both exception flavors are exercised.
        auto rtc = make_rtc();
        bool timer = false;
        rtc_t::datetime t{};
        t.hour = 7;
        t.minute = 30;
        t.second = 15;
        rtc.set_alarm_any_day(t, [] { throw 42; });
        rtc.set_timer(1.0, [&] {
            timer = true;
            throw std::runtime_error("timer boom");
        });
        mock_i2c_set_reg(REG_CTRL_2, CTRL2_AF | CTRL2_TF);
        CHECK_NOTHROW(rtc.interrupt_handler());
        CHECK(timer);
    }
}

TEST_CASE("to_str formats the datetime")
{
    rtc_t::datetime t{};
    t.year = 2024;
    t.month = 12;
    t.day = 31;
    t.dotw = 2;
    t.hour = 23;
    t.minute = 59;
    t.second = 58;
    char buf[32];
    rtc_t::to_str(buf, sizeof(buf), t);
    CHECK(std::strcmp(buf, "2024.12.31 2 23:59:58") == 0);
}

TEST_CASE("to_str handles null and truncated buffers")
{
    // Keep the values opaque to the optimizer (the branches are compiled out
    // when the whole call is constant-folded on -Os RISC-V builds).
    rtc_t::datetime t{};
    t.year = 2024;
    t.month = 12;
    t.day = 31;
    t.dotw = 2;
    t.hour = 23;
    t.minute = 59;
    t.second = 58;
    volatile char *volatile buf = nullptr;
    volatile size_t len = 0;
    rtc_t::to_str(const_cast<char *>(buf), len, t);
    char small[8];
    volatile char *volatile small_buf = small;
    rtc_t::to_str(const_cast<char *>(small_buf), len, t);
    volatile size_t small_len = sizeof(small);
    rtc_t::to_str(const_cast<char *>(small_buf), small_len, t);
    CHECK(small[7] == '\0');
}

TEST_CASE("set_drift_correction selects the best mode")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        // 0 ppm: mode 0 with offset 0.
        rtc.set_drift_correction(0);
        CHECK(mock_i2c_wrote(REG_OFFSET, 0x00));
    }
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        // 5 ppm is closer to one normal LSB (4.34 ppm, error 0.66) than to one
        // coarse LSB (4.069 ppm, error 0.93): mode 0.
        rtc.set_drift_correction(5);
        CHECK(mock_i2c_wrote(REG_OFFSET, 0x01));
    }
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        // 4 ppm is closer to one coarse LSB (error 0.069) than to one normal
        // LSB (error 0.34): the coarse mode is selected.
        rtc.set_drift_correction(4);
        CHECK(mock_i2c_wrote(REG_OFFSET, OFFSET_MODE | 0x01));
    }
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        // A large positive drift saturates at +63 LSB.
        rtc.set_drift_correction(1000000);
        CHECK(mock_i2c_wrote(REG_OFFSET, 0x3F));
    }
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        // A large negative drift saturates at -64 LSB (two's complement).
        rtc.set_drift_correction(-1000000);
        CHECK(mock_i2c_wrote(REG_OFFSET, 0x40));
    }
}

TEST_CASE("set_drift_correction accumulates with the previous offset")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        // A previously programmed +63 normal LSB (273.42 ppm) plus +5 ppm
        // saturates at +63 LSB.
        mock_i2c_set_reg(REG_OFFSET, 0x3F);
        rtc.set_drift_correction(5);
        CHECK(mock_i2c_wrote(REG_OFFSET, 0x3F));
    }
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        // A previously programmed -1 normal LSB (-4.34 ppm, two's complement
        // 0x7F) plus +4 ppm cancels out to 0 LSB.
        mock_i2c_set_reg(REG_OFFSET, 0x7F);
        rtc.set_drift_correction(4);
        CHECK(mock_i2c_wrote(REG_OFFSET, 0x00));
    }
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        // The previous offset is converted to ppm using its own mode: +1
        // coarse LSB (4.069 ppm) plus +1 ppm totals 5.069 ppm, best
        // approximated by +1 normal LSB (4.34 ppm, error 0.729) rather than by
        // +1 coarse LSB (error 1.0), so the mode changes to normal.
        mock_i2c_set_reg(REG_OFFSET, OFFSET_MODE | 0x01);
        rtc.set_drift_correction(1);
        CHECK(mock_i2c_wrote(REG_OFFSET, 0x01));
    }
}

TEST_CASE("reset_drift_correction zeroes the offset register")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        rtc.reset_drift_correction();
        CHECK(mock_i2c_wrote(REG_OFFSET, 0x00));
    }
}

TEST_CASE("I2C read and write failures are reported")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();

        mock_i2c_fail_reads(true);
        CHECK_THROWS_AS((void) rtc.get_datetime(), rtc_t::exception);
        CHECK_THROWS_AS((void) rtc.get_alarm(), rtc_t::exception);
        CHECK_THROWS_AS((void) rtc.get_timer(), rtc_t::exception);
        CHECK_THROWS_AS((void) rtc.alarm_flag(), rtc_t::exception);
        CHECK_THROWS_AS((void) rtc.timer_flag(), rtc_t::exception);
        CHECK_THROWS_AS(rtc.set_drift_correction(1000), rtc_t::exception);
        mock_i2c_fail_reads(false);

        mock_i2c_fail_writes(true);
        CHECK_THROWS_AS(rtc.init(), rtc_t::exception);
        CHECK_THROWS_AS(rtc.reset(), rtc_t::exception);
        CHECK_THROWS_AS(rtc.stop(), rtc_t::exception);
        CHECK_THROWS_AS(rtc.start(), rtc_t::exception);
        // The OS flag is set in the register, so clearing it requires a write.
        mock_i2c_set_reg(REG_SECONDS, SECONDS_OS);
        CHECK_THROWS_AS((void) rtc.oscillator_stop_flag(), rtc_t::exception);
        mock_i2c_set_reg(REG_SECONDS, 0);
        rtc_t::datetime t{};
        t.year = 2024;
        t.month = 1;
        t.day = 1;
        CHECK_THROWS_AS(rtc.set_datetime(t), rtc_t::exception);
        CHECK_THROWS_AS(rtc.disable_alarm(), rtc_t::exception);
        CHECK_THROWS_AS(rtc.set_alarm_any_day(t), rtc_t::exception);
        CHECK_THROWS_AS(rtc.set_alarm_day(t), rtc_t::exception);
        CHECK_THROWS_AS(rtc.set_timer(1, rtc_t::timer_clock::CLK_1_HZ), rtc_t::exception);
        CHECK_THROWS_AS(rtc.set_timer(1.0), rtc_t::exception);
        CHECK_THROWS_AS(rtc.disable_timer(), rtc_t::exception);
        CHECK_THROWS_AS(rtc.set_drift_correction(1000), rtc_t::exception);
        CHECK_THROWS_AS(rtc.reset_drift_correction(), rtc_t::exception);
        mock_i2c_fail_writes(false);
    }
}

TEST_CASE("destructor removes the I2C device")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        // rm_device failure is logged, not thrown (destructor is noexcept).
        mock_i2c_fail_rm_device(true);
    }
    CHECK(mock_i2c_rm_device_count() == 0);
    mock_i2c_fail_rm_device(false);
}

TEST_CASE("move assignment tolerates an rm_device failure")
{
    mock_i2c_reset();
    {
        auto a = make_rtc();
        auto b = make_rtc();
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
        auto a = make_rtc();
        auto b = make_rtc();
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
        rtc_t::datetime t{};
        t.year = 2024;
        t.month = 1;
        t.day = 1;
        set_datetime_regs(t);
        (void) a.get_datetime();
        CHECK(mock_i2c_add_device_count() == 2);
    }
    // a's destructor removes the device that a originally registered.
    CHECK(mock_i2c_rm_device_count() == 2);
}

TEST_CASE("move construction steals the device handle")
{
    mock_i2c_reset();
    {
        auto a = make_rtc();
        auto b = std::move(a);
        // The moved-from object's destructor must not remove the device.
        CHECK(mock_i2c_rm_device_count() == 0);
        rtc_t::datetime t{};
        t.year = 2024;
        t.month = 1;
        t.day = 1;
        set_datetime_regs(t);
        (void) b.get_datetime();
        CHECK(mock_i2c_add_device_count() == 1);
    }
    CHECK(mock_i2c_rm_device_count() == 1);
}

TEST_CASE("a moved-from object is inert")
{
    mock_i2c_reset();
    {
        auto a = make_rtc();
        auto b = std::move(a);
        rtc_t::datetime t{};
        t.year = 2024;
        t.month = 1;
        t.day = 1;
        set_datetime_regs(t);
        // Every method that would touch the bus throws instead of forwarding
        // a null device handle to the I2C driver.
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
        CHECK_THROWS_AS((void) a.get_datetime(), rtc_t::exception);
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
        CHECK_THROWS_AS(a.set_drift_correction(1), rtc_t::exception);
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
        CHECK_THROWS_AS(a.reset(), rtc_t::exception);
        // The moved-to object still works.
        CHECK(b.get_datetime().year == 2024);
    }
}

TEST_CASE("set_datetime writes the year within the supported range")
{
    mock_i2c_reset();
    {
        auto rtc = make_rtc();
        rtc_t::datetime t{};
        t.year = 1970;
        t.month = 1;
        t.day = 1;
        rtc.set_datetime(t);
        CHECK(mock_i2c_wrote(REG_YEARS, 0x00));
        t.year = 2069;
        rtc.set_datetime(t);
        // 2069 - 1970 = 99 -> BCD 0x99.
        CHECK(mock_i2c_wrote(REG_YEARS, 0x99));
    }
}
