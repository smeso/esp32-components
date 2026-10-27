/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <atomic>
#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include <doctest/doctest.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/timers.h>

#include "cpptaskcreate.hpp"
#include "mock_bus.hpp"
#if CONFIG_IDF_TARGET_LINUX
#include "mock_timers.hpp"
#endif
#include "tca9555.hpp"

using namespace mock_bus;

namespace {

// Handles that pass the constructor's null checks but are never dereferenced:
// the I2C driver entry points are redirected to the mocks in mock_bus.cpp
// (components/utils/mocks) by the linker --wrap options in its CMakeLists.txt.
const auto kBus = reinterpret_cast<i2c_master_bus_handle_t>(0x1);

// TCA9555 register addresses (mirrors the private constants in tca9555.hpp).
constexpr uint8_t REG_INPUT_0 = 0x00;
constexpr uint8_t REG_INPUT_1 = 0x01;
constexpr uint8_t REG_OUTPUT_0 = 0x02;
constexpr uint8_t REG_OUTPUT_1 = 0x03;
constexpr uint8_t REG_POLARITY_0 = 0x04;
constexpr uint8_t REG_POLARITY_1 = 0x05;
constexpr uint8_t REG_CONFIG_0 = 0x06;
constexpr uint8_t REG_CONFIG_1 = 0x07;

using tca_t = tca9555;

tca_t
make_tca(uint8_t addr = 0x20)
{
    return tca_t(kBus, addr);
}

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
}

}   // namespace

TEST_CASE("tca9555 constants")
{
    static_assert(tca_t::PINS_PER_PORT == 8);
    static_assert(tca_t::NUM_PINS == 16);
    CHECK(tca_t::PINS_PER_PORT == 8);
    CHECK(tca_t::NUM_PINS == 16);
}

TEST_CASE("tca9555 is movable but not copyable")
{
    static_assert(!std::is_copy_constructible_v<tca_t>);
    static_assert(!std::is_copy_assignable_v<tca_t>);
    static_assert(std::is_move_constructible_v<tca_t>);
    static_assert(std::is_nothrow_move_constructible_v<tca_t>);
    static_assert(std::is_nothrow_move_assignable_v<tca_t>);
}

TEST_CASE("exception derives from std::exception")
{
    static_assert(std::is_base_of_v<std::exception, tca_t::exception>);
}

TEST_CASE("exception includes the message and the error name")
{
    tca_t::exception plain("boom");
    CHECK(std::strcmp(plain.what(), "tca9555: boom") == 0);

    tca_t::exception with_err("i2c failed", ESP_ERR_INVALID_ARG);
    std::string msg(with_err.what());
    CHECK(msg.find("tca9555: i2c failed") == 0);
    CHECK(msg.find("ESP_ERR_INVALID_ARG") != std::string::npos);
}

TEST_CASE("constructor rejects a null I2C bus handle")
{
    CHECK_THROWS_AS(tca_t(nullptr), tca_t::exception);
}

TEST_CASE("constructor rejects invalid I2C addresses")
{
    CHECK_THROWS_AS(make_tca(0x1F), tca_t::exception);
    CHECK_THROWS_AS(make_tca(0x28), tca_t::exception);
}

TEST_CASE("constructor registers the I2C device")
{
    mock_i2c_reset();
    {
        auto dev = make_tca();
        CHECK(mock_i2c_add_device_count() == 1);
        CHECK(mock_i2c_last_device_address() == 0x20);
    }
    CHECK(mock_i2c_rm_device_count() == 1);
}

TEST_CASE("constructor accepts the full address range")
{
    mock_i2c_reset();
    for (uint8_t addr = 0x20; addr <= 0x27; ++addr) {
        auto dev = make_tca(addr);
        CHECK(mock_i2c_last_device_address() == addr);
    }
    CHECK(mock_i2c_add_device_count() == 8);
}

TEST_CASE("constructor reports i2c_master_bus_add_device failures")
{
    mock_i2c_reset();
    mock_i2c_fail_add_device(true);
    CHECK_THROWS_AS(make_tca(), tca_t::exception);
    mock_i2c_fail_add_device(false);
}

TEST_CASE("register helpers handle both ports at runtime")
{
    mock_i2c_reset();
    {
        auto dev = make_tca();
        for (int p = 0; p < 2; ++p) {
            const auto port = static_cast<tca_t::port>(p);
            dev.port_mode(port, 0xFF);
            dev.write_port(port, 0x0F);
            (void) dev.read_port(port);
            dev.set_polarity(port, 0xF0);
            dev.pin_mode(port, 3, true);
            dev.digital_write(port, 4, true);
            dev.set_polarity(port, 5, true);
            (void) dev.digital_read(port, 6);
        }
        CHECK(mock_i2c_wrote(REG_CONFIG_0, 0xFF));
        CHECK(mock_i2c_wrote(REG_CONFIG_1, 0xFF));
        CHECK(mock_i2c_wrote(REG_OUTPUT_0, 0x0F));
        CHECK(mock_i2c_wrote(REG_OUTPUT_1, 0x0F));
        CHECK(mock_i2c_wrote(REG_POLARITY_0, 0xF0));
        CHECK(mock_i2c_wrote(REG_POLARITY_1, 0xF0));
    }
}

TEST_CASE("port_mode writes the config register")
{
    mock_i2c_reset();
    {
        auto dev = make_tca();
        dev.port_mode(tca_t::port::port0, 0x14);
        dev.port_mode(tca_t::port::port1, 0xFE);
        CHECK(mock_i2c_wrote(REG_CONFIG_0, 0x14));
        CHECK(mock_i2c_wrote(REG_CONFIG_1, 0xFE));
    }
}

TEST_CASE("pin_mode updates a single config bit")
{
    mock_i2c_reset();
    {
        auto dev = make_tca();
        // Start with all pins output on port 0.
        mock_i2c_set_reg(REG_CONFIG_0, 0x00);
        dev.pin_mode(tca_t::port::port0, 3, true);
        CHECK(mock_i2c_wrote(REG_CONFIG_0, 0x08));
        mock_i2c_set_reg(REG_CONFIG_0, 0xFF);
        dev.pin_mode(tca_t::port::port0, 5, false);
        CHECK(mock_i2c_wrote(REG_CONFIG_0, 0xDF));
    }
}

TEST_CASE("pin_mode rejects an out-of-range pin")
{
    mock_i2c_reset();
    auto dev = make_tca();
    CHECK_THROWS_AS(dev.pin_mode(tca_t::port::port0, 8, true), tca_t::exception);
    CHECK_THROWS_AS(dev.pin_mode(tca_t::port::port1, 8, true), tca_t::exception);
}

