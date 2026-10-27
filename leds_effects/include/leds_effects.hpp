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
#include <exception>
#include <memory>
#include <mutex>
#include <span>
#include <type_traits>
#include <vector>

#include <atomic>

#include <esp_log.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "cpptaskcreate.hpp"

namespace leds_effects {

// Temporal-dithering parameters shared by every dithered effect
// (`smooth_transition()`, `deep_color()`, `sun_effect()`, `breathing()` and
// `transition_task`).
//
// The strip is refreshed once per dither frame at `dither_hz` frames per
// second. A frame cannot be shorter than one FreeRTOS tick, so the usable
// rate is `dither_hz` capped to `CONFIG_FREERTOS_HZ` (i.e.
// `configTICK_RATE_HZ`); asking for more only stretches every frame to a
// tick.
//
// Temporal dithering renders sub-8-bit color detail by cycling through a few
// frames whose average is the wanted level. The eye only fuses that cycle
// into a steady color if it repeats at least `flicker_hz` times per second
// (the flicker-fusion threshold, roughly 60-120 Hz for a human eye); a
// slower cycle is seen as flicker. Both frequencies therefore bound the
// number of dither levels, and with it the effective bit depth the strip can
// render: see `levels()` and `effective_bits()`.
//
// The dither phases can also be spread over a group of neighboring LEDs
// (spatial dithering): every LED of the group renders a different phase of
// the cycle (see the `spatial_leds` template argument of `detail::fade_all()`
// and its users), so the extra resolution comes from the eye fusing
// neighboring LEDs instead of from a longer temporal cycle. Spatial
// dithering therefore multiplies the renderable levels (see
// `effective_levels()`) without lowering the flicker-fusion frequency.
struct DitherConfig
{
    // Requested dither frame rate, in Hz.
    std::size_t dither_hz = 200;
    // Flicker-fusion frequency the dither cycle must reach, in Hz.
    std::size_t flicker_hz = 60;

    // Frame rate actually usable: `dither_hz` capped to the tick rate, with a
    // zero request promoted to 1 Hz so the frame period stays defined. The
    // promotion is folded into arithmetic to leave a single conditional.
    //
    // GCC clones these accessors per constant caller (and inlines them into
    // each effect), so the direction each clone takes is fixed at compile
    // time: the sides that no caller exercises cannot be covered per clone.
    constexpr std::size_t frame_hz() const
    {
        constexpr std::size_t tick_hz = configTICK_RATE_HZ;
        const std::size_t hz =
                dither_hz < tick_hz ? dither_hz : tick_hz;   // GCOVR_EXCL_BR_LINE
        return hz + static_cast<std::size_t>(hz == 0);       // GCOVR_EXCL_BR_LINE
    }

    // Frame period in FreeRTOS ticks, used for the per-frame delays. It is at
    // least one (and never zero, unlike `pdMS_TO_TICKS(frame_ms())` on a slow
    // tick), because the frame rate is capped to the tick rate.
    constexpr TickType_t frame_ticks() const
    {
        return static_cast<TickType_t>(configTICK_RATE_HZ / frame_hz());
    }

    // Frame period in milliseconds, used to count the frames of a fade (see
    // `detail::dither_frames()`). It is derived from `frame_ticks()` so the
    // frame count, the per-frame delay and the requested rate agree even for a
    // `dither_hz` that does not divide the tick rate: a frame really lasts
    // `frame_ticks()` ticks, which can be shorter than `1000 / dither_hz`. The
    // tick rate is at most 1000 Hz, so this never rounds down to zero.
    constexpr std::size_t frame_ms() const
    {
        return 1000 * static_cast<std::size_t>(frame_ticks()) /
               configTICK_RATE_HZ;   // GCOVR_EXCL_BR_LINE: see frame_hz()
    }

    // Number of dither frames (levels) per 8-bit step: the cycle may not
    // repeat slower than the flicker-fusion frequency, so at most
    // `frame_hz() / flicker_hz` frames fit in it. A zero fusion frequency
    // means "no flicker limit" and is treated as 1 Hz; a frequency above the
    // frame rate leaves a single level (which disables the dithering).
    constexpr std::size_t levels() const
    {
        const std::size_t flicker = flicker_hz + static_cast<std::size_t>(flicker_hz == 0);
        const std::size_t per_step = frame_hz() / flicker;
        return per_step + static_cast<std::size_t>(per_step == 0);
    }

    // Number of levels every 8-bit step is subdivided into when the dither
    // phases are also spread over `spatial_leds` LEDs: the temporal `levels()`
    // multiplied by the spatial ones. `spatial_leds` is at least one (one LED
    // is the plain temporal dithering); the count of LEDs in the group is
    // selected by the `spatial_leds` template argument of the effects, so the
    // default matches the temporal-only resolution.
    constexpr std::size_t effective_levels(std::size_t spatial_leds = 1) const
    {
        return levels() * spatial_leds;   // GCOVR_EXCL_BR_LINE: see frame_hz()
    }

