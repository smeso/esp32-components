/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <atomic>
#include <cstddef>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <type_traits>

#include <doctest/doctest.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "leds_effects.hpp"
#include "mock_strip.hpp"

using namespace mock_bus;

namespace {

using strip_t = mock_strip<3>;

// Arguments for the stopper task used to interrupt an effect mid-way.
struct stop_args
{
    std::atomic<bool> *stop;
    std::size_t delay_ms;
};

// A higher-priority task that sets the stop flag after `delay_ms`. Because
// the effects run in the (lower-priority) test task and block on
// vTaskDelay(), the stopper preempts them at the first tick boundary after
// the delay and aborts the effect.
void
stop_after(void *arg)
{
    auto *args = static_cast<stop_args *>(arg);
    vTaskDelay(pdMS_TO_TICKS(args->delay_ms));
    args->stop->store(true);
    vTaskDelete(nullptr);
}

void
arm_stopper(std::atomic<bool> &stop, std::size_t delay_ms)
{
    static stop_args args;
    args.stop = &stop;
    args.delay_ms = delay_ms;
    xTaskCreate(stop_after, "led_stop", 2048, &args, 5, nullptr);
}

// Every frame delay blocks for at least one tick now, so a delayed stopper
// task can interrupt any effect deterministically: it preempts the (lower-
// priority) test task at a tick boundary and sets the stop flag, and the
// effect notices it at the next `stop &&` check.

// Number of allocations made through counting_allocator since the last reset,
// so a test can assert that the spatial dithering's scratch buffer really goes
// through the caller-provided allocator (and that the default spatial_leds == 1
// fade does not allocate at all).
std::size_t counting_allocations = 0;

// A default-constructible std::allocator clone that counts its allocations.
template<typename T>
struct counting_allocator
{
    using value_type = T;

    counting_allocator() = default;
    template<typename U>
    counting_allocator(const counting_allocator<U> &) noexcept
    {
    }

    T *allocate(std::size_t n)
    {
        ++counting_allocations;
        return std::allocator<T>{}.allocate(n);
    }
    void deallocate(T *p, std::size_t n) noexcept
    {
        std::allocator<T>{}.deallocate(p, n);
    }
};

template<typename T, typename U>
bool
operator==(const counting_allocator<T> &, const counting_allocator<U> &)
{
    return true;
}

}   // namespace

TEST_CASE("rotating template-value walk covers every LED once")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    leds_effects::rotating<strip_t, strip_t::RGB{ 1, 2, 3 }>(strip, 1, 1, &stop);
    REQUIRE(strip.frame_count() == 4);
    // One frame per LED position, plus the final all_off().
    for (std::size_t i = 0; i < 3; ++i) {
        const auto &frame = strip.frame(i);
        for (std::size_t l = 0; l < strip_t::num_leds(); ++l)
            CHECK((frame[l].r == (l == i ? 1 : 0)));
        CHECK(frame[i].g == 2);
        CHECK(frame[i].b == 3);
    }
    CHECK(strip.frame(3) == strip_t::colors_t{});
}

TEST_CASE("rotating runtime-color walk covers every LED once")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    leds_effects::rotating(strip, strip_t::RGB{ 1, 2, 3 }, 1, 1, &stop);
    REQUIRE(strip.frame_count() == 4);
    for (std::size_t i = 0; i < 3; ++i) {
        const auto &frame = strip.frame(i);
        for (std::size_t l = 0; l < strip_t::num_leds(); ++l)
            CHECK((frame[l].r == (l == i ? 1 : 0)));
        CHECK(frame[i].g == 2);
        CHECK(frame[i].b == 3);
    }
    CHECK(strip.frame(3) == strip_t::colors_t{});
}

TEST_CASE("rotating stops on the stop flag before rendering anything")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    stop.store(true);
    leds_effects::rotating<strip_t, strip_t::RGB{ 1, 2, 3 }>(strip, 1, 1, &stop);
    CHECK(strip.frame_count() == 0);

    leds_effects::rotating(strip, strip_t::RGB{ 1, 2, 3 }, 1, 1, &stop);
    CHECK(strip.frame_count() == 0);
}

TEST_CASE("rotating stops mid-way and skips all_off")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    arm_stopper(stop, 25);
    // With a 10 ms interval the walk blocks on real time, so the stopper
    // (priority 5) preempts the effect and aborts it before the final
    // all_off().
    leds_effects::rotating<strip_t, strip_t::RGB{ 9, 9, 9 }>(strip, 10, 1000, &stop);
    REQUIRE(strip.frame_count() > 0);
    CHECK(strip.frame_count() < 1000);
    // The last frame is a walk position (one LED lit), not all_off().
    CHECK(strip.frame(strip.frame_count() - 1) != strip_t::colors_t{});
    CHECK(stop.load());
}

TEST_CASE("rotating with negative repetitions runs until stopped")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    arm_stopper(stop, 25);
    leds_effects::rotating<strip_t, strip_t::RGB{ 9, 9, 9 }>(strip, 10, -1, &stop);
    REQUIRE(strip.frame_count() > 0);
    CHECK(strip.frame(strip.frame_count() - 1) != strip_t::colors_t{});
    CHECK(stop.load());
}

TEST_CASE("rotating runtime-color with negative repetitions runs until stopped")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    arm_stopper(stop, 25);
    leds_effects::rotating(strip, strip_t::RGB{ 9, 9, 9 }, 10, -1, &stop);
    REQUIRE(strip.frame_count() > 0);
    CHECK(strip.frame(strip.frame_count() - 1) != strip_t::colors_t{});
    CHECK(stop.load());
}

TEST_CASE("bouncing template-value walk covers every position once")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    leds_effects::bouncing<strip_t, strip_t::RGB{ 1, 2, 3 }>(strip, 1, 1, &stop);
    // 2 * (n - 1) positions plus the final all_off().
    REQUIRE(strip.frame_count() == 5);
    const std::size_t positions = 2 * (strip_t::num_leds() - 1);
    for (std::size_t i = 0; i < positions; ++i) {
        const auto &frame = strip.frame(i);
        const std::size_t idx = i < strip_t::num_leds() ? i : positions - i;
        for (std::size_t l = 0; l < strip_t::num_leds(); ++l) {
            CHECK(frame[l].r == (l == idx ? 1 : 0));
            CHECK(frame[l].g == (l == idx ? 2 : 0));
            CHECK(frame[l].b == (l == idx ? 3 : 0));
        }
    }
    CHECK(strip.frame(4) == strip_t::colors_t{});
}

TEST_CASE("bouncing runtime-color walk covers every position once")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    leds_effects::bouncing(strip, strip_t::RGB{ 1, 2, 3 }, 1, 1, &stop);
    REQUIRE(strip.frame_count() == 5);
    const std::size_t positions = 2 * (strip_t::num_leds() - 1);
    for (std::size_t i = 0; i < positions; ++i) {
        const auto &frame = strip.frame(i);
        const std::size_t idx = i < strip_t::num_leds() ? i : positions - i;
        for (std::size_t l = 0; l < strip_t::num_leds(); ++l)
            CHECK(frame[l].r == (l == idx ? 1 : 0));
    }
    CHECK(strip.frame(4) == strip_t::colors_t{});
}

TEST_CASE("bouncing stops on the stop flag before rendering anything")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    stop.store(true);
    leds_effects::bouncing<strip_t, strip_t::RGB{ 1, 2, 3 }>(strip, 1, 1, &stop);
    CHECK(strip.frame_count() == 0);

    leds_effects::bouncing(strip, strip_t::RGB{ 1, 2, 3 }, 1, 1, &stop);
    CHECK(strip.frame_count() == 0);
}

TEST_CASE("bouncing with negative repetitions runs until stopped")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    arm_stopper(stop, 25);
    leds_effects::bouncing<strip_t, strip_t::RGB{ 9, 9, 9 }>(strip, 10, -1, &stop);
    REQUIRE(strip.frame_count() > 0);
    CHECK(strip.frame(strip.frame_count() - 1) != strip_t::colors_t{});
    CHECK(stop.load());
}

TEST_CASE("bouncing runtime-color with negative repetitions runs until stopped")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    arm_stopper(stop, 25);
    leds_effects::bouncing(strip, strip_t::RGB{ 9, 9, 9 }, 10, -1, &stop);
    REQUIRE(strip.frame_count() > 0);
    CHECK(strip.frame(strip.frame_count() - 1) != strip_t::colors_t{});
    CHECK(stop.load());
}

TEST_CASE("rotating and bouncing with a null stop flag")
{
    strip_t strip;
    // The null stop flag short-circuits the `stop &&` checks in the walk
    // loops.
    leds_effects::rotating<strip_t, strip_t::RGB{ 1, 2, 3 }>(strip, 1, 1, nullptr);
    CHECK(strip.frame_count() == 4);
    strip.reset();
    leds_effects::rotating(strip, strip_t::RGB{ 1, 2, 3 }, 1, 1, nullptr);
    CHECK(strip.frame_count() == 4);
    strip.reset();
    leds_effects::bouncing<strip_t, strip_t::RGB{ 1, 2, 3 }>(strip, 1, 1, nullptr);
    CHECK(strip.frame_count() == 5);
    strip.reset();
    leds_effects::bouncing(strip, strip_t::RGB{ 1, 2, 3 }, 1, 1, nullptr);
    CHECK(strip.frame_count() == 5);
}