TEST_CASE("digital_write updates a single output bit")
{
    mock_i2c_reset();
    {
        auto dev = make_tca();
        mock_i2c_set_reg(REG_OUTPUT_0, 0x00);
        dev.digital_write(tca_t::port::port0, 3, true);
        CHECK(mock_i2c_wrote(REG_OUTPUT_0, 0x08));
        mock_i2c_set_reg(REG_OUTPUT_0, 0xFF);
        dev.digital_write(tca_t::port::port0, 5, false);
        CHECK(mock_i2c_wrote(REG_OUTPUT_0, 0xDF));
    }
}

TEST_CASE("digital_write rejects an out-of-range pin")
{
    mock_i2c_reset();
    auto dev = make_tca();
    CHECK_THROWS_AS(dev.digital_write(tca_t::port::port0, 8, true), tca_t::exception);
    CHECK_THROWS_AS(dev.digital_write(tca_t::port::port1, 8, true), tca_t::exception);
}

TEST_CASE("write_port writes the whole output port")
{
    mock_i2c_reset();
    {
        auto dev = make_tca();
        dev.write_port(tca_t::port::port0, 0x5A);
        dev.write_port(tca_t::port::port1, 0xA5);
        CHECK(mock_i2c_wrote(REG_OUTPUT_0, 0x5A));
        CHECK(mock_i2c_wrote(REG_OUTPUT_1, 0xA5));
    }
}

TEST_CASE("digital_read reads a single input bit")
{
    mock_i2c_reset();
    {
        auto dev = make_tca();
        mock_i2c_set_reg(REG_INPUT_0, 0x08);
        mock_i2c_set_reg(REG_INPUT_1, 0x20);
        CHECK(dev.digital_read(tca_t::port::port0, 3));
        CHECK(!dev.digital_read(tca_t::port::port0, 2));
        CHECK(dev.digital_read(tca_t::port::port1, 5));
        CHECK(!dev.digital_read(tca_t::port::port1, 4));
    }
}

TEST_CASE("digital_read rejects an out-of-range pin")
{
    mock_i2c_reset();
    auto dev = make_tca();
    CHECK_THROWS_AS((void) dev.digital_read(tca_t::port::port0, 8), tca_t::exception);
    CHECK_THROWS_AS((void) dev.digital_read(tca_t::port::port1, 8), tca_t::exception);
}

TEST_CASE("read_port reads the whole input port")
{
    mock_i2c_reset();
    {
        auto dev = make_tca();
        mock_i2c_set_reg(REG_INPUT_0, 0x5A);
        mock_i2c_set_reg(REG_INPUT_1, 0xA5);
        CHECK(dev.read_port(tca_t::port::port0) == 0x5A);
        CHECK(dev.read_port(tca_t::port::port1) == 0xA5);
    }
}

TEST_CASE("set_polarity writes the polarity registers")
{
    mock_i2c_reset();
    {
        auto dev = make_tca();
        dev.set_polarity(tca_t::port::port0, 0x0F);
        dev.set_polarity(tca_t::port::port1, 0xF0);
        CHECK(mock_i2c_wrote(REG_POLARITY_0, 0x0F));
        CHECK(mock_i2c_wrote(REG_POLARITY_1, 0xF0));
    }
}

TEST_CASE("set_polarity for a single pin")
{
    mock_i2c_reset();
    {
        auto dev = make_tca();
        mock_i2c_set_reg(REG_POLARITY_0, 0x00);
        dev.set_polarity(tca_t::port::port0, 3, true);
        CHECK(mock_i2c_wrote(REG_POLARITY_0, 0x08));
        mock_i2c_set_reg(REG_POLARITY_0, 0xFF);
        dev.set_polarity(tca_t::port::port0, 5, false);
        CHECK(mock_i2c_wrote(REG_POLARITY_0, 0xDF));
    }
}

TEST_CASE("set_polarity rejects an out-of-range pin")
{
    mock_i2c_reset();
    auto dev = make_tca();
    CHECK_THROWS_AS(dev.set_polarity(tca_t::port::port0, 8, true), tca_t::exception);
    CHECK_THROWS_AS(dev.set_polarity(tca_t::port::port1, 8, true), tca_t::exception);
}

TEST_CASE("read and write failures are reported")
{
    mock_i2c_reset();
    {
        auto dev = make_tca();

        mock_i2c_fail_reads(true);
        CHECK_THROWS_AS((void) dev.read_port(tca_t::port::port0), tca_t::exception);
        CHECK_THROWS_AS((void) dev.digital_read(tca_t::port::port1, 0), tca_t::exception);
        mock_i2c_fail_reads(false);

        mock_i2c_fail_writes(true);
        CHECK_THROWS_AS(dev.write_port(tca_t::port::port0, 0xFF), tca_t::exception);
        CHECK_THROWS_AS(dev.port_mode(tca_t::port::port1, 0xFF), tca_t::exception);
        CHECK_THROWS_AS(dev.set_polarity(tca_t::port::port0, 0xFF), tca_t::exception);
        CHECK_THROWS_AS(dev.digital_write(tca_t::port::port0, 0, true), tca_t::exception);
        CHECK_THROWS_AS(dev.pin_mode(tca_t::port::port0, 0, true), tca_t::exception);
        mock_i2c_fail_writes(false);
    }
}

TEST_CASE("destructor removes the I2C device")
{
    mock_i2c_reset();
    {
        auto dev = make_tca();
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
        auto a = make_tca();
        auto b = make_tca();
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
        auto a = make_tca();
        auto b = make_tca();
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
        a.write_port(tca_t::port::port0, 0x33);
        CHECK(mock_i2c_wrote(REG_OUTPUT_0, 0x33));
    }
    // a's destructor removes the device that a originally registered.
    CHECK(mock_i2c_rm_device_count() == 2);
}

TEST_CASE("tca9555 methods reject a moved-from object")
{
    mock_i2c_reset();
    auto a = make_tca();
    auto b = std::move(a);
    // Intentional use of the moved-from object: the I/O helpers must reject it
    // instead of handing a null handle to the I2C driver.
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
    CHECK_THROWS_AS((void) a.read_port(tca_t::port::port0), tca_t::exception);
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
    CHECK_THROWS_AS((void) a.digital_read(tca_t::port::port0, 0), tca_t::exception);
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
    CHECK_THROWS_AS(a.write_port(tca_t::port::port0, 0x00), tca_t::exception);
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
    CHECK_THROWS_AS(a.port_mode(tca_t::port::port0, 0x00), tca_t::exception);
    // The stolen device still works.
    b.write_port(tca_t::port::port0, 0x33);
    CHECK(mock_i2c_wrote(REG_OUTPUT_0, 0x33));
}

