/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

#include <doctest/doctest.h>
#include "mock_heap.hpp"
#include "mock_rmt.hpp"
#include "spiram_allocation.hpp"
#include "ws2812.hpp"

using namespace mock_bus;

namespace {

using strip_t = ws2812<3>;
constexpr gpio_num_t kPin = GPIO_NUM_2;

}   // namespace

TEST_CASE("template parameters are reflected in the driver")
{
    CHECK(ws2812<3>::num_leds() == 3);
    CHECK(ws2812<7>::num_leds() == 7);
    CHECK(strip_t::num_leds() == 3);
}

TEST_CASE("the pixel buffer is not part of the object")
{
    // 3 bytes per LED: keeping the buffer in the object would make it 900
    // bytes for a 300-LED strip, so a strip of that length allocated as a
    // local would need a task stack large enough to hold it. The buffer is
    // heap-allocated instead, which keeps the object size independent of
    // NumLeds.
    static_assert(sizeof(ws2812<3>) == sizeof(ws2812<300>));
}

TEST_CASE("a custom allocator places the pixel buffer where it asks")
{
    mock_rmt_reset();
    {
        // SpiramAllocator keeps the buffer out of internal RAM: long strips
        // (3 bytes per LED) no longer have to fit in the scarce internal heap.
        using spiram_strip_t = ws2812<3, order_rgb, SpiramAllocator<uint8_t>>;
        spiram_strip_t strip(kPin);
        strip.set_all_leds({ 0x11, 0x22, 0x33 });
        CHECK(mock_rmt_last_payload() ==
              std::vector<uint8_t>{ 0x11, 0x22, 0x33, 0x11, 0x22, 0x33, 0x11, 0x22, 0x33 });
    }
}

TEST_CASE("the pixel buffer is allocated through the provided allocator")
{
    mock_rmt_reset();
    using spiram_strip_t = ws2812<3, order_rgb, SpiramAllocator<uint8_t>>;
    // Failing the underlying heap allocation proves the constructor goes
    // through the caller-provided allocator (SpiramAllocator, which throws
    // std::bad_alloc) instead of plain new[].
    mock_heap::fail_allocations(true);
    CHECK_THROWS_AS(spiram_strip_t{ kPin }, std::bad_alloc);
    mock_heap::fail_allocations(false);
    CHECK_NOTHROW(spiram_strip_t{ kPin });
}

TEST_CASE("ws2812 is movable but not copyable")
{
    static_assert(!std::is_copy_constructible_v<strip_t>);
    static_assert(!std::is_copy_assignable_v<strip_t>);
    static_assert(std::is_move_constructible_v<strip_t>);
    static_assert(std::is_nothrow_move_constructible_v<strip_t>);
    static_assert(std::is_nothrow_move_assignable_v<strip_t>);
}

TEST_CASE("valid color orders instantiate")
{
    CHECK(ws2812<1, order_rgb>::num_leds() == 1);
    CHECK(ws2812<1, order_grb>::num_leds() == 1);
}

TEST_CASE("exception derives from std::exception")
{
    static_assert(std::is_base_of_v<std::exception, ws2812<1>::exception>);
}

TEST_CASE("exception includes the message and the error name")
{
    ws2812<1>::exception plain("boom");
    CHECK(std::strcmp(plain.what(), "ws2812: boom") == 0);

    ws2812<1>::exception with_err("rmt failed", ESP_ERR_INVALID_ARG);
    std::string msg(with_err.what());
    CHECK(msg.find("ws2812: rmt failed") == 0);
    CHECK(msg.find("ESP_ERR_INVALID_ARG") != std::string::npos);
}

TEST_CASE("constructor reports rmt_new_tx_channel failures")
{
    mock_rmt_reset();
    mock_rmt_fail_new_tx_channel(true);
    CHECK_THROWS_AS(strip_t strip(kPin), strip_t::exception);
    mock_rmt_fail_new_tx_channel(false);
}

TEST_CASE("constructor reports rmt_new_bytes_encoder failures")
{
    mock_rmt_reset();
    mock_rmt_fail_new_bytes_encoder(true);
    CHECK_THROWS_AS(strip_t strip(kPin), strip_t::exception);
    mock_rmt_fail_new_bytes_encoder(false);
}

TEST_CASE("constructor reports rmt_enable failures")
{
    mock_rmt_reset();
    mock_rmt_fail_enable(true);
    CHECK_THROWS_AS(strip_t strip(kPin), strip_t::exception);
    mock_rmt_fail_enable(false);
}