TEST_CASE("smooth_transition interpolates between the colors")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    leds_effects::smooth_transition(
            strip, strip_t::RGB{ 0, 0, 0 }, strip_t::RGB{ 255, 0, 0 }, 100, &stop);
    REQUIRE(strip.frame_count() >= 2);
    // First frame is the start color, last frame is the end color.
    CHECK(strip.frame(0)[0].r == 0);
    CHECK(strip.frame(0)[0].g == 0);
    const auto &last = strip.frame(strip.frame_count() - 1);
    CHECK(last[0].r == 255);
    CHECK(last[0].g == 0);
    // The transition reached intermediate red values (temporal dithering
    // makes individual frames oscillate, but the range is covered).
    bool saw_mid = false;
    for (std::size_t i = 1; i < strip.frame_count() - 1; ++i) {
        if (strip.frame(i)[0].r > 0 && strip.frame(i)[0].r < 255)
            saw_mid = true;
    }
    CHECK(saw_mid);
}

TEST_CASE("smooth_transition to cyan clamps green and blue at 255")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    // A transition that reaches (255, 255, 255) exercises the ri < 255
    // dithering clamp branches on all three channels.
    leds_effects::smooth_transition(
            strip, strip_t::RGB{ 0, 0, 0 }, strip_t::RGB{ 255, 255, 255 }, 100, &stop);
    REQUIRE(strip.frame_count() >= 2);
    const auto &last = strip.frame(strip.frame_count() - 1);
    CHECK(last[0].r == 255);
    CHECK(last[0].g == 255);
    CHECK(last[0].b == 255);
}

TEST_CASE("smooth_transition with a two-frame total uses a single step")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    // Exactly two dither frames: one step from start to end.
    constexpr std::size_t frame_ms = leds_effects::DitherConfig{}.frame_ms();
    leds_effects::smooth_transition(
            strip, strip_t::RGB{ 0, 0, 0 }, strip_t::RGB{ 255, 0, 0 }, 2 * frame_ms, &stop);
    REQUIRE(strip.frame_count() == 2);
    CHECK(strip.frame(1)[0].r == 255);
}

TEST_CASE("smooth_transition short duration applies the end color directly")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    // Shorter than one dither frame: only the end color is applied. The
    // duration is read from a volatile so the compiler cannot fold the
    // dither_frames() clamp away (a constant would be evaluated at compile
    // time and the coverage tooling would never see the branch).
    volatile std::size_t duration = 5;
    leds_effects::smooth_transition(
            strip, strip_t::RGB{ 1, 1, 1 }, strip_t::RGB{ 9, 9, 9 }, duration, &stop);
    REQUIRE(strip.frame_count() == 1);
    CHECK(strip.frame(0)[0].r == 9);
    CHECK(strip.frame(0)[0].g == 9);
    CHECK(strip.frame(0)[0].b == 9);
}

TEST_CASE("smooth_transition short duration stops on the stop flag")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    stop.store(true);
    // Even the short-duration path must check the stop flag before touching
    // the strip, like every other effect does.
    volatile std::size_t duration = 5;
    leds_effects::smooth_transition(
            strip, strip_t::RGB{ 1, 1, 1 }, strip_t::RGB{ 9, 9, 9 }, duration, &stop);
    CHECK(strip.frame_count() == 0);
}

TEST_CASE("smooth_transition stops on the stop flag")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    stop.store(true);
    leds_effects::smooth_transition(
            strip, strip_t::RGB{ 0, 0, 0 }, strip_t::RGB{ 255, 0, 0 }, 100, &stop);
    CHECK(strip.frame_count() == 0);
}

TEST_CASE("smooth_transition honors the dither frame rate")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    // 50 Hz frames: a 20 ms frame period, so 100 ms is five frames (fewer
    // than the ten the default rate renders).
    leds_effects::DitherConfig dither;
    dither.dither_hz = 50;
    dither.flicker_hz = 10;
    leds_effects::smooth_transition(
            strip, strip_t::RGB{ 0, 0, 0 }, strip_t::RGB{ 255, 0, 0 }, 100, &stop, dither);
    REQUIRE(strip.frame_count() == 5);
    CHECK(strip.frame(0)[0].r == 0);
    CHECK(strip.frame(4)[0].r == 255);
}

TEST_CASE("the spatial scratch buffer is allocated through the caller's allocator")
{
    strip_t strip;
    using rgb_t = strip_t::RGB;

    // With spatial_leds > 1 the dithering needs a per-LED scratch buffer; the
    // effect heap-allocates it through the Allocator template argument instead
    // of putting a strip-sized array on the task stack.
    counting_allocations = 0;
    leds_effects::smooth_transition<strip_t, 2, counting_allocator<rgb_t>>(
            strip, rgb_t{ 0, 0, 0 }, rgb_t{ 255, 0, 0 }, 100);
    CHECK(counting_allocations >= 1);
    CHECK(strip.frame_count() >= 2);

    // The default spatial_leds == 1 fade uses set_all_leds() and needs no
    // scratch, so it allocates nothing.
    strip.reset();
    counting_allocations = 0;
    leds_effects::smooth_transition<strip_t, 1, counting_allocator<rgb_t>>(
            strip, rgb_t{ 0, 0, 0 }, rgb_t{ 255, 0, 0 }, 100);
    CHECK(counting_allocations == 0);
    CHECK(strip.frame_count() >= 2);
}

TEST_CASE("sun_effect rises and sets")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    leds_effects::sun_effect(strip, 100, true, &stop);
    REQUIRE(strip.frame_count() >= 2);
    // Rising: starts dim warm, ends bright warm.
    CHECK(strip.frame(0)[0].r < 100);
    const auto &last = strip.frame(strip.frame_count() - 1);
    CHECK(last[0].r == 255);
    CHECK(last[0].g == 180);
    CHECK(last[0].b == 80);

    strip.reset();
    leds_effects::sun_effect(strip, 100, false, &stop);
    REQUIRE(strip.frame_count() >= 2);
    // Setting: starts bright warm, ends dim warm.
    CHECK(strip.frame(0)[0].r == 255);
    CHECK(strip.frame(0)[0].g == 180);
    const auto &last2 = strip.frame(strip.frame_count() - 1);
    CHECK(last2[0].r == 5);
    CHECK(last2[0].g == 1);
    CHECK(last2[0].b == 0);
}

TEST_CASE("sun_effect maps the transition endpoints through the calibration")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    leds_effects::LedCalibration cal;
    cal.gamma = 2.0f;
    leds_effects::sun_effect(strip, 100, true, &stop, cal);
    REQUIRE(strip.frame_count() >= 2);
    const auto &first = strip.frame(0);
    const auto &last = strip.frame(strip.frame_count() - 1);
    // calibrate({ 5, 1, 0 }, gamma 2.0) -> pow(5/255, 2) ~ 0 -> black,
    // calibrate({ 255, 180, 80 }, gamma 2.0) -> pow(x, 2) per channel.
    CHECK(first[0].r == 0);
    CHECK(first[0].g == 0);
    CHECK(last[0].r == 255);
    CHECK(last[0].g == 127);
    CHECK(last[0].b == 25);
}

TEST_CASE("breathing alternates the two colors and waits")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    leds_effects::breathing(
            strip, strip_t::RGB{ 0, 0, 0 }, strip_t::RGB{ 255, 0, 0 }, 100, 10, 1, &stop);
    // Two transitions (up and down) with two wait periods in between.
    REQUIRE(strip.frame_count() >= 4);
    // The first transition goes dark -> red and the second red -> dark.
    CHECK(strip.frame(0)[0].r < 100);
    bool saw_peak = false;
    for (std::size_t i = 0; i < strip.frame_count(); ++i) {
        if (strip.frame(i)[0].r == 255)
            saw_peak = true;
    }
    CHECK(saw_peak);
    const auto &end = strip.frame(strip.frame_count() - 1);
    CHECK(end[0].r == 0);
    CHECK(end[0].g == 0);
    CHECK(end[0].b == 0);
}

TEST_CASE("breathing with an already-set stop flag is a no-op")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    stop.store(true);
    leds_effects::breathing(
            strip, strip_t::RGB{ 0, 0, 0 }, strip_t::RGB{ 255, 0, 0 }, 100, 10, 1, &stop);
    CHECK(strip.frame_count() == 0);
}

TEST_CASE("breathing stops during the first wait")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    // Every frame blocks for one tick, so a 100 ms transition renders for
    // about 100 ms. The stopper at 150 ms fires while the first wait (the
    // second 100 ms slot) is blocking, so the wait aborts.
    arm_stopper(stop, 150);
    leds_effects::breathing(
            strip, strip_t::RGB{ 0, 0, 0 }, strip_t::RGB{ 255, 0, 0 }, 100, 100, 1, &stop);
    REQUIRE(strip.frame_count() > 0);
    CHECK(stop.load());
}

TEST_CASE("breathing stops during the second wait")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    // The first transition and the first wait (100 ms each) complete, the
    // second transition renders, and the stopper at 350 ms sets the flag
    // while the second wait (the last 100 ms slot) is blocking.
    arm_stopper(stop, 350);
    leds_effects::breathing(
            strip, strip_t::RGB{ 0, 0, 0 }, strip_t::RGB{ 255, 0, 0 }, 100, 100, 1, &stop);
    REQUIRE(strip.frame_count() > 0);
    CHECK(stop.load());
}

TEST_CASE("breathing with negative repetitions runs until stopped")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    arm_stopper(stop, 250);
    leds_effects::breathing(
            strip, strip_t::RGB{ 0, 0, 0 }, strip_t::RGB{ 255, 0, 0 }, 100, 100, -1, &stop);
    REQUIRE(strip.frame_count() > 0);
    CHECK(stop.load());
}