TEST_CASE("tca9555_button is neither copyable nor movable")
{
    settle_timers();
    auto dev = std::make_shared<tca_t>(kBus);
    auto buttons = std::make_unique<tca9555_button<tca_t>>(dev);
    static_assert(!std::is_copy_constructible_v<tca9555_button<tca_t>>);
    static_assert(!std::is_copy_assignable_v<tca9555_button<tca_t>>);
    static_assert(!std::is_move_constructible_v<tca9555_button<tca_t>>);
    static_assert(!std::is_move_assignable_v<tca9555_button<tca_t>>);
    buttons->stop();
}

TEST_CASE("tca9555_button exception includes the message")
{
    settle_timers();
    tca9555_button<tca_t>::exception e("boom");
    CHECK(std::strcmp(e.what(), "tca9555_button: boom") == 0);
}

TEST_CASE("tca9555_button start and stop are idempotent")
{
    settle_timers();
    auto dev = std::make_shared<tca_t>(kBus);
    auto buttons = std::make_unique<tca9555_button<tca_t>>(dev);
    // Interrupts and ticks are ignored while stopped.
    buttons->handle_interrupt();
    buttons->start();
    buttons->start();
    buttons->stop();
    buttons->stop();
}

TEST_CASE("tca9555_button callbacks must be registered before start")
{
    settle_timers();
    auto dev = std::make_shared<tca_t>(kBus);
    auto buttons = std::make_unique<tca9555_button<tca_t, 1>>(dev);
    buttons->start();
    CHECK_THROWS_AS(buttons->on_press(tca_t::port::port0, 0, [] {}),
                    tca9555_button<tca_t, 1>::exception);
    CHECK_THROWS_AS(buttons->on_release(tca_t::port::port0, 0, [] {}),
                    tca9555_button<tca_t, 1>::exception);
    CHECK_THROWS_AS(buttons->add_longpress(tca_t::port::port0, 0, 100, [] {}),
                    tca9555_button<tca_t, 1>::exception);
    buttons->stop();
}

TEST_CASE("tca9555_button callbacks reject out-of-range pins")
{
    settle_timers();
    auto dev = std::make_shared<tca_t>(kBus);
    auto buttons = std::make_unique<tca9555_button<tca_t, 1>>(dev);
    CHECK_THROWS_AS(buttons->on_press(tca_t::port::port0, 8, [] {}),
                    tca9555_button<tca_t, 1>::exception);
    CHECK_THROWS_AS(buttons->on_release(tca_t::port::port1, 8, [] {}),
                    tca9555_button<tca_t, 1>::exception);
    CHECK_THROWS_AS(buttons->add_longpress(tca_t::port::port0, 8, 100, [] {}),
                    tca9555_button<tca_t, 1>::exception);
    CHECK_THROWS_AS(buttons->set_active_low(tca_t::port::port0, 8, false),
                    tca9555_button<tca_t, 1>::exception);
    buttons->stop();
}

TEST_CASE("tca9555_button set_active_low must be called before start")
{
    settle_timers();
    auto dev = std::make_shared<tca_t>(kBus);
    auto buttons = std::make_unique<tca9555_button<tca_t>>(dev);
    buttons->start();
    CHECK_THROWS_AS(buttons->set_active_low(tca_t::port::port0, 0, false),
                    tca9555_button<tca_t>::exception);
    buttons->stop();
}

TEST_CASE("tca9555_button active_low is per-pin")
{
    settle_timers();
    mock_i2c_reset();
    auto dev = std::make_shared<tca_t>(kBus);
    auto buttons = std::make_unique<tca9555_button<tca_t>>(dev, true, 10);
    std::vector<bool> pressed_low;
    std::vector<bool> pressed_high;
    // Pin 0 is active-low (constructor default), pin 1 is active-high.
    buttons->set_active_low(tca_t::port::port0, 1, false);
    buttons->on_press(tca_t::port::port0, 0, [&] { pressed_low.push_back(true); });
    buttons->on_press(tca_t::port::port0, 1, [&] { pressed_high.push_back(true); });
    buttons->start();

    // Both idle: pin 0 high, pin 1 low.
    mock_i2c_set_reg(REG_INPUT_0, 0xFD);
    buttons->handle_interrupt();
    vTaskDelay(pdMS_TO_TICKS(40));
    CHECK(pressed_low.empty());
    CHECK(pressed_high.empty());

    // Pin 0 pressed (low), pin 1 still idle (low for active-high).
    mock_i2c_set_reg(REG_INPUT_0, 0xFC);
    buttons->handle_interrupt();
    wait_until([&] { return pressed_low.size() == 1; });
    CHECK(pressed_low.size() == 1);
    CHECK(pressed_high.empty());

    // Pin 0 idle again (high), pin 1 pressed (high).
    mock_i2c_set_reg(REG_INPUT_0, 0xFF);
    buttons->handle_interrupt();
    wait_until([&] { return pressed_high.size() == 1; });
    CHECK(pressed_high.size() == 1);
    CHECK(pressed_low.size() == 1);

    buttons->stop();
}

TEST_CASE("tca9555_button start_tick_timer requires start first")
{
    settle_timers();
    auto dev = std::make_shared<tca_t>(kBus);
    auto buttons = std::make_unique<tca9555_button<tca_t>>(dev);
    CHECK_THROWS_AS(buttons->start_tick_timer(50), tca9555_button<tca_t>::exception);
    buttons->start();
    buttons->start_tick_timer(50);
    // Re-creating the tick timer replaces the old one.
    buttons->start_tick_timer(100);
    buttons->stop();
}

TEST_CASE("tca9555_button tick timer fires and is replaced on restart")
{
    settle_timers();
    auto dev = std::make_shared<tca_t>(kBus);
    auto buttons = std::make_unique<tca9555_button<tca_t>>(dev, true, 10);
    std::vector<bool> pressed;
    buttons->on_press(tca_t::port::port0, 0, [&] { pressed.push_back(true); });
    buttons->start();
    buttons->start_tick_timer(10);
    // Replace the running tick timer with a fresh one.
    buttons->start_tick_timer(10);

    mock_i2c_set_reg(REG_INPUT_0, 0xFF);
    vTaskDelay(pdMS_TO_TICKS(30));
    mock_i2c_set_reg(REG_INPUT_0, 0xFE);
    wait_until([&] { return pressed.size() == 1; });
    CHECK(pressed.size() == 1);

    buttons->stop();
}

