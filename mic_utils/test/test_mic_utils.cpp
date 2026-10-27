/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

#include <doctest/doctest.h>
#include "mic_utils.hpp"

namespace {

// 1 kHz stereo (2 channels per frame) buffer; `silent` frames are zero,
// `loud` frames are a quarter of the full-scale square wave.
std::vector<int16_t>
make_buffer(size_t frames, uint32_t sample_rate, size_t loud_frames)
{
    std::vector<int16_t> buf(frames * 2);
    for (size_t i = 0; i < loud_frames; ++i) {
        const int16_t v =
                (i % 2 == 0) ? static_cast<int16_t>(std::numeric_limits<int16_t>::max() / 4) :
                               -static_cast<int16_t>(std::numeric_limits<int16_t>::max() / 4);
        buf[2 * i] = v;
        buf[2 * i + 1] = v;
    }
    return buf;
}

}   // namespace

TEST_CASE("empty channel list is silent")
{
    const std::vector<int16_t> buf = make_buffer(8000, 8000, 0);
    constexpr std::array<size_t, 0> no_channels{};
    CHECK(has_silence(buf, no_channels, 2, 8000, 1));
}

TEST_CASE("insufficient samples is not silent")
{
    const std::vector<int16_t> buf = make_buffer(4000, 8000, 4000);
    constexpr std::array<size_t, 1> channels = { 0 };
    CHECK_FALSE(has_silence(buf, channels, 2, 8000, 1));
}

TEST_CASE("buffers without a complete frame are not silent")
{
    // A zero-frame buffer holds fewer frames than the window, so it must be
    // reported as not silent even though it contains no loud sample at all.
    constexpr std::array<size_t, 1> channels = { 0 };
    const std::vector<int16_t> empty;
    CHECK_FALSE(has_silence(empty, channels, 2, 8000, 1));
    // Fewer samples than a single complete frame.
    const std::vector<int16_t> partial = { 0 };
    CHECK_FALSE(has_silence(partial, channels, 2, 8000, 1));
}

TEST_CASE("zero channels per frame is not silent")
{
    const std::vector<int16_t> buf = make_buffer(8000, 8000, 0);
    constexpr std::array<size_t, 1> channels = { 0 };
    // `channels_per_frame` must be a runtime value so the
    // `channels_per_frame == 0` branch is not constant-folded at the call
    // site (a literal 0 would emit no branch in the compiled copy).
    volatile size_t cpf = 0;
    CHECK_FALSE(has_silence(buf, channels, cpf, 8000, 1));
}

TEST_CASE("silent buffer is silent")
{
    const std::vector<int16_t> buf = make_buffer(8000, 8000, 0);
    constexpr std::array<size_t, 2> channels = { 0, 1 };
    CHECK(has_silence(buf, channels, 2, 8000, 1));
    CHECK(has_silence(buf, channels, 2, 8000, 1, 0.0F));
}

TEST_CASE("loud buffer is not silent")
{
    const std::vector<int16_t> buf = make_buffer(8000, 8000, 8000);
    constexpr std::array<size_t, 2> channels = { 0, 1 };
    CHECK_FALSE(has_silence(buf, channels, 2, 8000, 1));
}

TEST_CASE("only the most recent seconds are considered")
{
    // 2 s of loud audio followed by 2 s of silence: the last 2 s are silent
    // even though the buffer starts loudly, but a 3 s window reaches back
    // into the loud part. A low sample rate keeps the buffer small enough
    // for the emulated target's heap.
    const std::vector<int16_t> buf = make_buffer(4000, 1000, 2000);
    constexpr std::array<size_t, 2> channels = { 0, 1 };
    CHECK(has_silence(buf, channels, 2, 1000, 2));
    CHECK_FALSE(has_silence(buf, channels, 2, 1000, 3));
}

TEST_CASE("only the selected channels are measured")
{
    // Left channel loud, right channel silent: silence is detected when the
    // right channel is measured and not when the left one is.
    const size_t frames = 8000;
    std::vector<int16_t> buf(frames * 2);
    for (size_t i = 0; i < frames; ++i)
        buf[2 * i] = std::numeric_limits<int16_t>::max() / 4;
    constexpr std::array<size_t, 1> left = { 0 };
    constexpr std::array<size_t, 1> right = { 1 };
    constexpr std::array<size_t, 2> both = { 0, 1 };
    CHECK_FALSE(has_silence(buf, left, 2, 8000, 1));
    CHECK(has_silence(buf, right, 2, 8000, 1));
    CHECK_FALSE(has_silence(buf, both, 2, 8000, 1));
}