TEST_CASE("breathing stops mid-transition on the second transition")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    // The transitions block on their dither delays, so the stopper can fire
    // anywhere. With a 1000 ms duration each transition renders for a
    // second; the stopper at 1500 ms falls into the second transition (which
    // goes red -> dark), so the check after it aborts mid-fade.
    arm_stopper(stop, 1500);
    leds_effects::breathing(
            strip, strip_t::RGB{ 0, 0, 0 }, strip_t::RGB{ 255, 0, 0 }, 1000, 50, 1, &stop);
    REQUIRE(strip.frame_count() > 0);
    CHECK(stop.load());
}

TEST_CASE("breathing stops mid-transition on the third transition")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    // Each repetition is two 1000 ms transitions with 50 ms waits, so the
    // first repetition ends at about 2.1 s; the stopper at 2500 ms falls into
    // the third transition (the first one of the second repetition), which
    // aborts at the check after it.
    arm_stopper(stop, 2500);
    leds_effects::breathing(
            strip, strip_t::RGB{ 0, 0, 0 }, strip_t::RGB{ 255, 0, 0 }, 1000, 50, 2, &stop);
    REQUIRE(strip.frame_count() > 0);
    CHECK(stop.load());
}

TEST_CASE("breathing with a null stop flag")
{
    strip_t strip;
    leds_effects::breathing(
            strip, strip_t::RGB{ 0, 0, 0 }, strip_t::RGB{ 255, 0, 0 }, 100, 10, 1, nullptr);
    REQUIRE(strip.frame_count() >= 4);
    // The null stop flag short-circuits all the `stop &&` checks.
    const auto &end = strip.frame(strip.frame_count() - 1);
    CHECK(end[0].r == 0);
}

TEST_CASE("breathing maps the two colors through the calibration")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    // gamma 2.0 keeps black black and maps the 128 green level to
    // pow(128/255, 2) * 255 = 64, like the sun_effect calibration test.
    leds_effects::LedCalibration cal;
    cal.gamma = 2.0f;
    leds_effects::breathing(strip,
                            strip_t::RGB{ 0, 0, 0 },
                            strip_t::RGB{ 255, 128, 0 },
                            100,
                            10,
                            1,
                            &stop,
                            cal);
    REQUIRE(strip.frame_count() >= 4);
    // The calibrated upper color { 255, 64, 0 } is reached at least once.
    bool saw_peak = false;
    for (std::size_t i = 0; i < strip.frame_count(); ++i) {
        if (strip.frame(i)[0].r == 255 && strip.frame(i)[0].g == 64)
            saw_peak = true;
    }
    CHECK(saw_peak);
    // It ends on the calibrated lower color (black).
    const auto &end = strip.frame(strip.frame_count() - 1);
    CHECK(end[0].r == 0);
    CHECK(end[0].g == 0);
    CHECK(end[0].b == 0);
}

TEST_CASE("LedCalibration defaults are neutral")
{
    constexpr leds_effects::LedCalibration cal;
    CHECK(cal.inv_matrix[0][0] == 1.0f);
    CHECK(cal.inv_matrix[1][1] == 1.0f);
    CHECK(cal.inv_matrix[2][2] == 1.0f);
    CHECK(cal.inv_matrix[0][1] == 0.0f);
    CHECK(cal.black[0] == 0.0f);
    CHECK(cal.gamma == 1.0f);
    // The identity matrix leaves the targets unchanged.
    constexpr auto d = cal.drive(0x8000, 0x1234, 0xABCD);
    static_assert(d.r == 0x8000);
    static_assert(d.g == 0x1234);
    static_assert(d.b == 0xABCD);
    CHECK(d.r == 0x8000);
    CHECK(d.g == 0x1234);
    CHECK(d.b == 0xABCD);
    // Constant evaluation never executes the instrumented code, so also drive
    // at runtime to keep the branch coverage complete. The volatile inputs
    // stop the compiler from folding the call into another constant
    // expression.
    volatile uint16_t vr = 0x8000;
    volatile uint16_t vg = 0x1234;
    volatile uint16_t vb = 0xABCD;
    const auto runtime_d = cal.drive(vr, vg, vb);
    CHECK(runtime_d.r == 0x8000);
    CHECK(runtime_d.g == 0x1234);
    CHECK(runtime_d.b == 0xABCD);
}

TEST_CASE("LedCalibration applies the full inverse matrix")
{
    leds_effects::LedCalibration cal;
    // The green primary leaks half its light into the red channel, so the
    // inverse matrix removes that contribution when computing the drive.
    cal.inv_matrix = {
        std::array<float, 3>{ 1.0f, 0.5f, 0.0f },
        std::array<float, 3>{ 0.0f, 1.0f, 0.0f },
        std::array<float, 3>{ 0.0f, 0.0f, 1.0f },
    };
    const auto d = cal.drive(0x4000, 0x4000, 0x0000);
    // d0 = 0.25 + 0.5 * 0.25 = 0.375 -> 24576, d1 = 0.25 -> 16384.
    CHECK(d.r == 24576);
    CHECK(d.g == 16384);
    CHECK(d.b == 0);
}

TEST_CASE("LedCalibration clamps out-of-gamut targets and rescales bright ones")
{
    leds_effects::LedCalibration cal;
    // A target below the black level is out of gamut on every channel and
    // clamps to zero.
    cal.black = { 0.5f, 0.5f, 0.5f };
    const auto dark = cal.drive(0x0000, 0x0000, 0x0000);
    CHECK(dark.r == 0);
    CHECK(dark.g == 0);
    CHECK(dark.b == 0);

    // The red primary is only half as bright as the reference, so a
    // full-scale red target needs twice the drive and rescales all channels
    // to keep the hue at a lower brightness.
    cal.black = { 0.0f, 0.0f, 0.0f };
    cal.inv_matrix = {
        std::array<float, 3>{ 2.0f, 0.0f, 0.0f },
        std::array<float, 3>{ 0.0f, 1.0f, 0.0f },
        std::array<float, 3>{ 0.0f, 0.0f, 1.0f },
    };
    const auto bright = cal.drive(0xFFFF, 0x4000, 0x4000);
    // d = (2.0, 0.25, 0.25) rescaled by 2 -> (1.0, 0.125, 0.125).
    CHECK(bright.r == 65535);
    CHECK(bright.g == 8192);
    CHECK(bright.b == 8192);
}

TEST_CASE("LedCalibration gamma encodes the drive values")
{
    leds_effects::LedCalibration cal;
    cal.gamma = 2.0f;
    const auto d = cal.drive(0x8000, 0xFFFF, 0x0000);
    // pow(0.5, 2.0) = 0.25 -> 16384; full scale and black stay unchanged.
    CHECK(d.r == 16384);
    CHECK(d.g == 65535);
    CHECK(d.b == 0);

    // The gamma path is also constexpr: pow(0.5, 2.0) = 0.25 -> 16384.
    constexpr leds_effects::LedCalibration gamma_cal{ .gamma = 2.0f };
    constexpr auto gd = gamma_cal.drive(0x8000, 0xFFFF, 0x0000);
    static_assert(gd.r == 16384);
    static_assert(gd.g == 65535);
    static_assert(gd.b == 0);
}

TEST_CASE("LedCalibration gamma handles invalid data")
{
    // Non-finite gamma falls back to the identity encoding instead of
    // invoking undefined behavior in the quantizer.
    leds_effects::LedCalibration nan_gamma;
    nan_gamma.gamma = std::numeric_limits<float>::quiet_NaN();
    const auto d_nan = nan_gamma.drive(0x8000, 0x1234, 0xABCD);
    CHECK(d_nan.r == 0x8000);
    CHECK(d_nan.g == 0x1234);
    CHECK(d_nan.b == 0xABCD);

    // A negative (or zero) gamma is treated as 1.0, not applied.
    leds_effects::LedCalibration negative;
    negative.gamma = -2.0f;
    const auto d_neg = negative.drive(0x8000, 0x1234, 0xABCD);
    CHECK(d_neg.r == 0x8000);
    CHECK(d_neg.g == 0x1234);
    CHECK(d_neg.b == 0xABCD);
}

TEST_CASE("LedCalibration non-finite calibration data maps to black")
{
    // NaN or Inf in the matrix or the black level used to propagate into
    // the quantizer (undefined behavior); now it maps to black.
    leds_effects::LedCalibration cal;
    cal.inv_matrix = {
        std::array<float, 3>{ std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f },
        std::array<float, 3>{ 0.0f, std::numeric_limits<float>::infinity(), 0.0f },
        std::array<float, 3>{ 0.0f, 0.0f, 1.0f },
    };
    const auto d = cal.drive(0xFFFF, 0xFFFF, 0xFFFF);
    CHECK(d.r == 0);
    CHECK(d.g == 0);
    CHECK(d.b == 0);

    leds_effects::LedCalibration black_cal;
    black_cal.black = { std::numeric_limits<float>::infinity(),
                        std::numeric_limits<float>::quiet_NaN(),
                        0.0f };
    const auto db = black_cal.drive(0x8000, 0x8000, 0x8000);
    CHECK(db.r == 0);
    CHECK(db.g == 0);
    CHECK(db.b == 0);
}

