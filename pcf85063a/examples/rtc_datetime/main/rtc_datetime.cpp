/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Minimal PCF85063A example: initializes the I2C bus, sets the RTC to the
 * current ESP32 system time (synchronizing the RTC clock), and then prints
 * the RTC time once per second.
 *
 * All the hardware configuration (I2C GPIOs and clock speed) is done through
 * Kconfig. The example only needs the pcf85063a component itself, so it does
 * not depend on any other component.
 */

#include <cstdio>

#include <esp_log.h>

#include <driver/gpio.h>
#include <driver/i2c_master.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "pcf85063a.hpp"

namespace {

constexpr const char *TAG = "rtc_datetime";

}   // namespace

extern "C" void
app_main(void)
{
    i2c_master_bus_handle_t bus_handle = nullptr;
    {
        i2c_master_bus_config_t bus_cfg = {
            .i2c_port = -1,
            .sda_io_num = static_cast<gpio_num_t>(CONFIG_RTC_DATETIME_I2C_SDA_GPIO),
            .scl_io_num = static_cast<gpio_num_t>(CONFIG_RTC_DATETIME_I2C_SCL_GPIO),
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

    pcf85063a<> rtc(bus_handle, CONFIG_RTC_DATETIME_I2C_CLK_SPEED);
    rtc.init();
    rtc.set_datetime_with_drift();
    ESP_LOGI(TAG, "RTC synchronized to the ESP32 system time");

    while (true) {
        const pcf85063a<>::datetime now = rtc.get_datetime();
        char buf[32];
        pcf85063a<>::to_str(buf, sizeof(buf), now);
        ESP_LOGI(TAG, "RTC time: %s", buf);
        vTaskDelay(pdMS_TO_TICKS(CONFIG_RTC_DATETIME_PRINT_PERIOD_MS));
    }
}
