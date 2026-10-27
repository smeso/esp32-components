/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Minimal mic_utils example: generates a short stereo buffer (one second of
 * a sine wave followed by a silent tail) and reports how has_silence()
 * classifies four windows: the tone, the tail, the whole buffer with a
 * window covering only the tail (silent) and the whole buffer with a window
 * that also covers the tone (not silent). The buffer is built in memory, so
 * the example runs without any microphone hardware (or any other component).
 *
 * All the configuration (sample rate, silence duration and threshold) is
 * done through Kconfig.
 */

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>

#include <esp_log.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "mic_utils.hpp"

namespace {

constexpr const char *TAG = "detect_silence";

// The threshold is a string in Kconfig (no floating-point type is available),
// so parse it with std::strtof at startup.
float
silence_threshold()
{
    return std::strtof(CONFIG_DETECT_SILENCE_THRESHOLD, nullptr);
}

}   // namespace

extern "C" void
app_main(void)
{
    constexpr uint32_t sample_rate = CONFIG_DETECT_SILENCE_SAMPLE_RATE;
    constexpr size_t channels_per_frame = 2;
    constexpr size_t silence_seconds = CONFIG_DETECT_SILENCE_SECONDS;
    const float threshold = silence_threshold();

    // One second of 440 Hz tone on both channels followed by a silent tail,
    // interleaved as stereo frames.
    constexpr size_t tone_frames = sample_rate;
    constexpr size_t tail_frames = sample_rate * silence_seconds;
    std::vector<int16_t> buf((tone_frames + tail_frames) * channels_per_frame);
    for (size_t i = 0; i < tone_frames; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
        const int16_t v = static_cast<int16_t>(
                0.25F * 32767.0F * std::sin(2.0F * static_cast<float>(M_PI) * 440.0F * t));
        buf[2 * i] = v;
        buf[2 * i + 1] = v;
    }
    // The tail is already zero-initialized.

    constexpr std::array<size_t, 2> channels = { 0, 1 };
    const std::span<int16_t> tone{ buf.data(), tone_frames * channels_per_frame };
    const std::span<int16_t> tail{ buf.data() + tone_frames * channels_per_frame,
                                   tail_frames * channels_per_frame };

    const bool tone_is_silence =
            has_silence(tone, channels, channels_per_frame, sample_rate, 1, threshold);
    const bool tail_is_silence = has_silence(
            tail, channels, channels_per_frame, sample_rate, silence_seconds, threshold);
    // The window is the last `seconds` seconds of the buffer: with
    // `silence_seconds` it covers exactly the silent tail, with
    // `silence_seconds + 1` it also reaches into the tone.
    const bool whole_tail_is_silence = has_silence(
            buf, channels, channels_per_frame, sample_rate, silence_seconds, threshold);
    const bool whole_is_silence = has_silence(
            buf, channels, channels_per_frame, sample_rate, silence_seconds + 1, threshold);

    ESP_LOGI(TAG,
             "tone: %s, tail: %s, whole buffer (tail window): %s, "
             "whole buffer (tone window): %s",
             tone_is_silence ? "silent" : "not silent",
             tail_is_silence ? "silent" : "not silent",
             whole_tail_is_silence ? "silent" : "not silent",
             whole_is_silence ? "silent" : "not silent");

    if (tone_is_silence || whole_is_silence) {
        ESP_LOGE(TAG, "unexpected: windows covering the tone must not be silent");
        std::abort();
    }
    if (tail_is_silence && whole_tail_is_silence) {
        ESP_LOGI(TAG, "done");
        return;
    }
    ESP_LOGE(TAG, "silence detection did not match the expected outcome");
    std::abort();
}