TEST_CASE("calibrate is neutral with the default calibration")
{
    constexpr strip_t::RGB in{ 0x12, 0x34, 0xAB };
    constexpr auto out = leds_effects::calibrate(in, leds_effects::LedCalibration{});
    static_assert(out.r == 0x12);
    static_assert(out.g == 0x34);
    static_assert(out.b == 0xAB);
    CHECK(out.r == 0x12);
    CHECK(out.g == 0x34);
    CHECK(out.b == 0xAB);

    // The calibrate path is also exercised at runtime to keep the branch
    // coverage of the instrumented code complete.
    volatile strip_t::RGB vin{ 0x12, 0x34, 0xAB };
    const auto rout = leds_effects::calibrate(vin, leds_effects::LedCalibration{});
    CHECK(rout.r == 0x12);
    CHECK(rout.g == 0x34);
    CHECK(rout.b == 0xAB);
}

TEST_CASE("calibrate applies the inverse matrix and clamps at 255")
{
    // The red primary is only half as bright as the reference: a full-scale
    // red target needs twice the drive and rescales all channels, so the
    // green/blue fractions map back below full scale.
    leds_effects::LedCalibration cal;
    cal.inv_matrix = {
        std::array<float, 3>{ 2.0f, 0.0f, 0.0f },
        std::array<float, 3>{ 0.0f, 1.0f, 0.0f },
        std::array<float, 3>{ 0.0f, 0.0f, 1.0f },
    };
    const auto bright = leds_effects::calibrate(strip_t::RGB{ 255, 64, 64 }, cal);
    CHECK(bright.r == 255);
    CHECK(bright.g == 32);
    CHECK(bright.b == 32);

    // A target below the black level is out of gamut and clamps to black.
    cal.black = { 0.5f, 0.5f, 0.5f };
    const auto dark = leds_effects::calibrate(strip_t::RGB{ 0, 0, 0 }, cal);
    CHECK(dark.r == 0);
    CHECK(dark.g == 0);
    CHECK(dark.b == 0);
}

TEST_CASE("calibrate gamma-encodes the channels")
{
    leds_effects::LedCalibration cal;
    cal.gamma = 2.0f;
    // drive(0x8000) = pow(0.5, 2.0) = 0.25 -> 16384 -> 0x40.
    const auto d = leds_effects::calibrate(strip_t::RGB{ 128, 255, 0 }, cal);
    CHECK(d.r == 64);
    CHECK(d.g == 255);
    CHECK(d.b == 0);

    constexpr leds_effects::LedCalibration gamma_cal{ .gamma = 2.0f };
    constexpr auto gd = leds_effects::calibrate(strip_t::RGB{ 128, 255, 0 }, gamma_cal);
    static_assert(gd.r == 64);
    static_assert(gd.g == 255);
    static_assert(gd.b == 0);
}

TEST_CASE("calibrate with non-finite calibration data maps to black")
{
    leds_effects::LedCalibration cal;
    cal.inv_matrix = {
        std::array<float, 3>{ std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f },
        std::array<float, 3>{ 0.0f, std::numeric_limits<float>::infinity(), 0.0f },
        std::array<float, 3>{ 0.0f, 0.0f, 1.0f },
    };
    const auto d = leds_effects::calibrate(strip_t::RGB{ 200, 100, 50 }, cal);
    CHECK(d.r == 0);
    CHECK(d.g == 0);
    CHECK(d.b == 0);
}

// The test builds run at the ESP-IDF default 100 Hz tick rate. The dither
// frame rate is capped to it, so a requested 200 Hz dither really runs at
// 100 Hz here.
static_assert(configTICK_RATE_HZ == 100, "these dither tests assume a 100 Hz tick");

TEST_CASE("DitherConfig caps the dither rate to the tick rate")
{
    leds_effects::DitherConfig config;
    // Above the tick rate: capped to it.
    volatile std::size_t fast = configTICK_RATE_HZ + 100;
    config.dither_hz = fast;
    config.flicker_hz = 60;
    CHECK(config.frame_hz() == configTICK_RATE_HZ);
    CHECK(config.frame_ms() == 1000 / configTICK_RATE_HZ);

    // Below the tick rate: kept as requested.
    volatile std::size_t slow = 50;
    config.dither_hz = slow;
    CHECK(config.frame_hz() == 50);

    // Zero: clamped to 1 Hz so the frame period stays defined.
    volatile std::size_t zero = 0;
    config.dither_hz = zero;
    CHECK(config.frame_hz() == 1);
    CHECK(config.frame_ms() == 1000);
}

TEST_CASE("DitherConfig derives the dither levels from the fusion frequency")
{
    leds_effects::DitherConfig config;
    volatile std::size_t requested = 100;
    config.dither_hz = requested;

    // One frame per cycle: no dithering at all.
    config.flicker_hz = 100;
    CHECK(config.levels() == 1);
    CHECK(config.effective_bits() == 8);

    // Two levels: one extra bit.
    config.flicker_hz = 50;
    CHECK(config.levels() == 2);
    CHECK(config.effective_bits() == 9);

    // Four levels: two extra bits.
    config.flicker_hz = 25;
    CHECK(config.levels() == 4);
    CHECK(config.effective_bits() == 10);

    // 33 levels: floor(log2(33)) = 5 extra bits.
    config.flicker_hz = 3;
    CHECK(config.levels() == 33);
    CHECK(config.effective_bits() == 13);

    // A zero fusion frequency is treated as 1 Hz (never divides by zero).
    config.flicker_hz = 0;
    CHECK(config.levels() == 100);
    CHECK(config.effective_bits() == 14);

    // A fusion frequency above the frame rate leaves a single level.
    config.flicker_hz = 200;
    CHECK(config.levels() == 1);
    CHECK(config.effective_bits() == 8);
}

TEST_CASE("DitherConfig multiplies the levels by the spatial dithering")
{
    leds_effects::DitherConfig config;
    volatile std::size_t requested = 100;
    config.dither_hz = requested;
    config.flicker_hz = 25;   // levels == 4
    REQUIRE(config.levels() == 4);

    // One spatial LED: the temporal resolution alone.
    CHECK(config.effective_levels() == 4);
    CHECK(config.effective_bits() == 10);

    // Two spatial LEDs double the renderable levels and add one bit, while
    // the temporal cycle (and the flicker frequency) is unchanged.
    CHECK(config.effective_levels(2) == 8);
    CHECK(config.effective_bits(2) == 11);
    CHECK(config.levels() == 4);

    // Three spatial LEDs: 12 levels, still 11 bits (floor(log2(12))).
    CHECK(config.effective_levels(3) == 12);
    CHECK(config.effective_bits(3) == 11);
}

TEST_CASE("dither_phase spreads the phases over the spatial group")
{
    // One spatial LED: the temporal frame index reduced modulo the levels.
    CHECK(leds_effects::detail::dither_phase(0, 4, 1, 0) == 0);
    CHECK(leds_effects::detail::dither_phase(5, 4, 1, 0) == 1);

    // Two LEDs: consecutive LEDs take consecutive phases and the frame
    // advances the phase by one spatial slot, wrapping at levels * 2.
    CHECK(leds_effects::detail::dither_phase(0, 4, 2, 0) == 0);
    CHECK(leds_effects::detail::dither_phase(0, 4, 2, 1) == 1);
    CHECK(leds_effects::detail::dither_phase(1, 4, 2, 0) == 2);
    CHECK(leds_effects::detail::dither_phase(1, 4, 2, 1) == 3);
    CHECK(leds_effects::detail::dither_phase(4, 4, 2, 1) == 1);

    // Three LEDs, two levels.
    CHECK(leds_effects::detail::dither_phase(0, 2, 3, 2) == 2);
    CHECK(leds_effects::detail::dither_phase(1, 2, 3, 1) == 4);
}

TEST_CASE("dither_all spreads the phases over the spatial group")
{
    strip_t strip;
    // The spatial dithering needs a caller-provided scratch buffer (so the
    // strip-sized array never lands on the task stack and any allocator can
    // back it).
    std::array<strip_t::RGB, strip_t::num_leds()> scratch{};

    // One LED: the plain temporal dithering (set_all_leds), the same color on
    // every LED. 0x8040 with two levels rounds its low byte to one of two
    // phases, and the first frame lands on the upper one. The scratch buffer
    // is ignored.
    leds_effects::detail::dither_all<strip_t, 1>(strip, 0x8040, 0, 0, 0, 2, scratch);
    CHECK(strip.frame(0)[0].r == 0x81);
    CHECK(strip.frame(0)[1].r == 0x81);
    CHECK(strip.frame(0)[2].r == 0x81);

    // Two LEDs: four phases. The group repeats along the strip and its LEDs
    // show consecutive phases, so the first frame lights LEDs 0 and 2 but not
    // LED 1 (the low byte rounds to one of the four phases).
    leds_effects::detail::dither_all<strip_t, 2>(strip, 0x8040, 0, 0, 0, 2, scratch);
    CHECK(strip.frame(1)[0].r == 0x81);
    CHECK(strip.frame(1)[1].r == 0x80);
    CHECK(strip.frame(1)[2].r == 0x81);

    // The next frame advances the phase by one spatial slot: both phases are
    // now above the threshold, so the whole strip shows the lower level.
    leds_effects::detail::dither_all<strip_t, 2>(strip, 0x8040, 0, 0, 1, 2, scratch);
    CHECK(strip.frame(2)[0].r == 0x80);
    CHECK(strip.frame(2)[1].r == 0x80);
    CHECK(strip.frame(2)[2].r == 0x80);
}

