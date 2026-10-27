/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Minimal TCA9555 example: configures one LED and one button on the GPIO
 * expander, then blinks the LED. While the button is held the LED blinks
 * faster; a long press turns the LED solid on until the button is released.
 *
 * The TCA9555 is polled through the button manager's tick timer, so no
 * interrupt line (INT#) has to be wired up. All the hardware configuration
 * (GPIOs, I2C address and clock speed) is done through Kconfig.
 */

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>

#include <esp_log.h>

#include <driver/gpio.h>
#include <driver/i2c_master.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "tca9555.hpp"

namespace {

constexpr const char *TAG = "gpio_blink";

constexpr uint32_t BLINK_PERIOD_MS = 500;
constexpr uint32_t FAST_BLINK_PERIOD_MS = 100;
constexpr uint32_t LONG_PRESS_MS = 1000;
constexpr uint32_t MAIN_LOOP_DELAY_MS = 10;

constexpr auto led_port = tca9555::port::port0;
constexpr uint8_t led_pin = 0;

constexpr auto button_port = tca9555::port::port1;
constexpr uint8_t button_pin = 1;

}   // namespace

extern "C" void
app_main(void)
{
    i2c_master_bus_handle_t bus_handle = nullptr;
    {
        i2c_master_bus_config_t bus_cfg = {
            .i2c_port = -1,
            .sda_io_num = static_cast<gpio_num_t>(CONFIG_GPIO_BLINK_I2C_SDA_GPIO),
            .scl_io_num = static_cast<gpio_num_t>(CONFIG_GPIO_BLINK_I2C_SCL_GPIO),
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

    std::shared_ptr<tca9555> dev;
    try {
        dev = std::make_shared<tca9555>(bus_handle,
                                        static_cast<uint8_t>(CONFIG_GPIO_BLINK_I2C_ADDR),
                                        CONFIG_GPIO_BLINK_I2C_CLK_SPEED);
    } catch (const tca9555::exception &e) {
        ESP_LOGE(TAG, "failed to initialize the TCA9555: %s", e.what());
        std::abort();
    }

    // P0.0 drives the LED (output); P1.1 reads the button (input).
    dev->pin_mode(led_port, led_pin, false);
    dev->pin_mode(button_port, button_pin, true);
    dev->digital_write(led_port, led_pin, false);

    // The button callbacks run on the worker task of the task queue the
    // button manager creates internally; the main loop only reads the
    // atomics, so no extra synchronization is needed.
    std::atomic<bool> button_held{ false };
    std::atomic<bool> long_press{ false };

    tca9555_button<tca9555, 1> buttons(dev, true, 50);
    buttons.on_press(button_port, button_pin, [&] {
        button_held.store(true);
        ESP_LOGI(TAG, "button pressed");
    });
    buttons.on_release(button_port, button_pin, [&] {
        button_held.store(false);
        long_press.store(false);
        ESP_LOGI(TAG, "button released");
    });
    buttons.add_longpress(button_port, button_pin, LONG_PRESS_MS, [&] {
        long_press.store(true);
        ESP_LOGI(TAG, "long press detected");
    });

    buttons.start();
    buttons.start_tick_timer(100);

    ESP_LOGI(TAG,
             "TCA9555 example running: LED on P0.%u, button on P1.%u",
             led_pin,
             button_pin);

    bool led_on = false;
    uint32_t last_toggle_ms = 0;
    while (true) {
        const uint32_t now_ms =
                static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
        const uint32_t period = long_press.load()  ? BLINK_PERIOD_MS :
                                button_held.load() ? FAST_BLINK_PERIOD_MS :
                                                     0;
        if (period == 0) {
            // Button released and no long press: keep the LED off.
            if (led_on) {
                led_on = false;
                dev->digital_write(led_port, led_pin, false);
            }
        } else if (now_ms - last_toggle_ms >= period) {
            last_toggle_ms = now_ms;
            led_on = !led_on;
            dev->digital_write(led_port, led_pin, led_on);
        }
        vTaskDelay(pdMS_TO_TICKS(MAIN_LOOP_DELAY_MS));
    }
}
