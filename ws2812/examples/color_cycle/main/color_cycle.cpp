/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Minimal WS2812 example: fades every LED of a strip through a color cycle
 * (red, green, blue) with a configurable number of steps per color.
 *
 * All the hardware configuration (data GPIO, number of LEDs, update interval
 * and steps per color) is done through Kconfig. The example only needs the
 * ws2812 component itself, so it does not depend on any other component.
 */

#include <cstddef>
#include <cstdint>

#include <esp_log.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "ws2812.hpp"

namespace {

constexpr const char *TAG = "color_cycle";

constexpr size_t NUM_LEDS = CONFIG_COLOR_CYCLE_NUM_LEDS;

using strip_t = ws2812<NUM_LEDS>;

}   // namespace

extern "C" void
app_main(void)
{
    strip_t strip(static_cast<gpio_num_t>(CONFIG_COLOR_CYCLE_GPIO));
    ESP_LOGI(TAG,
             "driving %zu WS2812 LEDs on GPIO %d",
             strip.num_leds(),
             CONFIG_COLOR_CYCLE_GPIO);

    constexpr uint8_t STEPS = CONFIG_COLOR_CYCLE_STEPS;
    while (true) {
        for (const auto &color : { strip_t::RGB{ 255, 0, 0 },
                                   strip_t::RGB{ 0, 255, 0 },
                                   strip_t::RGB{ 0, 0, 255 } }) {
            for (uint8_t step = 0; step < STEPS; ++step) {
                const uint8_t level = static_cast<uint8_t>(
                        (static_cast<uint16_t>(step) * 255U) / (STEPS - 1U));
                strip_t::RGB target = color;
                target.r = static_cast<uint8_t>((target.r * level) / 255U);
                target.g = static_cast<uint8_t>((target.g * level) / 255U);
                target.b = static_cast<uint8_t>((target.b * level) / 255U);
                strip.set_all_leds(target);
                vTaskDelay(pdMS_TO_TICKS(CONFIG_COLOR_CYCLE_INTERVAL_MS));
            }
        }
    }
}