    // Effective bit depth per channel: 8 plain bits plus one dithered bit per
    // doubling of `effective_levels(spatial_leds)` (every 8-bit step is
    // subdivided into that many values).
    constexpr unsigned effective_bits(std::size_t spatial_leds = 1) const
    {
        unsigned bits = 8;
        for (std::size_t l = effective_levels(spatial_leds); l >= 2; l /= 2)
            ++bits;
        return bits;
    }
};

namespace detail {

// `pdMS_TO_TICKS()` rounds down, so a delay shorter than one tick maps to
// zero. `vTaskDelay(0)` never blocks: a frame loop using it degrades into a
// busy loop that starves lower-priority tasks and can trip the task
// watchdog. Every per-frame delay is therefore clamped to at least one tick,
// so the effects stay safe on any tick rate (their frames just get longer).
constexpr TickType_t
delay_ticks(std::size_t ms)
{
    return pdMS_TO_TICKS(ms) > 0 ? static_cast<TickType_t>(pdMS_TO_TICKS(ms)) : 1;
}

// Sleep out the remainder of the current dither frame. `frame_start` is the
// tick count taken before the frame's LEDs were updated, so the time spent
// setting them is subtracted from the delay instead of being added on top of
// it. If updating the LEDs already took longer than a whole frame, the
// requested frame rate cannot be met (e.g. the strip is too long to refresh
// within one frame): the frame still yields one tick, so a consistently
// overrunning strip does not starve lower-priority tasks with a busy loop, and
// the overrun is logged only once per effect run (`overrun_logged` is the
// caller's per-run latch; logging it every frame would turn a slow strip into
// a log storm). The effect then runs as fast as the strip allows instead of
// silently falling behind.
inline void
frame_delay(TickType_t frame_start, TickType_t frame_ticks, bool &overrun_logged)
{
    static constexpr const char *TAG = "leds_effects";
    const TickType_t elapsed = xTaskGetTickCount() - frame_start;
    if (elapsed < frame_ticks) {
        vTaskDelay(frame_ticks - elapsed);
        return;
    }
    if (!overrun_logged) {
        ESP_LOGW(TAG,
                 "LED update overran its frame: %lu ticks elapsed for a %lu-tick frame",
                 static_cast<unsigned long>(elapsed),
                 static_cast<unsigned long>(frame_ticks));
        overrun_logged = true;
    }
    vTaskDelay(1);
}

// Ticks left of a `frame_ticks` frame that started `elapsed` ticks ago, at
// least one so a blocking wait always yields even when the frame already
// overran (a plain unsigned subtraction would wrap around and block for
// almost forever).
constexpr TickType_t
frame_remaining(TickType_t elapsed, TickType_t frame_ticks)
{
    return elapsed < frame_ticks ? frame_ticks - elapsed : 1;   // GCOVR_EXCL_BR_LINE
}

// Number of dither frames that cover `ms` at a `frame_ms` frame period: the
// division rounds up so the rendered frames fill the requested time instead of
// stopping a frame short, and the result is at least one (so a zero or
// sub-frame duration still renders a single frame).
constexpr std::size_t
dither_frames(std::size_t ms, std::size_t frame_ms)
{
    // GCC clones this helper per constant caller, so the direction each clone
    // takes is fixed at compile time.
    const std::size_t frames =
            ms / frame_ms + (ms % frame_ms != 0 ? 1 : 0);   // GCOVR_EXCL_BR_LINE
    return frames > 0 ? frames : 1;                         // GCOVR_EXCL_BR_LINE
}

// Smoothstep ease of a fade position in [0, 1].
constexpr float
smoothstep(float t)
{
    return t * t * (3.0f - 2.0f * t);
}

// Normalized position of frame `frame` of `frames`: the first frame is 0, the
// last is 1 and a single frame is 1.
constexpr float
fade_position(std::size_t frame, std::size_t frames)
{
    // GCC clones this helper per inlined caller, so the side a caller with a
    // constant frame count never exercises cannot be covered per clone.
    if (frames > 1)   // GCOVR_EXCL_BR_LINE
        return static_cast<float>(frame) / static_cast<float>(frames - 1);
    return 1.0f;
}

// Interpolate between two 16-bit channel values, rounding to nearest. The
// float arithmetic avoids the unsigned wrap-around of an integer lerp when
// `from` is brighter than `to`.
inline uint32_t
lerp16(uint32_t from, uint32_t to, float t)
{
    return static_cast<uint32_t>(static_cast<float>(from) +
                                 (static_cast<float>(to) - static_cast<float>(from)) * t +
                                 0.5f);
}

// Temporal dithering of a 16-bit channel down to 8 bits with `levels` dither
// phases: each frame shows either the high byte or (while below full scale)
// the next level up, in the proportion of the channel's low byte rounded to
// the nearest phase, so a `levels`-frame cycle averages out to the low byte.
// `levels == 1` disables the dithering and keeps the high byte alone.
inline uint8_t
dither16(uint32_t value16, uint32_t frame, std::size_t levels)
{
    const uint32_t base = value16 >> 8;
    if (levels > 1 && base < 255) {
        const uint32_t frac = ((value16 & 0xFF) * levels + 128) >> 8;
        if ((frame % levels) < frac)
            return static_cast<uint8_t>(base + 1);
    }
    return static_cast<uint8_t>(base);
}

// Dither phase of LED `led` within the repeating spatial group of
// `spatial_leds` LEDs, at temporal frame `frame`: the temporal phase advances
// by one spatial slot per frame and the LED's index within the group offsets
// it, so the `levels * spatial_leds` phases are covered over a `levels`-frame
// temporal cycle. `spatial_leds == 1` reduces to the temporal phase
// `frame % levels`. The result is always a valid phase for a `levels *
// spatial_leds`-level `dither16()` call.
constexpr std::size_t
dither_phase(uint32_t frame, std::size_t levels, std::size_t spatial_leds, std::size_t led)
{
    return (static_cast<std::size_t>(frame) * spatial_leds + led) % (levels * spatial_leds);
}

// A per-LED color in the 16-bit internal space (three channel values).
using channel3 = std::array<uint32_t, 3>;

// Build a `channel3` from three already-16-bit channel values.
constexpr channel3
rgb16(uint32_t r, uint32_t g, uint32_t b)
{
    return { r, g, b };
}

// Widen an 8-bit color to the 16-bit internal space (the same ×257 scaling
// deep_color() uses).
template<typename RGB>
constexpr channel3
widen(const RGB &color)
{
    return rgb16(static_cast<uint32_t>(color.r) * 257U,
                 static_cast<uint32_t>(color.g) * 257U,
                 static_cast<uint32_t>(color.b) * 257U);
}

// Advance a temporal dither frame counter by one, wrapping it at the end of
// one full dither cycle (`levels * spatial_leds` frames). Wrapping at the
// cycle length (rather than letting the `uint32_t` counter overflow) keeps
// `dither16()` and `dither_phase()` from skipping a phase: `UINT32_MAX + 1` is
// not a multiple of every cycle length, so an overflowing counter would make
// the dither pattern jump.
constexpr uint32_t
next_dither_frame(uint32_t frame, std::size_t levels, std::size_t spatial_leds)
{
    return static_cast<uint32_t>((static_cast<std::size_t>(frame) + 1) %
                                 (levels * spatial_leds));
}

// Type of the caller-provided scratch buffer of the dithered helpers: a span
// of exactly `T::num_leds()` colors when the frame is dithered LED by LED
// (`use_scratch`), and a plain dynamic span when the buffer is unused (the
// uniform `spatial_leds == 1` fast paths). Encoding the required size in the
// span's extent lets the helpers reject a wrong-sized buffer at compile time
// (see `is_full_frame_v`) instead of trusting a runtime `size()`.
template<typename T, bool use_scratch>
using scratch_span =
        std::span<typename T::RGB, use_scratch ? T::num_leds() : std::dynamic_extent>;

// Whether `Scratch` is a full strip frame the spatial dithering can fill in
// place: a `std::array` of `T::num_leds()` colors or the fixed-extent span of
// that many colors. A dynamic span (e.g. the one a `std::vector` converts to)
// is rejected: unlike the fixed-extent types its size is only a runtime
// precondition of the conversion, so a wrong-sized buffer would be a silent
// out-of-bounds write in a release build rather than a compile error.
template<typename T, typename Scratch>
inline constexpr bool is_full_frame_v =
        std::is_same_v<Scratch, scratch_span<T, true>> ||
        std::is_same_v<Scratch, std::array<typename T::RGB, T::num_leds()>>;

// Render a uniform 16-bit `r`/`g`/`b` color on the whole strip with temporal
// and spatial dithering: with `spatial_leds == 1` every LED shows the same
// `levels`-phase temporal dither (via set_all_leds()), otherwise the strip is
// written LED by LED, each showing the dither phase of its slot in the
// repeating spatial group of `spatial_leds` LEDs (via set_leds()).
//
// The spatial path fills `scratch` (a `std::array` or fixed-extent span of
// exactly `T::num_leds()` colors, checked by the `static_assert` below) with
// the frame before writing it, so the strip-sized array is heap-allocated by
// the caller with whatever allocator it needs instead of landing on the task
// stack; the `spatial_leds == 1` path ignores it.
template<typename T, std::size_t spatial_leds, typename Scratch>
inline void
dither_all(T &strip,
           uint32_t r,
           uint32_t g,
           uint32_t b,
           uint32_t frame,
           std::size_t levels,
           Scratch &&scratch)
{
    if constexpr (spatial_leds == 1) {
        (void) scratch;
        strip.set_all_leds({ dither16(r, frame, levels),
                             dither16(g, frame, levels),
                             dither16(b, frame, levels) });
    } else {
        static_assert(is_full_frame_v<T, std::remove_cvref_t<Scratch>>,
                      "the scratch buffer must be a std::array or a fixed-extent span "
                      "of exactly T::num_leds() colors");
        const std::size_t total = levels * spatial_leds;
        for (std::size_t i = 0; i < T::num_leds(); ++i) {
            const std::size_t phase =
                    dither_phase(frame, levels, spatial_leds, i % spatial_leds);
            scratch[i] = { dither16(r, phase, total),
                           dither16(g, phase, total),
                           dither16(b, phase, total) };
        }
        strip.set_leds(scratch);
    }
}

// Fill `scratch` with one dithered fade frame and write it to the strip: every
// LED is interpolated from its `from` color to its 16-bit `to` target by `t`
// and dithered with the phase of its slot in the spatial group. `from(led)` and
// `to(led)` return the 16-bit r/g/b of `led`. With `uniform` the whole strip
// shows a single color, so the `spatial_leds == 1` case uses set_all_leds() and
// leaves `scratch` untouched. Otherwise `scratch` must be a `std::array` or
// fixed-extent span of exactly `T::num_leds()` colors (checked by the
// `static_assert` below).
template<typename T,
         std::size_t spatial_leds,
         bool uniform,
         typename FromFn,
         typename ToFn,
         typename Scratch>
inline void
dither_fade(T &strip,
            FromFn from,
            ToFn to,
            float t,
            uint32_t frame,
            std::size_t levels,
            Scratch &&scratch)
{
    if constexpr (uniform && spatial_leds == 1) {
        (void) scratch;
        const channel3 c = from(0);
        const channel3 d = to(0);
        strip.set_all_leds({ dither16(lerp16(c[0], d[0], t), frame, levels),
                             dither16(lerp16(c[1], d[1], t), frame, levels),
                             dither16(lerp16(c[2], d[2], t), frame, levels) });
    } else {
        static_assert(is_full_frame_v<T, std::remove_cvref_t<Scratch>>,
                      "the scratch buffer must be a std::array or a fixed-extent span "
                      "of exactly T::num_leds() colors");
        const std::size_t total = levels * spatial_leds;
        for (std::size_t i = 0; i < T::num_leds(); ++i) {
            const channel3 c = from(i);
            const channel3 d = to(i);
            const std::size_t phase =
                    dither_phase(frame, levels, spatial_leds, i % spatial_leds);
            scratch[i] = { dither16(lerp16(c[0], d[0], t), phase, total),
                           dither16(lerp16(c[1], d[1], t), phase, total),
                           dither16(lerp16(c[2], d[2], t), phase, total) };
        }
        strip.set_leds(scratch);
    }
}

// The shared dithered fade engine behind `fade_all()` (one color for the whole
// strip) and `transition_task`'s transitions (one color per LED). It renders
// `dither_frames(duration_ms, dither.frame_ms())` frames, interpolating from
// the per-LED `from` color to the per-LED 16-bit `to` target (or, with
// `reverse`, from the target back to `from`), and advances the shared `frame`
// counter so successive fades keep a continuous dither pattern. With `uniform`
// the strip shows the same color everywhere, which also enables the
// `spatial_leds == 1` set_all_leds() fast path in dither_fade().
//
// `exact_ends` renders the endpoints exactly instead of as one of their two
// dither phases: the first frame shows `from`, the last shows the target (a
// single frame shows the target, like a zero-length fade). It is only used by
// the uniform fades (the per-LED transition passes `false`); `reverse` must
// then be false. Returns false (without rendering the remaining frames) once
// the stop flag is set.
//
// `scratch` must be a `std::array` or fixed-extent span of exactly
// `T::num_leds()` colors whenever the fade dithers LED by LED (a spatial group
// larger than one, or a non-uniform fade); the uniform `spatial_leds == 1`
// fade leaves it untouched. `dither_fade()` `static_assert`s the required size
// (see `is_full_frame_v`).
template<typename T,
         std::size_t spatial_leds,
         bool uniform,
         typename FromFn,
         typename ToFn,
         typename Scratch>
inline bool
fade_run(T &strip,
         FromFn from,
         ToFn to,
         std::size_t duration_ms,
         const std::atomic<bool> *stop,
         uint32_t &frame,
         bool reverse,
         bool exact_ends,
         const DitherConfig &dither,
         Scratch &&scratch)
{
    const std::size_t frame_ms = dither.frame_ms();
    const TickType_t frame_ticks = dither.frame_ticks();
    const std::size_t levels = dither.levels();
    const std::size_t frames = dither_frames(duration_ms, frame_ms);
    bool overrun_logged = false;
    for (std::size_t f = 0; f < frames; ++f) {
        if (stop && stop->load())
            return false;
        const TickType_t frame_start = xTaskGetTickCount();
        const bool last = f + 1 == frames;
        if (exact_ends && (last || f == 0)) {
            const channel3 c = f == 0 && !last ? from(0) : to(0);
            strip.set_all_leds({ static_cast<uint8_t>(c[0] >> 8),
                                 static_cast<uint8_t>(c[1] >> 8),
                                 static_cast<uint8_t>(c[2] >> 8) });
        } else {
            float t = smoothstep(fade_position(f, frames));
            if (reverse)
                t = 1.0f - t;
            dither_fade<T, spatial_leds, uniform>(strip, from, to, t, frame, levels, scratch);
        }
        frame_delay(frame_start, frame_ticks, overrun_logged);
        frame = next_dither_frame(frame, levels, spatial_leds);
    }
    return true;
}

// Total number of frames rendered by a walk effect: `positions` frames per
// repetition, unbounded (until stopped) for negative repetitions. Keeping
// this guard in one place makes the `repetitions < 0` sentinel safe to use.
constexpr std::size_t
total_frames(int repetitions, std::size_t positions)
{
    return repetitions < 0 ? SIZE_MAX : static_cast<std::size_t>(repetitions) * positions;
}

// Build a walk frame on the fly instead of materializing an N×N table of
// pre-rendered frames: only `positions` distinct frames exist, and a stack
// table of N² colors (12 KB for a 64-LED strip) would overflow typical
// task stacks.
template<typename T>
constexpr std::array<typename T::RGB, T::num_leds()>
walk_frame(std::size_t index, const typename T::RGB &color)
{
    std::array<typename T::RGB, T::num_leds()> leds{};
    leds[index] = color;
    return leds;
}

// Wait out `wait_ms` in 10 ms slices, giving up (returning false) as soon as
// the stop flag is set. The final slice is shortened to the remainder, so
// the wait never overshoots the requested duration.
constexpr std::size_t
wait_slice(std::size_t wait_ms, std::size_t waited_ms)
{
    return wait_ms - waited_ms < 10 ? wait_ms - waited_ms : 10;
}

inline bool
wait_remaining(std::size_t wait_ms, const std::atomic<bool> *stop)
{
    std::size_t waited = 0;
    while (waited < wait_ms) {
        if (stop && stop->load())
            return false;
        const std::size_t slice = wait_slice(wait_ms, waited);
        vTaskDelay(delay_ticks(slice));   // GCOVR_EXCL_BR_LINE
        waited += slice;
    }
    return true;
}

#if defined(__clang__)
// std::pow is not constant-evaluable under the esp-clang toolchain: its
// libstdc++ routes pow(float, float) through __builtin_powf, which clang does
// not support in constant expressions. drive() uses this fallback when it is
// evaluated at compile time; the runtime path keeps std::pow unchanged. GCC
// handles constexpr std::pow natively, so the fallback is not compiled there.
constexpr float
pow(float base, float exp)
{
    if (exp == 0.0f)
        return 1.0f;
    if (base == 0.0f || base == 1.0f)
        return base;
    // A negative, NaN or infinite base would spin forever in the
    // normalization loops below. drive() never produces one (its inputs are
    // clamped to [0, 1] and its calibration data is guarded), so fail safe
    // to black instead of hanging.
    if (!std::isfinite(base) || base < 0.0f)
        return 0.0f;

    // base = m * 2^e with m in [0.5, 1), so log2(base) = e + log2(m).
    int e = 0;
    float m = base;
    while (m < 0.5f) {
        m *= 2.0f;
        --e;
    }
    while (m > 1.0f) {
        m *= 0.5f;
        ++e;
    }

    // log2(m) = 2 / ln(2) * atanh((m - 1) / (m + 1)) via the atanh series.
    const float z = (m - 1.0f) / (m + 1.0f);
    const float z2 = z * z;
    float zk = z;
    float sum = 0.0f;
    for (int k = 1; k <= 23; k += 2) {
        sum += zk / static_cast<float>(k);
        zk *= z2;
    }
    constexpr float ln2 = 0.69314718055994530941723212145818f;
    const float y = exp * (static_cast<float>(e) + 2.0f * sum / ln2);

    // 2^y = 2^floor(y) * 2^frac(y), with 2^frac via the exp series.
    int n = static_cast<int>(y);
    if (static_cast<float>(n) > y)
        --n;
    const float f = y - static_cast<float>(n);
    float fk = 1.0f;
    float fact = 1.0f;
    float acc = 1.0f;
    const float fln2 = f * ln2;
    for (int k = 1; k <= 20; ++k) {
        fk *= fln2;
        fact *= static_cast<float>(k);
        acc += fk / fact;
    }
    if (n >= 0) {
        for (int i = 0; i < n; ++i)
            acc *= 2.0f;
    } else {
        for (int i = 0; i < -n; ++i)
            acc *= 0.5f;
    }
    return acc;
}
#endif

// Common loop of the walk effects (rotating/bouncing, template-value and
// runtime-color variants): render `positions` frames per repetition (or run
// until stopped for negative repetitions), then turn everything off. The
// `frame` callback maps a frame index to the LEDs to display.
template<typename T, typename Frame>
inline void
walk(T &strip,
     Frame frame,
     const std::size_t interval_ms,
     const int repetitions,
     const std::size_t positions,
     const std::atomic<bool> *stop)
{
    for (std::size_t i = 0;; ++i) {
        if (repetitions >= 0 && i >= detail::total_frames(repetitions, positions))
            break;
        if (stop && stop->load())
            return;
        strip.set_leds(frame(i));
        vTaskDelay(delay_ticks(interval_ms));
    }
    strip.all_off();
}

// Blocking, temporally dithered fade of the whole strip: the shared transition
// engine behind `smooth_transition()`, `deep_color()` and `transition_task`'s
// fade. It renders `dither_frames(duration_ms, dither.frame_ms())` frames,
// interpolating from the 8-bit `from` color to the 16-bit target (or, with
// `reverse`, from the target back to `from`); `frame` is advanced across calls
// so successive fades keep a continuous dither pattern. The effective dither
// bit depth (and thus the flicker behaviour) is set by `dither`.
//
// `spatial_leds` is the number of LEDs the dither phases are spread over (see
// `dither_all()`): one LED is the plain temporal dithering, a larger group
// renders `levels() * spatial_leds` levels (see
// `DitherConfig::effective_levels()`) without shortening the temporal cycle.
// It must not exceed the strip's LED count.
//
// With `exact_ends` the endpoints of a forward fade are rendered exactly
// instead of as one of their two dither phases: the first frame shows `from`
// and the last shows the target (a single frame shows the target, like a
// zero-length fade). `reverse` must then be false. Returns false (without
// rendering) once the stop flag is set.
//
// `scratch` must be a `std::array` or fixed-extent span of exactly
// `T::num_leds()` colors when `spatial_leds > 1` (the spatial dithering writes
// the whole frame there before setting the strip) and is unused otherwise; see
// `is_full_frame_v`.
template<typename T, std::size_t spatial_leds = 1, typename Scratch>
inline bool
fade_all(T &strip,
         const typename T::RGB &from,
         uint32_t to_r,
         uint32_t to_g,
         uint32_t to_b,
         std::size_t duration_ms,
         const std::atomic<bool> *stop,
         uint32_t &frame,
         bool reverse,
         bool exact_ends,
         const DitherConfig &dither,
         Scratch &&scratch)
{
    static_assert(spatial_leds >= 1, "spatial_leds must be at least one LED");
    static_assert(spatial_leds <= T::num_leds(),
                  "spatial_leds must not exceed the number of LEDs");
    const uint32_t from_r = static_cast<uint32_t>(from.r) * 257U;
    const uint32_t from_g = static_cast<uint32_t>(from.g) * 257U;
    const uint32_t from_b = static_cast<uint32_t>(from.b) * 257U;
    return fade_run<T, spatial_leds, true>(
            strip,
            [=](std::size_t) { return channel3{ from_r, from_g, from_b }; },
            [=](std::size_t) { return channel3{ to_r, to_g, to_b }; },
            duration_ms,
            stop,
            frame,
            reverse,
            exact_ends,
            dither,
            scratch);
}

// The phases of deep_color(): fade in from `normal_color`, hold the target for
// `hold_ms` (or until the stop flag for a zero hold), fade back out and turn
// the strip off. `scratch` is the caller-owned per-LED buffer for the spatial
// dithering (empty when `spatial_leds == 1`; see `scratch_span`). It is a
// separate function from deep_color() so that deep_color() can own the scratch
// buffer's exception cleanup in one place without spreading it over (and
// hiding the coverage of) the phase branches below.
template<typename T, std::size_t spatial_leds>
inline void
deep_color_run(T &strip,
               uint32_t tr,
               uint32_t tg,
               uint32_t tb,
               const typename T::RGB &normal_color,
               std::size_t fade_in_ms,
               std::size_t fade_out_ms,
               std::size_t hold_ms,
               const std::atomic<bool> *stop,
               const DitherConfig &dither,
               scratch_span<T, spatial_leds != 1> scratch)
{
    const std::size_t frame_ms = dither.frame_ms();
    const TickType_t frame_ticks = dither.frame_ticks();
    const std::size_t levels = dither.levels();

    uint32_t frame = 0;
    bool overrun_logged = false;

    // Fade-in phase: it aborts on the stop flag, which also skips the hold.
    const bool faded_in = fade_all<T, spatial_leds>(strip,
                                                    normal_color,
                                                    tr,
                                                    tg,
                                                    tb,
                                                    fade_in_ms,
                                                    stop,
                                                    frame,
                                                    /* reverse = */ false,
                                                    /* exact_ends = */ false,
                                                    dither,
                                                    scratch);

    // Hold phase
    if (faded_in) {
        if (hold_ms > 0) {
            const std::size_t hold_frames = dither_frames(hold_ms, frame_ms);
            for (std::size_t h = 0; h < hold_frames; ++h) {
                if (stop && stop->load())
                    break;
                const TickType_t frame_start = xTaskGetTickCount();
                dither_all<T, spatial_leds>(strip, tr, tg, tb, frame, levels, scratch);
                frame_delay(frame_start, frame_ticks, overrun_logged);
                frame = next_dither_frame(frame, levels, spatial_leds);
            }
        } else {
            // The loop only exits through the stop flag; with a null stop
            // flag and hold_ms == 0 it would never terminate, so the
            // stop == nullptr edge is excluded.
            while (!(stop && stop->load())) {   // GCOVR_EXCL_BR_LINE
                // The frame counter is wrapped at the end of the dither cycle
                // (next_dither_frame()), so it never overflows and the dither
                // phases stay continuous.
                const TickType_t frame_start = xTaskGetTickCount();
                dither_all<T, spatial_leds>(
                        strip, tr, tg, tb, frame, levels, scratch);   // GCOVR_EXCL_BR_LINE
                frame_delay(frame_start, frame_ticks, overrun_logged);
                frame = next_dither_frame(frame, levels, spatial_leds);
            }
        }
    }

    // Fade-out phase: ignores the stop flag (the effect always ends with
    // all_off()); the dither frame counter continues from the earlier phases.
    // It always fades the full way back from the target to `normal_color`, so
    // when the stop flag aborted the fade-in (leaving the strip somewhere
    // between the two) the first fade-out frame jumps to the target before
    // fading out. That is a <= one fade-in worth of visible step only on the
    // interrupted path.
    fade_all<T, spatial_leds>(strip,
                              normal_color,
                              tr,
                              tg,
                              tb,
                              fade_out_ms,
                              /* stop = */ nullptr,
                              frame,
                              /* reverse = */ true,
                              /* exact_ends = */ false,
                              dither,
                              scratch);
    strip.all_off();
}

}   // namespace detail

// Walk a single lit LED around the strip, one frame per LED. `repetitions`
// counts whole walks; a negative value runs until the stop flag is set. The
// strip requires at least one LED. On normal completion the LEDs are turned
// off; when the stop flag aborts the walk, the strip is left in its last
// state (the last rendered frame).
template<typename T, typename T::RGB value>
inline void
rotating(T &strip,
         const std::size_t interval_ms,
         const int repetitions = 1,
         const std::atomic<bool> *stop = nullptr)
{
    static_assert(T::num_leds() >= 1, "rotating needs at least one LED");
    constexpr std::size_t n = T::num_leds();
    detail::walk<T>(   // GCOVR_EXCL_BR_LINE
            strip,
            /* frame = */ [](std::size_t i) { return detail::walk_frame<T>(i % n, value); },
            interval_ms,
            repetitions,
            /* positions = */ n,
            stop);
}

// Runtime-color variant of rotating().
template<typename T>
inline void
rotating(T &strip,
         const typename T::RGB &value,
         const std::size_t interval_ms,
         const int repetitions = 1,
         const std::atomic<bool> *stop = nullptr)
{
    static_assert(T::num_leds() >= 1, "rotating needs at least one LED");
    constexpr std::size_t n = T::num_leds();
    detail::walk<T>(   // GCOVR_EXCL_BR_LINE
            strip,
            /* frame = */
            [&value](std::size_t i) { return detail::walk_frame<T>(i % n, value); },
            interval_ms,
            repetitions,
            /* positions = */ n,
            stop);
}

// Bounce a single lit LED back and forth across the strip, one frame per
// position. `repetitions` counts whole bounces; a negative value runs until
// the stop flag is set. The strip requires at least two LEDs (with one LED
// there is nothing to bounce between). Like rotating(), a normal completion
// turns the LEDs off, while a stop-flag abort leaves the strip in its last
// state.
template<typename T, typename T::RGB value>
inline void
bouncing(T &strip,
         const std::size_t interval_ms,
         const int repetitions = 1,
         const std::atomic<bool> *stop = nullptr)
{
    static_assert(T::num_leds() >= 2, "bouncing needs at least two LEDs");
    constexpr std::size_t n = T::num_leds();
    constexpr std::size_t positions = 2 * (n - 1);
    detail::walk<T>(   // GCOVR_EXCL_BR_LINE
            strip,
            /* frame = */
            [](std::size_t i) {   // GCOVR_EXCL_START
                const std::size_t pos = i % positions;
                return detail::walk_frame<T>(pos < n ? pos : positions - pos, value);
            },   // GCOVR_EXCL_STOP
            interval_ms,
            repetitions,
            positions,
            stop);
}

// Runtime-color variant of bouncing().
template<typename T>
inline void
bouncing(T &strip,
         const typename T::RGB &value,
         const std::size_t interval_ms,
         const int repetitions = 1,
         const std::atomic<bool> *stop = nullptr)
{
    static_assert(T::num_leds() >= 2, "bouncing needs at least two LEDs");
    constexpr std::size_t n = T::num_leds();
    constexpr std::size_t positions = 2 * (n - 1);
    detail::walk<T>(   // GCOVR_EXCL_BR_LINE
            strip,
            /* frame = */
            [&value](std::size_t i) {
                const std::size_t pos = i % positions;
                return detail::walk_frame<T>(pos < n ? pos : positions - pos, value);
            },
            interval_ms,
            repetitions,
            positions,
            stop);
}

// Fade from `start` to `end` in `duration_ms` using temporal dithering: the
// strip alternates between the two nearest colors of the 16-bit target, so
// intermediate shades look smoother than a plain 8-bit ramp. It is the shared
// transition engine (see detail::fade_all()) that `transition_task` also uses,
// run synchronously in the calling task; the final frame shows `end` exactly.
// `dither` selects the frame rate and the flicker-fusion frequency, and with
// them the effective bit depth (see DitherConfig); its frame rate is capped to
// the tick rate, so on a slow tick the transition is rendered with fewer,
// longer frames. `spatial_leds` spreads the dither phases over that many
// LEDs, multiplying the renderable levels; see detail::fade_all(). On normal
// completion it leaves the strip at `end`; if the stop flag aborts the fade,
// the strip is left mid-fade at the last rendered frame.
//
// `Allocator` (whose `value_type` must be `T::RGB`) allocates the per-LED
// scratch buffer used by the spatial dithering; the default `spatial_leds == 1`
// fade does not need it and does not allocate.
template<typename T,
         std::size_t spatial_leds = 1,
         typename Allocator = std::allocator<typename T::RGB>>
inline void
smooth_transition(T &strip,
                  const typename T::RGB &start,
                  const typename T::RGB &end,
                  const std::size_t duration_ms,
                  const std::atomic<bool> *stop = nullptr,
                  const DitherConfig &dither = DitherConfig{})
{
    // GCOVR_EXCL_BR_START: the never-taken std::bad_alloc unwinding edges of
    // the scratch buffer (the only owned object here; the fade itself has no
    // runtime branches).
    std::vector<typename T::RGB, Allocator> scratch;
    if constexpr (spatial_leds > 1)
        scratch.resize(T::num_leds());
    uint32_t frame = 0;
    detail::fade_all<T, spatial_leds>(strip,
                                      start,
                                      static_cast<uint32_t>(end.r) * 257U,
                                      static_cast<uint32_t>(end.g) * 257U,
                                      static_cast<uint32_t>(end.b) * 257U,
                                      duration_ms,
                                      stop,
                                      frame,
                                      /* reverse = */ false,
                                      /* exact_ends = */ true,
                                      dither,
                                      detail::scratch_span<T, (spatial_leds != 1)>(scratch));
    // GCOVR_EXCL_BR_STOP
}

struct LedCalibration
{
    struct Drive
    {
        uint16_t r;
        uint16_t g;
        uint16_t b;
    };

