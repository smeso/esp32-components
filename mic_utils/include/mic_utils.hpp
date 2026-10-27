/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <span>
#include <stdexcept>
#include <type_traits>

/*
 * Tell whether the most recent `seconds` of audio in an interleaved buffer are
 * silent: their RMS level, normalized to the sample type's full scale, must be
 * at or below `threshold`.
 *
 * Template parameters:
 * - `T`: contiguous container of integral samples (e.g. std::vector<int16_t>,
 *   std::span<const uint8_t> or a plain array). Samples are assumed to be
 *   zero-centered (silence at value 0); unsigned types with silence at
 *   mid-scale (e.g. conventional unsigned 8-bit PCM) are not supported.
 * - `SumT`: floating-point accumulation type (`float` by default; use
 *   `double` on targets with hardware double-precision support, see the note).
 *
 * Parameters:
 * - `buf`: interleaved samples; a trailing partial frame is ignored.
 * - `channels`: indexes of the channels that contribute to the measurement
 *   (within [0, channels_per_frame)); an empty list yields `true` without
 *   looking at any sample. The indexes must be distinct, so each channel
 *   contributes exactly once.
 * - `channels_per_frame`: number of interleaved channels per frame; `0` yields
 *   `false`.
 * - `sample_rate`, `seconds`: the window is the last `sample_rate * seconds`
 *   frames. If the buffer holds fewer frames than the window, `false` is
 *   returned even if the whole buffer is silent. A zero sample rate or a zero
 *   duration yields `false`.
 * - `threshold`: maximum normalized RMS that is still considered silent; a
 *   negative or NaN threshold yields `false`.
 *
 * Samples are normalized against the largest magnitude the sample type can
 * hold, i.e. `max(|min|, max)`, so a normalized RMS is always in [0, 1] even
 * for signed types whose minimum magnitude is one larger than their maximum.
 *
 * Throws std::out_of_range if any channel index is out of range and
 * std::invalid_argument if `channels` repeats an index; both are checked
 * before any sample is examined. Exceptions must be enabled in the build
 * (-fno-exceptions is not supported).
 *
 * When several inputs are invalid the checks are applied in this order, which
 * decides both the result and whether an exception is thrown: a zero
 * `channels_per_frame` yields `false`; the channel indexes are then validated
 * (so a degenerate `sample_rate`/`seconds`/`threshold` cannot mask an
 * out-of-range or repeated index); then a zero `sample_rate`/`seconds` or a
 * negative or NaN `threshold` yields `false`; then an empty `channels` list
 * yields `true`; finally a buffer shorter than the window yields `false`.
 *
 * NOTE: with the default SumT = float, accumulating millions of samples
 * loses low bits and can bias the RMS of quiet-but-not-silent tails; use
 * SumT = double for long windows on targets with double-precision HW.
 */
template<typename T, typename SumT = float>
[[nodiscard]] bool
has_silence(const T &buf,
            std::span<const size_t> channels,
            size_t channels_per_frame,
            uint32_t sample_rate,
            size_t seconds,
            float threshold = 0.001f)
{
    using sample_t = std::remove_cvref_t<decltype(buf[0])>;
    static_assert(std::is_integral_v<sample_t>,
                  "has_silence only supports integral sample types, normalized "
                  "against their full scale (silence is expected at zero)");
    static_assert(std::is_floating_point_v<SumT>,
                  "has_silence requires a floating-point accumulation type");
    // Without a frame size no channel index can be valid, so this check has to
    // come before the index validation below.
    if (channels_per_frame == 0)
        return false;

    // All channel indexes are validated up front: the function either scans
    // all the requested channels or throws without doing any work.
    for (size_t i = 0; i < channels.size(); ++i) {
        if (channels[i] >= channels_per_frame)
            // GCOVR_EXCL_BR_LINE: the std::out_of_range transfer edge is
            // never recorded by gcov on xtensa, even though the
            // "channel index out of range throws" test exercises it.
            throw std::out_of_range(   // GCOVR_EXCL_BR_LINE
                    "has_silence: channel index out of range");
        for (size_t j = i + 1; j < channels.size(); ++j)
            if (channels[i] == channels[j])
                // GCOVR_EXCL_BR_LINE: the std::invalid_argument transfer edge
                // is never recorded by gcov on xtensa, even though the
                // "duplicate channel index throws" test exercises it.
                throw std::invalid_argument(   // GCOVR_EXCL_BR_LINE
                        "has_silence: duplicate channel index");
    }

    if (sample_rate == 0)
        return false;
    if (seconds == 0)
        return false;
    if (!(threshold >= 0.0F))
        return false;
    // An empty channel list is silent by definition: there is nothing to
    // measure, so no sample needs to be examined.
    if (channels.empty())
        return true;

    // A trailing partial frame (buf.size() not a multiple of
    // channels_per_frame) is silently ignored.
    const uint64_t num_frames = std::size(buf) / channels_per_frame;
    // Compare `seconds` against `num_frames / sample_rate` instead of building
    // the window first: the division cannot overflow (unlike the product),
    // and `seconds > num_frames / sample_rate` is equivalent to
    // `sample_rate * seconds > num_frames`, i.e. the buffer is too short.
    if (seconds > num_frames / sample_rate)
        return false;
    // Safe in both 32- and 64-bit: `seconds <= num_frames / sample_rate`
    // guarantees the product is at most `num_frames`.
    const uint64_t window_frames = static_cast<uint64_t>(sample_rate) * seconds;
    const uint64_t start = num_frames - window_frames;
    // `count` cannot overflow: distinct, in-range indexes imply
    // channels.size() <= channels_per_frame and the check above implies
    // window_frames <= num_frames, so
    // count <= num_frames * channels_per_frame <= std::size(buf). This also
    // keeps the division below well defined (count >= 1 here).
    const uint64_t count = window_frames * channels.size();

    SumT sum_sq = 0.0;
    for (size_t ch : channels) {
        for (uint64_t i = 0; i < window_frames; ++i) {
            const auto sample =
                    buf[static_cast<size_t>((start + i) * channels_per_frame + ch)];
            sum_sq += static_cast<SumT>(sample) * static_cast<SumT>(sample);
        }
    }
    // Normalize against the largest magnitude the sample type can hold
    // (`max(|min|, max)`, which differ by one for signed types) so that a
    // full-scale negative signal does not exceed 1.0.
    const SumT full_scale =
            std::max(static_cast<SumT>(std::numeric_limits<sample_t>::max()),
                     std::abs(static_cast<SumT>(std::numeric_limits<sample_t>::min())));
    const SumT rms = std::sqrt(sum_sq / static_cast<SumT>(count));
    const SumT normalized_rms = rms / full_scale;
    return normalized_rms <= static_cast<SumT>(threshold);
}