TEST_CASE("multi-channel buffers use channels_per_frame")
{
    // 4 channels per frame (e.g. TDM): measure channels 1 and 3 only.
    const size_t frames = 8000;
    std::vector<int16_t> buf(frames * 4);
    for (size_t i = 0; i < frames; ++i)
        buf[4 * i + 1] = std::numeric_limits<int16_t>::max() / 4;
    constexpr std::array<size_t, 2> channels = { 1, 3 };
    CHECK_FALSE(has_silence(buf, channels, 4, 8000, 1));
    constexpr std::array<size_t, 2> silent_channels = { 0, 2 };
    CHECK(has_silence(buf, silent_channels, 4, 8000, 1));
}

TEST_CASE("channel index out of range throws")
{
    const std::vector<int16_t> buf = make_buffer(8000, 8000, 0);
    constexpr std::array<size_t, 1> bad = { 2 };
    CHECK_THROWS_AS((void) has_silence(buf, bad, 2, 8000, 1), std::out_of_range);
}

TEST_CASE("channel validation is not masked by degenerate inputs")
{
    const std::vector<int16_t> buf = make_buffer(8000, 8000, 0);
    constexpr std::array<size_t, 1> bad = { 5 };
    // An out-of-range index wins over a buffer too small to hold a frame ...
    const std::vector<int16_t> empty;
    CHECK_THROWS_AS((void) has_silence(empty, bad, 2, 8000, 1), std::out_of_range);
    // ... and over the degenerate-parameter checks.
    volatile uint32_t zero_rate = 0;
    volatile size_t zero_seconds = 0;
    CHECK_THROWS_AS((void) has_silence(buf, bad, 2, zero_rate, 1), std::out_of_range);
    CHECK_THROWS_AS((void) has_silence(buf, bad, 2, 8000, zero_seconds), std::out_of_range);
    CHECK_THROWS_AS((void) has_silence(buf, bad, 2, 8000, 1, -1.0F), std::out_of_range);
}

TEST_CASE("duplicate channel indexes throw")
{
    const std::vector<int16_t> buf = make_buffer(8000, 8000, 0);
    constexpr std::array<size_t, 2> dup = { 0, 0 };
    CHECK_THROWS_AS((void) has_silence(buf, dup, 2, 8000, 1), std::invalid_argument);
}

TEST_CASE("non-contiguous buffers are supported")
{
    // A std::span over a slice of the vector: same data, different type.
    const std::vector<int16_t> buf = make_buffer(16000, 8000, 0);
    const std::span<const int16_t> silent{ buf.data(), buf.size() };
    constexpr std::array<size_t, 2> channels = { 0, 1 };
    CHECK(has_silence(silent, channels, 2, 8000, 1));
}

TEST_CASE("threshold controls the classification")
{
    const std::vector<int16_t> buf = make_buffer(8000, 8000, 8000);
    constexpr std::array<size_t, 2> channels = { 0, 1 };
    // The buffer is a quarter-scale square wave: normalized RMS = 0.25, so
    // any threshold below 0.25 reports "not silent" and anything above
    // reports "silent".
    CHECK_FALSE(has_silence(buf, channels, 2, 8000, 1, 0.1F));
    CHECK(has_silence(buf, channels, 2, 8000, 1, 0.5F));
}

TEST_CASE("plain arrays are supported")
{
    int16_t buf[200] = {};
    constexpr std::array<size_t, 2> channels = { 0, 1 };
    CHECK(has_silence(buf, channels, 2, 100, 1));
    buf[0] = std::numeric_limits<int16_t>::max() / 4;
    CHECK_FALSE(has_silence(buf, channels, 2, 100, 1));
}