TEST_CASE("next_dither_frame wraps at the end of the dither cycle")
{
    using leds_effects::detail::next_dither_frame;
    // Below the cycle length it just increments.
    CHECK(next_dither_frame(0, 4, 1) == 1);
    CHECK(next_dither_frame(2, 4, 1) == 3);

    // At the end of the cycle (levels * spatial_leds) it wraps to zero instead
    // of overflowing. A plain uint32_t overflow would not be a multiple of
    // every cycle length (e.g. 3 * 2 = 6 does not divide 2^32), so the dither
    // phases would skip values; wrapping here keeps them continuous.
    CHECK(next_dither_frame(3, 4, 1) == 0);
    CHECK(next_dither_frame(4, 3, 2) == 5);
    CHECK(next_dither_frame(5, 3, 2) == 0);
}

TEST_CASE("frame_delay sleeps out the remainder of the frame")
{
    bool overrun_logged = false;
    // An update that took no time leaves the whole frame to sleep out.
    const TickType_t before = xTaskGetTickCount();
    leds_effects::detail::frame_delay(before, pdMS_TO_TICKS(20), overrun_logged);
    CHECK(xTaskGetTickCount() >= before + pdMS_TO_TICKS(20));
    CHECK_FALSE(overrun_logged);

    // An update that already overran the frame does not sleep a whole extra
    // frame: it yields one tick and logs the warning once.
    const TickType_t stale = xTaskGetTickCount() - pdMS_TO_TICKS(100);
    leds_effects::detail::frame_delay(stale, 1, overrun_logged);
    CHECK(xTaskGetTickCount() - stale >= pdMS_TO_TICKS(100));
    CHECK(overrun_logged);

    // A second overrun in the same effect run does not log again (the latch is
    // already set), but still yields.
    const TickType_t stale2 = xTaskGetTickCount() - pdMS_TO_TICKS(50);
    leds_effects::detail::frame_delay(stale2, 1, overrun_logged);
    CHECK(xTaskGetTickCount() - stale2 >= pdMS_TO_TICKS(50));
}

TEST_CASE("frame_remaining never returns zero ticks")
{
    using leds_effects::detail::frame_remaining;
    // A frame that still has time left keeps the remainder.
    CHECK(frame_remaining(2, 10) == 8);
    CHECK(frame_remaining(0, 1) == 1);
    // A frame that already overran still yields one tick instead of wrapping
    // around to a huge timeout.
    CHECK(frame_remaining(11, 10) == 1);
}

TEST_CASE("dither16 renders the low byte with the configured number of levels")
{
    volatile uint32_t frame = 0;
    // levels == 1: no dithering, the high byte alone.
    CHECK(leds_effects::detail::dither16(0x5555, frame, 1) == 0x55);
    CHECK(leds_effects::detail::dither16(0xABFF, frame, 1) == 0xAB);

    // levels == 2: a low byte of at least half a step rounds up to the next
    // level on the frames below the phase threshold.
    CHECK(leds_effects::detail::dither16(0x0080, 0, 2) == 0x01);
    CHECK(leds_effects::detail::dither16(0x0080, 1, 2) == 0x00);
    CHECK(leds_effects::detail::dither16(0x0001, 0, 2) == 0x00);

    // levels == 4: the low byte is rounded to the nearest quarter.
    CHECK(leds_effects::detail::dither16(0x0040, 0, 4) == 0x01);
    CHECK(leds_effects::detail::dither16(0x00C0, 2, 4) == 0x01);
    CHECK(leds_effects::detail::dither16(0x00C0, 3, 4) == 0x00);

    // A saturated channel keeps 0xFF when the next level up is requested.
    CHECK(leds_effects::detail::dither16(0xFFFF, 0, 2) == 0xFF);
}

TEST_CASE("dither_frames clamps short durations to one frame")
{
    volatile std::size_t two = 20;
    CHECK(leds_effects::detail::dither_frames(two, 10) == 2);
    volatile std::size_t short_ms = 5;
    CHECK(leds_effects::detail::dither_frames(short_ms, 10) == 1);
}

TEST_CASE("deep_color fades in, holds and fades out")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    leds_effects::deep_color(
            strip, 0xFFFF, 0x0000, 0x0000, strip_t::RGB{ 0, 0, 0 }, 100, 100, 50, &stop);
    REQUIRE(strip.frame_count() >= 4);
    // Fade-in starts at the normal color.
    CHECK(strip.frame(0)[0].r == 0);
    // The last frame is all_off().
    const auto &last = strip.frame(strip.frame_count() - 1);
    CHECK(last[0].r == 0);
    CHECK(last[0].g == 0);
    CHECK(last[0].b == 0);
    // The fade-in reached the target red (calibrated 16-bit 0xFFFF >> 8).
    bool saw_peak = false;
    for (std::size_t i = 0; i < strip.frame_count(); ++i) {
        if (strip.frame(i)[0].r == 255)
            saw_peak = true;
    }
    CHECK(saw_peak);
}

TEST_CASE("deep_color with calibration scales the targets and dithers them")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    // The red primary is only half as bright as the reference, so a
    // half-scale red target needs full drive while green and blue keep
    // their fractional dithering.
    leds_effects::LedCalibration cal;
    cal.inv_matrix = {
        std::array<float, 3>{ 2.0f, 0.0f, 0.0f },
        std::array<float, 3>{ 0.0f, 1.0f, 0.0f },
        std::array<float, 3>{ 0.0f, 0.0f, 1.0f },
    };
    // A dither rate below the tick rate (so the cap is exercised) with a
    // fusion frequency giving two levels: 0x5555's low byte (0x55) rounds to
    // one of two phases, so the hold alternates between 85 and 86.
    leds_effects::DitherConfig dither;
    dither.dither_hz = configTICK_RATE_HZ / 2;
    dither.flicker_hz = configTICK_RATE_HZ / 4;
    REQUIRE(dither.levels() == 2);
    leds_effects::deep_color(strip,
                             0x8000,
                             0x5555,
                             0x5555,
                             strip_t::RGB{ 0, 0, 0 },
                             0,
                             0,
                             100,
                             &stop,
                             &cal,
                             dither);
    REQUIRE(strip.frame_count() >= 3);
    // The first hold frame carries the calibrated target: red reaches full
    // scale (0x8000 drives to 0xFFFF, whose low byte keeps it saturated),
    // green and blue are a third of it (21845 -> 85, below the phase dither
    // threshold on the first frame).
    const auto &hold = strip.frame(1);
    CHECK(hold[0].r == 255);
    CHECK(hold[0].g == 85);
    CHECK(hold[0].b == 85);
    // The next frame wraps the phase to 0 and shows the level above.
    const auto &dithered = strip.frame(2);
    CHECK(dithered[0].r == 255);
    CHECK(dithered[0].g == 86);
    CHECK(dithered[0].b == 86);
}

TEST_CASE("deep_color hold with full-scale green and blue reaches the 255 clamp")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    // A 0xFF00 target keeps 0xFF in the high byte, so the dithering clamps
    // green and blue at full scale.
    leds_effects::deep_color(
            strip, 0x0000, 0xFF00, 0xFF00, strip_t::RGB{ 0, 0, 0 }, 0, 0, 100, &stop);
    REQUIRE(strip.frame_count() >= 2);
    const auto &hold = strip.frame(1);
    CHECK(hold[0].g == 255);
    CHECK(hold[0].b == 255);
}

TEST_CASE("deep_color with a stop flag set before the first frame")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    stop.store(true);
    leds_effects::deep_color(
            strip, 0xFFFF, 0x0000, 0x0000, strip_t::RGB{ 0, 0, 0 }, 100, 100, 50, &stop);
    // The fade-in aborts immediately; the fade-out still runs (it does not
    // check the stop flag) and ends with all_off().
    REQUIRE(strip.frame_count() >= 1);
    CHECK(strip.frame(strip.frame_count() - 1) == strip_t::colors_t{});
}

TEST_CASE("deep_color without hold runs until the stop flag is set")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    // hold_ms == 0 with no stop flag loops forever in the hold phase; the
    // dither delay always blocks, so the stopper can preempt the effect. The
    // stopper fires while it is blocking, the hold exits and the fade-out
    // runs.
    arm_stopper(stop, 250);
    leds_effects::deep_color(
            strip, 0xFFFF, 0x0000, 0x0000, strip_t::RGB{ 0, 0, 0 }, 100, 100, 0, &stop);
    REQUIRE(strip.frame_count() >= 3);
    CHECK(strip.frame(strip.frame_count() - 1) == strip_t::colors_t{});
    CHECK(stop.load());
}

TEST_CASE("deep_color stops mid-hold and still fades out")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    // The dither delay always blocks for at least one tick, so a delayed
    // stopper task can preempt the effect. The stopper fires while the hold
    // phase (hold_ms > 0) is running, aborting it; the fade-out still runs
    // and ends with all_off().
    arm_stopper(stop, 250);
    leds_effects::deep_color(
            strip, 0xFFFF, 0x0000, 0x0000, strip_t::RGB{ 0, 0, 0 }, 100, 100, 1000, &stop);
    REQUIRE(strip.frame_count() >= 3);
    CHECK(strip.frame(strip.frame_count() - 1) == strip_t::colors_t{});
    CHECK(stop.load());
}

TEST_CASE("deep_color zero-length fade phases still render")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    // fade_in_ms = 0: the fade-in still renders one frame; hold_ms > 0 holds;
    // fade_out_ms = 0 renders one frame and all_off().
    leds_effects::deep_color(
            strip, 0xFFFF, 0x0000, 0x0000, strip_t::RGB{ 0, 0, 0 }, 0, 0, 10, &stop);
    REQUIRE(strip.frame_count() >= 3);
    CHECK(strip.frame(strip.frame_count() - 1) == strip_t::colors_t{});
}

