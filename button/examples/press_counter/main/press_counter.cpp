/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Minimal button example: counts presses, releases and long presses of a
 * button connected to a GPIO, and prints the counters once per second.
 *
 * The button GPIO, the active level and the long-press threshold are
 * configurable through Kconfig. The example only needs the button component
 * itself, so it does not depend on any other component.
 */

#include <atomic>
#include <cstdint>
#include <cstdio>

#include <esp_log.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "button.hpp"

namespace {

constexpr const char *TAG = "press_counter";

constexpr uint32_t REPORT_PERIOD_MS = 1000;

std::atomic<uint32_t> press_count{ 0 };
std::atomic<uint32_t> release_count{ 0 };
std::atomic<uint32_t> long_press_count{ 0 };

}   // namespace

extern "C" void
app_main(void)
{
    // The callbacks run from the FreeRTOS timer daemon task; the main loop
    // only reads the atomics, so no extra synchronization is needed.
    button<1> btn(static_cast<gpio_num_t>(CONFIG_PRESS_COUNTER_GPIO),
                  CONFIG_PRESS_COUNTER_ACTIVE_LOW,
                  true,
                  50);
    btn.on_press([] {
        press_count.fetch_add(1);
        ESP_LOGI(TAG, "pressed");
    });
    btn.on_release([] {
        release_count.fetch_add(1);
        ESP_LOGI(TAG, "released");
    });
    btn.add_longpress(CONFIG_PRESS_COUNTER_LONG_PRESS_MS, [] {
        long_press_count.fetch_add(1);
        ESP_LOGI(TAG, "long press");
    });
    btn.start();

    ESP_LOGI(TAG,
             "button example running: GPIO %d, active-low %s, long-press %d ms",
             CONFIG_PRESS_COUNTER_GPIO,
             CONFIG_PRESS_COUNTER_ACTIVE_LOW ? "yes" : "no",
             CONFIG_PRESS_COUNTER_LONG_PRESS_MS);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(REPORT_PERIOD_MS));
        ESP_LOGI(TAG,
                 "presses: %lu, releases: %lu, long presses: %lu",
                 static_cast<unsigned long>(press_count.load()),
                 static_cast<unsigned long>(release_count.load()),
                 static_cast<unsigned long>(long_press_count.load()));
    }
}