    // Inverse of the 3×3 LED mixing matrix M, stored row-major. M's columns
    // are the measured tristimulus values (CIE XYZ, or any linear
    // trichromatic space) of the red, green and blue primaries at full
    // drive, minus the black level. `drive()` multiplies a target color by
    // this inverse to obtain the drive values that reproduce it on the
    // calibrated LED. The default is the identity matrix, i.e. the reference
    // space is the LED's own color space and targets map straight to drive.
    std::array<std::array<float, 3>, 3> inv_matrix{ std::array<float, 3>{ 1.0f, 0.0f, 0.0f },
                                                    std::array<float, 3>{ 0.0f, 1.0f, 0.0f },
                                                    std::array<float, 3>{ 0.0f, 0.0f, 1.0f } };

    // Black level: tristimulus values measured with all channels off
    // (leakage, ambient light), in the same units as the target colors.
    std::array<float, 3> black{ 0.0f, 0.0f, 0.0f };

    // Gamma exponent used to encode the drive values. Perceived brightness
    // follows a power law, so a 50 % duty cycle does not look half as
    // bright; encoding with gamma makes an evenly spaced ramp of targets
    // look evenly spaced. 2.2 is the conventional approximation for sRGB,
    // 1.0 disables it. Non-positive values (including NaN) are treated as
    // 1.0.
    float gamma = 1.0f;

