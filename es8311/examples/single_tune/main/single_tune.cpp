/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Minimal ES8311 example: initializes the I2C bus and the I2S TX channel,
 * plays a short tune for CONFIG_SINGLE_TUNE_PLAY_SECONDS seconds and then
 * powers the codec down and stops.
 *
 * All the hardware configuration (GPIOs, I2C address, bits per sample, MCLK
 * multiple, sample rate and I2C clock speed) is done through Kconfig.
 */

#include <cmath>
#include <cstdint>
#include <vector>

#include <esp_log.h>

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/i2s_std.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "es8311.hpp"

namespace {

constexpr const char *TAG = "single_tune";

// Number of samples rendered into the buffer for each note.
constexpr size_t SAMPLES_PER_NOTE = 4410;

struct note
{
    float freq;
    size_t samples;
};

// A simple ascending arpeggio.
constexpr note tune[] = {
    { 261.63f, SAMPLES_PER_NOTE },   // C4
    { 329.63f, SAMPLES_PER_NOTE },   // E4
    { 392.00f, SAMPLES_PER_NOTE },   // G4
    { 523.25f, SAMPLES_PER_NOTE },   // C5
};

// Sample type must match the configured I2S data width: 16-bit samples are
// stored in int16_t, 32-bit samples in int32_t.
#if CONFIG_SINGLE_TUNE_BITS_32
using sample_t = int32_t;
#else
using sample_t = int16_t;
#endif

std::vector<sample_t>
make_tune(uint32_t sample_rate)
{
    const float step = 2.0F * static_cast<float>(M_PI) / static_cast<float>(sample_rate);
    std::vector<sample_t> samples;
    for (const note &n : tune) {
        const size_t start = samples.size();
        samples.resize(start + n.samples);
        for (size_t i = 0; i < n.samples; ++i) {
            const float t = step * static_cast<float>(i);
            const float v = 0.35F * std::sin(2.0F * static_cast<float>(M_PI) * n.freq * t);
            samples[start + i] = static_cast<sample_t>(v * 32767.0F);
        }
    }
    return samples;
}

}   // namespace

extern "C" void
app_main(void)
{
    constexpr uint32_t sample_rate = CONFIG_SINGLE_TUNE_SAMPLE_RATE;
    constexpr i2s_data_bit_width_t bits =
#if CONFIG_SINGLE_TUNE_BITS_32
            I2S_DATA_BIT_WIDTH_32BIT;
#else
            I2S_DATA_BIT_WIDTH_16BIT;
#endif

    i2c_master_bus_handle_t bus_handle = nullptr;
    {
        i2c_master_bus_config_t bus_cfg = {
            .i2c_port = -1,
            .sda_io_num = static_cast<gpio_num_t>(CONFIG_SINGLE_TUNE_I2C_SDA_GPIO),
            .scl_io_num = static_cast<gpio_num_t>(CONFIG_SINGLE_TUNE_I2C_SCL_GPIO),
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

    i2s_chan_handle_t tx_handle = nullptr;
    {
        i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
        chan_cfg.auto_clear = true;
        ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, nullptr));
    }

    {
        i2s_std_config_t std_cfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, I2S_SLOT_MODE_STEREO),
            .gpio_cfg = {
                .mclk = static_cast<gpio_num_t>(CONFIG_SINGLE_TUNE_I2S_MCLK_GPIO),
                .bclk = static_cast<gpio_num_t>(CONFIG_SINGLE_TUNE_I2S_BCLK_GPIO),
                .ws = static_cast<gpio_num_t>(CONFIG_SINGLE_TUNE_I2S_WS_GPIO),
                .dout = static_cast<gpio_num_t>(CONFIG_SINGLE_TUNE_I2S_DOUT_GPIO),
                .din = I2S_GPIO_UNUSED,
                .invert_flags = {},
            },
        };
        std_cfg.clk_cfg.mclk_multiple =
#if CONFIG_SINGLE_TUNE_MCLK_MULTIPLE_256
                I2S_MCLK_MULTIPLE_256;
#elif CONFIG_SINGLE_TUNE_MCLK_MULTIPLE_384
                I2S_MCLK_MULTIPLE_384;
#elif CONFIG_SINGLE_TUNE_MCLK_MULTIPLE_512
                I2S_MCLK_MULTIPLE_512;
#elif CONFIG_SINGLE_TUNE_MCLK_MULTIPLE_768
                I2S_MCLK_MULTIPLE_768;
#else
                I2S_MCLK_MULTIPLE_1024;
#endif
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
        ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
    }

    constexpr i2s_mclk_multiple_t mclk_multiple =
#if CONFIG_SINGLE_TUNE_MCLK_MULTIPLE_256
            I2S_MCLK_MULTIPLE_256;
#elif CONFIG_SINGLE_TUNE_MCLK_MULTIPLE_384
            I2S_MCLK_MULTIPLE_384;
#elif CONFIG_SINGLE_TUNE_MCLK_MULTIPLE_512
            I2S_MCLK_MULTIPLE_512;
#elif CONFIG_SINGLE_TUNE_MCLK_MULTIPLE_768
            I2S_MCLK_MULTIPLE_768;
#else
            I2S_MCLK_MULTIPLE_1024;
#endif
#if CONFIG_SINGLE_TUNE_BITS_32
    constexpr bool i2s_32bit = true;
#else
    constexpr bool i2s_32bit = false;
#endif

    es8311<mclk_multiple, i2s_32bit> dac(bus_handle,
                                         tx_handle,
                                         sample_rate,
                                         {},
                                         {},
                                         static_cast<uint8_t>(CONFIG_SINGLE_TUNE_I2C_ADDR),
                                         CONFIG_SINGLE_TUNE_I2C_CLK_SPEED);
    dac.power_up_dac_only();
    dac.set_volume(65);

    const std::vector<sample_t> samples = make_tune(sample_rate);
    ESP_LOGI(TAG,
             "playing %zu samples at %lu Hz for %d s",
             samples.size(),
             static_cast<unsigned long>(sample_rate),
             CONFIG_SINGLE_TUNE_PLAY_SECONDS);

    // Stereo samples: two values per frame.
    const size_t bytes_per_frame = sizeof(sample_t) * 2U;
    const size_t to_play = static_cast<size_t>(sample_rate) *
                           static_cast<size_t>(CONFIG_SINGLE_TUNE_PLAY_SECONDS) *
                           bytes_per_frame;

    // Loop the tune until the configured duration has elapsed.
    size_t played = 0;
    while (played < to_play) {
        played += dac.write(samples);
    }

    dac.power_down();
    ESP_LOGI(TAG, "done");
}