TEST_CASE("constructor error paths log teardown failures")
{
    mock_rmt_reset();
    mock_rmt_fail_new_bytes_encoder(true);
    mock_rmt_fail_del_channel(true);
    CHECK_THROWS_AS(strip_t strip(kPin), strip_t::exception);
    mock_rmt_fail_new_bytes_encoder(false);
    mock_rmt_fail_del_channel(false);

    mock_rmt_fail_enable(true);
    mock_rmt_fail_del_encoder(true);
    CHECK_THROWS_AS(strip_t strip(kPin), strip_t::exception);
    mock_rmt_fail_del_encoder(false);
    mock_rmt_fail_del_channel(true);
    CHECK_THROWS_AS(strip_t strip(kPin), strip_t::exception);
    mock_rmt_fail_enable(false);
    mock_rmt_fail_del_channel(false);
}

TEST_CASE("set_all_leds transmits the RGB bytes for every LED")
{
    mock_rmt_reset();
    {
        strip_t strip(kPin);
        strip.set_all_leds({ 0x11, 0x22, 0x33 });
        CHECK(mock_rmt_last_payload_size() == 3 * 3);
        const std::vector<uint8_t> expected{
            0x11, 0x22, 0x33, 0x11, 0x22, 0x33, 0x11, 0x22, 0x33,
        };
        CHECK(mock_rmt_last_payload() == expected);
    }
}

TEST_CASE("set_leds transmits the provided colors")
{
    mock_rmt_reset();
    {
        strip_t strip(kPin);
        std::array<strip_t::RGB, 3> colors{
            strip_t::RGB{ 1, 2, 3 },
            strip_t::RGB{ 4, 5, 6 },
            strip_t::RGB{ 7, 8, 9 },
        };
        strip.set_leds(colors);
        CHECK(mock_rmt_last_payload() == std::vector<uint8_t>{ 1, 2, 3, 4, 5, 6, 7, 8, 9 });
    }
}

TEST_CASE("set_leds accepts a heap-allocated container")
{
    // A std::vector keeps the colors off the stack, so a stream too long for a
    // stack-allocated std::array can still be driven.
    mock_rmt_reset();
    {
        strip_t strip(kPin);
        std::vector<strip_t::RGB> colors{
            strip_t::RGB{ 1, 2, 3 },
            strip_t::RGB{ 4, 5, 6 },
            strip_t::RGB{ 7, 8, 9 },
        };
        strip.set_leds(colors);
        CHECK(mock_rmt_last_payload() == std::vector<uint8_t>{ 1, 2, 3, 4, 5, 6, 7, 8, 9 });
    }
}

TEST_CASE("set_leds rejects a runtime-sized container of the wrong size")
{
    mock_rmt_reset();
    {
        strip_t strip(kPin);
        std::vector<strip_t::RGB> too_few(2);
        CHECK_THROWS_AS(strip.set_leds(too_few), strip_t::exception);
        std::vector<strip_t::RGB> too_many(4);
        CHECK_THROWS_AS(strip.set_leds(too_many), strip_t::exception);
    }
}

TEST_CASE("order_rgb set_leds transmits directly from the caller's container")
{
    // With order_rgb the on-wire byte order matches the in-memory RGB layout,
    // so set_leds() hands the caller's storage to the RMT driver instead of
    // staging a copy in the pixel buffer.
    mock_rmt_reset();
    {
        strip_t strip(kPin);
        std::array<strip_t::RGB, 3> colors{
            strip_t::RGB{ 1, 2, 3 },
            strip_t::RGB{ 4, 5, 6 },
            strip_t::RGB{ 7, 8, 9 },
        };
        strip.set_leds(colors);
        CHECK(mock_rmt_last_payload_ptr() == colors.data());
        CHECK(mock_rmt_last_payload() == std::vector<uint8_t>{ 1, 2, 3, 4, 5, 6, 7, 8, 9 });
    }
}

#if CONFIG_IDF_TARGET_LINUX
TEST_CASE("every transmitted frame ends with a latch gap")
{
    // The reset gap is a real (blocking) delay, which the Linux simulator
    // implements as nanosleep(): at least the requested time must elapse
    // before a setter returns, otherwise consecutive frames would be
    // concatenated into a single bit stream by the strip.
    mock_rmt_reset();
    {
        strip_t strip(kPin);
        const auto start = std::chrono::steady_clock::now();
        strip.set_all_leds({ 1, 2, 3 });
        const auto elapsed = std::chrono::steady_clock::now() - start;
        // WS2812B needs the line low for at least ~280 us to latch a frame.
        CHECK(elapsed >= std::chrono::microseconds(280));
    }
}
#endif   // CONFIG_IDF_TARGET_LINUX