    // Convert a target color (per-channel linear light in the reference
    // space, 0..65535) into the gamma-encoded drive values that reproduce
    // it on the calibrated LED. Out-of-gamut channels are clamped to zero
    // and, if a channel would exceed full scale, all channels are rescaled
    // so the hue is preserved at a lower brightness. Non-finite calibration
    // data (e.g. NaN/Inf values loaded from storage) maps to black instead
    // of invoking undefined behavior in the quantizer.
    constexpr Drive drive(uint16_t r, uint16_t g, uint16_t b) const
    {
        constexpr float inv_full = 1.0f / 65535.0f;
        const float gamma_enc = gamma > 0.0f ? gamma : 1.0f;
        const float tr = static_cast<float>(r) * inv_full - black[0];
        const float tg = static_cast<float>(g) * inv_full - black[1];
        const float tb = static_cast<float>(b) * inv_full - black[2];

        float d0 = inv_matrix[0][0] * tr + inv_matrix[0][1] * tg + inv_matrix[0][2] * tb;
        float d1 = inv_matrix[1][0] * tr + inv_matrix[1][1] * tg + inv_matrix[1][2] * tb;
        float d2 = inv_matrix[2][0] * tr + inv_matrix[2][1] * tg + inv_matrix[2][2] * tb;

        if (d0 < 0.0f)
            d0 = 0.0f;
        if (d1 < 0.0f)
            d1 = 0.0f;
        if (d2 < 0.0f)
            d2 = 0.0f;

        const float max = d0 > d1 ? d0 : d1;
        const float peak = d2 > max ? d2 : max;
        if (peak > 1.0f) {
            d0 /= peak;
            d1 /= peak;
            d2 /= peak;
        }

        const auto encode = [gamma_enc](float v) {
            if (v <= 0.0f)
                return 0.0f;
#if defined(__clang__)
            if (std::is_constant_evaluated())
                return detail::pow(v, gamma_enc);
#endif
            if (gamma_enc == 1.0f)
                return v;
            return std::pow(v, gamma_enc);
        };
        const auto quantize = [](float v) -> uint16_t {
            if (!std::isfinite(v))
                return 0;
            v = v * 65535.0f + 0.5f;
            return v >= 65535.0f ? 65535 : static_cast<uint16_t>(v);   // GCOVR_EXCL_BR_LINE
        };
        return Drive{ quantize(encode(d0)), quantize(encode(d1)), quantize(encode(d2)) };
    }
};

// Calibrate an 8-bit-per-channel RGB color: the channels are widened to the
// 16-bit calibrated space (the same ×257 scaling `deep_color()` uses), run
// through `LedCalibration::drive()` and truncated back to 8 bits, matching
// the high byte `deep_color()` renders. With the default (identity)
// calibration the color is returned unchanged.
template<typename RGB>
constexpr RGB
calibrate(const RGB &color, const LedCalibration &calibration)
{
    const auto drv = calibration.drive(static_cast<uint16_t>(color.r) * 257U,
                                       static_cast<uint16_t>(color.g) * 257U,
                                       static_cast<uint16_t>(color.b) * 257U);
    return RGB{ static_cast<uint8_t>(drv.r >> 8),
                static_cast<uint8_t>(drv.g >> 8),
                static_cast<uint8_t>(drv.b >> 8) };
}

// Rise (or set) the strip like a sun over `duration_ms`, with the
// transition endpoints mapped through `calibration` (via `calibrate()`).
template<typename T>
inline void
sun_effect(T &strip,
           const std::size_t duration_ms,
           bool rising = true,
           const std::atomic<bool> *stop = nullptr,
           const LedCalibration &calibration = LedCalibration{},
           const DitherConfig &dither = DitherConfig{})
{
    constexpr typename T::RGB dim{ 5, 1, 0 };
    constexpr typename T::RGB bright{ 255, 180, 80 };
    if (rising) {
        smooth_transition(strip,
                          calibrate(dim, calibration),
                          calibrate(bright, calibration),
                          duration_ms,
                          stop,
                          dither);
    } else {
        smooth_transition(strip,
                          calibrate(bright, calibration),
                          calibrate(dim, calibration),
                          duration_ms,
                          stop,
                          dither);
    }
}

// Alternate between `color1` and `color2`, pausing `wait_ms` at each end. Like
// `sun_effect()`, the two colors are mapped through `calibration` (via
// `calibrate()`) before the transitions run, so the breathing runs between the
// calibrated endpoints. `repetitions` counts whole cycles; a negative value
// runs until the stop flag is set. On normal completion it leaves the strip at
// `color1`; a stop-flag abort leaves it mid-fade at the last rendered frame.
template<typename T>
inline void
breathing(T &strip,
          const typename T::RGB &color1,
          const typename T::RGB &color2,
          const std::size_t duration_ms,
          const std::size_t wait_ms,
          const int repetitions = 1,
          const std::atomic<bool> *stop = nullptr,
          const LedCalibration &calibration = LedCalibration{},
          const DitherConfig &dither = DitherConfig{})
{
    const typename T::RGB cal_color1 = calibrate(color1, calibration);
    const typename T::RGB cal_color2 = calibrate(color2, calibration);
    for (std::size_t i = 0;; ++i) {
        if (repetitions >= 0 && i >= detail::total_frames(repetitions, 1))
            break;
        if (stop && stop->load())
            return;
        smooth_transition(strip, cal_color1, cal_color2, duration_ms, stop, dither);
        if (stop && stop->load())   // GCOVR_EXCL_BR_LINE
            return;
        if (!detail::wait_remaining(wait_ms, stop))   // GCOVR_EXCL_BR_LINE
            return;
        smooth_transition(strip, cal_color2, cal_color1, duration_ms, stop, dither);
        if (stop && stop->load())   // GCOVR_EXCL_BR_LINE
            return;
        if (!detail::wait_remaining(wait_ms, stop))   // GCOVR_EXCL_BR_LINE
            return;
    }
}

// Show `target` (a 16-bit-per-channel calibrated color, see LedCalibration)
// on the strip, fading in from `normal_color` and fading back out to it. The
// fades are the shared transition engine (detail::fade_all()) that
// `transition_task` also uses, run synchronously in the calling task. With
// hold_ms > 0 the target is shown for that long; with hold_ms == 0 it is shown
// until the stop flag is set (passing a null stop flag then never terminates).
// The fade-out ignores the stop flag, so the effect always ends with
// all_off(). `spatial_leds` spreads the dither phases of the fades and of the
// hold over that many LEDs, multiplying the renderable levels; see
// detail::fade_all(). `Allocator` (whose `value_type` must be `T::RGB`)
// allocates the per-LED scratch buffer the spatial dithering needs; the
// default `spatial_leds == 1` effect does not allocate.
template<typename T,
         std::size_t spatial_leds = 1,
         typename Allocator = std::allocator<typename T::RGB>>
inline void
deep_color(T &strip,
           const uint16_t target_r,
           const uint16_t target_g,
           const uint16_t target_b,
           const typename T::RGB &normal_color,
           const std::size_t fade_in_ms = 0,
           const std::size_t fade_out_ms = 0,
           const std::size_t hold_ms = 0,
           const std::atomic<bool> *stop = nullptr,
           const LedCalibration *calibration = nullptr,
           const DitherConfig &dither = DitherConfig{})
{
    constexpr LedCalibration default_calibration{};

    if (calibration == nullptr)
        calibration = &default_calibration;
    const auto drv = calibration->drive(target_r, target_g, target_b);

    // GCOVR_EXCL_BR_START: the never-taken std::bad_alloc unwinding edges of
    // the scratch buffer. The buffer is the only owned object in this
    // function; the phase branches live in detail::deep_color_run() and stay
    // reportable.
    std::vector<typename T::RGB, Allocator> scratch;
    if constexpr (spatial_leds > 1)
        scratch.resize(T::num_leds());
    detail::deep_color_run<T, spatial_leds>(
            strip,
            drv.r,
            drv.g,
            drv.b,
            normal_color,
            fade_in_ms,
            fade_out_ms,
            hold_ms,
            stop,
            dither,
            detail::scratch_span<T, (spatial_leds != 1)>(scratch));
    // GCOVR_EXCL_BR_STOP
}

/**
 * Strip-owning background task that slowly fades the strip to whatever
 * color is posted to it (a 16-bit-per-channel calibrated target color,
 * like the one `deep_color()` renders, see `LedCalibration::drive()`).
 *
 * The constructor spawns a dedicated FreeRTOS task (via `cppTaskCreate()`)
 * that starts with all LEDs off and then, on every received color, fades
 * the strip from its current color to the new one over `transition_ms`
 * milliseconds. Once the target is reached the task keeps rendering its
 * temporal dithering (like `deep_color()`'s hold phase) until the next
 * color is posted, so the perceived color stays accurate instead of
 * freezing on whichever 8-bit dither phase the fade ended on.
 *
 * The destructor turns the LEDs off again; `stop()` does the same and deletes
 * the task, and `start()` re-creates it. `stop()` drops the transitions that
 * are still queued, so only the transition that is already in flight can delay
 * it.
 *
 * The queue is an implementation detail: callers only use
 * `post_transition()` (one color for the whole strip) and
 * `post_transition(r, g, b)` (one 16-bit target color per LED).
 *
 * The strip methods (`set_all_leds()`, `set_leds()`, `all_on()`,
 * `all_off()`) mirror the `ws2812` interface but run through the same
 * transition machinery, using the transition time passed to the
 * constructor. `num_leds()` forwards to the strip type.
 *
 * Threading: `post_transition()` and the `ws2812`-like setters are safe to
 * call from any task but must not be called from ISR context (they allocate
 * the command on the heap and take the queue lock). The queue handle is
 * protected by a mutex shared with `stop()`, `start()` and the destructor, so
 * a post that races a stop is either dropped or serialized instead of touching
 * a deleted queue. The strip itself is only ever touched by the transition
 * task, so no external serialization is needed. Like with every effect, the
 * destructor (and `stop()`) must not be called from the transition task
 * itself: it would wait for the task to delete itself forever.
 *
 * A strip fault (the strip's setters may throw, e.g. `ws2812` on a failed RMT
 * transmission) is caught in the task body: it is logged and brings the task
 * down the same way `stop()` does, instead of unwinding into `cppTaskCreate()`'s
 * entry point, which would abort the whole firmware. The task can then be
 * recreated with `start()`.
 *
 * The strip must outlive this object; the `calibration` pointer must too,
 * because it is dereferenced by the transition task on every posted color.
 *
 * No potentially large array lives on the stack: the per-LED color
 * buffers (the two rendered/current 8-bit frames and the three 16-bit
 * target ramps) are heap-allocated once by the constructor, and each
 * posted command is heap-allocated too. A strip of hundreds of LEDs
 * therefore keeps both the object itself and the task stack small.
 *
 * `spatial_leds` spreads the dither phases of the transitions and of the idle
 * dithering over that many LEDs, multiplying the renderable levels; see
 * detail::fade_all(). It must not exceed the strip's LED count.
 */
template<typename T, std::size_t spatial_leds = 1>
class transition_task
{
    static constexpr const char *TAG = "transition_task";