TEST_CASE("tca9555_button long-press threshold limit is enforced")
{
    settle_timers();
    auto dev = std::make_shared<tca_t>(kBus);
    auto buttons = std::make_unique<tca9555_button<tca_t, 1>>(dev);
    buttons->add_longpress(tca_t::port::port0, 0, 100, [] {});
    CHECK_THROWS_AS(buttons->add_longpress(tca_t::port::port0, 0, 200, [] {}),
                    tca9555_button<tca_t, 1>::exception);
}

TEST_CASE("tca9555_button handle_interrupt debounces presses and releases")
{
    settle_timers();
    mock_i2c_reset();
    auto dev = std::make_shared<tca_t>(kBus);
    auto buttons = std::make_unique<tca9555_button<tca_t>>(dev, true, 10);
    std::vector<bool> pressed;
    std::vector<bool> released;
    buttons->on_press(tca_t::port::port0, 3, [&] { pressed.push_back(true); });
    buttons->on_release(tca_t::port::port0, 3, [&] { released.push_back(true); });
    buttons->start();
    // Give the FreeRTOS timer task a moment to settle after the timers are
    // created in the constructor.
    vTaskDelay(pdMS_TO_TICKS(20));

    // Idle: all input pins high with active-low buttons.
    mock_i2c_set_reg(REG_INPUT_0, 0xFF);
    mock_i2c_set_reg(REG_INPUT_1, 0xFF);
    buttons->handle_interrupt();
    vTaskDelay(pdMS_TO_TICKS(20));

    // Pin 3 goes low (button pressed): the debounce timer fires and the
    // press callback runs.
    mock_i2c_set_reg(REG_INPUT_0, 0xF7);
    buttons->handle_interrupt();
    wait_until([&] { return pressed.size() == 1; });
    CHECK(pressed.size() == 1);
    CHECK(released.empty());

    // Pin 3 goes high again (button released): the release callback runs.
    mock_i2c_set_reg(REG_INPUT_0, 0xFF);
    buttons->handle_interrupt();
    wait_until([&] { return released.size() == 1; });
    CHECK(pressed.size() == 1);
    CHECK(released.size() == 1);

    buttons->stop();
}

TEST_CASE("tca9555_button debounce_ms zero fires callbacks immediately")
{
    settle_timers();
    mock_i2c_reset();
    auto dev = std::make_shared<tca_t>(kBus);
    auto buttons = std::make_unique<tca9555_button<tca_t>>(dev, true, 0);
    std::vector<bool> pressed;
    std::vector<bool> released;
    buttons->on_press(tca_t::port::port0, 3, [&] { pressed.push_back(true); });
    buttons->on_release(tca_t::port::port0, 3, [&] { released.push_back(true); });
    buttons->start();

    // Idle: all input pins high with active-low buttons.
    mock_i2c_set_reg(REG_INPUT_0, 0xFF);
    buttons->handle_interrupt();
    CHECK(pressed.empty());
    CHECK(released.empty());

    // Pin 3 goes low (button pressed): the press callback fires immediately,
    // without waiting for a debounce timer.
    mock_i2c_set_reg(REG_INPUT_0, 0xF7);
    buttons->handle_interrupt();
    wait_until([&] { return pressed.size() == 1; });
    CHECK(pressed.size() == 1);
    CHECK(released.empty());

    // Pin 3 goes high again (button released): the release callback fires
    // immediately.
    mock_i2c_set_reg(REG_INPUT_0, 0xFF);
    buttons->handle_interrupt();
    wait_until([&] { return released.size() == 1; });
    CHECK(pressed.size() == 1);
    CHECK(released.size() == 1);

    buttons->stop();
}

TEST_CASE("tca9555_button debounce_ms zero fires immediately in tick mode")
{
    settle_timers();
    mock_i2c_reset();
    auto dev = std::make_shared<tca_t>(kBus);
    auto buttons = std::make_unique<tca9555_button<tca_t>>(dev, true, 0);
    std::vector<bool> pressed;
    std::vector<bool> released;
    buttons->on_press(tca_t::port::port1, 1, [&] { pressed.push_back(true); });
    buttons->on_release(tca_t::port::port1, 1, [&] { released.push_back(true); });
    buttons->start();
    buttons->start_tick_timer(10);

    // Let the first tick prime the baseline (all pins idle), then change a
    // pin so the next tick evaluates the change immediately.
    mock_i2c_set_reg(REG_INPUT_1, 0xFF);
    vTaskDelay(pdMS_TO_TICKS(30));
    mock_i2c_set_reg(REG_INPUT_1, 0xFD);
    wait_until([&] { return pressed.size() == 1; });
    CHECK(pressed.size() == 1);
    CHECK(released.empty());

    mock_i2c_set_reg(REG_INPUT_1, 0xFF);
    wait_until([&] { return released.size() == 1; });
    CHECK(released.size() == 1);

    buttons->stop();
}

TEST_CASE("tca9555_button long-press fires after the threshold")
{
    settle_timers();
    mock_i2c_reset();
    auto dev = std::make_shared<tca_t>(kBus);
    auto buttons = std::make_unique<tca9555_button<tca_t, 2>>(dev, true, 10);
    std::vector<bool> long_pressed;
    buttons->on_press(tca_t::port::port0, 0, [] {});
    buttons->add_longpress(tca_t::port::port0, 0, 30, [&] { long_pressed.push_back(true); });
    buttons->add_longpress(tca_t::port::port0, 0, 500, [&] { long_pressed.push_back(true); });
    buttons->start();

    mock_i2c_set_reg(REG_INPUT_0, 0xFE);
    buttons->handle_interrupt();
    vTaskDelay(pdMS_TO_TICKS(20));
    CHECK(long_pressed.empty());
    wait_until([&] { return long_pressed.size() == 1; });
    // Only the short threshold fired; the 500 ms one is still pending.
    CHECK(long_pressed.size() == 1);

    // Releasing the pin stops the long-press machinery.
    mock_i2c_set_reg(REG_INPUT_0, 0xFF);
    buttons->handle_interrupt();
    vTaskDelay(pdMS_TO_TICKS(40));
    CHECK(long_pressed.size() == 1);

    buttons->stop();
}

TEST_CASE("tca9555_button tick polls the input pins")
{
    settle_timers();
    mock_i2c_reset();
    auto dev = std::make_shared<tca_t>(kBus);
    auto buttons = std::make_unique<tca9555_button<tca_t>>(dev, true, 10);
    std::vector<bool> pressed;
    buttons->on_press(tca_t::port::port1, 1, [&] { pressed.push_back(true); });
    buttons->start();
    buttons->start_tick_timer(10);

    // Let the first tick prime the baseline (all pins idle), then change a
    // pin so the next tick detects the change and schedules the debounce.
    mock_i2c_set_reg(REG_INPUT_1, 0xFF);
    vTaskDelay(pdMS_TO_TICKS(30));
    mock_i2c_set_reg(REG_INPUT_1, 0xFD);
    wait_until([&] { return pressed.size() == 1; });
    CHECK(pressed.size() == 1);

    buttons->stop();
}