TEST_CASE("a failed wait aborts the channel and leaves the object usable")
{
    mock_rmt_reset();
    {
        strip_t strip(kPin);
        mock_rmt_fail_wait_all_done(true);
        CHECK_THROWS_AS(strip.set_all_leds({ 1, 2, 3 }), strip_t::exception);
        // The in-flight transaction must be stopped and the channel restarted
        // so the RMT drops the payload reference and the channel is not left
        // busy.
        CHECK(mock_rmt_disable_count() >= 1);
        CHECK(mock_rmt_enable_count() >= 1);
        mock_rmt_fail_wait_all_done(false);
        strip.set_all_leds({ 4, 5, 6 });
        CHECK(mock_rmt_last_payload() == std::vector<uint8_t>{ 4, 5, 6, 4, 5, 6, 4, 5, 6 });
    }
}

TEST_CASE("a failed wait tolerates abort failures")
{
    mock_rmt_reset();
    {
        strip_t strip(kPin);
        mock_rmt_fail_wait_all_done(true);
        mock_rmt_fail_disable(true);
        mock_rmt_fail_enable(true);
        CHECK_THROWS_AS(strip.set_all_leds({ 1, 2, 3 }), strip_t::exception);
        mock_rmt_fail_disable(false);
        mock_rmt_fail_enable(false);
        mock_rmt_fail_wait_all_done(false);
    }
}

TEST_CASE("set_leds size mismatch reports the expected size")
{
    mock_rmt_reset();
    {
        strip_t strip(kPin);
        std::vector<strip_t::RGB> too_few(2);
        try {
            strip.set_leds(too_few);
            FAIL("expected an exception");
        } catch (const strip_t::exception &e) {
            CHECK(std::strcmp(
                          e.what(),
                          "ws2812: the colors container must hold exactly NumLeds colors") ==
                  0);
        }
    }
}

TEST_CASE("grb color order swaps red and green")
{
    mock_rmt_reset();
    {
        ws2812<1, order_grb> strip(kPin);
        strip.set_all_leds({ 0x11, 0x22, 0x33 });
        CHECK(mock_rmt_last_payload() == std::vector<uint8_t>{ 0x22, 0x11, 0x33 });
    }
}

TEST_CASE("all_on and all_off")
{
    mock_rmt_reset();
    {
        strip_t strip(kPin);
        strip.all_on();
        CHECK(mock_rmt_last_payload() == std::vector<uint8_t>(3 * 3, 0xFF));
        strip.all_off();
        CHECK(mock_rmt_last_payload() == std::vector<uint8_t>(3 * 3, 0x00));
    }
}

TEST_CASE("transmit failures are reported")
{
    mock_rmt_reset();
    {
        strip_t strip(kPin);
        mock_rmt_fail_transmit(true);
        CHECK_THROWS_AS(strip.set_all_leds({ 1, 2, 3 }), strip_t::exception);
        mock_rmt_fail_transmit(false);
    }
}

TEST_CASE("wait_all_done failures are reported")
{
    mock_rmt_reset();
    {
        strip_t strip(kPin);
        mock_rmt_fail_wait_all_done(true);
        CHECK_THROWS_AS(strip.set_all_leds({ 1, 2, 3 }), strip_t::exception);
        mock_rmt_fail_wait_all_done(false);
    }
}

TEST_CASE("destructor deinitializes the strip even when all_off fails")
{
    mock_rmt_reset();
    {
        strip_t strip(kPin);
        mock_rmt_fail_transmit(true);
        // The destructor swallows the all_off() exception and still tears the
        // RMT channel down.
    }
}

TEST_CASE("destructor tolerates rmt_disable failures")
{
    mock_rmt_reset();
    {
        strip_t strip(kPin);
        mock_rmt_fail_disable(true);
    }
    mock_rmt_fail_disable(false);
}

TEST_CASE("destructor tolerates rmt_del_encoder failures")
{
    mock_rmt_reset();
    {
        strip_t strip(kPin);
        mock_rmt_fail_del_encoder(true);
    }
    mock_rmt_fail_del_encoder(false);
}

TEST_CASE("destructor tolerates rmt_del_channel failures")
{
    mock_rmt_reset();
    {
        strip_t strip(kPin);
        mock_rmt_fail_del_channel(true);
    }
    mock_rmt_fail_del_channel(false);
}

TEST_CASE("move construction steals the channel and keeps it working")
{
    mock_rmt_reset();
    {
        strip_t a(kPin);
        strip_t b(std::move(a));
        b.set_all_leds({ 0xAA, 0xBB, 0xCC });
        CHECK(mock_rmt_last_payload() ==
              std::vector<uint8_t>{ 0xAA, 0xBB, 0xCC, 0xAA, 0xBB, 0xCC, 0xAA, 0xBB, 0xCC });
    }
}

