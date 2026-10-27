/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Minimal ES7210 example: initializes the I2C bus and the I2S RX channel,
 * records CONFIG_RECORD_AUDIO_RECORD_SECONDS seconds of audio from the
 * configured microphones and then prints a short summary (bytes read, peak
 * amplitude and number of non-zero samples) proving that audio data was
 * captured, before powering the ADC down and stopping.
 *
 * All the hardware configuration (GPIOs, I2C address, bits per sample, MCLK
 * multiple, sample rate, microphone mask and recording duration) is done
 * through Kconfig.
 */

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include <esp_log.h>

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/i2s_std.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "es7210.hpp"

namespace {

constexpr const char *TAG = "record_audio";

// Sample type must match the configured I2S data width: 16-bit samples are
// stored in int16_t, 32-bit samples in int32_t.
#if CONFIG_RECORD_AUDIO_BITS_32
using sample_t = int32_t;
#else
using sample_t = int16_t;
#endif

struct audio_stats
{
    size_t samples;
    size_t nonzero;
    int64_t sum_abs;
    int32_t peak;
};

audio_stats
analyze(const std::vector<sample_t> &samples)
{
    audio_stats stats{};
    for (sample_t v : samples) {
        if (v != 0)
            stats.nonzero++;
        const int32_t abs_v = std::abs(static_cast<int32_t>(v));
        if (abs_v > stats.peak)
            stats.peak = abs_v;
        stats.sum_abs += abs_v;
    }
    stats.samples = samples.size();
    return stats;
}

}   // namespace

extern "C" void
app_main(void)
{
    constexpr uint32_t sample_rate = CONFIG_RECORD_AUDIO_SAMPLE_RATE;
    constexpr i2s_data_bit_width_t bits =
#if CONFIG_RECORD_AUDIO_BITS_32
            I2S_DATA_BIT_WIDTH_32BIT;
#else
            I2S_DATA_BIT_WIDTH_16BIT;
#endif
    constexpr size_t bytes_per_sample = sizeof(sample_t);
    constexpr size_t chunk_samples = 512;
    constexpr size_t target_samples = static_cast<size_t>(CONFIG_RECORD_AUDIO_SAMPLE_RATE) *
                                      static_cast<size_t>(CONFIG_RECORD_AUDIO_RECORD_SECONDS);

    i2c_master_bus_handle_t bus_handle = nullptr;
    {
        i2c_master_bus_config_t bus_cfg = {
            .i2c_port = -1,
            .sda_io_num = static_cast<gpio_num_t>(CONFIG_RECORD_AUDIO_I2C_SDA_GPIO),
            .scl_io_num = static_cast<gpio_num_t>(CONFIG_RECORD_AUDIO_I2C_SCL_GPIO),
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = true,
                .allow_pd = false,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus_handle));
    }

    i2s_chan_handle_t rx_handle = nullptr;
    {
        i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
        chan_cfg.auto_clear = true;
        ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, nullptr, &rx_handle));
    }

    {
        i2s_std_config_t std_cfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, I2S_SLOT_MODE_STEREO),
            .gpio_cfg = {
                .mclk = static_cast<gpio_num_t>(CONFIG_RECORD_AUDIO_I2S_MCLK_GPIO),
                .bclk = static_cast<gpio_num_t>(CONFIG_RECORD_AUDIO_I2S_BCLK_GPIO),
                .ws = static_cast<gpio_num_t>(CONFIG_RECORD_AUDIO_I2S_WS_GPIO),
                .dout = I2S_GPIO_UNUSED,
                .din = static_cast<gpio_num_t>(CONFIG_RECORD_AUDIO_I2S_DIN_GPIO),
                .invert_flags = {},
            },
        };
        std_cfg.clk_cfg.mclk_multiple =
#if CONFIG_RECORD_AUDIO_MCLK_MULTIPLE_256
                I2S_MCLK_MULTIPLE_256;
#elif CONFIG_RECORD_AUDIO_MCLK_MULTIPLE_384
                I2S_MCLK_MULTIPLE_384;
