/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Sound driven example piloting a WS2812 LED strip from the audio recorded by
 * an ES7210 microphone ADC: a sound_driven instance wraps the ADC in an
 * adc_sink (the discarding "DAC" the samples are written to) and, on every
 * audio chunk, computes the bark-scale spectral amplitudes of the recording,
 * which are then mapped to the colors of the strip's LEDs.
 *
 * All the hardware configuration (GPIOs, I2C address, sample rate, LED GPIO
 * and strip length) is done through Kconfig. The example needs the
 * sound_driven component itself plus es7210, ws2812 and leds_effects.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <esp_log.h>

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/i2s_std.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "es7210.hpp"
#include "leds_effects.hpp"
#include "sound_driven.hpp"
#include "ws2812.hpp"

namespace {

constexpr const char *TAG = "mic_vu_meter";

constexpr size_t NUM_LEDS = CONFIG_MIC_VU_METER_NUM_LEDS;

constexpr i2s_mclk_multiple_t MCLK_MULTIPLE = I2S_MCLK_MULTIPLE_256;
using adc_t = es7210<MCLK_MULTIPLE, false>;
using strip_t = ws2812<NUM_LEDS>;

// 7 spectral groups (one per LED): the callback receives 7 values (one per
// bark band).
constexpr size_t SPECTRAL_GROUPS = NUM_LEDS;

// The microphone recording is only analyzed (and then dropped), so the ES7210
// is wrapped in the discarding sink provided by sound_driven.
using meter_t = sound_driven<adc_sink<adc_t>, 1, SPECTRAL_GROUPS>;
static_assert(meter_t::callback_size == SPECTRAL_GROUPS);

// How bright the hue-mapped band colors are, in 1/1000 of the full scale.
constexpr uint32_t BRIGHTNESS = 700;

// Typical sRGB-like LED primaries: maps CIE XYZ targets to drive.
constexpr leds_effects::LedCalibration calibration{
    .inv_matrix = { std::array<float, 3>{ 3.2404542f, -1.5371385f, -0.4985314f },
                    std::array<float, 3>{ -0.9692660f, 1.8760108f, 0.0415560f },
                    std::array<float, 3>{ 0.0556434f, -0.2040259f, 1.0572252f } },
    .black = { 0, 0, 0 },
    .gamma = 1.0f,
};

// Map a band's amplitude (0..65535) to a brightness-scaled 8-bit channel
// value.
constexpr uint8_t
channel_value(uint16_t amplitude)
{
    const uint32_t scaled = (static_cast<uint32_t>(amplitude) * BRIGHTNESS + 32767U) / 65535U;
    return static_cast<uint8_t>(std::min<uint32_t>(scaled, 255U));
}

// Map the spectral amplitudes to the strip: the bands are painted over the
// LEDs (band g lights up LED g % NUM_LEDS, in a hue that shifts along the
// strip), so the strip shows a colored bar whose length and brightness
// follow the audio spectrum.
std::array<strip_t::RGB, NUM_LEDS>
spectrum_leds(const std::array<uint16_t, meter_t::callback_size> &amps)
{
    std::array<strip_t::RGB, NUM_LEDS> leds{};
    for (size_t g = 0; g < SPECTRAL_GROUPS; ++g) {
        const uint8_t level = channel_value(amps[g]);
        strip_t::RGB color{};
        if (g < SPECTRAL_GROUPS / 3)
            color.r = level;
        else if (g < 2 * SPECTRAL_GROUPS / 3)
            color.g = level;
        else
            color.b = level;
        color = calibrate(color, calibration);
        leds[g % NUM_LEDS] = color;
    }
    return leds;
}

}   // namespace

extern "C" void
app_main(void)
{
    static strip_t strip(static_cast<gpio_num_t>(CONFIG_MIC_VU_METER_LED_GPIO));
    ESP_LOGI(TAG,
             "driving %zu WS2812 LEDs on GPIO %d",
             strip.num_leds(),
             CONFIG_MIC_VU_METER_LED_GPIO);

    i2c_master_bus_handle_t bus_handle = nullptr;
    {
        i2c_master_bus_config_t bus_cfg = {
            .i2c_port = -1,
            .sda_io_num = static_cast<gpio_num_t>(CONFIG_MIC_VU_METER_I2C_SDA_GPIO),
            .scl_io_num = static_cast<gpio_num_t>(CONFIG_MIC_VU_METER_I2C_SCL_GPIO),
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

    constexpr uint32_t sample_rate = 16000;
    {
        i2s_std_config_t std_cfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                            I2S_SLOT_MODE_STEREO),
            .gpio_cfg = {
                .mclk = static_cast<gpio_num_t>(CONFIG_MIC_VU_METER_I2S_MCLK_GPIO),
                .bclk = static_cast<gpio_num_t>(CONFIG_MIC_VU_METER_I2S_BCLK_GPIO),
                .ws = static_cast<gpio_num_t>(CONFIG_MIC_VU_METER_I2S_WS_GPIO),
                .dout = I2S_GPIO_UNUSED,
                .din = static_cast<gpio_num_t>(CONFIG_MIC_VU_METER_I2S_DIN_GPIO),
                .invert_flags = {},
            },
        };
        std_cfg.clk_cfg.mclk_multiple = MCLK_MULTIPLE;
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
        ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
    }

    adc_t adc(bus_handle,
              rx_handle,
              sample_rate,
              adc_t::MIC1 | adc_t::MIC2,
              static_cast<uint8_t>(CONFIG_MIC_VU_METER_I2C_ADDR),
              CONFIG_MIC_VU_METER_I2C_CLK_SPEED);

    if (adc.is_7210()) {
        ESP_LOGI(TAG, "ES7210 chip ID verified, version %u", adc.get_version());
    } else {
        ESP_LOGW(TAG, "unexpected ES7210 chip ID");
    }

    adc.set_gain(adc_t::GAIN_30DB);

    // The sound_driven callback runs on the audio task right before every
    // chunk is handed to the ADC wrapper: paint the strip there, so the LEDs
    // follow the music in real time.
    adc_sink<adc_t> sink{ adc };
    meter_t meter(sink, [](std::array<uint16_t, meter_t::callback_size> amps) {
        strip.set_leds(spectrum_leds(amps));
    });

    adc.start();

    // Record forever: read a chunk from the microphones and feed it to the
    // meter, which analyzes it and forwards it to the (sink) ADC.
    std::vector<int16_t> chunk(512);
    size_t chunks = 0;
    size_t timeouts = 0;
    while (true) {
        try {
            const size_t read_bytes = adc.read(chunk, pdMS_TO_TICKS(500));
            (void) read_bytes;
        } catch (const adc_t::exception &e) {
            timeouts++;
            if (timeouts == 1 || timeouts % 100 == 0) {
                ESP_LOGW(TAG, "read timeout (%zu so far): %s", timeouts, e.what());
            }
            continue;
        }
        const size_t written = meter.write(chunk, 2, pdMS_TO_TICKS(100));
        (void) written;
        chunks++;
    }
}