TEST_CASE("deep_color short hold still renders a frame")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    // hold_ms below the dither period clamps to one hold frame, like the
    // fade phases do; the target color (0xFF00 -> 0xFF high byte) shows up.
    leds_effects::deep_color(
            strip, 0x0000, 0xFF00, 0xFF00, strip_t::RGB{ 0, 0, 0 }, 0, 0, 4, &stop);
    REQUIRE(strip.frame_count() >= 3);
    const auto &hold = strip.frame(1);
    CHECK(hold[0].g == 255);
    CHECK(hold[0].b == 255);
}

TEST_CASE("deep_color spreads the dither over the spatial group")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    // Two dither levels at the tick rate: a 10 ms frame period, so the 100 ms
    // fade-in and hold render ten frames each. Spread over two LEDs the four
    // phases subdivide each 8-bit step, so 0x8040 renders 0x80 or 0x81
    // depending on the LED's phase and the frame.
    leds_effects::DitherConfig dither;
    dither.dither_hz = configTICK_RATE_HZ;
    dither.flicker_hz = configTICK_RATE_HZ / 2;
    REQUIRE(dither.levels() == 2);
    REQUIRE(dither.effective_levels(2) == 4);
    leds_effects::deep_color<strip_t, 2>(
            strip, 0x8040, 0, 0, strip_t::RGB{ 0, 0, 0 }, 100, 0, 100, &stop, nullptr, dither);
    REQUIRE(strip.frame_count() == 22);
    // The first hold frame (after the ten fade-in frames) splits the phases
    // over the group: LED 0 and 2 are lit, LED 1 is not.
    CHECK(strip.frame(10)[0].r == 0x81);
    CHECK(strip.frame(10)[1].r == 0x80);
    CHECK(strip.frame(10)[2].r == 0x81);
    // The next frame advances the phase by a spatial slot and both phases
    // fall on the lower level.
    CHECK(strip.frame(11)[0].r == 0x80);
    CHECK(strip.frame(11)[1].r == 0x80);
    CHECK(strip.frame(11)[2].r == 0x80);
    // Then the phases wrap back and the first pattern returns.
    CHECK(strip.frame(12)[0].r == 0x81);
    CHECK(strip.frame(12)[1].r == 0x80);
    CHECK(strip.frame(12)[2].r == 0x81);
    // The fade-out ends with all_off().
    CHECK(strip.frame(strip.frame_count() - 1) == strip_t::colors_t{});
}

TEST_CASE("deep_color fades down from a brighter normal color")
{
    strip_t strip;
    std::atomic<bool> stop{ false };
    // The normal color is brighter than the calibrated target on every
    // channel. Interpolating in unsigned arithmetic used to wrap around,
    // rendering garbage frames; the fade must run smoothly from the normal
    // color down to the (dithered) target and back up.
    leds_effects::deep_color(
            strip, 0x4000, 0x4000, 0x4000, strip_t::RGB{ 255, 255, 255 }, 100, 100, 50, &stop);
    REQUIRE(strip.frame_count() >= 4);
    // First frame: the normal color, full white.
    CHECK(strip.frame(0)[0].r == 255);
    CHECK(strip.frame(0)[0].g == 255);
    CHECK(strip.frame(0)[0].b == 255);
    // The fade went down (0x4000 calibrated -> 0x40 high byte = 64) before
    // coming back: intermediate frames stay within the sane range instead of
    // wrapping.
    bool saw_down = false;
    for (std::size_t i = 1; i + 1 < strip.frame_count(); ++i) {
        const auto &f = strip.frame(i);
        CHECK(f[0].r <= 255);
        CHECK(f[0].g <= 255);
        CHECK(f[0].b <= 255);
        if (f[0].r < 255)
            saw_down = true;
    }
    CHECK(saw_down);
    CHECK(strip.frame(strip.frame_count() - 1) == strip_t::colors_t{});
}

TEST_CASE("effects tolerate a null stop flag")
{
    strip_t strip;
    // All effects accept a null stop flag (the default).
    leds_effects::rotating<strip_t, strip_t::RGB{ 1, 2, 3 }>(strip, 1, 1, nullptr);
    CHECK(strip.frame_count() == 4);
    strip.reset();
    leds_effects::smooth_transition(
            strip, strip_t::RGB{ 0, 0, 0 }, strip_t::RGB{ 255, 0, 0 }, 100, nullptr);
    CHECK(strip.frame_count() >= 2);
    strip.reset();
    leds_effects::deep_color(
            strip, 0xFFFF, 0x0000, 0x0000, strip_t::RGB{ 0, 0, 0 }, 100, 100, 10, nullptr);
    CHECK(strip.frame_count() >= 3);
}

// ---------------------------------------------------------------------------
// transition_task
// ---------------------------------------------------------------------------

#include <vector>

#include "cpptaskcreate.hpp"