#elif CONFIG_RECORD_AUDIO_MCLK_MULTIPLE_512
                I2S_MCLK_MULTIPLE_512;
#elif CONFIG_RECORD_AUDIO_MCLK_MULTIPLE_768
                I2S_MCLK_MULTIPLE_768;
#else
                I2S_MCLK_MULTIPLE_1024;
#endif
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
        ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
    }

    constexpr i2s_mclk_multiple_t mclk_multiple =
#if CONFIG_RECORD_AUDIO_MCLK_MULTIPLE_256
            I2S_MCLK_MULTIPLE_256;
#elif CONFIG_RECORD_AUDIO_MCLK_MULTIPLE_384
            I2S_MCLK_MULTIPLE_384;
#elif CONFIG_RECORD_AUDIO_MCLK_MULTIPLE_512
            I2S_MCLK_MULTIPLE_512;
#elif CONFIG_RECORD_AUDIO_MCLK_MULTIPLE_768
            I2S_MCLK_MULTIPLE_768;
#else
            I2S_MCLK_MULTIPLE_1024;
#endif
#if CONFIG_RECORD_AUDIO_BITS_32
    constexpr bool i2s_32bit = true;
#else
    constexpr bool i2s_32bit = false;
#endif

    es7210<mclk_multiple, i2s_32bit> adc(bus_handle,
                                         rx_handle,
                                         sample_rate,
                                         static_cast<uint8_t>(CONFIG_RECORD_AUDIO_MIC_MASK),
                                         static_cast<uint8_t>(CONFIG_RECORD_AUDIO_I2C_ADDR),
                                         CONFIG_RECORD_AUDIO_I2C_CLK_SPEED);

    if (adc.is_7210()) {
        ESP_LOGI(TAG, "ES7210 chip ID verified, version %u", adc.get_version());
    } else {
        ESP_LOGW(TAG, "unexpected ES7210 chip ID");
    }

    adc.start();
    adc.set_gain(es7210<mclk_multiple, i2s_32bit>::GAIN_30DB);

    std::vector<sample_t> samples(target_samples);
    size_t timeouts = 0;
    size_t recorded = 0;
    const TickType_t deadline =
            xTaskGetTickCount() +
            pdMS_TO_TICKS(static_cast<uint32_t>(CONFIG_RECORD_AUDIO_RECORD_SECONDS) * 1000U);
    ESP_LOGI(TAG,
             "recording %d s at %lu Hz (0x%02X)",
             CONFIG_RECORD_AUDIO_RECORD_SECONDS,
             static_cast<unsigned long>(sample_rate),
             CONFIG_RECORD_AUDIO_I2C_ADDR);
    while (recorded < target_samples && xTaskGetTickCount() < deadline) {
        try {
            const std::span<sample_t> chunk =
                    std::span<sample_t>{ samples }.subspan(recorded, chunk_samples);
            const size_t read_bytes = adc.read(chunk, pdMS_TO_TICKS(500));
            recorded += read_bytes / bytes_per_sample;
        } catch (const es7210<mclk_multiple, i2s_32bit>::exception &e) {
            // No data available yet (e.g. no microphone signal): keep trying
            // until the recording window elapses.
            timeouts++;
            if (timeouts == 1 || timeouts % 100 == 0) {
                ESP_LOGW(TAG, "read timeout (%zu so far): %s", timeouts, e.what());
            }
        }
    }

    adc.stop();

    const audio_stats stats = analyze(samples);
    ESP_LOGI(TAG,
             "recorded %zu samples (%.1f s)",
             samples.size(),
             static_cast<double>(samples.size()) / static_cast<double>(sample_rate));
    ESP_LOGI(TAG, "peak amplitude: %d", static_cast<int>(stats.peak));
    ESP_LOGI(TAG, "non-zero samples: %zu / %zu", stats.nonzero, stats.samples);
    ESP_LOGI(TAG,
             "average absolute amplitude: %.3f",
             stats.samples == 0 ?
                     0.0 :
                     static_cast<double>(stats.sum_abs) / static_cast<double>(stats.samples));
    ESP_LOGI(TAG, "recording finished (%zu read timeouts)", timeouts);
}
