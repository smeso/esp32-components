/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Minimal sound_driven example: generates a short tone in memory and feeds it
 * to a sound_driven instance backed by a software DAC, printing the per-band
 * amplitudes to the log. The example is self-contained: the software DAC and
 * the tone generator are defined here, so it only needs the sound_driven
 * component itself (and the components it requires) and does not depend on any
 * other component.
 */

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include <esp_log.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "sound_driven.hpp"

namespace {

constexpr const char *TAG = "audio_meter";

// Software DAC sink: sound_driven only needs mclk_multiple, get_sample_rate()
// and write(). Every chunk handed to it is just counted, so the example needs
// no real hardware.
struct software_dac
{
    static constexpr auto mclk_multiple = 256;
    std::size_t chunks = 0;
    std::size_t samples = 0;
    std::uint32_t sample_rate;

    explicit software_dac(std::uint32_t rate)
            : sample_rate(rate)
    {
    }

    [[nodiscard]] std::uint32_t get_sample_rate() const noexcept
    {
        return sample_rate;
    }

    std::size_t write(std::span<const std::int16_t> data,
                      std::uint8_t = 2,
                      TickType_t = portMAX_DELAY)
    {
        ++chunks;
        samples += data.size();
        return data.size();
    }
};

// Interleaved stereo buffer with `frames` frames of a sine wave at
// `frequency` Hz and the given peak amplitude.
std::vector<std::int16_t>
sine_buffer(std::uint32_t sample_rate, double frequency, std::size_t frames, std::int16_t peak)
{
    std::vector<std::int16_t> buf(frames * 2);
    const double step = 2.0 * M_PI * frequency / static_cast<double>(sample_rate);
    for (std::size_t i = 0; i < frames; ++i) {
        const std::int16_t v = static_cast<std::int16_t>(
                static_cast<double>(peak) * std::sin(step * static_cast<double>(i)));
        buf[2 * i] = v;
        buf[2 * i + 1] = v;
    }
    return buf;
}

}   // namespace

extern "C" void
app_main(void)
{
    constexpr std::uint32_t sample_rate = 44100;
    software_dac dac(sample_rate);

    // 7 spectral groups (one per band): the callback receives 7 values.
    sound_driven<software_dac, 1, 7> meter(dac, [](std::array<std::uint16_t, 7> amps) {
        char bands[128];
        int off = 0;
        for (std::size_t g = 0; g < 7; ++g) {
            off += std::snprintf(bands + off,
                                 sizeof(bands) - static_cast<std::size_t>(off),
                                 "%s%u",
                                 g == 0 ? "" : " ",
                                 static_cast<unsigned>(amps[g]));
        }
        ESP_LOGI(TAG, "bands: %s", bands);
    });

    // One second of a 440 Hz tone at half the full scale: the energy lands in
    // the low/mid bands.
    const std::vector<std::int16_t> tone = sine_buffer(sample_rate, 440.0, sample_rate, 16000);
    const std::size_t written = meter.write(tone);
    ESP_LOGI(TAG,
             "metered %u samples in %u chunks (sample rate %lu Hz)",
             static_cast<unsigned>(written),
             static_cast<unsigned>(dac.chunks),
             static_cast<unsigned long>(sample_rate));
    ESP_LOGI(TAG, "done");
}
