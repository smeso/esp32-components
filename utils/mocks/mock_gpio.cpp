/*
 * SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mock_gpio.hpp"

#include <esp_err.h>

namespace mock_bus {

namespace {

int levels[GPIO_NUM_MAX];
bool fail_config = false;
bool fail_install_isr_service = false;
bool install_isr_service_invalid_state = false;
unsigned install_isr_service_calls = 0;
bool fail_isr_handler_add = false;
bool fail_isr_handler_remove = false;
gpio_isr_t registered_handler = nullptr;
void *registered_arg = nullptr;

}   // namespace

void
mock_gpio_reset()
{
    for (int i = 0; i < GPIO_NUM_MAX; ++i)
        levels[i] = 0;
    fail_config = false;
    fail_install_isr_service = false;
    install_isr_service_invalid_state = false;
    install_isr_service_calls = 0;
    fail_isr_handler_add = false;
    fail_isr_handler_remove = false;
    registered_handler = nullptr;
    registered_arg = nullptr;
}

void
mock_gpio_set_level(gpio_num_t pin, int level)
{
    levels[pin] = level;
}

int
mock_gpio_get_level(gpio_num_t pin)
{
    return levels[pin];
}

void
mock_gpio_fail_config(bool fail)
{
    fail_config = fail;
}

void
mock_gpio_fail_install_isr_service(bool fail)
{
    fail_install_isr_service = fail;
}

void
mock_gpio_install_isr_service_invalid_state(bool fail)
{
    install_isr_service_invalid_state = fail;
}

unsigned
mock_gpio_install_isr_service_calls()
{
    return install_isr_service_calls;
}

void
mock_gpio_fail_isr_handler_add(bool fail)
{
    fail_isr_handler_add = fail;
}

void
mock_gpio_fail_isr_handler_remove(bool fail)
{
    fail_isr_handler_remove = fail;
}

gpio_isr_t
mock_gpio_isr_handler()
{
    return registered_handler;
}

void *
mock_gpio_isr_arg()
{
    return registered_arg;
}

}   // namespace mock_bus

// ---------------------------------------------------------------------------
// Mocked ESP-IDF GPIO driver entry points. The test component is linked into
// the firmware before the real esp_driver_gpio library, so the linker --wrap
// options in this component's CMakeLists.txt redirect every reference to
// these functions to the __wrap_* implementations, letting the tests drive
// their GPIO-based drivers without real hardware.
// ---------------------------------------------------------------------------

extern "C" {

esp_err_t
__wrap_gpio_config(const gpio_config_t *pGPIOConfig)
{
    if (mock_bus::fail_config)
        return ESP_ERR_INVALID_ARG;
    (void) pGPIOConfig;
    return ESP_OK;
}

int
__wrap_gpio_get_level(gpio_num_t gpio_num)
{
    return mock_bus::mock_gpio_get_level(gpio_num);
}

esp_err_t
__wrap_gpio_install_isr_service(int intr_alloc_flags)
{
    (void) intr_alloc_flags;
    ++mock_bus::install_isr_service_calls;
    if (mock_bus::fail_install_isr_service)
        return ESP_ERR_NO_MEM;
    if (mock_bus::install_isr_service_invalid_state)
        return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

esp_err_t
__wrap_gpio_isr_handler_add(gpio_num_t gpio_num, gpio_isr_t isr_handler, void *args)
{
    (void) gpio_num;
    if (mock_bus::fail_isr_handler_add)
        return ESP_ERR_INVALID_STATE;
    mock_bus::registered_handler = isr_handler;
    mock_bus::registered_arg = args;
    return ESP_OK;
}

esp_err_t
__wrap_gpio_isr_handler_remove(gpio_num_t gpio_num)
{
    (void) gpio_num;
    if (mock_bus::fail_isr_handler_remove)
        return ESP_ERR_INVALID_STATE;
    mock_bus::registered_handler = nullptr;
    mock_bus::registered_arg = nullptr;
    return ESP_OK;
}

esp_err_t
__wrap_gpio_reset_pin(gpio_num_t gpio_num)
{
    (void) gpio_num;
    return ESP_OK;
}

}   // extern "C"