TEST_CASE("tca9555_button tick ignores pins without callbacks")
{
    settle_timers();
    mock_i2c_reset();
    auto dev = std::make_shared<tca_t>(kBus);
    auto buttons = std::make_unique<tca9555_button<tca_t>>(dev, true, 10);
    std::vector<bool> pressed;
    buttons->on_press(tca_t::port::port1, 1, [&] { pressed.push_back(true); });
    buttons->start();
    buttons->start_tick_timer(10);

    // Let the first tick prime the baseline (all pins idle), then toggle only
    // the unconfigured pin 0 of port 1: tick must not react to it.
    mock_i2c_set_reg(REG_INPUT_1, 0xFF);
    vTaskDelay(pdMS_TO_TICKS(30));
    mock_i2c_set_reg(REG_INPUT_1, 0xFE);
    vTaskDelay(pdMS_TO_TICKS(40));
    CHECK(pressed.empty());

    // The configured pin 1 now changes: the press is detected.
    mock_i2c_set_reg(REG_INPUT_1, 0xFD);
    wait_until([&] { return pressed.size() == 1; });
    CHECK(pressed.size() == 1);

    buttons->stop();
}

TEST_CASE("tca9555_button tick with no configured pins does nothing")
{
    settle_timers();
    mock_i2c_reset();
    auto dev = std::make_shared<tca_t>(kBus);
    auto buttons = std::make_unique<tca9555_button<tca_t>>(dev, true, 10);
    buttons->start();
    buttons->start_tick_timer(10);

    // No callbacks at all: tick must not even react to pin changes.
    mock_i2c_set_reg(REG_INPUT_0, 0xFE);
    vTaskDelay(pdMS_TO_TICKS(40));

    buttons->stop();
}

TEST_CASE("tca9555_button handle_interrupt monitors all 16 pins")
{
    settle_timers();
    mock_i2c_reset();
    auto dev = std::make_shared<tca_t>(kBus);
    auto buttons = std::make_unique<tca9555_button<tca_t>>(dev, true, 10);
    std::vector<bool> pressed0;
    std::vector<bool> pressed1;
    buttons->on_press(tca_t::port::port0, 7, [&] { pressed0.push_back(true); });
    buttons->on_press(tca_t::port::port1, 0, [&] { pressed1.push_back(true); });
    buttons->start();

    // One press on each port: the per-pin loop sees a state change on both.
    mock_i2c_set_reg(REG_INPUT_0, 0x7F);
    mock_i2c_set_reg(REG_INPUT_1, 0xFE);
    buttons->handle_interrupt();
    wait_until([&] { return pressed0.size() == 1 && pressed1.size() == 1; });
    CHECK(pressed0.size() == 1);
    CHECK(pressed1.size() == 1);

    // No further state changes: no new presses.
    buttons->handle_interrupt();
    vTaskDelay(pdMS_TO_TICKS(40));
    CHECK(pressed0.size() == 1);
    CHECK(pressed1.size() == 1);

    buttons->stop();
}

