/*
 * SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>

#include <driver/gpio.h>

namespace mock_bus {

// Reset all GPIO mock state: pin levels, interrupt service state and failure
// flags.
void mock_gpio_reset();

// Simulated GPIO input level (the value returned by gpio_get_level()).
void mock_gpio_set_level(gpio_num_t pin, int level);
[[nodiscard]] int mock_gpio_get_level(gpio_num_t pin);

// Failure injection for the mocked GPIO driver entry points.
void mock_gpio_fail_config(bool fail);
void mock_gpio_fail_install_isr_service(bool fail);
// Make gpio_install_isr_service() return ESP_ERR_INVALID_STATE, as if the ISR
// service had already been installed by a previous call.
void mock_gpio_install_isr_service_invalid_state(bool fail);
// Number of gpio_install_isr_service() calls since the last mock_gpio_reset(),
// so tests can check that the service is installed only once.
[[nodiscard]] unsigned mock_gpio_install_isr_service_calls();
void mock_gpio_fail_isr_handler_add(bool fail);
void mock_gpio_fail_isr_handler_remove(bool fail);

// The ISR handler registered by the last gpio_isr_handler_add() call, so
// tests can invoke it directly.
[[nodiscard]] gpio_isr_t mock_gpio_isr_handler();
[[nodiscard]] void *mock_gpio_isr_arg();

}   // namespace mock_bus