TEST_CASE("signed full scale is normalized against the larger magnitude")
{
    // INT16_MIN has magnitude 32768, one more than INT16_MAX: normalized
    // against max(|min|, max) a constant INT16_MIN signal is exactly 1.0, like
    // a constant INT16_MAX one, so at threshold 1.0 both are silent and the
    // classification is symmetric.
    std::vector<int16_t> buf(16000, std::numeric_limits<int16_t>::min());
    constexpr std::array<size_t, 2> channels = { 0, 1 };
    CHECK(has_silence(buf, channels, 2, 8000, 1, 1.0F));
    std::fill(buf.begin(), buf.end(), std::numeric_limits<int16_t>::max());
    CHECK(has_silence(buf, channels, 2, 8000, 1, 1.0F));
    // Below full scale both signs are correctly classified as not silent.
    CHECK_FALSE(has_silence(buf, channels, 2, 8000, 1, 0.5F));
}

TEST_CASE("unsigned sample types normalize against their own max")
{
    // 8-bit samples: full-scale 0xFF is not silent, 0x00 is.
    std::vector<uint8_t> buf(16000);
    for (size_t i = 0; i < 8000; ++i)
        buf[2 * i] = std::numeric_limits<uint8_t>::max();
    constexpr std::array<size_t, 2> channels = { 0, 1 };
    CHECK_FALSE(has_silence(buf, channels, 2, 8000, 1));
    std::fill(buf.begin(), buf.end(), 0);
    CHECK(has_silence(buf, channels, 2, 8000, 1));
}

TEST_CASE("degenerate windows and thresholds are not silent")
{
    const std::vector<int16_t> buf = make_buffer(8000, 8000, 0);
    constexpr std::array<size_t, 1> channels = { 0 };
    CHECK_FALSE(has_silence(buf, channels, 2, 0, 1));
    CHECK_FALSE(has_silence(buf, channels, 2, 8000, 0));
    CHECK_FALSE(has_silence(buf, channels, 2, 8000, 1, -0.5F));
    CHECK_FALSE(
            has_silence(buf, channels, 2, 8000, 1, std::numeric_limits<float>::quiet_NaN()));
}

TEST_CASE("window computation does not overflow")
{
    // On 32-bit targets a 32-bit `sample_rate * seconds` wraps to a small
    // window (here 0xFFFFFFFE * 2 mod 2^32 == 0xFFFFFFFC, and 2 would wrap to
    // 0), silently shrinking or emptying the window. The 64-bit computation
    // rejects the buffer instead of misclassifying it.
    const std::vector<int16_t> buf = make_buffer(8000, 8000, 0);
    constexpr std::array<size_t, 1> channels = { 0 };
    volatile uint32_t rate = 0xFFFFFFFFU;
    CHECK_FALSE(has_silence(buf, channels, 2, rate, 2));
}

TEST_CASE("window computation does not overflow on 64-bit hosts")
{
    // `size_t` is 64-bit here: (2^64 - 1) / 1000 + 1 == 18446744073709552
    // wraps the 64-bit `sample_rate * seconds` product to 384, which would
    // shrink the window to the last 384 frames. Those are silent while the
    // beginning of the buffer is loud, so an unguarded computation would
    // report the whole (loud) buffer as silent.
    const std::vector<int16_t> buf = make_buffer(8000, 8000, 7616);
    constexpr std::array<size_t, 1> channels = { 0 };
    volatile uint32_t rate = 1000;
    const size_t huge_seconds = std::numeric_limits<size_t>::max() / rate + 1;
    CHECK_FALSE(has_silence(buf, channels, 2, rate, huge_seconds));
}

TEST_CASE("double accumulation type")
{
    const std::vector<int16_t> buf = make_buffer(8000, 8000, 0);
    constexpr std::array<size_t, 2> channels = { 0, 1 };
    CHECK(has_silence<decltype(buf), double>(buf, channels, 2, 8000, 1));
    CHECK_FALSE(has_silence<decltype(buf), double>(
            make_buffer(8000, 8000, 8000), channels, 2, 8000, 1));
    // Runtime values keep the degenerate-window branches in the compiled copy
    // (a literal 0 would be folded away at the call site by constprop).
    volatile uint32_t zero_rate = 0;
    volatile size_t zero_seconds = 0;
    CHECK_FALSE(has_silence<decltype(buf), double>(buf, channels, 2, zero_rate, 1));
    CHECK_FALSE(has_silence<decltype(buf), double>(buf, channels, 2, 8000, zero_seconds));
}