  public:
    using RGB = typename T::RGB;
    using colors_t = std::array<RGB, T::num_leds()>;

    static_assert(spatial_leds >= 1, "spatial_leds must be at least one LED");
    static_assert(spatial_leds <= T::num_leds(),
                  "spatial_leds must not exceed the number of LEDs");

    /* transition_ms value marking the park sentinel internally (a real
     * transition is clamped to frames >= 1, and SIZE_MAX can never be a
     * duration). Passing it as a transition time, or as the default
     * transition time of the constructor, throws `exception`: it would
     * otherwise be indistinguishable from the park sentinel that `stop()`
     * posts to the task. */
    static constexpr std::size_t park_sentinel_ms = SIZE_MAX;

    /** One posted transition: the per-LED 16-bit target colors. */
    struct command
    {
        std::array<uint16_t, T::num_leds()> r;
        std::array<uint16_t, T::num_leds()> g;
        std::array<uint16_t, T::num_leds()> b;
        std::size_t transition_ms;
    };

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
            return msg_buf_;   // GCOVR_EXCL_LINE: what() is inlined into its
                               // callers, the outlined copy never runs
        }

      private:
        char msg_buf_[128]{};
    };

    /**
     * Spawn the transition task, which turns every LED off first.
     *
     * @param strip the strip to drive; must outlive this object
     * @param calibration optional `LedCalibration` applied to every posted
     *        color (the 16-bit targets are mapped through
     *        `LedCalibration::drive()`, exactly like `deep_color()` does);
     *        nullptr uses the identity calibration
     * @param default_transition_ms transition time used by the
     *        `ws2812`-like wrappers (`set_all_leds()`, `set_leds()`,
     *        `all_on()`, `all_off()`); must not be `park_sentinel_ms`
     * @param name task name (as in `xTaskCreate()`); unlike `xTaskCreate()`
     *        it must stay valid for the object's lifetime, because `start()`
     *        re-creates the task with it
     * @param stack_bytes task stack size in bytes
     * @param priority task priority (as in `xTaskCreate()`)
     * @param queue_len transition queue depth; when full, a post returns
     *        false (the color is dropped, posting never blocks)
     * @param dither temporal-dithering parameters (frame rate and
     *        flicker-fusion frequency, see `DitherConfig`) used by every
     *        transition and by the idle dithering of the target
     * @throws exception when `default_transition_ms` equals
     *         `park_sentinel_ms` (it marks the internal park sentinel), or
     *         when the queue cannot be created or `cppTaskCreate()` fails
     * @throws std::bad_alloc when the per-LED color buffers cannot be
     *         allocated, or when `cppTaskCreate()` cannot allocate its
     *         callable; the transition queue is freed before the exception
     *         leaves the constructor
     */
    explicit transition_task(T &strip,
                             const LedCalibration *calibration = nullptr,
                             std::size_t default_transition_ms = 500,
                             const char *name = "led_transition",
                             uint32_t stack_bytes = 4096,
                             UBaseType_t priority = 5,
                             UBaseType_t queue_len = 4,
                             const DitherConfig &dither = DitherConfig{})
            // GCOVR_EXCL_BR_START: allocator-throw edges of the member
            // initializers (bad_alloc is not handled explicitly)
            : strip_(strip)
            , calibration_(calibration)
            , default_transition_ms_(default_transition_ms)
            , name_(name)
            , stack_bytes_(stack_bytes)
            , priority_(priority)
            , dither_(dither)
            , current_(std::make_unique<colors_t>())
            , frame_(std::make_unique<colors_t>())
            , recv_cmd_(std::make_unique<command>())
            , park_cmd_(std::make_unique<command>())
            , to_r_(std::make_unique<uint32_t[]>(T::num_leds()))
            , to_g_(std::make_unique<uint32_t[]>(T::num_leds()))
            , to_b_(std::make_unique<uint32_t[]>(T::num_leds()))
    // GCOVR_EXCL_BR_STOP
    {
        if (default_transition_ms_ == park_sentinel_ms) {
            throw exception("default_transition_ms is the park sentinel");
        }
        queue_ = xQueueCreate(queue_len, sizeof(command));   // GCOVR_EXCL_BR_LINE
        if (queue_ == nullptr) {   // GCOVR_EXCL_BR_LINE: OOM is not testable
            throw exception("xQueueCreate failed");   // GCOVR_EXCL_LINE
        }
        // GCOVR_EXCL_START: the task-creation failure branch cannot be
        // covered deterministically from this component's tests (the
        // mock_timers kernel hook is requested by the cpptaskcreate and
        // task_queue test components, not by this one). The catch also frees
        // the queue when cppTaskCreate() throws std::bad_alloc instead of
        // returning a failure, so the queue cannot leak out of a constructor
        // that never completes.
        try {
            if (spawn_task() != pdPASS) {
                throw exception("cppTaskCreate failed");
            }
        } catch (...) {
            vQueueDelete(queue_);
            queue_ = nullptr;
            throw;
        }
        // GCOVR_EXCL_STOP
        /* Wait until the transition task is up (and has turned the LEDs
         * off), so stop() can immediately delete it deterministically. */
        while (!running_.load()) {   // GCOVR_EXCL_BR_LINE
            vTaskDelay(1);           // GCOVR_EXCL_LINE
        }
    }   // GCOVR_EXCL_BR_LINE: constructor EH cleanup edge

    /** Stop the task (turning the LEDs off) and free the queue. */
    ~transition_task() noexcept
    {
        stop();
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (queue_ != nullptr) {   // GCOVR_EXCL_BR_LINE
            vQueueDelete(queue_);
            queue_ = nullptr;
        }
    }   // GCOVR_EXCL_BR_LINE: destructor member-cleanup edge

    transition_task(const transition_task &) = delete;
    transition_task &operator=(const transition_task &) = delete;
    transition_task(transition_task &&) = delete;
    transition_task &operator=(transition_task &&) = delete;

    /**
     * Stop the task, turn every LED off and delete the task. Safe to call
     * multiple times; `start()` can re-create the task afterwards.
     * Transitions that are still queued (or being rendered) are dropped:
     * stop() discards the queued transitions, waits for the transition that
     * is in flight to finish, then deletes the task. Must not be called from
     * the transition task itself.
     */
    void stop() noexcept
    {
        /* Snapshot whether the task is alive before posting the sentinel.
         * Only the owning task runs stop()/start()/the constructor, and it
         * always waits for the handshake to complete before returning, so
         * the task cannot delete itself (or be re-created) unobserved. */
        if (running_.exchange(false)) {
            park_count_.store(0);
            park_cmd_->transition_ms = park_sentinel_ms;
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                /* Drop the transitions that are still queued *before* posting
                 * the sentinel, so only the transition that is already in
                 * flight can delay this call. Posting takes the same lock, and
                 * a post that read running_ before the exchange re-checks it
                 * under the lock, so no new command can slip in behind the
                 * sentinel. The queue is empty after the reset, so the send
                 * cannot fail and its result can be ignored. */
                xQueueReset(queue_);
                (void) xQueueSend(queue_, park_cmd_.get(), 0);
            }
            while (park_count_.load() == 0) {   // GCOVR_EXCL_BR_LINE
                vTaskDelay(1);
            }
            park_count_.store(0);
            std::lock_guard<std::mutex> lock(queue_mutex_);
            /* Drop anything a racing post slipped in behind the sentinel:
             * the task is gone now, so nothing consumes the queue. */
            xQueueReset(queue_);
        }
    }

    /**
     * (Re)create the task after stop(). Safe to call multiple times.
     * @throws exception when `cppTaskCreate()` fails
     * @throws std::bad_alloc when `cppTaskCreate()` cannot allocate its
     *         callable
     */
    void start()
    {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (running_.load()) {
                return;
            }
            park_count_.store(0);   // GCOVR_EXCL_BR_LINE: store() never throws
            /* Drop anything a racing post left behind while the task was
             * down, so the re-created task starts from a known state. */
            xQueueReset(queue_);   // GCOVR_EXCL_BR_LINE: xQueueReset() never throws
            std::fill(current_->begin(), current_->end(), RGB{});
            // GCOVR_EXCL_START: the task-creation failure branch cannot be
            // covered deterministically from this component's tests (the
            // mock_timers kernel hook is requested by the cpptaskcreate and
            // task_queue test components, not by this one).
            if (spawn_task() != pdPASS) {
                throw exception("cppTaskCreate failed");
            }
            // GCOVR_EXCL_STOP
        }
        /* Wait until the transition task is up (and has turned the LEDs
         * off), so a following stop() can delete it deterministically. */
        while (!running_.load()) {   // GCOVR_EXCL_BR_LINE
            vTaskDelay(1);           // GCOVR_EXCL_LINE
        }
    }

    /**
     * Fade the whole strip to `target_r/g/b` (a 16-bit-per-channel
     * calibrated color, see `LedCalibration::drive()`) over
     * `transition_ms` milliseconds (0 = the default transition time from
     * the constructor). Returns false when the task is stopped or the
     * queue is full (the color is dropped, posting never blocks).
     * @throws exception when `transition_ms` equals `park_sentinel_ms`
     *         (it marks the internal park sentinel)
     * @throws std::bad_alloc when the command cannot be allocated (the
     *         command is only allocated while the task is running)
     */
    bool post_transition(uint16_t target_r,
                         uint16_t target_g,
                         uint16_t target_b,
                         std::size_t transition_ms = 0)
    {
        if (transition_ms == park_sentinel_ms) {
            throw exception("transition_ms is the park sentinel");
        }
        /* Check running_ before allocating, so a post to a stopped task
         * returns false without a heap allocation (and without the throwing
         * bad_alloc edge). A post that races the check is caught by post(). */
        if (!running_.load()) {
            return false;
        }
        auto cmd = std::make_unique<command>();   // GCOVR_EXCL_BR_LINE: bad_alloc edge
        cmd->r.fill(target_r);
        cmd->g.fill(target_g);
        cmd->b.fill(target_b);
        cmd->transition_ms = transition_ms != 0 ? transition_ms : default_transition_ms_;
        return post(*cmd);
    }

    /**
     * Fade every LED to its own 16-bit-per-channel calibrated target
     * color over `transition_ms` milliseconds (0 = the default
     * transition time from the constructor). Returns false when the task
     * is stopped or the queue is full (the colors are dropped, posting
     * never blocks).
     * @throws exception when `transition_ms` equals `park_sentinel_ms`
     *         (it marks the internal park sentinel)
     * @throws std::bad_alloc when the command cannot be allocated (the
     *         command is only allocated while the task is running)
     */
    bool post_transition(const std::array<uint16_t, T::num_leds()> &target_r,
                         const std::array<uint16_t, T::num_leds()> &target_g,
                         const std::array<uint16_t, T::num_leds()> &target_b,
                         std::size_t transition_ms = 0)
    {
        if (transition_ms == park_sentinel_ms) {
            throw exception("transition_ms is the park sentinel");
        }
        if (!running_.load()) {
            return false;
        }
        auto cmd = std::make_unique<command>();   // GCOVR_EXCL_BR_LINE: bad_alloc edge
        cmd->r = target_r;
        cmd->g = target_g;
        cmd->b = target_b;
        cmd->transition_ms = transition_ms != 0 ? transition_ms : default_transition_ms_;
        return post(*cmd);
    }

    /** Fade the whole strip to `color` using the default transition time. */
    bool set_all_leds(const RGB &color)
    {
        return post_transition(static_cast<uint16_t>(color.r) * 257U,
                               static_cast<uint16_t>(color.g) * 257U,
                               static_cast<uint16_t>(color.b) * 257U);
    }

    /** Fade every LED to its own color using the default transition time. */
    bool set_leds(const colors_t &colors)
    {
        if (!running_.load()) {
            return false;
        }
        auto cmd = std::make_unique<command>();   // GCOVR_EXCL_BR_LINE: bad_alloc edge
        for (std::size_t i = 0; i < T::num_leds(); ++i) {
            cmd->r[i] = static_cast<uint16_t>(colors[i].r) * 257U;
            cmd->g[i] = static_cast<uint16_t>(colors[i].g) * 257U;
            cmd->b[i] = static_cast<uint16_t>(colors[i].b) * 257U;
        }
        cmd->transition_ms = default_transition_ms_;
        return post(*cmd);
    }

    /** Fade the whole strip to white using the default transition time. */
    bool all_on()
    {
        return set_all_leds({ 255, 255, 255 });   // GCOVR_EXCL_BR_LINE: throw edges
    }

    /** Fade the whole strip to black using the default transition time. */
    bool all_off()
    {
        return set_all_leds({ 0, 0, 0 });
    }

    /** Number of LEDs in the strip (forwarded to the strip type). */
    static constexpr std::size_t num_leds()
    {
        return T::num_leds();
    }

  private:
    /** Enqueue a command; returns false when stopped or full.
     *
     * Posting is task-only and never from an ISR: the command is built with a
     * heap allocation by the caller, and copying several kB into the queue is
     * not something an interrupt handler should do. The queue handle is shared
     * with stop()/start()/the destructor, so the access is serialized by
     * `queue_mutex_` to keep an in-flight post from touching a deleted queue.
     * The lock is only held around a non-blocking `xQueueSend()`, so posting
     * does not block while a stop is in progress.
     *
     * GCOVR_EXCL_BR_LINE: the queue_ == nullptr branch only runs after a
     * constructor task-creation failure (already excluded there); gcov
     * also mis-attributes the short-circuit edges. */
    bool post(const command &cmd) noexcept
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        // GCOVR_EXCL_START: the guard only rejects a post that raced stop()
        // (the public entry points check running_ before allocating a command)
        // or one that survived a constructor task-creation failure; neither is
        // reachable deterministically from the tests.
        if (queue_ == nullptr || !running_.load()) {
            return false;
        }
        // GCOVR_EXCL_STOP
        return xQueueSend(queue_, &cmd, 0) == pdTRUE;   // GCOVR_EXCL_BR_LINE
    }

    /** The drive values `color` maps to with the current calibration. */
    LedCalibration::Drive drive_of(uint16_t r, uint16_t g, uint16_t b) const noexcept
    {
        if (calibration_ != nullptr) {
            return calibration_->drive(r, g, b);
        }
        return { r, g, b };
    }

    /** Smoothly move the strip from its current color to `to`'s target.
     *
     * The transition runs on the same dithered fade engine as
     * `detail::fade_all()`, in its per-LED form: `fade_all()` fades the whole
     * strip to one color, whereas the transition task fades every LED from the
     * color it currently shows to its own target, so it calls the shared
     * `detail::fade_run()` directly with per-LED `from`/`to` colors.
     *
     * The dither frame counter is local to each transition (a fresh pattern
     * on every posted color); `fade_run()` only keeps it continuous across the
     * fades that share the same counter, like `deep_color()`'s phases. */
    void transition_to(const command &to)
    {
        for (std::size_t i = 0; i < T::num_leds(); ++i) {
            const auto drv = drive_of(to.r[i], to.g[i], to.b[i]);
            to_r_[i] = drv.r;
            to_g_[i] = drv.g;
            to_b_[i] = drv.b;
        }

        const TickType_t frame_ticks = dither_.frame_ticks();
        uint32_t frame = 0;
        bool overrun_logged = false;
        detail::fade_run<T, spatial_leds, false>(
                strip_,
                [this](std::size_t i) { return detail::widen((*current_)[i]); },
                [this](std::size_t i) { return detail::rgb16(to_r_[i], to_g_[i], to_b_[i]); },
                to.transition_ms,
                /* stop = */ nullptr,
                frame,
                /* reverse = */ false,
                /* exact_ends = */ false,
                dither_,
                *frame_);
        // Show the exact target for one frame, so it is fully rendered even
        // when every dithering step was already at the target color.
        const TickType_t frame_start = xTaskGetTickCount();
        for (std::size_t i = 0; i < T::num_leds(); ++i) {
            (*frame_)[i] = { static_cast<uint8_t>(to_r_[i] >> 8),
                             static_cast<uint8_t>(to_g_[i] >> 8),
                             static_cast<uint8_t>(to_b_[i] >> 8) };
        }
        strip_.set_leds(*frame_);
        detail::frame_delay(frame_start, frame_ticks, overrun_logged);
        *current_ = *frame_;
    }

    /** Keep dithering the transition target until the next command arrives.
     *
     * The fade in `transition_to()` ends on a single 8-bit frame; without
     * this loop the strip would freeze on that phase and a target with
     * fractional lower bytes would look slightly off. Rendering the target
     * with an ever-changing dither frame (like `deep_color()`'s hold phase)
     * averages those fractions out. The queue is polled between frames, so
     * the returned command (stored in `recv_cmd_`) is handled by `run()`.
     *
     * `current_` is not refreshed here: the next transition starts from the
     * high byte of the previous target rather than from the phase actually
     * displayed, which differs by at most one 8-bit step. */
    void hold_dither()
    {
        const std::size_t levels = dither_.levels();
        const std::size_t total = levels * spatial_leds;
        const TickType_t frame_ticks = dither_.frame_ticks();
        uint32_t frame = 0;
        for (;;) {
            const TickType_t frame_start = xTaskGetTickCount();
            for (std::size_t i = 0; i < T::num_leds(); ++i) {
                const std::size_t phase =
                        detail::dither_phase(frame, levels, spatial_leds, i % spatial_leds);
                (*frame_)[i] = { detail::dither16(to_r_[i], phase, total),
                                 detail::dither16(to_g_[i], phase, total),
                                 detail::dither16(to_b_[i], phase, total) };
            }
            strip_.set_leds(*frame_);
            // Wrapping the counter at the end of the dither cycle (instead of
            // letting it overflow) keeps dither16() and dither_phase() from
            // skipping a phase; see next_dither_frame().
            frame = detail::next_dither_frame(frame, levels, spatial_leds);
            // Poll the queue only for the rest of the frame (the render time
            // is subtracted with frame_remaining(), like frame_delay() does),
            // so the idle dithering runs at the configured rate instead of at
            // render time + frame.
            //
            // GCOVR_EXCL_BR_START: the inlined frame_remaining() clamp has a
            // side a real frame never takes (an update never overruns a whole
            // frame in the tests); the helper has its own unit test.
            const TickType_t elapsed = xTaskGetTickCount() - frame_start;
            if (xQueueReceive(queue_,
                              recv_cmd_.get(),
                              detail::frame_remaining(elapsed, frame_ticks)) == pdTRUE) {
                return;
            }
            // GCOVR_EXCL_BR_STOP
        }
    }

    /** Create the transition task (used by the constructor and start()). */
    BaseType_t spawn_task()
    {
        return cppTaskCreate(name_, stack_bytes_, priority_, nullptr, [this] { run(); });
    }

    /** Turn the LEDs off, logging a strip fault instead of letting it escape.
     *
     * The strip's setters may throw (e.g. `ws2812` on a failed RMT
     * transmission). Letting that exception unwind out of the task body would
     * reach `cppTaskCreate()`'s entry point, which logs it and calls abort(),
     * tearing down the whole firmware. The startup call keeps running (the
     * constructor is waiting for the task to come up); the callers that run
     * per frame bring the task down instead. */
    bool all_off_safe() noexcept
    {
        try {
            strip_.all_off();
            return true;
        } catch (...) {
            ESP_LOGE(TAG, "LED strip all_off failed");
            return false;
        }
    }

    /** Take the task down after a strip fault, like `stop()` does.
     *
     * `running_` is cleared so posts are refused and `start()` can re-create
     * the task; `park_count_` acknowledges a `stop()` that is waiting for the
     * handshake. `vTaskDelete(nullptr)` does not return. Not noexcept: on the
     * simulator the task deletion is implemented as a C++ forced unwind, which
     * would call terminate() if it hit a noexcept boundary. */
    void park_after_fault()
    {
        running_.store(false);
        std::fill(current_->begin(), current_->end(), RGB{});
        park_count_.store(1);
        vTaskDelete(nullptr);
    }   // GCOVR_EXCL_LINE: vTaskDelete(nullptr) never returns

    /** Task body: consume transitions, deleting the task whenever stop() asks. */
    void run()
    {
        /* A strip fault while turning the LEDs off is logged and the task
         * still comes up, so the constructor's handshake cannot hang on a
         * faulty strip. */
        all_off_safe();
        std::fill(current_->begin(), current_->end(), RGB{});
        running_.store(true);
        for (;;) {
            /* A portMAX_DELAY receive never fails while the object is
             * alive; the loop only ends when stop()'s sentinel deletes the
             * task. */
            xQueueReceive(queue_, recv_cmd_.get(), portMAX_DELAY);   // GCOVR_EXCL_BR_LINE
            for (;;) {
                if (recv_cmd_->transition_ms == park_sentinel_ms) {
                    /* The park sentinel from stop(): turn everything off,
                     * acknowledge it and delete this task. start() re-creates
                     * the task.
                     * The sentinel command must not be rendered. */
                    all_off_safe();
                    std::fill(current_->begin(), current_->end(), RGB{});
                    park_count_.store(1);
                    vTaskDelete(nullptr);
                }
                /* A strip fault during a transition is caught here: the
                 * exception must not unwind into cppTaskCreate()'s entry
                 * point, which would abort the firmware. The task is brought
                 * down (so the caller can retry with start()) once the catch
                 * block has exited: vTaskDelete() must not run while a C++
                 * exception is still being handled. */
                // GCOVR_EXCL_BR_START: the throwing-call edges of the
                // try/catch (only transition_to() throws in the tests;
                // hold_dither() and ESP_LOGE() have no reachable throw) are
                // not covered.
                bool faulted = false;
                try {
                    transition_to(*recv_cmd_);
                    hold_dither();
                } catch (...) {
                    ESP_LOGE(TAG, "LED strip update failed, stopping the task");
                    faulted = true;
                }
                // GCOVR_EXCL_BR_STOP
                if (faulted) {
                    park_after_fault();
                }
            }
        }
    }

    T &strip_;
    const LedCalibration *calibration_;
    std::size_t default_transition_ms_;
    const char *name_;
    uint32_t stack_bytes_;
    UBaseType_t priority_;
    DitherConfig dither_;
    std::unique_ptr<colors_t> current_;
    std::unique_ptr<colors_t> frame_;
    std::unique_ptr<command> recv_cmd_;
    std::unique_ptr<command> park_cmd_;
    std::unique_ptr<uint32_t[]> to_r_;
    std::unique_ptr<uint32_t[]> to_g_;
    std::unique_ptr<uint32_t[]> to_b_;
    /* Serializes every access to `queue_` from the outside (post(), stop(),
     * start(), the destructor); the transition task itself only receives from
     * the queue without the lock, because the queue is deleted only after the
     * task has been parked. */
    std::mutex queue_mutex_;
    QueueHandle_t queue_{};
    std::atomic<bool> running_{ false };
    std::atomic<unsigned> park_count_{ 0 };
};

}   // namespace leds_effects