TEST_CASE("tca9555_button callbacks can be posted to a task_queue")
{
    settle_timers();
    mock_i2c_reset();
    auto dev = std::make_shared<tca_t>(kBus);
    auto queue =
            std::make_shared<task_queue<tca9555_button<tca_t>::callback_t>>("tca_test", 8);
    auto buttons = std::make_unique<tca9555_button<tca_t>>(dev, true, 10, queue);
    std::vector<bool> pressed;
    buttons->on_press(tca_t::port::port0, 0, [&] { pressed.push_back(true); });
    buttons->start();

    mock_i2c_set_reg(REG_INPUT_0, 0xFE);
    buttons->handle_interrupt();
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
    while (pressed.empty() && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    CHECK(pressed.size() == 1);

    buttons->stop();
}

TEST_CASE("tca9555_button destructor retries the drain while the queue is full")
{
    settle_timers();
    auto dev = std::make_shared<tca_t>(kBus);
    std::atomic<size_t> processed{ 0 };
    std::atomic<bool> entered{ false };
    SemaphoreHandle_t gate = xSemaphoreCreateBinary();
    REQUIRE(gate != nullptr);
    {
        // A queue with room for two items (its pool holds three). The first
        // item parks the worker and the next two fill the pool, so the drain
        // sentinel cannot be posted: the destructor must retry until the
        // worker has consumed enough items to make room again.
        auto queue =
                std::make_shared<task_queue<tca9555_button<tca_t>::callback_t>>("tca_full", 2);
        REQUIRE(queue->post([&] {
            entered.store(true);
            xSemaphoreTake(gate, portMAX_DELAY);
            processed.fetch_add(1);
        }));
        wait_until([&] { return entered.load(); });
        auto buttons = std::make_unique<tca9555_button<tca_t>>(dev, true, 10, queue);
        REQUIRE(queue->post([&] { processed.fetch_add(1); }));
        REQUIRE(queue->post([&] { processed.fetch_add(1); }));
        REQUIRE_FALSE(queue->post([] {}));

        // Unblock the worker from a helper task, so the destructor's first
        // drain attempt is guaranteed to find the pool still full.
        const auto created =
                cppTaskCreate("tca_gate", 2048, uxTaskPriorityGet(nullptr), nullptr, [gate] {
                    vTaskDelay(pdMS_TO_TICKS(50));
                    xSemaphoreGive(gate);
                    vTaskSuspend(nullptr);
                });
        REQUIRE(created == pdPASS);

        buttons.reset();
        CHECK(processed.load() == 3);
    }
    vSemaphoreDelete(gate);
}

TEST_CASE("tca9555_button empty callbacks are tolerated")
{
    settle_timers();
    mock_i2c_reset();
    auto dev = std::make_shared<tca_t>(kBus);
    auto buttons = std::make_unique<tca9555_button<tca_t>>(dev, true, 10);
    // A pin with only a release callback still debounces: the empty press
    // callback must be tolerated by run().
    std::vector<bool> released;
    buttons->on_release(tca_t::port::port0, 0, [&] { released.push_back(true); });
    buttons->start();

    mock_i2c_set_reg(REG_INPUT_0, 0xFE);
    buttons->handle_interrupt();
    vTaskDelay(pdMS_TO_TICKS(40));
    CHECK(released.empty());

    mock_i2c_set_reg(REG_INPUT_0, 0xFF);
    buttons->handle_interrupt();
    wait_until([&] { return released.size() == 1; });
    CHECK(released.size() == 1);

    buttons->stop();
}

TEST_CASE("tca9555_button pins without callbacks are ignored")
{
    settle_timers();
    mock_i2c_reset();
    auto dev = std::make_shared<tca_t>(kBus);
    auto buttons = std::make_unique<tca9555_button<tca_t>>(dev, true, 10);
    std::vector<bool> pressed;
    buttons->on_press(tca_t::port::port0, 1, [&] { pressed.push_back(true); });
    buttons->start();

    // Pin 0 has no callbacks: toggling it must not produce any event and must
    // not disturb the configured pin 1.
    mock_i2c_set_reg(REG_INPUT_0, 0xFE);
    buttons->handle_interrupt();
    vTaskDelay(pdMS_TO_TICKS(40));
    CHECK(pressed.empty());

    // The configured pin 1 still works.
    mock_i2c_set_reg(REG_INPUT_0, 0xFC);
    buttons->handle_interrupt();
    wait_until([&] { return pressed.size() == 1; });
    CHECK(pressed.size() == 1);

    buttons->stop();
}

TEST_CASE("tca9555_button ignores touched but unconfigured pins")
{
    settle_timers();
    mock_i2c_reset();
    auto dev = std::make_shared<tca_t>(kBus);
    auto buttons = std::make_unique<tca9555_button<tca_t>>(dev, true, 10);
    std::vector<bool> pressed;
    // Pin 0 is touched (so it exists in the pin map) but has no callbacks;
    // pin 1 is the configured one.
    buttons->set_active_low(tca_t::port::port0, 0, true);
    buttons->on_press(tca_t::port::port0, 1, [&] { pressed.push_back(true); });
    buttons->start();

    // Toggling the unconfigured pin 0 must be ignored...
    mock_i2c_set_reg(REG_INPUT_0, 0xFE);
    buttons->handle_interrupt();
    vTaskDelay(pdMS_TO_TICKS(40));
    CHECK(pressed.empty());

    // ...while the configured pin 1 still fires.
    mock_i2c_set_reg(REG_INPUT_0, 0xFC);
    buttons->handle_interrupt();
    wait_until([&] { return pressed.size() == 1; });
    CHECK(pressed.size() == 1);

    buttons->stop();
}

TEST_CASE("tca9555_button pins with all callbacks removed are ignored")
{
    settle_timers();
    mock_i2c_reset();
    auto dev = std::make_shared<tca_t>(kBus);
    auto buttons = std::make_unique<tca9555_button<tca_t>>(dev, true, 10);
    std::vector<bool> pressed;
    buttons->on_press(tca_t::port::port0, 2, [&] { pressed.push_back(true); });
    // Overwriting the only callback with an empty one unconfigures the pin.
    buttons->on_press(tca_t::port::port0, 2, {});
    buttons->start();

    mock_i2c_set_reg(REG_INPUT_0, 0xFB);
    buttons->handle_interrupt();
    vTaskDelay(pdMS_TO_TICKS(40));
    CHECK(pressed.empty());

    buttons->stop();
}

TEST_CASE("tca9555_button pins with only an empty long-press are ignored")
{
    settle_timers();
    mock_i2c_reset();
    auto dev = std::make_shared<tca_t>(kBus);
    auto buttons = std::make_unique<tca9555_button<tca_t, 1>>(dev, true, 10);
    buttons->add_longpress(tca_t::port::port0, 3, 20, {});
    buttons->start();

    mock_i2c_set_reg(REG_INPUT_0, 0xF7);
    buttons->handle_interrupt();
    vTaskDelay(pdMS_TO_TICKS(40));

    buttons->stop();
}

TEST_CASE("tca9555_button later long-press thresholds also fire")
{
    settle_timers();
    mock_i2c_reset();
    auto dev = std::make_shared<tca_t>(kBus);
    auto buttons = std::make_unique<tca9555_button<tca_t, 2>>(dev, true, 10);
    std::vector<bool> fired;
    buttons->on_press(tca_t::port::port0, 0, [] {});
    // Register the slow threshold first so the fast one fires through the
    // second slot: the callback loop must skip the non-matching timer.
    buttons->add_longpress(tca_t::port::port0, 0, 500, [&] { fired.push_back(true); });
    buttons->add_longpress(tca_t::port::port0, 0, 30, [&] { fired.push_back(true); });
    buttons->start();

    mock_i2c_set_reg(REG_INPUT_0, 0xFE);
    buttons->handle_interrupt();
    wait_until([&] { return fired.size() == 2; });
    CHECK(fired.size() == 2);

    mock_i2c_set_reg(REG_INPUT_0, 0xFF);
    buttons->handle_interrupt();
    vTaskDelay(pdMS_TO_TICKS(40));
    buttons->stop();
}

TEST_CASE("tca9555_button empty long-press callbacks are tolerated")
{
    settle_timers();
    mock_i2c_reset();
    auto dev = std::make_shared<tca_t>(kBus);
    auto buttons = std::make_unique<tca9555_button<tca_t, 1>>(dev, true, 10);
    // An empty callback still creates the long-press timer; when it fires the
    // callback loop finds the empty slot and simply exits without running it.
    buttons->on_press(tca_t::port::port0, 0, [] {});
    buttons->add_longpress(tca_t::port::port0, 0, 20, {});
    buttons->start();

    mock_i2c_set_reg(REG_INPUT_0, 0xFE);
    buttons->handle_interrupt();
    vTaskDelay(pdMS_TO_TICKS(100));

    mock_i2c_set_reg(REG_INPUT_0, 0xFF);
    buttons->handle_interrupt();
    vTaskDelay(pdMS_TO_TICKS(40));
    buttons->stop();
}

TEST_CASE("tca9555_button gives up when the task queue never drains")
{
    settle_timers();
    mock_i2c_reset();
    auto dev = std::make_shared<tca_t>(kBus);
    SemaphoreHandle_t gate = xSemaphoreCreateBinary();
    REQUIRE(gate != nullptr);
    {
        // The pool of this queue stays exhausted for the whole retry window
        // (the worker is parked inside the first item and the second one is
        // queued behind it), so the sentinel of drain() can never be posted.
        auto queue = std::make_shared<task_queue<tca9555_button<tca_t>::callback_t>>(
                "tca_stuck", 1);
        std::atomic<bool> parked{ false };
        REQUIRE(queue->post([&] {
            parked.store(true);
            xSemaphoreTake(gate, portMAX_DELAY);
        }));
        wait_until([&] { return parked.load(); });
        REQUIRE(queue->post([] {}));

        auto buttons = std::make_unique<tca9555_button<tca_t>>(dev, true, 10, queue);
        // The destructor must stop retrying instead of spinning forever. The
        // queued items do not capture the button, so running them after its
        // destruction is harmless.
        buttons.reset();
        xSemaphoreGive(gate);
    }
    vSemaphoreDelete(gate);
}

TEST_CASE("tca9555_button rejects a null device")
{
    settle_timers();
    CHECK_THROWS_AS((tca9555_button<tca_t>(std::shared_ptr<tca_t>())),
                    tca9555_button<tca_t>::exception);
}

TEST_CASE("tca9555_button rejects a timer period below the tick resolution")
{
    settle_timers();
    auto dev = std::make_shared<tca_t>(kBus);
    auto buttons = std::make_unique<tca9555_button<tca_t, 1>>(dev);
    CHECK_THROWS_AS(buttons->add_longpress(tca_t::port::port0, 0, 0, [] {}),
                    tca9555_button<tca_t, 1>::exception);
    buttons->start();
    CHECK_THROWS_AS(buttons->start_tick_timer(0), tca9555_button<tca_t, 1>::exception);
    // The rejected periods left no timer behind: a valid one still works.
    buttons->start_tick_timer(10);
    buttons->stop();
}

TEST_CASE("tca9555_button drops a press that bounces back before the debounce expires")
{
    settle_timers();
    mock_i2c_reset();
    auto dev = std::make_shared<tca_t>(kBus);
    auto buttons = std::make_unique<tca9555_button<tca_t>>(dev, true, 30);
    std::vector<bool> pressed;
    std::vector<bool> released;
    buttons->on_press(tca_t::port::port0, 0, [&] { pressed.push_back(true); });
    buttons->on_release(tca_t::port::port0, 0, [&] { released.push_back(true); });
    buttons->start();

    mock_i2c_set_reg(REG_INPUT_0, 0xFF);
    buttons->handle_interrupt();

    // The pin goes low and back high inside the debounce window. The return
    // edge does not re-arm the timer, so the timer still fires, but the level
    // it reads then is the released one: no press (and no later release) may
    // be reported.
    mock_i2c_set_reg(REG_INPUT_0, 0xFE);
    buttons->handle_interrupt();
    mock_i2c_set_reg(REG_INPUT_0, 0xFF);
    buttons->handle_interrupt();
    vTaskDelay(pdMS_TO_TICKS(60));
    CHECK(pressed.empty());
    CHECK(released.empty());

    // The pin state was not disturbed: a real press is still reported.
    mock_i2c_set_reg(REG_INPUT_0, 0xFE);
    buttons->handle_interrupt();
    wait_until([&] { return pressed.size() == 1; });
    CHECK(pressed.size() == 1);
    CHECK(released.empty());

    mock_i2c_set_reg(REG_INPUT_0, 0xFF);
    buttons->handle_interrupt();
    wait_until([&] { return released.size() == 1; });
    CHECK(released.size() == 1);

    buttons->stop();
}

TEST_CASE("tca9555_button contains an I2C failure in the debounce evaluation")
{
    settle_timers();
    mock_i2c_reset();
    auto dev = std::make_shared<tca_t>(kBus);
    auto buttons = std::make_unique<tca9555_button<tca_t>>(dev, true, 10);
    std::vector<bool> pressed;
    buttons->on_press(tca_t::port::port0, 0, [&] { pressed.push_back(true); });
    buttons->start();

    mock_i2c_set_reg(REG_INPUT_0, 0xFF);
    buttons->handle_interrupt();

    // Arm the debounce timer, then fail the read its callback performs: the
    // failure is contained, the event is dropped and the pin state is left
    // untouched.
    mock_i2c_set_reg(REG_INPUT_0, 0xFE);
    buttons->handle_interrupt();
    mock_i2c_fail_reads(true);
    vTaskDelay(pdMS_TO_TICKS(40));
    mock_i2c_fail_reads(false);
    CHECK(pressed.empty());

    // A later edge evaluates the pin again and the press is delivered.
    mock_i2c_set_reg(REG_INPUT_0, 0xFF);
    buttons->handle_interrupt();
    mock_i2c_set_reg(REG_INPUT_0, 0xFE);
    buttons->handle_interrupt();
    wait_until([&] { return pressed.size() == 1; });
    CHECK(pressed.size() == 1);

    buttons->stop();
}

TEST_CASE("tca9555_button reports events dropped on a full task queue")
{
    settle_timers();
    mock_i2c_reset();
    auto dev = std::make_shared<tca_t>(kBus);
    // A queue with room for one item (its pool holds two): parking the worker
    // and filling the remaining slot leaves no room for the callback posted by
    // handle_interrupt(), which must be dropped (and reported) instead of
    // being delivered.
    auto queue =
            std::make_shared<task_queue<tca9555_button<tca_t>::callback_t>>("tca_drop", 1);
    std::atomic<bool> parked{ false };
    std::atomic<int> calls{ 0 };
    REQUIRE(queue->post([&] {
        parked.store(true);
        vTaskDelay(pdMS_TO_TICKS(150));
    }));
    wait_until([&] { return parked.load(); });
    REQUIRE(queue->post([] {}));

    auto buttons = std::make_unique<tca9555_button<tca_t>>(dev, true, 0, queue);
    buttons->on_press(tca_t::port::port0, 0, [&] { calls.fetch_add(1); });
    buttons->start();

    mock_i2c_set_reg(REG_INPUT_0, 0xFE);
    buttons->handle_interrupt();
    CHECK(calls.load() == 0);
    buttons->stop();
    CHECK(calls.load() == 0);
}

TEST_CASE("tca9555_button reports poll ticks dropped on a full task queue")
{
    settle_timers();
    mock_i2c_reset();
    auto dev = std::make_shared<tca_t>(kBus);
    auto queue = std::make_shared<task_queue<tca9555_button<tca_t>::callback_t>>(
            "tca_tick_drop", 1);
    std::atomic<bool> parked{ false };
    REQUIRE(queue->post([&] {
        parked.store(true);
        vTaskDelay(pdMS_TO_TICKS(150));
    }));
    wait_until([&] { return parked.load(); });
    REQUIRE(queue->post([] {}));

    auto buttons = std::make_unique<tca9555_button<tca_t>>(dev, true, 0, queue);
    buttons->on_press(tca_t::port::port0, 0, [] {});
    buttons->start();
    buttons->start_tick_timer(10);
    // Every tick in the window finds the queue full: the tick is dropped and
    // reported instead of being delivered.
    vTaskDelay(pdMS_TO_TICKS(50));
    buttons->stop();
}

TEST_CASE("tca9555_button stop drains the callbacks that are still queued")
{
    settle_timers();
    mock_i2c_reset();
    auto dev = std::make_shared<tca_t>(kBus);
    auto queue =
            std::make_shared<task_queue<tca9555_button<tca_t>::callback_t>>("tca_stop", 4);
    auto buttons = std::make_unique<tca9555_button<tca_t>>(dev, true, 0, queue);
    buttons->start();

    // The worker is parked inside the first item and the second one is queued
    // behind it: stop() must not return before that item has run.
    std::atomic<bool> parked{ false };
    std::atomic<bool> ran{ false };
    REQUIRE(queue->post([&] {
        parked.store(true);
        vTaskDelay(pdMS_TO_TICKS(150));
    }));
    wait_until([&] { return parked.load(); });
    REQUIRE(queue->post([&] { ran.store(true); }));

    buttons->stop();
    CHECK(ran.load());
}

TEST_CASE("tca9555_button polling re-primes its baseline after a stop")
{
    settle_timers();
    mock_i2c_reset();
    auto dev = std::make_shared<tca_t>(kBus);
    auto buttons = std::make_unique<tca9555_button<tca_t>>(dev, true, 10);
    std::vector<bool> pressed;
    buttons->on_press(tca_t::port::port0, 0, [&] { pressed.push_back(true); });
    buttons->start();
    buttons->start_tick_timer(10);

    // Prime the baseline with the released level, then press the pin.
    mock_i2c_set_reg(REG_INPUT_0, 0xFF);
    vTaskDelay(pdMS_TO_TICKS(30));
    mock_i2c_set_reg(REG_INPUT_0, 0xFE);
    wait_until([&] { return pressed.size() == 1; });
    REQUIRE(pressed.size() == 1);

    // Stop while the pin is pressed and release it while stopped: the restart
    // re-arms the tick timer and re-primes the baseline with the released
    // level, so the next press is reported again.
    buttons->stop();
    mock_i2c_set_reg(REG_INPUT_0, 0xFF);
    buttons->start();
    vTaskDelay(pdMS_TO_TICKS(40));
    CHECK(pressed.size() == 1);

    mock_i2c_set_reg(REG_INPUT_0, 0xFE);
    wait_until([&] { return pressed.size() == 2; });
    CHECK(pressed.size() == 2);

    buttons->stop();
}

#if CONFIG_IDF_TARGET_LINUX

TEST_CASE("tca9555_button debounce_ms zero creates no debounce timer")
{
    settle_timers();
    mock_timers_reset();
    auto dev = std::make_shared<tca_t>(kBus);
    auto buttons = std::make_unique<tca9555_button<tca_t>>(dev, true, 0);
    // With debouncing disabled no debounce timer is created, so a failing
    // xTimerCreate must not prevent callbacks from being registered.
    mock_timers_fail_x_timer_create_after(0);
    buttons->on_press(tca_t::port::port0, 0, [] {});
    mock_timers_reset();
    buttons->stop();
}

TEST_CASE("tca9555_button reports xTimerCreate debounce failures")
{
    settle_timers();
    mock_timers_reset();
    auto dev = std::make_shared<tca_t>(kBus);
    auto buttons = std::make_unique<tca9555_button<tca_t>>(dev);
    // Debounce timers are created lazily when a pin is first used: let the
    // first pin's timer succeed, then fail the second one so on_press throws.
    mock_timers_fail_x_timer_create_after(1);
    buttons->on_press(tca_t::port::port0, 0, [] {});
    CHECK_THROWS_AS(buttons->on_press(tca_t::port::port0, 1, [] {}),
                    tca9555_button<tca_t>::exception);
    mock_timers_reset();
    buttons->stop();
}

TEST_CASE("tca9555_button set_active_low creates no timer")
{
    settle_timers();
    mock_timers_reset();
    auto dev = std::make_shared<tca_t>(kBus);
    auto buttons = std::make_unique<tca9555_button<tca_t>>(dev, true, 10);
    // A pin that only gets its active level set is never monitored: no timer
    // may be created for it (the drain timer was created by the constructor).
    mock_timers_fail_x_timer_create_after(0);
    CHECK_NOTHROW(buttons->set_active_low(tca_t::port::port0, 0, false));
    mock_timers_reset();
    buttons->stop();
}

TEST_CASE("tca9555_button destructor deletes every timer it created")
{
    settle_timers();
    mock_timers_reset();
    auto dev = std::make_shared<tca_t>(kBus);
    {
        auto buttons = std::make_unique<tca9555_button<tca_t, 1>>(dev, true, 10);
        buttons->on_press(tca_t::port::port0, 0, [] {});
        buttons->add_longpress(tca_t::port::port0, 0, 20, [] {});
        buttons->start();
        buttons->start_tick_timer(10);
        CHECK(mock_timers_delete_count() == 0);
    }
    // The drain, debounce, long-press and tick timers must all be deleted with
    // a blocking command, not dropped when the command queue is full.
    CHECK(mock_timers_delete_count() == 4);
}

TEST_CASE("tca9555_button start_tick_timer reports xTimerCreate failures")
{
    settle_timers();
    mock_timers_reset();
    auto dev = std::make_shared<tca_t>(kBus);
    auto buttons = std::make_unique<tca9555_button<tca_t>>(dev);
    buttons->start();
    mock_timers_fail_x_timer_create_after(0);
    CHECK_THROWS_AS(buttons->start_tick_timer(50), tca9555_button<tca_t>::exception);
    mock_timers_reset();
    buttons->stop();
}

TEST_CASE("tca9555_button start_tick_timer reports xTimerStart failures")
{
    settle_timers();
    mock_timers_reset();
    auto dev = std::make_shared<tca_t>(kBus);
    auto buttons = std::make_unique<tca9555_button<tca_t>>(dev);
    buttons->start();
    // The timer is created, but starting it fails: the constructor-like
    // cleanup in start_tick_timer() deletes the timer before throwing.
    mock_timers_fail_x_timer_start(true);
    CHECK_THROWS_AS(buttons->start_tick_timer(50), tca9555_button<tca_t>::exception);
    mock_timers_fail_x_timer_start(false);
    buttons->stop();
}

TEST_CASE("tca9555_button add_longpress reports xTimerCreate failures")
{
    settle_timers();
    mock_timers_reset();
    auto dev = std::make_shared<tca_t>(kBus);
    auto buttons = std::make_unique<tca9555_button<tca_t, 1>>(dev);
    // The pin's lazily-created debounce timer is the first xTimerCreate let
    // through; the long-press timer (the second one) fails.
    mock_timers_fail_x_timer_create_after(1);
    CHECK_THROWS_AS(buttons->add_longpress(tca_t::port::port0, 0, 100, [] {}),
                    tca9555_button<tca_t, 1>::exception);
    mock_timers_reset();
    buttons->stop();
}

#endif   // CONFIG_IDF_TARGET_LINUX