namespace {

// Wait until the predicate holds or a 2 s deadline elapses (so a failure
// fails the test instead of hanging the suite).
template<typename Pred>
void
wait_until(Pred pred)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
    while (!pred() && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// Wait until the latest rendered frame satisfies `pred`, i.e. the posted
// target has been reached, or until a 2 s deadline elapses. The transition
// task keeps dithering the target afterwards (it never goes idle), so this
// replaces the old "frame history stopped growing" check.
template<typename Pred>
bool
wait_until_latest(const mock_strip<3> &strip, Pred pred)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
    for (;;) {
        const std::size_t n = strip.frame_count();
        if (n > 0 && pred(strip.frame(n - 1))) {
            return true;
        }
        if (xTaskGetTickCount() >= deadline) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// The temporal dithering of the transition alternates each channel between
// the two neighboring 8-bit levels of its 16-bit target (the low byte of
// 257 * v is v itself, so a channel shows v or v + 1, saturating at 255).
bool
is_dither_of(uint8_t rendered, uint8_t target)
{
    return rendered == target ||
           (target < 255 && rendered == target + 1);   // GCOVR_EXCL_BR_LINE
}

// A strip whose setters throw when the call counter reaches `throw_on_call`
// (a 0-based index; -1 never throws). It exercises the transition task's
// strip-fault recovery: a throw from `all_off()` at startup and a throw from
// `set_leds()` during a transition must not escape the task body.
struct throwing_strip
{
    struct RGB
    {
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;
    };

    using colors_t = std::array<RGB, 3>;

    static constexpr std::size_t num_leds()
    {
        return 3;
    }

    std::atomic<int> throw_on_call{ -1 };
    std::atomic<int> calls{ 0 };
    std::atomic<int> frames{ 0 };

    void maybe_throw()
    {
        const int call = calls.fetch_add(1);
        if (call == throw_on_call.load()) {
            throw std::runtime_error("strip fault");
        }
    }

    void set_leds(std::span<const RGB>)
    {
        maybe_throw();
        frames.fetch_add(1);
    }

    void set_all_leds(const RGB &)
    {
        maybe_throw();
        frames.fetch_add(1);
    }

    void all_off()
    {
        maybe_throw();
        frames.fetch_add(1);
    }
};

// The transition task never returns from its task body (it deletes itself at
// stop()), so the abort hook is never needed, but the mocks component pulls
// the --wrap=abort redirect in anyway (the task_queue/cpptaskcreate tests
// request it); the symbol is forced out of the mocks archive via the
// "-u,__wrap_abort" link option only in those components, so nothing else is
// needed here.

}   // namespace

TEST_CASE("transition_task starts with all LEDs off")
{
    strip_t strip;
    {
        leds_effects::transition_task<strip_t> task(strip, nullptr, 20, "tt_off", 8192);
        wait_until([&] { return strip.frame_count() >= 1; });
        REQUIRE(strip.frame_count() >= 1);
        CHECK(strip.frame(0) == strip_t::colors_t{});
    }
}

TEST_CASE("transition_task destructor turns the LEDs off and deletes the task")
{
    strip_t strip;
    {
        leds_effects::transition_task<strip_t> task(strip, nullptr, 20, "tt_dtor", 8192);
        CHECK(task.post_transition(0xFFFF, 0, 0));
        wait_until([&] { return strip.frame_count() > 0; });
    }
    // The destructor deletes the task and turns the LEDs off.
    wait_until([&] {
        return strip.frame_count() > 0 &&
               strip.frame(strip.frame_count() - 1) == strip_t::colors_t{};
    });
    const std::size_t last = strip.frame_count() - 1;
    CHECK(strip.frame(last) == strip_t::colors_t{});
    vTaskDelay(pdMS_TO_TICKS(50));
    // The deleted task renders nothing anymore.
    CHECK(strip.frame_count() == last + 1);
}

TEST_CASE("transition_task stops and can be started again")
{
    strip_t strip;
    leds_effects::transition_task<strip_t> task(strip, nullptr, 20, "tt_restart", 8192);
    wait_until([&] { return strip.frame_count() >= 1; });
    const std::size_t baseline = strip.frame_count();

    task.stop();
    vTaskDelay(pdMS_TO_TICKS(50));
    const std::size_t stopped = strip.frame_count();
    // Stopped: nothing renders. Posts are refused too.
    CHECK_FALSE(task.post_transition(0xFFFF, 0, 0));
    CHECK(strip.frame_count() == stopped);

    task.start();
    CHECK(task.post_transition(0xFFFF, 0, 0));
    wait_until([&] { return strip.frame_count() > stopped; });
    CHECK(strip.frame_count() > baseline);

    // A second stop()/start() round still works.
    task.stop();
    task.start();
    // A start() on an already-running task is a no-op.
    task.start();
    CHECK(task.post_transition(0, 0xFFFF, 0));
    wait_until([&] { return strip.frame_count() > stopped + 2; });
}

TEST_CASE("transition_task stops with a transition in flight")
{
    strip_t strip;
    leds_effects::transition_task<strip_t> task(strip, nullptr, 5000, "tt_inflight", 8192);
    wait_until([&] { return strip.frame_count() >= 1; });
    // A 5000 ms transition is still running when stop() arrives.
    CHECK(task.post_transition(0xFFFF, 0, 0));
    vTaskDelay(pdMS_TO_TICKS(50));
    task.stop();
    wait_until([&] { return strip.frame_count() > 0; });
    CHECK(strip.frame(strip.frame_count() - 1) == strip_t::colors_t{});
    // The deleted task renders nothing anymore, even though the transition
    // had almost 5 s of frames left.
    const std::size_t last = strip.frame_count();
    vTaskDelay(pdMS_TO_TICKS(100));
    CHECK(strip.frame_count() == last);

    // The object is still usable after the interrupted transition.
    task.start();
    CHECK(task.post_transition(0, 0xFFFF, 0, 10));
    wait_until([&] { return strip.frame_count() > last; });
}

TEST_CASE("transition_task slowly transitions to the posted color")
{
    strip_t strip;
    leds_effects::transition_task<strip_t> task(strip, nullptr, 100, "tt_fade", 8192);
    wait_until([&] { return strip.frame_count() >= 1; });
    strip.reset();
    CHECK(task.post_transition(0xFFFF, 0x8000, 0, 100));
    // The target (255, 128, 0) has zero low bytes on green/blue and a
    // saturated red, so it renders exactly (no dithering).
    REQUIRE(wait_until_latest(strip, [](const strip_t::colors_t &f) {
        return f[0].r == 255 && f[0].g == 128 && f[0].b == 0;
    }));
    const auto &last = strip.frame(strip.frame_count() - 1);
    CHECK(last[0].r == 255);
    CHECK(last[0].g == 128);
    CHECK(last[0].b == 0);
    // The transition really passed through intermediate frames (more than
    // just the initial all-off one).
    CHECK(strip.frame_count() > 2);
    // The first frame starts at the previous color (all LEDs off here).
    CHECK(strip.frame(0) == strip_t::colors_t{});
}

TEST_CASE("transition_task posts one color per LED")
{
    strip_t strip;
    leds_effects::transition_task<strip_t> task(strip, nullptr, 20, "tt_perled", 8192);
    wait_until([&] { return strip.frame_count() >= 1; });
    std::array<uint16_t, strip_t::num_leds()> r{};
    std::array<uint16_t, strip_t::num_leds()> g{};
    std::array<uint16_t, strip_t::num_leds()> b{};
    for (std::size_t i = 0; i < strip_t::num_leds(); ++i) {
        r[i] = static_cast<uint16_t>((i + 1) * 0x2000);
    }
    CHECK(task.post_transition(r, g, b, 10));
    // 0x2000 * (i + 1) >> 8 = 32 * (i + 1) per LED; every low byte is zero,
    // so the reached target renders exactly.
    REQUIRE(wait_until_latest(strip, [](const strip_t::colors_t &f) {
        return f[0].r == 32 && f[1].r == 64 && f[2].r == 96;
    }));
    const auto &last = strip.frame(strip.frame_count() - 1);
    for (std::size_t i = 0; i < strip_t::num_leds(); ++i) {
        CHECK(last[i].r == static_cast<uint8_t>(32 * (i + 1)));
        CHECK(last[i].g == 0);
        CHECK(last[i].b == 0);
    }

    // A per-LED post with the default transition time (transition_ms == 0)
    // uses the constructor's default.
    strip.reset();
    g.fill(0xFFFF);
    CHECK(task.post_transition(r, g, b));
    REQUIRE(wait_until_latest(
            strip, [](const strip_t::colors_t &f) { return f[0].g == 255 && f[2].g == 255; }));
    const auto &last2 = strip.frame(strip.frame_count() - 1);
    CHECK(last2[0].g == 255);
    CHECK(last2[2].g == 255);
}

TEST_CASE("transition_task ws2812-style wrappers use the default transition")
{
    strip_t strip;
    leds_effects::transition_task<strip_t> task(strip, nullptr, 20, "tt_wrap", 8192);
    wait_until([&] { return strip.frame_count() >= 1; });

    strip.reset();
    CHECK(task.set_all_leds({ 10, 20, 30 }));
    REQUIRE(wait_until_latest(strip, [](const strip_t::colors_t &f) {
        return is_dither_of(f[0].r, 10) && is_dither_of(f[0].g, 20) &&
               is_dither_of(f[0].b, 30);
    }));
    {
        // The 16-bit target of (10, 20, 30) dithers to (10/11, 20/21,
        // 30/31); the dithered target is still a valid dither.
        const auto &last = strip.frame(strip.frame_count() - 1);
        CHECK(is_dither_of(last[0].r, 10));
        CHECK(is_dither_of(last[0].g, 20));
        CHECK(is_dither_of(last[0].b, 30));
    }

    strip.reset();
    std::array<strip_t::RGB, strip_t::num_leds()> colors{};
    colors[0] = { 1, 2, 3 };
    colors[1] = { 4, 5, 6 };
    colors[2] = { 7, 8, 9 };
    CHECK(task.set_leds(colors));
    REQUIRE(wait_until_latest(strip, [](const strip_t::colors_t &f) {
        return is_dither_of(f[0].r, 1) && is_dither_of(f[1].g, 5) && is_dither_of(f[2].b, 9);
    }));
    {
        const auto &last = strip.frame(strip.frame_count() - 1);
        CHECK(is_dither_of(last[0].r, 1));
        CHECK(is_dither_of(last[1].g, 5));
        CHECK(is_dither_of(last[2].b, 9));
    }

    strip.reset();
    CHECK(task.all_on());
    REQUIRE(wait_until_latest(strip, [](const strip_t::colors_t &f) {
        return f[0].r == 255 && f[0].g == 255 && f[0].b == 255;
    }));
    {
        const auto &last = strip.frame(strip.frame_count() - 1);
        CHECK(last[0].r == 255);
        CHECK(last[0].g == 255);
        CHECK(last[0].b == 255);
    }

    strip.reset();
    CHECK(task.all_off());
    REQUIRE(wait_until_latest(
            strip, [](const strip_t::colors_t &f) { return f == strip_t::colors_t{}; }));
    CHECK(strip.frame(strip.frame_count() - 1) == strip_t::colors_t{});
}

TEST_CASE("transition_task applies the calibration to posted targets")
{
    strip_t strip;
    // The red primary is only half as bright as the reference: a 0x8000 red
    // target needs the full drive, i.e. a 0xFF high byte.
    leds_effects::LedCalibration cal;
    cal.inv_matrix = {
        std::array<float, 3>{ 2.0f, 0.0f, 0.0f },
        std::array<float, 3>{ 0.0f, 1.0f, 0.0f },
        std::array<float, 3>{ 0.0f, 0.0f, 1.0f },
    };
    leds_effects::transition_task<strip_t> task(strip, &cal, 20, "tt_cal", 8192);
    wait_until([&] { return strip.frame_count() >= 1; });
    CHECK(task.post_transition(0x8000, 0, 0, 10));
    // The calibrated target (full-scale red) renders exactly.
    REQUIRE(wait_until_latest(strip, [](const strip_t::colors_t &f) {
        return f[0].r == 255 && f[0].g == 0 && f[0].b == 0;
    }));
    const auto &last = strip.frame(strip.frame_count() - 1);
    CHECK(last[0].r == 255);
    CHECK(last[0].g == 0);
    CHECK(last[0].b == 0);
}

TEST_CASE("transition_task reports a full queue and keeps running")
{
    strip_t strip;
    // A long default transition keeps the task busy while the queue (depth
    // 1) fills up.
    leds_effects::transition_task<strip_t> task(strip, nullptr, 5000, "tt_full", 8192, 5, 1);
    wait_until([&] { return strip.frame_count() >= 1; });
    CHECK(task.post_transition(0xFFFF, 0, 0));
    // The first command is being rendered, so this one fills the queue...
    CHECK(task.post_transition(0, 0xFFFF, 0));
    // ...and this one is dropped.
    CHECK_FALSE(task.post_transition(0, 0, 0xFFFF));
}

TEST_CASE("transition_task survives a strip fault while turning the LEDs off")
{
    throwing_strip strip;
    // The very first all_off() (from the task body) throws. The task must log
    // it and still come up, so the constructor handshake does not hang.
    strip.throw_on_call.store(0);
    leds_effects::transition_task<throwing_strip> task(
            strip, nullptr, 20, "tt_off_fault", 8192);
    // The task is up and its later strip updates succeed (the call after the
    // faulty all_off no longer matches the throwing index).
    CHECK(task.post_transition(0xFFFF, 0, 0, 10));
    wait_until([&] { return strip.frames.load() > 0; });
    CHECK(strip.frames.load() > 0);
}

TEST_CASE("transition_task stops itself when a strip update throws")
{
    throwing_strip strip;
    leds_effects::transition_task<throwing_strip> task(strip, nullptr, 20, "tt_fault", 8192);
    // The startup all_off() is call 0; make the next strip update (the first
    // set_leds() of the transition) throw.
    strip.throw_on_call.store(1);
    CHECK(task.post_transition(0xFFFF, 0, 0, 10));
    // The fault brings the task down (the exception is thrown on the first
    // set_leds), so posts are refused afterwards.
    vTaskDelay(pdMS_TO_TICKS(50));
    CHECK_FALSE(task.post_transition(0, 0, 0, 10));

    // The object is still usable: start() re-creates the task (its calls no
    // longer match the throwing index) and a new color reaches the strip.
    task.start();
    CHECK(task.post_transition(0, 0xFFFF, 0, 10));
    const int before = strip.frames.load();
    wait_until([&] { return strip.frames.load() > before; });
    CHECK(strip.frames.load() > before);
}

TEST_CASE("transition_task drops queued transitions on stop")
{
    strip_t strip;
    // A 500 ms default transition keeps the task busy while the queue (depth
    // 4) fills up with two more commands.
    leds_effects::transition_task<strip_t> task(strip, nullptr, 500, "tt_drop", 8192, 5, 4);
    wait_until([&] { return strip.frame_count() >= 1; });
    CHECK(task.post_transition(0xFFFF, 0, 0));   // the (possibly in-flight) red
    CHECK(task.post_transition(0, 0xFFFF, 0));   // queued, must be dropped
    CHECK(task.post_transition(0, 0, 0xFFFF));   // queued, must be dropped
    vTaskDelay(pdMS_TO_TICKS(20));
    // stop() discards the transitions that are still queued instead of
    // rendering them: the green and blue targets never reach the strip, and
    // the end state is all-off.
    task.stop();
    for (std::size_t i = 0; i < strip.frame_count(); ++i) {
        CHECK(strip.frame(i)[0].g == 0);
        CHECK(strip.frame(i)[0].b == 0);
    }
    CHECK(strip.frame(strip.frame_count() - 1) == strip_t::colors_t{});
}

TEST_CASE("transition_task reports a zero-length transition in one frame")
{
    strip_t strip;
    leds_effects::transition_task<strip_t> task(strip, nullptr, 20, "tt_zero", 8192);
    wait_until([&] { return strip.frame_count() >= 1; });
    strip.reset();
    CHECK(task.post_transition(0xFFFF, 0, 0, 1));
    // The transition still ends at the target color (a saturated red
    // channel renders exactly).
    REQUIRE(wait_until_latest(strip,
                              [](const strip_t::colors_t &f) { return f[0].r == 255; }));
    CHECK(strip.frame(strip.frame_count() - 1)[0].r == 255);
}

TEST_CASE("transition_task keeps dithering the target until the next transition")
{
    strip_t strip;
    // Two dither levels (a dither rate below the tick rate, so the cap is
    // exercised) so the sub-8-bit low byte is actually rendered.
    leds_effects::DitherConfig dither;
    dither.dither_hz = configTICK_RATE_HZ / 2;
    dither.flicker_hz = configTICK_RATE_HZ / 4;
    REQUIRE(dither.levels() == 2);
    leds_effects::transition_task<strip_t> task(
            strip, nullptr, 20, "tt_dither", 8192, 5, 4, dither);
    wait_until([&] { return strip.frame_count() >= 1; });

    // 0xAA55 has a non-zero low byte on every channel, so it dithers between
    // 0xAA and 0xAB. Wait until the target is reached.
    CHECK(task.post_transition(0xAA55, 0xAA55, 0xAA55, 20));
    REQUIRE(wait_until_latest(strip, [](const strip_t::colors_t &f) {
        return is_dither_of(f[0].r, 0xAA) && is_dither_of(f[0].g, 0xAA) &&
               is_dither_of(f[0].b, 0xAA);
    }));

    // The task keeps rendering frames (dithering the target) after the
    // transition, instead of going idle.
    const std::size_t reached = strip.frame_count();
    vTaskDelay(pdMS_TO_TICKS(50));
    REQUIRE(strip.frame_count() > reached);
    {
        const auto &last = strip.frame(strip.frame_count() - 1);
        CHECK(is_dither_of(last[0].r, 0xAA));
        CHECK(is_dither_of(last[0].g, 0xAA));
        CHECK(is_dither_of(last[0].b, 0xAA));
    }

    // A transition posted while the task is dithering is picked up (and,
    // with 0x00FF, itself dithers between 0x00 and 0x01).
    CHECK(task.post_transition(0x00FF, 0x00FF, 0x00FF, 20));
    REQUIRE(wait_until_latest(strip, [](const strip_t::colors_t &f) {
        return is_dither_of(f[0].r, 0x00) && is_dither_of(f[0].g, 0x00) &&
               is_dither_of(f[0].b, 0x00);
    }));
    const auto &last = strip.frame(strip.frame_count() - 1);
    CHECK(is_dither_of(last[0].r, 0x00));
    CHECK(is_dither_of(last[0].g, 0x00));
    CHECK(is_dither_of(last[0].b, 0x00));

    // Idle for several frame periods: the task keeps rendering frames and its
    // queue polls time out (the stop sentinel only arrives later).
    const std::size_t holding = strip.frame_count();
    vTaskDelay(pdMS_TO_TICKS(200));
    CHECK(strip.frame_count() > holding);
}

TEST_CASE("transition_task spreads the dither over the spatial group")
{
    strip_t strip;
    // Two dither levels at the tick rate, spread over two LEDs: four phases
    // subdivide each 8-bit step. 0x8040 dithers between 0x80 and 0x81.
    leds_effects::DitherConfig dither;
    dither.dither_hz = configTICK_RATE_HZ;
    dither.flicker_hz = configTICK_RATE_HZ / 2;
    REQUIRE(dither.levels() == 2);
    REQUIRE(dither.effective_levels(2) == 4);
    leds_effects::transition_task<strip_t, 2> task(
            strip, nullptr, 20, "tt_spatial", 8192, 5, 4, dither);
    wait_until([&] { return strip.frame_count() >= 1; });

    CHECK(task.post_transition(0x8040, 0x8040, 0x8040, 20));
    REQUIRE(wait_until_latest(strip, [](const strip_t::colors_t &f) {
        return is_dither_of(f[0].r, 0x80) && is_dither_of(f[1].r, 0x80) &&
               is_dither_of(f[2].r, 0x80);
    }));
    // The idle dithering keeps rendering the spatially dithered target.
    const std::size_t reached = strip.frame_count();
    vTaskDelay(pdMS_TO_TICKS(50));
    REQUIRE(strip.frame_count() > reached);
    const auto &last = strip.frame(strip.frame_count() - 1);
    CHECK(is_dither_of(last[0].r, 0x80));
    CHECK(is_dither_of(last[0].g, 0x80));
    CHECK(is_dither_of(last[0].b, 0x80));
}

TEST_CASE("transition_task num_leds forwards to the strip type")
{
    CHECK((leds_effects::transition_task<strip_t>::num_leds() == strip_t::num_leds()));
    CHECK((leds_effects::transition_task<strip_t>::num_leds() == 3));
}

TEST_CASE("transition_task is neither copyable nor movable")
{
    static_assert(!std::is_copy_constructible_v<leds_effects::transition_task<strip_t>>);
    static_assert(!std::is_copy_assignable_v<leds_effects::transition_task<strip_t>>);
    static_assert(!std::is_move_constructible_v<leds_effects::transition_task<strip_t>>);
    static_assert(!std::is_move_assignable_v<leds_effects::transition_task<strip_t>>);
}

TEST_CASE("transition_task exception includes the message")
{
    leds_effects::transition_task<strip_t>::exception e("boom");
    CHECK(std::strcmp(e.what(), "transition_task: boom") == 0);
}

TEST_CASE("transition_task rejects the park sentinel as a transition time")
{
    strip_t strip;
    using task_t = leds_effects::transition_task<strip_t>;
    task_t task(strip, nullptr, 20, "tt_sentinel", 8192);
    wait_until([&] { return strip.frame_count() >= 1; });
    CHECK_THROWS_AS(task.post_transition(0xFFFF, 0, 0, task_t::park_sentinel_ms),
                    task_t::exception);
    std::array<uint16_t, strip_t::num_leds()> r{};
    std::array<uint16_t, strip_t::num_leds()> g{};
    std::array<uint16_t, strip_t::num_leds()> b{};
    CHECK_THROWS_AS(task.post_transition(r, g, b, task_t::park_sentinel_ms),
                    task_t::exception);
}

TEST_CASE("transition_task rejects the park sentinel as the default transition time")
{
    strip_t strip;
    using task_t = leds_effects::transition_task<strip_t>;
    CHECK_THROWS_AS(task_t(strip, nullptr, task_t::park_sentinel_ms, "tt_sentinel_def", 8192),
                    task_t::exception);
}

TEST_CASE("transition_task posts are refused before the task is running")
{
    strip_t strip;
    leds_effects::transition_task<strip_t> task(strip, nullptr, 20, "tt_stopped", 8192);
    // Wait for the constructor handshake, then stop.
    wait_until([&] { return strip.frame_count() >= 1; });
    task.stop();
    CHECK_FALSE(task.post_transition(0xFFFF, 0, 0));
    CHECK_FALSE(task.set_all_leds({ 1, 2, 3 }));
    CHECK_FALSE(task.all_on());
    CHECK_FALSE(task.all_off());
    std::array<strip_t::RGB, strip_t::num_leds()> colors{};
    CHECK_FALSE(task.set_leds(colors));
    // The per-LED overload refuses a stopped task too (without allocating the
    // command).
    std::array<uint16_t, strip_t::num_leds()> per_led{};
    CHECK_FALSE(task.post_transition(per_led, per_led, per_led));
}