TEST_CASE("move assignment releases the target and steals the source")
{
    mock_rmt_reset();
    {
        strip_t a(kPin);
        strip_t b(kPin);
        a.set_all_leds({ 1, 2, 3 });
        b = std::move(a);
        b.set_all_leds({ 4, 5, 6 });
        CHECK(mock_rmt_last_payload() == std::vector<uint8_t>{ 4, 5, 6, 4, 5, 6, 4, 5, 6 });
        // Self-move assignment is a no-op and must not tear the channel down.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-move"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wself-move"
#endif
        b = std::move(b);
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
        b.set_all_leds({ 7, 8, 9 });
        CHECK(mock_rmt_last_payload() == std::vector<uint8_t>{ 7, 8, 9, 7, 8, 9, 7, 8, 9 });
    }
}

TEST_CASE("move assignment handles a moved-from target")
{
    mock_rmt_reset();
    {
        strip_t a(kPin);
        strip_t b(kPin);
        // Move from a into b: b's channel is torn down and a's is stolen.
        b = std::move(a);
        // a is moved-from (null channel): assigning into it must not try to
        // tear its channel down, but still steals b's channel back into a.
        a = std::move(b);
        a.set_all_leds({ 4, 5, 6 });
        CHECK(mock_rmt_last_payload() == std::vector<uint8_t>{ 4, 5, 6, 4, 5, 6, 4, 5, 6 });
    }
}

TEST_CASE("move assignment tolerates an rmt_disable failure")
{
    mock_rmt_reset();
    {
        strip_t a(kPin);
        strip_t b(kPin);
        mock_rmt_fail_disable(true);
        // The failed disable is logged, not thrown (move assignment is
        // noexcept); the channel handle is still stolen from a.
        b = std::move(a);
        mock_rmt_fail_disable(false);
        b.set_all_leds({ 1, 2, 3 });
        CHECK(mock_rmt_last_payload_size() == 9);
    }
}

TEST_CASE("move assignment tolerates an rmt_del_encoder failure")
{
    mock_rmt_reset();
    {
        strip_t a(kPin);
        strip_t b(kPin);
        mock_rmt_fail_del_encoder(true);
        b = std::move(a);
        mock_rmt_fail_del_encoder(false);
    }
}

TEST_CASE("move assignment tolerates an rmt_del_channel failure")
{
    mock_rmt_reset();
    {
        strip_t a(kPin);
        strip_t b(kPin);
        mock_rmt_fail_del_channel(true);
        b = std::move(a);
        mock_rmt_fail_del_channel(false);
    }
}

TEST_CASE("a moved-from strip is a no-op on destruction")
{
    mock_rmt_reset();
    {
        strip_t a(kPin);
        strip_t b(std::move(a));
        // a is moved-from: its destructor must not touch the RMT driver.
    }
}

TEST_CASE("calling methods on a moved-from strip throws")
{
    mock_rmt_reset();
    strip_t a(kPin);
    strip_t b(std::move(a));
    // Intentional use of the moved-from object: the class must throw instead
    // of passing a null channel to the RMT driver.
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
    CHECK_THROWS_AS(a.set_all_leds({ 1, 2, 3 }), strip_t::exception);
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
    CHECK_THROWS_AS(a.all_off(), strip_t::exception);
    std::array<strip_t::RGB, 3> colors{};
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
    CHECK_THROWS_AS(a.set_leds(colors), strip_t::exception);
}

TEST_CASE("move assignment turns the target LEDs off before releasing the channel")
{
    mock_rmt_reset();
    {
        strip_t a(kPin);
        strip_t b(kPin);
        b.set_all_leds({ 1, 2, 3 });
        a = std::move(b);
        // b's last frame must have been replaced by an all-off transmission.
        CHECK(mock_rmt_last_payload() == std::vector<uint8_t>(3 * 3, 0x00));
    }
}

TEST_CASE("move assignment tolerates an all_off failure")
{
    mock_rmt_reset();
    {
        strip_t a(kPin);
        strip_t b(kPin);
        mock_rmt_fail_transmit(true);
        // The failed shutdown is swallowed (move assignment is noexcept) and
        // the channel handle is still stolen from b.
        a = std::move(b);
        mock_rmt_fail_transmit(false);
        a.set_all_leds({ 1, 2, 3 });
        CHECK(mock_rmt_last_payload_size() == 9);
    }
}

TEST_CASE("wait_all_done timeout is reported in the exception")
{
    mock_rmt_reset();
    {
        strip_t strip(kPin);
        mock_rmt_fail_wait_all_done(true);
        try {
            strip.set_all_leds({ 1, 2, 3 });
            FAIL("expected an exception");
        } catch (const strip_t::exception &e) {
            CHECK(std::string(e.what()).find("ESP_ERR_TIMEOUT") != std::string::npos);
        }
        mock_rmt_fail_wait_all_done(false);
    }
}
