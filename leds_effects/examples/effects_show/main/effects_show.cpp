/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Minimal leds_effects example: cycles through the effects on a simulated LED
 * strip, logging the state of the strip on every change so the sequence is
 * visible on the console.
 *
 * All the configuration (number of LEDs and effect timings) is done through
 * Kconfig. The example only needs the leds_effects component itself, so it
 * does not depend on any other component.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>

#include <esp_log.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "leds_effects.hpp"

namespace {

constexpr const char *TAG = "effects_show";

// A software strip: the effects only require num_leds(), an RGB color type,
// set_leds(), set_all_leds() and all_off(), so the example can run without
// any LED hardware (or any other component).
class simulated_strip
{
  public:
    struct RGB
    {
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;
    };

    static constexpr std::size_t num_leds_value = CONFIG_EFFECTS_SHOW_NUM_LEDS;

    static constexpr std::size_t num_leds()
    {
        return num_leds_value;
    }

    using colors_t = std::array<RGB, num_leds_value>;

    void set_leds(std::span<const RGB> leds)
    {
        ESP_LOGI(TAG, "set_leds: %s", to_str(leds));
    }

    void set_all_leds(const RGB &color)
    {
        colors_t leds{};
        leds.fill(color);
        ESP_LOGI(TAG, "set_all_leds: %s", to_str(leds));
    }

    void all_off()
    {
        ESP_LOGI(TAG, "all_off");
    }

  private:
    static const char *to_str(std::span<const RGB> leds)
    {
        static char buf[CONFIG_EFFECTS_SHOW_NUM_LEDS * 16 + 1];
        std::size_t off = 0;
        for (const RGB &led : leds) {
            off += static_cast<std::size_t>(std::snprintf(
                    buf + off, sizeof(buf) - off, "(%u,%u,%u) ", led.r, led.g, led.b));
        }
        return buf;
    }
};

}   // namespace

extern "C" void
app_main(void)
{
    ESP_LOGI(TAG, "running effects on %zu simulated LEDs", simulated_strip::num_leds());
    ESP_LOGI(TAG,
             "temporal dithering renders %u effective bits per channel",
             leds_effects::DitherConfig{}.effective_bits());
    simulated_strip strip;

    leds_effects::rotating<simulated_strip, simulated_strip::RGB{ 255, 0, 0 }>(
            strip, CONFIG_EFFECTS_SHOW_INTERVAL_MS, 2);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_EFFECTS_SHOW_GAP_MS));

    leds_effects::bouncing<simulated_strip, simulated_strip::RGB{ 0, 255, 0 }>(
            strip, CONFIG_EFFECTS_SHOW_INTERVAL_MS, 2);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_EFFECTS_SHOW_GAP_MS));

    leds_effects::sun_effect(strip, CONFIG_EFFECTS_SHOW_TRANSITION_MS, true);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_EFFECTS_SHOW_GAP_MS));

    leds_effects::smooth_transition(strip,
                                    simulated_strip::RGB{ 255, 0, 0 },
                                    simulated_strip::RGB{ 0, 0, 255 },
                                    CONFIG_EFFECTS_SHOW_TRANSITION_MS);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_EFFECTS_SHOW_GAP_MS));

    leds_effects::breathing(strip,
                            simulated_strip::RGB{ 0, 0, 0 },
                            simulated_strip::RGB{ 0, 255, 255 },
                            CONFIG_EFFECTS_SHOW_TRANSITION_MS,
                            CONFIG_EFFECTS_SHOW_GAP_MS,
                            2);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_EFFECTS_SHOW_GAP_MS));

    leds_effects::deep_color(strip,
                             0xFFFF,
                             0x8000,
                             0x2000,
                             simulated_strip::RGB{ 0, 0, 0 },
                             CONFIG_EFFECTS_SHOW_TRANSITION_MS,
                             CONFIG_EFFECTS_SHOW_TRANSITION_MS,
                             CONFIG_EFFECTS_SHOW_GAP_MS);

    ESP_LOGI(TAG, "done");
}
